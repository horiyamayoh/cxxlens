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


def hash_files_digest(*paths: pathlib.Path) -> str:
    """Reproduce hashFiles for files yielded in the runner's match order."""

    if not paths:
        return ""
    result = hashlib.sha256()
    for path in paths:
        result.update(hashlib.sha256(path.read_bytes()).digest())
    return result.hexdigest()


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


def package_cache_config(lock: dict[str, Any]) -> dict[str, Any]:
    config = lock.get("package_cache")
    if not isinstance(config, dict):
        raise ValueError("package-cache authority is missing")
    required = (
        "environment",
        "documentation_environment",
        "hit_environment",
        "key_environment",
        "key_template",
        "key_version",
        "profile_environment",
        "receipt_environment",
        "receipt_schema",
        "runner_arch_environment",
        "runner_os_environment",
    )
    if any(not isinstance(config.get(field), str) or not config[field] for field in required):
        raise ValueError("package-cache authority is incomplete")
    return config


def locked_package_profiles(lock: dict[str, Any]) -> dict[str, dict[str, dict[str, str]]]:
    llvm = lock.get("llvm")
    documentation = lock.get("documentation")
    if not isinstance(llvm, dict) or not isinstance(documentation, dict):
        raise ValueError("package-cache package authority is missing")
    profiles = llvm.get("profiles")
    packages = llvm.get("packages")
    package_digests = llvm.get("package_sha256")
    architecture = llvm.get("architecture")
    if (
        not isinstance(profiles, dict)
        or not isinstance(packages, dict)
        or not isinstance(package_digests, dict)
        or not isinstance(architecture, str)
    ):
        raise ValueError("package-cache package authority is invalid")

    result: dict[str, dict[str, dict[str, str]]] = {}
    for profile, names in profiles.items():
        if not isinstance(profile, str) or not isinstance(names, list) or not names:
            raise ValueError("package-cache profile authority is invalid")
        expected: dict[str, dict[str, str]] = {}
        for name in names:
            if not isinstance(name, str) or name in expected:
                raise ValueError("package-cache package authority is invalid")
            version = packages.get(name)
            digest = package_digests.get(name)
            if (
                not isinstance(version, str)
                or not isinstance(digest, str)
                or len(digest) != 64
                or any(character not in "0123456789abcdef" for character in digest)
            ):
                raise ValueError("package-cache package authority is invalid")
            expected[name] = {
                "package": name,
                "version": version,
                "architecture": architecture,
                "package_digest": "sha256:" + digest,
            }
        result[profile] = expected

    documentation_package = documentation.get("package")
    documentation_version = documentation.get("version")
    documentation_architecture = documentation.get("architecture")
    documentation_digest = documentation.get("sha256")
    if not all(
        isinstance(value, str)
        for value in (
            documentation_package,
            documentation_version,
            documentation_architecture,
            documentation_digest,
        )
    ) or len(documentation_digest) != 64 or any(
        character not in "0123456789abcdef" for character in documentation_digest
    ):
        raise ValueError("package-cache documentation authority is invalid")
    result["documentation"] = {
        documentation_package: {
            "package": documentation_package,
            "version": documentation_version,
            "architecture": documentation_architecture,
            "package_digest": "sha256:" + documentation_digest,
        }
    }
    return result


def validate_package_cache_profiles(
    lock: dict[str, Any],
    profiles: Any,
    cache_hit: str,
    expected_profile_names: set[str] | None = None,
) -> dict[str, list[dict[str, str]]]:
    if not isinstance(profiles, dict) or not profiles:
        raise ValueError("package-cache provenance profiles are invalid")
    if expected_profile_names is not None and set(profiles) != expected_profile_names:
        raise ValueError("package-cache provenance profile scope differs")
    expected_profiles = locked_package_profiles(lock)
    allowed_sources = {"verified-cache", "verified-download"}
    expected_source = "verified-cache" if cache_hit == "hit" else "verified-download"
    validated: dict[str, list[dict[str, str]]] = {}
    required_fields = {
        "package",
        "version",
        "architecture",
        "package_digest",
        "source",
    }
    for profile, rows in profiles.items():
        expected = expected_profiles.get(profile)
        if expected is None or not isinstance(rows, list):
            raise ValueError("package-cache provenance profile is invalid")
        if len(rows) != len(expected):
            raise ValueError(f"package-cache provenance package set differs: {profile}")
        canonical_rows: list[dict[str, str]] = []
        seen: set[str] = set()
        for row in rows:
            if not isinstance(row, dict) or set(row) != required_fields:
                raise ValueError("package-cache provenance package record is invalid")
            package = row.get("package")
            if not isinstance(package, str):
                raise ValueError(
                    f"package-cache provenance package identity differs: {profile}"
                )
            expected_record = expected.get(package)
            if expected_record is None or package in seen:
                raise ValueError(
                    f"package-cache provenance package identity differs: {profile}"
                )
            if any(row[field] != value for field, value in expected_record.items()):
                raise ValueError(
                    f"package-cache provenance package authority differs: {profile}"
                )
            source = row.get("source")
            if source not in allowed_sources:
                raise ValueError("package-cache provenance package source is invalid")
            if source != expected_source:
                if cache_hit == "hit":
                    raise ValueError("package-cache provenance cache-hit source differs")
                raise ValueError("package-cache provenance cache-miss source differs")
            seen.add(package)
            canonical_rows.append(dict(row))
        if seen != set(expected):
            raise ValueError(f"package-cache provenance package set differs: {profile}")
        validated[profile] = sorted(canonical_rows, key=lambda row: row["package"])
    return validated


