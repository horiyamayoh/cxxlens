#!/usr/bin/env python3
"""Reject ambient runtime services outside the current runtime adapters."""

from __future__ import annotations

import pathlib
import re
import sys


FORBIDDEN = {
    "direct filesystem": re.compile(r"std::filesystem::"),
    "command shell": re.compile(r"(?<![:.\w])(?:system|popen)\s*\("),
    "ambient wall clock": re.compile(r"std::chrono::system_clock::now\s*\("),
    "ambient steady clock": re.compile(r"std::chrono::steady_clock::now\s*\("),
    "ambient standard hash": re.compile(r"std::hash\s*<"),
}


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} SOURCE_ROOT", file=sys.stderr)
        return 2
    source_root = pathlib.Path(sys.argv[1])
    failures: list[str] = []
    for path in sorted(source_root.rglob("*")):
        if path.suffix not in {".cpp", ".hpp"} or "runtime" in path.parts:
            continue
        text = path.read_text(encoding="utf-8")
        for name, pattern in FORBIDDEN.items():
            for match in pattern.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                failures.append(f"{path}:{line}: {name} must use a runtime port")

    process = (source_root / "runtime/provider_process_adapter.cpp").read_text(
        encoding="utf-8"
    )
    provider_header = (
        source_root.parent / "include/cxxlens/sdk/provider.hpp"
    ).read_text(encoding="utf-8")
    runtime_tests = (
        source_root.parent / "tests/unit/sdk/provider_runtime_test.cpp"
    ).read_text(encoding="utf-8")
    for marker in (
        "class provider_process_port",
        "process_invocation",
        "process_output",
        "make_system_provider_process_port",
    ):
        if marker not in provider_header:
            failures.append(f"provider process port marker missing: {marker}")
    for marker in (
        "no-shell-argv-exec",
        "network-syscall-deny",
        "(void)::kill(-child",
        "provider.binary-identity-mismatch",
    ):
        if marker not in process:
            failures.append(f"provider process adapter marker missing: {marker}")
    for fixture in (
        "timeout",
        "cancel",
        "output limit",
        "binary-identity",
        "sandbox",
    ):
        if fixture not in runtime_tests.lower():
            failures.append(f"provider process regression fixture missing: {fixture}")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("validated runtime-port isolation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
