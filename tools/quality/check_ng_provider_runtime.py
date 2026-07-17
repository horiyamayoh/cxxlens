#!/usr/bin/env python3
"""Validate the NG process provider runtime and Clang 22 worker contract."""

from __future__ import annotations

import argparse
import pathlib
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
            "no-shell-argv-exec",
            "network-syscall-deny",
            "provider.binary-identity-mismatch",
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
        ),
        "src/llvm/clang22/provider_worker.cpp": (
            "frontend.clang22.entity_observation",
            "cc.call_site",
            "ignored-or-gcc-specific-option",
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
    if runtime is None or runtime["status"] != "implemented" or runtime["owner_issue"] != "#101":
        raise ContractError("public.provider-runtime is not an implemented Issue #101 entry")

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
