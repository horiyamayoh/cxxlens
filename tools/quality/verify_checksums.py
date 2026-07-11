#!/usr/bin/env python3
"""Verify the checked-in design package checksum manifest."""

from __future__ import annotations

import hashlib
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} SHA256SUMS", file=sys.stderr)
        return 2
    manifest = pathlib.Path(sys.argv[1]).resolve()
    failures: list[str] = []
    checked = 0
    for line in manifest.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        expected, relative_path = line.split(maxsplit=1)
        candidate = (manifest.parent / relative_path).resolve()
        actual = hashlib.sha256(candidate.read_bytes()).hexdigest()
        checked += 1
        if actual != expected:
            failures.append(f"{relative_path}: expected {expected}, got {actual}")
    if failures:
        print("design package checksum failures:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    print(f"verified {checked} design package checksums")
    return 0


if __name__ == "__main__":
    sys.exit(main())
