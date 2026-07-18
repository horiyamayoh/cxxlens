#!/usr/bin/env python3
"""Validate the NG process provider runtime and Clang 22 worker contract."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from typing import Any

import jsonschema
import yaml


class ContractError(ValueError):
    pass


def load(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ContractError(f"expected mapping: {path}")
    return value


def validate(root: pathlib.Path) -> None:
    contract = load(root / "schemas/cxxlens_ng_provider_runtime_contract.yaml")
    contract_schema = load(
        root / "schemas/cxxlens_ng_provider_runtime_contract.schema.yaml"
    )
    report_schema = load(
        root / "schemas/cxxlens_ng_provider_execution_report.schema.yaml"
    )
    jsonschema.Draft202012Validator.check_schema(contract_schema)
    jsonschema.Draft202012Validator(contract_schema).validate(contract)
    budget = contract["runtime"]["budget"]
    if budget != {
        "logical_surface_independent": {
            "output_bytes": "sum-control-and-payload-for-non-lifecycle-provider-output-frames",
            "rows": "task-global-sum-of-validated-batch-end-row-count",
            "diagnostics": "task-global-decoded-unresolved-record-count",
            "validator": "shared-logical-process-production-validator",
            "exhaustion": "provider.output-limit-without-success-adoption",
        },
        "process_isolation_only": {
            "wall_ms": "monotonic-deadline-process-group-kill",
            "cpu_ms": "rlimit-cpu-rounded-up-to-seconds",
            "address_space_bytes": "rlimit-as-not-rss",
            "transport_bytes": "combined-stdout-stderr-drain-and-rlimit-fsize",
            "open_files": "rlimit-nofile",
            "subprocesses": "rlimit-nproc",
        },
        "wire_credit": "independent-from-logical-and-transport-byte-budgets",
        "direct_run_worker": "trusted-callback-logical-limits-and-cooperative-cancellation-only",
        "unsupported_created_file_count": "omitted-not-claimed",
    }:
        raise ContractError("provider execution budget surface contract is not exact")
    selection = contract["selection"]
    if selection["candidate_identity"] != {
        "schema": "cxxlens.provider-candidate.v1",
        "digest": "cxxlens-semantic-digest-v2",
        "fields": [
            "full-canonical-manifest",
            "ordered-executable-argv",
            "authoritative-path",
            "trust-verdict",
            "certification-verdict",
            "canonical-certified-qualifications",
            "canonical-sandbox-report",
            "validation-error",
        ],
        "source_binding": "decision-source-plus-candidate-digest",
    }:
        raise ContractError("provider candidate identity projection is not exact")
    if selection["candidate_order"] != [
        "discovery-source-precedence",
        "provider-id",
        "provider-version",
        "binary-digest",
        "candidate-digest",
    ]:
        raise ContractError("provider candidate order is not a strict canonical total order")
    executable_binding = contract["runtime"]["launch"]["executable_binding"]
    if executable_binding != {
        "source_resolution": "single-open-or-working-directory-openat",
        "measured_image": "copied-sealed-executable-memfd",
        "digest_subject": "exact-sealed-image-bytes",
        "execution": "execveat-verified-fd-at-empty-path",
        "path_reopen_after_measurement": "forbidden",
        "rename_symlink_in_place_mutation": "verified-image-or-reject",
    }:
        raise ContractError("provider executable measurement is not bound to executed bytes")
    if contract["runtime"]["sandbox"]["evidence_digest_v3"] != [
        "resolved-policy-canonical-form",
        "recomputed-policy-digest",
        "measured-executable-digest",
        "achieved-assurance",
        "invocation-budget-limits",
        "exact-applied-mechanisms",
    ]:
        raise ContractError("sandbox evidence v3 omits measured executable identity")
    jsonschema.Draft202012Validator.check_schema(report_schema)
    fallback_identity = load(
        root / "schemas/cxxlens_ng_clang22_fallback_identity.yaml"
    )
    fallback_identity_schema = load(
        root / "schemas/cxxlens_ng_clang22_fallback_identity.schema.yaml"
    )
    jsonschema.Draft202012Validator.check_schema(fallback_identity_schema)
    jsonschema.Draft202012Validator(fallback_identity_schema).validate(fallback_identity)
    expected_fallback_vectors = {
        "normal-usr-golden",
        "function-overloads",
        "special-members-and-operators",
        "template-primary-specializations",
        "constrained-overloads",
        "same-signature-redeclaration",
        "definition-preference",
        "cross-tu-order-permutation",
        "unanchored-opaque",
        "toolchain-change",
    }
    if {vector["id"] for vector in fallback_identity["vectors"]} != expected_fallback_vectors:
        raise ContractError("Clang 22 fallback identity conformance vectors are incomplete")

    sample_report = {
        "schema": "cxxlens.provider-execution-report.v1",
        "terminal": "provider.binary-identity-mismatch",
        "provider": {
            "id": "cxxlens.clang22.reference",
            "version": "1.0.0",
            "binary_digest": "sha256:" + "a" * 64,
            "semantic_contract_digest": "sha256:" + "b" * 64,
        },
        "input_binding": {
            "task": "sha256:" + "c" * 64,
            "invocation": "sha256:" + "d" * 64,
            "toolchain": "sha256:" + "e" * 64,
            "environment": "sha256:" + "f" * 64,
        },
        "measured_executable_digest": "sha256:" + "a" * 64,
        "sandbox": {
            "platform": "linux-glibc",
            "mechanisms": ["no-shell-argv-exec"],
            "achieved": "enforced",
            "policy_digest": "sha256:" + "1" * 64,
            "evidence_digest": "sha256:" + "2" * 64,
        },
        "frames": {
            "count": 0,
            "last_sequence": None,
            "transcript_digest": "sha256:" + "3" * 64,
        },
        "diagnostics": [],
        "semantic_digest": "sha256:" + "4" * 64,
    }
    jsonschema.Draft202012Validator(report_schema).validate(sample_report)
    stable_terminals = set(contract["terminal"]["stable"])
    schema_terminals = set(report_schema["properties"]["terminal"]["enum"])
    if schema_terminals != stable_terminals:
        raise ContractError("execution report terminal enum diverges from runtime registry")
    invalid_report = dict(sample_report)
    invalid_report["terminal"] = "provider.unknown-reason"
    if not list(jsonschema.Draft202012Validator(report_schema).iter_errors(invalid_report)):
        raise ContractError("execution report schema accepted an unregistered terminal")
    runtime_source = (root / "src/sdk/provider_runtime.cpp").read_text(encoding="utf-8")
    terminal_block = runtime_source.split(
        "constexpr std::array stable_terminal_reasons{", 1
    )
    if len(terminal_block) != 2:
        raise ContractError("runtime stable terminal registry is missing")
    cpp_terminals = set(
        re.findall(r'std::string_view\{"((?:provider|security)\.[a-z0-9-]+)"\}',
                   terminal_block[1].split("};", 1)[0])
    )
    if cpp_terminals != stable_terminals:
        raise ContractError("C++ terminal registry diverges from runtime authority")

    required = {
        "include/cxxlens/sdk/provider.hpp": (
            "class process_provider_runtime",
            "class provider_process_port",
            "select_provider",
            "expected_binary_digest",
            "provider_fallback_policy",
            "certified_qualifications",
            "candidate_digest",
            "class provider_selection",
            "selected_candidate() const",
            "authority_request() const",
        ),
        "src/runtime/provider_process_adapter.cpp": (
            "provider.binary-identity-mismatch",
            "make_verified_executable",
            "working-directory-open",
            "MFD_ALLOW_SEALING",
            "F_SEAL_WRITE",
            "SYS_execveat",
            "AT_EMPTY_PATH",
            "resolve_sandbox_policy",
            "sandbox_evidence_digest",
            "security.sandbox-insufficient",
            "CLOSE_RANGE_UNSHARE",
            "AUDIT_ARCH_X86_64",
            "SECCOMP_RET_KILL_PROCESS",
            "close_inherited_descriptors",
        ),
        "src/sdk/provider_runtime.cpp": (
            "provider.timeout",
            "provider.cancelled",
            "provider.binary-identity-mismatch",
            "provider.protocol-state-invalid",
            "provider.credit-exceeded",
            "provider.batch-invalid",
            "provider.required-feature-missing",
            "provider.protocol-minor-mismatch",
            "request.selection.validate()",
            "effective_sandbox",
            "security.sandbox-policy-mismatch",
            "validate_provider_transcript",
            "allowed_failure_terminal",
            "validated_success_",
            "decode_task_accepted_metadata",
            "decode_batch_begin_metadata",
            "decode_coverage_metadata",
            "decode_unresolved_metadata",
            "decode_evidence_metadata",
            "decode_task_complete_metadata",
            "decode_task_failed_metadata",
            "encode_host_transcript",
            "CXXLENS_PROVIDER_MANIFEST",
            "CXXLENS_PROVIDER_TASK_INPUT_DIGEST",
        ),
        "src/sdk/provider_validation_internal.hpp": (
            "transcript_validation_request",
            "validate_provider_transcript",
        ),
        "src/sdk/provider.cpp": (
            "provider.fallback-policy-mismatch",
            "cxxlens.provider-fallback-policy.v1",
            "provider.unknown-required-extension",
            "provider.invalid-frame-flags",
            "provider.unsupported-compression",
            "encode_control_text",
            "encode_task_accepted_metadata",
            "encode_batch_begin_metadata",
            "encode_coverage_metadata",
            "encode_unresolved_metadata",
            "encode_evidence_metadata",
            "decode_schema_negotiate_metadata",
            "decode_open_task_metadata",
            "decode_credit_metadata",
            "decode_close_metadata",
            "validate_host_transcript",
            "provider.host-transcript-invalid",
            "value.summary",
            "encode_column_chunk",
            "decode_columnar_batch_end",
            "valid_utf8",
            "control-utf8",
            "no-shell-argv-exec",
            "network-syscall-deny",
            "inherited-fd-close-range",
            "seccomp-audit-arch",
            "candidate_identity_digest",
            "duplicate-canonical-candidate",
        ),
        "src/llvm/clang22/provider_sdk.cpp": (
            "getExpansionRange",
            "getImmediateExpansionRange",
            "native.source-origin-invalid",
        ),
        "src/llvm/clang22/provider_worker.cpp": (
            "frontend.clang22.entity_observation",
            "cc.call_site",
            "ignored-or-gcc-specific-option",
            "derive_domain_identity",
            "call.direct_callee",
            "symbol.is_definition",
            "symbol.is_canonical_declaration",
            "provider.entity-redeclaration-incompatible",
            "indirect_member_pointer",
            "virtual_member",
            "provider.call-kind-target-inconsistent",
            "source_origin_chain",
            "observation_dedup_key",
            "const auto key = observation_dedup_key(observation);",
            "call_occurrence_class",
            "ordered_observations",
            "cxxlens.clang22.task.v2",
            "source_snapshot",
            "clang22.declaration-fallback.v2",
            "make_declaration_identity",
            "canonical_source_anchor",
            "symbol.identity_confidence",
            "call.direct_callee_identity_confidence",
            "provider.declaration-identity-unresolved",
            "validate_host_transcript",
            "CXXLENS_PROVIDER_PROTOCOL_MINOR",
        ),
        "CMakeLists.txt": (
            "cxxlens-clang-worker-22",
            "cxxlens_ng_provider_runtime_contract.yaml",
        ),
        "tests/fixtures/provider_process_fixture.cpp": (
            "validate_host_transcript",
            "CXXLENS_PROVIDER_MANIFEST",
            "CXXLENS_PROVIDER_PROTOCOL_MINOR",
        ),
        "tests/unit/sdk/provider_runtime_test.cpp": (
            "check_verified_executable_binding",
            "verified-old",
            "measured and executed as one image",
        ),
        "tests/adapter/clang22/provider_normalizer_test.cpp": (
            "one TWICE macro expansion collapsed its two same-callee calls",
            "two TWICE macro expansions did not preserve four call occurrences",
            "same-expansion macro call IDs depend on observation input order",
        ),
    }
    for relative, markers in required.items():
        path = root / relative
        if not path.is_file():
            raise ContractError(f"missing provider runtime evidence: {relative}")
        text = path.read_text(encoding="utf-8")
        missing = [marker for marker in markers if marker not in text]
        if missing:
            raise ContractError(f"{relative} lacks markers: {missing}")

    worker = (root / "src/llvm/clang22/provider_worker.cpp").read_text(encoding="utf-8")
    for forbidden in ("call.direct_callee_anchor", "builder.set<relation::anchor>"):
        if forbidden in worker:
            raise ContractError(
                f"Clang normalizer leaks occurrence anchor into semantic entity identity: {forbidden}"
            )

    catalog = load(root / "schemas/cxxlens_ng_public_api_catalog.yaml")
    entries = {entry["id"]: entry for entry in catalog["entries"]}
    runtime = entries.get("public.provider-runtime")
    if runtime is None or runtime["status"] != "implemented" or runtime["owner_issue"] != "#151":
        raise ContractError("public.provider-runtime is not an implemented Issue #151 entry")
    required_budget_invariants = {
        "shared-validator-enforces-logical-output-bytes-rows-and-diagnostic-records-before-success-adoption",
        "process-port-separately-enforces-wall-cpu-address-space-transport-open-file-and-subprocess-resources",
        "address-space-budget-is-rlimit-as-not-rss",
        "created-file-count-is-not-claimed",
    }
    if not required_budget_invariants.issubset(runtime.get("invariants", [])):
        raise ContractError("provider runtime catalog omits Issue #123 budget invariants")
    if "docs/design/adr/0087-provider-budget-surface-parity.md" not in runtime.get(
        "implementation_evidence", []
    ):
        raise ContractError("provider runtime catalog omits Issue #123 evidence")

    header = (root / "include/cxxlens/sdk/provider.hpp").read_text(encoding="utf-8")
    for marker in (
        "address_space_bytes",
        "transport_bytes",
        "result<void> validate() const",
        "set_output_budget",
        "counts_toward_output_budget",
        "Resource preemption is provided only",
    ):
        if marker not in header:
            raise ContractError(f"provider budget public marker is missing: {marker}")
    if "rss_bytes" in header or "created_files" in header:
        raise ContractError("provider budget still claims RSS or created-file count enforcement")
    process_source = (root / "src/runtime/provider_process_adapter.cpp").read_text(
        encoding="utf-8"
    )
    for marker in (
        "RLIMIT_AS, invocation.budget.address_space_bytes",
        "RLIMIT_FSIZE, invocation.budget.transport_bytes",
        "invocation.budget.open_files",
        "invocation.budget.subprocesses",
        "invocation.budget.wall_ms",
    ):
        if marker not in process_source:
            raise ContractError(f"provider process resource marker is missing: {marker}")
    validator_source = (root / "src/sdk/provider_runtime.cpp").read_text(encoding="utf-8")
    for marker in (
        "logical_output_bytes",
        "batch_terminal->row_count > request.budget->rows - output_rows",
        "records->size() > request.budget->diagnostics - diagnostics",
        'return fail("provider.output-limit", request.task_id, "output_bytes")',
    ):
        if marker not in validator_source:
            raise ContractError(f"shared logical budget marker is missing: {marker}")
    budget_test = (root / "tests/unit/sdk/provider_runtime_test.cpp").read_text(
        encoding="utf-8"
    )
    for marker in (
        "logical output-byte limit diverged by execution surface",
        "row budget could be bypassed by column chunks or execution surface",
        "diagnostic record budget diverged by framing or execution surface",
        "run_worker did not enforce logical output bytes before success",
        "one execution budget dimension accepted zero",
        "exact process transport byte budget was rejected",
        "worker output limit was not distinguished",
    ):
        if marker not in budget_test:
            raise ContractError(f"provider budget acceptance marker is missing: {marker}")
    native = entries.get("public.native-provider-sdk")
    if native is None or native["status"] != "implemented" or native["owner_issue"] != "#153":
        raise ContractError("public.native-provider-sdk is not an implemented Issue #153 entry")

    namespaces = load(root / "schemas/cxxlens_ng_namespace_registry.yaml")
    if not any(
        entry["kind"] == "relation"
        and entry["prefix"] == "frontend.clang22."
        and entry["owner"] == "cxxlens.clang22.reference"
        for entry in namespaces["entries"]
    ):
        raise ContractError("frontend.clang22 relation namespace is not registered")

    support = load(root / "schemas/cxxlens_ng_provider_support_matrix.yaml")
    if not any(
        entry["provider_id"] == "cxxlens.clang22.reference"
        and entry["status"] == "conformance-only"
        and entry["blocker_issue"] is None
        for entry in support["entries"]
    ):
        raise ContractError("Clang 22 provider conformance tuple is not published")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["check"])
    parser.add_argument("--root", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    try:
        validate(arguments.root.resolve())
    except (ContractError, jsonschema.ValidationError, jsonschema.SchemaError) as error:
        print(f"provider runtime contract check failed: {error}", file=sys.stderr)
        return 1
    print("provider runtime contract check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
