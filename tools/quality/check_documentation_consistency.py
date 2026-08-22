#!/usr/bin/env python3
"""Check links in active documentation without generating an asset ledger."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[2]
LINK = re.compile(r"!?(?:\[[^\]]*\])\(([^)]+)\)")


class DocumentationError(ValueError):
    pass


def resolve(root: pathlib.Path, source: pathlib.Path, target: str) -> pathlib.Path:
    target = urllib.parse.unquote(target.split("#", 1)[0].split("?", 1)[0]).strip()
    if target.startswith("<") and target.endswith(">"):
        target = target[1:-1]
    return root / target.removeprefix("/") if target.startswith("/") else source.parent / target


def check(root: pathlib.Path) -> None:
    for source in root.rglob("*.md"):
        relative_source = source.relative_to(root).as_posix()
        if relative_source.startswith(("docs/archive/", ".claude/", ".git/", "build/")):
            continue
        for match in LINK.finditer(source.read_text(encoding="utf-8")):
            target = match.group(1).strip().split(maxsplit=1)[0]
            if not target or target.startswith(("#", "http://", "https://", "mailto:")):
                continue
            resolved = resolve(root, source, target).resolve()
            try:
                relative = resolved.relative_to(root.resolve()).as_posix()
            except ValueError as error:
                raise DocumentationError(
                    f"documentation link escapes repository: {relative_source} -> {target}"
                ) from error
            if not resolved.exists():
                raise DocumentationError(f"broken documentation link: {relative_source} -> {target}")
            if relative.startswith("docs/archive/") and relative_source not in {"README.md", "docs/README.md"}:
                raise DocumentationError(f"active documentation links to archive: {relative_source} -> {target}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    args = parser.parse_args()
    try:
        check(args.root.resolve())
    except (DocumentationError, OSError) as error:
        print(f"documentation consistency check failed: {error}", file=sys.stderr)
        return 1
    print("documentation links are consistent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
