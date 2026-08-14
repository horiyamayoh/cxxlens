#!/usr/bin/env python3
"""Validate the proposed NG1 provider hardening authority.

The accepted provider protocol names NG1 features, but that name-only surface
is not sufficient to implement or qualify a provider.  This checker keeps the
versioned hardening contract closed until the live state machine and its
positive/negative evidence exist.
"""

from __future__ import annotations

import argparse
import hashlib
import json
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
MANIFEST_SCHEMA = pathlib.Path("schemas/cxxlens_ng_provider_manifest.schema.yaml")
EXECUTION_REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_provider_execution_report.schema.yaml"
)
SPILL_FSYNC_RECEIPT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_provider_spill_fsync_receipt.schema.yaml"
)
RUNTIME_CONTRACT = pathlib.Path("schemas/cxxlens_ng_provider_runtime_contract.yaml")
VECTORS = pathlib.Path("schemas/cxxlens_ng_provider_ng1_conformance_vectors.yaml")
VECTORS_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_provider_ng1_conformance_vectors.schema.yaml"
)
QUALIFICATION_REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_provider_ng1_qualification_report.schema.yaml"
)
DIGEST_GRAMMAR_ADR = pathlib.Path(
    "docs/design/adr/0100-ng1-resume-provider-digest-grammar.md"
)
DIGEST_GRAMMAR_ISSUE = "#243"


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


def document_digest(value: Any) -> str:
    canonical = json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    return "sha256:" + hashlib.sha256(canonical).hexdigest()


FEATURES = [
    "durable-resume-token",
    "heartbeat",
    "progress-rate-enforcement",
    "spill-staging",
    "long-run-fault-qualification",
]

