#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys


def main() -> int:
    executable = pathlib.Path(sys.argv[1])
    result = subprocess.run(
        ["ldd", str(executable)],
        check=True,
        capture_output=True,
        text=True,
    )
    forbidden = []
    for line in result.stdout.splitlines():
        dependency = line.strip().split("=>", 1)[0].strip()
        dependency_name = pathlib.Path(dependency).name.lower()
        if "llvm" in dependency_name or "clang" in dependency_name:
            forbidden.append(line)
    if forbidden:
        raise SystemExit(
            "provider/query/store link closure exposed LLVM/Clang:\n" + "\n".join(forbidden)
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
