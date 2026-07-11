#!/usr/bin/env python3
"""Reject forbidden ambient inputs and containers in canonical identity code."""

from __future__ import annotations

import pathlib
import re
import sys


FORBIDDEN = {
    "std::hash": re.compile(r"std::hash\s*<"),
    "unordered iteration": re.compile(r"std::unordered_(?:map|set)"),
    "ambient random": re.compile(r"(?:random_device|mt19937|rand\s*\()"),
    "ambient wall clock": re.compile(r"system_clock::now\s*\("),
    "ambient thread identity": re.compile(r"this_thread::get_id\s*\("),
    "pointer address encoding": re.compile(r"reinterpret_cast\s*<\s*(?:u?intptr_t|std::u?intptr_t)"),
}


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    failures: list[str] = []
    for path in sorted((root / "src/core").glob("*canonical*")):
        text = path.read_text(encoding="utf-8")
        for reason, pattern in FORBIDDEN.items():
            if pattern.search(text):
                failures.append(f"{path}: canonical identity uses {reason}")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("validated canonical identity input isolation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
