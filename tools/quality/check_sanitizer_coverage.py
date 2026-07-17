#!/usr/bin/env python3
"""Validate fail-closed first-party sanitizer compile coverage."""

from __future__ import annotations

import argparse
import json
import pathlib
import shlex
import sys
from typing import Any

import jsonschema
import yaml


CONTRACT = pathlib.Path("schemas/cxxlens_ng_sanitizer_coverage.yaml")
SCHEMA = pathlib.Path("schemas/cxxlens_ng_sanitizer_coverage.schema.yaml")
KNOWN_SANITIZERS = {"address", "undefined", "thread"}
INSTALL_DATABASES = {
    "consumer-build/compile_commands.json",
    "sdk_consumer-build/compile_commands.json",
    "clang22_sdk_consumer-build/compile_commands.json",
    "examples-consumer-build/compile_commands.json",
}


class SanitizerCoverageError(ValueError):
    """A stable sanitizer coverage contract violation."""


def fail(message: str) -> None:
    raise SanitizerCoverageError(message)


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    document = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        fail(f"expected mapping: {path}")
    return document


def validate_contract(root: pathlib.Path) -> dict[str, Any]:
    contract = load_yaml(root / CONTRACT)
    schema = load_yaml(root / SCHEMA)
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(contract)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"sanitizer coverage schema validation failed: {error.message}")

    expected_configurations = {
        "normal": {
            "required_sanitizers": [],
            "forbidden_sanitizers": ["address", "undefined", "thread"],
        },
        "asan-ubsan": {
            "required_sanitizers": ["address", "undefined"],
            "forbidden_sanitizers": ["thread"],
        },
        "tsan": {
            "required_sanitizers": ["thread"],
            "forbidden_sanitizers": ["address", "undefined"],
        },
    }
    if contract["configurations"] != expected_configurations:
        fail("sanitizer configuration sets differ from the accepted isolation matrix")
    if contract["scope"]["noninstrumented_external_allowlist"]:
        fail("the accepted first-party sanitizer allowlist must remain empty")
    canaries = {row["id"]: row for row in contract["canaries"]}
    if set(canaries) != {"address", "undefined", "thread"} or canaries["thread"][
        "roles"
    ] != ["test", "provider"]:
        fail("sanitizer canary roles differ from the accepted boundary coverage")

    required_markers = {
        "tests/CMakeLists.txt": [
            "cxxlens_enable_sanitizers(${target})",
            "foreach(role IN ITEMS test provider)",
            "cxxlens_configure_sanitizer_tests()",
        ],
        "cmake/CxxlensSanitizerOptions.cpp": [
            "__asan_default_options",
            "__tsan_default_options",
            "handle_segv=0",
        ],
        "tests/install/run_install_test.cmake.in": [
            "-DCXXLENS_ENABLE_ASAN=@CXXLENS_ENABLE_ASAN@",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ],
        ".github/workflows/nightly.yml": [
            "check_sanitizer_coverage.py",
            "--require-install-consumers",
        ],
    }
    for relative, markers in required_markers.items():
        text = (root / relative).read_text(encoding="utf-8")
        for marker in markers:
            if marker not in text:
                fail(f"sanitizer implementation marker is missing: {relative}: {marker}")
    return contract


def command_tokens(entry: dict[str, Any]) -> list[str]:
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(isinstance(value, str) for value in arguments):
        return arguments
    command = entry.get("command")
    if not isinstance(command, str):
        fail("compile database entry lacks command or arguments")
    return shlex.split(command)


def sanitizer_flags(entry: dict[str, Any]) -> set[str]:
    result: set[str] = set()
    for token in command_tokens(entry):
        if token.startswith("-fsanitize="):
            result.update(token.removeprefix("-fsanitize=").split(","))
        elif token == "/fsanitize=address":
            result.add("address")
    return result & KNOWN_SANITIZERS


def source_path(entry: dict[str, Any]) -> pathlib.Path:
    value = entry.get("file")
    directory = entry.get("directory")
    if not isinstance(value, str) or not isinstance(directory, str):
        fail("compile database entry lacks file/directory identity")
    path = pathlib.Path(value)
    if not path.is_absolute():
        path = pathlib.Path(directory) / path
    return path.resolve()


def validate_database(path: pathlib.Path, expected: set[str]) -> int:
    try:
        entries = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot load compile database {path}: {error}")
    if not isinstance(entries, list) or not entries:
        fail(f"compile database is empty: {path}")
    checked = 0
    for entry in entries:
        if not isinstance(entry, dict):
            fail(f"invalid compile database entry: {path}")
        source = source_path(entry)
        if source.suffix.lower() not in {".c", ".cc", ".cpp", ".cxx"}:
            continue
        actual = sanitizer_flags(entry)
        if actual != expected:
            fail(
                f"sanitizer set differs for {source}: expected {sorted(expected)}, "
                f"got {sorted(actual)}"
            )
        checked += 1
    if checked == 0:
        fail(f"compile database has no first-party C/C++ objects: {path}")
    return checked


def parse_expected(value: str) -> set[str]:
    if value in {"", "none"}:
        return set()
    result = set(value.split(","))
    if not result or not result <= KNOWN_SANITIZERS:
        fail(f"unknown expected sanitizer set: {value}")
    if "thread" in result and len(result) != 1:
        fail("ThreadSanitizer cannot be mixed with ASan/UBSan")
    return result


def check_build(
    build_dir: pathlib.Path, expected: set[str], require_install_consumers: bool
) -> tuple[int, int]:
    databases = [build_dir / "compile_commands.json"]
    install_root = build_dir / "tests/install-consumer"
    nested = {
        path.relative_to(install_root).as_posix(): path
        for path in install_root.glob("*-build/compile_commands.json")
    }
    if require_install_consumers and set(nested) != INSTALL_DATABASES:
        fail(
            "installed consumer compile database set differs: "
            f"expected {sorted(INSTALL_DATABASES)}, got {sorted(nested)}"
        )
    databases.extend(nested[name] for name in sorted(nested))
    checked = sum(validate_database(path, expected) for path in databases)
    return checked, len(databases)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("contract", "check"))
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--build-dir", type=pathlib.Path)
    parser.add_argument("--expected", default="none")
    parser.add_argument("--require-install-consumers", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    validate_contract(root)
    if args.command == "contract":
        print("sanitizer coverage contract passed")
        return 0
    if args.build_dir is None:
        parser.error("check requires --build-dir")
    checked, databases = check_build(
        args.build_dir.resolve(),
        parse_expected(args.expected),
        args.require_install_consumers,
    )
    print(f"verified sanitizer coverage for {checked} objects in {databases} databases")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SanitizerCoverageError as error:
        print(f"sanitizer coverage check failed: {error}", file=sys.stderr)
        raise SystemExit(1)
