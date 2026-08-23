#!/usr/bin/env python3
"""Check compatibility v2 and the ordinary support table.

This checker intentionally has no release-qualification or evidence-report
mode.  ``check`` validates the static contract; ``inspect`` and ``doctor``
return a compatibility decision for the supplied axes/environment.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from collections import Counter
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
REQUEST_SCHEMA = pathlib.Path("schemas/cxxlens_ng_compatibility_request.schema.yaml")
REPORT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_compatibility_report.schema.yaml")
SUPPORT_TABLE = pathlib.Path("schemas/cxxlens_support_matrix.yaml")
SUPPORT_SCHEMA = pathlib.Path("schemas/cxxlens_support_matrix.schema.yaml")

SEMVER = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
FORBIDDEN_OPERATIONAL_FIELDS = {
    "runtime_qualified",
    "evidence_refs",
    "qualification_state",
    "binary_digest",
    "evidence_digest",
    "production_supported",
}
FORBIDDEN_REASON_CODES = {"compat.release-not-qualified"}


class ReleaseContractError(ValueError):
    """A compatibility or support-table violation."""


def fail(message: str) -> None:
    raise ReleaseContractError(message)


def load(path: pathlib.Path) -> Any:
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8"))
    except OSError as error:
        fail(f"cannot read {path}: {error}")


def validate(document: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"{label} schema validation failed: {error.message}")


def reject_old_fields(value: Any, path: str = "document") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if key in FORBIDDEN_OPERATIONAL_FIELDS:
                fail(f"obsolete operational field remains: {path}.{key}")
            reject_old_fields(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            reject_old_fields(child, f"{path}[{index}]")


def reject_old_reason_codes(value: Any, path: str = "document") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if key in {"reason_codes", "code"}:
                values = child if isinstance(child, list) else [child]
                if any(item in FORBIDDEN_REASON_CODES for item in values):
                    fail(f"obsolete compatibility reason remains: {path}.{key}")
            reject_old_reason_codes(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            reject_old_reason_codes(child, f"{path}[{index}]")


def validate_support_table(root: pathlib.Path) -> dict[str, Any]:
    document = load(root / SUPPORT_TABLE)
    validate(document, load(root / SUPPORT_SCHEMA), "support table")
    reject_old_fields(document, "support")
    reject_old_reason_codes(document, "support")
    keys = [
        (
            row["release_version"],
            row["surface"],
            row["os"],
            row["architecture"],
            row["compiler_provider_major"],
            row["linkage"],
        )
        for row in document["entries"]
    ]
    duplicates = [key for key, count in Counter(keys).items() if count != 1]
    if duplicates:
        fail(f"support table contains duplicate environments: {duplicates}")
    if any(row["os"] == "windows" or row["compiler_provider_major"].startswith("msvc")
           for row in document["entries"]):
        fail("Windows/MSVC must remain unsupported and unlisted")
    return document


def validate_repository(root: pathlib.Path) -> None:
    request_schema = load(root / REQUEST_SCHEMA)
    report_schema = load(root / REPORT_SCHEMA)
    if request_schema["$id"].endswith(".v1") or report_schema["$id"].endswith(".v1"):
        fail("compatibility v1 schema is still authoritative")
    if request_schema["properties"]["schema"].get("const") != "cxxlens.ng-compatibility-request.v2":
        fail("compatibility request schema is not v2")
    if report_schema["properties"]["schema"].get("const") != "cxxlens.ng-compatibility-report.v2":
        fail("compatibility report schema is not v2")
    validate_support_table(root)



def semver(value: str) -> tuple[int, int, int]:
    match = SEMVER.fullmatch(value)
    if match is None:
        fail(f"invalid semantic version: {value}")
    return tuple(int(part) for part in match.groups())


def grouped(rows: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for row in rows:
        axis = row["axis"]
        if axis in result:
            fail(f"compatibility axis appears more than once: {axis}")
        result[axis] = row
    return result


def axis_result(required: dict[str, Any], offered: dict[str, Any] | None) -> dict[str, Any]:
    axis = required["axis"]
    base = {
        "axis": axis,
        "required_version": required["version"],
        "offered_version": offered["version"] if offered else None,
        "status": "unsupported",
        "reason_codes": [],
        "missing_required_features": [],
        "unavailable_optional_features": [],
    }
    if offered is None:
        base["reason_codes"] = ["compat.axis-missing"]
        return base
    required_features = {row["id"]: row["requirement"] for row in required["features"]}
    offered_features = {row["id"] for row in offered["features"]}
    base["missing_required_features"] = sorted(
        feature for feature, requirement in required_features.items()
        if requirement == "required" and feature not in offered_features
    )
    base["unavailable_optional_features"] = sorted(
        feature for feature, requirement in required_features.items()
        if requirement == "optional" and feature not in offered_features
    )
    if base["missing_required_features"]:
        base["reason_codes"] = ["compat.required-feature-missing"]
        return base
    required_version = semver(required["version"])
    offered_version = semver(offered["version"])
    if required["contract_digest"] != offered["contract_digest"]:
        base["reason_codes"] = ["compat.contract-digest-mismatch"]
        return base
    if required_version[0] != offered_version[0]:
        base["reason_codes"] = ["compat.axis-major-mismatch"]
        return base
    if offered_version < required_version:
        base["status"] = "migration-required"
        base["reason_codes"] = ["compat.migration-required"]
        return base
    if offered_version != required_version:
        base["reason_codes"] = ["compat.axis-version-mismatch"]
        return base
    base["status"] = "supported"
    base["reason_codes"] = ["compat.exact"]
    if base["unavailable_optional_features"]:
        base["reason_codes"].append("compat.optional-feature-unavailable")
    return base


def environment_supported(table: dict[str, Any], release_id: str,
                           environment: dict[str, str]) -> bool:
    release_version = release_id.removeprefix("distribution-")
    return any(
        row["release_version"] == release_version
        and row["os"] == environment["os"]
        and row["architecture"] == environment["architecture"]
        and row["compiler_provider_major"] == environment["toolchain"]
        and row["linkage"] == environment["linkage"]
        for row in table["entries"]
    )


def decide(root: pathlib.Path, request: dict[str, Any]) -> dict[str, Any]:
    request_schema = load(root / REQUEST_SCHEMA)
    report_schema = load(root / REPORT_SCHEMA)
    validate(request, request_schema, "compatibility request")
    table = validate_support_table(root)
    required = grouped(request["required_axes"])
    offered = grouped(request["offered_axes"])
    axes = [axis_result(required_axis, offered.get(axis))
            for axis, required_axis in sorted(required.items())]
    reasons = sorted({reason for result in axes for reason in result["reason_codes"]})
    statuses = {result["status"] for result in axes}
    findings: list[dict[str, str]] = []
    if request["operation"] == "doctor" and not environment_supported(
        table, request["release_id"], request["environment"]
    ):
        reasons.append("compat.environment-unsupported")
        findings.append({
            "code": "compat.environment-unsupported",
            "severity": "blocker",
            "message": "The OS, architecture, toolchain/provider major, and linkage tuple is not listed.",
        })
        statuses.add("unsupported")
    if "unsupported" in statuses:
        decision = "unsupported"
    elif "migration-required" in statuses:
        decision = "migration-required"
    else:
        decision = "supported"
    report = {
        "schema": "cxxlens.ng-compatibility-report.v2",
        "request_id": request["request_id"],
        "operation": request["operation"],
        "decision": decision,
        "release_id": request["release_id"],
        "context": request["context"],
        "axis_results": axes,
        "reason_codes": sorted(set(reasons)),
        "migration_steps": [],
        "environment_findings": findings,
        "fallback_used": False,
    }
    validate(report, report_schema, "compatibility report")
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "inspect", "doctor"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--request", type=pathlib.Path)
    arguments = parser.parse_args()
    try:
        root = arguments.root.resolve()
        validate_repository(root)
        if arguments.command == "check":
            if arguments.request is not None:
                fail("--request is valid only for inspect or doctor")
            print("validated compatibility v2 and ordinary support table")
            return 0
        if arguments.request is None:
            fail("inspect and doctor require --request")
        request = load(arguments.request.resolve())
        if request.get("operation") != arguments.command:
            fail("request operation differs from the invoked command")
        print(json.dumps(decide(root, request), ensure_ascii=False, indent=2, sort_keys=True))
        return 0
    except (OSError, ReleaseContractError, KeyError, TypeError) as error:
        print(f"compatibility contract check failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
