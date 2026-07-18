#!/usr/bin/env python3
"""Validate the implemented next-generation author SDK contract."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile
from typing import Any

import jsonschema
import yaml

import check_ng_query_contract as query_contract


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "sdk"))
from relation_idl_compiler import canonical_relation  # noqa: E402
CATALOG = pathlib.Path("schemas/cxxlens_ng_public_api_catalog.yaml")
SCHEMA = pathlib.Path("schemas/cxxlens_ng_public_api_catalog.schema.yaml")
PROJECT_CATALOG_CONTRACT = pathlib.Path("schemas/cxxlens_ng_project_catalog_contract.yaml")
PROJECT_CATALOG_SCHEMA = pathlib.Path("schemas/cxxlens_ng_project_catalog_contract.schema.yaml")
PROVIDER_TASK_CONTRACT = pathlib.Path("schemas/cxxlens_ng_portable_provider_task_contract.yaml")
PROVIDER_TASK_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_portable_provider_task_contract.schema.yaml"
)
EXPECTED_PATHS = {
    "generated-typed-query",
    "runtime-dynamic-query",
    "portable-provider",
    "clang22-native-provider",
    "high-level-recipe",
}
EXPECTED_IMPLEMENTED_ENTRIES = {
    "public.common",
    "public.project-catalog",
    "public.relation-static",
    "public.relation-dynamic",
    "public.claim-kernel",
    "public.snapshot-store",
    "public.logical-query",
    "public.query-runtime",
    "public.provider-sdk",
    "public.provider-runtime",
    "public.native-provider-sdk",
    "public.recipe-foundation",
}
FORBIDDEN_ORDINARY = (
    re.compile(r"\bclang::"),
    re.compile(r"\bllvm::"),
    re.compile(r"#\s*include\s*[<\"](?:clang|llvm)/"),
)


class SdkContractError(ValueError):
    """Stable SDK catalog or implementation violation."""


def fail(message: str) -> None:
    raise SdkContractError(message)


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail(f"expected mapping: {path}")
    return value


def schema_validate(document: dict[str, Any], schema: dict[str, Any]) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"SDK catalog schema validation failed: {error.message}")


def unique_rows(rows: list[dict[str, Any]], label: str) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for row in rows:
        identifier = row["id"]
        if identifier in result:
            fail(f"duplicate {label} ID: {identifier}")
        result[identifier] = row
    return result


def validate_project_catalog_contract(
    root: pathlib.Path, entries: dict[str, dict[str, Any]]
) -> None:
    contract = load_yaml(root / PROJECT_CATALOG_CONTRACT)
    schema_validate(contract, load_yaml(root / PROJECT_CATALOG_SCHEMA))
    public_entry = entries.get("public.project-catalog", {})
    if public_entry.get("owner_issue") != "#121":
        fail("project catalog ownership must remain with Issue #121")
    signature = "\n".join(
        symbol.get("signature", "") for symbol in public_entry.get("symbols", [])
    )
    for marker in (
        "catalog_compile_unit",
        "effective_invocation_digest",
        "source_digest",
        "environment_digest",
        "project_catalog> make",
        "canonical_projection",
    ):
        if marker not in signature:
            fail(f"project catalog public projection marker is missing: {marker}")

    registry = load_yaml(root / "schemas/cxxlens_ng_relation_registry.yaml")
    project = next(
        (row for row in registry["relations"] if row["name"] == "build.project"), None
    )
    if project is None:
        fail("build.project catalog consumer is missing")
    columns = {column["name"] for column in project["columns"]}
    required = {"catalog", "catalog_digest", "logical_root", "environment_digest"}
    if not required.issubset(columns):
        fail(f"build.project catalog authority fields are missing: {sorted(required - columns)}")

    header = (root / "include/cxxlens/sdk/relation.hpp").read_text(encoding="utf-8")
    relation_source = (root / "src/sdk/relation.cpp").read_text(encoding="utf-8")
    provider_source = (root / "src/sdk/provider.cpp").read_text(encoding="utf-8")
    worker_source = (root / "src/llvm/clang22/provider_worker.cpp").read_text(encoding="utf-8")
    for marker in ("catalog_compile_unit", "project_catalog::make", "canonical_projection"):
        if marker not in header + relation_source:
            fail(f"project catalog implementation marker is missing: {marker}")
    validation = provider_source.find("task_value.validate()")
    acceptance = provider_source.find("message_type::task_accepted", validation)
    if validation < 0 or acceptance < 0 or validation > acceptance:
        fail("provider task catalog is not validated before task_accepted")
    if "sdk::project_catalog::make(" not in worker_source:
        fail("native provider worker bypasses the shared project catalog loader")


def validate_provider_task_contract(
    root: pathlib.Path, entries: dict[str, dict[str, Any]]
) -> None:
    contract = load_yaml(root / PROVIDER_TASK_CONTRACT)
    schema_validate(contract, load_yaml(root / PROVIDER_TASK_SCHEMA))
    public_entry = entries.get("public.provider-sdk", {})
    if public_entry.get("owner_issue") != "#140":
        fail("portable provider SDK ownership must remain with Issue #140")
    signatures = "\n".join(
        symbol.get("signature", "") for symbol in public_entry.get("symbols", [])
    )
    for marker in (
        "semantic_contract_digest",
        "provider_session",
        "task::make",
        "canonical_projection",
        "dependency_groups",
        "encode_host_transcript",
        "validate_host_transcript",
    ):
        if marker not in signatures:
            fail(f"portable provider task public marker is missing: {marker}")

    provider_source = (root / "src/sdk/provider.cpp").read_text(encoding="utf-8")
    runtime_source = (root / "src/sdk/provider_runtime.cpp").read_text(encoding="utf-8")
    validation = provider_source.find("task_value.validate()")
    acceptance = provider_source.find("message_type::task_accepted", validation)
    if validation < 0 or acceptance < 0 or validation > acceptance:
        fail("portable provider task validation does not precede task_accepted")
    for marker in (
        "task-output-or-dependency",
        "task-output-whitelist",
        "provider.semantic_contract_digest()",
        "task_value.dependency_groups",
    ):
        if marker not in provider_source:
            fail(f"portable provider task implementation marker is missing: {marker}")
    for marker in (
        "decode_batch_begin_metadata(value.control)",
        "metadata->task_id != request.task_id",
    ):
        if marker not in runtime_source:
            fail(f"batch_begin task binding marker is missing: {marker}")
    protocol = load_yaml(root / "schemas/cxxlens_ng_provider_protocol.yaml")
    if protocol["state_machine_validation"]["exact_bindings"]["batch_begin"][0] != "task-id":
        fail("provider protocol batch_begin omits task ID")


def validate_static_row_view_contract(
    root: pathlib.Path, entries: dict[str, dict[str, Any]]
) -> None:
    public_entry = entries.get("public.relation-static", {})
    if public_entry.get("owner_issue") != "#154":
        fail("static row view exact validation ownership must remain with Issue #154")
    required_errors = {
        "sdk.row-descriptor-mismatch",
        "sdk.foreign-column",
        "sdk.column-not-found",
        "sdk.cell-type-mismatch",
        "sdk.cell-invalid",
        "sdk.unknown-cell",
    }
    if not required_errors.issubset(public_entry.get("errors", [])):
        fail("static row view catalog omits exact validation errors")

    header = (root / "include/cxxlens/sdk/relation.hpp").read_text(encoding="utf-8")
    begin = header.find("class static_row_view")
    end = header.find("\n\t};", begin)
    if begin < 0 or end < 0:
        fail("static_row_view public implementation is missing")
    view = header[begin:end]
    for marker in (
        "reference.descriptor_id != descriptor.id",
        "descriptor.column(reference.column_id)",
        "reference.type == expected->type",
        "validate_row(descriptor, row_)",
        "detached_cell::absent(reference.type)",
    ):
        if marker not in view:
            fail(f"static_row_view exact validation marker is missing: {marker}")
    if view.find("validate_row(descriptor, row_)") > view.find(
        "row_.cells.find(reference.column_id)"
    ):
        fail("static_row_view reads the cell before complete row validation")

    test = (root / "tests/unit/sdk/sdk_test.cpp").read_text(encoding="utf-8")
    for marker in (
        "static-row-view-validation",
        "static typed read accepted a wrong-type integer cell",
        "static typed read accepted an invalid digest",
        "static typed read accepted an invalid closed symbol",
        "static typed read accepted invalid UTF-8",
        "static typed read accepted a row with a different descriptor shape",
        "validated dynamic/static read parity or typed optional absence failed",
    ):
        if marker not in test:
            fail(f"static_row_view acceptance marker is missing: {marker}")


def validate_claim_evidence_occurrence_contract(
    root: pathlib.Path, entries: dict[str, dict[str, Any]]
) -> None:
    public_entry = entries.get("public.claim-kernel", {})
    required_invariants = {
        "evidence-occurrence-is-one-self-contained-claim-envelope-with-no-detached-reference-or-record-collection",
        "occurrence-subject-is-structurally-bound-by-descriptor-semantic-key-assertion-content-and-row",
        "one-occurrence-belongs-to-exactly-one-semantic-claim-content-and-is-never-shared-across-contents",
        "missing-orphan-and-ambiguous-evidence-resolution-are-unrepresentable",
    }
    if not required_invariants.issubset(public_entry.get("invariants", [])):
        fail("claim evidence occurrence catalog omits its structural binding law")
    if "docs/design/adr/0086-self-contained-claim-evidence-occurrences.md" not in public_entry.get(
        "implementation_evidence", []
    ):
        fail("claim evidence occurrence catalog omits Issue #155 evidence")

    header = (root / "include/cxxlens/sdk/claim.hpp").read_text(encoding="utf-8")
    if "detached evidence-ID reference collection" not in header:
        fail("public claim documentation omits self-contained evidence occurrence ownership")
    claim_source = (root / "src/sdk/claim.cpp").read_text(encoding="utf-8")
    for marker in (
        "value.descriptor",
        "value.semantic_key",
        "value.assertion",
        "value.content",
        "value.row.canonical_form()",
        "if (auto valid = validate_claim(engine, value); !valid)",
    ):
        if marker not in claim_source:
            fail(f"claim occurrence subject binding marker is missing: {marker}")
    store_source = (root / "src/sdk/store.cpp").read_text(encoding="utf-8")
    if "if (auto valid = validate_claim(engine, output); !valid)" not in store_source:
        fail("persisted claim decoder does not share claim identity validation")
    test = (root / "tests/unit/sdk/sdk_test.cpp").read_text(encoding="utf-8")
    for marker in (
        "has_detached_evidence_references<cxxlens::sdk::claim>",
        "has_detached_evidence_references<cxxlens::sdk::claim_batch_result>",
        "evidence occurrence subject repoint was accepted",
        "evidence occurrence content repoint was accepted",
        "claim occurrence law lost metadata or retained an exact duplicate",
    ):
        if marker not in test:
            fail(f"claim evidence occurrence acceptance marker is missing: {marker}")


def validate_catalog(root: pathlib.Path, catalog: dict[str, Any]) -> None:
    schema_validate(catalog, load_yaml(root / SCHEMA))
    paths = unique_rows(catalog["author_paths"], "author path")
    if set(paths) != EXPECTED_PATHS:
        fail(f"author path set differs: {sorted(paths)}")
    entries = unique_rows(catalog["entries"], "public entry")
    implemented = {
        identifier
        for identifier, entry in entries.items()
        if entry["status"] == "implemented"
    }
    if not EXPECTED_IMPLEMENTED_ENTRIES.issubset(implemented):
        fail(
            "implemented SDK entry set is incomplete: "
            f"{sorted(EXPECTED_IMPLEMENTED_ENTRIES - implemented)}"
        )
    if entries.get("public.recipe", {}).get("owner_issue") != "#136":
        fail("flagship recipe closed-world ownership must remain with Issue #136")
    validate_project_catalog_contract(root, entries)
    validate_provider_task_contract(root, entries)
    validate_static_row_view_contract(root, entries)
    validate_claim_evidence_occurrence_contract(root, entries)
    recipe_source = (root / "src/sdk/recipe.cpp").read_text(encoding="utf-8")
    for marker in (
        "execution_status::complete",
        "execution_status::truncated",
        "execution_status::cancelled_with_partial",
        "execution_status::failed_before_result",
        "call_search_state::partial",
        "call_search_state::failed",
        "result.closed()",
        "result.closure_ids().empty()",
        'result.summary_guarantee().approximation == "exact"',
    ):
        if marker not in recipe_source:
            fail(f"recipe execution-completeness marker is missing: {marker}")
    for path in paths.values():
        if path["entry"] not in entries:
            fail(f"author path references unknown entry: {path['entry']}")
        if path.get("negative_mode") not in {"compile-fail", "runtime-rejection"}:
            fail(f"author path has no exact negative mode: {path['id']}")
    for entry in entries.values():
        for dependency in entry.get("depends_on", []):
            if dependency not in entries:
                fail(f"public entry has dangling dependency: {entry['id']} -> {dependency}")

    referenced_paths: set[str] = set()
    for package in catalog["packages"]:
        referenced_paths.update(package["headers"])
    for path in paths.values():
        referenced_paths.update(
            path[field]
            for field in ("implementation", "positive_example", "negative_example", "harness")
        )
    for entry in entries.values():
        referenced_paths.update(entry["headers"])
        referenced_paths.update(entry["implementation_evidence"])
    missing = sorted(path for path in referenced_paths if not (root / path).is_file())
    if missing:
        fail(f"SDK catalog evidence is missing: {missing}")

    emitted_codes: set[str] = set()
    for source in [
        *sorted((root / "src/sdk").glob("*.cpp")),
        root / "src/llvm/clang22/provider_sdk.cpp",
    ]:
        emitted_codes.update(
            re.findall(
                r'"((?:sdk|store|provider|native|recipe)\.[a-z0-9._-]+)"',
                source.read_text(encoding="utf-8"),
            )
        )
    catalog_codes = {
        code
        for entry in entries.values()
        if entry["status"] == "implemented"
        for code in entry["errors"]
    }
    if missing_codes := sorted(emitted_codes - catalog_codes):
        fail(f"implemented SDK error codes are absent from the catalog: {missing_codes}")


def validate_boundaries(root: pathlib.Path) -> None:
    ordinary_roots = [root / "include/cxxlens/sdk", root / "include/cxxlens/relations"]
    violations: list[str] = []
    for ordinary_root in ordinary_roots:
        for header in sorted(ordinary_root.rglob("*.hpp")):
            for number, line in enumerate(header.read_text(encoding="utf-8").splitlines(), 1):
                if any(pattern.search(line) for pattern in FORBIDDEN_ORDINARY):
                    violations.append(f"{header.relative_to(root)}:{number}")
    for source in sorted((root / "src/sdk").glob("*.cpp")):
        for number, line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
            if any(pattern.search(line) for pattern in FORBIDDEN_ORDINARY):
                violations.append(f"{source.relative_to(root)}:{number}")
    if violations:
        fail(f"ordinary SDK leaks LLVM/Clang: {violations}")

    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    for target in (
        "cxxlens_base",
        "cxxlens_kernel",
        "cxxlens_query",
        "cxxlens_cpp",
        "cxxlens_recipes",
        "cxxlens_provider_sdk",
        "cxxlens_clang22_provider_sdk",
    ):
        if not re.search(rf"add_library\(\s*{target}\b", cmake):
            fail(f"next-generation target DAG marker is missing: {target}")
    provider_block = re.search(
        r"add_library\(\s*cxxlens_provider_sdk\b(.*?)"
        r"add_library\(\s*cxxlens_clang22_provider_sdk\b",
        cmake,
        re.DOTALL,
    )
    if provider_block is None:
        fail("independent cxxlens_provider_sdk target is missing")
    if re.search(r"\b(?:LLVM|Clang)\b", provider_block.group(1)):
        fail("ordinary provider SDK target has a forbidden LLVM/Clang dependency")
    for marker in (
        "EXPORT cxxlensProviderSDKTargets",
        "cxxlensProviderSDKConfig.cmake",
        "EXPORT cxxlensClang22ProviderSDKTargets",
    ):
        if marker not in cmake:
            fail(f"SDK install package marker is missing: {marker}")

    extension_text = "\n".join(
        path.read_text(encoding="utf-8")
        for directory in ordinary_roots
        for path in sorted(directory.rglob("*.hpp"))
    )
    for pattern in (
        r"enum\s+class\s+relation_(?:id|kind|type)",
        r"enum\s+class\s+provider_(?:id|kind|type)",
        r"switch\s*\([^)]*(?:relation|provider)",
    ):
        if re.search(pattern, extension_text):
            fail(f"central relation/provider dispatch is forbidden: {pattern}")


def run(command: list[str], *, expect_success: bool, label: str) -> None:
    completed = subprocess.run(command, check=False, text=True, capture_output=True)
    succeeded = completed.returncode == 0
    if succeeded != expect_success:
        detail = completed.stderr.strip() or completed.stdout.strip()
        fail(f"{label} {'unexpectedly succeeded' if succeeded else 'failed'}: {detail}")


def validate_generation_and_negatives(root: pathlib.Path, compiler: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cxxlens-sdk-contract-") as directory:
        temporary = pathlib.Path(directory)
        registry = load_yaml(root / "schemas/cxxlens_ng_relation_registry.yaml")
        generated_relations = {
            "cc.entity": "cc_entity.hpp",
            "cc.call_site": "cc_call_site.hpp",
            "cc.call_direct_target": "cc_call_direct_target.hpp",
            "company.lock.acquire": "company_lock_acquire.hpp",
        }
        for name, filename in generated_relations.items():
            generated = temporary / filename
            run(
                [
                    sys.executable,
                    str(root / "tools/sdk/relation_idl_compiler.py"),
                    "--registry",
                    str(root / "schemas/cxxlens_ng_relation_registry.yaml"),
                    "--relation",
                    name,
                    "--output",
                    str(generated),
                ],
                expect_success=True,
                label=f"relation IDL generation {name}",
            )
            committed = root / "include/cxxlens/relations" / filename
            if generated.read_bytes() != committed.read_bytes():
                fail(f"committed generated relation is stale: {committed.relative_to(root)}")
            relation = next(row for row in registry["relations"] if row["name"] == name)
            generated_text = generated.read_text(encoding="utf-8")
            for marker in [
                relation["descriptor_id"],
                relation["semantics"],
                relation["owner_namespace"],
                *(row["id"] for row in relation["columns"]),
            ]:
                if marker not in generated_text:
                    fail(f"generated relation tag omitted registry identity: {marker}")
            relation_canonical = json.dumps(
                canonical_relation(relation),
                ensure_ascii=False,
                separators=(",", ":"),
                sort_keys=True,
            )
            relation_digest = "sha256:" + hashlib.sha256(
                relation_canonical.encode("utf-8")
            ).hexdigest()
            if relation_digest not in generated_text:
                fail(f"generated relation tag omitted exact authority digest: {name}")

            permuted_registry = copy.deepcopy(registry)
            permuted_relation = next(
                row for row in permuted_registry["relations"] if row["name"] == name
            )
            permuted_relation["references"].reverse()
            permuted_relation["merge"]["conflict_columns"].reverse()
            permuted_registry_path = temporary / f"{name}.permuted.yaml"
            permuted_registry_path.write_text(
                yaml.safe_dump(permuted_registry, sort_keys=False), encoding="utf-8"
            )
            permuted_generated = temporary / f"{filename}.permuted"
            run(
                [
                    sys.executable,
                    str(root / "tools/sdk/relation_idl_compiler.py"),
                    "--registry",
                    str(permuted_registry_path),
                    "--relation",
                    name,
                    "--output",
                    str(permuted_generated),
                ],
                expect_success=True,
                label=f"relation IDL permutation generation {name}",
            )
            if permuted_generated.read_bytes() != generated.read_bytes():
                fail(f"relation IDL generation depends on set insertion order: {name}")

        source = temporary / "generated_test.cpp"
        source.write_text(
            '#include "cc_entity.hpp"\n'
            '#include "cc_call_site.hpp"\n'
            '#include "cc_call_direct_target.hpp"\n'
            '#include "company_lock_acquire.hpp"\n'
            "int main(){return cxxlens::cc::relations::call_site::descriptor().validate()?0:1;}\n",
            encoding="utf-8",
        )
        run(
            [
                compiler,
                "-std=c++23",
                "-fsyntax-only",
                f"-I{root / 'include'}",
                f"-I{temporary}",
                str(source),
            ],
            expect_success=True,
            label="generated relation tags syntax check",
        )

        core_sources = {
            path: path.read_bytes()
            for base in (root / "src", root / "include/cxxlens/sdk")
            for path in sorted(base.rglob("*"))
            if path.is_file()
        }
        external_registry = json.loads(json.dumps(registry))
        external = json.loads(
            json.dumps(
                next(
                    row
                    for row in external_registry["relations"]
                    if row["name"] == "company.lock.acquire"
                )
            )
        )
        replacements = {
            "company.lock.acquire": "company.audit.acquire",
            "company_lock_acquire_id": "company_audit_acquire_id",
            "company_lock_id": "company_audit_lock_id",
            "company.lock-mode/1": "company.audit-mode/1",
            "company.lock/1": "company.audit/1",
            "company.lock-extraction.compile-unit": "company.audit-extraction.compile-unit",
            "cxxlens::company::relations::lock_acquire": "cxxlens::company::relations::audit_acquire",
        }

        def replace_strings(value: Any) -> Any:
            if isinstance(value, str):
                for old, new in replacements.items():
                    value = value.replace(old, new)
                return value
            if isinstance(value, list):
                return [replace_strings(item) for item in value]
            if isinstance(value, dict):
                return {key: replace_strings(item) for key, item in value.items()}
            return value

        external = replace_strings(external)
        external_registry["relations"].append(external)
        external_path = temporary / "external_registry.yaml"
        external_path.write_text(
            yaml.safe_dump(external_registry, sort_keys=False), encoding="utf-8"
        )
        external_header = temporary / "company_audit_acquire.hpp"
        run(
            [
                sys.executable,
                str(root / "tools/sdk/relation_idl_compiler.py"),
                "--registry",
                str(external_path),
                "--relation",
                "company.audit.acquire",
                "--output",
                str(external_header),
            ],
            expect_success=True,
            label="external relation IDL generation",
        )
        if not external_header.is_file() or any(
            path.read_bytes() != before for path, before in core_sources.items()
        ):
            fail("external relation generation changed core SDK source")

        for relative in (
            "examples/sdk/negative/generated_unknown_column.cpp",
            "examples/sdk/negative/native_pointer_escape.cpp",
        ):
            run(
                [
                    compiler,
                    "-std=c++23",
                    "-fsyntax-only",
                    f"-I{root / 'include'}",
                    str(root / relative),
                ],
                expect_success=False,
                label=f"compile-fail example {relative}",
            )


def validate_scaffold(root: pathlib.Path, compiler: str, executable: str) -> None:
    manifest_schema = load_yaml(root / "schemas/cxxlens_ng_provider_manifest.schema.yaml")
    with tempfile.TemporaryDirectory(prefix="cxxlens-provider-scaffold-") as directory:
        temporary = pathlib.Path(directory)
        for provider_class, expected in (
            ("portable", "cxxlens::provider_sdk"),
            ("clang22-native", "cxxlens::clang22_provider_sdk"),
        ):
            output = temporary / provider_class
            run(
                [
                    executable,
                    str(output),
                    f"company.test.{provider_class.replace('-', '_')}",
                    provider_class,
                    "company.test.relation",
                ],
                expect_success=True,
                label=f"{provider_class} provider scaffold",
            )
            document = json.loads((output / "provider-manifest.json").read_text(encoding="utf-8"))
            try:
                jsonschema.Draft202012Validator(manifest_schema).validate(document)
            except jsonschema.ValidationError as error:
                fail(f"generated provider manifest schema validation failed: {error.message}")
            cmake = (output / "CMakeLists.txt").read_text(encoding="utf-8")
            if expected not in cmake:
                fail(f"{provider_class} scaffold links the wrong SDK target")
            run(
                [
                    compiler,
                    "-std=c++23",
                    "-fsyntax-only",
                    f"-I{root / 'include'}",
                    str(output / "src/main.cpp"),
                ],
                expect_success=True,
                label=f"{provider_class} scaffold source syntax check",
            )


def validate_cpp_query_ir(root: pathlib.Path, executable: str) -> None:
    completed = subprocess.run(
        [executable, "query-ir"], check=False, text=True, capture_output=True
    )
    if completed.returncode != 0:
        fail(f"C++ logical query IR export failed: {completed.stderr.strip()}")
    try:
        document = json.loads(completed.stdout)
        query_contract.validate_ir(
            document,
            load_yaml(root / "schemas/cxxlens_ng_logical_query_ir.schema.yaml"),
            load_yaml(root / "schemas/cxxlens_ng_logical_query_contract.yaml"),
            load_yaml(root / "schemas/cxxlens_ng_relation_registry.yaml"),
        )
    except (json.JSONDecodeError, query_contract.QueryContractError) as error:
        fail(f"C++ logical query IR differs from the accepted authority: {error}")


def validate_cpp_provider_manifest(root: pathlib.Path, executable: str) -> None:
    completed = subprocess.run(
        [executable, "provider-manifest"],
        check=False,
        text=True,
        capture_output=True,
    )
    if completed.returncode != 0:
        fail(f"C++ provider manifest export failed: {completed.stderr.strip()}")
    try:
        document = json.loads(completed.stdout)
        jsonschema.Draft202012Validator(
            load_yaml(root / "schemas/cxxlens_ng_provider_manifest.schema.yaml")
        ).validate(document)
    except (json.JSONDecodeError, jsonschema.ValidationError) as error:
        fail(f"C++ provider manifest differs from the accepted authority: {error}")


def validate_store_implementation(root: pathlib.Path) -> None:
    header = (root / "include/cxxlens/sdk/store.hpp").read_text(encoding="utf-8")
    source = (root / "src/sdk/store.cpp").read_text(encoding="utf-8")
    claim = (root / "src/sdk/claim.cpp").read_text(encoding="utf-8")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    for marker in (
        "snapshot_series_selector",
        "partition_manifest",
        "closure_certificate",
        "snapshot_writer",
        "open_sqlite_snapshot_store",
        "open_publication",
        "canonical_export",
    ):
        if marker not in header:
            fail(f"snapshot/store public marker is missing: {marker}")
    for marker in (
        'canonical_identity_digest("snapshot"',
        'canonical_identity_digest("snapshot-series"',
        'canonical_identity_digest("partition-content"',
        'canonical_identity_digest("closure-certificate"',
        '"store.publish-stale-parent"',
        '"store.current-corrupt"',
        '"store.hash-collision"',
        '"BEGIN IMMEDIATE;',
    ):
        if marker not in source:
            fail(f"snapshot/store implementation marker is missing: {marker}")
    if 'canonical_identity_digest("claim-content"' not in claim or (
        'typed_digest("content"' in claim
    ):
        fail("claim identity does not use the accepted claim-content canonical tuple")
    for path in ("src/sdk/store.cpp",):
        if path not in cmake:
            fail(f"snapshot/store build or install evidence is missing: {path}")
    schema_validate(
        load_yaml(root / "schemas/cxxlens_ng_sqlite_store_contract.yaml"),
        load_yaml(root / "schemas/cxxlens_ng_sqlite_store_contract.schema.yaml"),
    )
    test = (root / "tests/unit/sdk/store_test.cpp").read_text(encoding="utf-8")
    for marker in (
        "memory/SQLite snapshot IDs diverged",
        "staged claims became visible",
        "stale parent publish was accepted",
        "partial partition received a closure certificate",
        "compaction reclaimed a pinned generation",
        "SQLite reopen changed semantic identity",
        "corrupt current silently fell back",
    ):
        if marker not in test:
            fail(f"snapshot/store acceptance marker is missing: {marker}")


def validate_query_runtime_implementation(root: pathlib.Path) -> None:
    header = (root / "include/cxxlens/sdk/query.hpp").read_text(encoding="utf-8")
    query_source = (root / "src/sdk/query.cpp").read_text(encoding="utf-8")
    source = (root / "src/sdk/query_execution.cpp").read_text(encoding="utf-8")
    decoder = (root / "src/sdk/query_ir_decoder.cpp").read_text(encoding="utf-8")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    test_cmake = (root / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    for marker in (
        "decode_arguments",
        "execution_budget",
        "cancellation_probe",
        "annotated_row",
        "query_guarantee_fragment",
        "query_summary_guarantee",
        "query_result",
        "result_row_cursor",
        "reference_engine",
    ):
        if marker not in header:
            fail(f"query runtime public marker is missing: {marker}")
    for marker in (
        "cxxlens.reference-query-planner.v1",
        "query.inner_join.v1",
        "query.semi_join.v1",
        "query.distinct.v1",
        "sdk.query-output-budget",
        "sdk.query-cancelled",
        "inputs_complete",
        "explain_physical",
        "contributor_guarantees",
        "guarantees_are_canonical",
        "fragment_set_digest",
    ):
        if marker not in source:
            fail(f"query runtime implementation marker is missing: {marker}")
    if "expressions.size() == 1U" not in query_source:
        fail("query boolean unary canonical fold is missing")
    for marker in (
        "additive_optional_minor",
        "sdk.query-relation-requirement-incompatible",
        "expand_output_schema",
    ):
        if marker not in query_source:
            fail(f"query requirement reconciliation marker is missing: {marker}")
    for marker in (
        "duplicate-key",
        "absent_if_schema_missing",
        "sdk.query-argument-invalid",
        "query.order_by.v1",
    ):
        if marker not in decoder:
            fail(f"query IR decoder marker is missing: {marker}")
    for path in ("src/sdk/query_execution.cpp", "src/sdk/query_ir_decoder.cpp"):
        if path not in cmake:
            fail(f"query runtime build evidence is missing: {path}")
    if "query_execution" not in test_cmake:
        fail("query runtime example build evidence is missing: query_execution")
    schema_validate(
        load_yaml(root / "schemas/cxxlens_ng_query_runtime_contract.yaml"),
        load_yaml(root / "schemas/cxxlens_ng_query_runtime_contract.schema.yaml"),
    )
    jsonschema.Draft202012Validator.check_schema(
        load_yaml(root / "schemas/cxxlens_ng_query_execution_result.schema.yaml")
    )
    test = (root / "tests/unit/sdk/query_runtime_test.cpp").read_text(encoding="utf-8")
    for marker in (
        "memory/SQLite semantic rows diverged",
        "condition/interpretation-aware inner join diverged",
        "physical index leaked into logical IR",
        "cancellation did not return deterministic sealed partial result",
        "successful execution was confused with complete/closed input",
        "duplicate contributor guarantee was not rejected at the public row boundary",
        "cursor owned row and canonical guarantee cardinality diverged",
        "one-operand all/any did not canonical-fold to its atom",
        "factory-success unary predicate was rejected by semi_join",
        "compatible minor retained requirement depended on operand order",
        "compatible minor union reconciliation was not permutation invariant",
        "incompatible descriptor reconciliation was not deterministic",
    ):
        if marker not in test:
            fail(f"query runtime acceptance marker is missing: {marker}")


def validate(
    root: pathlib.Path, compiler: str, scaffold: str, doctor: str
) -> dict[str, Any]:
    catalog = load_yaml(root / CATALOG)
    validate_catalog(root, catalog)
    validate_boundaries(root)
    validate_generation_and_negatives(root, compiler)
    validate_scaffold(root, compiler, scaffold)
    validate_cpp_query_ir(root, doctor)
    validate_cpp_provider_manifest(root, doctor)
    validate_store_implementation(root)
    validate_query_runtime_implementation(root)
    return catalog


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--scaffold", required=True)
    parser.add_argument("--doctor", required=True)
    return parser.parse_args()


def main() -> int:
    args = arguments()
    catalog = validate(args.root.resolve(), args.compiler, args.scaffold, args.doctor)
    print(
        "NG author SDK contract check passed: "
        f"{len(catalog['author_paths'])} author paths, {len(catalog['entries'])} catalog entries"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (SdkContractError, OSError, subprocess.SubprocessError, yaml.YAMLError) as error:
        print(f"NG author SDK contract check failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