EXPECTED_LIFECYCLE = {
    "host_receipt": {
        "type": "uint64",
        "unit": "nanoseconds",
        "source": "host-injected-monotonic-clock",
        "capture": "once-before-shared-validation",
        "authority": "liveness-and-progress-rate",
        "monotonicity": "non-decreasing-per-protocol-session",
        "backwards": "provider.heartbeat-clock-invalid",
    },
    "session_start": {
        "event": "validated-schema-negotiate-complete",
        "receipt": "host-receipt-at-event-boundary",
        "binding": ["protocol_session_id"],
    },
    "task_start": {
        "event": "validated-task-accepted",
        "receipt": "host-receipt-at-frame-ingress-before-validation",
        "binding": ["protocol_session_id", "task_id"],
    },
    "task_terminal": {
        "event": "validated-task-complete-or-task-failed",
        "receipt": "host-receipt-at-frame-ingress-before-validation",
    },
}


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
    "field_types": {
        "schema": "fixed-string",
        "kind": "enum",
        "provider_id": "typed-id",
        "provider_version": "semantic-version",
        "protocol_session_id": "typed-id",
        "task_id": "typed-id",
        "stream_id": "uint64",
        "heartbeat_sequence": "uint64",
        "monotonic_time_ns": "uint64",
        "highest_contiguous_acked_sequence": "uint64",
        "staged_digest": "semantic-digest",
    },
    "field_constraints": {
        "schema": "exact-control-schema",
        "kind": "probe-or-ack",
        "provider_id": "non-empty-canonical-identity",
        "provider_version": "exact-negotiated-provider-version",
        "protocol_session_id": "exact-session-binding",
        "task_id": "exact-task-binding",
        "stream_id": "exact-stream-binding",
        "heartbeat_sequence": "contiguous-from-zero-per-session",
        "monotonic_time_ns": "injected-clock-non-decreasing",
        "highest_contiguous_acked_sequence": "contiguous-ack-not-ahead-of-observed",
        "staged_digest": "exact-staged-prefix-digest",
    },
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
        "namespace": "per-session-and-direction",
        "contiguous": "required",
        "duplicate": "reject",
        "replay": "reject",
    },
    "clock": {
        "source": "host-injected-monotonic-clock",
        "unit": "nanoseconds",
        "wall_clock": "forbidden",
        "host_receipt": {
            "source": "host-injected-monotonic-clock",
            "capture": "once-before-shared-validation",
            "authority": "liveness-and-progress-rate",
        },
        "provider_timestamp": {
            "field": "monotonic_time_ns",
            "clock_domain": "session-host-monotonic",
            "role": "ordering-and-diagnostics-only",
            "ordering_scope": ["protocol_session_id", "task_id", "stream_id"],
            "future": {
                "predicate": "provider-timestamp-greater-than-frame-host-receipt",
                "equality": "accepted",
                "failure": "provider.heartbeat-clock-invalid",
            },
            "backwards": {
                "predicate": "provider-timestamp-less-than-previous-accepted-in-scope",
                "equality": "accepted",
                "failure": "provider.heartbeat-clock-invalid",
            },
            "baseline_update": "after-full-control-validation-only",
            "rate_and_liveness_authority": "forbidden",
        },
        "arithmetic": "checked-u64-subtraction",
    },
    "liveness": {
        "interval_ns": 1_000_000_000,
        "timeout_ns": 5_000_000_000,
        "startup_grace_ns": 10_000_000_000,
        "activation": {"event": "task-start", "initial_state": "no-valid-ack"},
        "probe_schedule": {
            "first_probe": "at-task-start",
            "next_probe": "previous-probe-host-receipt-plus-interval",
        },
        "initial_ack_deadline": {
            "state": "no-valid-ack",
            "basis": "task-start-host-receipt",
            "deadline": "checked-add-task-start-host-receipt-and-startup-grace",
            "accepted_if": "ack-host-receipt-less-than-deadline",
            "boundary": "deadline-inclusive-failure",
            "failure": "provider.heartbeat-timeout",
        },
        "ack_deadline": {
            "enabled_if": "first-valid-ack-established",
            "basis": "latest-probe-host-receipt",
            "deadline": "checked-add-probe-host-receipt-and-timeout",
            "accepted_if": "ack-host-receipt-less-than-deadline",
            "boundary": "timeout-inclusive-failure",
            "failure": "provider.heartbeat-timeout",
        },
        "idle_timeout": {
            "enabled_if": "first-valid-ack-established",
            "basis": "last-valid-ack-host-receipt",
            "rejection": "now-minus-last-valid-ack-receipt-greater-than-or-equal-timeout",
            "boundary": "timeout-inclusive-failure",
            "failure": "provider.heartbeat-timeout",
        },
        "startup_boundary": "grace-inclusive",
        "time_authority": "host-injected-monotonic-clock",
        "no_valid_ack": "initial-ack-deadline-only",
        "last_valid_ack": "host-receipt-time-of-validated-ack",
        "timeout_arithmetic": "checked-u64-subtraction",
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
    "field_types": {
        "schema": "fixed-string",
        "task_id": "typed-id",
        "dependency_group_id": "typed-id",
        "progress_sequence": "uint64",
        "monotonic_time_ns": "uint64",
        "completed_units": "uint64",
        "total_units": "uint64",
    },
    "field_constraints": {
        "schema": "exact-control-schema",
        "task_id": "exact-task-binding",
        "dependency_group_id": "exact-open-dependency-group-binding",
        "progress_sequence": "contiguous-from-zero-per-task",
        "monotonic_time_ns": "provider-ordering-only-host-receipt-authority",
        "completed_units": "zero-through-total-inclusive",
        "total_units": "positive-and-constant-per-task",
    },
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
        "first_sample_deadline": {
            "state": "no-valid-sample",
            "basis": "task-start-host-receipt",
            "deadline": "checked-add-task-start-host-receipt-and-startup-grace",
            "accepted_if": "valid-sample-host-receipt-less-than-deadline",
            "boundary": "timeout-inclusive-failure",
            "terminal_bypass": "forbidden",
            "failure": "provider.progress-rate",
        },
        "sample_window_ns": 5_000_000_000,
        "maximum_sample_gap_ns": 10_000_000_000,
        "minimum_units_per_second": 1,
        "zero_delta_after_grace": "reject",
        "arithmetic": "overflow-safe-u128-cross-multiplication",
        "rate_formula": "delta_units*1000000000>=delta_time_ns*minimum_units_per_second",
        "sample_admission": "elapsed-at-least-window-or-terminal",
        "rate_check": "every-admitted-consecutive-sample-pair",
        "equality_at_deadline": "accepted",
        "sample_time_authority": "host-injected-monotonic-clock-at-valid-frame-receipt",
        "provider_timestamp_role": "ordering-only-not-rate-authority",
        "provider_timestamp_future": "provider-timestamp-greater-than-frame-host-receipt-is-provider.heartbeat-clock-invalid",
        "provider_timestamp_backwards": "provider-timestamp-less-than-previous-accepted-in-scope-is-provider.heartbeat-clock-invalid",
        "provider_timestamp_scope": ["protocol_session_id", "task_id", "dependency_group_id"],
        "provider_timestamp_baseline_update": "after-full-control-validation-only",
        "delta_time": "current-receipt-ns-minus-previous-receipt-ns",
        "overflow": "checked-u128-or-fail",
        "zero_elapsed": "provider.progress-rate",
        "maximum_gap": "provider.progress-rate",
        "terminal_total_required": True,
        "failure": "provider.progress-rate",
    },
    "credit": {
        "counts_toward_output_credit": False,
        "counts_toward_transport_budget": True,
    },
}

