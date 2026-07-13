#!/usr/bin/env python3
"""Validate package-level Phase B Contract Candidate records."""

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


SCHEMA_ID = "cxxlens.package-contract-candidates.v1"
MANIFEST_PATH = "schemas/cxxlens_package_contract_candidates.yaml"
EXPECTED_OUTCOMES = {
    "empty_success",
    "unresolved",
    "unsupported",
    "ambiguous",
    "partial",
    "failure",
}


class CandidateError(ValueError):
    """A package candidate invariant violation with a stable diagnostic."""


def fail(message: str) -> None:
    raise CandidateError(message)


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value).encode("utf-8")).hexdigest()


def signature_fingerprint(signature: str) -> str:
    return "sha256:" + hashlib.sha256(signature.encode("utf-8")).hexdigest()


def schema_validate(value: Any, schema: dict[str, Any]) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(value)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"package candidate schema validation failed: {error.message}")


def package_owners(conventions: dict[str, Any]) -> dict[str, str]:
    result: dict[str, str] = {}
    for row in conventions["package_candidate_owners"]:
        for package in row["packages"]:
            if package in result:
                fail(f"duplicate global package owner: {package}")
            result[package] = row["issue"]
    return result


def validate_candidates(
    manifest: dict[str, Any],
    schema: dict[str, Any],
    catalog: dict[str, Any],
    conventions: dict[str, Any],
    ownership: dict[str, Any],
    root: pathlib.Path,
) -> None:
    if not isinstance(manifest, dict) or manifest.get("schema") != SCHEMA_ID:
        fail("unsupported package candidate schema")
    schema_validate(manifest, schema)
    if catalog.get("authority", {}).get("package_contract_candidates") != MANIFEST_PATH:
        fail("catalog does not name the package candidate authority")

    packages = {row["id"]: row for row in catalog["packages"]}
    apis = {
        api["id"]: (package, api)
        for package in catalog["packages"]
        for api in package["apis"]
    }
    atomic_units = {api["atomic_unit"]["id"] for _, api in apis.values()}
    owners = package_owners(conventions)
    requirement_ids = set(catalog["registries"]["requirements"])
    use_case_ids = set(catalog["registries"]["use_cases"])
    trace_ids = requirement_ids | use_case_ids
    public_type_ids = {row["id"] for row in ownership["public_types"]}
    component_ids = {row["id"] for row in ownership["shared_components"]}
    provider_ids = {row["id"] for row in ownership["providers"]}
    schema_ids = {row["id"] for row in ownership["schemas"]}

    seen_issues: set[str] = set()
    seen_packages: set[str] = set()
    seen_apis: set[str] = set()
    acceptance_ids: set[str] = set()
    registry_ids: set[str] = set()
    registry_entries: set[tuple[str, str]] = set()

    for group in manifest["groups"]:
        issue = group["issue"]
        if issue in seen_issues:
            fail(f"duplicate candidate issue: {issue}")
        seen_issues.add(issue)
        group_packages = group["packages"]
        if group_packages != sorted(group_packages):
            fail(f"{issue}: packages must use canonical order")
        if set(group_packages) != {package for package, owner in owners.items() if owner == issue}:
            fail(f"{issue}: package ownership differs from global conventions")
        overlap = seen_packages & set(group_packages)
        if overlap:
            fail(f"package appears in multiple candidate groups: {sorted(overlap)}")
        seen_packages.update(group_packages)

        unsigned = copy.deepcopy(group)
        actual_fingerprint = unsigned.pop("candidate_fingerprint")
        if actual_fingerprint != digest(unsigned):
            fail(f"{issue}: candidate fingerprint mismatch")
        expected_pointer = f"{MANIFEST_PATH}#issue-{issue.removeprefix('#')}"
        for package_id in group_packages:
            package = packages.get(package_id)
            if package is None:
                fail(f"{issue}: unknown package {package_id}")
            contract = package["contract"]
            if contract["owner_issue"] != issue:
                fail(f"{package_id}: catalog candidate owner differs")
            expected_transition = (
                issue if contract["state"] == "candidate" else "#54"
            )
            if (
                contract["state"] not in {"candidate", "frozen"}
                or contract["transition_issue"] != expected_transition
            ):
                fail(f"{package_id}: catalog candidate/frozen state or transition differs")
            if contract.get("candidate_manifest") != expected_pointer:
                fail(f"{package_id}: candidate manifest pointer differs")
            if contract.get("candidate_fingerprint") != actual_fingerprint:
                fail(f"{package_id}: candidate fingerprint differs from manifest")

        for path in [group["normative_document"], *group["candidate_headers"], *group["usage_examples"]]:
            candidate_path = root / path
            if not candidate_path.is_file():
                fail(f"{issue}: candidate evidence path is missing: {path}")
        if any(path.startswith("include/") for path in group["candidate_headers"]):
            fail(f"{issue}: package candidate cannot pre-empt #53 public-header integration")

        policies = {row["id"]: row for row in group["policies"]}
        if len(policies) != len(group["policies"]):
            fail(f"{issue}: duplicate policy ID")
        for policy in policies.values():
            if set(policy["result_semantics"]) != EXPECTED_OUTCOMES:
                fail(f"{policy['id']}: result semantics are incomplete")

        expected_api_ids = {
            api["id"]
            for package_id in group_packages
            for api in packages[package_id]["apis"]
        }
        actual_api_ids = [row["api_id"] for row in group["api_contracts"]]
        if len(actual_api_ids) != len(set(actual_api_ids)):
            fail(f"{issue}: duplicate API contract")
        if set(actual_api_ids) != expected_api_ids:
            fail(f"{issue}: API coverage differs from assigned packages")
        used_policies: set[str] = set()
        for contract in group["api_contracts"]:
            api_id = contract["api_id"]
            if api_id in seen_apis:
                fail(f"API appears in multiple candidate groups: {api_id}")
            seen_apis.add(api_id)
            package, api = apis[api_id]
            if contract["package"] != package["id"] or package["id"] not in group_packages:
                fail(f"{api_id}: package differs from catalog")
            if contract["atomic_unit"] != api["atomic_unit"]["id"]:
                fail(f"{api_id}: atomic unit differs from catalog")
            policy_id = contract["policy"]
            if policy_id not in policies:
                fail(f"{api_id}: policy reference is dangling")
            used_policies.add(policy_id)
            declaration = contract["declaration"]
            if api["declaration"]["status"] != "exact" or any(
                declaration[field] != api["declaration"][field]
                for field in ("source", "signature", "signature_fingerprint")
            ):
                fail(f"{api_id}: exact declaration differs from catalog")
            if declaration["signature_fingerprint"] != signature_fingerprint(
                declaration["signature"]
            ):
                fail(f"{api_id}: signature fingerprint mismatch")
            if not (root / declaration["source"]).is_file():
                fail(f"{api_id}: declaration source is missing")
            declaration_count = declaration["signature"].count(";") + 1
            if len(contract["family_members"]) != declaration_count:
                fail(f"{api_id}: atomic family member count differs from declaration")
            refs = contract["ownership_refs"]
            for kind, values, known in (
                ("public type", refs["public_types"], public_type_ids),
                ("shared component", refs["shared_components"], component_ids),
                ("provider", refs["providers"], provider_ids),
                ("schema", refs["schemas"], schema_ids),
            ):
                dangling = set(values) - known
                if dangling:
                    fail(f"{api_id}: dangling {kind} owner references: {sorted(dangling)}")
            dangling_dependencies = set(contract["dependency_apis"]) - apis.keys()
            if dangling_dependencies:
                fail(f"{api_id}: dangling API dependencies: {sorted(dangling_dependencies)}")
            if contract["dependency_apis"] != api.get("requires", {}).get("apis", []):
                fail(f"{api_id}: dependency API metadata differs from catalog")
            dangling_trace = (
                set(contract["traceability"]["requirements_or_use_cases"]) - trace_ids
            )
            if dangling_trace:
                fail(f"{api_id}: dangling traceability: {sorted(dangling_trace)}")
            if api.get("contract_traceability") != contract["traceability"][
                "requirements_or_use_cases"
            ]:
                fail(f"{api_id}: catalog contract traceability differs")
            for category in ("positive", "negative", "ambiguous"):
                case = contract["acceptance"][category]
                if not case["id"].startswith(f"{api_id}.{category}"):
                    fail(f"{api_id}: {category} case ID is not API-bound")
                if case["id"] in acceptance_ids:
                    fail(f"duplicate acceptance case ID: {case['id']}")
                acceptance_ids.add(case["id"])
        if used_policies != set(policies):
            fail(f"{issue}: unused or missing policy coverage")

        for registry in group["registry_owners"]:
            registry_id = registry["id"]
            if registry_id in registry_ids:
                fail(f"duplicate registry owner: {registry_id}")
            registry_ids.add(registry_id)
            if registry["owner_atomic_unit"] not in atomic_units:
                fail(f"{registry_id}: dangling registry owner atomic unit")
            for entry in registry["entries"]:
                key = (registry["kind"], entry)
                if key in registry_entries:
                    fail(f"registry entry has multiple owners: {entry}")
                registry_entries.add(key)

        for boundary in group["consumer_boundaries"]:
            if boundary["owner_package"] not in packages or boundary["consumer_package"] not in packages:
                fail(f"{issue}: consumer boundary has an unknown package")
            if not ({boundary["owner_package"], boundary["consumer_package"]} & set(group_packages)):
                fail(f"{issue}: consumer boundary is unrelated to assigned packages")

    candidate_or_frozen_packages = {
        package["id"]
        for package in catalog["packages"]
        if package["contract"]["state"] in {"candidate", "frozen"}
    }
    if seen_packages != candidate_or_frozen_packages:
        fail(
            "candidate manifest must cover every catalog candidate/frozen package "
            "exactly once"
        )


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail(f"expected mapping document: {path}")
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path.cwd())
    args = parser.parse_args()
    root = args.root.resolve()
    manifest = load_yaml(root / MANIFEST_PATH)
    validate_candidates(
        manifest,
        load_yaml(root / "schemas/cxxlens_package_contract_candidates.schema.yaml"),
        load_yaml(root / "schemas/cxxlens_public_api_contract.yaml"),
        load_yaml(root / "schemas/cxxlens_global_contract_conventions.yaml"),
        load_yaml(root / "schemas/cxxlens_contract_ownership.yaml"),
        root,
    )
    groups = manifest["groups"]
    print(
        "validated package contract candidates: "
        f"{len(groups)} groups, "
        f"{sum(len(group['packages']) for group in groups)} packages, and "
        f"{sum(len(group['api_contracts']) for group in groups)} APIs"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CandidateError, OSError, yaml.YAMLError) as error:
        print(f"package contract candidate validation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
