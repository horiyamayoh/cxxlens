#!/usr/bin/env python3
"""Emit a digest-bound CI toolchain/SBOM provenance record."""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[2]
SUPPLY_CHAIN_LOCK = pathlib.Path("tools/ci/llvm22-noble.lock.json")
REQUIREMENTS_LOCK = pathlib.Path("tools/quality/requirements.lock")
sys.path.insert(0, str(ROOT / "tools" / "ci"))

from bootstrap_supply_chain import (  # noqa: E402
    PACKAGE_CACHE_SCHEMA,
    cache_provenance_digest,
    package_authority,
    package_cache_key,
)


def run(*command: str) -> str:
    completed = subprocess.run(command, check=False, capture_output=True, text=True)
    if completed.returncode:
        return ""
    return completed.stdout.strip()


def file_digest(path: pathlib.Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def local_workflow_lock(root: pathlib.Path) -> dict[str, str]:
    try:
        lock = json.loads((root / SUPPLY_CHAIN_LOCK).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"could not read local workflow lock: {error}") from error
    workflows = lock.get("local_workflows")
    if not isinstance(workflows, dict):
        raise ValueError("local workflow lock is missing")
    return workflows


def provenance_digest(document: dict[str, Any]) -> str:
    projection = {key: value for key, value in document.items() if key != "digest"}
    return "sha256:" + hashlib.sha256(
        json.dumps(projection, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()


def command_identity(command: str) -> dict[str, str]:
    resolved = shutil.which(command)
    if not resolved:
        return {"command": command, "status": "unavailable"}
    path = pathlib.Path(resolved).resolve()
    version = run(str(path), "--version").splitlines()
    return {
        "command": command,
        "path": str(path),
        "binary_digest": file_digest(path),
        "version": version[0] if version else "unknown",
    }


def package_versions(lock: dict[str, Any]) -> list[dict[str, str]]:
    packages = {
        package: {
            "version": version,
            "digest": lock["llvm"]["package_sha256"][package],
            "architecture": lock["llvm"]["architecture"],
        }
        for package, version in lock["llvm"]["packages"].items()
    }
    documentation = lock["documentation"]
    packages[documentation["package"]] = {
        "version": documentation["version"],
        "digest": documentation["sha256"],
        "architecture": documentation["architecture"],
    }
    result = []
    for package, authority in sorted(packages.items()):
        version = run("dpkg-query", "--showformat=${Version}", "--show", package)
        if version:
            if version != authority["version"]:
                raise ValueError(f"installed package differs from lock: {package}")
            result.append(
                {
                    "package": package,
                    "version": version,
                    "architecture": authority["architecture"],
                    "package_digest": "sha256:" + authority["digest"],
                }
            )
    return result


def locked_python_versions(path: pathlib.Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for match in re.finditer(r"(?m)^([A-Za-z0-9_.-]+)==([^\s]+) \\\n", path.read_text(encoding="utf-8")):
        name = re.sub(r"[-_.]+", "-", match.group(1)).lower()
        result[name] = match.group(2)
    if not result:
        raise ValueError("Python requirements lock is empty")
    return result


def python_distributions(path: pathlib.Path) -> list[dict[str, str]]:
    result = []
    for name, expected in sorted(locked_python_versions(path).items()):
        distribution = importlib.metadata.distribution(name)
        if distribution.version != expected:
            raise ValueError(f"installed Python distribution differs from lock: {name}")
        record = next(
            (
                candidate
                for candidate in (distribution.files or [])
                if pathlib.PurePosixPath(str(candidate)).name == "RECORD"
            ),
            None,
        )
        if record is None:
            raise ValueError(f"Python distribution has no RECORD: {name}")
        record_path = pathlib.Path(distribution.locate_file(record))
        result.append(
            {
                "name": name,
                "version": distribution.version,
                "record_digest": file_digest(record_path),
            }
        )
    return result


def pinned_actions(root: pathlib.Path) -> list[dict[str, str]]:
    actions: list[dict[str, str]] = []
    for workflow in sorted((root / ".github/workflows").glob("*.yml")):
        for line in workflow.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if stripped.startswith("- uses: "):
                reference = stripped.removeprefix("- uses: ")
            elif stripped.startswith("uses: "):
                reference = stripped.removeprefix("uses: ")
            else:
                continue
            reference = reference.split("#", 1)[0].rstrip()
            if reference.startswith("./"):
                local_reference = pathlib.PurePosixPath(reference[2:])
                if (
                    not reference.startswith("./.github/workflows/")
                    or local_reference.is_absolute()
                    or ".." in local_reference.parts
                    or local_reference.as_posix() != reference[2:]
                ):
                    raise ValueError(
                        f"local workflow reference is not a tracked workflow: {workflow}: {reference}"
                    )
                local_path = root / local_reference
                if not local_path.is_file():
                    raise ValueError(
                        f"local workflow reference is unavailable: {workflow}: {reference}"
                    )
                expected_digest = local_workflow_lock(root).get(local_reference.as_posix())
                if (
                    not isinstance(expected_digest, str)
                    or len(expected_digest) != 64
                    or any(character not in "0123456789abcdef" for character in expected_digest)
                ):
                    raise ValueError(
                        f"local workflow is absent from supply-chain lock: {workflow}: {reference}"
                    )
                actual_digest = file_digest(local_path).removeprefix("sha256:")
                if actual_digest != expected_digest:
                    raise ValueError(
                        f"local workflow differs from supply-chain lock: {workflow}: {reference}"
                    )
                continue
            name, separator, revision = reference.partition("@")
            if not separator or len(revision) != 40 or any(
                character not in "0123456789abcdef" for character in revision
            ):
                raise ValueError(f"workflow action is not pinned: {workflow}: {reference}")
            actions.append({"workflow": str(workflow.relative_to(root)), "name": name, "revision": revision})
    return actions


def supply_chain(root: pathlib.Path) -> tuple[dict[str, Any], dict[str, str]]:
    lock_path = root / SUPPLY_CHAIN_LOCK
    requirements_path = root / REQUIREMENTS_LOCK
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    requirements_digest = file_digest(requirements_path)
    expected = "sha256:" + lock["python"]["requirements_sha256"]
    if requirements_digest != expected:
        raise ValueError("Python requirements digest differs from supply-chain lock")
    actions = pinned_actions(root)
    for action in actions:
        if lock["actions"].get(action["name"]) != action["revision"]:
            raise ValueError(f"workflow action differs from supply-chain lock: {action['name']}")
    binding = {
        "schema": lock["schema"],
        "lock_path": str(SUPPLY_CHAIN_LOCK),
        "lock_digest": file_digest(lock_path),
        "requirements_path": str(REQUIREMENTS_LOCK),
        "requirements_digest": requirements_digest,
    }
    return lock, binding


def load_package_cache_provenance(
    path: pathlib.Path, root: pathlib.Path
) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"could not read package-cache provenance: {error}") from error
    if not isinstance(document, dict) or document.get("schema") != PACKAGE_CACHE_SCHEMA:
        raise ValueError("unknown package-cache provenance schema")
    if document.get("digest") != cache_provenance_digest(document):
        raise ValueError("package-cache provenance digest mismatch")
    lock = json.loads((root / SUPPLY_CHAIN_LOCK).read_text(encoding="utf-8"))
    lock_digest = file_digest(root / SUPPLY_CHAIN_LOCK)
    if document.get("cache_key_authority_digest") != lock_digest:
        raise ValueError("package-cache provenance authority digest mismatch")
    profile = document.get("profile")
    if not isinstance(profile, str):
        raise ValueError("package-cache provenance profile is missing")
    expected = package_authority(lock, profile)
    status = document.get("cache_status")
    source = document.get("cache_source")
    if status not in {"disabled", "hit", "miss", "invalid"}:
        raise ValueError("package-cache provenance status is invalid")
    if source not in {"verified-cache", "verified-download"}:
        raise ValueError("package-cache provenance source is invalid")
    if (status == "hit") != (source == "verified-cache"):
        raise ValueError("package-cache provenance status/source mismatch")
    if document.get("transport_only") is not True:
        raise ValueError("package-cache provenance must be transport-only")
    if document.get("cache_key") != package_cache_key(lock, profile, lock_digest):
        raise ValueError("package-cache provenance key mismatch")
    rows = document.get("packages")
    if not isinstance(rows, list) or len(rows) != len(expected):
        raise ValueError("package-cache provenance package set is incomplete")
    by_name: dict[str, dict[str, Any]] = {}
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError("package-cache provenance package row is invalid")
        name = row.get("package")
        if not isinstance(name, str) or name in by_name or name not in expected:
            raise ValueError("package-cache provenance package identity is invalid")
        expected_row = {
            "package": name,
            "version": expected[name]["version"],
            "architecture": expected[name]["architecture"],
            "sha256": "sha256:" + expected[name]["sha256"],
            "source": source,
        }
        if row != expected_row:
            raise ValueError(f"package-cache provenance differs from lock: {name}")
        by_name[name] = row
    if set(by_name) != set(expected):
        raise ValueError("package-cache provenance package set differs from profile")
    return document


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", default="clang++-22")
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--package-cache-provenance", type=pathlib.Path)
    args = parser.parse_args()
    root = args.root.resolve()
    lock, supply_chain_binding = supply_chain(root)
    requirements_path = root / REQUIREMENTS_LOCK
    source = {
        "revision": run("git", "-C", str(root), "rev-parse", "HEAD"),
        "tree": run("git", "-C", str(root), "rev-parse", "HEAD^{tree}"),
    }
    if not all(source.values()):
        raise SystemExit("could not determine source revision/tree")
    document: dict[str, Any] = {
        "schema": "cxxlens.toolchain-provenance.v1",
        "source": source,
        "configuration": args.configuration,
        "tools": [
            command_identity(args.compiler),
            command_identity("clang-tidy-22"),
            command_identity("doxygen"),
            command_identity("ninja"),
            command_identity("python3"),
        ],
        "packages": package_versions(lock),
        "python_distributions": python_distributions(requirements_path),
        "actions": pinned_actions(root),
        "runner": {
            "label": lock["runner"]["label"],
            "image_os": os.environ.get("ImageOS", "unavailable"),
            "image_version": os.environ.get("ImageVersion", "unavailable"),
            "architecture": os.environ.get("RUNNER_ARCH", os.uname().machine),
            "os_release_digest": file_digest(pathlib.Path("/etc/os-release")),
            "kernel": run("uname", "-srmo"),
        },
        "supply_chain": supply_chain_binding,
    }
    package_cache_path = args.package_cache_provenance
    if package_cache_path is None:
        configured_path = os.environ.get("CXXLENS_PACKAGE_CACHE_PROVENANCE")
        package_cache_path = pathlib.Path(configured_path) if configured_path else None
    if package_cache_path is not None:
        if not package_cache_path.is_absolute():
            package_cache_path = root / package_cache_path
        document["package_cache"] = load_package_cache_provenance(
            package_cache_path, root
        )
    document["digest"] = provenance_digest(document)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(document["digest"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
