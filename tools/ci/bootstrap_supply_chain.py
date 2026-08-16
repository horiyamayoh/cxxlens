#!/usr/bin/env python3
"""Install the digest- and version-pinned CI toolchain without llvm.sh."""

from __future__ import annotations

import argparse
import hashlib
import json
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
PACKAGE_CACHE_SCHEMA = "cxxlens.ci-package-cache-provenance.v2"


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


def package_authority(
    lock: dict[str, Any], profile_name: str
) -> dict[str, dict[str, str]]:
    if profile_name == "documentation":
        documentation = lock["documentation"]
        return {
            documentation["package"]: {
                "package": documentation["package"],
                "version": documentation["version"],
                "architecture": documentation["architecture"],
                "sha256": documentation["sha256"],
            }
        }
    llvm = lock["llvm"]
    profiles = llvm["profiles"]
    if profile_name not in profiles:
        raise SupplyChainError(f"unknown LLVM install profile: {profile_name}")
    return {
        name: {
            "package": name,
            "version": llvm["packages"][name],
            "architecture": llvm["architecture"],
            "sha256": llvm["package_sha256"][name],
        }
        for name in profiles[profile_name]
    }


def package_cache_key(
    lock: dict[str, Any], profile_name: str, lock_digest: str
) -> str:
    expected = package_authority(lock, profile_name)
    architecture = next(iter(expected.values()))["architecture"]
    return (
        f"cxxlens-package-cache-v1-{lock['runner']['label']}-"
        f"{architecture}-{profile_name}-{lock_digest.removeprefix('sha256:')}"
    )


def package_fields(archive: pathlib.Path) -> dict[str, str]:
    return {
        field: run(["dpkg-deb", "--field", str(archive), field], capture=True)
        for field in ("Package", "Version", "Architecture")
    }


def validate_package_archive(
    archive: pathlib.Path, expected: dict[str, str], label: str
) -> None:
    fields = package_fields(archive)
    expected_fields = {
        "Package": expected["package"],
        "Version": expected["version"],
        "Architecture": expected["architecture"],
    }
    if fields != expected_fields:
        raise SupplyChainError(
            f"{label} metadata mismatch: expected {expected_fields}, received {fields}"
        )
    verify_bytes(archive.read_bytes(), expected["sha256"], label)


def package_cache_filename(expected: dict[str, str]) -> str:
    return f"{expected['package']}-{expected['sha256']}.deb"


def resolve_cached_archives(
    cache_directory: pathlib.Path | None,
    expected: dict[str, dict[str, str]],
) -> tuple[dict[str, pathlib.Path] | None, str, str | None]:
    if cache_directory is None:
        return None, "disabled", None
    cache_directory.mkdir(parents=True, exist_ok=True)
    candidates = sorted(cache_directory.glob("*.deb"))
    if not candidates:
        return None, "miss", None
    if len(candidates) != len(expected):
        return (
            None,
            "invalid",
            f"cached package count mismatch: expected {len(expected)}, received {len(candidates)}",
        )
    resolved: dict[str, pathlib.Path] = {}
    try:
        for archive in candidates:
            fields = package_fields(archive)
            name = fields["Package"]
            if name not in expected or name in resolved:
                raise SupplyChainError(f"unexpected or duplicate cached package: {archive.name}")
            validate_package_archive(archive, expected[name], archive.name)
            resolved[name] = archive
    except (OSError, SupplyChainError, KeyError) as error:
        return None, "invalid", str(error)
    if set(resolved) != set(expected):
        return None, "invalid", "cached package set differs from profile"
    return resolved, "hit", None


def replace_package_cache(
    cache_directory: pathlib.Path,
    archives: dict[str, pathlib.Path],
    expected: dict[str, dict[str, str]],
) -> None:
    cache_directory.mkdir(parents=True, exist_ok=True)
    for archive in cache_directory.glob("*.deb"):
        archive.unlink()
    for name in sorted(archives):
        destination = cache_directory / package_cache_filename(expected[name])
        temporary = destination.with_name(destination.name + ".tmp")
        shutil.copyfile(archives[name], temporary)
        temporary.replace(destination)


