#!/usr/bin/env python3
"""Build and verify exact application-analysis compilers from pinned sources."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import platform
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[2]
LOCK_PATH = ROOT / "tools/ci/application-analysis-toolchains.lock.json"


class ToolchainError(ValueError):
    """A fail-closed application-analysis toolchain violation."""


def require_digest(value: Any, length: int, field: str) -> str:
    if (
        not isinstance(value, str)
        or len(value) != length
        or any(character not in "0123456789abcdef" for character in value)
    ):
        raise ToolchainError(f"invalid {field}")
    return value


def require_string(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value or "\0" in value:
        raise ToolchainError(f"invalid {field}")
    return value


def require_unique_strings(value: Any, field: str) -> list[str]:
    if (
        not isinstance(value, list)
        or not value
        or any(not isinstance(item, str) or not item for item in value)
        or len(value) != len(set(value))
    ):
        raise ToolchainError(f"invalid {field}")
    return value


def load_lock(path: pathlib.Path = LOCK_PATH) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ToolchainError(f"could not read toolchain lock: {error}") from error
    if value.get("schema") != "cxxlens.application-analysis-toolchain-lock.v1":
        raise ToolchainError("unknown toolchain lock schema")
    if value.get("document_version") != "1.0.0":
        raise ToolchainError("unknown toolchain lock document version")
    if set(value) != {"document_version", "gcc", "runner", "schema"}:
        raise ToolchainError("unknown toolchain lock field")
    runner = value.get("runner")
    gcc = value.get("gcc")
    if not isinstance(runner, dict) or not isinstance(gcc, dict):
        raise ToolchainError("toolchain lock sections are missing")
    if runner != {
        "architecture": "X64",
        "label": "ubuntu-24.04",
        "os": "Linux",
    }:
        raise ToolchainError("toolchain runner lock differs")
    expected_gcc_fields = {
        "build_targets",
        "configure_arguments",
        "exact_version",
        "install_targets",
        "prerequisite_checksums_sha256",
        "prerequisite_script_sha256",
        "source_archive_bytes",
        "source_sha512",
        "source_url",
        "target_triples",
    }
    if set(gcc) != expected_gcc_fields:
        raise ToolchainError("unknown GCC toolchain lock field")
    if gcc.get("exact_version") != "16.2.0":
        raise ToolchainError("GCC version lock differs")
    source_url = require_string(gcc.get("source_url"), "GCC source URL")
    if source_url != (
        "https://gcc.gnu.org/pub/gcc/releases/gcc-16.2.0/"
        "gcc-16.2.0.tar.xz"
    ):
        raise ToolchainError("GCC source authority differs")
    require_digest(gcc.get("source_sha512"), 128, "GCC source SHA-512")
    require_digest(
        gcc.get("prerequisite_script_sha256"),
        64,
        "GCC prerequisite script SHA-256",
    )
    require_digest(
        gcc.get("prerequisite_checksums_sha256"),
        64,
        "GCC prerequisite checksum SHA-256",
    )
    if gcc.get("source_archive_bytes") != 107200820:
        raise ToolchainError("GCC source byte count differs")
    if require_unique_strings(gcc.get("target_triples"), "GCC targets") != [
        "x86_64-linux-gnu",
        "x86_64-pc-linux-gnu",
    ]:
        raise ToolchainError("GCC target lock differs")
    expected_configure = [
        "--disable-bootstrap",
        "--disable-libatomic",
        "--disable-libcc1",
        "--disable-libgomp",
        "--disable-libitm",
        "--disable-libquadmath",
        "--disable-libsanitizer",
        "--disable-libssp",
        "--disable-libvtv",
        "--disable-multilib",
        "--disable-nls",
        "--enable-checking=release",
        "--enable-languages=c,c++",
        "--without-isl",
    ]
    if (
        require_unique_strings(gcc.get("configure_arguments"), "GCC configure")
        != expected_configure
    ):
        raise ToolchainError("GCC configure lock differs")
    if require_unique_strings(gcc.get("build_targets"), "GCC build targets") != [
        "all-gcc",
        "all-target-libstdc++-v3",
    ]:
        raise ToolchainError("GCC build target lock differs")
    if require_unique_strings(gcc.get("install_targets"), "GCC install targets") != [
        "install-gcc",
        "install-target-libstdc++-v3",
    ]:
        raise ToolchainError("GCC install target lock differs")
    return value


def verify_file(path: pathlib.Path, algorithm: str, expected: str, field: str) -> None:
    digest = hashlib.new(algorithm)
    try:
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise ToolchainError(f"could not read {field}: {error}") from error
    if digest.hexdigest() != expected:
        raise ToolchainError(f"{field} checksum mismatch")


def run(command: list[str], *, cwd: pathlib.Path, capture: bool = False) -> str:
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=environment,
        check=False,
        capture_output=capture,
        text=capture,
    )
    if completed.returncode:
        detail = completed.stderr.strip() if capture else ""
        raise ToolchainError(
            f"command failed ({completed.returncode}): {command!r}: {detail}"
        )
    return completed.stdout.strip() if capture else ""


def assert_runner() -> None:
    if platform.machine() != "x86_64":
        raise ToolchainError(f"unsupported runner architecture: {platform.machine()}")
    try:
        release = pathlib.Path("/etc/os-release").read_text(encoding="utf-8")
    except OSError as error:
        raise ToolchainError(f"could not read runner release: {error}") from error
    values = dict(
        line.split("=", 1) for line in release.splitlines() if "=" in line
    )
    if values.get("ID", "").strip('"') != "ubuntu" or values.get(
        "VERSION_ID", ""
    ).strip('"') != "24.04":
        raise ToolchainError("GCC lock requires Ubuntu 24.04")


def verify_gcc(prefix: pathlib.Path, lock: dict[str, Any]) -> None:
    compiler = prefix / "bin/g++"
    if not compiler.is_file():
        raise ToolchainError("installed GCC compiler is missing")
    gcc = lock["gcc"]
    version = run(
        [str(compiler), "-dumpfullversion", "-dumpversion"],
        cwd=prefix,
        capture=True,
    )
    target = run([str(compiler), "-dumpmachine"], cwd=prefix, capture=True)
    if version != gcc["exact_version"]:
        raise ToolchainError(f"installed GCC version differs: {version}")
    if target not in gcc["target_triples"]:
        raise ToolchainError(f"installed GCC target differs: {target}")
    with tempfile.TemporaryDirectory(prefix="cxxlens-gcc16-canary-") as temporary:
        source = pathlib.Path(temporary) / "canary.cpp"
        executable = pathlib.Path(temporary) / "canary"
        source.write_text(
            "#include <version>\n"
            "static_assert(__cplusplus > 202002L);\n"
            "int main() { return 0; }\n",
            encoding="utf-8",
        )
        run(
            [str(compiler), "-std=c++23", str(source), "-o", str(executable)],
            cwd=pathlib.Path(temporary),
        )
        run([str(executable)], cwd=pathlib.Path(temporary))


def download_source(destination: pathlib.Path, lock: dict[str, Any]) -> None:
    gcc = lock["gcc"]
    request = urllib.request.Request(
        gcc["source_url"], headers={"User-Agent": "cxxlens-toolchain-bootstrap/1"}
    )
    try:
        with urllib.request.urlopen(request, timeout=120) as response, destination.open(
            "wb"
        ) as output:
            declared_length = response.headers.get("Content-Length")
            if declared_length is not None:
                try:
                    declared_bytes = int(declared_length)
                except ValueError as error:
                    raise ToolchainError(
                        "GCC source declared byte count is invalid"
                    ) from error
                if declared_bytes != gcc["source_archive_bytes"]:
                    raise ToolchainError("GCC source declared byte count mismatch")
            received = 0
            while chunk := response.read(1024 * 1024):
                received += len(chunk)
                if received > gcc["source_archive_bytes"]:
                    raise ToolchainError("GCC source exceeds the byte limit")
                output.write(chunk)
    except (OSError, urllib.error.URLError) as error:
        raise ToolchainError(f"could not download GCC source: {error}") from error
    if destination.stat().st_size != gcc["source_archive_bytes"]:
        raise ToolchainError("GCC source byte count mismatch")
    verify_file(destination, "sha512", gcc["source_sha512"], "GCC source")


def install_gcc(prefix: pathlib.Path, work_directory: pathlib.Path, jobs: int) -> None:
    lock = load_lock()
    assert_runner()
    if jobs <= 0 or jobs > 64:
        raise ToolchainError("parallel job count is outside the bounded range")
    if not prefix.is_absolute() or not work_directory.is_absolute():
        raise ToolchainError("toolchain paths must be absolute")
    if prefix.exists():
        verify_gcc(prefix, lock)
        return
    if work_directory.exists() and any(work_directory.iterdir()):
        raise ToolchainError("GCC work directory is not empty")
    work_directory.mkdir(parents=True, exist_ok=True)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    archive = work_directory / "gcc-16.2.0.tar.xz"
    download_source(archive, lock)
    with tarfile.open(archive, mode="r:xz") as source_archive:
        source_archive.extractall(work_directory, filter="data")
    source = work_directory / "gcc-16.2.0"
    prerequisite_script = source / "contrib/download_prerequisites"
    prerequisite_checksums = source / "contrib/prerequisites.sha512"
    verify_file(
        prerequisite_script,
        "sha256",
        lock["gcc"]["prerequisite_script_sha256"],
        "GCC prerequisite script",
    )
    verify_file(
        prerequisite_checksums,
        "sha256",
        lock["gcc"]["prerequisite_checksums_sha256"],
        "GCC prerequisite checksums",
    )
    run([str(prerequisite_script), "--no-isl"], cwd=source)
    build = work_directory / "build"
    build.mkdir()
    configure = [
        str(source / "configure"),
        f"--prefix={prefix}",
        *lock["gcc"]["configure_arguments"],
    ]
    run(configure, cwd=build)
    run(
        ["make", "--silent", f"-j{jobs}", *lock["gcc"]["build_targets"]],
        cwd=build,
    )
    run(
        ["make", "--silent", *lock["gcc"]["install_targets"]],
        cwd=build,
    )
    verify_gcc(prefix, lock)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    subcommands = parser.add_subparsers(dest="command", required=True)
    subcommands.add_parser("validate-lock")
    verify = subcommands.add_parser("verify-gcc")
    verify.add_argument("--prefix", type=pathlib.Path, required=True)
    install = subcommands.add_parser("install-gcc")
    install.add_argument("--prefix", type=pathlib.Path, required=True)
    install.add_argument("--work-directory", type=pathlib.Path, required=True)
    install.add_argument("--jobs", type=int, default=4)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if arguments.command == "validate-lock":
        load_lock()
    elif arguments.command == "verify-gcc":
        lock = load_lock()
        assert_runner()
        verify_gcc(arguments.prefix, lock)
    elif arguments.command == "install-gcc":
        install_gcc(arguments.prefix, arguments.work_directory, arguments.jobs)
    else:
        raise ToolchainError("unknown command")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ToolchainError as error:
        print(f"application-analysis toolchain bootstrap failed: {error}", file=sys.stderr)
        raise SystemExit(2)
