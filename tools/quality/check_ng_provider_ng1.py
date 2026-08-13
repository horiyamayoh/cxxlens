#!/usr/bin/env python3
"""Validate the proposed NG1 provider hardening authority.

The accepted provider protocol names NG1 features, but that name-only surface
is not sufficient to implement or qualify a provider.  This checker keeps the
versioned hardening contract closed until the live state machine and its
positive/negative evidence exist.
"""

from __future__ import annotations

import argparse
import pathlib
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONTRACT = pathlib.Path("schemas/cxxlens_ng_provider_ng1_hardening.yaml")
CONTRACT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_provider_ng1_hardening.schema.yaml"
)
PROTOCOL = pathlib.Path("schemas/cxxlens_ng_provider_protocol.yaml")


class Ng1ContractError(ValueError):
    """A fail-closed NG1 hardening contract violation."""


def fail(path: str, message: str) -> None:
    raise Ng1ContractError(f"provider.ng1.{path}: {message}")


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail("document", f"expected mapping: {path}")
    return value


def schema_validate(value: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(value)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail("schema", f"{label}: {error.message}")


def expect(actual: Any, expected: Any, path: str) -> None:
    if actual != expected:
        fail(path, f"expected {expected!r}, got {actual!r}")


FEATURES = [
    "durable-resume-token",
    "heartbeat",
    "progress-rate-enforcement",
    "spill-staging",
    "long-run-fault-qualification",
]


EXPECTED_HEARTBEAT = {
    "message_type": 23,
    "control_schema": "cxxlens.provider-control.heartbeat.v1",
    "direction": {"probe": "host-to-provider", "ack": "provider-to-host"},
    "exact_fields": [
        "schema",
        "kind",
        "provider_id",
        "provider_version",
        "protocol_session_id",
        "task_id",
        "stream_id",
        "heartbeat_sequence",
        "monotonic_time_ns",
        "highest_contiguous_acked_sequence",
        "staged_digest",
    ],
    "kinds": ["probe", "ack"],
    "identity_binding": [
        "provider_id",
        "provider_version",
        "protocol_session_id",
        "task_id",
        "stream_id",
    ],
    "sequence": {
        "starts_at": 0,
        "contiguous": "required",
        "duplicate": "reject",
        "replay": "reject",
    },
    "clock": {
        "source": "injected-monotonic-clock",
        "unit": "nanoseconds",
        "wall_clock": "forbidden",
        "backwards": "provider.heartbeat-clock-invalid",
    },
    "liveness": {
        "interval_ns": 1_000_000_000,
        "timeout_ns": 5_000_000_000,
        "startup_grace_ns": 10_000_000_000,
        "ack_deadline": "timeout-after-last-probe",
        "terminal_grace": "no-heartbeat-required-after-terminal",
        "timeout_result": "provider.heartbeat-timeout",
    },
    "credit": {
        "counts_toward_output_credit": False,
        "counts_toward_transport_budget": True,
        "max_control_bytes": 65536,
    },
}

EXPECTED_PROGRESS = {
    "message_type": 17,
    "control_schema": "cxxlens.provider-control.progress.v2",
    "exact_fields": [
        "schema",
        "task_id",
        "dependency_group_id",
        "progress_sequence",
        "monotonic_time_ns",
        "completed_units",
        "total_units",
    ],
    "unit": "provider-declared-monotonic-work-unit",
    "constraints": {
        "total_units": "positive-and-constant-per-task",
        "completed_units": "zero-through-total-inclusive",
        "sequence": "contiguous-from-zero",
        "time": "monotonic-with-heartbeat-clock",
        "duplicate_or_replay": "reject",
    },
    "enforcement": {
        "startup_grace_ns": 10_000_000_000,
        "sample_window_ns": 5_000_000_000,
        "maximum_sample_gap_ns": 10_000_000_000,
        "minimum_units_per_second": 1,
        "zero_delta_after_grace": "reject",
        "arithmetic": "overflow-safe-u128-cross-multiplication",
        "terminal_total_required": True,
        "failure": "provider.progress-rate",
    },
    "credit": {
        "counts_toward_output_credit": False,
        "counts_toward_transport_budget": True,
    },
}

EXPECTED_RESUME = {
    "message_type": 19,
    "control_schema": "cxxlens.provider-control.resume.v2",
    "exact_fields": [
        "schema",
        "kind",
        "provider_id",
        "provider_version",
        "provider_binary_digest",
        "provider_semantic_contract_digest",
        "protocol_session_id",
        "task_id",
        "dependency_group_id",
        "atomic_output_group_id",
        "batch_id",
        "stream_id",
        "highest_contiguous_acked_sequence",
        "staged_digest",
        "token_generation",
        "token_digest",
    ],
    "kinds": ["request", "accepted", "rejected"],
    "token_digest": {
        "domain": "cxxlens.provider-resume-token.v1",
        "projection": "all-fields-except-token_digest",
        "algorithm": "cxxlens-semantic-digest-v2",
    },
    "durability": {
        "prerequisite": "fsync-confirmed-spill-ack",
        "volatile_ack_is_not_authority": True,
        "generation": "strictly-increasing",
    },
    "acceptance": {
        "exact_provider_and_task_binding": "required",
        "exact_group_and_batch_binding": "required",
        "exact_stream_and_staged_digest_binding": "required",
        "replay_from": "highest_contiguous_acked_sequence-plus-one",
        "terminal_or_foreign_token": "provider.resume-token-stale",
        "mutation_or_digest_mismatch": "provider.resume-replay-invalid",
        "open_dependency_group_adoption": "forbidden",
    },
}

EXPECTED_SPILL = {
    "record_schema": "cxxlens.provider-spill-record.v1",
    "storage": "private-provider-port",
    "pathname_authority": "forbidden",
    "append_only": True,
    "record": {
        "exact_fields": [
            "schema",
            "record_ordinal",
            "task_id",
            "dependency_group_id",
            "atomic_output_group_id",
            "batch_id",
            "stream_id",
            "sequence",
            "payload_bytes",
            "payload_digest",
            "record_digest",
        ],
        "digest": {
            "domain": "cxxlens.provider-spill-record.v1",
            "projection": "all-fields-except-record_digest",
            "algorithm": "cxxlens-semantic-digest-v2",
        },
    },
    "limits": {
        "maximum_record_bytes": 16_777_216,
        "maximum_total_bytes": 67_108_864,
        "maximum_records": 65_536,
        "validate_before_allocation": True,
    },
    "durability": {
        "append": "exact-byte-count-and-digest",
        "acknowledgement": "fsync-before-resume-token",
        "ordering": "contiguous-record-ordinal",
        "torn_last_record": "provider.spill-corrupt",
        "corruption_or_gap": "provider.spill-corrupt",
    },
    "cleanup": {
        "success": "after-final-report-and-token-disposal",
        "failure": "after-recovery-classification",
        "cleanup_failure": "provider.recovery-failed",
        "retry": "forbidden-after-unknown-effect",
    },
}

EXPECTED_RECOVERY = {
    "states": [
        "running",
        "heartbeat-timeout",
        "progress-rate-failure",
        "cancel-requested",
        "worker-killed",
        "resume-replay",
        "resumed",
        "completed",
        "failed",
    ],
    "transition_rules": {
        "worker_hang": [
            "heartbeat-timeout",
            "worker-killed",
            "resume-replay",
            "resumed",
            "failed",
        ],
        "worker_crash": ["worker-killed", "resume-replay", "resumed", "failed"],
        "spill_corruption": ["failed"],
        "stale_token": ["failed"],
        "cancellation": ["cancel-requested", "worker-killed", "failed"],
        "terminal": ["completed", "failed"],
    },
    "publication": {
        "prior_published_snapshot": "unchanged-on-failure",
        "current_dependency_group": "rollback",
        "prior_adopted_groups": "retain-only-if-predeclared-partial-policy",
        "success_requires": [
            "all-groups-sealed",
            "coverage-balanced",
            "unresolved-accounted",
            "progress-terminal",
        ],
    },
    "no_fallback": {
        "adjacent_provider": "forbidden",
        "non-durable_resume": "forbidden",
        "manifest_self_claim": "forbidden",
    },
}

EXPECTED_STABLE_FAILURES = [
    "provider.heartbeat-clock-invalid",
    "provider.heartbeat-timeout",
    "provider.progress-rate",
    "provider.resume-token-stale",
    "provider.resume-replay-invalid",
    "provider.spill-corrupt",
    "provider.recovery-failed",
]

EXPECTED_QUALIFICATION = {
    "schema": "cxxlens.provider-ng1-qualification.v1",
    "exact_binding": [
        "revision",
        "tree",
        "provider_binary_digest",
        "provider_semantic_contract_digest",
        "protocol_minor",
        "hardening_contract_digest",
    ],
    "required_profiles": ["static", "shared"],
    "required_cases": [
        "positive-heartbeat-and-progress",
        "stale-heartbeat",
        "heartbeat-timeout",
        "zero-progress-after-grace",
        "stale-resume-token",
        "foreign-resume-token",
        "spill-corruption",
        "worker-crash-recovery",
        "worker-hang-recovery",
        "cancellation-recovery",
        "permutation-replay",
        "long-run-fault",
    ],
    "release_claim": "exact-measured-certificate-only",
    "unavailable_provider": "unqualified",
    "unavailable_platform": "unqualified",
}


def validate_ng1_contract(
    root: pathlib.Path,
    protocol: dict[str, Any] | None = None,
    hardening: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Validate the NG1 authority and return the loaded contract."""

    if hardening is None:
        hardening = load_yaml(root / CONTRACT)
    schema = load_yaml(root / CONTRACT_SCHEMA)
    schema_validate(hardening, schema, "NG1 hardening contract")

    expect(hardening["schema"], "cxxlens.provider-ng1-hardening.v1", "schema")
    expect(hardening["document_version"], "1.0.0", "document_version")
    if hardening["maturity"] not in {"proposed", "accepted"}:
        fail("maturity", "must be proposed or accepted")
    expect(
        hardening["authority"],
        {
            "design": "docs/design/cxxlens_next_generation_integrated_design_ja.md",
            "protocol": "schemas/cxxlens_ng_provider_protocol.yaml",
            "runtime": "schemas/cxxlens_ng_provider_runtime_contract.yaml",
            "decision_adr": "docs/design/adr/0099-provider-ng1-hardening.md",
            "decision_issue": "#233",
            "implementation_issue": "#183",
            "owner": "steward.ng-provider-runtime",
        },
        "authority",
    )
    expect(
        hardening["profile"],
        {
            "id": "NG1",
            "protocol_major": 1,
            "protocol_minor": 1,
            "required_features": FEATURES,
            "capability_claim": "exact-certified-qualification-only",
            "manifest_self_claim_authority": "forbidden",
        },
        "profile",
    )
    expect(hardening["heartbeat"], EXPECTED_HEARTBEAT, "heartbeat")
    expect(hardening["progress"], EXPECTED_PROGRESS, "progress")
    expect(hardening["resume"], EXPECTED_RESUME, "resume")
    expect(hardening["spill"], EXPECTED_SPILL, "spill")
    expect(hardening["recovery"], EXPECTED_RECOVERY, "recovery")
    expect(hardening["stable_failures"], EXPECTED_STABLE_FAILURES, "stable_failures")
    expect(hardening["qualification"], EXPECTED_QUALIFICATION, "qualification")

    heartbeat_liveness = hardening["heartbeat"]["liveness"]
    if not (
        heartbeat_liveness["interval_ns"]
        < heartbeat_liveness["timeout_ns"]
        <= heartbeat_liveness["startup_grace_ns"]
    ):
        fail("heartbeat.liveness", "interval < timeout <= startup grace required")
    progress_enforcement = hardening["progress"]["enforcement"]
    if not (
        progress_enforcement["sample_window_ns"]
        < progress_enforcement["maximum_sample_gap_ns"]
        <= progress_enforcement["startup_grace_ns"]
    ):
        fail("progress.enforcement", "sample window/gap/grace ordering")
    spill_limits = hardening["spill"]["limits"]
    if spill_limits["maximum_record_bytes"] > 16_777_216:
        fail("spill.limits", "record exceeds negotiated provider payload limit")
    if spill_limits["maximum_total_bytes"] < spill_limits["maximum_record_bytes"]:
        fail("spill.limits", "total spill budget is below one record")
    if set(EXPECTED_STABLE_FAILURES) != {
        hardening["heartbeat"]["clock"]["backwards"],
        hardening["heartbeat"]["liveness"]["timeout_result"],
        hardening["progress"]["enforcement"]["failure"],
        hardening["resume"]["acceptance"]["terminal_or_foreign_token"],
        hardening["resume"]["acceptance"]["mutation_or_digest_mismatch"],
        hardening["spill"]["durability"]["corruption_or_gap"],
        hardening["spill"]["cleanup"]["cleanup_failure"],
    }:
        fail("stable_failures", "failure codes are not cross-bound to controls")

    if protocol is None:
        protocol = load_yaml(root / PROTOCOL)
    ng1 = protocol.get("profiles", {}).get("NG1")
    if not isinstance(ng1, dict):
        fail("protocol", "NG1 profile is missing")
    expect(ng1.get("includes"), "NG0", "protocol.profiles.NG1.includes")
    expect(ng1.get("required"), FEATURES, "protocol.profiles.NG1.required")
    expect(
        ng1.get("hardening_contract"),
        CONTRACT.as_posix(),
        "protocol.profiles.NG1.hardening_contract",
    )
    message_23 = next(
        (row for row in protocol.get("message_types", {}).get("registry", []) if row.get("id") == 23),
        None,
    )
    expect(
        message_23,
        {"id": 23, "name": "heartbeat", "direction": "bidirectional", "profile": "NG1"},
        "protocol.message_types.23",
    )
    return hardening


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    args = parser.parse_args()
    root = args.root.resolve()
    contract = validate_ng1_contract(root)
    print(
        "verified NG1 hardening contract: "
        f"maturity={contract['maturity']}, "
        f"profiles={len(contract['qualification']['required_profiles'])}, "
        f"cases={len(contract['qualification']['required_cases'])}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, Ng1ContractError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1) from error
