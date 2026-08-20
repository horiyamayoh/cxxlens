#!/usr/bin/env python3
"""Validate and digest the exactly-once autonomous work-unit inventory."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = pathlib.Path("schemas/cxxlens_ng_work_units.yaml")
SCHEMA = pathlib.Path("schemas/cxxlens_ng_work_units.schema.yaml")
EXPECTED_ISSUES = {"#173", "#183", "#185", "#200", "#201", "#202", "#205", "#261", "#277"}
REQUIRED_SQLITE_PRODUCTS = {
    "sqlite.active-read-connection",
    "sqlite.nested-mapping-terminal",
    "sqlite.logical-read-receipt",
}


class WorkUnitError(ValueError):
    """A fail-closed work-unit inventory violation."""


def load(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, yaml.YAMLError) as error:
        raise WorkUnitError(f"cannot load {path}: {error}") from error
    if not isinstance(value, dict):
        raise WorkUnitError(f"expected mapping: {path}")
    return value


def canonical_digest(root: pathlib.Path, paths: list[str]) -> str:
    projection: list[dict[str, str]] = []
    for relative in sorted(paths):
        path = root / relative
        if not path.is_file():
            raise WorkUnitError(f"authority source is missing: {relative}")
        projection.append({"path": relative, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    encoded = json.dumps(projection, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def path_conflicts(left: str, right: str) -> bool:
    a = tuple(left.rstrip("/").split("/"))
    b = tuple(right.rstrip("/").split("/"))
    return a == b or a[: len(b)] == b or b[: len(a)] == a


def _acyclic(units: dict[str, dict[str, Any]]) -> None:
    visiting: set[str] = set()
    complete: set[str] = set()

    def visit(identifier: str) -> None:
        if identifier in complete:
            return
        if identifier in visiting:
            raise WorkUnitError(f"dependency cycle: {identifier}")
        visiting.add(identifier)
        for dependency in units[identifier]["depends_on"]:
            visit(dependency)
        visiting.remove(identifier)
        complete.add(identifier)

    for identifier in units:
        visit(identifier)


def _transitive_dependencies(identifier: str, units: dict[str, dict[str, Any]]) -> set[str]:
    result: set[str] = set()
    pending = list(units[identifier]["depends_on"])
    while pending:
        current = pending.pop()
        if current in result:
            continue
        result.add(current)
        pending.extend(units[current]["depends_on"])
    return result


def validate(root: pathlib.Path, *, allow_placeholder: bool = False) -> dict[str, Any]:
    manifest = load(root / MANIFEST)
    schema = load(root / SCHEMA)
    if allow_placeholder:
        manifest = json.loads(json.dumps(manifest).replace("sha256:PLACEHOLDER", "sha256:" + "0" * 64))
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(manifest)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise WorkUnitError(f"work-unit schema validation failed: {error.message}") from error

    entries = manifest["entries"]
    issues = [entry["issue"] for entry in entries]
    if len(issues) != len(set(issues)) or set(issues) != EXPECTED_ISSUES:
        raise WorkUnitError("open issues must be registered exactly once")
    if set(manifest["open_issue_inventory"]) != EXPECTED_ISSUES:
        raise WorkUnitError("open issue inventory drift")
    if set(issues) & set(manifest["closed_contract_owners"]):
        raise WorkUnitError("closed contract owner owns an active work unit")

    units: dict[str, dict[str, Any]] = {}
    unit_issue: dict[str, str] = {}
    product_owners: dict[str, str] = {}
    for entry in entries:
        actual_digest = canonical_digest(root, entry["authority_sources"])
        if not allow_placeholder and actual_digest != entry["authority_digest"]:
            raise WorkUnitError(f"authority digest drift: {entry['issue']}")
        for unit in entry["units"]:
            identifier = unit["id"]
            if identifier in units:
                raise WorkUnitError(f"duplicate unit ID: {identifier}")
            if not identifier.startswith(f"wu-{entry['issue'][1:]}-"):
                raise WorkUnitError(f"unit/issue mismatch: {identifier}")
            units[identifier] = unit
            unit_issue[identifier] = entry["issue"]
            for product in unit["owned_products"]:
                if product in product_owners:
                    raise WorkUnitError(f"duplicate product owner: {product}")
                product_owners[product] = identifier
            for generated in unit["generated_surfaces"]:
                if any(path_conflicts(generated, owned) for owned in unit["owned_paths"]):
                    raise WorkUnitError(f"generated surface must remain integration-owned: {identifier}:{generated}")

    for identifier, unit in units.items():
        for dependency in unit["depends_on"]:
            if dependency not in units:
                raise WorkUnitError(f"unknown dependency: {identifier}:{dependency}")
        for peer in unit["serialized_with"]:
            if peer not in units:
                raise WorkUnitError(f"unknown serialization peer: {identifier}:{peer}")
            if identifier not in units[peer]["serialized_with"]:
                raise WorkUnitError(f"asymmetric serialization: {identifier}:{peer}")
    _acyclic(units)
    if not REQUIRED_SQLITE_PRODUCTS.issubset(product_owners):
        raise WorkUnitError("required SQLite product owner is missing")

    identifiers = sorted(units)
    for index, left_id in enumerate(identifiers):
        left = units[left_id]
        for right_id in identifiers[index + 1 :]:
            right = units[right_id]
            overlaps = any(path_conflicts(a, b) for a in left["owned_paths"] for b in right["owned_paths"])
            if overlaps and right_id not in left["serialized_with"]:
                raise WorkUnitError(f"undeclared owned-path overlap: {left_id}:{right_id}")
    for consumer_id, consumer in units.items():
        dependencies = _transitive_dependencies(consumer_id, units)
        for product in consumer["consumed_products"]:
            producer_id = product_owners.get(product)
            if producer_id is None:
                raise WorkUnitError(f"consumed product has no owner: {consumer_id}:{product}")
            if producer_id not in dependencies:
                raise WorkUnitError(f"consumed product lacks dependency: {consumer_id}:{producer_id}")
        for producer_id, producer in units.items():
            if consumer_id == producer_id:
                continue
            overlap = any(path_conflicts(consumed, owned) for consumed in consumer["consumed_paths"] for owned in producer["owned_paths"])
            if overlap and producer_id not in dependencies and producer_id not in consumer["serialized_with"]:
                raise WorkUnitError(f"consumed path lacks dependency or serialization: {consumer_id}:{producer_id}")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "digests"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        manifest = validate(root, allow_placeholder=args.command == "digests")
        if args.command == "digests":
            for entry in manifest["entries"]:
                print(f"{entry['issue']} {canonical_digest(root, entry['authority_sources'])}")
        else:
            print("work-unit-inventory: ok")
    except WorkUnitError as error:
        print(f"work-unit-inventory: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
