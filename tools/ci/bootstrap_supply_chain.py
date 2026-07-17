#!/usr/bin/env python3
"""Install the digest- and version-pinned CI toolchain without llvm.sh."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import platform
import subprocess
import sys
import tempfile
import urllib.request
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[2]
LOCK = pathlib.Path("tools/ci/llvm22-noble.lock.json")
KEYRING = pathlib.Path("/etc/apt/keyrings/cxxlens-llvm.gpg")
SOURCE_LIST = pathlib.Path("/etc/apt/sources.list.d/cxxlens-llvm.list")


class SupplyChainError(ValueError):
    """A fail-closed supply-chain bootstrap violation."""


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def verify_bytes(content: bytes, expected: str, label: str) -> None:
    actual = sha256_bytes(content)
    if actual != expected:
        raise SupplyChainError(
            f"{label} checksum mismatch: expected {expected}, received {actual}"
        )


def run(
    command: list[str], *, capture: bool = False, cwd: pathlib.Path | None = None
) -> str:
    completed = subprocess.run(
        command,
        check=False,
        capture_output=capture,
        text=capture,
        cwd=cwd,
    )
    if completed.returncode:
        detail = completed.stderr.strip() if capture else ""
        raise SupplyChainError(
            f"command failed ({completed.returncode}): {command!r} {detail}"
        )
    return completed.stdout.strip() if capture else ""


def load_lock(root: pathlib.Path = ROOT) -> dict[str, Any]:
    path = root / LOCK
    try:
        lock = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SupplyChainError(f"could not read supply-chain lock: {error}") from error
    if lock.get("schema") != "cxxlens.ci-supply-chain-lock.v1":
        raise SupplyChainError("unknown supply-chain lock schema")
    llvm = lock.get("llvm")
    python = lock.get("python")
    runner = lock.get("runner")
    actions = lock.get("actions")
    if not all(isinstance(value, dict) for value in (llvm, python, runner, actions)):
        raise SupplyChainError("supply-chain lock sections are missing")
    packages = llvm.get("packages")
    package_sha256 = llvm.get("package_sha256")
    profiles = llvm.get("profiles")
    if not isinstance(packages, dict) or not packages:
        raise SupplyChainError("LLVM package lock is empty")
    if any(not isinstance(value, str) or value.count(":") != 1 for value in packages.values()):
        raise SupplyChainError("LLVM packages must use exact epoch-qualified versions")
    if not isinstance(package_sha256, dict) or set(package_sha256) != set(packages):
        raise SupplyChainError("LLVM package digest set differs from version lock")
    if any(
        not isinstance(value, str)
        or len(value) != 64
        or any(character not in "0123456789abcdef" for character in value)
        for value in package_sha256.values()
    ):
        raise SupplyChainError("LLVM package digest is not SHA-256")
    if not isinstance(profiles, dict) or not profiles:
        raise SupplyChainError("LLVM profiles are missing")
    for name, members in profiles.items():
        if not isinstance(members, list) or not members or len(members) != len(set(members)):
            raise SupplyChainError(f"LLVM profile is empty or duplicated: {name}")
        unknown = set(members) - set(packages)
        if unknown:
            raise SupplyChainError(f"LLVM profile contains unlocked packages: {name}: {unknown}")
    for name, revision in actions.items():
        if (
            not isinstance(name, str)
            or not isinstance(revision, str)
            or len(revision) != 40
            or any(character not in "0123456789abcdef" for character in revision)
        ):
            raise SupplyChainError(f"action is not pinned to a commit: {name}")
    requirements = root / python.get("requirements", "")
    if not requirements.is_file():
        raise SupplyChainError("locked Python requirements are missing")
    verify_bytes(
        requirements.read_bytes(),
        python.get("requirements_sha256", ""),
        "Python requirements lock",
    )
    key = llvm.get("signing_key")
    if not isinstance(key, dict) or any(
        not isinstance(key.get(field), str) or not key[field]
        for field in ("url", "sha256", "primary_fingerprint")
    ):
        raise SupplyChainError("LLVM signing-key authority is incomplete")
    return lock


def download(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "cxxlens-ci-bootstrap/1"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def verify_key(content: bytes, expected_fingerprint: str, directory: pathlib.Path) -> pathlib.Path:
    source = directory / "llvm-snapshot.gpg.key"
    source.write_bytes(content)
    details = run(
        ["gpg", "--batch", "--show-keys", "--with-colons", str(source)],
        capture=True,
    )
    fingerprints = [
        row.split(":")[9]
        for row in details.splitlines()
        if row.startswith("fpr:") and len(row.split(":")) > 9
    ]
    if not fingerprints or fingerprints[0] != expected_fingerprint:
        raise SupplyChainError("LLVM signing-key primary fingerprint mismatch")
    keyring = directory / "cxxlens-llvm.gpg"
    run(["gpg", "--batch", "--yes", "--dearmor", "--output", str(keyring), str(source)])
    return keyring


def assert_runner(lock: dict[str, Any]) -> None:
    if platform.machine() != "x86_64":
        raise SupplyChainError(f"unsupported runner architecture: {platform.machine()}")
    os_release = pathlib.Path("/etc/os-release").read_text(encoding="utf-8")
    values = dict(
        line.split("=", 1) for line in os_release.splitlines() if "=" in line
    )
    if values.get("ID", "").strip('"') != "ubuntu" or values.get(
        "VERSION_ID", ""
    ).strip('"') != "24.04":
        raise SupplyChainError("LLVM lock requires Ubuntu 24.04")
    if lock["runner"]["architecture"] != "X64":
        raise SupplyChainError("runner architecture lock is inconsistent")


def install(root: pathlib.Path, profile_name: str) -> None:
    lock = load_lock(root)
    assert_runner(lock)
    llvm = lock["llvm"]
    if profile_name not in llvm["profiles"]:
        raise SupplyChainError(f"unknown LLVM install profile: {profile_name}")
    signing_key = llvm["signing_key"]
    key_content = download(signing_key["url"])
    verify_bytes(key_content, signing_key["sha256"], "LLVM signing key")
    with tempfile.TemporaryDirectory(prefix="cxxlens-llvm-bootstrap-") as temporary:
        directory = pathlib.Path(temporary)
        keyring = verify_key(
            key_content, signing_key["primary_fingerprint"], directory
        )
        source = directory / "cxxlens-llvm.list"
        source.write_text(
            "deb [arch={architecture} signed-by={keyring}] {repository} "
            "{suite} {component}\n".format(
                architecture=llvm["architecture"],
                keyring=KEYRING,
                repository=llvm["repository"],
                suite=llvm["suite"],
                component=llvm["component"],
            ),
            encoding="utf-8",
        )
        run(["sudo", "install", "-D", "-m", "0644", str(keyring), str(KEYRING)])
        run(["sudo", "install", "-D", "-m", "0644", str(source), str(SOURCE_LIST)])
    run(
        [
            "sudo",
            "apt-get",
            "-o",
            f"Dir::Etc::sourcelist={SOURCE_LIST}",
            "-o",
            "Dir::Etc::sourceparts=-",
            "-o",
            "APT::Get::List-Cleanup=0",
            "update",
        ]
    )
    package_requests = [
        f"{name}={llvm['packages'][name]}" for name in llvm["profiles"][profile_name]
    ]
    with tempfile.TemporaryDirectory(prefix="cxxlens-llvm-packages-") as temporary:
        package_directory = pathlib.Path(temporary)
        run(["apt-get", "download", *package_requests], cwd=package_directory)
        archives: dict[str, pathlib.Path] = {}
        for archive in sorted(package_directory.glob("*.deb")):
            name = run(["dpkg-deb", "--field", str(archive), "Package"], capture=True)
            version = run(["dpkg-deb", "--field", str(archive), "Version"], capture=True)
            if name not in llvm["profiles"][profile_name] or version != llvm["packages"][name]:
                raise SupplyChainError(f"downloaded LLVM package identity mismatch: {archive.name}")
            verify_bytes(archive.read_bytes(), llvm["package_sha256"][name], archive.name)
            if name in archives:
                raise SupplyChainError(f"duplicate downloaded LLVM package: {name}")
            archives[name] = archive
        if set(archives) != set(llvm["profiles"][profile_name]):
            raise SupplyChainError("downloaded LLVM package set differs from profile")
        run(
            [
                "sudo",
                "apt-get",
                "install",
                "--yes",
                "--no-install-recommends",
                "--no-upgrade",
                *[str(archives[name]) for name in llvm["profiles"][profile_name]],
            ]
        )
    for name in llvm["profiles"][profile_name]:
        actual = run(
            ["dpkg-query", "--showformat=${Version}", "--show", name], capture=True
        )
        if actual != llvm["packages"][name]:
            raise SupplyChainError(
                f"installed package version mismatch: {name}: {actual}"
            )
    version = run(["clang++-22", "--version"], capture=True).splitlines()[0]
    if llvm["expected_release"] not in version:
        raise SupplyChainError(f"Clang release mismatch: {version}")
    if "clang-tidy-22" in llvm["profiles"][profile_name]:
        tidy = run(["clang-tidy-22", "--version"], capture=True)
        if llvm["expected_release"] not in tidy:
            raise SupplyChainError("clang-tidy release mismatch")


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "install", "verify-artifact"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--profile")
    parser.add_argument("--artifact", type=pathlib.Path)
    parser.add_argument("--sha256")
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    try:
        if args.command == "check":
            load_lock(root)
        elif args.command == "install":
            if not args.profile:
                raise SupplyChainError("install requires --profile")
            install(root, args.profile)
        else:
            if not args.artifact or not args.sha256:
                raise SupplyChainError("verify-artifact requires --artifact and --sha256")
            verify_bytes(args.artifact.read_bytes(), args.sha256, str(args.artifact))
    except (OSError, SupplyChainError, subprocess.SubprocessError) as error:
        print(f"CI supply-chain bootstrap failed: {error}", file=sys.stderr)
        return 1
    print("CI supply-chain bootstrap passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
