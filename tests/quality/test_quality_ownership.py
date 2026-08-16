#!/usr/bin/env python3
"""Positive and fail-closed tests for quality evidence ownership."""

from __future__ import annotations

import copy
import pathlib
import shutil
import sys
import unittest
import tempfile

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/quality"))

from check_quality_ownership import (  # noqa: E402
    CONSTRUCTIBILITY_MANIFEST,
    CONSTRUCTIBILITY_SCHEMA,
    MANIFEST,
    QualityOwnershipError,
    canonical_digest,
    evidence_id,
    load_yaml,
    select_mode,
    validate_constructibility_projection,
    validate_evidence,
    validate_manifest,
)
from collect_toolchain_provenance import pinned_actions  # noqa: E402
from install_artifact_manifest import (  # noqa: E402
    InstallArtifactError,
    build_manifest,
    verify_manifest,
)


def evidence(check: str) -> dict:
    configuration = "test"
    record = {
        "schema": "cxxlens.quality-evidence.v1",
        "logical_check_id": check,
        "check_version": 1,
        "configuration": configuration,
        "revision": "1" * 40,
        "tree": "2" * 40,
        "toolchain_digest": "sha256:" + "3" * 64,
        "configuration_digest": canonical_digest(configuration),
        "checker_digest": "sha256:" + "5" * 64,
        "input_digest": "sha256:" + "6" * 64,
        "output_digest": "sha256:" + "7" * 64,
        "result": "passed",
    }
    record["evidence_id"] = evidence_id(record)
    return record


class QualityOwnershipTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = load_yaml(ROOT / MANIFEST)

    def test_manifest_and_repository_wiring_are_valid(self) -> None:
        validate_manifest(ROOT, self.manifest)

    def test_implementation_learning_input_drift_is_rejected(self) -> None:
        for identifier, missing in (
            ("quality.unit-contracts", "AGENTS.md"),
            ("quality.unit-contracts", "docs"),
            (
                "quality.production-contracts",
                ".github/ISSUE_TEMPLATE/design-feedback.yml",
            ),
            ("quality.production-contracts", ".markdownlint-cli2.jsonc"),
        ):
            with self.subTest(identifier=identifier):
                manifest = copy.deepcopy(self.manifest)
                check = next(
                    row for row in manifest["checks"] if row["id"] == identifier
                )
                check["inputs"].remove(missing)
                with self.assertRaisesRegex(
                    QualityOwnershipError,
                    "implementation-learning inputs are incomplete",
                ):
                    validate_manifest(ROOT, manifest)

    def test_exact_evidence_set_is_accepted(self) -> None:
        validate_evidence(
            [evidence("a"), evidence("b")], {("a", "test"), ("b", "test")}
        )

    def test_duplicate_evidence_is_rejected(self) -> None:
        row = evidence("a")
        with self.assertRaisesRegex(QualityOwnershipError, "duplicate logical evidence"):
            validate_evidence([row, copy.deepcopy(row)], {("a", "test")})

    def test_missing_required_evidence_is_rejected(self) -> None:
        with self.assertRaisesRegex(QualityOwnershipError, "instance set differs"):
            validate_evidence([evidence("a")], {("a", "test"), ("b", "test")})

    def test_each_binding_field_mutation_is_rejected(self) -> None:
        baseline = evidence("a")
        for field in (
            "revision",
            "tree",
            "toolchain_digest",
            "configuration_digest",
            "checker_digest",
            "input_digest",
            "output_digest",
        ):
            with self.subTest(field=field):
                changed = copy.deepcopy(baseline)
                changed[field] += "x"
                with self.assertRaisesRegex(QualityOwnershipError, "does not match"):
                    validate_evidence([changed], {("a", "test")})

    def test_configuration_mutation_is_rejected_independently(self) -> None:
        changed = evidence("a")
        changed["configuration"] = "other"
        changed["evidence_id"] = evidence_id(changed)
        with self.assertRaisesRegex(QualityOwnershipError, "configuration digest"):
            validate_evidence([changed], {("a", "other")})

    def test_one_check_can_require_distinct_configurations(self) -> None:
        static = evidence("a")
        static["configuration"] = "static"
        static["configuration_digest"] = canonical_digest("static")
        static["evidence_id"] = evidence_id(static)
        shared = copy.deepcopy(static)
        shared["configuration"] = "shared"
        shared["configuration_digest"] = canonical_digest("shared")
        shared["evidence_id"] = evidence_id(shared)
        validate_evidence(
            [static, shared], {("a", "static"), ("a", "shared")}
        )

    def test_selector_expands_authoritative_and_unknown_changes(self) -> None:
        for path in (
            "include/cxxlens/sdk.hpp",
            "schemas/new.yaml",
            "cmake/new.cmake",
            ".github/workflows/quality.yml",
            "tools/quality/check_quality_ownership.py",
            "unclassified.asset",
        ):
            with self.subTest(path=path):
                self.assertEqual(select_mode([path]), "full")
        self.assertEqual(select_mode(["src/sdk/common.cpp"]), "fast")
        self.assertEqual(select_mode(["docs/development/build-and-test.md"]), "check")
        self.assertEqual(select_mode(["src/sdk/common.cpp"], graph_available=False), "full")

    def test_mutable_workflow_action_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            workflows = root / ".github/workflows"
            workflows.mkdir(parents=True)
            (workflows / "quality.yml").write_text(
                "steps:\n  - uses: actions/checkout@v4\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(ValueError, "not pinned"):
                pinned_actions(root)

    def test_install_artifact_binding_and_file_swap_are_fail_closed(self) -> None:
        compiler = pathlib.Path(shutil.which("c++") or "")
        with tempfile.TemporaryDirectory() as temporary:
            prefix = pathlib.Path(temporary) / "prefix"
            prefix.mkdir()
            artifact = prefix / "artifact.txt"
            artifact.write_text("accepted\n", encoding="utf-8")
            baseline = build_manifest(ROOT, prefix, compiler, "static-test")
            verify_manifest(ROOT, prefix, compiler, "static-test", baseline)

            for mutate in (
                lambda row: row["source"].__setitem__("revision", "0" * 40),
                lambda row: row["source"].__setitem__("tree", "0" * 40),
                lambda row: row.__setitem__("configuration", "shared-test"),
                lambda row: row["toolchain"].__setitem__(
                    "binary_digest", "sha256:" + "0" * 64
                ),
            ):
                with self.subTest(mutation=mutate):
                    changed = copy.deepcopy(baseline)
                    mutate(changed)
                    with self.assertRaisesRegex(InstallArtifactError, "binding mismatch"):
                        verify_manifest(ROOT, prefix, compiler, "static-test", changed)

            artifact.write_text("substituted\n", encoding="utf-8")
            with self.assertRaisesRegex(
                InstallArtifactError,
                r"binding mismatch: .*files .*changed=\['artifact.txt'\]",
            ):
                verify_manifest(ROOT, prefix, compiler, "static-test", baseline)

    def test_install_artifact_rejects_stale_occurrence_provenance(self) -> None:
        compiler = pathlib.Path(shutil.which("c++") or "")
        with tempfile.TemporaryDirectory() as temporary:
            prefix = pathlib.Path(temporary) / "prefix"
            occurrence = prefix / "share/cxxlens/materialization/clang22"
            occurrence.mkdir(parents=True)
            (prefix / "artifact.txt").write_text("accepted\n", encoding="utf-8")
            (occurrence / "occurrence-v1.json").write_text(
                '{"source_revision":"' + "0" * 40 + '","source_tree":"' + "0" * 40 + '"}\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                InstallArtifactError,
                "occurrence source provenance mismatch.*refusing to create",
            ):
                build_manifest(ROOT, prefix, compiler, "static-test")

    def test_install_artifact_accepts_configured_source_provenance(self) -> None:
        compiler = pathlib.Path(shutil.which("c++") or "")
        revision = "a" * 40
        tree = "b" * 40
        with tempfile.TemporaryDirectory() as temporary:
            prefix = pathlib.Path(temporary) / "prefix"
            occurrence = prefix / "share/cxxlens/materialization/clang22"
            occurrence.mkdir(parents=True)
            (prefix / "artifact.txt").write_text("accepted\n", encoding="utf-8")
            (occurrence / "occurrence-v1.json").write_text(
                '{"source_revision":"' + revision + '","source_tree":"' + tree + '"}\n',
                encoding="utf-8",
            )
            manifest = build_manifest(
                ROOT,
                prefix,
                compiler,
                "static-test",
                source_revision=revision,
                source_tree=tree,
            )
            self.assertEqual(manifest["source"], {"revision": revision, "tree": tree})
            verify_manifest(
                ROOT,
                prefix,
                compiler,
                "static-test",
                manifest,
                source_revision=revision,
                source_tree=tree,
            )

    def test_install_test_passes_configured_source_identity(self) -> None:
        install_script = (ROOT / "tests/install/run_install_test.cmake.in").read_text(
            encoding="utf-8"
        )
        self.assertIn('--source-revision "@CXXLENS_SOURCE_REVISION@"', install_script)
        self.assertIn('--source-tree "@CXXLENS_SOURCE_TREE@"', install_script)

    def test_install_test_uses_configured_executable_suffix(self) -> None:
        install_script = (ROOT / "tests/install/run_install_test.cmake.in").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            'set(install_executable_suffix "@CMAKE_EXECUTABLE_SUFFIX@")',
            install_script,
        )
        self.assertIn(
            "function(resolve_install_artifact_path canonical_path resolved_path)",
            install_script,
        )
        self.assertIn(
            'string(APPEND resolved "${install_executable_suffix}")',
            install_script,
        )
        for executable in (
            "cxxlens-provider-scaffold",
            "cxxlens-sdk-doctor",
            "cxxlens-clang-worker-22",
            "cxxlens-clang22-materialize",
        ):
            with self.subTest(executable=executable):
                self.assertIn(
                    f'"${{install_prefix}}/bin/{executable}"',
                    install_script,
                )
        for executable in (
            "cxxlens-provider-scaffold",
            "cxxlens-sdk-doctor",
            "cxxlens-clang-worker-22",
        ):
            with self.subTest(runtime_executable=executable):
                self.assertIn(
                    f"${{install_prefix}}/bin/{executable}${{install_executable_suffix}}",
                    install_script,
                )
        self.assertIn(
            'resolve_install_artifact_path("${required}" resolved_required)',
            install_script,
        )
        self.assertIn(
            "${build_dir}/cxxlens-${consumer_executable}${install_executable_suffix}",
            install_script,
        )
        self.assertIn(
            "${example_build_dir}/cxxlens-installed-${example}${install_executable_suffix}",
            install_script,
        )


