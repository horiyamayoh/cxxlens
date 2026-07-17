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
    jsonschema.Draft202012Validator.check_schema(report_schema)

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
            "class provider_selection",
            "selected_candidate() const",
            "authority_request() const",
        ),
        "src/runtime/provider_process_adapter.cpp": (
            "provider.binary-identity-mismatch",
            "resolve_sandbox_policy",
            "sandbox_evidence_digest",
            "security.sandbox-insufficient",
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
            "encode_column_chunk",
            "decode_columnar_batch_end",
            "valid_utf8",
            "control-utf8",
            "no-shell-argv-exec",
            "network-syscall-deny",
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
            "cxxlens.clang22.task.v2",
            "source_snapshot",
        ),
        "CMakeLists.txt": (
            "cxxlens-clang-worker-22",
            "cxxlens_ng_provider_runtime_contract.yaml",
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

    catalog = load(root / "schemas/cxxlens_ng_public_api_catalog.yaml")
    entries = {entry["id"]: entry for entry in catalog["entries"]}
    runtime = entries.get("public.provider-runtime")
    if runtime is None or runtime["status"] != "implemented" or runtime["owner_issue"] != "#106":
        raise ContractError("public.provider-runtime is not an implemented Issue #106 entry")

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
