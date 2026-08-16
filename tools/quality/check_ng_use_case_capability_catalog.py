#!/usr/bin/env python3
"""Project the accepted readiness use-case declarations into a checked catalog view.

The readiness document remains the semantic authority.  This report is deliberately
only a demand-reference projection: it does not invent consumer, dependency,
provider, relation, or qualification facts that the accepted readiness contract does
not contain.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import subprocess
import sys
from typing import Any, Mapping

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
READINESS_PATH = pathlib.Path("schemas/cxxlens_ng_api_development_readiness.yaml")
READINESS_SCHEMA_PATH = pathlib.Path(
    "schemas/cxxlens_ng_api_development_readiness.schema.yaml"
)
CATALOG_SCHEMA_PATH = pathlib.Path(
    "schemas/cxxlens_ng_use_case_capability_catalog.schema.yaml"
)
CATALOG_SOURCE_PATHS = (
    READINESS_PATH,
    READINESS_SCHEMA_PATH,
    CATALOG_SCHEMA_PATH,
)


class CatalogError(ValueError):
    """A fail-closed catalog projection violation."""


def _source_bytes(
    path: pathlib.Path,
    *,
    source_bytes: Mapping[str, bytes] | None,
    root: pathlib.Path | None,
) -> bytes:
    if source_bytes is None:
        try:
            return path.read_bytes()
        except (OSError, UnicodeError) as error:
            raise CatalogError(f"cannot read source: {path}") from error
    if root is None:
        raise CatalogError(f"bound source root is missing: {path}")
    try:
        relative = path.absolute().relative_to(root.absolute()).as_posix()
    except ValueError as error:
        raise CatalogError(f"source is outside bound root: {path}") from error
    if relative not in source_bytes:
        raise CatalogError(f"source was not bound to HEAD: {relative}")
    return source_bytes[relative]


def load_yaml(
    path: pathlib.Path,
    *,
    source_bytes: Mapping[str, bytes] | None = None,
    root: pathlib.Path | None = None,
) -> Any:
    try:
        return yaml.safe_load(
            _source_bytes(path, source_bytes=source_bytes, root=root).decode("utf-8")
        )
    except (CatalogError, UnicodeError, yaml.YAMLError) as error:
        raise CatalogError(f"cannot load YAML: {path}: {error}") from error


def validate(document: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(
            schema,
            format_checker=jsonschema.Draft202012Validator.FORMAT_CHECKER,
        ).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise CatalogError(f"{label} schema validation failed: {error.message}") from error


def git_value(root: pathlib.Path, expression: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "rev-parse", expression],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise CatalogError(f"cannot bind source revision: {expression}") from error
    value = result.stdout.strip()
    if len(value) != 40 or any(character not in "0123456789abcdef" for character in value):
        raise CatalogError(f"git returned a non-canonical revision for {expression}")
    return value


def reject_dirty_source_files(root: pathlib.Path) -> None:
    try:
        result = subprocess.run(
            [
                "git",
                "-C",
                str(root),
                "status",
                "--porcelain=v1",
                "--untracked-files=all",
                "--",
                *(path.as_posix() for path in CATALOG_SOURCE_PATHS),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise CatalogError("cannot verify relevant catalog source cleanliness") from error
    if result.stdout.strip():
        raise CatalogError(
            "relevant catalog source files are dirty; commit or discard them before "
            "binding the report revision/tree"
        )


def sha256(
    path: pathlib.Path,
    *,
    source_bytes: Mapping[str, bytes] | None = None,
    root: pathlib.Path | None = None,
) -> str:
    try:
        return "sha256:" + hashlib.sha256(
            _source_bytes(path, source_bytes=source_bytes, root=root)
        ).hexdigest()
    except CatalogError as error:
        raise CatalogError(f"cannot digest source: {path}") from error


def validate_readiness_document(readiness: Any, readiness_schema: Any) -> None:
    if not isinstance(readiness_schema, dict):
        raise CatalogError("readiness schema must be a mapping")
    validate(readiness, readiness_schema, "readiness")


def project_use_cases(readiness: dict[str, Any]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    direction = readiness.get("product_direction")
    if not isinstance(direction, dict):
        raise CatalogError("product_direction is missing")
    if direction.get("contract") != "product.direction.semantic-knowledge-platform.v1":
        raise CatalogError("product direction contract is not the accepted direction")
    if direction.get("owner_issue") != "#271":
        raise CatalogError("product direction owner issue is not #271")
    closure = direction.get("closure")
    if not isinstance(closure, dict) or closure.get("tracking_issue") != "#275":
        raise CatalogError("demand-closure tracking issue is not #275")
    roadmap = direction.get("roadmap")
    if not isinstance(roadmap, dict):
        raise CatalogError("product direction roadmap is missing")
    source_entries = roadmap.get("use_case_families")
    if not isinstance(source_entries, list) or not source_entries:
        raise CatalogError("readiness contains no admitted use-case family declarations")

    use_cases: list[dict[str, Any]] = []
    demanded_by: dict[str, list[str]] = {}
    seen_use_cases: set[str] = set()
    for entry in source_entries:
        if not isinstance(entry, dict):
            raise CatalogError("use-case family declaration is not a mapping")
        required = ("id", "stage", "flagship", "capabilities", "disposition", "tracking_issue")
        if any(field not in entry for field in required):
            raise CatalogError("use-case family declaration is incomplete")
        use_case_id = entry["id"]
        if not isinstance(use_case_id, str) or use_case_id in seen_use_cases:
            raise CatalogError(f"duplicate or invalid use-case ID: {use_case_id!r}")
        seen_use_cases.add(use_case_id)
        capabilities = entry["capabilities"]
        if not isinstance(capabilities, list) or not capabilities or any(
            not isinstance(capability, str) or not capability for capability in capabilities
        ):
            raise CatalogError(f"invalid capability declaration for {use_case_id}")
        if len(capabilities) != len(set(capabilities)):
            raise CatalogError(f"duplicate capability reference for {use_case_id}")
        for capability in capabilities:
            demanded_by.setdefault(capability, []).append(use_case_id)
        use_cases.append(
            {
                "id": use_case_id,
                "stage": entry["stage"],
                "flagship": entry["flagship"],
                "capabilities": sorted(capabilities),
                "disposition": entry["disposition"],
                "tracking_issue": entry["tracking_issue"],
            }
        )

    use_cases.sort(key=lambda entry: entry["id"])
    capabilities = [
        {"id": capability, "demanded_by": sorted(use_case_ids)}
        for capability, use_case_ids in sorted(demanded_by.items())
    ]
    return use_cases, capabilities


def build_report(
    root: pathlib.Path, *, source_bytes: Mapping[str, bytes] | None = None
) -> dict[str, Any]:
    root = root.resolve()
    reject_dirty_source_files(root)
    readiness_path = root / READINESS_PATH
    readiness = load_yaml(readiness_path, source_bytes=source_bytes, root=root)
    readiness_schema = load_yaml(
        root / READINESS_SCHEMA_PATH, source_bytes=source_bytes, root=root
    )
    validate_readiness_document(readiness, readiness_schema)
    if not isinstance(readiness, dict):
        raise CatalogError("readiness document must be a mapping")
    use_cases, capabilities = project_use_cases(copy.deepcopy(readiness))
    report = {
        "schema": "cxxlens.ng-use-case-capability-catalog.v1",
        "document_version": "1.0.0",
        "role": "demand-reference-projection",
        "source": {
            "readiness_schema": "cxxlens.ng-api-development-readiness.v1",
            "readiness_path": READINESS_PATH.as_posix(),
            "contract": "product.demand-closure.v1",
            "owner_issue": "#271",
            "tracking_issue": "#275",
            "source_pointer": "/product_direction/roadmap/use_case_families",
            "revision": git_value(root, "HEAD"),
            "tree": git_value(root, "HEAD^{tree}"),
        "readiness_digest": sha256(
            readiness_path, source_bytes=source_bytes, root=root
        ),
        },
        "projection": {
            "scope": "readiness-declared-use-case-families",
            "status": "declaration-only-not-qualified",
            "excludes": [
                "consumer-identity",
                "dependency-edges",
                "provider-bindings",
                "relation-bindings",
                "evidence-qualification",
            ],
        },
        "use_cases": use_cases,
        "capability_registry": capabilities,
    }
    validate_report(report, root, source_bytes=source_bytes)
    return report


def validate_report(
    report: dict[str, Any],
    root: pathlib.Path = ROOT,
    *,
    source_bytes: Mapping[str, bytes] | None = None,
) -> None:
    root = root.resolve()
    schema = load_yaml(
        root / CATALOG_SCHEMA_PATH, source_bytes=source_bytes, root=root
    )
    if not isinstance(schema, dict):
        raise CatalogError("catalog schema must be a mapping")
    validate(report, schema, "catalog report")

    use_case_ids = [entry["id"] for entry in report["use_cases"]]
    if len(use_case_ids) != len(set(use_case_ids)):
        raise CatalogError("catalog report contains duplicate use-case IDs")

    capability_ids = [entry["id"] for entry in report["capability_registry"]]
    if len(capability_ids) != len(set(capability_ids)):
        raise CatalogError("catalog report contains duplicate capability IDs")

    expected_demanded_by: dict[str, list[str]] = {}
    for use_case in report["use_cases"]:
        for capability in use_case["capabilities"]:
            expected_demanded_by.setdefault(capability, []).append(use_case["id"])

    actual_demanded_by = {
        entry["id"]: entry["demanded_by"]
        for entry in report["capability_registry"]
    }
    expected_capabilities = set(expected_demanded_by)
    actual_capabilities = set(actual_demanded_by)
    if actual_capabilities != expected_capabilities:
        missing = sorted(expected_capabilities - actual_capabilities)
        extra = sorted(actual_capabilities - expected_capabilities)
        raise CatalogError(
            "capability registry IDs do not exactly match use-case capability references "
            f"(missing={missing}, extra={extra})"
        )

    for capability, demanded_by in expected_demanded_by.items():
        expected = sorted(demanded_by)
        actual = actual_demanded_by[capability]
        if actual != expected:
            raise CatalogError(
                f"capability demanded_by mapping is not exact for {capability!r}: "
                f"expected {expected}, got {actual}"
            )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        report = build_report(args.root)
        encoded = json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
        if args.output is None:
            sys.stdout.write(encoded)
        else:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(encoded, encoding="utf-8")
    except (CatalogError, OSError, UnicodeError) as error:
        print(f"use-case capability catalog check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
