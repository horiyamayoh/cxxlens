#!/usr/bin/env python3
"""Validate an exact NG1 provider qualification certificate."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from typing import Any

import jsonschema

from check_ng_provider_ng1 import (
    CONTRACT,
    PROTOCOL,
    QUALIFICATION_REPORT_SCHEMA,
    VECTORS,
    VECTORS_SCHEMA,
    Ng1ContractError,
    document_digest,
    load_yaml,
    schema_validate,
    validate_ng1_contract,
)


ROOT = pathlib.Path(__file__).resolve().parents[2]


class Ng1QualificationError(ValueError):
    """An NG1 qualification certificate is not exact or complete."""


def fail(message: str) -> None:
    raise Ng1QualificationError(f"provider.ng1.qualification: {message}")


def load_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_reject_duplicate_members,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot load strict JSON report {path}: {error}")
    if not isinstance(value, dict):
        fail("qualification report root must be an object")
    return value


def _reject_duplicate_members(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            fail(f"duplicate report member: {key}")
        value[key] = item
    return value


def expected_case(case_id: str, outcome: str) -> dict[str, str]:
    if outcome == "accepted":
        return {"id": case_id, "decision": "accepted"}
    if outcome.startswith("provider."):
        return {"id": case_id, "decision": "rejected", "reason_code": outcome}
    return {"id": case_id, "decision": "recovery", "outcome": outcome}


def validate_report(
    root: pathlib.Path,
    report_path: pathlib.Path,
    expected_revision: str,
    expected_tree: str,
) -> None:
    contract = validate_ng1_contract(root)
    report_schema = load_yaml(root / QUALIFICATION_REPORT_SCHEMA)
    report = load_json(report_path)
    schema_validate(report, report_schema, "NG1 qualification report")

    binding = report["binding"]
    if binding["revision"] != expected_revision:
        fail("report revision differs from the expected exact revision")
    if binding["tree"] != expected_tree:
        fail("report tree differs from the expected exact tree")
    if binding["report_schema_digest"] != document_digest(report_schema):
        fail("report schema digest is stale")
    if binding["vectors_digest"] != document_digest(load_yaml(root / VECTORS)):
        fail("conformance vectors digest is stale")
    if binding["vectors_schema_digest"] != document_digest(
        load_yaml(root / VECTORS_SCHEMA)
    ):
        fail("conformance vectors schema digest is stale")
    if binding["protocol_contract_digest"] != document_digest(
        load_yaml(root / PROTOCOL)
    ):
        fail("provider protocol digest is stale")
    if binding["hardening_contract_digest"] != document_digest(
        load_yaml(root / CONTRACT)
    ):
        fail("hardening contract digest is stale")
    if binding["provider_binary_digest_source"] != "host-measured-executable-bytes":
        fail("provider binary digest is not host-measured")
    if binding["provider_semantic_contract_digest_source"] != "selected-contract-digest":
        fail("provider semantic contract digest is not selected-contract-bound")

    expected_profiles = set(contract["qualification"]["required_profiles"])
    actual_profiles = {profile["profile"] for profile in report["profiles"]}
    if actual_profiles != expected_profiles:
        fail(f"profile set differs: expected={sorted(expected_profiles)}, got={sorted(actual_profiles)}")
    expected_ids = set(contract["qualification"]["required_cases"])
    expected_outcomes = contract["qualification"]["required_case_outcomes"]
    for profile in report["profiles"]:
        cases = {case["id"]: case for case in profile["cases"]}
        if len(cases) != len(profile["cases"]):
            fail(f"{profile['profile']} case IDs are duplicated")
        if set(cases) != expected_ids:
            fail(f"{profile['profile']} case set differs from the authority")
        for case_id, outcome in expected_outcomes.items():
            actual = cases[case_id]
            expected = expected_case(case_id, outcome)
            if actual != expected:
                fail(
                    f"{profile['profile']} case {case_id} differs: "
                    f"expected={expected!r}, got={actual!r}"
                )
    print(
        "verified NG1 qualification report: "
        f"revision={expected_revision}, tree={expected_tree}, profiles={len(actual_profiles)}, cases={len(expected_ids)}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("--expected-revision")
    parser.add_argument("--expected-tree")
    arguments = parser.parse_args()
    root = arguments.root.resolve()
    if arguments.mode == "check":
        contract = validate_ng1_contract(root)
        print(
            "verified NG1 qualification authority: "
            f"report_schema={contract['qualification']['report_schema']}"
        )
        return 0
    if (
        arguments.report is None
        or arguments.expected_revision is None
        or arguments.expected_tree is None
    ):
        parser.error("report mode requires --report, --expected-revision, and --expected-tree")
    validate_report(
        root,
        arguments.report,
        arguments.expected_revision,
        arguments.expected_tree,
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, Ng1ContractError, Ng1QualificationError, jsonschema.ValidationError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1) from error
