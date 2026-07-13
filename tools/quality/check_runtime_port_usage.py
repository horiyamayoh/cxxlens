#!/usr/bin/env python3
"""Reject ambient runtime services in domain implementation code."""

from __future__ import annotations

import pathlib
import re
import sys


FORBIDDEN = {
    "direct filesystem": re.compile(r"std::filesystem::"),
    # A public domain method may legitimately be named `system()` (for example,
    # file_selector::system). Only unqualified C process entry points are ambient services.
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
    process = (source_root / "runtime/process_adapter.cpp").read_text(encoding="utf-8")
    runtime_tests = (
        source_root.parent / "tests/unit/runtime/runtime_ports_test.cpp"
    ).read_text(encoding="utf-8")
    if "::fork(" in process:
        failures.append("process adapter retains a multithread-unsafe fork child path")
    for marker in (
        "::posix_spawnp",
        "POSIX_SPAWN_SETPGROUP",
        "::pipe2",
        "set_nonblocking",
        "drain_failure",
        "terminate_group_and_reap",
        "::memfd_create",
        "termination_signal",
    ):
        if marker not in process:
            failures.append(f"process adapter fail-closed marker missing: {marker}")
    for fixture in (
        "final drain output limit was accepted",
        "stdout and stderr did not share an output limit",
        "nonblocking setup failure was accepted",
        "concurrent production launches were not reliable",
        "timeout/cancellation left a live descendant",
        "anonymous standard input transport truncated process input",
        "worker signal termination evidence was lost",
    ):
        if fixture not in runtime_tests:
            failures.append(f"production process regression fixture missing: {fixture}")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("validated runtime-port isolation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
