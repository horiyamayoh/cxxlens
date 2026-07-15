#!/usr/bin/env python3
"""Generate and validate the next-generation documentation migration ledger."""

from __future__ import annotations

import argparse
import collections
import hashlib
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
CATALOG_SCHEMA = pathlib.Path("schemas/cxxlens_ng_catalog_bootstrap.schema.yaml")
CATALOGS = {
    "relation-registry": pathlib.Path("schemas/cxxlens_ng_relation_registry.yaml"),
    "provider-protocol": pathlib.Path("schemas/cxxlens_ng_provider_protocol.yaml"),
    "public-cpp-api-catalog": pathlib.Path("schemas/cxxlens_ng_public_api_catalog.yaml"),
    "acceptance-manifest": pathlib.Path("schemas/cxxlens_ng_acceptance_manifest.yaml"),
    "security-profile": pathlib.Path("schemas/cxxlens_ng_security_profile.yaml"),
}
LINK = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")


class DocumentationError(ValueError):
    """A stable documentation or migration-ledger invariant violation."""


def fail(message: str) -> None:
    raise DocumentationError(message)


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    document = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        fail(f"expected mapping: {path}")
    return document


def validate_schema(document: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"{label} schema validation failed: {error.message}")


def sha256(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def git(root: pathlib.Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        fail(f"git {' '.join(arguments)} failed: {completed.stderr.strip()}")
    return completed.stdout


def repository_assets(root: pathlib.Path) -> list[str]:
    # The ledger is a contract over repository-owned assets. CI setup may create
    # untracked helper files in the checkout, so only the Git index is authoritative.
    # Contributors stage newly added assets before regenerating the ledger.
    output = git(root, "ls-files", "--cached", "-z")
    paths = {item for item in output.split("\0") if item}
    paths.add(LEDGER.as_posix())
    result = []
    for relative in sorted(paths):
        path = root / relative
        if relative == LEDGER.as_posix() or path.is_file() or path.is_symlink():
            result.append(relative)
    return result


def validate_policy(policy: dict[str, Any], root: pathlib.Path) -> None:
    validate_schema(policy, load_yaml(root / POLICY_SCHEMA), "asset migration policy")
    prefixes = [row["prefix"] for row in policy["default_rules"]]
    if len(prefixes) != len(set(prefixes)):
        fail("asset migration policy has duplicate default prefixes")
    for left in prefixes:
        for right in prefixes:
            if left != right and right.startswith(left):
                fail(f"asset migration default prefixes overlap: {left}, {right}")
    override_paths = [row["path"] for row in policy["overrides"]]
    if len(override_paths) != len(set(override_paths)):
        fail("asset migration policy has duplicate path overrides")
    marker_ids = [row["id"] for row in policy["drift"]["stale_markers"]]
    if len(marker_ids) != len(set(marker_ids)):
        fail("documentation drift policy has duplicate marker IDs")
    for marker in policy["drift"]["stale_markers"]:
        try:
            re.compile(marker["pattern"])
        except re.error as error:
            fail(f"invalid stale marker regex {marker['id']}: {error}")


def validate_record(path: str, record: dict[str, Any]) -> None:
    disposition = record["disposition"]
    status = record["status"]
    replacement = record["replacement"]
    removal = record["removal_issue"]
    if status == "active" and removal is not None:
        fail(f"active asset cannot have a removal issue: {path}")
    if status in {"planned", "redirect", "archived", "legacy-baseline"} and removal is None:
        fail(f"nonterminal or legacy asset has no removal issue: {path}")
    if disposition in {"replace", "archive", "delete"} and replacement is None:
        fail(f"asset disposition has no replacement: {path}")
    if status == "redirect" and disposition != "replace":
        fail(f"redirect asset must use replace disposition: {path}")
    if status == "archived" and disposition != "archive":
        fail(f"archived asset must use archive disposition: {path}")


def generate_ledger(root: pathlib.Path) -> tuple[dict[str, Any], dict[str, Any]]:
    policy = load_yaml(root / POLICY)
    validate_policy(policy, root)
    assets = repository_assets(root)
    overrides = {row["path"]: row["record"] for row in policy["overrides"]}
    unknown_overrides = sorted(set(overrides) - set(assets))
    if unknown_overrides:
        fail(f"asset migration overrides name missing assets: {unknown_overrides}")
    rows = []
    for path in assets:
        if path in overrides:
            record = dict(overrides[path])
        else:
            matches = [
                row["record"]
                for row in policy["default_rules"]
                if path.startswith(row["prefix"])
            ]
            if len(matches) != 1:
                fail(f"asset must match exactly one migration rule: {path} ({len(matches)})")
            record = dict(matches[0])
        validate_record(path, record)
        rows.append({"path": path, **record})
    counts = collections.Counter(row["disposition"] for row in rows)
    ledger = {
        "schema": "cxxlens.asset-migration-ledger.v1",
        "document_version": "1.0.0",
        "generated_by": "tools/quality/check_documentation_consistency.py",
        "source_policy": POLICY.as_posix(),
        "source_policy_digest": sha256((root / POLICY).read_bytes()),
        "asset_count": len(rows),
        "classifications": dict(sorted(counts.items())),
        "assets": rows,
    }
    validate_schema(ledger, load_yaml(root / LEDGER_SCHEMA), "asset migration ledger")
    if ledger["asset_count"] != len({row["path"] for row in rows}):
        fail("asset migration ledger does not classify paths exactly once")
    return policy, ledger


def render_ledger(ledger: dict[str, Any]) -> str:
    return json.dumps(ledger, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def front_matter(path: pathlib.Path) -> dict[str, Any]:
    text = path.read_text(encoding="utf-8")
    if not text.startswith("---\n"):
        fail(f"document metadata front matter is missing: {path}")
    end = text.find("\n---\n", 4)
    if end < 0:
        fail(f"document metadata front matter is unterminated: {path}")
    value = yaml.safe_load(text[4:end])
    if not isinstance(value, dict):
        fail(f"document metadata front matter is not a mapping: {path}")
    return value


def resolve_repo_path(root: pathlib.Path, source: pathlib.Path, target: str) -> pathlib.Path:
    target = urllib.parse.unquote(target.split("#", 1)[0].split("?", 1)[0])
    if target.startswith("<") and target.endswith(">"):
        target = target[1:-1]
    if target.startswith("/"):
        return root / target.removeprefix("/")
    return source.parent / target


def validate_links(root: pathlib.Path, active_markdown: list[str]) -> None:
    for relative in active_markdown:
        source = root / relative
        text = source.read_text(encoding="utf-8")
        for match in LINK.finditer(text):
            target = match.group(1).strip()
            if not target or target.startswith(("#", "http://", "https://", "mailto:")):
                continue
            target = target.split(maxsplit=1)[0]
            resolved = resolve_repo_path(root, source, target).resolve()
            try:
                normalized = resolved.relative_to(root.resolve()).as_posix()
            except ValueError:
                fail(f"documentation link escapes the repository: {relative} -> {target}")
            if not resolved.exists():
                fail(f"broken documentation link: {relative} -> {target}")
            if normalized.startswith(("docs/archive/legacy-v1/", "docs/archive/pre-cxxlens/")):
                fail(f"active documentation links directly to archived authority: {relative} -> {target}")


def validate_archives_and_redirects(root: pathlib.Path, ledger: dict[str, Any]) -> None:
    for row in ledger["assets"]:
        path = row["path"]
        if row["status"] == "archived":
            metadata = front_matter(root / path)
            expected = {
                "cxxlens_document_status": "archived",
                "cxxlens_authority": "non-normative",
                "cxxlens_replacement": row["replacement"],
                "cxxlens_removal_issue": row["removal_issue"],
            }
            for key, value in expected.items():
                if metadata.get(key) != value:
                    fail(f"archive metadata differs for {path}: {key}")
        if row["status"] == "redirect":
            metadata = front_matter(root / path)
            if (
                metadata.get("cxxlens_document_status") != "redirect"
                or metadata.get("cxxlens_authority") != "non-normative"
                or metadata.get("cxxlens_replacement") != row["replacement"]
                or metadata.get("cxxlens_removal_issue") != row["removal_issue"]
            ):
                fail(f"redirect metadata differs: {path}")
            archive = metadata.get("cxxlens_archive")
            if not isinstance(archive, str) or not (root / archive).is_file():
                fail(f"redirect archive target is missing: {path}")
        replacement = row["replacement"]
        if replacement is not None:
            replacement_path = replacement.split("#", 1)[0]
            if not (root / replacement_path).exists():
                fail(f"asset replacement path is missing: {path} -> {replacement}")


def validate_catalogs(root: pathlib.Path) -> None:
    schema = load_yaml(root / CATALOG_SCHEMA)
    index = (root / "docs/design/catalogs/README.md").read_text(encoding="utf-8")
    for expected_kind, relative in CATALOGS.items():
        document = load_yaml(root / relative)
        validate_schema(document, schema, f"NG {expected_kind}")
        if document["kind"] != expected_kind or document["maturity"] != "bootstrap":
            fail(f"NG catalog bootstrap state differs: {relative}")
        if relative.as_posix() not in index:
            fail(f"NG catalog index does not reference {relative}")
        entries = {entry["id"]: entry for entry in document["entries"]}
        if len(entries) != len(document["entries"]):
            fail(f"NG catalog has duplicate entry IDs: {relative}")
        for entry in entries.values():
            missing = sorted(set(entry.get("depends_on", [])) - set(entries))
            if missing:
                fail(f"NG catalog has missing dependencies: {relative}: {missing}")
        for replacement in document["replaces"]:
            if not (root / replacement).exists():
                fail(f"NG catalog replacement source is missing: {relative}: {replacement}")


def validate_stale_markers(root: pathlib.Path, policy: dict[str, Any]) -> None:
    active = policy["drift"]["active_markdown"]
    for marker in policy["drift"]["stale_markers"]:
        pattern = re.compile(marker["pattern"], re.IGNORECASE)
        allowed = set(marker["allowed_paths"])
        unknown_allowed = sorted(allowed - set(active))
        if unknown_allowed:
            fail(f"stale marker allowlist names non-active documents: {marker['id']}: {unknown_allowed}")
        for relative in active:
            if relative in allowed:
                continue
            if pattern.search((root / relative).read_text(encoding="utf-8")):
                fail(f"stale marker {marker['id']} appears in active documentation: {relative}")


def validate_documentation(root: pathlib.Path, policy: dict[str, Any], ledger: dict[str, Any]) -> None:
    active_from_ledger = sorted(
        row["path"]
        for row in ledger["assets"]
        if row["status"] == "active"
        and row["path"].endswith(".md")
        and row["authority"] in {"normative", "informative"}
    )
    active_from_policy = sorted(policy["drift"]["active_markdown"])
    if active_from_ledger != active_from_policy:
        fail("active Markdown drift set differs from generated ledger")
    validate_links(root, active_from_policy)
    validate_archives_and_redirects(root, ledger)
    validate_catalogs(root)
    validate_stale_markers(root, policy)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("generate", "check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    policy, ledger = generate_ledger(root)
    expected = render_ledger(ledger)
    if args.mode == "generate":
        (root / LEDGER).write_text(expected, encoding="utf-8")
    else:
        actual = (root / LEDGER).read_text(encoding="utf-8")
        if actual != expected:
            fail("generated asset migration ledger is stale; run generate mode")
    validate_documentation(root, policy, ledger)
    if args.mode == "report":
        if args.output is None:
            fail("report mode requires --output")
        if git(root, "status", "--porcelain").strip():
            fail("commit-bound documentation report requires a clean worktree")
        output = args.output if args.output.is_absolute() else root / args.output
        output.parent.mkdir(parents=True, exist_ok=True)
        report = {
            "schema": "cxxlens.documentation-consistency-report.v1",
            "status": "green",
            "issue": "#58",
            "commit": git(root, "rev-parse", "HEAD").strip(),
            "tree": git(root, "rev-parse", "HEAD^{tree}").strip(),
            "asset_count": ledger["asset_count"],
            "classifications": ledger["classifications"],
            "ledger_digest": sha256((root / LEDGER).read_bytes()),
            "active_markdown_count": len(policy["drift"]["active_markdown"]),
            "catalog_count": len(CATALOGS),
        }
        output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        "documentation consistency passed: "
        f"{ledger['asset_count']} assets, {len(policy['drift']['active_markdown'])} active Markdown, "
        f"{len(CATALOGS)} NG catalogs"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        DocumentationError,
        json.JSONDecodeError,
        jsonschema.ValidationError,
        OSError,
        subprocess.SubprocessError,
        yaml.YAMLError,
    ) as error:
        print(f"documentation consistency failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
