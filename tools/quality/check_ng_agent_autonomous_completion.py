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
ENVIRONMENT_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
EXECUTION_TERMINALS = {
    "cancelled",
    "exited",
    "launch-failed",
    "not-launched",
    "signaled",
    "timed-out",
}


class AgentAutonomousCompletionError(ValueError):
    """A fail-closed metric or receipt binding violation."""


def _digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")


def _digest_object(value: Any) -> str:
    return _digest_bytes(_canonical_bytes(value))


def _exact_object(
    value: Any,
    fields: set[str],
    description: str,
) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != fields:
        raise AgentAutonomousCompletionError(f"{description} field set is not exact")
    return value


def _nonempty_text(value: Any, description: str) -> str:
    if not isinstance(value, str) or not value.strip() or "\x00" in value:
        raise AgentAutonomousCompletionError(f"{description} is not nonempty text")
    return value


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


def _context_summary(root: pathlib.Path, scenario_id: str) -> dict[str, Any]:
    try:
        context = capability.build_resolution(root, scenario_id)
    except capability.CapabilityResolutionError as error:
        raise AgentAutonomousCompletionError(
            f"canonical scenario context is unavailable: {scenario_id}"
        ) from error
    encoded = _canonical_bytes(context)
    return {
        "schema": context["schema"],
        "use_case_id": context["use_case_id"],
        "result_state": context["result"]["state"],
        "byte_count": len(encoded),
        "digest": _digest_bytes(encoded),
    }


def _normalize_command(value: Any, scenario_id: str) -> dict[str, Any]:
    command = _exact_object(
        value,
        {"argv", "working_directory", "environment"},
        f"execution command for {scenario_id}",
    )
    argv = command["argv"]
    if (
        not isinstance(argv, list)
        or not argv
        or any(not isinstance(argument, str) or "\x00" in argument for argument in argv)
        or not argv[0]
    ):
        raise AgentAutonomousCompletionError(
            f"execution command lacks exact argv for {scenario_id}"
        )
    if command["working_directory"] != "repository-root":
        raise AgentAutonomousCompletionError(
            f"execution command has ambient working directory for {scenario_id}"
        )
    environment = command["environment"]
    if not isinstance(environment, list):
        raise AgentAutonomousCompletionError(
            f"execution command environment is not a list for {scenario_id}"
        )
    normalized_environment: list[dict[str, str]] = []
    for index, entry in enumerate(environment):
        item = _exact_object(
            entry,
            {"name", "value_digest"},
            f"execution environment {scenario_id}[{index}]",
        )
        if not isinstance(item["name"], str) or not ENVIRONMENT_NAME.fullmatch(item["name"]):
            raise AgentAutonomousCompletionError(
                f"execution environment name is invalid for {scenario_id}"
            )
        if not isinstance(item["value_digest"], str) or not DIGEST.fullmatch(
            item["value_digest"]
        ):
            raise AgentAutonomousCompletionError(
                f"execution environment digest is invalid for {scenario_id}"
            )
        normalized_environment.append(copy.deepcopy(item))
    names = [entry["name"] for entry in normalized_environment]
    if names != sorted(set(names)):
        raise AgentAutonomousCompletionError(
            f"execution environment is not canonical for {scenario_id}"
        )
    return {
        "argv": list(argv),
        "working_directory": "repository-root",
        "environment": normalized_environment,
    }


def _normalize_stream(value: Any, scenario_id: str, field: str) -> dict[str, Any]:
    stream = _exact_object(
        value,
        {"byte_count", "digest", "complete"},
        f"execution {field} receipt for {scenario_id}",
    )
    if not isinstance(stream["byte_count"], int) or isinstance(stream["byte_count"], bool) or stream[
        "byte_count"
    ] < 0:
        raise AgentAutonomousCompletionError(
            f"execution {field} byte count is invalid for {scenario_id}"
        )
    if not isinstance(stream["digest"], str) or not DIGEST.fullmatch(stream["digest"]):
        raise AgentAutonomousCompletionError(
            f"execution {field} digest is invalid for {scenario_id}"
        )
    if not isinstance(stream["complete"], bool):
        raise AgentAutonomousCompletionError(
            f"execution {field} completeness is invalid for {scenario_id}"
        )
    return copy.deepcopy(stream)


