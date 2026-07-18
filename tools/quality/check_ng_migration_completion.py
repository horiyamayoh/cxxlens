#!/usr/bin/env python3
"""Fail closed when a superseded architecture asset returns to the active tree."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys


FORBIDDEN_PREFIXES = (
    "contracts/",
    "examples/packages/",
    "examples/m2-flagship/",
    "src/config/",
    "src/core/",
    "src/facts/",
    "src/query/",
    "src/search/",
    "src/select/",
    "src/source/",
    "src/store/",
    "src/testing/",
    "src/workspace/",
    "src/llvm/common/",
)
FORBIDDEN_FILES = {
    ".github/workflows/api-unit.yml",
    "schemas/cxxlens_public_api_contract.yaml",
    "schemas/cxxlens_public_api_contract_freeze.yaml",
    "schemas/cxxlens_legacy_api_baseline.yaml",
    "schemas/cxxlens_ng_authority_transition.yaml",
    "schemas/cxxlens_ng_catalog_bootstrap.schema.yaml",
}
FORBIDDEN_CODE_MARKERS = (
    re.compile(r"\bfact_kind\b"),
    re.compile(r"\bfact_profile\b"),
    re.compile(r"\bng_legacy_fact_store_adapter\b"),
    re.compile(r"\bcxxlens-frontend-worker\b"),
    re.compile(r"\bcxxlens_public_api_contract\b"),
)


class MigrationError(ValueError):
    pass


def fail(message: str) -> None:
    raise MigrationError(message)


def active_files(root: pathlib.Path) -> list[pathlib.Path]:
    ignored = {root / "build", root / ".git", root / "docs/archive"}
    output = []
    for path in root.rglob("*"):
        if not path.is_file() or any(parent == path or parent in path.parents for parent in ignored):
            continue
        if "__pycache__" in path.parts or path.suffix == ".pyc":
            continue
        output.append(path)
    return sorted(output)


def validate(root: pathlib.Path) -> None:
    files = active_files(root)
    relatives = {path.relative_to(root).as_posix() for path in files}
    returned = sorted(
        relative
        for relative in relatives
        if relative in FORBIDDEN_FILES
        or any(relative.startswith(prefix) for prefix in FORBIDDEN_PREFIXES)
    )
    if returned:
        fail(f"superseded assets returned: {returned}")

    schemas = {
        relative
        for relative in relatives
        if relative.startswith("schemas/")
    }
    invalid_schemas = sorted(
        relative
        for relative in schemas
        if not pathlib.Path(relative).name.startswith(
            ("cxxlens_ng_", "cxxlens_asset_migration_")
        )
    )
    if invalid_schemas:
        fail(f"non-next-generation schemas remain: {invalid_schemas}")

    ledger = json.loads(
        (root / "schemas/cxxlens_asset_migration_ledger.json").read_text(
            encoding="utf-8"
        )
    )
    nonterminal = [
        row["path"]
        for row in ledger["assets"]
        if row["status"] not in {"active", "archived", "generated"}
    ]
    if nonterminal:
        fail(f"nonterminal ledger rows remain: {nonterminal}")

    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    for target in (
        "cxxlens::base",
        "cxxlens::kernel",
        "cxxlens::query",
        "cxxlens::cpp",
        "cxxlens::recipes",
        "cxxlens::provider_sdk",
        "cxxlens::clang22_provider_sdk",
    ):
        if target not in cmake:
            fail(f"target DAG is incomplete: {target}")

    scan_roots = ("include/", "src/", "examples/", "tests/", ".github/", "cmake/")
    allowed_marker_file = "tests/install/run_install_test.cmake.in"
    for path in files:
        relative = path.relative_to(root).as_posix()
        if relative == allowed_marker_file or not relative.startswith(scan_roots):
            continue
        if path.suffix not in {".cpp", ".hpp", ".py", ".txt", ".cmake", ".yml", ".yaml"}:
            continue
        text = path.read_text(encoding="utf-8")
        for marker in FORBIDDEN_CODE_MARKERS:
            if marker.search(text):
                fail(f"superseded marker returned: {relative}: {marker.pattern}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    try:
        validate(arguments.root.resolve())
    except (MigrationError, OSError, json.JSONDecodeError) as error:
        print(f"migration completion check failed: {error}", file=sys.stderr)
        return 1
    print("migration completion check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
