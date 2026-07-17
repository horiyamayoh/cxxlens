#!/usr/bin/env python3
"""Generate and validate the completed next-generation asset ledger."""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import re
import subprocess
import sys
import urllib.parse
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
POLICY = pathlib.Path("schemas/cxxlens_asset_migration_policy.yaml")
POLICY_SCHEMA = pathlib.Path("schemas/cxxlens_asset_migration_policy.schema.yaml")
LEDGER = pathlib.Path("schemas/cxxlens_asset_migration_ledger.json")
LEDGER_SCHEMA = pathlib.Path("schemas/cxxlens_asset_migration_ledger.schema.yaml")
LINK = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")


class DocumentationError(ValueError):
    pass


def fail(message: str) -> None:
    raise DocumentationError(message)


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail(f"expected mapping: {path}")
    return value


def validate_schema(document: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"{label} schema validation failed: {error.message}")


def repository_assets(root: pathlib.Path) -> list[str]:
    completed = subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "-z",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        fail(f"git ls-files failed: {completed.stderr.strip()}")
    paths = {item for item in completed.stdout.split("\0") if item}
    paths.add(LEDGER.as_posix())
    return sorted(
        relative
        for relative in paths
        if relative == LEDGER.as_posix() or (root / relative).is_file()
    )


def generate_ledger(root: pathlib.Path) -> dict[str, Any]:
    policy = load_yaml(root / POLICY)
    validate_schema(policy, load_yaml(root / POLICY_SCHEMA), "asset migration policy")
    archive_prefixes = tuple(policy["archive_prefixes"])
    rows: list[dict[str, Any]] = []
    for path in repository_assets(root):
        if path == LEDGER.as_posix():
            status, authority, replacement, removal = (
                "generated",
                "generated-inventory",
                None,
                None,
            )
        elif path.startswith(archive_prefixes):
            status, authority, replacement, removal = (
                "archived",
                "historical",
                "docs/design/cxxlens_next_generation_integrated_design_ja.md",
                "#72",
            )
        else:
            status, authority, replacement, removal = ("active", "active", None, None)
        rows.append(
            {
                "path": path,
                "status": status,
                "authority": authority,
                "replacement": replacement,
                "removal_issue": removal,
            }
        )
    counts = collections.Counter(row["status"] for row in rows)
    ledger = {
        "schema": "cxxlens.asset-migration-ledger.v2",
        "document_version": "2.0.0",
        "generated_by": "tools/quality/check_documentation_consistency.py",
        "source_policy": POLICY.as_posix(),
        "asset_count": len(rows),
        "classifications": {
            "active": counts["active"],
            "archived": counts["archived"],
            "generated": counts["generated"],
        },
        "assets": rows,
    }
    validate_schema(ledger, load_yaml(root / LEDGER_SCHEMA), "asset migration ledger")
    if len(rows) != len({row["path"] for row in rows}):
        fail("asset migration ledger contains duplicate paths")
    return ledger


def resolve_repo_path(root: pathlib.Path, source: pathlib.Path, target: str) -> pathlib.Path:
    target = urllib.parse.unquote(target.split("#", 1)[0].split("?", 1)[0])
    if target.startswith("<") and target.endswith(">"):
        target = target[1:-1]
    return root / target.removeprefix("/") if target.startswith("/") else source.parent / target


def validate_active_documentation(root: pathlib.Path, ledger: dict[str, Any]) -> None:
    for row in ledger["assets"]:
        if row["status"] != "active" or not row["path"].endswith(".md"):
            continue
        source = root / row["path"]
        for match in LINK.finditer(source.read_text(encoding="utf-8")):
            target = match.group(1).strip().split(maxsplit=1)[0]
            if not target or target.startswith(("#", "http://", "https://", "mailto:")):
                continue
            resolved = resolve_repo_path(root, source, target).resolve()
            try:
                relative = resolved.relative_to(root.resolve()).as_posix()
            except ValueError:
                fail(f"documentation link escapes repository: {row['path']} -> {target}")
            if not resolved.exists():
                fail(f"broken documentation link: {row['path']} -> {target}")
            archive_portal = relative == "docs/archive/README.md" and row["path"] in {
                "README.md",
                "docs/README.md",
            }
            if relative.startswith("docs/archive/") and not archive_portal:
                fail(f"active documentation links to archive: {row['path']} -> {target}")


def render(ledger: dict[str, Any]) -> str:
    return json.dumps(ledger, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("generate", "check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path)
    arguments = parser.parse_args()
    root = arguments.root.resolve()
    try:
        ledger = generate_ledger(root)
        validate_active_documentation(root, ledger)
        expected = render(ledger)
        if arguments.mode == "generate":
            (root / LEDGER).write_text(expected, encoding="utf-8")
        elif arguments.mode == "check":
            if not (root / LEDGER).is_file() or (root / LEDGER).read_text(
                encoding="utf-8"
            ) != expected:
                fail("asset migration ledger is stale")
        else:
            output = arguments.output or pathlib.Path("asset-migration-report.json")
            output.write_text(expected, encoding="utf-8")
    except (DocumentationError, OSError) as error:
        print(f"documentation consistency check failed: {error}", file=sys.stderr)
        return 1
    print("documentation consistency check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