def copy_cached_archives(
    archives: dict[str, pathlib.Path],
    expected: dict[str, dict[str, str]],
    directory: pathlib.Path,
) -> dict[str, pathlib.Path]:
    private: dict[str, pathlib.Path] = {}
    for name in sorted(archives):
        archive = directory / f"{name}.deb"
        shutil.copyfile(archives[name], archive)
        validate_package_archive(archive, expected[name], f"cached package {name}")
        private[name] = archive
    return private


def cache_provenance_digest(document: dict[str, Any]) -> str:
    projection = {key: value for key, value in document.items() if key != "digest"}
    return "sha256:" + hashlib.sha256(
        json.dumps(projection, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()


def build_package_cache_provenance(
    lock: dict[str, Any],
    profile_name: str,
    lock_digest: str,
    cache_status: str,
    cache_source: str,
) -> dict[str, Any]:
    expected = package_authority(lock, profile_name)
    if cache_status not in {"disabled", "hit", "miss", "invalid"}:
        raise SupplyChainError(f"unknown package cache status: {cache_status}")
    if cache_source not in {"verified-cache", "verified-download"}:
        raise SupplyChainError(f"unknown package cache source: {cache_source}")
    if (cache_status == "hit") != (cache_source == "verified-cache"):
        raise SupplyChainError("package cache status/source mismatch")
    document: dict[str, Any] = {
        "schema": PACKAGE_CACHE_SCHEMA,
        "profile": profile_name,
        "transport_only": True,
        "cache_status": cache_status,
        "cache_source": cache_source,
        "dependency_resolution": (
            "locked-apt-repository"
            if profile_name != "documentation"
            else "locked-package-archive"
        ),
        "repository_refresh": (
            "verified-before-install"
            if profile_name != "documentation"
            else "not-required"
        ),
        "cache_key": package_cache_key(lock, profile_name, lock_digest),
        "cache_key_authority_digest": lock_digest,
        "packages": [
            {
                "package": name,
                "version": expected[name]["version"],
                "architecture": expected[name]["architecture"],
                "sha256": "sha256:" + expected[name]["sha256"],
                "source": cache_source,
            }
            for name in sorted(expected)
        ],
    }
    document["digest"] = cache_provenance_digest(document)
    return document


def write_package_cache_provenance(
    output: pathlib.Path, document: dict[str, Any]
) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


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
    if not all(
        isinstance(value, dict)
        for value in (llvm, documentation, python, runner, actions, local_workflows)
    ) or not local_workflows:
        raise SupplyChainError("supply-chain lock sections are missing")
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
    if llvm.get("architecture") != "amd64":
        raise SupplyChainError("LLVM package architecture is not amd64")
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


def configure_llvm_repository(llvm: dict[str, Any]) -> None:
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


def install_package_archives(archives: dict[str, pathlib.Path]) -> None:
    run(
        [
            "sudo",
            "apt-get",
            "install",
            "--yes",
            "--no-install-recommends",
            "--no-upgrade",
            *[str(archives[name]) for name in sorted(archives)],
        ]
    )


def install_documentation(
    lock: dict[str, Any],
    package_cache_directory: pathlib.Path | None = None,
) -> tuple[str, str]:
    expected = package_authority(lock, "documentation")
    archives, cache_status, _ = resolve_cached_archives(
        package_cache_directory, expected
    )
    cache_source = "verified-cache" if archives is not None else "verified-download"
    if archives is None:
        documentation = lock["documentation"]
        content = download(documentation["url"])
        verify_bytes(content, documentation["sha256"], "Doxygen package")
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-documentation-package-"
        ) as temporary:
            archive = pathlib.Path(temporary) / "doxygen.deb"
            archive.write_bytes(content)
            validate_package_archive(archive, expected["doxygen"], "Doxygen package")
            archives = {"doxygen": archive}
            if package_cache_directory is not None:
                replace_package_cache(package_cache_directory, archives, expected)
            if installed_package_version("doxygen") != expected["doxygen"]["version"]:
                install_package_archives(archives)
    else:
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-cached-documentation-package-"
        ) as temporary:
            private_archives = copy_cached_archives(
                archives, expected, pathlib.Path(temporary)
            )
            if installed_package_version("doxygen") != expected["doxygen"]["version"]:
                install_package_archives(private_archives)
    actual = installed_package_version("doxygen")
    if actual != expected["doxygen"]["version"]:
        raise SupplyChainError(f"installed Doxygen version mismatch: {actual}")
    version = run(["doxygen", "--version"], capture=True)
    if version != lock["documentation"]["expected_release"]:
        raise SupplyChainError(f"Doxygen release mismatch: {version}")
    return cache_status, cache_source


