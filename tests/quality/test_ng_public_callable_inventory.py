#!/usr/bin/env python3
"""Exact and fail-closed tests for the installed-public callable census."""

from __future__ import annotations

import copy
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import public_callable_inventory as inventory  # noqa: E402
from check_ng_sdk_contract import (  # noqa: E402
    SdkContractError,
    admitted_generated_relations,
    load_yaml,
    validate_generated_relation_header,
)


class NgPublicCallableInventoryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        compiler = shutil.which("clang++-22")
        if compiler is None:
            raise RuntimeError("exact public callable tests require clang++-22")
        cls.compiler = compiler
        cls.document = inventory.load_document(ROOT / inventory.INVENTORY)
        inventory.validate_inventory_document(ROOT, cls.document)
        cls.observed, cls.extractor = inventory.extract_ast_census(
            ROOT, cls.compiler
        )

    @staticmethod
    def copied_ast_root(temporary: str) -> pathlib.Path:
        root = pathlib.Path(temporary)
        shutil.copytree(ROOT / "include/cxxlens", root / "include/cxxlens")
        catalog = root / "schemas/cxxlens_ng_public_api_catalog.yaml"
        catalog.parent.mkdir(parents=True)
        shutil.copy2(
            ROOT / "schemas/cxxlens_ng_public_api_catalog.yaml", catalog
        )
        return root

    @staticmethod
    def replace_once(path: pathlib.Path, old: str, new: str) -> None:
        source = path.read_text(encoding="utf-8")
        if source.count(old) != 1:
            raise AssertionError(f"expected exactly one fixture marker in {path}: {old}")
        path.write_text(source.replace(old, new), encoding="utf-8")

    @staticmethod
    def identity_document(
        rows: list[dict],
        slots: list[int],
        high_water: dict[tuple[str, str], int] | None = None,
    ) -> dict:
        callables = copy.deepcopy(rows)
        next_slots: dict[tuple[str, str], int] = {}
        for row, slot in zip(callables, slots, strict=True):
            row["identity"] = {"overload_slot": slot}
            row["id"] = inventory.callable_id(row, slot)
            scope = inventory._identity_scope_key(row)
            next_slots[scope] = max(next_slots.get(scope, 0), slot + 1)
        if high_water:
            next_slots.update(high_water)
        return {
            "allocator": {
                "domain": inventory.CALLABLE_ID_DOMAIN,
                "scopes": inventory._canonical_allocator_scopes(next_slots),
            },
            "callables": callables,
        }

    def overload_fixture(self) -> list[dict]:
        groups: dict[tuple[str, str], list[dict]] = {}
        for row in self.observed:
            groups.setdefault(inventory._identity_scope_key(row), []).append(row)
        return copy.deepcopy(next(rows for rows in groups.values() if len(rows) >= 2)[:2])

    def assert_ast_rejected(
        self, root: pathlib.Path, pattern: str
    ) -> None:
        with self.assertRaisesRegex(inventory.CallableInventoryError, pattern):
            inventory.check_ast_inventory(root, self.compiler, self.document)

    def test_repository_inventory_is_exact_and_covers_special_surfaces(self) -> None:
        observed, extractor = inventory.check_ast_inventory(
            ROOT, self.compiler, self.document
        )
        self.assertEqual(extractor, self.extractor)
        self.assertEqual(len(observed), len(self.document["callables"]))
        self.assertEqual(
            {row["callable_kind"] for row in observed},
            {
                "free",
                "function-template",
                "constructor",
                "destructor",
                "member",
                "static",
                "operator",
                "virtual",
            },
        )
        self.assertEqual(
            {
                row["signature"]["declaration_state"]
                for row in observed
            },
            {"declared", "deleted", "defaulted", "pure-virtual"},
        )
        self.assertTrue(
            any(
                parameter["default"] is not None
                for row in observed
                for parameter in row["signature"]["parameters"]
            )
        )
        self.assertTrue(
            any(row["signature"]["template_parameters"] for row in observed)
        )
        self.assertTrue(any(row["signature"]["constraints"] for row in observed))
        self.assertTrue(
            any(
                row["signature"]["qualifiers"]["noexcept"] != "none"
                for row in observed
            )
        )
        self.assertTrue(
            any(
                row["signature"]["qualifiers"]["ref"] != "none"
                for row in observed
            )
        )
        for specifier in (
            "static",
            "virtual",
            "pure_virtual",
            "inline",
            "constexpr",
            "explicit",
        ):
            self.assertTrue(
                any(row["signature"]["specifiers"][specifier] for row in observed),
                specifier,
            )

    def test_regeneration_is_deterministic(self) -> None:
        doxygen_rows = []
        for index, row in enumerate(self.observed):
            projection = {"fixture": index}
            doxygen_rows.append(
                {
                    "declaring_header": row["declaring_header"],
                    "source_line": row["source_line"],
                    "source_column": row["source_column"] + 1,
                    "fully_qualified_name": row["fully_qualified_name"],
                    "doxygen_key": inventory.semantic_digest(projection),
                    "projection": projection,
                }
            )
        ast_result = (copy.deepcopy(self.observed), copy.deepcopy(self.extractor))
        with mock.patch.object(
            inventory, "extract_ast_census", return_value=ast_result
        ), mock.patch.object(
            inventory,
            "extract_doxygen_census",
            return_value=copy.deepcopy(doxygen_rows),
        ):
            first = inventory.build_inventory(
                ROOT, self.compiler, pathlib.Path("unused"), self.document
            )
            second = inventory.build_inventory(
                ROOT, self.compiler, pathlib.Path("unused"), self.document
            )
        self.assertEqual(inventory.canonical_json(first), inventory.canonical_json(second))
        self.assertEqual(first["inventory_digest"], inventory.inventory_digest(first))

    def test_signature_and_header_change_preserve_stable_id(self) -> None:
        original = copy.deepcopy(self.observed[0])
        scope = inventory._identity_scope_key(original)
        previous = self.identity_document([original], [4], {scope: 5})
        changed = copy.deepcopy(original)
        changed["declaring_header"] = "include/cxxlens/sdk/moved.hpp"
        changed["signature"]["source"] += " noexcept"
        changed["signature"]["qualifiers"]["noexcept"] = "noexcept"
        current = self.identity_document([changed], [4], {scope: 5})

        inventory.validate_stable_id_transition(previous, current)
        slots, scopes = inventory._allocate_identities([changed], previous)
        self.assertEqual(slots, {0: 4})
        self.assertEqual(scopes, previous["allocator"]["scopes"])
        self.assertEqual(
            previous["callables"][0]["id"], current["callables"][0]["id"]
        )

    def test_stable_id_and_identity_swaps_are_rejected(self) -> None:
        overloads = self.overload_fixture()
        previous = self.identity_document(overloads, [0, 1])

        id_only = copy.deepcopy(previous)
        id_only["callables"][0]["id"], id_only["callables"][1]["id"] = (
            id_only["callables"][1]["id"],
            id_only["callables"][0]["id"],
        )
        with self.assertRaisesRegex(
            inventory.CallableInventoryError,
            "stable ID differs from identity",
        ):
            inventory.validate_stable_id_transition(previous, id_only)

        slot_and_id = copy.deepcopy(previous)
        for row, slot in zip(slot_and_id["callables"], [1, 0], strict=True):
            row["identity"]["overload_slot"] = slot
            row["id"] = inventory.callable_id(row, slot)
        with self.assertRaisesRegex(
            inventory.CallableInventoryError,
            "exact public callable changed stable ID",
        ):
            inventory.validate_stable_id_transition(previous, slot_and_id)

    def test_allocator_high_water_cannot_decrease_or_reuse_a_slot(self) -> None:
        overloads = self.overload_fixture()
        scope = inventory._identity_scope_key(overloads[0])
        previous = self.identity_document(overloads, [0, 1], {scope: 3})

        decreased = self.identity_document(overloads, [0, 1], {scope: 2})
        with self.assertRaisesRegex(
            inventory.CallableInventoryError, "allocator high-water decreased"
        ):
            inventory.validate_stable_id_transition(previous, decreased)

        after_deletion = self.identity_document([overloads[0]], [0], {scope: 3})
        replacement = copy.deepcopy(overloads[1])
        replacement["signature"]["source"] += " noexcept"
        replacement["signature"]["qualifiers"]["noexcept"] = "noexcept"
        reused = self.identity_document(
            [overloads[0], replacement], [0, 1], {scope: 3}
        )
        with self.assertRaisesRegex(
            inventory.CallableInventoryError, "reused a historical overload slot"
        ):
            inventory.validate_stable_id_transition(after_deletion, reused)

        assigned, scopes = inventory._allocate_identities(
            [overloads[0], replacement], after_deletion
        )
        self.assertEqual(assigned[1], 3)
        self.assertEqual(
            next(row for row in scopes if inventory._identity_scope_key(row) == scope)[
                "next_overload_slot"
            ],
            4,
        )

    def test_ambiguous_multiple_signature_changes_are_rejected(self) -> None:
        overloads = self.overload_fixture()
        previous = self.identity_document(overloads, [0, 1])
        changed = copy.deepcopy(overloads)
        for index, row in enumerate(changed):
            row["signature"]["source"] += f" /* changed {index} */"
        with self.assertRaisesRegex(
            inventory.CallableInventoryError, "ambiguous public callable"
        ):
            inventory._allocate_identities(changed, previous)

    def test_stable_history_rejects_depth_one_checkout_and_accepts_parent(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-callable-history-") as temporary:
            base = pathlib.Path(temporary)
            source = base / "source"
            source.mkdir()

            def git(repository: pathlib.Path, *arguments: str) -> None:
                subprocess.run(
                    ["git", "-C", str(repository), *arguments],
                    check=True,
                    capture_output=True,
                    text=True,
                )

            git(source, "init", "--initial-branch=main")
            git(source, "config", "user.name", "cxxlens test")
            git(source, "config", "user.email", "cxxlens@example.invalid")
            relative = inventory.INVENTORY
            tracked = source / relative
            tracked.parent.mkdir(parents=True)
            tracked.write_text("version: legacy\n", encoding="utf-8")
            git(source, "add", relative.as_posix())
            git(source, "commit", "-m", "legacy inventory")
            tracked.write_text("version: current\n", encoding="utf-8")
            git(source, "add", relative.as_posix())
            git(source, "commit", "-m", "current inventory")

            depth_one = base / "depth-one"
            subprocess.run(
                [
                    "git",
                    "clone",
                    "--depth=1",
                    source.as_uri(),
                    str(depth_one),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            with self.assertRaisesRegex(
                inventory.CallableInventoryError,
                "history is unavailable in a shallow checkout",
            ):
                inventory.previous_inventory_for_check(
                    depth_one, depth_one / relative
                )

            depth_two = base / "depth-two"
            subprocess.run(
                [
                    "git",
                    "clone",
                    "--depth=2",
                    source.as_uri(),
                    str(depth_two),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                inventory.previous_inventory_for_check(
                    depth_two, depth_two / relative
                ),
                {"version": "legacy"},
            )

    def test_doxygen_missing_extra_and_projection_drift_are_rejected(self) -> None:
        exact = [
            {
                "declaring_header": row["declaring_header"],
                "source_line": row["evidence"]["source_anchor"]["doxygen"]["line"],
                "source_column": row["evidence"]["source_anchor"]["doxygen"]["column"],
                "fully_qualified_name": row["evidence"]["doxygen_name"],
                "doxygen_key": row["evidence"]["doxygen_key"],
                "projection": {"fixture": row["id"]},
            }
            for row in self.document["callables"]
        ]
        fixtures = {
            "inventory-only": exact[:-1],
            "doxygen-only": [
                *exact,
                {
                    "declaring_header": exact[0]["declaring_header"],
                    "source_line": exact[0]["source_line"],
                    "source_column": exact[0]["source_column"] + 10000,
                    "fully_qualified_name": "cxxlens :: extra",
                    "doxygen_key": inventory.semantic_digest("extra Doxygen row"),
                    "projection": {"fixture": "extra"},
                },
            ],
            "signature-projection-drift": [
                {
                    **exact[0],
                    "doxygen_key": inventory.semantic_digest(
                        {"fixture": "changed signature projection"}
                    ),
                },
                *exact[1:],
            ],
        }
        swapped = copy.deepcopy(exact)
        for field in ("doxygen_key", "fully_qualified_name"):
            swapped[0][field], swapped[1][field] = (
                swapped[1][field],
                swapped[0][field],
            )
        fixtures["row-level-key-and-name-swap"] = swapped
        for label, observed in fixtures.items():
            with self.subTest(label), mock.patch.object(
                inventory, "extract_doxygen_census", return_value=observed
            ):
                with self.assertRaisesRegex(
                    inventory.CallableInventoryError,
                    "Doxygen/inventory callable",
                ):
                    inventory.check_doxygen_inventory(
                        self.document, pathlib.Path("unused")
                    )

    def test_same_line_ast_and_doxygen_coordinates_pair_by_independent_order(self) -> None:
        ast_rows = copy.deepcopy(self.observed[:2])
        doxygen_rows = []
        for index, row in enumerate(ast_rows):
            row["declaring_header"] = "include/cxxlens/sdk/common.hpp"
            row["source_line"] = 999
            row["source_column"] = (20, 80)[index]
            doxygen_rows.append(
                {
                    "declaring_header": row["declaring_header"],
                    "source_line": 999,
                    "source_column": (3, 9)[index],
                    "fully_qualified_name": row["fully_qualified_name"],
                    "doxygen_key": inventory.semantic_digest({"row": index}),
                    "projection": {"row": index},
                }
            )
        pairs = inventory._pair_ast_doxygen_rows(
            list(reversed(ast_rows)), list(reversed(doxygen_rows))
        )
        self.assertEqual(
            [(left["source_column"], right["source_column"]) for left, right in pairs],
            [(20, 3), (80, 9)],
        )
        inventory_rows = copy.deepcopy(self.document["callables"][:2])
        for index, row in enumerate(inventory_rows):
            row["declaring_header"] = "include/cxxlens/sdk/common.hpp"
            row["evidence"]["source_anchor"] = {
                "ast": {"line": 999, "column": (20, 80)[index]},
                "doxygen": {"line": 999, "column": (3, 9)[index]},
            }
        inventory._validate_source_anchor_order(inventory_rows)
        for field in ("doxygen_key", "doxygen_name"):
            inventory_rows[0]["evidence"][field], inventory_rows[1]["evidence"][field] = (
                inventory_rows[1]["evidence"][field],
                inventory_rows[0]["evidence"][field],
            )
        (
            inventory_rows[0]["evidence"]["source_anchor"]["doxygen"],
            inventory_rows[1]["evidence"]["source_anchor"]["doxygen"],
        ) = (
            inventory_rows[1]["evidence"]["source_anchor"]["doxygen"],
            inventory_rows[0]["evidence"]["source_anchor"]["doxygen"],
        )
        with self.assertRaisesRegex(
            inventory.CallableInventoryError,
            "AST/Doxygen callable source order differs",
        ):
            inventory._validate_source_anchor_order(inventory_rows)

    def test_cross_header_semantic_callable_ownership_is_rejected(self) -> None:
        changed = copy.deepcopy(self.document)
        duplicate = copy.deepcopy(
            next(
                row
                for row in changed["callables"]
                if row["declaring_header"] != "include/cxxlens/sdk/claim.hpp"
            )
        )
        duplicate["declaring_header"] = "include/cxxlens/sdk/claim.hpp"
        scope = inventory._identity_scope_key(duplicate)
        allocator_scope = next(
            row
            for row in changed["allocator"]["scopes"]
            if inventory._identity_scope_key(row) == scope
        )
        duplicate_slot = allocator_scope["next_overload_slot"]
        allocator_scope["next_overload_slot"] += 1
        duplicate["identity"] = {"overload_slot": duplicate_slot}
        duplicate["id"] = inventory.callable_id(duplicate, duplicate_slot)
        changed["callables"].append(duplicate)
        changed["callables"].sort(key=lambda row: row["id"])
        changed["inventory_digest"] = inventory.inventory_digest(changed)
        with self.assertRaisesRegex(
            inventory.CallableInventoryError,
            "one semantic public callable is owned by multiple inventory rows",
        ):
            inventory.validate_inventory_document(ROOT, changed)

    def test_cross_header_declaration_is_rejected_by_ast_census(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-callable-cross-header-") as temporary:
            root = self.copied_ast_root(temporary)
            declarations = {
                "include/cxxlens/sdk/common.hpp": (
                    "\nnamespace cxxlens::sdk { using issue_169_alias = int; "
                    "issue_169_alias issue_169_cross_header(issue_169_alias); }\n"
                ),
                "include/cxxlens/sdk/claim.hpp": (
                    "\nnamespace cxxlens::sdk { int issue_169_cross_header(int); }\n"
                ),
            }
            for relative, declaration in declarations.items():
                path = root / relative
                path.write_text(
                    path.read_text(encoding="utf-8") + declaration,
                    encoding="utf-8",
                )
            with self.assertRaisesRegex(
                inventory.CallableInventoryError,
                "declared by multiple installed headers",
            ):
                inventory.extract_ast_census(root, self.compiler)

    def test_same_header_redeclaration_drift_is_rejected_before_collapse(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-callable-redeclaration-") as temporary:
            root = self.copied_ast_root(temporary)
            common = root / "include/cxxlens/sdk/common.hpp"
            common.write_text(
                common.read_text(encoding="utf-8")
                + (
                    "\nnamespace cxxlens::sdk {"
                    " void issue_169_redeclaration(int);"
                    " void issue_169_redeclaration(int value = 0);"
                    " }\n"
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                inventory.CallableInventoryError,
                "drifting public redeclarations",
            ):
                inventory.extract_ast_census(root, self.compiler)

    def test_macro_spelled_public_specifier_is_rejected_not_omitted(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-callable-macro-") as temporary:
            root = self.copied_ast_root(temporary)
            common = root / "include/cxxlens/sdk/common.hpp"
            common.write_text(
                common.read_text(encoding="utf-8")
                + (
                    "\n#define CXXLENS_ISSUE_169_CONSTEXPR constexpr\n"
                    "namespace cxxlens::sdk { CXXLENS_ISSUE_169_CONSTEXPR "
                    "int issue_169_macro_specifier() { return 169; } }\n"
                    "#undef CXXLENS_ISSUE_169_CONSTEXPR\n"
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                inventory.CallableInventoryError,
                "macro expansion is unsupported",
            ):
                inventory.extract_ast_census(root, self.compiler)

    def test_catalog_stability_drift_is_rejected(self) -> None:
        for expected, replacement in (
            ("source-versioned", "clang-major-versioned"),
            ("clang-major-versioned", "source-versioned"),
        ):
            with self.subTest(expected), self.assertRaisesRegex(
                inventory.CallableInventoryError, "callable stability differs from catalog"
            ):
                changed = copy.deepcopy(self.document)
                row = next(item for item in changed["callables"] if item["stability"] == expected)
                row["stability"] = replacement
                changed["inventory_digest"] = inventory.inventory_digest(changed)
                inventory.validate_inventory_document(ROOT, changed)

    def test_review_rejects_noncanonical_or_substituted_inventory(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-callable-report-source-") as temporary:
            alternate = pathlib.Path(temporary) / "inventory.yaml"
            shutil.copy2(ROOT / inventory.INVENTORY, alternate)
            with self.assertRaisesRegex(
                inventory.CallableInventoryError, "canonical inventory path"
            ):
                inventory._require_canonical_report_inventory(
                    ROOT, alternate, self.document
                )
        substituted = copy.deepcopy(self.document)
        substituted["callables"][0]["status"] = "substituted"
        with self.assertRaisesRegex(
            inventory.CallableInventoryError, "differs from the canonical inventory"
        ):
            inventory._require_canonical_report_inventory(
                ROOT, ROOT / inventory.INVENTORY, substituted
            )

    def test_wrong_clang_major_is_rejected_before_ast_extraction(self) -> None:
        wrong_version = subprocess.CompletedProcess(
            ["clang++-21", "--version"],
            0,
            stdout="Ubuntu clang version 21.1.0\n",
            stderr="",
        )
        with mock.patch.object(
            inventory.subprocess, "run", return_value=wrong_version
        ):
            with self.assertRaisesRegex(
                inventory.CallableInventoryError,
                "requires exact Clang major 22",
            ):
                inventory.clang_ast_roots(ROOT, "clang++-21", [])

    def test_token_canonicalizer_preserves_literal_spelling_and_removes_comments(self) -> None:
        source = (
            'call( "alpha  beta", R"tag(raw // text  /* bytes */)tag", '
            "' ', value/*comment*/+// line\n other)"
        )
        normalized = inventory.normalize_cpp_text(source)
        self.assertIn('"alpha  beta"', normalized)
        self.assertIn('R"tag(raw // text  /* bytes */)tag"', normalized)
        self.assertIn("' '", normalized)
        self.assertNotIn("comment", normalized)
        self.assertNotIn("// line", normalized)
        self.assertEqual(
            inventory._noexcept_spelling('const /* noexcept(false) */ &'),
            "none",
        )
        self.assertEqual(
            inventory._cv_ref_qualifiers(
                "noexcept ( sizeof ( const int ) > 0 && requires { value & value ; } )"
            ),
            ("none", "none"),
        )

    def test_nested_noexcept_requires_constraint_overloads_and_union_are_exact(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-callable-canonical-") as temporary:
            root = self.copied_ast_root(temporary)
            common = root / "include/cxxlens/sdk/common.hpp"
            common.write_text(
                common.read_text(encoding="utf-8")
                + """
namespace cxxlens::sdk {
template<class T> void issue_nested_noexcept(T) noexcept(noexcept(T{}()));
template<class T> requires (sizeof(T) == 1) void issue_constraint_only(T);
template<class T> requires (sizeof(T) == 2) void issue_constraint_only(T);
template<class T> requires requires(T value) { value == value; }
void issue_leading_requires(T);
template<class T> void issue_trailing_requires(T)
    requires requires(T value) { value == value; };
template<class T> requires (sizeof(T) == 4)
struct issue_constrained_record { void run(); };
template<class T> struct issue_partial_record;
template<class T> requires (sizeof(T) == 8)
struct issue_partial_record<T*> { void run(); };
union issue_public_union { void touch(); };
[[deprecated("static inline constexpr consteval explicit virtual")]]
void issue_attribute_words();
}
""",
                encoding="utf-8",
            )
            rows, _extractor = inventory.extract_ast_census(root, self.compiler)
        nested = next(
            row for row in rows if row["fully_qualified_name"].endswith("issue_nested_noexcept")
        )
        self.assertEqual(
            nested["signature"]["qualifiers"]["noexcept"],
            "noexcept ( noexcept ( T { } ( ) ) )",
        )
        overloads = [
            row for row in rows if row["fully_qualified_name"].endswith("issue_constraint_only")
        ]
        self.assertEqual(len(overloads), 2)
        self.assertEqual(
            {
                row["signature"]["constraints"][0]["source"]
                for row in overloads
            },
            {"requires ( sizeof ( T ) == 1 )", "requires ( sizeof ( T ) == 2 )"},
        )
        leading = next(
            row for row in rows if row["fully_qualified_name"].endswith("issue_leading_requires")
        )
        trailing = next(
            row for row in rows if row["fully_qualified_name"].endswith("issue_trailing_requires")
        )
        self.assertEqual(
            leading["signature"]["constraints"][-1],
            {
                "scope": "callable",
                "position": "leading-requires",
                "source": "requires requires ( T value ) { value == value ; }",
            },
        )
        self.assertEqual(
            trailing["signature"]["constraints"][-1]["position"],
            "trailing-requires",
        )
        member = next(
            row
            for row in rows
            if row["fully_qualified_name"].endswith("issue_constrained_record<T>::run")
        )
        self.assertIn(
            {
                "scope": "enclosing",
                "position": "leading-requires",
                "source": "requires ( sizeof ( T ) == 4 )",
            },
            member["signature"]["constraints"],
        )
        partial_member = next(
            row
            for row in rows
            if "issue_partial_record<" in row["fully_qualified_name"]
            and row["fully_qualified_name"].endswith("::run")
        )
        self.assertIn(
            {
                "scope": "enclosing",
                "position": "leading-requires",
                "source": "requires ( sizeof ( T ) == 8 )",
            },
            partial_member["signature"]["constraints"],
        )
        self.assertTrue(
            any(row["fully_qualified_name"].endswith("issue_public_union::touch") for row in rows)
        )
        attribute_words = next(
            row
            for row in rows
            if row["fully_qualified_name"].endswith("issue_attribute_words")
        )
        for specifier in (
            "static",
            "virtual",
            "inline",
            "constexpr",
            "consteval",
            "explicit",
        ):
            self.assertFalse(
                attribute_words["signature"]["specifiers"][specifier], specifier
            )

    def test_doxygen_union_projection_includes_requires_clause(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-callable-doxygen-union-") as temporary:
            xml = pathlib.Path(temporary) / "union.xml"
            xml.write_text(
                """<doxygen><compounddef kind="union"><sectiondef>
<memberdef kind="function" prot="public"><qualifiedname>cxxlens::sdk::u::run</qualifiedname>
<definition>void cxxlens::sdk::u::run</definition><argsstring>()</argsstring>
<requiresclause>requires (sizeof(T) == 1)</requiresclause>
<location file="include/cxxlens/sdk/common.hpp" line="1" column="1"/>
</memberdef></sectiondef></compounddef></doxygen>""",
                encoding="utf-8",
            )
            rows = inventory.extract_doxygen_census(pathlib.Path(temporary))
        self.assertEqual(
            rows[0]["projection"]["requires_clause"],
            "requires ( sizeof ( T ) == 1 )",
        )

    def test_unregistered_public_free_function_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-callable-free-") as temporary:
            root = self.copied_ast_root(temporary)
            common = root / "include/cxxlens/sdk/common.hpp"
            common.write_text(
                common.read_text(encoding="utf-8")
                + "\nnamespace cxxlens::sdk { int issue_169_unregistered_free(); }\n",
                encoding="utf-8",
            )
            self.assert_ast_rejected(root, "header/inventory callable sets differ")

    def test_unregistered_public_member_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-callable-member-") as temporary:
            root = self.copied_ast_root(temporary)
            common = root / "include/cxxlens/sdk/common.hpp"
            common.write_text(
                common.read_text(encoding="utf-8")
                + "\nnamespace cxxlens::sdk { struct issue_169_member { void run(); }; }\n",
                encoding="utf-8",
            )
            self.assert_ast_rejected(root, "header/inventory callable sets differ")

    def test_unregistered_overload_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-callable-overload-") as temporary:
            root = self.copied_ast_root(temporary)
            common = root / "include/cxxlens/sdk/common.hpp"
            common.write_text(
                common.read_text(encoding="utf-8")
                + "\nnamespace cxxlens::sdk { bool is_valid(int); }\n",
                encoding="utf-8",
            )
            self.assert_ast_rejected(root, "header/inventory callable sets differ")

    def test_qualifier_and_default_argument_drift_are_rejected(self) -> None:
        fixtures = (
            (
                "noexcept",
                "include/cxxlens/sdk/common.hpp",
                "constexpr bool is_valid(const canonical_value::kind value) noexcept",
                "constexpr bool is_valid(const canonical_value::kind value) /* drift */",
            ),
            (
                "default",
                "include/cxxlens/sdk/query.hpp",
                "from(std::string_view alias = {})",
                'from(std::string_view alias = "drift  value")',
            ),
        )
        for label, relative, old, new in fixtures:
            with self.subTest(label), tempfile.TemporaryDirectory(
                prefix=f"cxxlens-callable-{label}-"
            ) as temporary:
                root = self.copied_ast_root(temporary)
                self.replace_once(root / relative, old, new)
                self.assert_ast_rejected(root, "public callable signatures differ")

    def test_inventory_only_row_after_declaration_removal_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-callable-removed-") as temporary:
            root = self.copied_ast_root(temporary)
            common = root / "include/cxxlens/sdk/common.hpp"
            self.replace_once(
                common,
                "\t[[nodiscard]] std::string content_digest(std::span<const std::byte> bytes);\n",
                "",
            )
            self.assert_ast_rejected(root, "header/inventory callable sets differ")

    def test_one_callable_owned_by_multiple_inventory_rows_is_rejected(self) -> None:
        changed = copy.deepcopy(self.document)
        duplicate = copy.deepcopy(changed["callables"][0])
        scope = inventory._identity_scope_key(duplicate)
        allocator_scope = next(
            row
            for row in changed["allocator"]["scopes"]
            if inventory._identity_scope_key(row) == scope
        )
        duplicate_slot = allocator_scope["next_overload_slot"]
        allocator_scope["next_overload_slot"] += 1
        duplicate["identity"] = {"overload_slot": duplicate_slot}
        duplicate["id"] = inventory.callable_id(duplicate, duplicate_slot)
        changed["callables"].append(duplicate)
        changed["callables"].sort(key=lambda row: row["id"])
        changed["inventory_digest"] = inventory.inventory_digest(changed)
        with self.assertRaisesRegex(
            inventory.CallableInventoryError,
            "one public callable is owned by multiple inventory rows",
        ):
            inventory.validate_inventory_document(ROOT, changed)

    def test_private_callable_added_to_inventory_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-callable-private-") as temporary:
            root = self.copied_ast_root(temporary)
            common = root / "include/cxxlens/sdk/common.hpp"
            common.write_text(
                common.read_text(encoding="utf-8")
                + (
                    "\nnamespace cxxlens::sdk { class issue_169_private {"
                    " private: void secret(); }; }\n"
                ),
                encoding="utf-8",
            )
            changed = copy.deepcopy(self.document)
            private_row = copy.deepcopy(
                next(
                    row
                    for row in changed["callables"]
                    if row["callable_kind"] == "member"
                )
            )
            private_row["id"] = "cpp.callable.999999"
            private_row["fully_qualified_name"] = (
                "cxxlens::sdk::issue_169_private::secret"
            )
            private_row["declaring_header"] = "include/cxxlens/sdk/common.hpp"
            private_row["signature"] = {
                "source": "void cxxlens::sdk::issue_169_private::secret ()",
                "return_type": "void",
                "parameters": [],
                "template_parameters": [],
                "constraints": [],
                "specifiers": {
                    "static": False,
                    "virtual": False,
                    "pure_virtual": False,
                    "inline": False,
                    "constexpr": False,
                    "consteval": False,
                    "explicit": False,
                    "override": False,
                    "final": False,
                },
                "qualifiers": {"cv": "none", "ref": "none", "noexcept": "none"},
                "declaration_state": "declared",
            }
            private_row["origin"] = "out-of-line"
            changed["callables"].append(private_row)
            changed["callables"].sort(key=lambda row: row["id"])
            self.assert_ast_rejected_document(
                root, changed, "header/inventory callable sets differ"
            )

    def assert_ast_rejected_document(
        self, root: pathlib.Path, document: dict, pattern: str
    ) -> None:
        with self.assertRaisesRegex(inventory.CallableInventoryError, pattern):
            inventory.check_ast_inventory(root, self.compiler, document)

    def test_manual_generated_header_edit_is_rejected(self) -> None:
        catalog = load_yaml(ROOT / "schemas/cxxlens_ng_public_api_catalog.yaml")
        registry = load_yaml(ROOT / "schemas/cxxlens_ng_relation_registry.yaml")
        relation, relative = admitted_generated_relations(catalog, registry)[0]
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-callable-generated-"
        ) as temporary:
            candidate = pathlib.Path(temporary) / relative.name
            candidate.write_text(
                (ROOT / relative).read_text(encoding="utf-8")
                + "// forbidden manual edit\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                SdkContractError, "committed generated relation is stale"
            ):
                validate_generated_relation_header(
                    relation, candidate, label=relative.as_posix()
                )

    def test_quality_and_ci_wiring_is_fail_closed(self) -> None:
        developer_tools = (ROOT / "cmake/CxxlensDeveloperTools.cmake").read_text(
            encoding="utf-8"
        )
        self.assertLess(
            developer_tools.index("cxxlens-ng-public-callable-inventory-check"),
            developer_tools.index("if(CXXLENS_BUILD_DOCS)"),
        )
        self.assertIn(
            "add_dependencies(cxxlens-quality cxxlens-doxygen-contract)",
            developer_tools,
        )
        self.assertIn("check-doxygen", developer_tools)
        workflow = (ROOT / ".github/workflows/quality.yml").read_text(
            encoding="utf-8"
        )
        for marker in (
            "public_callable_inventory.py report",
            "--compiler clang++-22",
            "--expected-revision \"${GITHUB_SHA}\"",
            "cxxlens-ng-public-callable-inventory-${{ github.sha }}",
            "cxxlens-ng-public-callable-inventory-report.json",
            "cxxlens-ng-public-callable-inventory-review.md",
            "fetch-depth: 2",
        ):
            self.assertIn(marker, workflow)


if __name__ == "__main__":
    unittest.main()
