#!/usr/bin/env python3
"""Independently audit foundation and agent artifacts before parallel dispatch."""

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
sys.path.insert(0, str(ROOT / "tools" / "agent"))

from api_task_runner import RunnerError, authorize_run  # noqa: E402
from ownership_generator import (  # noqa: E402
    OwnershipError,
    generate_manifest,
    repository_paths,
    transition_request,
    validate_changed_paths,
    validate_manifest,
)
from ready_evaluator import (  # noqa: E402
    ReadyError,
    canonicalize_input,
    digest,
    generate_report as generate_ready_report,
    parse_prompt,
    resolve_api,
    validate_report as validate_ready_report,
)
from task_packet_generator import (  # noqa: E402
    TaskPacketError,
    generate_corpus,
    validate_corpus,
)


SCHEMA_ID = "cxxlens.readiness.authorization.v1"
STEWARD_ISSUES = {
    "generator.catalog": 27,
    "steward.ownership": 28,
    "steward.facts": 1,
    "steward.repository": 1,
}


class AuditError(ValueError):
    """A final-readiness invariant violation with a stable code."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code


def fail(code: str, message: str) -> None:
    raise AuditError(code, message)


def pretty_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def file_digest(path: pathlib.Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def load_json(path: pathlib.Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def load_yaml(path: pathlib.Path) -> Any:
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def state_counts(values: list[str]) -> dict[str, int]:
    counts = collections.Counter(values)
    return {state: counts.get(state, 0) for state in ("blocked", "complete", "ready")}


def catalog_entries(catalog: dict[str, Any]) -> list[tuple[dict[str, Any], dict[str, Any]]]:
    return [
        (package, api)
        for package in catalog["packages"]
        for api in package["apis"]
    ]


def validate_upstream_artifacts(inputs: dict[str, Any], root: pathlib.Path) -> None:
    expected_corpus = generate_corpus(inputs["catalog"], root)
    validate_corpus(
        inputs["corpus"],
        inputs["catalog"],
        root,
        inputs["task_schema"],
    )
    if inputs["corpus"] != expected_corpus:
        fail("readiness.task-packet-drift", "task packet regeneration differs")

    paths = repository_paths(root)
    expected_ownership = generate_manifest(inputs["corpus"], paths)
    validate_manifest(
        inputs["ownership"],
        inputs["corpus"],
        paths,
        inputs["ownership_schema"],
    )
    if inputs["ownership"] != expected_ownership:
        fail("readiness.ownership-drift", "ownership regeneration differs")

    expected_ready = generate_ready_report(
        inputs["corpus"],
        inputs["ownership"],
        inputs["m0"],
        inputs["m1"],
        inputs["m2"],
        inputs["requests"],
    )
    validate_ready_report(inputs["ready"], expected_ready, inputs["ready_schema"])
    for milestone in ("m0", "m1", "m2"):
        jsonschema.Draft202012Validator(inputs[f"{milestone}_schema"]).validate(
            inputs[milestone]
        )


def input_fingerprints(inputs: dict[str, Any], root: pathlib.Path) -> dict[str, str]:
    return {
        "catalog": inputs["corpus"]["catalog_fingerprint"],
        "task_packets": inputs["corpus"]["semantic_digest"],
        "ownership": inputs["ownership"]["semantic_digest"],
        "ready_report": inputs["ready"]["semantic_digest"],
        "dependency_requests": digest(canonicalize_input(inputs["requests"])),
        "m0": digest(canonicalize_input(inputs["m0"])),
        "m1": digest(canonicalize_input(inputs["m1"])),
        "m2": digest(canonicalize_input(inputs["m2"])),
        "design_package": file_digest(root / "docs/design/SHA256SUMS"),
        "quality_workflow": file_digest(root / ".github/workflows/quality.yml"),
        "build_gates": digest(
            {
                path: file_digest(root / path)
                for path in (
                    "CMakeLists.txt",
                    "CMakePresets.json",
                    "cmake/CxxlensDeveloperTools.cmake",
                    "tests/CMakeLists.txt",
                )
            }
        ),
        "audit_policy": digest(
            {
                "implementation": file_digest(root / "tools/agent/readiness_audit.py"),
                "runner": file_digest(root / "tools/agent/api_task_runner.py"),
                "unit_local_gate": file_digest(root / "tools/agent/unit_local_gate.py"),
                "ready_evaluator": file_digest(root / "tools/agent/ready_evaluator.py"),
                "schema": file_digest(
                    root / "schemas/cxxlens.readiness.authorization.v1.schema.yaml"
                ),
                "run_schema": file_digest(
                    root / "schemas/cxxlens.api-ready.run-manifest.v1.schema.yaml"
                ),
                "execution_result_schema": file_digest(
                    root / "schemas/cxxlens.api-ready.execution-result.v1.schema.yaml"
                ),
                "tests": file_digest(
                    root / "tests/agent/readiness/test_readiness_audit.py"
                ),
                "runner_tests": file_digest(
                    root / "tests/agent/runner/test_runner.py"
                ),
                "negative_fixtures": file_digest(
                    root / "tests/agent/readiness/fixtures/negative_cases.yaml"
                ),
            }
        ),
    }


def foundation_gates() -> list[dict[str, Any]]:
    rows = []
    for milestone, issue in (("M0", 12), ("M1", 22), ("M2", 26)):
        lowered = milestone.lower()
        rows.append(
            {
                "id": milestone,
                "issue": issue,
                "required_ci_job": (
                    "m0-acceptance (ON/OFF)"
                    if milestone == "M0"
                    else f"{lowered}-acceptance"
                ),
                "required_status": "success",
                "completion_manifest": f"schemas/cxxlens_{lowered}_completion.yaml",
                "replay_commands": [
                    {
                        "argv": ["cmake", "--preset", f"{lowered}-acceptance"],
                        "environment": {"CXX": "clang++-22"},
                    },
                    {
                        "argv": [
                            "cmake",
                            "--build",
                            "--preset",
                            f"{lowered}-acceptance",
                            "--target",
                            f"cxxlens-{lowered}-acceptance",
                        ],
                        "environment": {},
                    },
                ],
                "evidence": sorted(
                    {
                        f"schemas/cxxlens_{lowered}_completion.yaml",
                        f"schemas/cxxlens_{lowered}_completion.schema.yaml",
                        f"schemas/cxxlens_{lowered}_acceptance_report.schema.yaml",
                        ".github/workflows/quality.yml",
                    }
                ),
            }
        )
    return rows


def audit_blockers(node: dict[str, Any]) -> list[dict[str, Any]]:
    rows = []
    for blocker in node["blockers"]:
        resolution_issue = (
            28
            if blocker["code"] == "dependency-request-open"
            else STEWARD_ISSUES.get(blocker["steward"], 1)
        )
        rows.append({**blocker, "resolution_issue": resolution_issue})
    return rows


def make_dry_runs(inputs: dict[str, Any]) -> dict[str, Any]:
    packets = sorted(inputs["corpus"]["packets"], key=lambda item: item["api_id"])
    prompts = []
    for kind in ("free_function", "method", "method_family", "builder_family", "static_factory"):
        packet = next(item for item in packets if item["kind"] == kind)
        if parse_prompt(f"{packet['api_id']} を実装してください") != packet["api_id"]:
            fail("readiness.prompt-drift", packet["api_id"])
        resolved = resolve_api(
            packet["api_id"], inputs["ready"], inputs["corpus"], inputs["ownership"]
        )
        prompts.append(
            {
                "api_id": packet["api_id"],
                "kind": kind,
                "phase": packet["phase"],
                "atomic_unit_id": resolved["atomic_unit_id"],
                "state": resolved["state"],
                "start_authorized": resolved["start_authorized"],
                "packet_digest": resolved["task_packet_digest"],
                "shard_digest": resolved["shard_digest"],
                "acceptance_argv": resolved["acceptance_command"]["argv"],
            }
        )

    unknown_code = ""
    try:
        resolve_api("API-NOT-999", inputs["ready"], inputs["corpus"], inputs["ownership"])
    except ReadyError as error:
        unknown_code = error.code
    if unknown_code != "ready.unknown-api":
        fail("readiness.unknown-prompt-started", unknown_code)

    blocked_node = next(node for node in inputs["ready"]["nodes"] if node["state"] == "blocked")
    blocked_resolution = resolve_api(
        blocked_node["api_ids"][0],
        inputs["ready"],
        inputs["corpus"],
        inputs["ownership"],
    )
    blocked_code = ""
    try:
        authorize_run(blocked_resolution)
    except RunnerError as error:
        blocked_code = error.code
    if blocked_code != "runner.not-ready":
        fail("readiness.blocked-prompt-started", blocked_node["api_ids"][0])

    unit_id = blocked_node["atomic_unit_id"]
    unauthorized_path = next(
        item["path"]
        for item in inputs["ownership"]["tracked_paths"]
        if item["owner_role"]
        != next(
            unit["unit_owner_role"]
            for unit in inputs["ownership"]["units"]
            if unit["atomic_unit_id"] == unit_id
        )
    )
    unauthorized_code = ""
    try:
        validate_changed_paths(inputs["ownership"], unit_id, [unauthorized_path])
    except OwnershipError as error:
        unauthorized_code = error.code
    if unauthorized_code not in {
        "ownership.generated-direct-edit",
        "ownership.unauthorized-path",
    }:
        fail("readiness.unauthorized-edit-accepted", unauthorized_path)

    pending = next(
        request for request in inputs["requests"]["requests"] if request["state"] == "pending"
    )
    accepted = transition_request(pending, "accepted", inputs["ownership"])
    resolved = transition_request(
        accepted,
        "resolved",
        inputs["ownership"],
        ["shared contract published and task packet reissued"],
    )
    resolved_requests = copy.deepcopy(inputs["requests"])
    resolved_requests["requests"] = [
        resolved if request["request_id"] == pending["request_id"] else request
        for request in resolved_requests["requests"]
    ]
    reissued = generate_ready_report(
        inputs["corpus"],
        inputs["ownership"],
        inputs["m0"],
        inputs["m1"],
        inputs["m2"],
        resolved_requests,
    )
    original_node = next(
        node
        for node in inputs["ready"]["nodes"]
        if node["atomic_unit_id"] == pending["requesting_atomic_unit"]
    )
    reissued_node = next(
        node
        for node in reissued["nodes"]
        if node["atomic_unit_id"] == pending["requesting_atomic_unit"]
    )
    open_observed = any(
        blocker["code"] == "dependency-request-open" for blocker in original_node["blockers"]
    )
    removed = not any(
        blocker["code"] == "dependency-request-open" for blocker in reissued_node["blockers"]
    )
    if not open_observed or not removed or reissued["semantic_digest"] == inputs["ready"]["semantic_digest"]:
        fail("readiness.dependency-request-reissue", pending["request_id"])

    return {
        "prompts": prompts,
        "unknown_prompt": {"code": unknown_code, "before_compilation": True},
        "blocked_prompt": {"code": blocked_code, "before_compilation": True},
        "unauthorized_edit": {
            "code": unauthorized_code,
            "before_compilation": True,
        },
        "dependency_request": {
            "request_id": pending["request_id"],
            "atomic_unit_id": pending["requesting_atomic_unit"],
            "transition_order": ["pending", "accepted", "resolved"],
            "open_blocker_observed": open_observed,
            "resolved_blocker_removed": removed,
            "report_reissued": reissued["semantic_digest"]
            != inputs["ready"]["semantic_digest"],
        },
    }


def generate_authorization(inputs: dict[str, Any], root: pathlib.Path) -> dict[str, Any]:
    validate_upstream_artifacts(inputs, root)
    fingerprints = input_fingerprints(inputs, root)
    packets_by_api = {
        packet["api_id"]: packet for packet in inputs["corpus"]["packets"]
    }
    nodes_by_id = {
        node["atomic_unit_id"]: node for node in inputs["ready"]["nodes"]
    }
    shards_by_id = {shard["id"]: shard for shard in inputs["ready"]["shards"]}
    integration_by_package = {
        shard["package_id"]: shard
        for shard in inputs["ready"]["package_integration_shards"]
    }
    wave_by_unit = {
        unit_id: index
        for index, wave in enumerate(inputs["ready"]["topological_waves"])
        for unit_id in wave
    }
    units = []
    for unit_id, node in sorted(nodes_by_id.items()):
        packets = sorted(
            (
                packet
                for packet in packets_by_api.values()
                if packet["atomic_unit_id"] == unit_id
            ),
            key=lambda item: item["api_id"],
        )
        package_ids = {packet["package"]["id"] for packet in packets}
        if len(package_ids) != 1:
            fail("readiness.family-split", f"{unit_id}: {sorted(package_ids)}")
        package_id = next(iter(package_ids))
        shard = shards_by_id[node["shard_id"]]
        units.append(
            {
                "atomic_unit_id": unit_id,
                "package_id": package_id,
                "api_ids": node["api_ids"],
                "state": node["state"],
                "topological_wave": wave_by_unit[unit_id],
                "dispatch_authorized": node["state"] == "ready",
                "shard_id": shard["id"],
                "shard_digest": shard["semantic_digest"],
                "package_integration_shard_id": integration_by_package[package_id]["id"],
                "blockers": audit_blockers(node),
            }
        )

    unit_audit = {unit["atomic_unit_id"]: unit for unit in units}
    apis = []
    for package, api in sorted(catalog_entries(inputs["catalog"]), key=lambda item: item[1]["id"]):
        packet = packets_by_api[api["id"]]
        unit = unit_audit[packet["atomic_unit_id"]]
        apis.append(
            {
                "api_id": api["id"],
                "atomic_unit_id": unit["atomic_unit_id"],
                "package_id": package["id"],
                "kind": api["kind"],
                "phase": api["phase"],
                "declaration_status": api["declaration"]["status"],
                "implementation_state": api["implementation_state"],
                "state": unit["state"],
                "dispatch_authorized": unit["dispatch_authorized"],
                "task_packet_digest": packet["semantic_digest"],
                "blockers": unit["blockers"],
            }
        )

    api_states = state_counts([api["state"] for api in apis])
    unit_states = state_counts([unit["state"] for unit in units])
    ready_waves = inputs["ready"]["summary"]["ready_waves"]
    ready_units = sorted(unit["atomic_unit_id"] for unit in units if unit["state"] == "ready")
    entries = catalog_entries(inputs["catalog"])
    report: dict[str, Any] = {
        "schema": SCHEMA_ID,
        "policy": {
            "waivers_allowed": False,
            "dispatch_scope": "manifest_ready_units_only",
            "rollback_triggers": [
                "catalog-or-signature-drift",
                "completion-or-foundation-gate-failure",
                "dependency-or-provider-drift",
                "ownership-or-shared-contract-drift",
                "schema-task-packet-or-shard-drift",
            ],
        },
        "input_fingerprints": fingerprints,
        "foundation_gates": foundation_gates(),
        "catalog_audit": {
            "package_count": len(inputs["catalog"]["packages"]),
            "api_count": len(entries),
            "exact_declaration_count": sum(
                api["declaration"]["status"] == "exact" for _, api in entries
            ),
            "unresolved_declaration_count": sum(
                api["declaration"]["status"] == "unresolved" for _, api in entries
            ),
            "duplicate_api_count": 0,
            "dangling_api_dependency_count": 0,
            "unresolved_ready_signature_count": sum(
                api["readiness"]["state"] == "ready"
                and api["declaration"]["status"] != "exact"
                for _, api in entries
            ),
            "evidence_free_conformant_count": sum(
                api["implementation_state"] == "conformant"
                and not api["implementation_evidence"]
                for _, api in entries
            ),
        },
        "infrastructure_audit": {
            "atomic_unit_count": len(inputs["corpus"]["atomic_units"]),
            "packet_count": len(inputs["corpus"]["packets"]),
            "tracked_path_count": inputs["ownership"]["summary"]["tracked_path_count"],
            "reserved_path_count": inputs["ownership"]["summary"]["reserved_path_count"],
            "dependency_edge_count": len(inputs["ready"]["edges"]),
            "package_integration_shard_count": len(
                inputs["ready"]["package_integration_shards"]
            ),
            "write_overlap_count": 0,
            "unowned_path_count": 0,
            "cycle_count": 0,
            "dangling_edge_count": 0,
            "provider_ambiguity_count": 0,
        },
        "dry_runs": make_dry_runs(inputs),
        "units": units,
        "apis": apis,
        "authorization": {
            "decision": "authorized" if ready_units else "denied",
            "reason_code": (
                "ready-wave-available" if ready_units else "no-ready-incomplete-unit"
            ),
            "ready_unit_ids": ready_units,
            "ready_waves": ready_waves,
            "validity": {
                "state": "current",
                "basis": "input_fingerprints",
                "expires_on": "any_relevant_input_drift",
                "fingerprint_digest": digest(fingerprints),
            },
        },
        "summary": {
            "package_count": len(inputs["catalog"]["packages"]),
            "api_count": len(apis),
            "unit_count": len(units),
            "api_state_counts": api_states,
            "unit_state_counts": unit_states,
        },
        "checks": [
            "all-api-exactly-once",
            "catalog-chapter40-and-schema",
            "dependency-request-block-resolve-reissue",
            "foundation-completion-and-ci-linkage",
            "input-drift-auto-expiry",
            "ownership-and-package-integration",
            "prompt-and-shard-dry-run",
            "provider-dag-and-ready-predicate",
            "public-install-doxygen-flagship-evidence",
            "root-order-process-backend-cache-determinism",
            "task-packet-family-atomicity",
        ],
    }
    report["semantic_digest"] = digest(report)
    return report


def validate_authorization(
    report: dict[str, Any],
    expected: dict[str, Any],
    schema: dict[str, Any] | None = None,
) -> None:
    if not isinstance(report, dict) or report.get("schema") != SCHEMA_ID:
        fail("readiness.unknown-schema", "authorization schema is unsupported")
    if report.get("input_fingerprints") != expected["input_fingerprints"]:
        fail("readiness.input-drift", "authorization evidence expired on input drift")
    units = report.get("units", [])
    unit_ids = [unit["atomic_unit_id"] for unit in units]
    expected_unit_ids = [unit["atomic_unit_id"] for unit in expected["units"]]
    if sorted(unit_ids) != sorted(expected_unit_ids) or len(unit_ids) != len(set(unit_ids)):
        fail("readiness.unit-coverage", "atomic units are not covered exactly once")
    apis = report.get("apis", [])
    api_ids = [api["api_id"] for api in apis]
    expected_api_ids = [api["api_id"] for api in expected["apis"]]
    if sorted(api_ids) != sorted(expected_api_ids) or len(api_ids) != len(set(api_ids)):
        fail("readiness.api-coverage", "catalog APIs are not covered exactly once")
    units_by_id = {unit["atomic_unit_id"]: unit for unit in units}
    for unit in units:
        if unit["dispatch_authorized"] != (unit["state"] == "ready"):
            fail("readiness.false-authorization", unit["atomic_unit_id"])
        if unit["state"] == "blocked" and not unit["blockers"]:
            fail("readiness.blocker-missing", unit["atomic_unit_id"])
    for api in apis:
        unit = units_by_id.get(api["atomic_unit_id"])
        if unit is None or api["api_id"] not in unit["api_ids"]:
            fail("readiness.api-unit-drift", api["api_id"])
        if (
            api["state"] != unit["state"]
            or api["dispatch_authorized"] != unit["dispatch_authorized"]
            or api["blockers"] != unit["blockers"]
        ):
            fail("readiness.family-split", api["api_id"])
    ready_units = sorted(
        unit["atomic_unit_id"] for unit in units if unit["dispatch_authorized"]
    )
    authorization = report.get("authorization", {})
    decision = authorization.get("decision")
    if (decision == "authorized") != bool(ready_units):
        fail("readiness.false-authorization", f"decision={decision}")
    if authorization.get("ready_unit_ids") != ready_units:
        fail("readiness.ready-set-drift", "authorization ready set differs from units")
    unsigned = copy.deepcopy(report)
    semantic_digest = unsigned.pop("semantic_digest", None)
    if semantic_digest != digest(unsigned):
        fail("readiness.digest-mismatch", "authorization digest mismatch")
    if schema is not None:
        try:
            jsonschema.Draft202012Validator.check_schema(schema)
            jsonschema.Draft202012Validator(schema).validate(report)
        except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
            fail("readiness.schema-invalid", error.message)
    if report != expected:
        fail("readiness.stale-authorization", "authorization manifest is stale")


def load_inputs(root: pathlib.Path) -> dict[str, Any]:
    schemas = root / "schemas"
    return {
        "catalog": load_yaml(schemas / "cxxlens_public_api_contract.yaml"),
        "corpus": load_json(schemas / "cxxlens.agent-task-packet-corpus.v1.json"),
        "task_schema": load_yaml(schemas / "cxxlens.agent-task-packet.v1.schema.yaml"),
        "ownership": load_json(schemas / "cxxlens.agent-ownership.v1.json"),
        "ownership_schema": load_yaml(
            schemas / "cxxlens.agent-ownership.v1.schema.yaml"
        ),
        "requests": load_json(schemas / "cxxlens.dependency-request.examples.v1.json"),
        "ready": load_json(schemas / "cxxlens.api-ready.report.v1.json"),
        "ready_schema": load_yaml(schemas / "cxxlens.api-ready.v1.schema.yaml"),
        "m0": load_yaml(schemas / "cxxlens_m0_completion.yaml"),
        "m0_schema": load_yaml(schemas / "cxxlens_m0_completion.schema.yaml"),
        "m1": load_yaml(schemas / "cxxlens_m1_completion.yaml"),
        "m1_schema": load_yaml(schemas / "cxxlens_m1_completion.schema.yaml"),
        "m2": load_yaml(schemas / "cxxlens_m2_completion.yaml"),
        "m2_schema": load_yaml(schemas / "cxxlens_m2_completion.schema.yaml"),
        "authorization_schema": load_yaml(
            schemas / "cxxlens.readiness.authorization.v1.schema.yaml"
        ),
    }


def replay_static_checks(root: pathlib.Path) -> None:
    commands = [
        [sys.executable, f"tools/quality/check_{milestone}_completion.py", str(root)]
        for milestone in ("m0", "m1", "m2")
    ] + [
        [sys.executable, "tools/agent/task_packet_generator.py", "check", "--root", "."],
        [sys.executable, "tools/agent/ownership_generator.py", "check", "--root", "."],
        [sys.executable, "tools/agent/ready_evaluator.py", "check", "--root", "."],
    ]
    for command in commands:
        completed = subprocess.run(
            command,
            cwd=root,
            check=False,
            text=True,
            capture_output=True,
        )
        if completed.returncode != 0:
            detail = (completed.stderr or completed.stdout).strip()
            fail("readiness.replay-failed", f"argv={command} output={detail}")


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("generate", "check"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    inputs = load_inputs(root)
    generated = generate_authorization(inputs, root)
    path = root / "schemas/cxxlens.readiness.authorization.v1.json"
    if args.mode == "generate":
        path.write_text(pretty_json(generated), encoding="utf-8")
    else:
        replay_static_checks(root)
        report = load_json(path)
        validate_authorization(report, generated, inputs["authorization_schema"])
    summary = generated["summary"]
    print(
        f"readiness audit {args.mode} passed: {summary['package_count']} packages, "
        f"{summary['api_count']} APIs, states {summary['unit_state_counts']}, "
        f"decision {generated['authorization']['decision']}, "
        f"digest {generated['semantic_digest']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        AuditError,
        json.JSONDecodeError,
        jsonschema.ValidationError,
        OSError,
        OwnershipError,
        ReadyError,
        RunnerError,
        subprocess.CalledProcessError,
        TaskPacketError,
        yaml.YAMLError,
    ) as error:
        print(f"readiness audit failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
