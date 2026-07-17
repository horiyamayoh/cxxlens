#!/usr/bin/env python3
"""Validate quality ownership, evidence binding, and fail-closed selection."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys
from typing import Any

import jsonschema
import yaml

from collect_toolchain_provenance import pinned_actions, provenance_digest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = pathlib.Path("schemas/cxxlens_ng_quality_ownership.yaml")
SCHEMA = pathlib.Path("schemas/cxxlens_ng_quality_ownership.schema.yaml")
EVIDENCE_SCHEMA = pathlib.Path("schemas/cxxlens_ng_quality_evidence.schema.yaml")
EVIDENCE_FIELDS = (
    "logical_check_id",
    "check_version",
    "revision",
    "tree",
    "toolchain_digest",
    "configuration_digest",
    "checker_digest",
    "input_digest",
    "output_digest",
)


class QualityOwnershipError(ValueError):
    """A fail-closed quality ownership violation."""


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise QualityOwnershipError(f"expected mapping: {path}")
    return value


def canonical_digest(value: Any) -> str:
    encoded = json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def evidence_id(record: dict[str, Any]) -> str:
    missing = [field for field in EVIDENCE_FIELDS if field not in record]
    if missing:
        raise QualityOwnershipError(f"evidence binding fields are missing: {missing}")
    projection = {field: record[field] for field in EVIDENCE_FIELDS}
    return "quality-evidence:" + canonical_digest(projection).removeprefix("sha256:")


def validate_evidence(
    records: list[dict[str, Any]], required_instances: set[tuple[str, str]]
) -> dict[tuple[str, str], dict[str, Any]]:
    by_instance: dict[tuple[str, str], dict[str, Any]] = {}
    seen_ids: set[str] = set()
    for record in records:
        identifier = evidence_id(record)
        if record.get("evidence_id") != identifier:
            raise QualityOwnershipError("evidence ID does not match its bound fields")
        if identifier in seen_ids:
            raise QualityOwnershipError(f"duplicate logical evidence ID: {identifier}")
        seen_ids.add(identifier)
        configuration = record.get("configuration")
        if record.get("configuration_digest") != canonical_digest(configuration):
            raise QualityOwnershipError("configuration digest does not match configuration")
        instance = (record["logical_check_id"], configuration)
        if instance in by_instance:
            raise QualityOwnershipError(
                f"logical check configuration was generated more than once: {instance}"
            )
        by_instance[instance] = record
    missing = required_instances - set(by_instance)
    extra = set(by_instance) - required_instances
    if missing or extra:
        raise QualityOwnershipError(
            f"evidence instance set differs: missing={sorted(missing)}, extra={sorted(extra)}"
        )
    if records:
        revisions = {(row["revision"], row["tree"]) for row in records}
        if len(revisions) != 1:
            raise QualityOwnershipError("evidence records do not bind one revision/tree")
    return by_instance


def normalized_path_digest(root: pathlib.Path, paths: list[str]) -> str:
    entries: list[tuple[str, str, str]] = []
    for raw in sorted(set(paths)):
        candidate = root / raw
        if not candidate.exists() and not candidate.is_symlink():
            raise QualityOwnershipError(f"evidence input/output does not exist: {raw}")
        members = [candidate]
        if candidate.is_dir():
            members.extend(
                path
                for path in sorted(candidate.rglob("*"))
                if "__pycache__" not in path.parts and path.suffix != ".pyc"
            )
        for member in members:
            relative = member.relative_to(root).as_posix()
            if member.is_symlink():
                entries.append((relative, "symlink", os.readlink(member)))
            elif member.is_file():
                entries.append((relative, "file", hashlib.sha256(member.read_bytes()).hexdigest()))
            elif member.is_dir():
                entries.append((relative, "directory", ""))
    return canonical_digest(entries)


def git_value(root: pathlib.Path, expression: str) -> str:
    completed = subprocess.run(
        ["git", "rev-parse", expression],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def manifest_check(manifest: dict[str, Any], identifier: str) -> dict[str, Any]:
    matches = [row for row in manifest["checks"] if row["id"] == identifier]
    if len(matches) != 1:
        raise QualityOwnershipError(f"unknown or duplicate logical check ID: {identifier}")
    return matches[0]


def make_evidence(
    root: pathlib.Path,
    manifest: dict[str, Any],
    *,
    check_id: str,
    configuration: str,
    toolchain_provenance: pathlib.Path,
    checker_paths: list[str],
    output_paths: list[str],
) -> dict[str, Any]:
    check = manifest_check(manifest, check_id)
    if configuration not in check["configurations"]:
        raise QualityOwnershipError(
            f"configuration is not owned by {check_id}: {configuration}"
        )
    provenance = json.loads(toolchain_provenance.read_text(encoding="utf-8"))
    toolchain_digest = provenance.get("digest")
    if not isinstance(toolchain_digest, str):
        raise QualityOwnershipError("toolchain provenance has no digest")
    if toolchain_digest != provenance_digest(provenance):
        raise QualityOwnershipError("toolchain provenance digest mismatch")
    if provenance.get("source") != {
        "revision": git_value(root, "HEAD"),
        "tree": git_value(root, "HEAD^{tree}"),
    }:
        raise QualityOwnershipError("toolchain provenance revision/tree mismatch")
    supply_chain = provenance.get("supply_chain")
    if not isinstance(supply_chain, dict):
        raise QualityOwnershipError("toolchain provenance has no supply-chain binding")
    expected_supply_chain = {
        "lock_digest": "sha256:"
        + hashlib.sha256(
            (root / "tools/ci/llvm22-noble.lock.json").read_bytes()
        ).hexdigest(),
        "requirements_digest": "sha256:"
        + hashlib.sha256(
            (root / "tools/quality/requirements.lock").read_bytes()
        ).hexdigest(),
    }
    for field, expected in expected_supply_chain.items():
        if supply_chain.get(field) != expected:
            raise QualityOwnershipError(
                f"toolchain provenance {field} differs from repository lock"
            )
    runner = provenance.get("runner")
    if not isinstance(runner, dict) or any(
        field not in runner
        for field in (
            "label",
            "image_os",
            "image_version",
            "architecture",
            "os_release_digest",
            "kernel",
        )
    ):
        raise QualityOwnershipError("toolchain provenance runner identity is incomplete")
    if provenance.get("actions") != pinned_actions(root):
        raise QualityOwnershipError("toolchain provenance action set mismatch")
    checker_inputs = checker_paths or [str(pathlib.Path(__file__).relative_to(root))]
    checker_projection = {
        "command": check["command"],
        "files": normalized_path_digest(root, checker_inputs),
    }
    output_digest = (
        normalized_path_digest(root, output_paths)
        if output_paths
        else canonical_digest({"result": "passed"})
    )
    record = {
        "schema": "cxxlens.quality-evidence.v1",
        "logical_check_id": check_id,
        "check_version": check["version"],
        "configuration": configuration,
        "revision": provenance["source"]["revision"],
        "tree": provenance["source"]["tree"],
        "toolchain_digest": toolchain_digest,
        "configuration_digest": canonical_digest(configuration),
        "checker_digest": canonical_digest(checker_projection),
        "input_digest": normalized_path_digest(root, check["inputs"]),
        "output_digest": output_digest,
        "result": "passed",
    }
    record["evidence_id"] = evidence_id(record)
    jsonschema.Draft202012Validator(load_yaml(root / EVIDENCE_SCHEMA)).validate(record)
    return record


def required_instances(manifest: dict[str, Any], mode: str) -> set[tuple[str, str]]:
    owners = set(manifest["modes"][mode]["owners"])
    return {
        (row["id"], configuration)
        for row in manifest["checks"]
        if row["owner"] in owners
        for configuration in row["configurations"]
    }


def owner_instances(
    manifest: dict[str, Any], owners: set[str]
) -> set[tuple[str, str]]:
    return {
        (row["id"], configuration)
        for row in manifest["checks"]
        if row["owner"] in owners
        for configuration in row["configurations"]
    }


def load_evidence_directory(root: pathlib.Path, directory: pathlib.Path) -> list[dict[str, Any]]:
    schema = load_yaml(root / EVIDENCE_SCHEMA)
    records = []
    for path in sorted(directory.rglob("*.evidence.json")):
        record = json.loads(path.read_text(encoding="utf-8"))
        jsonschema.Draft202012Validator(schema).validate(record)
        records.append(record)
    return records


def select_mode(paths: list[str], *, graph_available: bool = True) -> str:
    if not graph_available or not paths:
        return "full"
    selected = "fast"
    for raw_path in paths:
        path = raw_path.replace("\\", "/").lstrip("./")
        if (
            path in {"CMakeLists.txt", "CMakePresets.json"}
            or path.startswith(("cmake/", ".github/", "schemas/", "include/"))
            or path == "tools/quality/check_quality_ownership.py"
        ):
            return "full"
        if path.startswith(("src/", "tests/", "examples/")):
            continue
        if path.startswith("docs/") or path in {"README.md", "CONTRIBUTING.md"}:
            selected = "check"
            continue
        return "full"
    return selected


def validate_manifest(root: pathlib.Path, manifest: dict[str, Any]) -> None:
    try:
        jsonschema.Draft202012Validator(load_yaml(root / SCHEMA)).validate(manifest)
    except jsonschema.ValidationError as error:
        raise QualityOwnershipError(
            f"quality ownership schema validation failed: {error.message}"
        ) from error

    checks = manifest["checks"]
    identifiers = [row["id"] for row in checks]
    if len(identifiers) != len(set(identifiers)):
        raise QualityOwnershipError("quality ownership contains duplicate check IDs")
    for row in checks:
        for relative in row["inputs"]:
            if not (root / relative).exists():
                raise QualityOwnershipError(
                    f"quality input does not exist: {row['id']} -> {relative}"
                )
        if len(row["configurations"]) != len(set(row["configurations"])):
            raise QualityOwnershipError(
                f"quality configurations contain duplicates: {row['id']}"
            )

    devtools = (root / "cmake/CxxlensDeveloperTools.cmake").read_text(encoding="utf-8")
    if "tests/quality/test_ng_" in devtools or "test_sanitizer_coverage.py" in devtools:
        raise QualityOwnershipError("quality target re-executes CTest-owned unit tests")
    if "add_dependencies(cxxlens-quality cxxlens-clang-tidy)" in devtools:
        raise QualityOwnershipError("quality target re-executes nightly-owned clang-tidy")
    presets = (root / "CMakePresets.json").read_text(encoding="utf-8")
    if '"jobs": 4' in presets:
        raise QualityOwnershipError("build presets retain the fixed jobs=4 policy")
    tests = (root / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    for required in (
        "unit.sdk.registration",
        "install.prepare",
        "install.relocation",
        "install.core-consumer",
        "install.sdk-consumer",
        "install.clang22-sdk-consumer",
        "install.examples-consumer",
        "install.shared-runtime-layout",
        "install.legacy-zero",
    ):
        if required not in tests and required.replace("install.", "") not in tests:
            raise QualityOwnershipError(f"selectable test is not registered: {required}")
    if "add_test(NAME install.consumer" in tests:
        raise QualityOwnershipError("monolithic install.consumer is still registered")
    workflow_text = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted((root / ".github/workflows").glob("*.yml"))
    )
    for row in checks:
        if f"--check-id {row['id']}" not in workflow_text:
            raise QualityOwnershipError(
                f"logical check does not emit workflow evidence: {row['id']}"
            )
        for configuration in row["configurations"]:
            if configuration not in workflow_text:
                raise QualityOwnershipError(
                    f"owned configuration is absent from workflows: {row['id']} -> {configuration}"
                )
    if "--mode full --evidence-dir" not in workflow_text:
        raise QualityOwnershipError("full workflow does not aggregate exact evidence")
    if "--owner nightly --evidence-dir" not in workflow_text:
        raise QualityOwnershipError("nightly workflow does not aggregate exact evidence")
    try:
        pinned_actions(root)
    except ValueError as error:
        raise QualityOwnershipError(str(error)) from error

    if manifest["modes"]["fast"]["final_sha"]:
        raise QualityOwnershipError("fast mode cannot qualify a final SHA")
    if not manifest["modes"]["full"]["final_sha"]:
        raise QualityOwnershipError("full mode must qualify the final SHA")
    if select_mode(["include/cxxlens/sdk.hpp"]) != "full":
        raise QualityOwnershipError("public-header selection is not fail closed")
    if select_mode(["unknown/new.asset"]) != "full":
        raise QualityOwnershipError("unknown-file selection is not fail closed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "select", "evidence", "aggregate"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--check-id")
    parser.add_argument("--configuration")
    parser.add_argument("--toolchain-provenance", type=pathlib.Path)
    parser.add_argument("--checker", action="append", default=[])
    parser.add_argument("--output", action="append", default=[])
    parser.add_argument("--evidence-output", type=pathlib.Path)
    parser.add_argument("--evidence-dir", type=pathlib.Path)
    parser.add_argument("--mode", choices=("fast", "check", "full", "stress"))
    parser.add_argument(
        "--owner",
        action="append",
        choices=("ctest", "quality-target", "build-test", "install-job", "gcc-header-job", "nightly"),
        default=[],
    )
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("paths", nargs="*")
    args = parser.parse_args()
    try:
        root = args.root.resolve()
        if args.command == "select":
            print(select_mode(args.paths))
            return 0
        manifest = load_yaml(root / MANIFEST)
        validate_manifest(root, manifest)
        if args.command == "evidence":
            if not all(
                (args.check_id, args.configuration, args.toolchain_provenance, args.evidence_output)
            ):
                raise QualityOwnershipError("evidence command requires check, configuration, provenance, and output")
            record = make_evidence(
                root,
                manifest,
                check_id=args.check_id,
                configuration=args.configuration,
                toolchain_provenance=args.toolchain_provenance.resolve(),
                checker_paths=args.checker,
                output_paths=args.output,
            )
            args.evidence_output.parent.mkdir(parents=True, exist_ok=True)
            args.evidence_output.write_text(
                json.dumps(record, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            print(record["evidence_id"])
            return 0
        if args.command == "aggregate":
            if not args.evidence_dir or not args.report or bool(args.mode) == bool(args.owner):
                raise QualityOwnershipError(
                    "aggregate command requires evidence directory, report, and exactly one of mode or owner"
                )
            records = load_evidence_directory(root, args.evidence_dir.resolve())
            expected = (
                required_instances(manifest, args.mode)
                if args.mode
                else owner_instances(manifest, set(args.owner))
            )
            accepted = validate_evidence(records, expected)
            current_source = (git_value(root, "HEAD"), git_value(root, "HEAD^{tree}"))
            if records and (records[0]["revision"], records[0]["tree"]) != current_source:
                raise QualityOwnershipError(
                    "evidence revision/tree does not match aggregation checkout"
                )
            report = {
                "schema": "cxxlens.quality-evidence-set.v1",
                "mode": args.mode or "owners:" + ",".join(sorted(args.owner)),
                "revision": records[0]["revision"] if records else "",
                "tree": records[0]["tree"] if records else "",
                "evidence_ids": sorted(row["evidence_id"] for row in accepted.values()),
                "set_digest": canonical_digest(
                    sorted(row["evidence_id"] for row in accepted.values())
                ),
                "result": "passed",
            }
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(
                json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            print(report["set_digest"])
            return 0
    except (
        OSError,
        subprocess.CalledProcessError,
        json.JSONDecodeError,
        jsonschema.ValidationError,
        QualityOwnershipError,
        yaml.YAMLError,
    ) as error:
        print(f"quality ownership check failed: {error}", file=sys.stderr)
        return 1
    print("quality ownership check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
