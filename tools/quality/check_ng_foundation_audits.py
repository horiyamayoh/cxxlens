#!/usr/bin/env python3
"""Measure the nine NG foundation zero audits and emit a commit-bound report."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys
from typing import Any

import jsonschema
import yaml

import check_ng_migration_completion as migration
import verify_checksums


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCHEMA = pathlib.Path("schemas/cxxlens_ng_foundation_audit_report.schema.yaml")
AUTHORITY_KEYS = ("owner", "owner_issue", "decision_issue", "exact_contract_issue")
AUDIT_IDS = (
    "legacy_assets",
    "legacy_authority_references",
    "legacy_code_paths",
    "legacy_schemas",
    "legacy_public_headers",
    "legacy_ci_gates",
    "migration_blockers",
    "unowned_contracts",
    "documentation_drift",
)


class AuditError(ValueError):
    """A fail-closed foundation audit violation."""


def fail(message: str) -> None:
    raise AuditError(message)


def load_document(path: pathlib.Path) -> dict[str, Any]:
    text = path.read_text(encoding="utf-8")
    value = json.loads(text) if path.suffix == ".json" else yaml.safe_load(text)
    if not isinstance(value, dict):
        fail(f"expected mapping: {path}")
    return value


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def target_digest(root: pathlib.Path, targets: list[str]) -> str:
    material = []
    for target in targets:
        path = root / target
        value = digest_bytes(path.read_bytes()) if path.is_file() else target
        material.append(f"{target}={value}")
    return digest_bytes(("\n".join(material) + "\n").encode("utf-8"))


def audit_entry(
    root: pathlib.Path,
    revision: str,
    tree: str,
    checker: str,
    targets: list[str],
    findings: list[str],
) -> dict[str, Any]:
    canonical_targets = sorted(set(targets))
    canonical_findings = sorted(set(findings))
    return {
        "checker": checker,
        "revision": revision,
        "tree": tree,
        "targets": canonical_targets,
        "target_digest": target_digest(root, canonical_targets),
        "count": len(canonical_findings),
        "finding_ids": canonical_findings,
    }


def relative_files(root: pathlib.Path) -> tuple[list[pathlib.Path], list[str]]:
    files = migration.active_files(root)
    return files, [path.relative_to(root).as_posix() for path in files]


def legacy_asset_findings(root: pathlib.Path, relatives: list[str]) -> tuple[list[str], list[str]]:
    ledger_path = root / "schemas/cxxlens_asset_migration_ledger.json"
    ledger = load_document(ledger_path)
    returned = [
        relative
        for relative in relatives
        if relative in migration.FORBIDDEN_FILES
        or any(relative.startswith(prefix) for prefix in migration.FORBIDDEN_PREFIXES)
    ]
    nonterminal = [
        row["path"]
        for row in ledger["assets"]
        if row["status"] not in {"active", "archived", "generated"}
    ]
    findings = [f"legacy.asset.returned:{path}" for path in returned]
    findings.extend(f"legacy.asset.nonterminal:{path}" for path in nonterminal)
    return ["schemas/cxxlens_asset_migration_ledger.json", *relatives], findings


def legacy_authority_reference_findings(
    root: pathlib.Path, authority_documents: list[str]
) -> tuple[list[str], list[str]]:
    findings = []
    link_pattern = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
    legacy_names = {
        "schemas/cxxlens_public_api_contract.yaml",
        "schemas/cxxlens_public_api_contract_freeze.yaml",
        "schemas/cxxlens_legacy_api_baseline.yaml",
    }
    for relative in authority_documents:
        path = root / relative
        if not path.is_file():
            findings.append(f"legacy.authority.missing:{relative}")
            continue
        text = path.read_text(encoding="utf-8")
        if path.suffix == ".md":
            targets = [match.group(1).strip() for match in link_pattern.finditer(text)]
        else:
            document = load_document(path)
            targets = []

            def collect(value: Any) -> None:
                if isinstance(value, str):
                    targets.append(value)
                elif isinstance(value, list):
                    for item in value:
                        collect(item)
                elif isinstance(value, dict):
                    for item in value.values():
                        collect(item)

            collect(document)
        if any("docs/archive/" in target for target in targets):
            findings.append(f"legacy.authority.archive-link:{relative}")
        if any(target in legacy_names for target in targets):
            findings.append(f"legacy.authority.old-contract:{relative}")
    return authority_documents, findings


def legacy_code_findings(
    root: pathlib.Path, files: list[pathlib.Path], relatives: list[str]
) -> tuple[list[str], list[str]]:
    scan_roots = ("include/", "src/", "examples/", "tests/", ".github/", "cmake/")
    allowed_marker_file = "tests/install/run_install_test.cmake.in"
    targets = []
    findings = []
    for path, relative in zip(files, relatives, strict=True):
        if relative == allowed_marker_file or not relative.startswith(scan_roots):
            continue
        if path.suffix not in {".cpp", ".hpp", ".py", ".txt", ".cmake", ".yml", ".yaml"}:
            continue
        targets.append(relative)
        text = path.read_text(encoding="utf-8")
        for marker in migration.FORBIDDEN_CODE_MARKERS:
            if marker.search(text):
                findings.append(f"legacy.code.marker:{relative}:{marker.pattern}")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    targets.append("CMakeLists.txt")
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
            findings.append(f"legacy.code.target-dag-missing:{target}")
    return targets, findings


def legacy_schema_findings(relatives: list[str]) -> tuple[list[str], list[str]]:
    targets = [path for path in relatives if path.startswith("schemas/")]
    invalid = [
        path
        for path in targets
        if not pathlib.Path(path).name.startswith(("cxxlens_ng_", "cxxlens_asset_migration_"))
    ]
    return targets, [f"legacy.schema:{path}" for path in invalid]


def legacy_public_header_findings(relatives: list[str]) -> tuple[list[str], list[str]]:
    headers = {path for path in relatives if path.startswith("include/cxxlens/")}
    findings = [
        *(f"legacy.public-header.missing:{path}" for path in migration.ALLOWED_PUBLIC_HEADERS - headers),
        *(f"legacy.public-header.extra:{path}" for path in headers - migration.ALLOWED_PUBLIC_HEADERS),
    ]
    return sorted(headers | migration.ALLOWED_PUBLIC_HEADERS), findings


def legacy_ci_findings(relatives: list[str]) -> tuple[list[str], list[str]]:
    targets = [path for path in relatives if path.startswith(".github/workflows/")]
    forbidden = sorted(set(targets) & migration.FORBIDDEN_FILES)
    return targets, [f"legacy.ci-gate:{path}" for path in forbidden]


def blocker_findings(issue_states: dict[int, str]) -> tuple[list[str], list[str]]:
    targets = [f"github.issue:{number}:{state}" for number, state in sorted(issue_states.items())]
    findings = [
        f"github.issue.open:{number}"
        for number, state in sorted(issue_states.items())
        if state == "open" and number not in (56, 71)
    ]
    return targets, findings


def unowned_contract_findings(
    root: pathlib.Path, authority_documents: list[str]
) -> tuple[list[str], list[str]]:
    targets = [
        relative
        for relative in authority_documents
        if relative.startswith("schemas/") and relative.endswith(".yaml")
    ]
    findings = []
    for relative in targets:
        authority = load_document(root / relative).get("authority")
        if not isinstance(authority, dict) or not any(key in authority for key in AUTHORITY_KEYS):
            findings.append(f"contract.unowned:{relative}")
    return targets, findings


def documentation_drift_findings(root: pathlib.Path) -> tuple[list[str], list[str]]:
    targets = [verify_checksums.MANIFEST.as_posix(), *verify_checksums.PACKAGE_PATHS]
    expected = verify_checksums.render(root)
    manifest = root / verify_checksums.MANIFEST
    findings = []
    if not manifest.is_file():
        findings.append(f"documentation.checksum-missing:{verify_checksums.MANIFEST}")
    elif manifest.read_text(encoding="utf-8") != expected:
        findings.append(f"documentation.checksum-drift:{verify_checksums.MANIFEST}")
    return targets, findings


def build_report(
    root: pathlib.Path,
    manifest: dict[str, Any],
    revision: str,
    tree: str,
    issue_states: dict[int, str],
) -> dict[str, Any]:
    files, relatives = relative_files(root)
    authorities = sorted(manifest["authority_documents"])
    measurements: dict[str, tuple[str, tuple[list[str], list[str]]]] = {
        "legacy_assets": (
            "foundation-audit.legacy-assets/v1",
            legacy_asset_findings(root, relatives),
        ),
        "legacy_authority_references": (
            "foundation-audit.legacy-authority-references/v1",
            legacy_authority_reference_findings(root, authorities),
        ),
        "legacy_code_paths": (
            "foundation-audit.legacy-code-paths/v1",
            legacy_code_findings(root, files, relatives),
        ),
        "legacy_schemas": (
            "foundation-audit.legacy-schemas/v1",
            legacy_schema_findings(relatives),
        ),
        "legacy_public_headers": (
            "foundation-audit.legacy-public-headers/v1",
            legacy_public_header_findings(relatives),
        ),
        "legacy_ci_gates": (
            "foundation-audit.legacy-ci-gates/v1",
            legacy_ci_findings(relatives),
        ),
        "migration_blockers": (
            "foundation-audit.current-github-blockers/v1",
            blocker_findings(issue_states),
        ),
        "unowned_contracts": (
            "foundation-audit.authority-ownership/v1",
            unowned_contract_findings(root, authorities),
        ),
        "documentation_drift": (
            "foundation-audit.documentation-checksums/v1",
            documentation_drift_findings(root),
        ),
    }
    if set(measurements) != set(AUDIT_IDS):
        fail("foundation audit implementation set differs")
    audits = {
        identifier: audit_entry(root, revision, tree, checker, targets, findings)
        for identifier, (checker, (targets, findings)) in sorted(measurements.items())
    }
    return {
        "schema": "cxxlens.ng-foundation-audit-report.v1",
        "revision": revision,
        "tree": tree,
        "audits": audits,
    }


def validate_report(root: pathlib.Path, report: dict[str, Any]) -> None:
    schema = load_document(root / SCHEMA)
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(report)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"foundation audit report schema validation failed: {error.message}")


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--issue-states", type=pathlib.Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--tree", required=True)
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    try:
        manifest = load_document(args.manifest)
        raw_states = load_document(args.issue_states)
        issue_states = {int(number): state for number, state in raw_states.items()}
        report = build_report(root, manifest, args.revision, args.tree, issue_states)
        validate_report(root, report)
        print(json.dumps(report, ensure_ascii=False, sort_keys=True))
        findings = {
            identifier: entry["finding_ids"]
            for identifier, entry in report["audits"].items()
            if entry["count"]
        }
        if args.command == "check" and findings:
            fail(f"foundation audit findings are nonzero: {findings}")
    except (AuditError, OSError, json.JSONDecodeError, yaml.YAMLError) as error:
        print(f"foundation audit check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
