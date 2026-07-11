#!/usr/bin/env python3
"""Discover positive/negative/ambiguous fixtures in canonical order."""

from __future__ import annotations

import pathlib
import sys

import yaml


def main() -> int:
    root = pathlib.Path(sys.argv[1]).resolve()
    expected_categories = {"positive", "negative", "ambiguous"}
    discovered: list[tuple[str, str, pathlib.Path]] = []
    for path in sorted(root.rglob("*.yaml"), key=lambda value: value.as_posix()):
        if path.is_symlink() or root not in path.resolve().parents:
            raise AssertionError(f"fixture escaped discovery root: {path}")
        document = yaml.safe_load(path.read_text(encoding="utf-8"))
        if document.get("schema") != "cxxlens.testing.case.v1":
            raise AssertionError(f"unsupported fixture schema: {path}")
        category = document.get("category")
        name = document.get("name")
        if category not in expected_categories or not isinstance(name, str) or not name:
            raise AssertionError(f"invalid fixture metadata: {path}")
        discovered.append((category, name, path.relative_to(root)))
    if {category for category, _, _ in discovered} != expected_categories:
        raise AssertionError("positive/negative/ambiguous fixture categories are required")
    keys = [(category, name) for category, name, _ in discovered]
    if len(keys) != len(set(keys)):
        raise AssertionError("fixture category/name must be unique")
    for category, name, path in discovered:
        print(f"{category}:{name}:{path.as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