class ConstructibilityGateProjectionTest(unittest.TestCase):
    @staticmethod
    def copied_authority_root(temporary: str) -> pathlib.Path:
        root = pathlib.Path(temporary)
        for relative in (CONSTRUCTIBILITY_MANIFEST, CONSTRUCTIBILITY_SCHEMA):
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        return root

    @staticmethod
    def write_manifest(root: pathlib.Path, manifest: dict) -> None:
        (root / CONSTRUCTIBILITY_MANIFEST).write_text(
            yaml.safe_dump(manifest, sort_keys=False, allow_unicode=True),
            encoding="utf-8",
        )

    def test_admitted_projection_passes_and_reports_file_provenance(self) -> None:
        result = validate_constructibility_projection(ROOT)
        self.assertEqual(result["contract"], "development.constructibility-gate.v1")
        self.assertEqual(result["gate_issue"], "#276")
        self.assertEqual(result["blocked_issue"], "#261")
        self.assertEqual(result["disposition"], "blocked")
        self.assertEqual(result["witness_count"], 7)
        self.assertTrue(result["manifest_digest"].startswith("sha256:"))
        self.assertTrue(result["schema_digest"].startswith("sha256:"))

    def test_authority_schema_rejects_constructibility_mutations(self) -> None:
        mutations = (
            lambda manifest: manifest["product_direction"]["constructibility_gate"][
                "required_witnesses"
            ].append("synthetic-witness"),
            lambda manifest: manifest["product_direction"]["agent_context"]["first_packet"][
                "constructibility"
            ].__setitem__("disposition", "constructible"),
            lambda manifest: manifest["product_direction"]["agent_context"]["first_packet"][
                "constructibility"
            ].__setitem__("disposition", "not-applicable"),
            lambda manifest: manifest["product_direction"]["agent_context"]["first_packet"][
                "constructibility"
            ].__setitem__("gate_issue", "#261"),
            lambda manifest: manifest["product_direction"]["agent_context"]["first_packet"][
                "exact_contract_ids"
            ].remove("development.constructibility-gate.v1"),
        )
        for mutate in mutations:
            with self.subTest(mutation=mutate):
                with tempfile.TemporaryDirectory() as temporary:
                    root = self.copied_authority_root(temporary)
                    manifest = load_yaml(root / CONSTRUCTIBILITY_MANIFEST)
                    mutate(manifest)
                    self.write_manifest(root, manifest)
                    with self.assertRaises(QualityOwnershipError):
                        validate_constructibility_projection(root)

if __name__ == "__main__":
    unittest.main()