def _normalize_process(value: Any, scenario_id: str, outcome: str) -> dict[str, Any]:
    process = _exact_object(
        value,
        {"terminal_state", "exit_status", "signal", "stdout", "stderr"},
        f"execution process receipt for {scenario_id}",
    )
    terminal = process["terminal_state"]
    if terminal not in EXECUTION_TERMINALS:
        raise AgentAutonomousCompletionError(
            f"execution terminal state is invalid for {scenario_id}"
        )
    exit_status = process["exit_status"]
    signal = process["signal"]
    if terminal == "exited":
        if (
            not isinstance(exit_status, int)
            or isinstance(exit_status, bool)
            or exit_status < 0
            or exit_status > 255
            or signal is not None
        ):
            raise AgentAutonomousCompletionError(
                f"exited process receipt is invalid for {scenario_id}"
            )
    elif terminal == "signaled":
        if (
            exit_status is not None
            or not isinstance(signal, int)
            or isinstance(signal, bool)
            or signal <= 0
        ):
            raise AgentAutonomousCompletionError(
                f"signaled process receipt is invalid for {scenario_id}"
            )
    elif exit_status is not None or signal is not None:
        raise AgentAutonomousCompletionError(
            f"non-exit process receipt invents terminal status for {scenario_id}"
        )
    stdout = _normalize_stream(process["stdout"], scenario_id, "stdout")
    stderr = _normalize_stream(process["stderr"], scenario_id, "stderr")
    if outcome == "completed" and (
        terminal != "exited"
        or exit_status != 0
        or not stdout["complete"]
        or not stderr["complete"]
    ):
        raise AgentAutonomousCompletionError(
            f"completed scenario lacks a successful complete process receipt: {scenario_id}"
        )
    return {
        "terminal_state": terminal,
        "exit_status": exit_status,
        "signal": signal,
        "stdout": stdout,
        "stderr": stderr,
    }


def _normalize_result(value: Any, scenario_id: str, outcome: str) -> dict[str, Any]:
    result = _exact_object(
        value,
        {"outcome", "bounded_completion", "reason_code", "completion_plan"},
        f"execution result receipt for {scenario_id}",
    )
    if result["outcome"] != outcome:
        raise AgentAutonomousCompletionError(
            f"execution result outcome is not cross-bound for {scenario_id}"
        )
    if not isinstance(result["bounded_completion"], bool):
        raise AgentAutonomousCompletionError(
            f"execution result completion marker is invalid for {scenario_id}"
        )
    reason_code = _nonempty_text(
        result["reason_code"], f"execution result reason for {scenario_id}"
    )
    completion_plan = result["completion_plan"]
    if (
        not isinstance(completion_plan, list)
        or any(not isinstance(step, str) or not step.strip() or "\x00" in step for step in completion_plan)
    ):
        raise AgentAutonomousCompletionError(
            f"execution result completion plan is invalid for {scenario_id}"
        )
    if outcome == "completed":
        if result["bounded_completion"] is not True or reason_code != "none" or completion_plan:
            raise AgentAutonomousCompletionError(
                f"completed execution result is not exact for {scenario_id}"
            )
    elif result["bounded_completion"] is not False or reason_code == "none":
        raise AgentAutonomousCompletionError(
            f"non-completed execution result is not exact for {scenario_id}"
        )
    if outcome == "safe-stop" and not completion_plan:
        raise AgentAutonomousCompletionError(
            f"safe-stop execution result lacks a completion plan for {scenario_id}"
        )
    return {
        "outcome": outcome,
        "bounded_completion": result["bounded_completion"],
        "reason_code": reason_code,
        "completion_plan": list(completion_plan),
    }


