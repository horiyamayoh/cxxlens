#!/usr/bin/env python3
"""Check that external GitHub Actions are pinned to immutable commits."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOWS = (
    pathlib.Path(".github/workflows/quality.yml"),
    pathlib.Path(".github/workflows/release.yml"),
)
PINNED_ACTION = re.compile(r"(?:^|\s)uses:\s+[^@\s]+@([0-9a-f]{40})(?:\s|$)")


class CiSupplyChainError(ValueError):
    """An external workflow action is not pinned to a commit."""


def check(root: pathlib.Path) -> None:
    """Validate action pinning without imposing a workflow/job layout."""
    for relative in WORKFLOWS:
        path = root / relative
        if not path.is_file():
            raise CiSupplyChainError(f"workflow is missing: {relative}")
        for line in path.read_text(encoding="utf-8").splitlines():
            if "uses:" not in line or "./" in line:
                continue
            if PINNED_ACTION.search(line) is None:
                raise CiSupplyChainError(
                    f"external action is not pinned to a commit: {relative}: {line.strip()}"
                )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    args = parser.parse_args()
    try:
        check(args.root.resolve())
    except (CiSupplyChainError, OSError) as error:
        print(f"CI action pin check failed: {error}", file=sys.stderr)
        return 1
    print("external workflow actions are pinned to commits")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