def package_cache_provenance(
    lock: dict[str, Any],
    *,
    lock_path: pathlib.Path | None = None,
    lock_digest: str | None = None,
) -> dict[str, Any]:
    config = package_cache_config(lock)
    authority_digest = package_cache_authority_digest(lock)
    effective_lock_path = (lock_path or (ROOT / SUPPLY_CHAIN_LOCK)).resolve()
    effective_lock_digest = lock_digest or file_digest(effective_lock_path)
    lock_digest_hex = effective_lock_digest.removeprefix("sha256:")
    if len(lock_digest_hex) != 64 or any(
        character not in "0123456789abcdef" for character in lock_digest_hex
    ):
        raise ValueError("package-cache lock digest is invalid")
    environment_names = {
        config[field]
        for field in (
            "environment",
            "documentation_environment",
            "hit_environment",
            "key_environment",
            "profile_environment",
            "receipt_environment",
            "runner_arch_environment",
            "runner_os_environment",
        )
    }
    if not any(name in os.environ for name in environment_names):
        return {
            "status": "not-requested",
            "receipt_schema": config["receipt_schema"],
            "authority_digest": authority_digest,
            "lock_digest": effective_lock_digest,
            "key": "unavailable",
            "cache_hit": "not-requested",
            "profiles": {},
        }
    required_environment = {
        field: os.environ.get(config[field])
        for field in (
            "environment",
            "documentation_environment",
            "hit_environment",
            "key_environment",
            "profile_environment",
            "receipt_environment",
            "runner_arch_environment",
            "runner_os_environment",
        )
    }
    if any(
        not isinstance(value, str) or not value
        for value in required_environment.values()
    ):
        raise ValueError("package-cache provenance environment binding is incomplete")
    cache_directory = pathlib.Path(required_environment["environment"]).expanduser()
    if not cache_directory.is_absolute():
        raise ValueError("package-cache provenance cache directory is not absolute")
    raw_path = required_environment["receipt_environment"]
    path = pathlib.Path(raw_path)
    if not path.is_absolute() or not path.is_file():
        raise ValueError("package-cache provenance receipt is unavailable")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError("package-cache provenance receipt is invalid") from error
    requested_profile = required_environment["profile_environment"]
    documentation = required_environment["documentation_environment"]
    runner = lock.get("runner")
    if (
        not isinstance(runner, dict)
        or required_environment["runner_os_environment"] != runner.get("os")
        or required_environment["runner_arch_environment"] != runner.get("architecture")
    ):
        raise ValueError("package-cache provenance runner differs from locked authority")
    expected_profile_names = set()
    if requested_profile != "none":
        expected_profile_names.add(requested_profile)
    elif documentation != "true":
        raise ValueError("package-cache provenance profile scope is invalid")
    if requested_profile != "none" and requested_profile not in lock["llvm"]["profiles"]:
        raise ValueError("package-cache provenance profile scope is invalid")
    if documentation == "true":
        expected_profile_names.add("documentation")
    elif documentation != "false":
        raise ValueError("package-cache provenance documentation scope is invalid")
    expected_key = config["key_template"]
    replacements = {
        "${runner.os}": required_environment["runner_os_environment"],
        "${runner.arch}": required_environment["runner_arch_environment"],
        "${profile}": requested_profile,
        "${documentation}": documentation,
        "${lock_hash_files_digest}": hash_files_digest(effective_lock_path),
    }
    for token, value in replacements.items():
        expected_key = expected_key.replace(token, value)
    if "${" in expected_key or not expected_key.startswith(
        f"cxxlens-ci-packages-{config['key_version']}-"
    ):
        raise ValueError("package-cache provenance key authority is invalid")
    if required_environment["key_environment"] != expected_key:
        raise ValueError("package-cache provenance key differs from locked authority")
    raw_hit = required_environment["hit_environment"]
    if raw_hit not in {"true", "false"}:
        raise ValueError("package-cache provenance environment binding is invalid")
    expected_hit = "hit" if raw_hit == "true" else "miss"
    if (
        not isinstance(document, dict)
        or document.get("schema") != config["receipt_schema"]
        or document.get("authority_digest") != authority_digest
        or document.get("key") != expected_key
        or document.get("cache_hit") != expected_hit
        or set(document) != {
            "schema",
            "authority_digest",
            "key",
            "cache_hit",
            "profiles",
        }
    ):
        raise ValueError("package-cache provenance receipt binding differs")
    validated_profiles = validate_package_cache_profiles(
        lock, document["profiles"], expected_hit, expected_profile_names
    )
    return {
        "status": "verified",
        "receipt_schema": config["receipt_schema"],
        "authority_digest": authority_digest,
        "lock_digest": effective_lock_digest,
        "key": expected_key,
        "cache_hit": expected_hit,
        "profile": requested_profile,
        "documentation": documentation,
        "runner_os": required_environment["runner_os_environment"],
        "runner_arch": required_environment["runner_arch_environment"],
        "profiles": validated_profiles,
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
        "package_cache": package_cache_provenance(
            lock,
            lock_path=root / SUPPLY_CHAIN_LOCK,
            lock_digest=supply_chain_binding["lock_digest"],
        ),
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