EXPECTED_RECEIPT = {
    "schema": "cxxlens.provider-spill-fsync-receipt.v1",
    "schema_path": "schemas/cxxlens_ng_provider_spill_fsync_receipt.schema.yaml",
    "exact_fields": [
        "schema",
        "provider_id",
        "protocol_session_id",
        "task_id",
        "stream_id",
        "highest_contiguous_acked_sequence",
        "staged_digest",
        "spill_digest",
        "total_bytes",
        "total_records",
        "fsync_sequence",
    ],
    "field_types": {
        "schema": "fixed-string",
        "provider_id": "typed-id",
        "protocol_session_id": "typed-id",
        "task_id": "typed-id",
        "stream_id": "uint64",
        "highest_contiguous_acked_sequence": "uint64",
        "staged_digest": "semantic-digest",
        "spill_digest": "semantic-digest",
        "total_bytes": "uint64",
        "total_records": "uint64",
        "fsync_sequence": "positive-uint64",
    },
    "field_constraints": {
        "schema": "exact-receipt-schema",
        "provider_id": "exact-provider-binding",
        "protocol_session_id": "exact-session-binding",
        "task_id": "exact-task-binding",
        "stream_id": "exact-stream-binding",
        "highest_contiguous_acked_sequence": "contiguous-durable-ack",
        "staged_digest": "exact-staged-prefix-digest",
        "spill_digest": "exact-canonical-spill-prefix-digest",
        "total_bytes": "checked-cumulative-byte-count",
        "total_records": "checked-cumulative-record-count",
        "fsync_sequence": "host-observed-strictly-increasing-positive-sequence",
    },
    "digest": {
        "domain": "cxxlens.provider-spill-prefix.v1",
        "projection": "canonical-tuple-of-binding-ack-staged-digest-spill-digest-and-cumulative-counters",
        "algorithm": "cxxlens-semantic-digest-v2",
        "encoding": "cxxlens-canonical-tuple-v1",
    },
    "authority": "host-observed-private-spill-port-result",
}

