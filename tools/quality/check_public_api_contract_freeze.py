#!/usr/bin/env python3
"""Validate the superseded public API Contract Freeze as migration provenance."""

from __future__ import annotations

import argparse
import collections
import copy
import hashlib
import json
import pathlib
import subprocess
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_global_contract_conventions import (  # noqa: E402
    generate_ownership,
    validate_conventions,
    validate_ownership,
)
from check_package_contract_candidates import validate_candidates  # noqa: E402
from check_phase_b_contract_integration import validate_metadata  # noqa: E402
from validate_api_contract import validate_document  # noqa: E402


SCHEMA_ID = "cxxlens.public-api-contract-freeze.v1"
MANIFEST = "schemas/cxxlens_public_api_contract_freeze.yaml"
EXPECTED_GROUPS = {
    "#43": ({"configuration", "core", "testing"}, 17),
    "#44": ({"facts", "interop", "workspace"}, 22),
    "#45": ({"explain", "search", "select"}, 25),
    "#46": ({"graph"}, 6),
    "#47": ({"report", "rules"}, 9),
    "#48": ({"transform"}, 9),
    "#49": ({"copy", "fuzz", "generate", "method_harness", "mock"}, 15),
    "#50": ({"flow", "models"}, 12),
    "#51": ({"qa", "review"}, 9),
}
PREREQUISITE_ISSUES = [
    *[f"#{issue}" for issue in range(31, 41)],
    *[f"#{issue}" for issue in range(42, 54)],
]


class FreezeError(ValueError):
    """A stable Contract Freeze invariant violation."""


def fail(message: str) -> None:
    raise FreezeError(message)


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail(f"expected mapping: {path}")
    return value


def load_json(path: pathlib.Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail(f"expected mapping: {path}")
    return value


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def value_digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value).encode("utf-8")).hexdigest()


