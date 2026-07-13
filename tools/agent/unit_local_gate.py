#!/usr/bin/env python3
"""Resolve declared unit evidence to exact CTest names and execute only those tests."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import shlex
import subprocess
import sys
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[2]
TARGET = re.compile(r"(?:^|/)CMakeFiles/([^/]+)[.]dir(?:/|$)")


class UnitLocalGateError(ValueError):
    """A unit-local target resolution or execution failure."""


def fail(code: str, message: str) -> None:
    raise UnitLocalGateError(f"{code}: {message}")


def safe_path(root: pathlib.Path, relative: str) -> pathlib.Path:
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root)
    except ValueError:
        fail("unit_local.unsafe-evidence", relative)
    if not candidate.is_file():
        fail("unit_local.evidence-missing", relative)
    return candidate


def compile_targets(compile_commands: list[dict[str, Any]]) -> dict[pathlib.Path, str]:
    targets: dict[pathlib.Path, str] = {}
    for row in compile_commands:
        source = pathlib.Path(row["file"]).resolve()
        output = str(row.get("output", ""))
        if not output:
            arguments = row.get("arguments")
            if not isinstance(arguments, list):
                command = row.get("command", "")
                arguments = shlex.split(command) if isinstance(command, str) else []
            output = next(
                (
                    str(arguments[index + 1])
                    for index, argument in enumerate(arguments[:-1])
                    if argument == "-o"
                ),
                "",
            )
        output = output.replace("\\", "/")
        match = TARGET.search(output)
        if match:
            targets[source] = match.group(1)
    return targets


def resolve_ctest_targets(
    root: pathlib.Path,
    evidence_paths: list[str],
    compile_commands: list[dict[str, Any]],
    ctest_document: dict[str, Any],
) -> list[str]:
    evidence = {safe_path(root, path): path for path in evidence_paths}
    compiled = compile_targets(compile_commands)
    tests = ctest_document.get("tests", [])
    resolved_by_evidence: dict[str, set[str]] = {
        relative: set() for relative in evidence.values()
    }
    for test in tests:
        name = test.get("name")
        command = [str(item) for item in test.get("command", [])]
        if not isinstance(name, str) or not name:
            continue
        executable = pathlib.Path(command[0]).name if command else ""
        for path, relative in evidence.items():
            if compiled.get(path) == executable:
                resolved_by_evidence[relative].add(name)
                continue
            for argument in command[1:]:
                argument_path = pathlib.Path(argument)
                if argument_path.is_absolute() and argument_path.resolve() == path:
                    resolved_by_evidence[relative].add(name)
                    break
    unresolved = sorted(
        path for path, test_names in resolved_by_evidence.items() if not test_names
    )
    if unresolved:
        fail("unit_local.target-unresolved", f"unmapped evidence: {unresolved}")
    selected = sorted(
        test_name
        for test_names in resolved_by_evidence.values()
        for test_name in test_names
    )
    if not selected:
        fail("unit_local.target-missing", "no executable evidence was declared")
    return sorted(set(selected))


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("run", "check-report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--build-dir", type=pathlib.Path, default=pathlib.Path("build/dev-clang"))
    parser.add_argument("--unit")
    parser.add_argument("--category", choices=("positive", "negative", "ambiguous"))
    parser.add_argument("--evidence", action="append", default=[])
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    build_dir = args.build_dir if args.build_dir.is_absolute() else root / args.build_dir
    compile_commands = json.loads(
        (build_dir / "compile_commands.json").read_text(encoding="utf-8")
    )
    shown = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        check=True,
        capture_output=True,
        text=True,
    )
    ctest_document = json.loads(shown.stdout)
    if args.mode == "check-report":
        report = json.loads(
            (root / "schemas/cxxlens.api-ready.report.v1.json").read_text(
                encoding="utf-8"
            )
        )
        checked = 0
        for shard in report["shards"]:
            if shard["state"] == "blocked":
                continue
            for step in shard["execution_steps"]:
                if step["scope"] != "unit_local":
                    continue
                try:
                    resolve_ctest_targets(
                        root,
                        step["evidence_paths"],
                        compile_commands,
                        ctest_document,
                    )
                except UnitLocalGateError as error:
                    fail(
                        "unit_local.report-target-invalid",
                        f"{shard['atomic_unit_id']}/{step['fixture_category']}: {error}",
                    )
                checked += 1
        print(f"validated {checked} executable atomic-unit fixture targets")
        return 0
    if not args.unit or not args.category:
        fail("unit_local.arguments", "run requires --unit and --category")
    if not args.evidence:
        fail("unit_local.target-missing", f"{args.unit}/{args.category}")
    selected = resolve_ctest_targets(
        root, sorted(set(args.evidence)), compile_commands, ctest_document
    )
    expression = "^(" + "|".join(re.escape(name) for name in selected) + ")$"
    print(
        json.dumps(
            {
                "schema": "cxxlens.unit-local-selection.v1",
                "atomic_unit_id": args.unit,
                "fixture_category": args.category,
                "evidence_paths": sorted(set(args.evidence)),
                "ctest_names": selected,
            },
            ensure_ascii=False,
            sort_keys=True,
        )
    )
    subprocess.run(
        [
            "ctest",
            "--test-dir",
            str(build_dir),
            "--output-on-failure",
            "--no-tests=error",
            "--tests-regex",
            expression,
        ],
        check=True,
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        json.JSONDecodeError,
        OSError,
        subprocess.CalledProcessError,
        UnitLocalGateError,
    ) as error:
        print(f"unit-local gate failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
