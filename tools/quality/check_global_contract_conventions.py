#!/usr/bin/env python3
"""Generate and validate the Phase B global public-contract conventions."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import sys
from typing import Any

import jsonschema
import yaml


SCHEMA_ID = "cxxlens.global-contract-conventions.v1"
OWNERSHIP_SCHEMA_ID = "cxxlens.contract-ownership.v1"
LEGACY_OWNERSHIP_SCHEMA_EXCLUSIONS = {
    "cxxlens_asset_migration_ledger.schema.yaml",
    "cxxlens_asset_migration_policy.schema.yaml",
    "cxxlens_legacy_api_baseline.schema.yaml",
    "cxxlens_ng_authority_transition.schema.yaml",
    "cxxlens_ng_authority_transition_report.schema.yaml",
    "cxxlens_ng_catalog_bootstrap.schema.yaml",
    "cxxlens_ng_claim_envelope.schema.yaml",
    "cxxlens_ng_compatibility_report.schema.yaml",
    "cxxlens_ng_compatibility_request.schema.yaml",
    "cxxlens_ng_release_bundle.schema.yaml",
    "cxxlens_ng_relation_conformance_report.schema.yaml",
    "cxxlens_ng_relation_conformance_vectors.schema.yaml",
    "cxxlens_ng_relation_registry.schema.yaml",
    "cxxlens_ng_logical_query_contract.schema.yaml",
    "cxxlens_ng_logical_query_ir.schema.yaml",
    "cxxlens_ng_query_conformance_report.schema.yaml",
    "cxxlens_ng_query_conformance_vectors.schema.yaml",
}
EXPECTED_SECTIONS = {
    "naming_and_family",
    "exact_declaration_and_fingerprint",
    "value_reference_ownership_lifetime",
    "const_noexcept_thread_safety",
    "callback_and_reentrancy",
    "cancellation_deadline_budget",
    "result_failure_unresolved_partial",
    "evidence_coverage_confidence_guarantee",
    "ordering_duplicates_variants",
    "serialization_schema_versioning",
    "shared_ownership_and_dependencies",
    "positive_negative_ambiguous_acceptance",
}
EXPECTED_RESULT_TABLE = {
    "empty_success": ("value", "empty", False, False, True, True, {"exact", "conservative"}),
    "unresolved": (
        "value",
        "partial",
        False,
        True,
        False,
        True,
        {"best_effort", "conservative"},
    ),
    "unsupported": ("error", "none", True, False, False, False, {"none"}),
    "ambiguous": ("value", "partial", False, True, False, True, {"best_effort"}),
    "partial": (
        "value",
        "partial",
        False,
        True,
        False,
        True,
        {"best_effort", "conservative"},
    ),
    "failure": ("error", "none", True, False, False, False, {"none"}),
}
EXPECTED_PACKAGE_ISSUES = {
    "configuration": "#43",
    "core": "#43",
    "testing": "#43",
    "facts": "#44",
    "interop": "#44",
    "workspace": "#44",
    "explain": "#45",
    "search": "#45",
    "select": "#45",
    "graph": "#46",
    "report": "#47",
    "rules": "#47",
    "transform": "#48",
    "copy": "#49",
    "fuzz": "#49",
    "generate": "#49",
    "method_harness": "#49",
    "mock": "#49",
    "flow": "#50",
    "models": "#50",
    "qa": "#51",
    "review": "#51",
}


class ConventionError(ValueError):
    """A global convention invariant violation with a stable diagnostic."""


def fail(message: str) -> None:
    raise ConventionError(message)


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value).encode("utf-8")).hexdigest()


def schema_validate(value: Any, schema: dict[str, Any], context: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(value)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"{context} schema validation failed: {error.message}")


def package_issue_lookup(conventions: dict[str, Any]) -> dict[str, str]:
    owners: dict[str, str] = {}
    for row in conventions["package_candidate_owners"]:
        for package in row["packages"]:
            if package in owners:
                fail(f"duplicate package candidate owner: {package}")
            owners[package] = row["issue"]
    return owners


def validate_conventions(
    conventions: dict[str, Any],
    schema: dict[str, Any],
    catalog: dict[str, Any],
    root: pathlib.Path,
) -> None:
    if not isinstance(conventions, dict) or conventions.get("schema") != SCHEMA_ID:
        fail("unsupported global conventions schema")
    schema_validate(conventions, schema, "global conventions")

    authority = conventions["authority"]
    for path in authority.values():
        if not (root / path).is_file():
            fail(f"global convention authority is missing: {path}")

    owners = package_issue_lookup(conventions)
    catalog_packages = {package["id"] for package in catalog["packages"]}
    if owners != EXPECTED_PACKAGE_ISSUES or set(owners) != catalog_packages:
        fail("package candidate owners must cover all 22 catalog packages exactly once")
    catalog_states = {package["contract"]["state"] for package in catalog["packages"]}
    if catalog_states != {conventions["contract_state"]}:
        fail("global convention state differs from catalog package contract states")
    if conventions["transition_issue"] != (
        "#54" if conventions["contract_state"] == "frozen" else "#42"
    ):
        fail("global convention transition authority differs from contract state")

    transitions = {
        (row["from"], row["to"], row["authority"])
        for row in conventions["state_machine"]["transitions"]
    }
    expected_transitions = {
        ("draft", "unresolved", "package_candidate_owner"),
        ("draft", "candidate", "package_candidate_owner"),
        ("unresolved", "candidate", "package_candidate_owner"),
        ("candidate", "frozen", "#54"),
    }
    if transitions != expected_transitions:
        fail("contract state transitions differ from the Phase B authority model")
    if set(conventions["required_contract_sections"]) != EXPECTED_SECTIONS:
        fail("candidate contract section checklist is incomplete")

    table = {row["id"]: row for row in conventions["result_decisions"]}
    if set(table) != set(EXPECTED_RESULT_TABLE):
        fail("result decision table must cover six mutually exclusive outcomes")
    for outcome, expected in EXPECTED_RESULT_TABLE.items():
        row = table[outcome]
        actual = (
            row["result_channel"],
            row["payload"],
            row["error_required"],
            row["unresolved_required"],
            row["coverage_complete"],
            row["authoritative_rows_preserved"],
            set(row["allowed_guarantees"]),
        )
        if actual != expected:
            fail(f"invalid result decision combination: {outcome}")
        if row["result_channel"] == "error" and row["allowed_guarantees"] != ["none"]:
            fail(f"error result cannot claim a material result guarantee: {outcome}")
        if row["coverage_complete"] and row["unresolved_required"]:
            fail(f"complete coverage cannot require unresolved rows: {outcome}")

    coverage = conventions["evidence_coverage_and_guarantee"]
    if set(coverage["coverage_states"]) != {
        "requested",
        "excluded",
        "covered",
        "failed",
        "unresolved",
        "not_applicable",
    }:
        fail("coverage conservation states are incomplete")
    if set(conventions["serialization_and_versioning"]["version_axes"]) != {
        "cpp_api",
        "public_schema",
        "fact_schema",
        "semantics",
        "adapter",
    }:
        fail("public version axes must remain independent")
    change_rows = {row["id"]: row for row in conventions["change_policy"]["classes"]}
    if set(change_rows) != {"additive", "source_breaking", "semantic_breaking", "schema_breaking"}:
        fail("public contract change classification is incomplete")
    if any(not row["fingerprint_invalidated"] for row in change_rows.values()):
        fail("every public contract change must invalidate its fingerprint")


def generate_ownership(
    catalog: dict[str, Any], conventions: dict[str, Any], root: pathlib.Path
) -> dict[str, Any]:
    shared_by_package = {
        row["package"]: row
        for row in catalog["registries"]["shared_implementation_contracts"]
    }
    public_types: list[dict[str, Any]] = []
    components: list[dict[str, Any]] = []
    for package in sorted(catalog["packages"], key=lambda row: row["id"]):
        package_id = package["id"]
        shared = shared_by_package[package_id]
        source = f"schemas/cxxlens_public_api_contract.yaml#package-{package_id}"
        for type_name in sorted(package["public_types"]):
            public_types.append(
                {
                    "id": f"public-type:{package_id}:{type_name}",
                    "type": type_name,
                    "package": package_id,
                    "header": package["header"],
                    "owner_atomic_unit": shared["owner_atomic_unit"],
                    "steward": shared["steward"],
                    "source_contract": source,
                }
            )
        for kind in (
            "package_internal_engine",
            "schema_fixture_contract",
            "shared_public_type_contract",
        ):
            components.append(
                {
                    "id": f"{shared['id']}.{kind}",
                    "kind": kind,
                    "package": package_id,
                    "owner_atomic_unit": shared["owner_atomic_unit"],
                    "steward": shared["steward"],
                    "semantics_version": shared["semantics_version"],
                    "source_contract": shared["source_contract"],
                }
            )

    providers = []
    for contract in sorted(
        catalog["registries"]["provider_implementation_contracts"],
        key=lambda row: row["id"],
    ):
        for subject in sorted(contract["subjects"]):
            providers.append(
                {
                    "id": f"{contract['id']}:{subject}",
                    "contract_id": contract["id"],
                    "kind": contract["kind"],
                    "subject": subject,
                    "owner_atomic_unit": contract["owner_atomic_unit"],
                    "steward": contract["steward"],
                    "semantics_version": contract["semantics_version"],
                    "source_contract": contract["source_contract"],
                }
            )

    schema_rows = [
        {
            "id": f"schema:{path.relative_to(root).as_posix()}",
            "path": path.relative_to(root).as_posix(),
            "owner_atomic_unit": "AU-CORE-001",
            "steward": "steward.schema",
            "source_contract": "docs/design/public_contract_conventions.md#schema-ownership",
        }
        for path in sorted((root / "schemas").glob("*.schema.yaml"))
        if path.name not in LEGACY_OWNERSHIP_SCHEMA_EXCLUSIONS
    ]
    manifest: dict[str, Any] = {
        "schema": OWNERSHIP_SCHEMA_ID,
        "source_catalog": "schemas/cxxlens_public_api_contract.yaml",
        "conventions": "schemas/cxxlens_global_contract_conventions.yaml",
        "public_types": public_types,
        "shared_components": sorted(components, key=lambda row: row["id"]),
        "providers": sorted(providers, key=lambda row: row["id"]),
        "schemas": schema_rows,
        "summary": {
            "public_type_count": len(public_types),
            "shared_component_count": len(components),
            "provider_subject_count": len(providers),
            "schema_count": len(schema_rows),
        },
    }
    manifest["semantic_digest"] = digest(manifest)
    return manifest


def validate_ownership(
    manifest: dict[str, Any],
    schema: dict[str, Any],
    expected: dict[str, Any],
    catalog: dict[str, Any],
    root: pathlib.Path,
) -> None:
    if not isinstance(manifest, dict) or manifest.get("schema") != OWNERSHIP_SCHEMA_ID:
        fail("unsupported contract ownership schema")
    schema_validate(manifest, schema, "contract ownership")
    unsigned = copy.deepcopy(manifest)
    semantic_digest = unsigned.pop("semantic_digest", None)
    if semantic_digest != digest(unsigned):
        fail("contract ownership semantic digest mismatch")

    atomic_units = {
        api["atomic_unit"]["id"]
        for package in catalog["packages"]
        for api in package["apis"]
    }
    for collection in ("public_types", "shared_components", "providers", "schemas"):
        rows = manifest[collection]
        ids = [row["id"] for row in rows]
        if len(ids) != len(set(ids)):
            fail(f"duplicate owner in {collection}")
        for row in rows:
            if row["owner_atomic_unit"] not in atomic_units:
                fail(f"dangling owner atomic unit: {row['id']}")
            source = row["source_contract"].split("#", 1)[0]
            if not (root / source).is_file():
                fail(f"dangling owner source contract: {row['id']}")
    subjects = [(row["kind"], row["subject"]) for row in manifest["providers"]]
    if len(subjects) != len(set(subjects)):
        fail("provider subject has more than one owner")
    type_names = [row["type"] for row in manifest["public_types"]]
    if len(type_names) != len(set(type_names)):
        fail("shared public type has more than one owner")
    paths = [row["path"] for row in manifest["schemas"]]
    if len(paths) != len(set(paths)):
        fail("schema has more than one owner")
    if manifest != expected:
        fail("contract ownership registry is stale or incomplete")


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail(f"expected mapping document: {path}")
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("generate", "check"))
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path.cwd())
    args = parser.parse_args()
    root = args.root.resolve()
    conventions = load_yaml(root / "schemas/cxxlens_global_contract_conventions.yaml")
    conventions_schema = load_yaml(root / "schemas/cxxlens_global_contract_conventions.schema.yaml")
    ownership_schema = load_yaml(root / "schemas/cxxlens_contract_ownership.schema.yaml")
    catalog = load_yaml(root / "schemas/cxxlens_public_api_contract.yaml")
    validate_conventions(conventions, conventions_schema, catalog, root)
    expected = generate_ownership(catalog, conventions, root)
    output = root / "schemas/cxxlens_contract_ownership.yaml"
    if args.mode == "generate":
        output.write_text(
            yaml.safe_dump(expected, allow_unicode=True, sort_keys=False, width=120),
            encoding="utf-8",
        )
    else:
        manifest = load_yaml(output)
        validate_ownership(manifest, ownership_schema, expected, catalog, root)
    print(
        "validated global contract conventions: "
        f"{len(catalog['packages'])} packages, "
        f"{expected['summary']['public_type_count']} public types, "
        f"{expected['summary']['shared_component_count']} components, "
        f"{expected['summary']['provider_subject_count']} provider subjects, and "
        f"{expected['summary']['schema_count']} schemas"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ConventionError, OSError, yaml.YAMLError) as error:
        print(f"global contract convention validation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
