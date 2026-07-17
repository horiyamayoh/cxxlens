#!/usr/bin/env python3
"""Require the sanitizer runtime's configured fail-closed exit code."""

from __future__ import annotations

import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: expect_failure.py EXECUTABLE MODE")
    result = subprocess.run([sys.argv[1], sys.argv[2]], check=False)
    if result.returncode != 86:
        raise SystemExit(
            f"sanitizer canary returned {result.returncode}; expected configured exit 86"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
