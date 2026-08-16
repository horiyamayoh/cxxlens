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
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[2]
SUPPLY_CHAIN_LOCK = pathlib.Path("tools/ci/llvm22-noble.lock.json")
REQUIREMENTS_LOCK = pathlib.Path("tools/quality/requirements.lock")


def run(*command: str) -> str:
    completed = subprocess.run(command, check=False, capture_output=True, text=True)
    if completed.returncode:
        return ""
    return completed.stdout.strip()


def file_digest(path: pathlib.Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def local_reference_lock(
    root: pathlib.Path, reference: pathlib.PurePosixPath, kind: str
) -> tuple[pathlib.Path, str]:
    try:
        lock = json.loads((root / SUPPLY_CHAIN_LOCK).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"could not read local reference lock: {error}") from error
    if kind == "workflow":
        section = lock.get("local_workflows")
        local_path = root / reference
    elif kind == "action":
        section = lock.get("local_actions")
        reference = pathlib.PurePosixPath(reference.as_posix() + "/action.yml")
        local_path = root / reference
    else:
        raise ValueError(f"unknown local reference kind: {kind}")
    if not isinstance(section, dict):
        raise ValueError(f"local {kind} lock is missing")
    expected = section.get(reference.as_posix())
    if not isinstance(expected, str):
        raise ValueError(f"local {kind} is absent from supply-chain lock: {reference}")
    return local_path, expected


def package_cache_authority_digest(lock: dict[str, Any]) -> str:
    return "sha256:" + hashlib.sha256(
        json.dumps(
            lock["package_cache"], sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
    ).hexdigest()


def package_cache_provenance(lock: dict[str, Any]) -> dict[str, Any]:
    config = lock.get("package_cache")
    if not isinstance(config, dict):
        raise ValueError("package-cache authority is missing")
    authority_digest = package_cache_authority_digest(lock)
    raw_path = os.environ.get(config["receipt_environment"])
    if not raw_path:
        return {
            "status": "not-requested",
            "authority_digest": authority_digest,
            "key": "unavailable",
            "cache_hit": "not-requested",
            "profiles": {},
        }
    path = pathlib.Path(raw_path)
    if not path.is_absolute() or not path.is_file():
        raise ValueError("package-cache provenance receipt is unavailable")
    document = json.loads(path.read_text(encoding="utf-8"))
    expected_key = os.environ.get(config["key_environment"], "unavailable")
    expected_hit = (
        "hit"
        if os.environ.get(config["hit_environment"], "").lower() == "true"
        else "miss"
    )
    if (
        document.get("schema") != "cxxlens.ci-package-cache-receipt.v1"
        or document.get("authority_digest") != authority_digest
        or document.get("key") != expected_key
        or document.get("cache_hit") != expected_hit
        or not isinstance(document.get("profiles"), dict)
    ):
        raise ValueError("package-cache provenance receipt binding differs")
    allowed_sources = {"verified-cache", "verified-download"}
    for profile, rows in document["profiles"].items():
        if not isinstance(profile, str) or not isinstance(rows, list) or not rows:
            raise ValueError("package-cache provenance profile is invalid")
        for row in rows:
            if (
                not isinstance(row, dict)
                or row.get("source") not in allowed_sources
                or not isinstance(row.get("package_digest"), str)
                or not row["package_digest"].startswith("sha256:")
            ):
                raise ValueError("package-cache provenance package record is invalid")
    return {
        "status": "verified",
        "authority_digest": authority_digest,
        "key": expected_key,
        "cache_hit": expected_hit,
        "profiles": document["profiles"],
        "receipt_digest": file_digest(path),
    }

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
        }
        for package, version in lock["llvm"]["packages"].items()
    }
    documentation = lock["documentation"]
    packages[documentation["package"]] = {
        "version": documentation["version"],
        "digest": documentation["sha256"],
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
                    local_reference.is_absolute()
                    or ".." in local_reference.parts
                    or local_reference.as_posix() != reference[2:]
                ):
                    raise ValueError(
                        f"local reference is not repository-scoped: {workflow}: {reference}"
                    )
                if reference.startswith("./.github/workflows/"):
                    kind = "workflow"
                elif reference.startswith("./.github/actions/"):
                    kind = "action"
                else:
                    raise ValueError(
                        f"unsupported local reference: {workflow}: {reference}"
                    )
                local_path, expected_digest = local_reference_lock(
                    root, local_reference, kind
                )
                if (
                    not local_path.is_file()
                    or len(expected_digest) != 64
                    or any(character not in "0123456789abcdef" for character in expected_digest)
                ):
                    raise ValueError(
                        f"local {kind} lock binding is invalid: {workflow}: {reference}"
                    )
                actual_digest = file_digest(local_path).removeprefix("sha256:")
                if actual_digest != expected_digest:
                    raise ValueError(
                        f"local {kind} differs from supply-chain lock: {workflow}: {reference}"
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", default="clang++-22")
    parser.add_argument("--configuration", required=True)
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
        "package_cache": package_cache_provenance(lock),
    }
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
