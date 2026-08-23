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


def admitted_generated_relations(
    catalog: dict[str, Any], registry: dict[str, Any]
) -> list[tuple[dict[str, Any], pathlib.Path]]:
    """Return every catalog-admitted generated relation and its committed header."""
    admitted_headers = {
        pathlib.Path(header)
        for collection in (catalog["packages"], catalog["entries"])
        for row in collection
        for header in row["headers"]
        if header.startswith("include/cxxlens/relations/")
    }
    registry_by_header: dict[pathlib.Path, dict[str, Any]] = {}
    for relation in registry["relations"]:
        tag = relation.get("generated_cpp_tag")
        projection = relation.get("cpp_projection")
        if projection == "dynamic-only" and tag is None:
            continue
        if projection != "installed-static" or not isinstance(tag, str):
            fail(
                "relation generated C++ tag/projection classification differs: "
                f"{relation['name']}"
            )
        header = pathlib.Path(
            "include/cxxlens/relations/"
            + str(relation["name"]).replace(".", "_")
            + ".hpp"
        )
        if header in registry_by_header:
            fail(
                "generated relation header has duplicate registry ownership: "
                f"{header}"
            )
        registry_by_header[header] = relation

    unbound = sorted(
        header.as_posix() for header in admitted_headers - set(registry_by_header)
    )
    if unbound:
        fail(f"catalog relation headers lack registry binding: {unbound}")
    unadmitted = sorted(
        header.as_posix() for header in set(registry_by_header) - admitted_headers
    )
    if unadmitted:
        fail(f"installed-static registry headers lack catalog admission: {unadmitted}")
    return [
        (registry_by_header[header], header)
        for header in sorted(admitted_headers, key=lambda path: path.as_posix())
    ]


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
    if contract.get("value_types", {}).get("compile_unit_entry") != {
        "fields": [
            "compile_unit_id",
            "effective_invocation_digest",
            "source_digest",
            "environment_digest",
        ],
        "identity": "stable-control-free-catalog-local-input-id",
        "digests": "exact-canonical-digest",
    }:
        fail("project catalog compile-unit input identity is not exact")
    if contract.get("identity_boundary") != {
        "catalog_compile_unit_id": "project-upstream-catalog-input-census-identity",
        "build_compile_unit_relation_id": (
            "independently-derived-from-accepted-relation-registry"
        ),
        "implicit_equality_alias": "forbidden",
        "consumer_mapping": "exact-entry-digests-to-final-relation-payload-and-id",
    }:
        fail("project catalog identity boundary is not exact")
    if contract.get("consumers", {}).get("build_compile_unit") != (
        "explicit-catalog-entry-to-final-relation-id-mapping-required"
    ):
        fail("project catalog build.compile_unit mapping is not explicit")
    public_entry = entries.get("public.project-catalog", {})
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

def validate_provider_task_contract(
    root: pathlib.Path, entries: dict[str, dict[str, Any]]
) -> None:
    contract = load_yaml(root / PROVIDER_TASK_CONTRACT)
    schema_validate(contract, load_yaml(root / PROVIDER_TASK_SCHEMA))
    public_entry = entries.get("public.provider-sdk", {})
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

    protocol = load_yaml(root / "schemas/cxxlens_ng_provider_protocol_v2.yaml")
    if protocol.get("document_version") != "2.0.0":
        fail("provider protocol v2 contract version is not current")
    request_task = protocol.get("request_task", {})
    if request_task.get("request_schema") != "cxxlens.clang22-materialization-request.v2_2":
        fail("provider protocol does not bind request v2.2")
    if request_task.get("task_schema") != "cxxlens.clang22.task.v4":
        fail("provider protocol does not bind task v4")
    if request_task.get("source_bytes_in_request") != "forbidden":
        fail("provider protocol permits source bytes in a task request")
    source_closure = protocol.get("source_closure_transport", {})
    success_path = source_closure.get("success_path", [])
    if "task-v4-sealed" not in success_path or "task-accepted" not in success_path:
        fail("provider protocol source-closure path is incomplete")


def validate_static_row_view_contract(
    root: pathlib.Path, entries: dict[str, dict[str, Any]]
) -> None:
    public_entry = entries.get("public.relation-static", {})
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
def validate_catalog(root: pathlib.Path, catalog: dict[str, Any]) -> None:
    schema_validate(catalog, load_yaml(root / SCHEMA))
    paths = unique_rows(catalog["author_paths"], "author path")
    if not paths:
        fail("catalog has no author acceptance path")
    entries = unique_rows(catalog["entries"], "public entry")
    implemented = {
        identifier
        for identifier, entry in entries.items()
        if entry["status"] == "implemented"
    }
    if not implemented:
        fail("catalog has no implemented SDK entry")
    validate_project_catalog_contract(root, entries)
    validate_provider_task_contract(root, entries)
    validate_static_row_view_contract(root, entries)
    validate_claim_evidence_occurrence_contract(root, entries)
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
            for field in ("positive_example", "negative_example")
        )
    for entry in entries.values():
        referenced_paths.update(entry["headers"])
    missing = sorted(path for path in referenced_paths if not (root / path).is_file())
    if missing:
        fail(f"SDK catalog acceptance path is missing: {missing}")

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
        catalog = load_yaml(root / CATALOG)
        registry = load_yaml(root / "schemas/cxxlens_ng_relation_registry.yaml")
        generated_relations = admitted_generated_relations(catalog, registry)
        generated_filenames: list[str] = []
        for relation, relative_header in generated_relations:
            name = str(relation["name"])
            filename = relative_header.name
            generated_filenames.append(filename)
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
            # Compile the generated artifact and check its semantic tags below.
            if not permuted_generated.is_file():
                fail(f"relation IDL generation produced no artifact: {name}")

        source = temporary / "generated_test.cpp"
        source.write_text(
            "".join(f'#include "{filename}"\n' for filename in generated_filenames)
            + "int main(){return "
            "cxxlens::cc::relations::call_site::descriptor().validate()?0:1;}\n",
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
        for provider_class in ("portable", "clang22-native"):
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
    """Validate the Store schemas; implementation behavior belongs to CTest."""
    schema_validate(
        load_yaml(root / "schemas/cxxlens_ng_sqlite_store_contract.yaml"),
        load_yaml(root / "schemas/cxxlens_ng_sqlite_store_contract.schema.yaml"),
    )
def validate_query_runtime_implementation(root: pathlib.Path) -> None:
    """Validate query runtime schemas; implementation behavior belongs to CTest."""
    schema_validate(
        load_yaml(root / "schemas/cxxlens_ng_query_runtime_contract.yaml"),
        load_yaml(root / "schemas/cxxlens_ng_query_runtime_contract.schema.yaml"),
    )
    jsonschema.Draft202012Validator.check_schema(
        load_yaml(root / "schemas/cxxlens_ng_query_execution_result.schema.yaml")
    )


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
    print("NG author SDK contract check passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (SdkContractError, OSError, subprocess.SubprocessError, yaml.YAMLError) as error:
        print(f"NG author SDK contract check failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
