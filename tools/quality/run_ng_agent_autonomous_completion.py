#!/usr/bin/env python3
"""Execute receipt-bound #277 golden commands in isolated clean clones.

This is a mechanical evidence producer, not an agent and not a semantic
qualification authority.  A scenario is marked ``completed`` only when the
declared command actually exits successfully and emits an exact, scenario-
bound result receipt.  Missing, malformed, timed-out, or failed commands are
recorded as failed/safe-stop outcomes with their real process and stream
receipts; no digest-shaped placeholder can be promoted by this runner.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import signal
import subprocess
import sys
import tempfile
from typing import Any

import jsonschema
import yaml

import check_ng_agent_autonomous_completion as metric
import check_ng_agent_capability_resolution as capability


ROOT = pathlib.Path(__file__).resolve().parents[2]
INPUT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_agent_autonomous_completion_runner_input.schema.yaml"
)
GENERATOR = pathlib.Path("tools/quality/run_ng_agent_autonomous_completion.py")
INPUT_SCHEMA_ID = "cxxlens.agent-autonomous-completion-runner-input.v1"
RESULT_SCHEMA_ID = "cxxlens.agent-autonomous-completion-result.v1"
HEX40 = re.compile(r"^[0-9a-f]{40}$")
DIGEST = re.compile(r"^sha256:[0-9a-f]{64}$")
ENVIRONMENT_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
# The timeout is part of this generator's source authority rather than an
# unbound input field.  The emitted witness is consequently reproducible from
# the exact runner revision/tree and cannot silently acquire a caller-specific
# budget that is absent from the receipt contract.
EXECUTION_TIMEOUT_SECONDS = 900


class AutonomousCompletionRunnerError(ValueError):
    """A fail-closed input, authority, or execution binding violation."""


def _canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")


def _digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _digest_object(value: Any) -> str:
    return _digest_bytes(_canonical_bytes(value))


def _git(root: pathlib.Path, *arguments: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), *arguments],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise AutonomousCompletionRunnerError(
            f"git authority unavailable: {' '.join(arguments)}"
        ) from error
    return result.stdout.strip()


def _authority(root: pathlib.Path) -> dict[str, str]:
    return metric._authority(root)


def _require_clean_authority(root: pathlib.Path) -> None:
    status = _git(root, "status", "--porcelain=v1", "--untracked-files=all")
    if status:
        raise AutonomousCompletionRunnerError(
            "dirty authority checkout cannot produce execution evidence"
        )


def _load_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise AutonomousCompletionRunnerError(f"cannot read runner input: {path}") from error
    if not isinstance(value, dict):
        raise AutonomousCompletionRunnerError("runner input must be an object")
    return value


def _validate_schema(root: pathlib.Path, value: dict[str, Any]) -> None:
    try:
        schema = yaml.safe_load((root / INPUT_SCHEMA).read_text(encoding="utf-8"))
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(value)
    except (
        OSError,
        UnicodeError,
        yaml.YAMLError,
        jsonschema.SchemaError,
        jsonschema.ValidationError,
    ) as error:
        raise AutonomousCompletionRunnerError(
            f"runner input schema validation failed: {error}"
        ) from error


def _exact_text(value: Any, description: str) -> str:
    if not isinstance(value, str) or not value or "\x00" in value:
        raise AutonomousCompletionRunnerError(f"{description} is invalid")
    return value


def _normalize_command(value: Any, scenario_id: str) -> tuple[dict[str, Any], dict[str, str]]:
    if not isinstance(value, dict) or set(value) != {"argv", "environment"}:
        raise AutonomousCompletionRunnerError(
            f"runner command fields are not exact: {scenario_id}"
        )
    argv = value["argv"]
    if (
        not isinstance(argv, list)
        or not argv
        or any(not isinstance(argument, str) or not argument or "\x00" in argument for argument in argv)
    ):
        raise AutonomousCompletionRunnerError(f"runner argv is invalid: {scenario_id}")
    environment = value["environment"]
    if not isinstance(environment, list):
        raise AutonomousCompletionRunnerError(f"runner environment is invalid: {scenario_id}")
    values: dict[str, str] = {}
    for index, entry in enumerate(environment):
        if not isinstance(entry, dict) or set(entry) != {"name", "value"}:
            raise AutonomousCompletionRunnerError(
                f"runner environment fields are not exact: {scenario_id}[{index}]"
            )
        name = entry["name"]
        if not isinstance(name, str) or not ENVIRONMENT_NAME.fullmatch(name):
            raise AutonomousCompletionRunnerError(
                f"runner environment name is invalid: {scenario_id}[{index}]"
            )
        if name in values:
            raise AutonomousCompletionRunnerError(
                f"runner environment contains duplicate name: {scenario_id}:{name}"
            )
        values[name] = _exact_text(entry["value"], f"runner environment value {scenario_id}:{name}")
    if list(values) != sorted(values):
        raise AutonomousCompletionRunnerError(
            f"runner environment is not canonical: {scenario_id}"
        )
    normalized = {
        "argv": list(argv),
        "working_directory": "repository-root",
        "environment": [
            {"name": name, "value_digest": _digest_bytes(values[name].encode("utf-8"))}
            for name in sorted(values)
        ],
    }
    return normalized, values


def _validate_input(
    root: pathlib.Path, value: dict[str, Any], authority: dict[str, str]
) -> dict[str, dict[str, Any]]:
    _validate_schema(root, value)
    if value["schema"] != INPUT_SCHEMA_ID:
        raise AutonomousCompletionRunnerError("runner input schema identifier drift")
    expected_authority = {
        key: authority[key] for key in ("revision", "tree", "catalog_digest")
    }
    if value["authority"] != expected_authority:
        raise AutonomousCompletionRunnerError("runner input authority is stale or mismatched")
    scenarios = metric._scenario_set(metric._catalog(root))
    expected_ids = [row["scenario_id"] for row in scenarios]
    rows = value["scenarios"]
    if [row["scenario_id"] for row in rows] != expected_ids:
        raise AutonomousCompletionRunnerError(
            "runner scenario census/order does not match the canonical nine paths"
        )
    selected: dict[str, dict[str, Any]] = {}
    for row in rows:
        scenario_id = row["scenario_id"]
        command, environment = _normalize_command(row["command"], scenario_id)
        selected[scenario_id] = {
            "command": command,
            "environment_values": environment,
        }
    return selected


def _stream(value: bytes, *, complete: bool = True) -> dict[str, Any]:
    return {
        "byte_count": len(value),
        "digest": _digest_bytes(value),
        "complete": complete,
    }


def _result_payload(
    value: Any, scenario_id: str
) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != {
        "schema",
        "scenario_id",
        "outcome",
        "bounded_completion",
        "reason_code",
        "completion_plan",
    }:
        raise AutonomousCompletionRunnerError(
            f"execution result receipt fields are not exact: {scenario_id}"
        )
    if value["schema"] != RESULT_SCHEMA_ID or value["scenario_id"] != scenario_id:
        raise AutonomousCompletionRunnerError(
            f"execution result receipt identity mismatch: {scenario_id}"
        )
    outcome = value["outcome"]
    if outcome not in {"completed", "failed", "safe-stop"}:
        raise AutonomousCompletionRunnerError(
            f"execution result receipt outcome is invalid: {scenario_id}"
        )
    result = {
        "outcome": outcome,
        "bounded_completion": value["bounded_completion"],
        "reason_code": value["reason_code"],
        "completion_plan": value["completion_plan"],
    }
    # Reuse the metric's exact result-state contract and outcome-specific rules.
    return metric._normalize_result(result, scenario_id, outcome)


def _fallback_result(
    *, outcome: str, reason_code: str, completion_plan: list[str]
) -> dict[str, Any]:
    return {
        "outcome": outcome,
        "bounded_completion": False,
        "reason_code": reason_code,
        "completion_plan": completion_plan,
    }


def _make_execution_clone(
    root: pathlib.Path,
    destination: pathlib.Path,
    revision: str,
) -> pathlib.Path:
    clone = destination / "repository"
    try:
        subprocess.run(
            ["git", "clone", "--no-local", "--quiet", str(root), str(clone)],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run(
            ["git", "-C", str(clone), "checkout", "--quiet", "--detach", revision],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise AutonomousCompletionRunnerError(
            "could not construct an exact clean execution clone"
        ) from error
    return clone


def _reset_execution_clone(clone: pathlib.Path, revision: str) -> None:
    try:
        subprocess.run(
            ["git", "-C", str(clone), "reset", "--hard", "--quiet", revision],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run(
            ["git", "-C", str(clone), "clean", "-ffdqx", "--quiet"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise AutonomousCompletionRunnerError(
            "could not reset the isolated execution clone"
        ) from error


def _run_process(
    execution_root: pathlib.Path,
    command: dict[str, Any],
    environment: dict[str, str],
    timeout_seconds: int,
) -> tuple[dict[str, Any], bytes]:
    """Run one command in an already isolated clean clone."""
    try:
        process_handle = subprocess.Popen(
            command["argv"],
            cwd=execution_root,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
        try:
            stdout, stderr = process_handle.communicate(timeout=timeout_seconds)
        except subprocess.TimeoutExpired as error:
            try:
                os.killpg(process_handle.pid, signal.SIGKILL)
            except (OSError, ProcessLookupError):
                pass
            trailing_stdout, trailing_stderr = process_handle.communicate()
            stdout_value = trailing_stdout or error.stdout or b""
            stderr_value = trailing_stderr or error.stderr or b""
            if isinstance(stdout_value, str):
                stdout_value = stdout_value.encode("utf-8", errors="replace")
            if isinstance(stderr_value, str):
                stderr_value = stderr_value.encode("utf-8", errors="replace")
            return (
                {
                    "terminal_state": "timed-out",
                    "exit_status": None,
                    "signal": None,
                    "stdout": _stream(stdout_value, complete=False),
                    "stderr": _stream(stderr_value, complete=False),
                },
                stdout_value,
            )
        returncode = process_handle.returncode
        if returncode is None:
            raise AutonomousCompletionRunnerError("execution process has no terminal status")
        if returncode < 0:
            process = {
                "terminal_state": "signaled",
                "exit_status": None,
                "signal": -returncode,
                "stdout": _stream(stdout),
                "stderr": _stream(stderr),
            }
        else:
            process = {
                "terminal_state": "exited",
                "exit_status": returncode,
                "signal": None,
                "stdout": _stream(stdout),
                "stderr": _stream(stderr),
            }
        return process, stdout
    except OSError as error:
        stderr = str(error).encode("utf-8", errors="replace")
        return (
            {
                "terminal_state": "launch-failed",
                "exit_status": None,
                "signal": None,
                "stdout": _stream(b""),
                "stderr": _stream(stderr),
            },
            b"",
        )


def _execute_scenario(
    root: pathlib.Path,
    execution_root: pathlib.Path,
    authority: dict[str, str],
    scenario: dict[str, str],
    selection: dict[str, Any],
) -> dict[str, Any]:
    scenario_id = scenario["scenario_id"]
    command = selection["command"]
    try:
        process, stdout = _run_process(
            execution_root,
            command,
            selection["environment_values"],
            EXECUTION_TIMEOUT_SECONDS,
        )
    except AutonomousCompletionRunnerError:
        raise
    result: dict[str, Any]
    if process["terminal_state"] in {"timed-out", "launch-failed", "signaled"}:
        result = _fallback_result(
            outcome="safe-stop",
            reason_code=f"execution-{process['terminal_state']}",
            completion_plan=[
                "Provide a bounded runnable result receipt and rerun the exact scenario command."
            ],
        )
        outcome = "safe-stop"
    else:
        try:
            decoded = json.loads(stdout.decode("utf-8"))
            result = _result_payload(decoded, scenario_id)
            outcome = result["outcome"]
            if outcome == "completed" and process["exit_status"] != 0:
                raise AutonomousCompletionRunnerError(
                    f"completed result exited unsuccessfully: {scenario_id}"
                )
        except (
            AutonomousCompletionRunnerError,
            UnicodeDecodeError,
            json.JSONDecodeError,
        ):
            result = _fallback_result(
                outcome="failed",
                reason_code=(
                    "execution-failed"
                    if process["exit_status"] != 0
                    else "execution-result-invalid"
                ),
                completion_plan=[],
            )
            outcome = "failed"
    witness = {
        "schema": "cxxlens.agent-autonomous-completion-execution-witness.v1",
        "scenario_id": scenario_id,
        "authority": {
            key: authority[key] for key in ("revision", "tree", "catalog_digest")
        },
        "context": capability.build_resolution(root, scenario_id),
        "command": command,
        "process": process,
        "result": result,
    }
    normalized = metric._normalize_execution_witness(
        root,
        witness,
        authority,
        scenario,
        outcome,
        input_context=True,
    )
    row: dict[str, Any] = {
        "scenario_id": scenario_id,
        "outcome": outcome,
        "bounded_completion": normalized["result"]["bounded_completion"],
        "context_digest": normalized["context"]["digest"],
        "command_digest": _digest_object(normalized["command"]),
        "receipt_digest": _digest_object(normalized),
        "execution_witness": witness,
    }
    if outcome in {"failed", "safe-stop"}:
        row["reason_code"] = normalized["result"]["reason_code"]
    if outcome == "safe-stop":
        row["completion_plan_digest"] = _digest_object(
            normalized["result"]["completion_plan"]
        )
    return row


def run(root: pathlib.Path, input_value: dict[str, Any]) -> dict[str, Any]:
    """Execute all nine declared paths and return evidence v2."""
    root = root.resolve()
    _require_clean_authority(root)
    authority = _authority(root)
    selected = _validate_input(root, input_value, authority)
    scenarios = metric._scenario_set(metric._catalog(root))
    with tempfile.TemporaryDirectory(prefix="cxxlens-agent-golden-") as temporary:
        execution_root = _make_execution_clone(
            root, pathlib.Path(temporary), authority["revision"]
        )
        rows = []
        for scenario in scenarios:
            _reset_execution_clone(execution_root, authority["revision"])
            rows.append(
                _execute_scenario(
                    root,
                    execution_root,
                    authority,
                    scenario,
                    selected[scenario["scenario_id"]],
                )
            )
    evidence = {
        "schema": "cxxlens.agent-autonomous-completion-evidence.v2",
        "authority": {
            key: authority[key] for key in ("revision", "tree", "catalog_digest")
        },
        "scenarios": rows,
    }
    # Validate the complete receipt census before exposing it to the caller.
    report = metric._report(root, evidence=evidence)
    metric.validate_report(root, report)
    return evidence


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("run", choices=("run",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--input-json", type=pathlib.Path, required=True)
    parser.add_argument("--output-evidence", type=pathlib.Path, required=True)
    parser.add_argument("--output-report", type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        root = args.root.resolve()
        evidence = run(root, _load_json(args.input_json))
        args.output_evidence.write_text(
            json.dumps(evidence, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
            encoding="utf-8",
        )
        report = metric._report(root, evidence=evidence)
        if args.output_report is not None:
            args.output_report.write_text(
                json.dumps(report, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
                encoding="utf-8",
            )
        print(json.dumps(report, ensure_ascii=False, sort_keys=True, indent=2))
        return 0
    except (AutonomousCompletionRunnerError, metric.AgentAutonomousCompletionError, OSError) as error:
        print(f"agent-autonomous-completion-runner: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
