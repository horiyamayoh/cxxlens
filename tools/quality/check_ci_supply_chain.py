#!/usr/bin/env python3
"""Validate the fail-closed CI bootstrap, locks, workflows, and provenance wiring."""

from __future__ import annotations

import argparse
import hashlib
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
SETUP_ACTION = pathlib.Path(".github/actions/setup-ci/action.yml")
WORKFLOWS = (
    pathlib.Path(".github/workflows/quality.yml"),
    pathlib.Path(".github/workflows/nightly.yml"),
    pathlib.Path(".github/workflows/pr-integration.yml"),
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


def local_workflow_digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


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
            if reference.startswith("./"):
                local_reference = pathlib.PurePosixPath(reference[2:])
                if (
                    local_reference.is_absolute()
                    or ".." in local_reference.parts
                    or local_reference.as_posix() != reference[2:]
                ):
                    raise CiSupplyChainError(
                        f"local reference is not repository-scoped: {path}: {reference}"
                    )
                repository_root = path.parents[2]
                if reference.startswith("./.github/workflows/"):
                    local_path = repository_root / local_reference
                    lock_section = lock.get("local_workflows")
                    label = "workflow"
                elif reference.startswith("./.github/actions/"):
                    local_path = repository_root / local_reference / "action.yml"
                    local_reference = pathlib.PurePosixPath(
                        local_reference.as_posix() + "/action.yml"
                    )
                    lock_section = lock.get("local_actions")
                    label = "action"
                else:
                    raise CiSupplyChainError(
                        f"unsupported local reference: {path}: {reference}"
                    )
                if not local_path.is_file():
                    raise CiSupplyChainError(
                        f"local {label} reference is unavailable: {path}: {reference}"
                    )
                if not isinstance(lock_section, dict):
                    raise CiSupplyChainError(
                        f"local {label} lock is missing: {path}: {reference}"
                    )
                expected_digest = lock_section.get(local_reference.as_posix())
                if (
                    not isinstance(expected_digest, str)
                    or len(expected_digest) != 64
                    or any(character not in "0123456789abcdef" for character in expected_digest)
                ):
                    raise CiSupplyChainError(
                        f"local {label} is absent from supply-chain lock: {path}: {reference}"
                    )
                actual_digest = local_workflow_digest(local_path)
                if actual_digest != expected_digest:
                    raise CiSupplyChainError(
                        f"local {label} differs from supply-chain lock: {path}: {reference}"
                    )
                continue
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
    required_developer_packages = {
        "libclang-22-dev",
        "libclang-rt-22-dev",
        "llvm-22-dev",
    }
    developer_packages = set(lock["llvm"]["profiles"].get("developer", ()))
    missing_developer_packages = required_developer_packages - developer_packages
    if missing_developer_packages:
        raise CiSupplyChainError(
            "developer profile lacks required exact Clang development packages: "
            + ", ".join(sorted(missing_developer_packages))
        )
    direct = parse_direct_requirements(root / REQUIREMENTS)
    locked = parse_hash_lock(root / REQUIREMENTS_LOCK)
    for name, version in direct.items():
        if name not in locked or locked[name][0] != version:
            raise CiSupplyChainError(f"direct requirement differs from hash lock: {name}")
    for workflow in WORKFLOWS:
        validate_workflow(root / workflow, lock)
    validate_workflow(root / SETUP_ACTION, lock)
    workflow_text = "\n".join(
        (root / workflow).read_text(encoding="utf-8") for workflow in WORKFLOWS
    )
    for marker in (
        "bootstrap_supply_chain.py install --profile",
        "actions/setup-python@",
        "python -m pip install --require-hashes",
    ):
        if marker in workflow_text:
            raise CiSupplyChainError(
                f"workflow duplicates setup owned by the composite action: {marker}"
            )
    if workflow_text.count("./.github/actions/setup-ci") < 10:
        raise CiSupplyChainError("too few CI jobs use the common setup action")
    setup_text = (root / SETUP_ACTION).read_text(encoding="utf-8")
    expected_cache = {
        "directory": "~/.cache/cxxlens/packages",
        "environment": "CXXLENS_PACKAGE_CACHE",
        "hit_environment": "CXXLENS_PACKAGE_CACHE_HIT",
        "key_environment": "CXXLENS_PACKAGE_CACHE_KEY",
        "receipt_environment": "CXXLENS_PACKAGE_CACHE_RECEIPT",
        "key_version": "v1",
        "key_template": (
            "cxxlens-ci-packages-v1-${runner.os}-${runner.arch}-"
            "${profile}-${documentation}-${lock_digest}"
        ),
        "scope": "exact-downloaded-debs-only",
        "correctness_role": "transport-optimization-only",
        "restore_keys": False,
    }
    if lock.get("package_cache") != expected_cache:
        raise CiSupplyChainError("downloaded-package cache contract differs")
    for marker in (
        "actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97",
        "actions/cache@0057852bfaa89a56745cba8c7296529d2fc39830",
        "cache: pip",
        "CXXLENS_PACKAGE_CACHE",
        "CXXLENS_PACKAGE_CACHE_KEY",
        "CXXLENS_PACKAGE_CACHE_HIT",
        "CXXLENS_PACKAGE_CACHE_RECEIPT",
        "cxxlens-ci-packages-v1-${{ runner.os }}-${{ runner.arch }}-",
        "${{ inputs.profile }}-${{ inputs.documentation }}-",
        "hashFiles('tools/ci/llvm22-noble.lock.json')",
        "bootstrap_supply_chain.py install",
        "tools/quality/requirements.lock",
    ):
        if marker not in setup_text:
            raise CiSupplyChainError(
                f"common CI setup action lacks required binding: {marker}"
            )
    if "restore-keys:" in setup_text:
        raise CiSupplyChainError("downloaded-package cache must not use fallback restore keys")
    bootstrap_text = (root / "tools/ci/bootstrap_supply_chain.py").read_text(
        encoding="utf-8"
    )
    for marker in (
        "CXXLENS_PACKAGE_CACHE",
        "package_cache_directory",
        "resolve_cached_archive",
        "verify_deb_archive",
        "write_package_cache_receipt",
        "verified-cache",
        "verified-download",
        '["apt-get", "download"',
    ):
        if marker not in bootstrap_text:
            raise CiSupplyChainError(
                f"bootstrap lacks exact package-cache binding: {marker}"
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
        'command_identity("doxygen")',
        "package_cache_provenance",
        "package_cache_authority_digest",
        '"package_cache": package_cache_provenance(lock)',
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
