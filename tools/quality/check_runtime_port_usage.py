#!/usr/bin/env python3
"""Reject ambient runtime services in domain implementation code."""

from __future__ import annotations

import pathlib
import re
import sys


FORBIDDEN = {
    "direct filesystem": re.compile(r"std::filesystem::"),
    "command shell": re.compile(r"\b(?:system|popen)\s*\("),
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
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("validated runtime-port isolation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
