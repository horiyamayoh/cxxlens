#!/usr/bin/env python3
"""Install the digest- and version-pinned CI toolchain without llvm.sh."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[2]
LOCK = pathlib.Path("tools/ci/llvm22-noble.lock.json")
KEYRING = pathlib.Path("/etc/apt/keyrings/cxxlens-llvm.gpg")
SOURCE_LIST = pathlib.Path("/etc/apt/sources.list.d/cxxlens-llvm.list")
PACKAGE_CACHE_ENV = "CXXLENS_PACKAGE_CACHE"
PACKAGE_CACHE_HIT_ENV = "CXXLENS_PACKAGE_CACHE_HIT"
PACKAGE_CACHE_KEY_ENV = "CXXLENS_PACKAGE_CACHE_KEY"
PACKAGE_CACHE_RECEIPT_ENV = "CXXLENS_PACKAGE_CACHE_RECEIPT"
PACKAGE_CACHE_PROFILE_ENV = "CXXLENS_PACKAGE_CACHE_PROFILE"
PACKAGE_CACHE_DOCUMENTATION_ENV = "CXXLENS_PACKAGE_CACHE_DOCUMENTATION"
PACKAGE_CACHE_RUNNER_OS_ENV = "CXXLENS_PACKAGE_CACHE_RUNNER_OS"
PACKAGE_CACHE_RUNNER_ARCH_ENV = "CXXLENS_PACKAGE_CACHE_RUNNER_ARCH"
PACKAGE_CACHE_RECEIPT_SCHEMA = "cxxlens.ci-package-cache-receipt.v2"


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


def installed_package_version(package: str) -> str:
    completed = subprocess.run(
        ["dpkg-query", "--showformat=${Version}", "--show", package],
        check=False,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip() if completed.returncode == 0 else ""


def load_lock(root: pathlib.Path = ROOT) -> dict[str, Any]:
    path = root / LOCK
    try:
        lock = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SupplyChainError(f"could not read supply-chain lock: {error}") from error
    if lock.get("schema") != "cxxlens.ci-supply-chain-lock.v1":
        raise SupplyChainError("unknown supply-chain lock schema")
    llvm = lock.get("llvm")
    documentation = lock.get("documentation")
    python = lock.get("python")
    runner = lock.get("runner")
    actions = lock.get("actions")
    local_workflows = lock.get("local_workflows")
    package_cache = lock.get("package_cache")
    if not all(
        isinstance(value, dict)
        for value in (
            llvm,
            documentation,
            python,
            runner,
            actions,
            local_workflows,
            package_cache,
        )
    ) or not local_workflows:
        raise SupplyChainError("supply-chain lock sections are missing")
    expected_package_cache = {
        "directory": "~/.cache/cxxlens/packages",
        "environment": PACKAGE_CACHE_ENV,
        "documentation_environment": PACKAGE_CACHE_DOCUMENTATION_ENV,
        "hit_environment": PACKAGE_CACHE_HIT_ENV,
        "key_environment": PACKAGE_CACHE_KEY_ENV,
        "receipt_environment": PACKAGE_CACHE_RECEIPT_ENV,
        "key_version": "v1",
        "key_template": (
            "cxxlens-ci-packages-v1-${runner.os}-${runner.arch}-"
            "${profile}-${documentation}-${lock_digest}"
        ),
        "profile_environment": PACKAGE_CACHE_PROFILE_ENV,
        "receipt_schema": PACKAGE_CACHE_RECEIPT_SCHEMA,
        "scope": "exact-downloaded-debs-only",
        "correctness_role": "transport-optimization-only",
        "restore_keys": False,
        "runner_arch_environment": PACKAGE_CACHE_RUNNER_ARCH_ENV,
        "runner_os_environment": PACKAGE_CACHE_RUNNER_OS_ENV,
    }
    if package_cache != expected_package_cache:
        raise SupplyChainError("downloaded-package cache contract differs")
    if (
        runner.get("label") != "ubuntu-24.04"
        or runner.get("architecture") != "X64"
        or runner.get("os") != "Linux"
    ):
        raise SupplyChainError("runner lock is inconsistent")
    if any(
        not isinstance(documentation.get(field), str) or not documentation[field]
        for field in (
            "package",
            "version",
            "architecture",
            "expected_release",
            "url",
            "sha256",
        )
    ):
        raise SupplyChainError("documentation package authority is incomplete")
    if (
        documentation["package"] != "doxygen"
        or documentation["architecture"] != "amd64"
        or len(documentation["sha256"]) != 64
        or any(
            character not in "0123456789abcdef"
            for character in documentation["sha256"]
        )
    ):
        raise SupplyChainError("documentation package identity is invalid")
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
    for relative_name, digest in local_workflows.items():
        if not isinstance(relative_name, str):
            raise SupplyChainError(f"local workflow lock entry is invalid: {relative_name}")
        relative_path = pathlib.PurePosixPath(relative_name)
        if (
            relative_path.is_absolute()
            or relative_path.as_posix() != relative_name
            or not relative_name.startswith(".github/workflows/")
            or ".." in relative_path.parts
            or not isinstance(digest, str)
            or len(digest) != 64
            or any(character not in "0123456789abcdef" for character in digest)
        ):
            raise SupplyChainError(f"local workflow lock entry is invalid: {relative_name}")
        workflow_path = root / relative_path
        if not workflow_path.is_file():
            raise SupplyChainError(f"locked local workflow is missing: {relative_name}")
        verify_bytes(workflow_path.read_bytes(), digest, f"local workflow {relative_name}")
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
    if lock["runner"].get("architecture") != "X64":
        raise SupplyChainError("runner architecture lock is inconsistent")
    if lock["runner"].get("os") != "Linux":
        raise SupplyChainError("runner operating-system lock is inconsistent")



def configure_llvm_repository(llvm: dict[str, Any], key_content: bytes) -> None:
    """Install the verified LLVM source and refresh its exact package index."""
    signing_key = llvm["signing_key"]
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

def package_cache_authority_digest(lock: dict[str, Any]) -> str:
    return "sha256:" + hashlib.sha256(
        json.dumps(
            lock["package_cache"], sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
    ).hexdigest()


def package_cache_directory(lock: dict[str, Any]) -> pathlib.Path:
    config = lock["package_cache"]
    raw = os.environ.get(config["environment"], config["directory"])
    directory = pathlib.Path(raw).expanduser()
    if not directory.is_absolute():
        raise SupplyChainError("downloaded-package cache path must be absolute")
    directory.mkdir(parents=True, exist_ok=True)
    return directory


def package_cache_hit_claimed(lock: dict[str, Any]) -> bool:
    return os.environ.get(lock["package_cache"]["hit_environment"], "").lower() == "true"


def cached_package_path(
    directory: pathlib.Path, namespace: str, package: str, digest: str
) -> pathlib.Path:
    if (
        not namespace
        or "/" in namespace
        or not package
        or "/" in package
        or len(digest) != 64
        or any(character not in "0123456789abcdef" for character in digest)
    ):
        raise SupplyChainError("downloaded-package cache identity is invalid")
    return directory / namespace / f"{package}-{digest}.deb"


def verify_deb_archive(
    archive: pathlib.Path,
    *,
    package: str,
    version: str,
    architecture: str,
    digest: str,
) -> None:
    verify_bytes(archive.read_bytes(), digest, f"cached package {package}")
    fields = {
        field: run(["dpkg-deb", "--field", str(archive), field], capture=True)
        for field in ("Package", "Version", "Architecture")
    }
    expected = {
        "Package": package,
        "Version": version,
        "Architecture": architecture,
    }
    if fields != expected:
        raise SupplyChainError(
            f"cached package metadata mismatch: expected {expected}, received {fields}"
        )


def resolve_cached_archive(
    archive: pathlib.Path,
    *,
    package: str,
    version: str,
    architecture: str,
    digest: str,
    cache_hit: bool,
) -> pathlib.Path | None:
    if not archive.is_file():
        if cache_hit:
            raise SupplyChainError(
                f"downloaded-package cache hit omitted locked package: {package}"
            )
        return None
    verify_deb_archive(
        archive,
        package=package,
        version=version,
        architecture=architecture,
        digest=digest,
    )
    return archive


def publish_cached_archive(source: pathlib.Path, target: pathlib.Path) -> pathlib.Path:
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + f".tmp-{os.getpid()}")
    try:
        shutil.copyfile(source, temporary)
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)
    return target


def write_package_cache_receipt(
    lock: dict[str, Any], profile: str, records: list[dict[str, str]]
) -> None:
    config = lock["package_cache"]
    raw_path = os.environ.get(config["receipt_environment"])
    if not raw_path:
        return
    path = pathlib.Path(raw_path)
    if not path.is_absolute():
        raise SupplyChainError("package-cache receipt path must be absolute")
    authority_digest = package_cache_authority_digest(lock)
    key = os.environ.get(config["key_environment"], "unavailable")
    cache_hit = "hit" if package_cache_hit_claimed(lock) else "miss"
    if path.is_file():
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise SupplyChainError(f"could not read package-cache receipt: {error}") from error
        if (
            document.get("schema") != config["receipt_schema"]
            or document.get("authority_digest") != authority_digest
            or document.get("key") != key
            or document.get("cache_hit") != cache_hit
            or not isinstance(document.get("profiles"), dict)
        ):
            raise SupplyChainError("package-cache receipt binding differs")
    else:
        document = {
            "schema": config["receipt_schema"],
            "authority_digest": authority_digest,
            "key": key,
            "cache_hit": cache_hit,
            "profiles": {},
        }
    canonical_records = sorted(records, key=lambda row: row["package"])
    if profile in document["profiles"] and document["profiles"][profile] != canonical_records:
        raise SupplyChainError(f"package-cache receipt profile differs: {profile}")
    document["profiles"][profile] = canonical_records
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp-{os.getpid()}")
    try:
        temporary.write_text(
            json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)

def install_documentation(lock: dict[str, Any]) -> None:
    documentation = lock["documentation"]
    cache = package_cache_directory(lock)
    cache_hit = package_cache_hit_claimed(lock)
    archive = cached_package_path(
        cache, "documentation", documentation["package"], documentation["sha256"]
    )
    resolved = resolve_cached_archive(
        archive,
        package=documentation["package"],
        version=documentation["version"],
        architecture=documentation["architecture"],
        digest=documentation["sha256"],
        cache_hit=cache_hit,
    )
    source = "verified-cache"
    if resolved is None:
        content = download(documentation["url"])
        verify_bytes(content, documentation["sha256"], "Doxygen package")
        with tempfile.TemporaryDirectory(prefix="cxxlens-documentation-package-") as temporary:
            candidate = pathlib.Path(temporary) / "doxygen.deb"
            candidate.write_bytes(content)
            verify_deb_archive(
                candidate,
                package=documentation["package"],
                version=documentation["version"],
                architecture=documentation["architecture"],
                digest=documentation["sha256"],
            )
            resolved = publish_cached_archive(candidate, archive)
        source = "verified-download"
    verify_deb_archive(
        resolved,
        package=documentation["package"],
        version=documentation["version"],
        architecture=documentation["architecture"],
        digest=documentation["sha256"],
    )
    if installed_package_version(documentation["package"]) != documentation["version"]:
        run(
            [
                "sudo",
                "apt-get",
                "install",
                "--yes",
                "--no-install-recommends",
                "--no-upgrade",
                str(resolved),
            ]
        )
    actual = installed_package_version(documentation["package"])
    if actual != documentation["version"]:
        raise SupplyChainError(f"installed Doxygen version mismatch: {actual}")
    version = run(["doxygen", "--version"], capture=True)
    if version != documentation["expected_release"]:
        raise SupplyChainError(f"Doxygen release mismatch: {version}")
    write_package_cache_receipt(
        lock,
        "documentation",
        [
            {
                "package": documentation["package"],
                "version": documentation["version"],
                "architecture": documentation["architecture"],
                "package_digest": "sha256:" + documentation["sha256"],
                "source": source,
            }
        ],
    )

def install(root: pathlib.Path, profile_name: str) -> None:
    lock = load_lock(root)
    assert_runner(lock)
    if profile_name == "documentation":
        install_documentation(lock)
        return
    llvm = lock["llvm"]
    if profile_name not in llvm["profiles"]:
        raise SupplyChainError(f"unknown LLVM install profile: {profile_name}")
    members = list(llvm["profiles"][profile_name])
    cache = package_cache_directory(lock)
    cache_hit = package_cache_hit_claimed(lock)

    signing_key = llvm["signing_key"]
    key_content = download(signing_key["url"])
    verify_bytes(key_content, signing_key["sha256"], "LLVM signing key")
    configure_llvm_repository(llvm, key_content)

    archives: dict[str, pathlib.Path] = {}
    sources: dict[str, str] = {}
    missing: list[str] = []
    for name in members:
        target = cached_package_path(cache, "llvm", name, llvm["package_sha256"][name])
        resolved = resolve_cached_archive(
            target,
            package=name,
            version=llvm["packages"][name],
            architecture=llvm["architecture"],
            digest=llvm["package_sha256"][name],
            cache_hit=cache_hit,
        )
        if resolved is None:
            missing.append(name)
        else:
            archives[name] = resolved
            sources[name] = "verified-cache"

    if missing:
        package_requests = [f"{name}={llvm['packages'][name]}" for name in missing]
        with tempfile.TemporaryDirectory(prefix="cxxlens-llvm-packages-") as temporary:
            package_directory = pathlib.Path(temporary)
            run(["apt-get", "download", *package_requests], cwd=package_directory)
            downloaded: dict[str, pathlib.Path] = {}
            for candidate in sorted(package_directory.glob("*.deb")):
                name = run(["dpkg-deb", "--field", str(candidate), "Package"], capture=True)
                if name not in missing or name in downloaded:
                    raise SupplyChainError(
                        f"downloaded LLVM package set is duplicated or unexpected: {candidate.name}"
                    )
                verify_deb_archive(
                    candidate,
                    package=name,
                    version=llvm["packages"][name],
                    architecture=llvm["architecture"],
                    digest=llvm["package_sha256"][name],
                )
                target = cached_package_path(
                    cache, "llvm", name, llvm["package_sha256"][name]
                )
                downloaded[name] = publish_cached_archive(candidate, target)
                sources[name] = "verified-download"
            if set(downloaded) != set(missing):
                raise SupplyChainError("downloaded LLVM package set differs from cache miss set")
            archives.update(downloaded)

    if set(archives) != set(members) or set(sources) != set(members):
        raise SupplyChainError("resolved LLVM package set differs from profile")
    for name, archive in archives.items():
        verify_deb_archive(
            archive,
            package=name,
            version=llvm["packages"][name],
            architecture=llvm["architecture"],
            digest=llvm["package_sha256"][name],
        )
    run(
        [
            "sudo",
            "apt-get",
            "install",
            "--yes",
            "--no-install-recommends",
            "--no-upgrade",
            *[str(archives[name]) for name in members],
        ]
    )
    write_package_cache_receipt(
        lock,
        profile_name,
        [
            {
                "package": name,
                "version": llvm["packages"][name],
                "architecture": llvm["architecture"],
                "package_digest": "sha256:" + llvm["package_sha256"][name],
                "source": sources[name],
            }
            for name in members
        ],
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
