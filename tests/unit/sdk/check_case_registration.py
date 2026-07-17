#!/usr/bin/env python3
"""Verify that every selectable SDK case is registered as an individual CTest."""

from __future__ import annotations

import argparse
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    parser.add_argument("expected", nargs="+")
    args = parser.parse_args()
    completed = subprocess.run(
        [args.executable, "--list"], check=True, capture_output=True, text=True
    )
    actual = [line for line in completed.stdout.splitlines() if line]
    if actual != args.expected:
        raise SystemExit(f"SDK case registration differs: expected={args.expected}, actual={actual}")
    if len(actual) != len(set(actual)):
        raise SystemExit("SDK case registration contains duplicates")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
