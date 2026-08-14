#!/usr/bin/env python3
"""Validate an exact NG1 provider qualification certificate."""

from __future__ import annotations

import argparse
import hashlib
import json
import stat
import pathlib
import subprocess
import sys
from typing import Any

import jsonschema

from check_ng_provider_ng1 import (
    CONTRACT,
    CONTRACT_SCHEMA,
    DIGEST_GRAMMAR_ADR,
    DIGEST_GRAMMAR_ISSUE,
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


def git_binding(root: pathlib.Path) -> tuple[str, str]:
    status = subprocess.run(
        ["git", "-C", str(root), "status", "--porcelain", "--untracked-files=all"],
        capture_output=True,
        check=False,
        text=True,
    )
    if status.returncode != 0:
        fail(f"cannot inspect Git worktree cleanliness: {status.stderr.strip()}")
    if status.stdout.strip():
        fail("exact qualification requires a clean Git worktree")
    values: list[str] = []
    for revision in ("HEAD", "HEAD^{tree}"):
        completed = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "--verify", revision],
            capture_output=True,
            check=False,
            text=True,
        )
        if completed.returncode != 0:
            fail(f"cannot resolve exact Git binding {revision}: {completed.stderr.strip()}")
        value = completed.stdout.strip()
        if len(value) != 40 or any(character not in "0123456789abcdef" for character in value):
            fail(f"Git binding {revision} is not a lowercase hexadecimal object ID")
        values.append(value)
    return values[0], values[1]


def file_digest(path: pathlib.Path, label: str) -> str:
    try:
        mode = path.stat().st_mode
        if not stat.S_ISREG(mode) or not mode & 0o111:
            fail(f"{label} is not an executable regular file: {path}")
        payload = path.read_bytes()
    except OSError as error:
        fail(f"cannot read {label} {path}: {error}")
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def expected_case(case_id: str, outcome: str) -> dict[str, str]:
    if outcome == "accepted":
        return {"id": case_id, "decision": "accepted"}
    if outcome.startswith("provider."):
        return {"id": case_id, "decision": "rejected", "reason_code": outcome}
    return {"id": case_id, "decision": "recovery", "outcome": outcome}


def validate_report(
    root: pathlib.Path,
    report_path: pathlib.Path,
    provider_binary: pathlib.Path,
    provider_semantic_contract: pathlib.Path,
    expected_revision: str | None = None,
    expected_tree: str | None = None,
) -> None:
    contract = validate_ng1_contract(root)
    report_schema = load_yaml(root / QUALIFICATION_REPORT_SCHEMA)
    report = load_json(report_path)
    schema_validate(report, report_schema, "NG1 qualification report")
    if report["authority"]["digest_grammar_adr"] != DIGEST_GRAMMAR_ADR.as_posix():
        fail("qualification report digest grammar ADR is not traceable")
    if report["authority"]["digest_grammar_issue"] != DIGEST_GRAMMAR_ISSUE:
        fail("qualification report digest grammar issue is not traceable")

    actual_revision, actual_tree = git_binding(root)
    if expected_revision is not None and expected_revision != actual_revision:
        fail("caller-supplied expected revision differs from Git HEAD")
    if expected_tree is not None and expected_tree != actual_tree:
        fail("caller-supplied expected tree differs from Git HEAD tree")

    binding = report["binding"]
    if binding["revision"] != actual_revision:
        fail("report revision differs from the exact Git HEAD")
    if binding["tree"] != actual_tree:
        fail("report tree differs from the exact Git HEAD tree")
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
    if binding["hardening_contract_schema_digest"] != document_digest(
        load_yaml(root / CONTRACT_SCHEMA)
    ):
        fail("hardening contract schema digest is stale")
    if binding["provider_binary_digest_source"] != "host-measured-executable-bytes":
        fail("provider binary digest is not host-measured")
    if binding["provider_semantic_contract_digest_source"] != "selected-contract-digest":
        fail("provider semantic contract digest is not selected-contract-bound")
    measured_binary_digest = file_digest(provider_binary, "provider executable")
    if binding["provider_binary_digest"] != measured_binary_digest:
        fail("provider binary digest differs from host-measured executable bytes")
    measured_semantic_contract_digest = document_digest(
        load_yaml(provider_semantic_contract)
    )
    if binding["provider_semantic_contract_digest"] != measured_semantic_contract_digest:
        fail("provider semantic contract digest differs from the selected contract")

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
        f"revision={actual_revision}, tree={actual_tree}, profiles={len(actual_profiles)}, cases={len(expected_ids)}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("--provider-binary", type=pathlib.Path)
    parser.add_argument("--provider-semantic-contract", type=pathlib.Path)
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
        or arguments.provider_binary is None
        or arguments.provider_semantic_contract is None
    ):
        parser.error(
            "report mode requires --report, --provider-binary, and "
            "--provider-semantic-contract"
        )
    validate_report(
        root,
        arguments.report,
        arguments.provider_binary,
        arguments.provider_semantic_contract,
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
