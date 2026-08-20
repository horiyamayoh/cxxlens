#!/usr/bin/env python3
"""Validate CI freshness authority and emit bounded freshness/evaluation reports."""

from __future__ import annotations

import argparse
import datetime
import json
import pathlib
import subprocess
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONTRACT = pathlib.Path("schemas/cxxlens_ng_autonomy_ci.yaml")
SCHEMA = pathlib.Path("schemas/cxxlens_ng_autonomy_ci.schema.yaml")


class AutonomyCiError(ValueError):
    """A stale, promotable, or structurally invalid CI configuration."""


def load(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, yaml.YAMLError) as error:
        raise AutonomyCiError(f"cannot load {path}: {error}") from error
    if not isinstance(value, dict):
        raise AutonomyCiError(f"expected mapping: {path}")
    return value


def git(root: pathlib.Path, *arguments: str) -> str:
    try:
        result = subprocess.run(["git", "-C", str(root), *arguments], check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as error:
        raise AutonomyCiError(f"git binding unavailable: {' '.join(arguments)}") from error
    return result.stdout.strip()


def triggers(document: dict[str, Any]) -> dict[str, Any]:
    value = document.get("on", document.get(True))
    if not isinstance(value, dict):
        raise AutonomyCiError("workflow trigger mapping missing")
    return value


def validate(root: pathlib.Path) -> dict[str, Any]:
    contract = load(root / CONTRACT)
    schema = load(root / SCHEMA)
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(contract)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise AutonomyCiError(f"schema validation failed: {error.message}") from error
    fast = load(root / contract["fast"]["workflow"])
    heavy = load(root / contract["heavy"]["workflow"])
    nightly = load(root / contract["nightly"]["workflow"])
    release = load(root / contract["release"]["workflow"])
    fast_on = triggers(fast)
    if set(fast_on) != {"push", "workflow_dispatch"} or fast_on["push"].get("branches") != ["main"]:
        raise AutonomyCiError("fast workflow is not every main SHA plus explicit dispatch")
    if "concurrency" in fast:
        raise AutonomyCiError("fast workflow may not cancel or coalesce commits")
    heavy_on = triggers(heavy)
    if heavy_on.get("workflow_run", {}).get("workflows") != ["Autonomy fast"]:
        raise AutonomyCiError("heavy workflow is not connected to Autonomy fast")
    if heavy.get("concurrency") != {"group": "autonomy-heavy-latest-main", "cancel-in-progress": True}:
        raise AutonomyCiError("heavy coalescing policy drift")
    heavy_text = (root / contract["heavy"]["workflow"]).read_text(encoding="utf-8")
    for marker in ("classify-heavy", "superseded", "needs.freshness.outputs.disposition == 'current'", "run_gate.py full", "Reclassify freshness after full gate", "steps.postflight.outputs.disposition == 'current'"):
        if marker not in heavy_text:
            raise AutonomyCiError(f"heavy freshness marker missing: {marker}")
    nightly_on = triggers(nightly)
    if "schedule" not in nightly_on or "workflow_dispatch" not in nightly_on:
        raise AutonomyCiError("Nightly lacks schedule/dispatch latest-main entry")
    if contract["nightly"].get("valid_events") != ["schedule", "workflow_dispatch"] or contract["nightly"].get("ineligible_events") != ["workflow_call"]:
        raise AutonomyCiError("Nightly release event eligibility drift")
    release_on = triggers(release)
    if set(release_on) != {"workflow_dispatch"}:
        raise AutonomyCiError("release evaluation is not dispatch-only")
    if release.get("concurrency", {}).get("cancel-in-progress") is not False:
        raise AutonomyCiError("release evaluation may not be cancelled")
    release_text = (root / contract["release"]["workflow"]).read_text(encoding="utf-8")
    for marker in ("git rev-parse origin/main", "workflow_dispatch", "not-qualified-evaluation-only-no-gr"):
        if marker not in release_text and marker != "not-qualified-evaluation-only-no-gr":
            raise AutonomyCiError(f"release freshness marker missing: {marker}")
    if contract["release"]["composition"] != {"aggregate_decision": "#173", "gr_execution_contract": "#167", "scope_closure_contract": "#179"}:
        raise AutonomyCiError("release role composition drift")
    if contract["release"]["current_capability"] != "not-qualified-evaluation-only-no-gr":
        raise AutonomyCiError("release workflow overclaims current capability")
    return contract


def write_report(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")


def classify_heavy(root: pathlib.Path, candidate: str, output: pathlib.Path, report: pathlib.Path) -> None:
    if len(candidate) != 40 or any(character not in "0123456789abcdef" for character in candidate):
        raise AutonomyCiError("candidate SHA is not full lowercase hex")
    current = git(root, "rev-parse", "origin/main")
    disposition = "current" if candidate == current else "superseded"
    with output.open("a", encoding="utf-8") as stream:
        stream.write(f"disposition={disposition}\ncandidate_sha={candidate}\n")
    write_report(report, {"schema": "cxxlens.autonomy-heavy-freshness.v1", "candidate_sha": candidate, "current_origin_main": current, "disposition": disposition, "heavy_evidence_authority": disposition == "current"})


def release_evaluation(root: pathlib.Path, candidate: str, report: pathlib.Path) -> None:
    current = git(root, "rev-parse", "origin/main")
    if candidate != current or git(root, "rev-parse", "HEAD") != candidate:
        raise AutonomyCiError("release candidate is not exact current origin/main")
    write_report(report, {"schema": "cxxlens.autonomy-release-evaluation.v1", "created_at": datetime.datetime.now(datetime.UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z"), "candidate_sha": candidate, "status": "not-qualified", "gr_issued": False, "roles": {"aggregate": "#173", "gr": "#167", "scope_closure": "#179"}, "missing_inputs": ["exact-successful-heavy", "exact-successful-nightly", "authenticated-gr-report", "authenticated-terminal-scope-report"], "production_qualification": "not-claimed"})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "classify-heavy", "release-evaluation"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--candidate")
    parser.add_argument("--github-output", type=pathlib.Path)
    parser.add_argument("--report", type=pathlib.Path)
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        validate(root)
        if args.command == "classify-heavy":
            if not args.candidate or not args.github_output or not args.report:
                raise AutonomyCiError("classify-heavy requires candidate, github output, and report")
            classify_heavy(root, args.candidate, args.github_output, args.report)
        elif args.command == "release-evaluation":
            if not args.candidate or not args.report:
                raise AutonomyCiError("release-evaluation requires candidate and report")
            release_evaluation(root, args.candidate, args.report)
        else:
            print("autonomy-ci: ok")
    except AutonomyCiError as error:
        print(f"autonomy-ci: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