def _normalize_execution_witness(
    root: pathlib.Path,
    value: Any,
    authority: dict[str, str],
    scenario: dict[str, str],
    outcome: str,
    *,
    input_context: bool,
) -> dict[str, Any]:
    scenario_id = scenario["scenario_id"]
    witness = _exact_object(
        value,
        {"schema", "scenario_id", "authority", "context", "command", "process", "result"},
        f"execution witness for {scenario_id}",
    )
    if witness["schema"] != "cxxlens.agent-autonomous-completion-execution-witness.v1":
        raise AgentAutonomousCompletionError(
            f"execution witness schema drift for {scenario_id}"
        )
    if witness["scenario_id"] != scenario_id:
        raise AgentAutonomousCompletionError(
            f"execution witness scenario mismatch for {scenario_id}"
        )
    expected_authority = {
        key: authority[key] for key in ("revision", "tree", "catalog_digest")
    }
    if witness["authority"] != expected_authority:
        raise AgentAutonomousCompletionError(
            f"execution witness authority mismatch for {scenario_id}"
        )
    expected_context = _context_summary(root, scenario_id)
    if input_context:
        supplied_context = witness["context"]
        try:
            capability.validate_resolution(root, supplied_context)
        except (capability.CapabilityResolutionError, TypeError) as error:
            raise AgentAutonomousCompletionError(
                f"execution witness context is invalid for {scenario_id}"
            ) from error
        if supplied_context != capability.build_resolution(root, scenario_id):
            raise AgentAutonomousCompletionError(
                f"execution witness context is not the canonical scenario context: {scenario_id}"
            )
        context = expected_context
    else:
        if witness["context"] != expected_context:
            raise AgentAutonomousCompletionError(
                f"execution witness context summary mismatch for {scenario_id}"
            )
        context = copy.deepcopy(witness["context"])
    if context["result_state"] != scenario["expected_result_state"]:
        raise AgentAutonomousCompletionError(
            f"execution witness context result drift for {scenario_id}"
        )
    command = _normalize_command(witness["command"], scenario_id)
    process = _normalize_process(witness["process"], scenario_id, outcome)
    result = _normalize_result(witness["result"], scenario_id, outcome)
    return {
        "schema": "cxxlens.agent-autonomous-completion-execution-witness.v1",
        "scenario_id": scenario_id,
        "authority": expected_authority,
        "context": context,
        "command": command,
        "process": process,
        "result": result,
    }


