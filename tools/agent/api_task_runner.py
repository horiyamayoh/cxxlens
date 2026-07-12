#!/usr/bin/env python3
"""Resolve minimal API prompts and run only authorized atomic-unit CI shards."""

from __future__ import annotations

import argparse
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


def authorize_run(resolution: dict) -> None:
    if not resolution["start_authorized"]:
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


def execute_shard(root: pathlib.Path, shard: dict) -> None:
    for command in shard["commands"]:
        environment = os.environ.copy()
        environment.update(command["environment"])
        subprocess.run(command["argv"], cwd=root, env=environment, check=True)


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
    report = load_json(root / "schemas/cxxlens.api-ready.report.v1.json")
    corpus = load_json(root / "schemas/cxxlens.agent-task-packet-corpus.v1.json")
    ownership = load_json(root / "schemas/cxxlens.agent-ownership.v1.json")
    if args.mode == "integrate":
        integration = select_integration_shard(report, args.package_id)
        authorize_integration(integration)
        if args.changed_path:
            validate_changed_paths(
                ownership,
                integration["package_integration_role"],
                args.changed_path,
            )
        if args.execute:
            execute_shard(root, integration)
        print(json.dumps(integration, ensure_ascii=False, indent=2, sort_keys=True))
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
        if args.changed_path:
            validate_changed_paths(
                ownership, resolution["atomic_unit_id"], args.changed_path
            )
        if args.execute:
            shard = next(item for item in report["shards"] if item["id"] == resolution["shard_id"])
            execute_shard(root, shard)
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
