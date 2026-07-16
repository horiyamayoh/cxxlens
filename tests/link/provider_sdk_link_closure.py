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
    lowered = result.stdout.lower()
    forbidden = [line for line in lowered.splitlines() if "llvm" in line or "clang" in line]
    if forbidden:
        raise SystemExit(
            "provider/query/store link closure exposed LLVM/Clang:\n" + "\n".join(forbidden)
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