def file_digest(path: pathlib.Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def relative(root: pathlib.Path, path: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(root).as_posix()
    except ValueError:
        fail(f"evidence path must remain under repository root: {path}")


def artifact(root: pathlib.Path, path: str | pathlib.Path) -> dict[str, str]:
    candidate = root / path if isinstance(path, str) else path
    if not candidate.is_file():
        fail(f"freeze input is missing: {candidate}")
    return {"path": relative(root, candidate), "sha256": file_digest(candidate)}


def input_path_groups(root: pathlib.Path) -> dict[str, set[str]]:
    integration = load_yaml(root / "schemas/cxxlens_phase_b_contract_integration.yaml")
    candidates = load_yaml(root / "schemas/cxxlens_package_contract_candidates.yaml")
    headers = {
        integration["public_headers"]["explicit_interop"],
        integration["public_headers"]["umbrella"],
        *integration["public_headers"]["ordinary"],
    }
    docs_examples = {
        "docs/design/cxxlens_integrated_design_ja.md",
        "docs/design/public_contract_conventions.md",
        "docs/design/contract_candidate_checklist.md",
        *(group["normative_document"] for group in candidates["groups"]),
        *(
            path
            for group in candidates["groups"]
            for path in group["usage_examples"]
        ),
        *(row["source"] for row in integration["package_examples"]),
    }
    groups: dict[str, set[str]] = {
        "catalog": {
            "schemas/cxxlens_public_api_contract.yaml",
            "schemas/cxxlens_api_catalog.schema.yaml",
            "docs/design/api_catalog_inventory.md",
            "docs/design/api_catalog_change_policy.md",
        },
        "public_headers": headers,
        "docs_examples": docs_examples,
        "task_packets": {
            "schemas/cxxlens.agent-task-packet-corpus.v1.json",
            "schemas/cxxlens.agent-task-packet-validation-report.v1.json",
            "schemas/cxxlens.agent-task-packet-validation-report.v1.schema.yaml",
            "schemas/cxxlens.agent-task-packet.v1.schema.yaml",
        },
        "ownership": {
            "schemas/cxxlens_contract_ownership.yaml",
            "schemas/cxxlens_contract_ownership.schema.yaml",
            "schemas/cxxlens.agent-ownership.v1.json",
            "schemas/cxxlens.agent-ownership.v1.schema.yaml",
            "schemas/cxxlens.dependency-request.examples.v1.json",
            "schemas/cxxlens.dependency-request.v1.schema.yaml",
        },
        "ready_report": {
            "schemas/cxxlens.api-ready.report.v1.json",
            "schemas/cxxlens.api-ready.v1.schema.yaml",
            "schemas/cxxlens.api-ready.run-manifest.v1.schema.yaml",
        },
        "readiness_authorization": {
            "schemas/cxxlens.readiness.authorization.v1.json",
            "schemas/cxxlens.readiness.authorization.v1.schema.yaml",
            "schemas/cxxlens.readiness.gate-evidence.v1.schema.yaml",
        },
        "phase_a_evidence": {
            *(f"schemas/cxxlens_m{milestone}_completion.yaml" for milestone in range(3)),
            *(f"schemas/cxxlens_m{milestone}_completion.schema.yaml" for milestone in range(3)),
            *(f"schemas/cxxlens_m{milestone}_acceptance_report.schema.yaml" for milestone in range(3)),
            "docs/design/VALIDATION_REPORT.md",
        },
        "high_risk_validation": {
            "schemas/cxxlens_high_risk_contract_validation.yaml",
            "schemas/cxxlens_high_risk_contract_validation.schema.yaml",
            "tests/contract_spikes/high_risk_contract_spike.py",
            "tests/contract_spikes/high_risk_validation_evidence.json",
            "tools/quality/check_high_risk_contract_validation.py",
        },
        "integration_acceptance": {
            "schemas/cxxlens_phase_b_contract_integration.yaml",
            "schemas/cxxlens_phase_b_contract_integration.schema.yaml",
            "schemas/cxxlens_public_api_contract_freeze.schema.yaml",
            "docs/design/phase_b_contract_integration.md",
            "docs/design/public_api_contract_freeze.md",
            "tests/install/run_install_test.cmake.in",
            "tools/quality/check_phase_b_contract_integration.py",
            "tools/quality/check_public_api_contract_freeze.py",
        },
    }
    claimed = set().union(*groups.values())
    groups["schemas_and_registries"] = {
        relative(root, path)
        for path in (root / "schemas").iterdir()
        if path.is_file()
        and relative(root, path) not in claimed
        and relative(root, path) != MANIFEST
    }
    all_paths = [path for paths in groups.values() for path in paths]
    if len(all_paths) != len(set(all_paths)):
        fail("freeze input categories overlap")
    return groups


def make_inputs(root: pathlib.Path) -> dict[str, Any]:
    categories = []
    for category, paths in sorted(input_path_groups(root).items()):
        files = [artifact(root, path) for path in sorted(paths)]
        categories.append({"id": category, "digest": value_digest(files), "files": files})
    return {
        "fingerprint": value_digest(
            [{"id": row["id"], "digest": row["digest"]} for row in categories]
        ),
        "categories": categories,
    }


def make_manifest(root: pathlib.Path) -> dict[str, Any]:
    catalog = load_yaml(root / "schemas/cxxlens_public_api_contract.yaml")
    candidates = load_yaml(root / "schemas/cxxlens_package_contract_candidates.yaml")
    ownership = load_yaml(root / "schemas/cxxlens_contract_ownership.yaml")
    high_risk = load_yaml(root / "schemas/cxxlens_high_risk_contract_validation.yaml")
    integration = load_yaml(root / "schemas/cxxlens_phase_b_contract_integration.yaml")
    corpus = load_json(root / "schemas/cxxlens.agent-task-packet-corpus.v1.json")
    agent_ownership = load_json(root / "schemas/cxxlens.agent-ownership.v1.json")
    ready = load_json(root / "schemas/cxxlens.api-ready.report.v1.json")
    authorization = load_json(root / "schemas/cxxlens.readiness.authorization.v1.json")
    inputs = make_inputs(root)
    packages = catalog["packages"]
    apis = [(package, api) for package in packages for api in package["apis"]]
    groups = {group["issue"]: group for group in candidates["groups"]}
    implementation_states = collections.Counter(api["implementation_state"] for _, api in apis)
    contract_states = collections.Counter(package["contract"]["state"] for package in packages)
    readiness_states = collections.Counter(api["readiness"]["state"] for _, api in apis)
    api_owners = [
        {
            "api_id": api["id"],
            "atomic_unit": api["atomic_unit"]["id"],
            "package": package["id"],
            "owner_issue": package["contract"]["owner_issue"],
            "declaration_source": api["declaration"]["source"],
            "signature_fingerprint": api["declaration"]["signature_fingerprint"],
            "candidate_fingerprint": package["contract"]["candidate_fingerprint"],
            "contract_traceability": api["contract_traceability"],
            "implementation_state": api["implementation_state"],
            "readiness_state": api["readiness"]["state"],
        }
        for package, api in sorted(apis, key=lambda row: row[1]["id"])
    ]
    package_owners = [
        {
            "package": package["id"],
            "issue": package["contract"]["owner_issue"],
            "api_count": len(package["apis"]),
            "state": package["contract"]["state"],
            "candidate_fingerprint": package["contract"]["candidate_fingerprint"],
        }
        for package in sorted(packages, key=lambda row: row["id"])
    ]
    package_validations = [
        {
            "issue": issue,
            "result": "validated",
            "packages": len(group["packages"]),
            "apis": len(group["api_contracts"]),
            "candidate_fingerprint": group["candidate_fingerprint"],
        }
        for issue, group in sorted(groups.items())
    ]
    manifest: dict[str, Any] = {
        "schema": SCHEMA_ID,
        "document_version": "1.0.0",
        "state": "frozen",
        "phase_c_authorized": True,
        "repository": {
            "binding": "git-head-at-execution",
            "input_fingerprint": inputs["fingerprint"],
        },
        "inputs": inputs,
        "summary": {
            "packages": len(packages),
            "apis": len(apis),
            "atomic_units": len({api["atomic_unit"]["id"] for _, api in apis}),
            "exact_declarations": sum(api["declaration"]["status"] == "exact" for _, api in apis),
            "unresolved_declarations": sum(api["declaration"]["status"] == "unresolved" for _, api in apis),
            "implementation_states": dict(sorted(implementation_states.items())),
            "contract_states": dict(sorted(contract_states.items())),
            "readiness_states": dict(sorted(readiness_states.items())),
        },
        "coverage": {
            "package_owners": package_owners,
            "api_primary_owners": api_owners,
            "ownership_registry": {
                "public_types": ownership["summary"]["public_type_count"],
                "shared_components": ownership["summary"]["shared_component_count"],
                "providers": ownership["summary"]["provider_subject_count"],
                "schemas": ownership["summary"]["schema_count"],
                "semantic_digest": ownership["semantic_digest"],
            },
            "downstream_edges": {
                "task_packets": len(corpus["packets"]),
                "ownership_skeletons": len(agent_ownership["skeletons"]),
                "ready_apis": sum(len(node["api_ids"]) for node in ready["nodes"]),
                "authorization_apis": len(authorization["apis"]),
            },
        },
        "evidence": {
            "package_validations": package_validations,
            "phase_a": [
                artifact(root, f"schemas/cxxlens_m{milestone}_completion.yaml")
                for milestone in range(3)
            ],
            "high_risk": {
                "state": high_risk["gate"]["state"],
                "semantic_digest": high_risk["evidence"]["semantic_digest"],
                "candidate_fingerprint_match": high_risk["gate"]["candidate_fingerprint_match"],
            },
            "integration": {
                "state": "green",
                "package_count": integration["counts"]["packages"],
                "api_count": integration["counts"]["apis"],
                "atomic_unit_count": integration["counts"]["atomic_units"],
                "source_tree": integration["matrices"]["source_tree"],
                "installed_tree": integration["matrices"]["installed_tree"],
                "compiler_families": ["Clang", "GCC"],
                "linkage": sorted(integration["matrices"]["linkage"]),
            },
        },
        "authorization": {
            "mode": "serial-single-writer",
            "scope": "phase-c-public-api-implementation",
            "prerequisite_issues": PREREQUISITE_ISSUES,
            "evidence_binding": "same-git-head",
            "drift_policy": {
                "invalidates_on_input_change": True,
                "revokes_phase_c_authorization": True,
            },
        },
    }
    return manifest


def schema_validate(document: dict[str, Any], schema: dict[str, Any]) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"freeze schema validation failed: {error.message}")


def validate_downstream_edges(root: pathlib.Path, catalog: dict[str, Any]) -> None:
    expected = {
        api["id"]: (package, api)
        for package in catalog["packages"]
        for api in package["apis"]
    }
    corpus = load_json(root / "schemas/cxxlens.agent-task-packet-corpus.v1.json")
    agent_ownership = load_json(root / "schemas/cxxlens.agent-ownership.v1.json")
    ready = load_json(root / "schemas/cxxlens.api-ready.report.v1.json")
    authorization = load_json(root / "schemas/cxxlens.readiness.authorization.v1.json")
    ready_by_api = {
        api_id: node for node in ready["nodes"] for api_id in node["api_ids"]
    }
    if sum(len(node["api_ids"]) for node in ready["nodes"]) != len(ready_by_api):
        fail("ready report contains duplicate API coverage")
    collections_by_id = {
        "task packet": {row["api_id"]: row for row in corpus["packets"]},
        "ownership skeleton": {row["api_id"]: row for row in agent_ownership["skeletons"]},
        "ready report": ready_by_api,
        "authorization": {row["api_id"]: row for row in authorization["apis"]},
    }
    for name, rows in collections_by_id.items():
        if set(rows) != set(expected):
            fail(f"{name} API coverage differs from frozen catalog")
    for api_id, (package, api) in expected.items():
        packet = collections_by_id["task packet"][api_id]
        if packet["contract"]["state"] != "frozen" or packet["contract"]["transition_issue"] != "#54":
            fail(f"{api_id}: task packet did not preserve the #54 frozen state")
        if packet["declaration"]["signature_fingerprint"] != api["declaration"]["signature_fingerprint"]:
            fail(f"{api_id}: task packet signature edge is stale")
        packet_trace = sorted(
            packet["traceability"]["requirements"] + packet["traceability"]["use_cases"]
        )
        if packet_trace != sorted(api["contract_traceability"]):
            fail(f"{api_id}: task packet traceability edge is lossy")
        skeleton = collections_by_id["ownership skeleton"][api_id]
        if skeleton["state"] != "frozen" or skeleton["contract_state"] != "frozen":
            fail(f"{api_id}: ownership skeleton is not frozen")
        authorization_row = collections_by_id["authorization"][api_id]
        if (
            authorization_row["contract_state"] != "frozen"
            or authorization_row["contract_transition_issue"] != "#54"
        ):
            fail(f"{api_id}: authorization lost the frozen contract edge")
        if package["contract"]["candidate_fingerprint"] != packet["contract"]["candidate_fingerprint"]:
            fail(f"{api_id}: candidate provenance changed downstream")


def validate_prerequisites(root: pathlib.Path) -> None:
    catalog = load_yaml(root / "schemas/cxxlens_public_api_contract.yaml")
    conventions = load_yaml(root / "schemas/cxxlens_global_contract_conventions.yaml")
    convention_schema = load_yaml(root / "schemas/cxxlens_global_contract_conventions.schema.yaml")
    ownership = load_yaml(root / "schemas/cxxlens_contract_ownership.yaml")
    ownership_schema = load_yaml(root / "schemas/cxxlens_contract_ownership.schema.yaml")
    candidates = load_yaml(root / "schemas/cxxlens_package_contract_candidates.yaml")
    candidate_schema = load_yaml(root / "schemas/cxxlens_package_contract_candidates.schema.yaml")
    validate_document(catalog)
    validate_conventions(conventions, convention_schema, catalog, root)
    expected_ownership = generate_ownership(catalog, conventions, root)
    validate_ownership(ownership, ownership_schema, expected_ownership, catalog, root)
    validate_candidates(candidates, candidate_schema, catalog, conventions, ownership, root)
    validate_metadata(root)
    groups = {group["issue"]: group for group in candidates["groups"]}
    if set(groups) != set(EXPECTED_GROUPS):
        fail("package validation issue ledger differs")
    for issue, (packages, api_count) in EXPECTED_GROUPS.items():
        group = groups[issue]
        if set(group["packages"]) != packages or len(group["api_contracts"]) != api_count:
            fail(f"{issue}: package/API ownership count differs")
    high_risk = load_yaml(root / "schemas/cxxlens_high_risk_contract_validation.yaml")
    snapshot = {row["issue"]: row["fingerprint"] for row in high_risk["candidate_snapshot"]}
    if snapshot != {issue: group["candidate_fingerprint"] for issue, group in groups.items()}:
        fail("#52 candidate fingerprint snapshot differs at freeze")
    validate_downstream_edges(root, catalog)


def validate_manifest(root: pathlib.Path) -> dict[str, Any]:
    schema = load_yaml(root / "schemas/cxxlens_public_api_contract_freeze.schema.yaml")
    manifest = load_yaml(root / MANIFEST)
    schema_validate(manifest, schema)
    if manifest["state"] != "superseded" or manifest["phase_c_authorized"] is not False:
        fail("legacy public API Contract Freeze still authorizes Phase C")
    supersession = manifest["supersession"]
    if (
        supersession["issue"] != "#57"
        or supersession["tracking_issue"] != "#56"
        or supersession["replacement"]
        != "schemas/cxxlens_ng_authority_transition.yaml"
        or supersession["legacy_new_work_authorized"] is not False
    ):
        fail("legacy public API Contract Freeze supersession edge differs")
    if manifest["summary"]["packages"] != 22 or manifest["summary"]["apis"] != 124:
        fail("legacy public API Contract Freeze baseline count differs")
    validate_downstream_edges(
        root, load_yaml(root / "schemas/cxxlens_public_api_contract.yaml")
    )
    return manifest


def git(root: pathlib.Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=False,
        text=True,
        capture_output=True,
    )
    if completed.returncode != 0:
        fail(f"git {' '.join(arguments)} failed: {completed.stderr.strip()}")
    return completed.stdout.strip()


def exact_report(
    root: pathlib.Path, manifest: dict[str, Any], integration_report_path: pathlib.Path
) -> dict[str, Any]:
    commit = git(root, "rev-parse", "HEAD")
    tree = git(root, "rev-parse", "HEAD^{tree}")
    if git(root, "status", "--porcelain"):
        fail("commit-bound freeze report requires a clean worktree")
    integration_report = load_json(integration_report_path)
    identity = integration_report.get("source_identity", {})
    if (
        integration_report.get("result") != "green"
        or not integration_report.get("traceability_complete")
        or identity.get("commit") != commit
        or identity.get("tree") != tree
        or identity.get("worktree_clean") is not True
    ):
        fail("Phase B integration report is not green and bound to the same clean commit")
    report = copy.deepcopy(manifest)
    report["repository"] = {
        "binding": "exact-git-commit",
        "input_fingerprint": manifest["inputs"]["fingerprint"],
        "commit_sha": commit,
        "tree_sha": tree,
        "evidence_commit_sha": commit,
        "worktree_clean": True,
        "integration_report": artifact(root, integration_report_path),
    }
    schema_validate(
        report, load_yaml(root / "schemas/cxxlens_public_api_contract_freeze.schema.yaml")
    )
    return report


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("generate", "check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--integration-report", type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    if args.mode == "generate":
        fail(
            "legacy public API Contract Freeze generation is forbidden after #57; "
            "use check_ng_authority.py"
        )
    else:
        manifest = validate_manifest(root)
        if args.mode == "report":
            fail(
                "legacy public API Contract Freeze report is forbidden after #57; "
                "use check_ng_authority.py report"
            )
    print(
        "superseded public API Contract Freeze check passed: "
        "legacy 22 packages, 124 APIs, Phase C authorization revoked, "
        f"historical input {manifest['inputs']['fingerprint']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        ValueError,
        json.JSONDecodeError,
        jsonschema.ValidationError,
        OSError,
        subprocess.SubprocessError,
        yaml.YAMLError,
    ) as error:
        print(f"public API Contract Freeze failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
