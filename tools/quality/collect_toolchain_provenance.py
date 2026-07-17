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
    packages = sorted(lock["llvm"]["packages"])
    result = []
    for package in packages:
        version = run("dpkg-query", "--showformat=${Version}", "--show", package)
        if version:
            if version != lock["llvm"]["packages"][package]:
                raise ValueError(f"installed LLVM package differs from lock: {package}")
            result.append(
                {
                    "package": package,
                    "version": version,
                    "package_digest": "sha256:"
                    + lock["llvm"]["package_sha256"][package],
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
