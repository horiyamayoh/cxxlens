#!/usr/bin/env python3
"""Resolve minimal API prompts and run only authorized atomic-unit CI shards."""

from __future__ import annotations

import argparse
import copy
import datetime
import hashlib
import json
import os
import pathlib
import subprocess
import sys

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "agent"))

from ownership_generator import OwnershipError, validate_changed_paths  # noqa: E402
from ready_evaluator import ReadyError, parse_prompt, resolve_api  # noqa: E402


class RunnerError(ValueError):
    """An API task runner refusal with a stable code."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code


def fail(code: str, message: str) -> None:
    raise RunnerError(code, message)


def reject_superseded_legacy_dispatch(root: pathlib.Path, mode: str) -> None:
    """Refuse execution once the next-generation authority transition is active."""
    if mode not in {"run", "integrate"}:
        return
    transition_path = root / "schemas/cxxlens_ng_authority_transition.yaml"
    if not transition_path.is_file():
        return
    transition = yaml.safe_load(transition_path.read_text(encoding="utf-8"))
    if not isinstance(transition, dict):
        fail("runner.legacy-authority-invalid", str(transition_path))
    dispatch = transition.get("dispatch", {})
    if (
        transition.get("state") == "active"
        and dispatch.get("legacy_atomic_unit_runner") == "revoked"
    ):
        fail(
            "runner.legacy-authority-superseded",
            "legacy 124-API Phase C dispatch was revoked by #57; follow #56",
        )


def authorize_run(resolution: dict) -> None:
    if resolution["state"] not in {"ready", "complete"}:
        fail(
            "runner.not-ready",
            f"{resolution['api_id']} state={resolution['state']} "
            f"blockers={resolution['blockers']}",
        )


def authorize_integration(shard: dict) -> None:
    if shard["state"] != "verification":
        fail(
            "runner.package-integration-blocked",
            f"{shard['package_id']} blocked_units={shard['blocked_atomic_units']}",
        )


def load_json(path: pathlib.Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def select_api_id(args: argparse.Namespace, report: dict) -> str:
    if args.api_id:
        return args.api_id
    if args.prompt:
        return parse_prompt(args.prompt)
    if args.unit:
        node = next(
            (item for item in report["nodes"] if item["atomic_unit_id"] == args.unit),
            None,
        )
        if node is None:
            fail("runner.unknown-unit", args.unit)
        return node["api_ids"][0]
    fail("runner.arguments", "one of --api-id, --prompt, or --unit is required")


def canonical_json(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest(value: object) -> str:
    return digest_bytes(canonical_json(value).encode("utf-8"))


def current_input_sha(root: pathlib.Path) -> str:
    completed = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def result_path(root: pathlib.Path, relative: str) -> pathlib.Path:
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root)
    except ValueError:
        fail("runner.unsafe-result-path", relative)
    return candidate


def validate_artifact_digest(artifact: dict) -> bool:
    unsigned = copy.deepcopy(artifact)
    actual = unsigned.pop("artifact_digest", None)
    return actual == digest(unsigned)


def verify_conformant_artifacts(
    root: pathlib.Path, shard: dict, input_sha: str
) -> list[dict]:
    verified = []
    for expected in shard["conformant_unit_artifacts"]:
        path = result_path(root, expected["execution_result_path"])
        if not path.is_file():
            fail(
                "runner.unit-artifact-missing",
                f"{expected['atomic_unit_id']}: {expected['execution_result_path']}",
            )
        artifact = load_json(path)
        valid = (
            artifact.get("schema") == "cxxlens.api-ready.execution-result.v1"
            and artifact.get("subject_kind") == "atomic_unit"
            and artifact.get("subject_id") == expected["atomic_unit_id"]
            and artifact.get("shard_id") == expected["shard_id"]
            and artifact.get("shard_digest") == expected["shard_digest"]
            and artifact.get("status") == expected["required_execution_status"]
            and artifact.get("input_sha") == input_sha
            and validate_artifact_digest(artifact)
        )
        if not valid:
            fail("runner.unit-artifact-nonconformant", expected["atomic_unit_id"])
        verified.append(
            {
                "atomic_unit_id": expected["atomic_unit_id"],
                "path": expected["execution_result_path"],
                "artifact_digest": artifact["artifact_digest"],
            }
        )
    return verified


def execute_shard(
    root: pathlib.Path,
    shard: dict,
    subject_kind: str,
    verified_unit_artifacts: list[dict] | None = None,
) -> dict:
    started = datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")
    input_sha = current_input_sha(root)
    step_results = []
    failed_step = None
    for step in shard["execution_steps"]:
        command = step["command"]
        environment = os.environ.copy()
        environment.update(command["environment"])
        completed = subprocess.run(
            command["argv"],
            cwd=root,
            env=environment,
            check=False,
            capture_output=True,
        )
        sys.stdout.buffer.write(completed.stdout)
        sys.stderr.buffer.write(completed.stderr)
        step_results.append(
            {
                "id": step["id"],
                "scope": step["scope"],
                "fixture_category": step["fixture_category"],
                "evidence_paths": step["evidence_paths"],
                "command": command,
                "exit_status": completed.returncode,
                "stdout_digest": digest_bytes(completed.stdout),
                "stderr_digest": digest_bytes(completed.stderr),
            }
        )
        if completed.returncode != 0:
            failed_step = step["id"]
            break
    artifact = {
        "schema": "cxxlens.api-ready.execution-result.v1",
        "subject_kind": subject_kind,
        "subject_id": (
            shard["atomic_unit_id"]
            if subject_kind == "atomic_unit"
            else shard["package_id"]
        ),
        "input_sha": input_sha,
        "shard_id": shard["id"],
        "shard_digest": shard["semantic_digest"],
        "status": "failed" if failed_step else "passed",
        "started_at": started,
        "ended_at": datetime.datetime.now(datetime.timezone.utc)
        .isoformat()
        .replace("+00:00", "Z"),
        "steps": step_results,
        "verified_unit_artifacts": verified_unit_artifacts or [],
    }
    artifact["artifact_digest"] = digest(artifact)
    schema = yaml.safe_load(
        (root / "schemas/cxxlens.api-ready.execution-result.v1.schema.yaml").read_text(
            encoding="utf-8"
        )
    )
    jsonschema.Draft202012Validator(schema).validate(artifact)
    path = result_path(root, shard["execution_result_path"])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(artifact, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    if failed_step:
        fail("runner.shard-failed", f"{failed_step}: result={shard['execution_result_path']}")
    return artifact


def select_integration_shard(report: dict, package_id: str | None) -> dict:
    if not package_id:
        fail("runner.arguments", "--package-id is required for integrate")
    shard = next(
        (
            item
            for item in report["package_integration_shards"]
            if item["package_id"] == package_id
        ),
        None,
    )
    if shard is None:
        fail("runner.unknown-package", package_id)
    return shard


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("resolve", "run", "integrate"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--api-id")
    parser.add_argument("--prompt")
    parser.add_argument("--unit")
    parser.add_argument("--package-id")
    parser.add_argument("--changed-path", action="append", default=[])
    parser.add_argument("--execute", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    reject_superseded_legacy_dispatch(root, args.mode)
    report = load_json(root / "schemas/cxxlens.api-ready.report.v1.json")
    corpus = load_json(root / "schemas/cxxlens.agent-task-packet-corpus.v1.json")
    ownership = load_json(root / "schemas/cxxlens.agent-ownership.v1.json")
    if args.mode == "integrate":
        if not args.execute:
            fail("runner.execution-required", "integrate requires --execute")
        integration = select_integration_shard(report, args.package_id)
        authorize_integration(integration)
        if args.changed_path:
            validate_changed_paths(
                ownership,
                integration["package_integration_role"],
                args.changed_path,
            )
        input_sha = current_input_sha(root)
        verified = verify_conformant_artifacts(root, integration, input_sha)
        artifact = execute_shard(root, integration, "package_integration", verified)
        print(json.dumps(artifact, ensure_ascii=False, indent=2, sort_keys=True))
        return 0
    run_schema = yaml.safe_load(
        (root / "schemas/cxxlens.api-ready.run-manifest.v1.schema.yaml").read_text(
            encoding="utf-8"
        )
    )
    api_id = select_api_id(args, report)
    resolution = resolve_api(api_id, report, corpus, ownership)
    jsonschema.Draft202012Validator(run_schema).validate(resolution)
    if args.mode == "run":
        authorize_run(resolution)
        if not args.execute:
            fail("runner.execution-required", "run requires --execute")
        if args.changed_path:
            validate_changed_paths(
                ownership, resolution["atomic_unit_id"], args.changed_path
            )
        shard = next(item for item in report["shards"] if item["id"] == resolution["shard_id"])
        artifact = execute_shard(root, shard, "atomic_unit")
        print(json.dumps(artifact, ensure_ascii=False, indent=2, sort_keys=True))
        return 0
    if args.execute:
        fail("runner.arguments", "resolve does not execute a shard")
    print(json.dumps(resolution, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        json.JSONDecodeError,
        jsonschema.ValidationError,
        OSError,
        OwnershipError,
        ReadyError,
        RunnerError,
        subprocess.CalledProcessError,
        yaml.YAMLError,
    ) as error:
        print(f"API task runner failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