EXPECTED_RESUME_DIGEST_GRAMMAR = {
    "manifest_content_digest": {
        "fields": ["provider_binary_digest", "provider_semantic_contract_digest"],
        "spelling": "sha256:<64 lowercase hex>",
        "authority": MANIFEST_SCHEMA.as_posix(),
        "conversion": "forbidden",
    },
    "semantic_digest": {
        "fields": [
            "task_input_digest",
            "normalized_invocation_digest",
            "toolchain_digest",
            "environment_digest",
            "sandbox_policy_digest",
            "staged_digest",
        ],
        "spelling": "semantic-v2:sha256:<64 lowercase hex>",
        "authority": "docs/design/cxxlens_next_generation_integrated_design_ja.md#5-identity-and-canonical-encoding",
    },
    "token_digest": {
        "field": "token_digest",
        "spelling": "semantic-v2:sha256:<64 lowercase hex>",
        "projection": "all-fields-except-token_digest",
        "algorithm": "cxxlens-semantic-digest-v2",
        "encoding": "cxxlens-canonical-tuple-v1",
        "identity_fields_are_exact_strings": True,
    },
    "namespace_conversion": "forbidden",
    "dual_namespace_acceptance": "forbidden",
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
        "task_input_digest",
        "normalized_invocation_digest",
        "toolchain_digest",
        "environment_digest",
        "sandbox_policy_digest",
        "dependency_group_id",
        "atomic_output_group_id",
        "batch_id",
        "stream_id",
        "highest_contiguous_acked_sequence",
        "staged_digest",
        "token_generation",
        "token_digest",
    ],
    "field_types": {
        "schema": "fixed-string",
        "kind": "enum",
        "provider_id": "typed-id",
        "provider_version": "semantic-version",
        "provider_binary_digest": "manifest-content-digest",
        "provider_semantic_contract_digest": "manifest-content-digest",
        "protocol_session_id": "typed-id",
        "task_id": "typed-id",
        "task_input_digest": "semantic-digest",
        "normalized_invocation_digest": "semantic-digest",
        "toolchain_digest": "semantic-digest",
        "environment_digest": "semantic-digest",
        "sandbox_policy_digest": "semantic-digest",
        "dependency_group_id": "typed-id",
        "atomic_output_group_id": "typed-id",
        "batch_id": "typed-id",
        "stream_id": "uint64",
        "highest_contiguous_acked_sequence": "uint64",
        "staged_digest": "semantic-digest",
        "token_generation": "uint64",
        "token_digest": "semantic-digest",
    },
    "digest_grammar": EXPECTED_RESUME_DIGEST_GRAMMAR,
    "field_constraints": {
        "schema": "exact-control-schema",
        "kind": "request-accepted-or-rejected",
        "provider_id": "exact-provider-binding",
        "provider_version": "exact-negotiated-provider-version",
        "provider_binary_digest": "exact-manifest-content-digest",
        "provider_semantic_contract_digest": "exact-selected-manifest-content-digest",
        "protocol_session_id": "exact-session-binding",
        "task_id": "exact-task-binding",
        "task_input_digest": "exact-task-input-binding",
        "normalized_invocation_digest": "exact-invocation-binding",
        "toolchain_digest": "exact-toolchain-binding",
        "environment_digest": "exact-environment-binding",
        "sandbox_policy_digest": "exact-sandbox-policy-binding",
        "dependency_group_id": "exact-open-dependency-group-binding",
        "atomic_output_group_id": "exact-atomic-output-group-binding",
        "batch_id": "exact-batch-binding",
        "stream_id": "exact-stream-binding",
        "highest_contiguous_acked_sequence": "contiguous-durable-ack",
        "staged_digest": "exact-staged-prefix-digest",
        "token_generation": "strictly-increasing-per-task",
        "token_digest": "canonical-projection-digest",
    },
    "kinds": ["request", "accepted", "rejected"],
    "token_digest": {
        "domain": "cxxlens.provider-resume-token.v1",
        "projection": "all-fields-except-token_digest",
        "algorithm": "cxxlens-semantic-digest-v2",
        "encoding": "cxxlens-canonical-tuple-v1",
    },
    "durability": {
        "prerequisite": "fsync-confirmed-spill-ack",
        "volatile_ack_is_not_authority": True,
        "generation": "strictly-increasing",
        "publication_order": "append-spill-fsync-construct-token-publish-token",
        "receipt": EXPECTED_RECEIPT,
        "atomic_persistence": "temp-write-fsync-rename-parent-fsync",
    },
    "replay": {
        "start": "highest_contiguous_acked_sequence-plus-one",
        "input_identity": "exact-original-task-input-and-selection-binding",
        "output_digest": "exact-equal-to-original-sealed-prefix",
        "duplicate_output": "reject",
        "gap_or_reorder": "provider.resume-replay-invalid",
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
    "binding": ["provider_id", "protocol_session_id", "task_id"],
    "framing": {
        "encoding": "deterministic-cbor-record",
        "length_prefix": "uint64-big-endian",
        "record_bytes": "header-plus-payload-and-framing-counted",
        "metadata_overhead": "included-in-total-quota",
    },
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
    "field_types": {
        "schema": "fixed-string",
        "record_ordinal": "uint64",
        "task_id": "typed-id",
        "dependency_group_id": "typed-id",
        "atomic_output_group_id": "typed-id",
        "batch_id": "typed-id",
        "stream_id": "uint64",
        "sequence": "uint64",
        "payload_bytes": "bytes",
        "payload_digest": "semantic-digest",
        "record_digest": "semantic-digest",
    },
    "field_constraints": {
        "schema": "exact-record-schema",
        "record_ordinal": "contiguous-from-zero",
        "task_id": "exact-task-binding",
        "dependency_group_id": "exact-open-dependency-group-binding",
        "atomic_output_group_id": "exact-atomic-output-group-binding",
        "batch_id": "exact-batch-binding",
        "stream_id": "exact-stream-binding",
        "sequence": "contiguous-wire-sequence",
        "payload_bytes": "exact-byte-count-before-allocation",
        "payload_digest": "digest-of-exact-payload-bytes",
        "record_digest": "digest-of-canonical-record-without-record-digest",
    },
        "digest": {
            "domain": "cxxlens.provider-spill-record.v1",
        "projection": "all-fields-except-record_digest",
        "algorithm": "cxxlens-semantic-digest-v2",
        "encoding": "cxxlens-canonical-tuple-v1",
        },
    },
    "limits": {
        "maximum_record_bytes": 16_777_216,
        "maximum_total_bytes": 67_108_864,
        "maximum_records": 65_536,
        "validate_before_allocation": True,
        "quota_arithmetic": "checked-u128-including-framing-and-metadata",
    },
    "durability": {
        "append": "exact-byte-count-and-digest",
        "acknowledgement": "fsync-before-resume-token",
        "ordering": "contiguous-record-ordinal",
        "allocation": "validate-header-length-and-digest-before-payload-allocation",
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
    "initial_state": "running",
    "terminal_states": ["completed", "failed"],
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
    "transition_matrix": {
        "running": {
            "heartbeat-timeout": "heartbeat-timeout",
            "progress-rate-failure": "progress-rate-failure",
            "cancel-requested": "cancel-requested",
            "worker-exit": "worker-killed",
            "output-sealed": "completed",
            "invalid-heartbeat-clock": "failed",
        },
        "heartbeat-timeout": {"worker-kill-confirmed": "worker-killed"},
        "progress-rate-failure": {"worker-kill-confirmed": "worker-killed"},
        "cancel-requested": {
            "cancel-acknowledged": "failed",
            "cancel-timeout": "worker-killed",
        },
        "worker-killed": {
            "durable-token-valid": "resume-replay",
            "durable-token-invalid": "failed",
        },
        "resume-replay": {
            "replay-valid": "resumed",
            "replay-invalid": "failed",
        },
        "resumed": {"output-sealed": "completed", "output-invalid": "failed"},
        "completed": {},
        "failed": {},
    },
    "publication": {
        "prior_published_snapshot": "unchanged-on-failure",
        "current_dependency_group": "rollback",
        "prior_adopted_groups": "retain-only-if-predeclared-partial-policy",
        "transaction": "sealed-output-before-snapshot-cas",
        "adoption_authority": "shared-validator-sealed-output-only",
        "replay_duplicate": "reject",
        "failure_effect": "prior-published-snapshot-unchanged",
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
    "report_schema": "schemas/cxxlens_ng_provider_ng1_qualification_report.schema.yaml",
    "checker": "tools/quality/check_ng_provider_ng1_qualification.py",
    "vectors": "schemas/cxxlens_ng_provider_ng1_conformance_vectors.yaml",
    "exact_binding": [
        "revision",
        "tree",
        "provider_binary_digest",
        "provider_binary_digest_source",
        "provider_semantic_contract_digest",
        "provider_semantic_contract_digest_source",
        "protocol_minor",
        "protocol_contract_digest",
        "hardening_contract_digest",
        "hardening_contract_schema_digest",
        "report_schema_digest",
        "vectors_digest",
        "vectors_schema_digest",
    ],
    "required_profiles": ["static", "shared"],
    "required_cases": [
        "positive-heartbeat-and-progress",
        "manifest-content-digest-binding",
        "stale-heartbeat",
        "heartbeat-timeout",
        "progress-sample-timeout",
        "zero-progress-after-grace",
        "stale-resume-token",
        "foreign-resume-token",
        "semantic-v2-provider-identity-rejected",
        "content-digest-semantic-field-rejected",
        "spill-corruption",
        "worker-crash-recovery",
        "worker-hang-recovery",
        "cancellation-recovery",
        "permutation-replay",
        "long-run-fault",
    ],
    "required_case_outcomes": {
        "positive-heartbeat-and-progress": "accepted",
        "manifest-content-digest-binding": "accepted",
        "stale-heartbeat": "provider.heartbeat-timeout",
        "heartbeat-timeout": "provider.heartbeat-timeout",
        "progress-sample-timeout": "provider.progress-rate",
        "zero-progress-after-grace": "provider.progress-rate",
        "stale-resume-token": "provider.resume-token-stale",
        "foreign-resume-token": "provider.resume-token-stale",
        "semantic-v2-provider-identity-rejected": "provider.resume-token-stale",
        "content-digest-semantic-field-rejected": "provider.resume-token-stale",
        "spill-corruption": "provider.spill-corrupt",
        "worker-crash-recovery": "replay-from-ack-plus-one-or-fail-closed",
        "worker-hang-recovery": "replay-from-durable-ack-or-fail-closed",
        "cancellation-recovery": "rollback-current-group-and-fail-closed",
        "permutation-replay": "provider.resume-replay-invalid",
        "long-run-fault": "accepted-only-with-exact-certificate-or-stable-failure",
    },
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
    vectors = load_yaml(root / VECTORS)
    schema_validate(vectors, load_yaml(root / VECTORS_SCHEMA), "NG1 conformance vectors")
    manifest_schema = load_yaml(root / MANIFEST_SCHEMA)
    expect(
        manifest_schema.get("$defs", {}).get("digest", {}).get("pattern"),
        r"^sha256:[0-9a-f]{64}$",
        "manifest_schema.digest.pattern",
    )

    expect(hardening["schema"], "cxxlens.provider-ng1-hardening.v1", "schema")
    expect(hardening["document_version"], "1.0.0", "document_version")
    expect(hardening["maturity"], "proposed", "maturity")
    expect(
        hardening["authority"],
        {
            "design": "docs/design/cxxlens_next_generation_integrated_design_ja.md",
            "protocol": "schemas/cxxlens_ng_provider_protocol.yaml",
            "runtime": "schemas/cxxlens_ng_provider_runtime_contract.yaml",
            "decision_adr": "docs/design/adr/0099-provider-ng1-hardening.md",
            "decision_issue": "#233",
            "implementation_issue": "#183",
            "digest_grammar_adr": DIGEST_GRAMMAR_ADR.as_posix(),
            "digest_grammar_issue": DIGEST_GRAMMAR_ISSUE,
            "owner": "steward.ng-provider-runtime",
            "spill_fsync_receipt_schema": SPILL_FSYNC_RECEIPT_SCHEMA.as_posix(),
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
    expect(hardening["lifecycle"], EXPECTED_LIFECYCLE, "lifecycle")
    expect(hardening["heartbeat"], EXPECTED_HEARTBEAT, "heartbeat")
    expect(hardening["progress"], EXPECTED_PROGRESS, "progress")
    expect(hardening["resume"], EXPECTED_RESUME, "resume")
    receipt_schema = load_yaml(root / SPILL_FSYNC_RECEIPT_SCHEMA)
    receipt_fields = EXPECTED_RECEIPT["exact_fields"]
    expect(receipt_schema.get("type"), "object", "spill_fsync_receipt_schema.type")
    expect(
        receipt_schema.get("additionalProperties"),
        False,
        "spill_fsync_receipt_schema.additionalProperties",
    )
    expect(receipt_schema.get("required"), receipt_fields, "spill_fsync_receipt_schema.required")
    expect(
        sorted(receipt_schema.get("properties", {})),
        sorted(receipt_fields),
        "spill_fsync_receipt_schema.properties",
    )
    receipt_digest = "semantic-v2:sha256:" + "0" * 64
    receipt_instance = {
        "schema": EXPECTED_RECEIPT["schema"],
        "provider_id": "provider:test",
        "protocol_session_id": "session:test",
        "task_id": "task:test",
        "stream_id": 1,
        "highest_contiguous_acked_sequence": 0,
        "staged_digest": receipt_digest,
        "spill_digest": receipt_digest,
        "total_bytes": 0,
        "total_records": 0,
        "fsync_sequence": 1,
    }
    schema_validate(receipt_instance, receipt_schema, "spill fsync receipt")
    receipt_validator = jsonschema.Draft202012Validator(receipt_schema)
    if receipt_validator.is_valid({**receipt_instance, "unexpected": True}):
        fail("spill_fsync_receipt_schema", "unexpected properties are accepted")
    if receipt_validator.is_valid({**receipt_instance, "fsync_sequence": 0}):
        fail("spill_fsync_receipt_schema", "zero fsync sequence is accepted")
    if receipt_validator.is_valid(
        {
            **receipt_instance,
            "staged_digest": "sha256:" + "0" * 64,
        }
    ):
        fail("spill_fsync_receipt_schema", "legacy sha256 digest is accepted")
    expect(hardening["spill"], EXPECTED_SPILL, "spill")
    expect(hardening["recovery"], EXPECTED_RECOVERY, "recovery")
    expect(hardening["stable_failures"], EXPECTED_STABLE_FAILURES, "stable_failures")
    expect(hardening["qualification"], EXPECTED_QUALIFICATION, "qualification")
    qualification_schema = load_yaml(root / QUALIFICATION_REPORT_SCHEMA)
    try:
        jsonschema.Draft202012Validator.check_schema(qualification_schema)
    except jsonschema.SchemaError as error:
        fail("qualification.report_schema", f"invalid schema: {error.message}")
    expect(
        qualification_schema.get("$id"),
        "https://cxxlens.dev/schemas/cxxlens.provider-ng1-qualification.v1",
        "qualification.report_schema.$id",
    )
    qualification_cases = []
    for case_id in hardening["qualification"]["required_cases"]:
        outcome = hardening["qualification"]["required_case_outcomes"][case_id]
        if outcome == "accepted":
            qualification_cases.append({"id": case_id, "decision": "accepted"})
        elif outcome.startswith("provider."):
            qualification_cases.append(
                {"id": case_id, "decision": "rejected", "reason_code": outcome}
            )
        else:
            qualification_cases.append(
                {"id": case_id, "decision": "recovery", "outcome": outcome}
            )
    qualification_certificate = {
        "schema": "cxxlens.provider-ng1-qualification.v1",
        "document_version": "1.0.0",
        "authority": {
            "contract": CONTRACT.as_posix(),
            "vectors": VECTORS.as_posix(),
            "decision_issue": "#233",
            "implementation_issue": "#183",
            "digest_grammar_adr": DIGEST_GRAMMAR_ADR.as_posix(),
            "digest_grammar_issue": DIGEST_GRAMMAR_ISSUE,
        },
        "binding": {
            "revision": "0" * 40,
            "tree": "1" * 40,
            "provider_binary_digest": "sha256:" + "2" * 64,
            "provider_binary_digest_source": "host-measured-executable-bytes",
            "provider_semantic_contract_digest": "sha256:" + "3" * 64,
            "provider_semantic_contract_digest_source": "selected-contract-digest",
            "protocol_minor": 1,
            "protocol_contract_digest": document_digest(
                protocol if protocol is not None else load_yaml(root / PROTOCOL)
            ),
            "hardening_contract_digest": document_digest(hardening),
            "hardening_contract_schema_digest": document_digest(
                load_yaml(root / CONTRACT_SCHEMA)
            ),
            "report_schema_digest": document_digest(qualification_schema),
            "vectors_digest": document_digest(vectors),
            "vectors_schema_digest": document_digest(load_yaml(root / VECTORS_SCHEMA)),
        },
        "profiles": [
            {
                "profile": profile,
                "status": "green",
                "evidence_digest": "sha256:" + ("4" if profile == "static" else "5") * 64,
                "cases": qualification_cases,
            }
            for profile in ("static", "shared")
        ],
        "status": "green",
    }
    schema_validate(
        qualification_certificate,
        qualification_schema,
        "NG1 qualification report",
    )
    expect(
        vectors["authority"],
        {
            "contract": CONTRACT.as_posix(),
            "decision_issue": "#233",
            "implementation_issue": "#183",
            "digest_grammar_adr": DIGEST_GRAMMAR_ADR.as_posix(),
            "digest_grammar_issue": DIGEST_GRAMMAR_ISSUE,
            "binding": {
                "state": "authority-only-unbound",
                "revision": None,
                "tree": None,
                "provider_binary_digest": None,
                "provider_semantic_contract_digest": None,
                "protocol_minor": 1,
                "hardening_contract_digest": None,
            },
        },
        "qualification.vectors.authority",
    )
    expect(
        vectors["authority"].get("binding"),
        {
            "state": "authority-only-unbound",
            "revision": None,
            "tree": None,
            "provider_binary_digest": None,
            "provider_semantic_contract_digest": None,
            "protocol_minor": 1,
            "hardening_contract_digest": None,
        },
        "qualification.vectors.authority.binding",
    )
    expected_cases = set(hardening["qualification"]["required_cases"])
    vector_ids = [vector["id"] for vector in vectors["vectors"]]
    if len(vector_ids) != len(set(vector_ids)):
        fail("qualification.vectors", "vector IDs must be unique")
    if len(vector_ids) != len(expected_cases):
        fail(
            "qualification.vectors",
            f"expected exactly {len(expected_cases)} vectors, got {len(vector_ids)}",
        )
    actual_cases = set(vector_ids)
    if actual_cases != expected_cases:
        fail("qualification.vectors", f"case set differs: expected={sorted(expected_cases)}, got={sorted(actual_cases)}")
    for vector in vectors["vectors"]:
        expected_outcome = hardening["qualification"]["required_case_outcomes"][vector["id"]]
        decision = vector["expected"]["decision"]
        expected_decision = (
            "accepted"
            if expected_outcome == "accepted"
            else "rejected"
            if vector["class"] == "negative"
            else "recovery"
        )
        expect(decision, expected_decision, f"qualification.vectors.{vector['id']}.decision")
        if vector["class"] == "positive":
            expect(vector["expected"], {"decision": "accepted"}, f"qualification.vectors.{vector['id']}.expected")
        elif vector["class"] == "negative":
            reason = vector["expected"].get("reason_code")
            if reason is None:
                fail(f"qualification.vectors.{vector['id']}", "negative vector lacks stable reason code")
            expect(reason, expected_outcome, f"qualification.vectors.{vector['id']}.reason_code")
        else:
            expect(vector["expected"].get("outcome"), expected_outcome, f"qualification.vectors.{vector['id']}.outcome")

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
        hardening["heartbeat"]["clock"]["provider_timestamp"]["backwards"]["failure"],
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
    protocol_authority = protocol.get("authority")
    if not isinstance(protocol_authority, dict):
        fail("protocol.authority", "authority mapping is missing")
    expect(
        protocol_authority.get("ng1_resume_digest_grammar_adr"),
        DIGEST_GRAMMAR_ADR.as_posix(),
        "protocol.authority.ng1_resume_digest_grammar_adr",
    )
    expect(
        protocol_authority.get("ng1_resume_digest_grammar_issue"),
        DIGEST_GRAMMAR_ISSUE,
        "protocol.authority.ng1_resume_digest_grammar_issue",
    )
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
    structured = protocol.get("structured_control_metadata", {})
    single_records = structured.get("single_records", {})
    expect(single_records.get("heartbeat"), EXPECTED_HEARTBEAT["exact_fields"], "protocol.structured_control_metadata.heartbeat")
    expect(single_records.get("progress"), EXPECTED_PROGRESS["exact_fields"], "protocol.structured_control_metadata.progress")
    expect(single_records.get("resume"), EXPECTED_RESUME["exact_fields"], "protocol.structured_control_metadata.resume")
    expect(structured.get("ng1_hardening_contract"), CONTRACT.as_posix(), "protocol.structured_control_metadata.ng1_hardening_contract")
    expected_unsigned = [
        "schema_negotiate.protocol_minor", "input_descriptor.total_bytes", "input_descriptor.chunk_bytes", "input_descriptor.chunk_count",
        "input_chunk.chunk_index", "input_chunk.offset", "input_chunk.byte_count", "credit.bytes", "credit.frames",
        "heartbeat.stream_id", "heartbeat.heartbeat_sequence", "heartbeat.monotonic_time_ns", "heartbeat.highest_contiguous_acked_sequence",
        "progress.progress_sequence", "progress.monotonic_time_ns", "progress.completed_units", "progress.total_units",
        "resume.stream_id", "resume.highest_contiguous_acked_sequence", "resume.token_generation",
    ]
    expect(structured.get("unsigned_fields"), expected_unsigned, "protocol.structured_control_metadata.unsigned_fields")
    terminal_reasons = protocol.get("failures", {}).get("terminal_reasons", [])
    if any(code not in terminal_reasons for code in EXPECTED_STABLE_FAILURES):
        fail("protocol.failures", "NG1 stable failures are not registered")
    execution_report = load_yaml(root / EXECUTION_REPORT_SCHEMA)
    terminal_enum = execution_report["properties"]["terminal"]["enum"]
    runtime = load_yaml(root / RUNTIME_CONTRACT)
    if hardening["maturity"] == "accepted":
        if any(code not in terminal_enum for code in EXPECTED_STABLE_FAILURES):
            fail("execution_report", "accepted NG1 failures are not reportable")
        if any(code not in runtime["terminal"]["stable"] for code in EXPECTED_STABLE_FAILURES):
            fail("runtime", "NG1 stable failures are not runtime terminals")
    else:
        if any(code in terminal_enum for code in EXPECTED_STABLE_FAILURES):
            fail("execution_report", "proposed NG1 failures must remain outside the active report enum")
        if set(runtime["terminal"].get("reserved_for_ng1", [])) != set(EXPECTED_STABLE_FAILURES):
            fail("runtime", "NG1 stable failures are not reserved in runtime authority")
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