def install_llvm(
    lock: dict[str, Any],
    profile_name: str,
    package_cache_directory: pathlib.Path | None = None,
) -> tuple[str, str]:
    expected = package_authority(lock, profile_name)
    llvm = lock["llvm"]
    configure_llvm_repository(llvm)
    archives, cache_status, _ = resolve_cached_archives(
        package_cache_directory, expected
    )
    cache_source = "verified-cache" if archives is not None else "verified-download"
    if archives is None:
        package_requests = [
            f"{name}={expected[name]['version']}" for name in sorted(expected)
        ]
        with tempfile.TemporaryDirectory(prefix="cxxlens-llvm-packages-") as temporary:
            package_directory = pathlib.Path(temporary)
            run(["apt-get", "download", *package_requests], cwd=package_directory)
            archives = {}
            for archive in sorted(package_directory.glob("*.deb")):
                fields = package_fields(archive)
                name = fields["Package"]
                if name not in expected or name in archives:
                    raise SupplyChainError(
                        f"downloaded LLVM package identity mismatch: {archive.name}"
                    )
                validate_package_archive(archive, expected[name], archive.name)
                archives[name] = archive
            if set(archives) != set(expected):
                raise SupplyChainError("downloaded LLVM package set differs from profile")
            if package_cache_directory is not None:
                replace_package_cache(package_cache_directory, archives, expected)
            install_package_archives(archives)
    else:
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-cached-llvm-packages-"
        ) as temporary:
            private_archives = copy_cached_archives(
                archives, expected, pathlib.Path(temporary)
            )
            install_package_archives(private_archives)
    for name in sorted(expected):
        actual = run(
            ["dpkg-query", "--showformat=${Version}", "--show", name], capture=True
        )
        if actual != expected[name]["version"]:
            raise SupplyChainError(
                f"installed package version mismatch: {name}: {actual}"
            )
    version = run(["clang++-22", "--version"], capture=True).splitlines()[0]
    if llvm["expected_release"] not in version:
        raise SupplyChainError(f"Clang release mismatch: {version}")
    if "clang-tidy-22" in expected:
        tidy = run(["clang-tidy-22", "--version"], capture=True)
        if llvm["expected_release"] not in tidy:
            raise SupplyChainError("clang-tidy release mismatch")
    return cache_status, cache_source


def install(
    root: pathlib.Path,
    profile_name: str,
    package_cache_directory: pathlib.Path | None = None,
    provenance_output: pathlib.Path | None = None,
) -> dict[str, Any]:
    lock = load_lock(root)
    assert_runner(lock)
    lock_digest = "sha256:" + sha256_bytes((root / LOCK).read_bytes())
    if profile_name == "documentation":
        cache_status, cache_source = install_documentation(
            lock, package_cache_directory
        )
    else:
        cache_status, cache_source = install_llvm(
            lock, profile_name, package_cache_directory
        )
    provenance = build_package_cache_provenance(
        lock, profile_name, lock_digest, cache_status, cache_source
    )
    if provenance_output is not None:
        write_package_cache_provenance(provenance_output, provenance)
    return provenance


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "install", "verify-artifact"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--profile")
    parser.add_argument("--artifact", type=pathlib.Path)
    parser.add_argument("--sha256")
    parser.add_argument("--package-cache-dir", type=pathlib.Path)
    parser.add_argument("--provenance-output", type=pathlib.Path)
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
            install(
                root,
                args.profile,
                args.package_cache_dir.resolve()
                if args.package_cache_dir is not None
                else None,
                args.provenance_output.resolve()
                if args.provenance_output is not None
                else None,
            )
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
