#!/usr/bin/env python3
"""Reject LLVM/Clang type leakage from stable public headers."""

from __future__ import annotations

import pathlib
import re
import sys


FORBIDDEN = (
    re.compile(r"\bclang::"),
    re.compile(r"\bllvm::"),
    re.compile(r"#\s*include\s*[<\"](?:clang|llvm)/"),
)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} PUBLIC_HEADER_DIRECTORY", file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1])
    violations: list[str] = []
    for header in sorted(root.rglob("*.hpp")):
        if header.relative_to(root) == pathlib.Path("interop/clang.hpp"):
            continue
        for line_number, line in enumerate(header.read_text(encoding="utf-8").splitlines(), 1):
            if any(pattern.search(line) for pattern in FORBIDDEN):
                violations.append(f"{header}:{line_number}: {line.strip()}")
    if violations:
        print("public header boundary violations:\n" + "\n".join(violations), file=sys.stderr)
        return 1
    print(f"validated ordinary public header boundary under {root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