def _validate_receipts(
    root: pathlib.Path,
    evidence: dict[str, Any],
    authority: dict[str, str],
    scenarios: list[dict[str, str]],
) -> dict[str, dict[str, Any]]:
    if evidence.get("schema") != "cxxlens.agent-autonomous-completion-evidence.v2":
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
    scenario_map = {row["scenario_id"]: row for row in scenarios}
    for scenario_id, row in selected.items():
        outcome = row["outcome"]
        if outcome == "not-evaluated":
            if set(row) != {"scenario_id", "outcome"}:
                raise AgentAutonomousCompletionError(
                    f"not-evaluated scenario carries fabricated evidence: {scenario_id}"
                )
            continue
        witness = _normalize_execution_witness(
            root,
            row.get("execution_witness"),
            authority,
            scenario_map[scenario_id],
            outcome,
            input_context=True,
        )
        normalized = {
            "scenario_id": scenario_id,
            "outcome": outcome,
            "bounded_completion": witness["result"]["bounded_completion"],
            "context_digest": witness["context"]["digest"],
            "command_digest": _digest_object(witness["command"]),
            "receipt_digest": _digest_object(witness),
            "execution_witness": witness,
        }
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
            normalized["reason_code"] = row["reason_code"]
            normalized["completion_plan_digest"] = row["completion_plan_digest"]
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
            normalized["reason_code"] = row["reason_code"]
        if row.get("context_digest") != normalized["context_digest"]:
            raise AgentAutonomousCompletionError(
                f"scenario context digest is not witness-derived: {scenario_id}"
            )
        if row.get("command_digest") != normalized["command_digest"]:
            raise AgentAutonomousCompletionError(
                f"scenario command digest is not witness-derived: {scenario_id}"
            )
        if row.get("receipt_digest") != normalized["receipt_digest"]:
            raise AgentAutonomousCompletionError(
                f"scenario receipt digest is not witness-derived: {scenario_id}"
            )
        if outcome in {"failed", "safe-stop"} and row["reason_code"] != witness["result"][
            "reason_code"
        ]:
            raise AgentAutonomousCompletionError(
                f"scenario reason code is not witness-derived: {scenario_id}"
            )
        if outcome == "safe-stop" and row["completion_plan_digest"] != _digest_object(
            witness["result"]["completion_plan"]
        ):
            raise AgentAutonomousCompletionError(
                f"scenario completion plan digest is not witness-derived: {scenario_id}"
            )
        selected[scenario_id] = normalized
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
        _validate_receipts(root, evidence, authority, scenarios)
        if evidence is not None
        else None
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
        "document_version": "1.1.0",
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
            "execution-receipts-bound"
            if evaluated
            else "execution-receipts-required"
        ),
        "qualification": "not-qualification-evidence",
        "provenance": {
            "generator": GENERATOR.as_posix(),
            "input_contract": "cxxlens.agent-autonomous-completion-evidence.v2",
            # A supplied file containing only not-evaluated rows is not an
            # execution receipt.  Keep the provenance honest and preserve the
            # fail-closed metric state in that case.
            "evidence_source": (
                "provided-receipts"
                if any(row["outcome"] != "not-evaluated" for row in outcomes)
                else "none"
            ),
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
    scenario_map = {row["scenario_id"]: row for row in report["scenario_set"]["scenarios"]}
    for row in report["outcomes"]:
        counts[row["outcome"]] += 1
        outcome = row["outcome"]
        scenario_id = row["scenario_id"]
        if outcome == "not-evaluated":
            if set(row) != {"scenario_id", "outcome"}:
                raise AgentAutonomousCompletionError(
                    f"not-evaluated report outcome carries evidence: {scenario_id}"
                )
            continue
        witness = _normalize_execution_witness(
            root,
            row.get("execution_witness"),
            authority,
            scenario_map[scenario_id],
            outcome,
            input_context=False,
        )
        if row.get("bounded_completion") != witness["result"]["bounded_completion"]:
            raise AgentAutonomousCompletionError(
                f"metric bounded completion is not witness-derived: {scenario_id}"
            )
        if row.get("context_digest") != witness["context"]["digest"]:
            raise AgentAutonomousCompletionError(
                f"metric context digest is not witness-derived: {scenario_id}"
            )
        if row.get("command_digest") != _digest_object(witness["command"]):
            raise AgentAutonomousCompletionError(
                f"metric command digest is not witness-derived: {scenario_id}"
            )
        if row.get("receipt_digest") != _digest_object(witness):
            raise AgentAutonomousCompletionError(
                f"metric receipt digest is not witness-derived: {scenario_id}"
            )
        if outcome in {"failed", "safe-stop"} and row.get("reason_code") != witness[
            "result"
        ]["reason_code"]:
            raise AgentAutonomousCompletionError(
                f"metric reason code is not witness-derived: {scenario_id}"
            )
        if outcome == "safe-stop" and row.get("completion_plan_digest") != _digest_object(
            witness["result"]["completion_plan"]
        ):
            raise AgentAutonomousCompletionError(
                f"metric completion plan is not witness-derived: {scenario_id}"
            )
    if population != {
        "denominator": len(scenario_ids),
        "completed": counts["completed"],
        "failed": counts["failed"],
        "safe_stop": counts["safe-stop"],
        "not_evaluated": counts["not-evaluated"],
    }:
        raise AgentAutonomousCompletionError("metric population does not match outcomes")
    evaluated = counts["not-evaluated"] == 0
    if report["status"] != ("evaluated" if evaluated else "not-evaluated"):
        raise AgentAutonomousCompletionError("metric status does not match outcome census")
    expected_disposition = (
        "execution-receipts-bound" if evaluated else "execution-receipts-required"
    )
    if report["evidence_disposition"] != expected_disposition:
        raise AgentAutonomousCompletionError(
            "metric evidence disposition does not match outcome census"
        )
    if report["status"] == "not-evaluated" and report["value_percent"] is not None:
        raise AgentAutonomousCompletionError("not-evaluated metric has a numeric rate")
    if report["status"] == "evaluated" and report["value_percent"] is None:
        raise AgentAutonomousCompletionError("evaluated metric has no numeric rate")
    expected_evidence_source = (
        "provided-receipts"
        if any(row["outcome"] != "not-evaluated" for row in report["outcomes"])
        else "none"
    )
    if report["provenance"]["evidence_source"] != expected_evidence_source:
        raise AgentAutonomousCompletionError(
            "metric evidence source does not match the receipt census"
        )
    expected_value = round(100.0 * counts["completed"] / len(scenario_ids), 6)
    if evaluated and report["value_percent"] != expected_value:
        raise AgentAutonomousCompletionError("metric value does not match outcome census")


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
