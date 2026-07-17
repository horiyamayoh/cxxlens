#!/usr/bin/env python3
"""Validate the fail-closed CI bootstrap, locks, workflows, and provenance wiring."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONTRACT = pathlib.Path("schemas/cxxlens_ng_ci_supply_chain_contract.yaml")
CONTRACT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_ci_supply_chain_contract.schema.yaml"
)
LOCK = pathlib.Path("tools/ci/llvm22-noble.lock.json")
REQUIREMENTS = pathlib.Path("tools/quality/requirements.txt")
REQUIREMENTS_LOCK = pathlib.Path("tools/quality/requirements.lock")
WORKFLOWS = (
    pathlib.Path(".github/workflows/quality.yml"),
    pathlib.Path(".github/workflows/nightly.yml"),
)
REQUIREMENT = re.compile(
    r"^([A-Za-z0-9_.-]+)==([^\s]+)\s+--hash=sha256:([0-9a-f]{64})$"
)


class CiSupplyChainError(ValueError):
    """A CI supply-chain contract violation."""


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise CiSupplyChainError(f"expected mapping: {path}")
    return value


def normalized_name(name: str) -> str:
    return re.sub(r"[-_.]+", "-", name).lower()


def parse_direct_requirements(path: pathlib.Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"([A-Za-z0-9_.-]+)==([^\s]+)", line)
        if not match:
            raise CiSupplyChainError(f"direct requirement is not exact: {line}")
        name = normalized_name(match.group(1))
        if name in result:
            raise CiSupplyChainError(f"duplicate direct requirement: {name}")
        result[name] = match.group(2)
    return result


def parse_hash_lock(path: pathlib.Path) -> dict[str, tuple[str, str]]:
    logical: list[str] = []
    pending = ""
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        pending += line.removesuffix("\\").rstrip() + " "
        if not line.endswith("\\"):
            logical.append(pending.strip())
            pending = ""
    if pending:
        raise CiSupplyChainError("requirements lock has an incomplete continuation")
    result: dict[str, tuple[str, str]] = {}
    for line in logical:
        match = REQUIREMENT.fullmatch(line)
        if not match:
            raise CiSupplyChainError(f"requirement lacks exact version/hash: {line}")
        name = normalized_name(match.group(1))
        if name in result:
            raise CiSupplyChainError(f"duplicate locked requirement: {name}")
        result[name] = (match.group(2), match.group(3))
    if not result:
        raise CiSupplyChainError("requirements lock is empty")
    return result


def validate_workflow(path: pathlib.Path, lock: dict[str, Any]) -> None:
    text = path.read_text(encoding="utf-8")
    for forbidden in (
        "llvm.sh",
        "wget ",
        "curl ",
        "sudo apt-get",
        "tools/quality/requirements.txt",
    ):
        if forbidden in text:
            raise CiSupplyChainError(f"workflow contains forbidden bootstrap: {path}: {forbidden}")
    expected_actions = lock["actions"]
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("runs-on:") and stripped != f"runs-on: {lock['runner']['label']}":
            raise CiSupplyChainError(f"workflow runner label is not locked: {path}: {stripped}")
        if stripped.startswith("- uses:") or stripped.startswith("uses:"):
            reference = stripped.removeprefix("-").strip().removeprefix("uses:")
            reference = reference.split("#", 1)[0].strip()
            name, separator, revision = reference.partition("@")
            if not separator or expected_actions.get(name) != revision:
                raise CiSupplyChainError(f"workflow action differs from lock: {path}: {reference}")
        if "python -m pip install" in stripped and not all(
            marker in stripped
            for marker in (
                "--require-hashes",
                "--only-binary=:all:",
                "tools/quality/requirements.lock",
            )
        ):
            raise CiSupplyChainError(f"workflow pip install is not hash-locked: {path}: {stripped}")
    expected_python = f'python-version: "{lock["python"]["version"]}"'
    setup_count = text.count("actions/setup-python@")
    if text.count(expected_python) != setup_count:
        raise CiSupplyChainError(f"workflow Python patch version is not exact: {path}")


def validate_repository(root: pathlib.Path) -> None:
    contract = load_yaml(root / CONTRACT)
    schema = load_yaml(root / CONTRACT_SCHEMA)
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(contract)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise CiSupplyChainError(f"CI supply-chain schema validation failed: {error.message}") from error
    lock = json.loads((root / LOCK).read_text(encoding="utf-8"))
    sys.path.insert(0, str(root / "tools/ci"))
    from bootstrap_supply_chain import load_lock  # pylint: disable=import-outside-toplevel

    validated = load_lock(root)
    if validated != lock:
        raise CiSupplyChainError("bootstrap and quality checker loaded different locks")
    direct = parse_direct_requirements(root / REQUIREMENTS)
    locked = parse_hash_lock(root / REQUIREMENTS_LOCK)
    for name, version in direct.items():
        if name not in locked or locked[name][0] != version:
            raise CiSupplyChainError(f"direct requirement differs from hash lock: {name}")
    for workflow in WORKFLOWS:
        validate_workflow(root / workflow, lock)
    workflow_text = "\n".join(
        (root / workflow).read_text(encoding="utf-8") for workflow in WORKFLOWS
    )
    expected_profiles = {
        "--profile developer": 4,
        "--profile compiler": 2,
        "--profile static-analysis": 1,
    }
    for marker, expected in expected_profiles.items():
        if workflow_text.count(marker) != expected:
            raise CiSupplyChainError(
                f"workflow bootstrap profile count differs: {marker}: expected {expected}"
            )
    if workflow_text.count("collect_toolchain_provenance.py") < 8:
        raise CiSupplyChainError("toolchain provenance is not collected by all evidence jobs")
    collector = (root / "tools/quality/collect_toolchain_provenance.py").read_text(
        encoding="utf-8"
    )
    for marker in (
        "llvm22-noble.lock.json",
        "requirements.lock",
        "ImageVersion",
        "python_distributions",
    ):
        if marker not in collector:
            raise CiSupplyChainError(f"provenance collector lacks supply-chain binding: {marker}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    args = parser.parse_args()
    try:
        validate_repository(args.root.resolve())
    except (
        CiSupplyChainError,
        OSError,
        json.JSONDecodeError,
        yaml.YAMLError,
    ) as error:
        print(f"CI supply-chain check failed: {error}", file=sys.stderr)
        return 1
    print("CI supply-chain contract check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
