#!/usr/bin/env python3
"""Measure #277 autonomous completion only from exact scenario receipts.

The packet/golden corpus is not an agent execution.  This producer therefore emits
``not-evaluated`` until a caller supplies one receipt-bound outcome for every
declared golden path.  A measured rate is an evaluation metric only; it is never
qualification or release evidence.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import re
import subprocess
import sys
from typing import Any

import jsonschema
import yaml

import check_ng_agent_capability_resolution as capability


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCHEMA = pathlib.Path("schemas/cxxlens_ng_agent_autonomous_completion_metric.schema.yaml")
CATALOG = pathlib.Path("schemas/cxxlens_ng_agent_capability_resolution.yaml")
GENERATOR = pathlib.Path("tools/quality/check_ng_agent_autonomous_completion.py")
RESULT_STATES = set(capability.RESULT_STATES)
OUTCOMES = {"completed", "failed", "safe-stop", "not-evaluated"}
HEX40 = re.compile(r"^[0-9a-f]{40}$")
DIGEST = re.compile(r"^sha256:[0-9a-f]{64}$")


class AgentAutonomousCompletionError(ValueError):
    """A fail-closed metric or receipt binding violation."""


def _digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _digest_object(value: Any) -> str:
    return _digest_bytes(
        json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode(
            "utf-8"
        )
    )


def _git(root: pathlib.Path, expression: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "rev-parse", expression],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise AgentAutonomousCompletionError(
            f"git authority unavailable: {expression}"
        ) from error
    value = result.stdout.strip()
    if not HEX40.fullmatch(value):
        raise AgentAutonomousCompletionError(f"git authority is not exact: {expression}")
    return value


def _load_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise AgentAutonomousCompletionError(f"cannot read evidence: {path}") from error
    if not isinstance(value, dict):
        raise AgentAutonomousCompletionError("evidence must be an object")
    return value


def _catalog(root: pathlib.Path) -> dict[str, Any]:
    value = capability.validate_catalog(root)
    return value


def _catalog_digest(root: pathlib.Path) -> str:
    try:
        return _digest_bytes((root / CATALOG).read_bytes())
    except OSError as error:
        raise AgentAutonomousCompletionError("capability catalog is unreadable") from error


def _authority(root: pathlib.Path) -> dict[str, str]:
    return {
        "revision": _git(root, "HEAD"),
        "tree": _git(root, "HEAD^{tree}"),
        "catalog_path": CATALOG.as_posix(),
        "catalog_digest": _catalog_digest(root),
        "stale_policy": "reject",
    }


def _scenario_set(catalog: dict[str, Any]) -> list[dict[str, str]]:
    return [
        {
            "scenario_id": row["id"],
            "use_case_id": row["use_case_id"],
            "expected_result_state": row["state"],
        }
        for row in catalog["golden_paths"]
    ]


def _validate_receipts(
    evidence: dict[str, Any],
    authority: dict[str, str],
    scenarios: list[dict[str, str]],
) -> dict[str, dict[str, Any]]:
    if evidence.get("schema") != "cxxlens.agent-autonomous-completion-evidence.v1":
        raise AgentAutonomousCompletionError("evidence schema identifier drift")
    if evidence.get("authority") != {
        key: authority[key] for key in ("revision", "tree", "catalog_digest")
    }:
        raise AgentAutonomousCompletionError("stale or mismatched evidence authority")
    rows = evidence.get("scenarios")
    if not isinstance(rows, list) or len(rows) != len(scenarios):
        raise AgentAutonomousCompletionError("evidence scenario census is incomplete")
    expected = {row["scenario_id"] for row in scenarios}
    selected: dict[str, dict[str, Any]] = {}
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise AgentAutonomousCompletionError(f"evidence scenario {index} is not an object")
        scenario_id = row.get("scenario_id")
        if not isinstance(scenario_id, str) or scenario_id not in expected:
            raise AgentAutonomousCompletionError(f"unknown evidence scenario: {scenario_id!r}")
        if scenario_id in selected:
            raise AgentAutonomousCompletionError(f"duplicate evidence scenario: {scenario_id}")
        outcome = row.get("outcome")
        if outcome not in OUTCOMES:
            raise AgentAutonomousCompletionError(f"invalid outcome for {scenario_id}")
        selected[scenario_id] = copy.deepcopy(row)
    if set(selected) != expected:
        raise AgentAutonomousCompletionError("evidence scenario census is not exact")
    for scenario_id, row in selected.items():
        outcome = row["outcome"]
        if outcome == "completed":
            if row.get("bounded_completion") is not True:
                raise AgentAutonomousCompletionError(
                    f"completed scenario lacks bounded completion witness: {scenario_id}"
                )
            for field in ("context_digest", "command_digest", "receipt_digest"):
                if not isinstance(row.get(field), str) or not DIGEST.fullmatch(row[field]):
                    raise AgentAutonomousCompletionError(
                        f"completed scenario lacks exact {field}: {scenario_id}"
                    )
        elif outcome == "safe-stop":
            if not isinstance(row.get("reason_code"), str) or not row["reason_code"].strip():
                raise AgentAutonomousCompletionError(
                    f"safe-stop scenario lacks typed reason: {scenario_id}"
                )
            if not isinstance(row.get("completion_plan_digest"), str) or not DIGEST.fullmatch(
                row["completion_plan_digest"]
            ):
                raise AgentAutonomousCompletionError(
                    f"safe-stop scenario lacks completion plan digest: {scenario_id}"
                )
        elif outcome == "failed":
            if not isinstance(row.get("reason_code"), str) or not row["reason_code"].strip():
                raise AgentAutonomousCompletionError(
                    f"failed scenario lacks typed reason: {scenario_id}"
                )
            if not isinstance(row.get("receipt_digest"), str) or not DIGEST.fullmatch(
                row["receipt_digest"]
            ):
                raise AgentAutonomousCompletionError(
                    f"failed scenario lacks receipt digest: {scenario_id}"
                )
    return selected


def _report(
    root: pathlib.Path,
    *,
    evidence: dict[str, Any] | None,
) -> dict[str, Any]:
    catalog = _catalog(root)
    authority = _authority(root)
    scenarios = _scenario_set(catalog)
    selected = (
        _validate_receipts(evidence, authority, scenarios) if evidence is not None else None
    )
    outcomes: list[dict[str, Any]] = []
    for scenario in scenarios:
        row = (
            selected[scenario["scenario_id"]]
            if selected is not None
            else {"scenario_id": scenario["scenario_id"], "outcome": "not-evaluated"}
        )
        outcomes.append(row)
    counts = {outcome: 0 for outcome in OUTCOMES}
    for row in outcomes:
        counts[row["outcome"]] += 1
    evaluated = evidence is not None and counts["not-evaluated"] == 0
    report: dict[str, Any] = {
        "schema": "cxxlens.ng-agent-autonomous-completion-metric.v1",
        "document_version": "1.0.0",
        "role": "evaluation-only-metric-report",
        "metric": "agent-autonomous-completion-rate",
        "authority": authority,
        "scenario_set": {
            "id": "agent-capability-golden-paths.v1",
            "source": CATALOG.as_posix(),
            "scenarios": scenarios,
        },
        "outcomes": outcomes,
        "population": {
            "denominator": len(scenarios),
            "completed": counts["completed"],
            "failed": counts["failed"],
            "safe_stop": counts["safe-stop"],
            "not_evaluated": counts["not-evaluated"],
        },
        "value_percent": (
            round(100.0 * counts["completed"] / len(scenarios), 6) if evaluated else None
        ),
        "status": "evaluated" if evaluated else "not-evaluated",
        "evidence_disposition": (
            "execution-receipts-bound" if evidence is not None else "execution-receipts-required"
        ),
        "qualification": "not-qualification-evidence",
        "provenance": {
            "generator": GENERATOR.as_posix(),
            "input_contract": "cxxlens.agent-autonomous-completion-evidence.v1",
            "evidence_source": "provided-receipts" if evidence is not None else "none",
        },
    }
    report["canonical_digest"] = _digest_object(report)
    return report


def validate_report(root: pathlib.Path, report: dict[str, Any]) -> None:
    try:
        schema = yaml.safe_load((root / SCHEMA).read_text(encoding="utf-8"))
        jsonschema.Draft202012Validator(schema).validate(report)
    except (OSError, UnicodeError, yaml.YAMLError, jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise AgentAutonomousCompletionError(f"metric schema validation failed: {error}") from error
    candidate = copy.deepcopy(report)
    digest = candidate.pop("canonical_digest", None)
    if digest != _digest_object(candidate):
        raise AgentAutonomousCompletionError("metric canonical digest mismatch")
    authority = _authority(root)
    if report["authority"] != authority:
        raise AgentAutonomousCompletionError("metric authority is stale")
    scenario_ids = [row["scenario_id"] for row in report["scenario_set"]["scenarios"]]
    if scenario_ids != [row["scenario_id"] for row in _scenario_set(_catalog(root))]:
        raise AgentAutonomousCompletionError("metric scenario set drift")
    outcome_ids = [row["scenario_id"] for row in report["outcomes"]]
    if outcome_ids != scenario_ids:
        raise AgentAutonomousCompletionError("metric outcome census/order drift")
    population = report["population"]
    counts = {outcome: 0 for outcome in OUTCOMES}
    for row in report["outcomes"]:
        counts[row["outcome"]] += 1
    if population != {
        "denominator": len(scenario_ids),
        "completed": counts["completed"],
        "failed": counts["failed"],
        "safe_stop": counts["safe-stop"],
        "not_evaluated": counts["not-evaluated"],
    }:
        raise AgentAutonomousCompletionError("metric population does not match outcomes")
    if report["status"] == "not-evaluated" and report["value_percent"] is not None:
        raise AgentAutonomousCompletionError("not-evaluated metric has a numeric rate")
    if report["status"] == "evaluated" and report["value_percent"] is None:
        raise AgentAutonomousCompletionError("evaluated metric has no numeric rate")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("report", "check"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--evidence-json", type=pathlib.Path)
    parser.add_argument("--input-json", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        if args.command == "report":
            evidence = _load_json(args.evidence_json) if args.evidence_json else None
            report = _report(root, evidence=evidence)
        else:
            if args.input_json is None:
                raise AgentAutonomousCompletionError("check requires --input-json")
            report = _load_json(args.input_json)
            validate_report(root, report)
        if args.output is not None:
            args.output.write_text(
                json.dumps(report, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
                encoding="utf-8",
            )
        print(json.dumps(report, ensure_ascii=False, sort_keys=True, indent=2))
        return 0
    except (AgentAutonomousCompletionError, OSError, UnicodeError) as error:
        print(f"agent-autonomous-completion: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
