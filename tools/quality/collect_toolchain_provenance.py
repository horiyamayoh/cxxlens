#!/usr/bin/env python3
"""Emit a digest-bound CI toolchain/SBOM provenance record."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[2]


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


def package_versions() -> list[dict[str, str]]:
    packages = [
        "clang-22",
        "clang-tidy-22",
        "libclang-22-dev",
        "llvm-22",
        "llvm-22-dev",
        "ninja-build",
    ]
    result = []
    for package in packages:
        version = run("dpkg-query", "--showformat=${Version}", "--show", package)
        if version:
            result.append({"package": package, "version": version})
    return result


def pinned_actions(root: pathlib.Path) -> list[dict[str, str]]:
    actions: list[dict[str, str]] = []
    for workflow in sorted((root / ".github/workflows").glob("*.yml")):
        for line in workflow.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if not stripped.startswith("- uses: "):
                continue
            reference = stripped.removeprefix("- uses: ").split("#", 1)[0].rstrip()
            name, separator, revision = reference.partition("@")
            if not separator or len(revision) != 40 or any(
                character not in "0123456789abcdef" for character in revision
            ):
                raise ValueError(f"workflow action is not pinned: {workflow}: {reference}")
            actions.append({"workflow": str(workflow.relative_to(root)), "name": name, "revision": revision})
    return actions


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", default="clang++-22")
    parser.add_argument("--configuration", required=True)
    args = parser.parse_args()
    root = args.root.resolve()
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
        "packages": package_versions(),
        "actions": pinned_actions(root),
        "runner": {
            "os_release_digest": file_digest(pathlib.Path("/etc/os-release")),
            "kernel": run("uname", "-srmo"),
        },
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
