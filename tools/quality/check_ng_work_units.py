#!/usr/bin/env python3
"""Validate and digest the exactly-once autonomous work-unit inventory."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
from typing import Any

import jsonschema
import yaml

from check_ng_development_decisions import GOVERNANCE_ENFORCEMENT_SURFACES


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = pathlib.Path("schemas/cxxlens_ng_work_units.yaml")
SCHEMA = pathlib.Path("schemas/cxxlens_ng_work_units.schema.yaml")
DECISION_REGISTER = pathlib.Path("schemas/cxxlens_ng_development_decision_register.yaml")
REVIEW_RECEIPTS = pathlib.Path("schemas/cxxlens_ng_development_review_receipts.yaml")
# The manifest is the projection being digested, so including it in its own
# authority source set would create a self-referential digest.  The decision
# register still covers the manifest itself; this set covers the remaining
# governance enforcement surfaces owned by wu-173.
GOVERNANCE_WORK_UNIT_AUTHORITY_SURFACES = frozenset(
    path for path in GOVERNANCE_ENFORCEMENT_SURFACES if path != str(MANIFEST)
)
EXPECTED_ISSUES = {"#173", "#183", "#200", "#201", "#202", "#205", "#261", "#277"}
COMPLETED_ISSUES = {"#185"}
REQUIRED_SQLITE_PRODUCTS = {
    "sqlite.active-read-connection",
    "sqlite.nested-mapping-terminal",
    "sqlite.logical-read-receipt",
}
NON_NORMATIVE_AUTHORITY_COMPONENTS = frozenset(
    {"implementation-learning", "archive", "archives"}
)
EVIDENCE_ONLY_AUTHORITY_COMPONENTS = frozenset(
    {"artifacts", "evidence", "evidence-only", "reports", "work-unit-evidence"}
)


class WorkUnitError(ValueError):
    """A fail-closed work-unit inventory violation."""


def validate_repository_path(value: Any, field: str) -> None:
    """Reject paths that are not canonical repository-relative coordinates."""
    if (
        not isinstance(value, str)
        or not value
        or value.startswith(("/", "\\"))
        or "\\" in value
        or "\x00" in value
        or any(part in {"", ".", "..", ".git"} for part in value.split("/"))
    ):
        raise WorkUnitError(f"unsafe repository-relative path: {field}:{value!r}")


def validate_repository_paths(values: Any, field: str) -> None:
    for index, value in enumerate(values):
        validate_repository_path(value, f"{field}[{index}]")


def validate_authority_sources(values: Any, field: str) -> None:
    validate_repository_paths(values, field)
    for index, value in enumerate(values):
        components = set(pathlib.PurePosixPath(value).parts)
        if components.intersection(NON_NORMATIVE_AUTHORITY_COMPONENTS):
            raise WorkUnitError(
                f"forbidden authority source path (non-normative or archived): "
                f"{field}[{index}]:{value}"
            )
        if components.intersection(EVIDENCE_ONLY_AUTHORITY_COMPONENTS):
            raise WorkUnitError(
                f"forbidden authority source path (evidence-only): "
                f"{field}[{index}]:{value}"
            )


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
        validate_repository_path(relative, "authority_sources")
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


def _git_bytes(root: pathlib.Path, *args: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(root), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise WorkUnitError(f"product receipt git object unavailable: {detail}")
    return result.stdout


def _authenticate_available_receipt(
    root: pathlib.Path,
    product: str,
    receipt: dict[str, Any],
    producer_id: str,
    producer: dict[str, Any],
) -> None:
    if receipt["producer_unit"] != producer_id:
        raise WorkUnitError(f"product receipt producer mismatch: {product}")
    commit = receipt["producer_commit"]
    if _git_bytes(root, "cat-file", "-t", commit) != b"commit\n":
        raise WorkUnitError(f"product receipt producer is not a commit: {product}")
    tree = _git_bytes(root, "rev-parse", f"{commit}^{{tree}}").decode("ascii").strip()
    if tree != receipt["producer_tree"]:
        raise WorkUnitError(f"product receipt tree mismatch: {product}")
    surfaces = producer.get("product_receipt_surfaces", {}).get(product)
    if surfaces is None or surfaces != {
        "artifact_path": receipt["artifact_path"],
        "evidence_path": receipt["evidence_path"],
    }:
        raise WorkUnitError(f"product receipt surface mismatch: {product}")
    if not all(path in producer["owned_paths"] for path in surfaces.values()):
        raise WorkUnitError(f"product receipt surface is not producer-owned: {product}")
    for role in ("artifact", "evidence"):
        path = receipt[f"{role}_path"]
        payload = _git_bytes(root, "show", f"{commit}:{path}")
        digest = "sha256:" + hashlib.sha256(payload).hexdigest()
        if digest != receipt[f"{role}_digest"]:
            raise WorkUnitError(f"product receipt {role} digest mismatch: {product}")


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
    completed = manifest["completed_issue_inventory"]
    completed_issues = [entry["issue"] for entry in completed]
    if len(completed_issues) != len(set(completed_issues)) or set(completed_issues) != COMPLETED_ISSUES:
        raise WorkUnitError("completed issue inventory drift")
    if set(completed_issues) & set(manifest["open_issue_inventory"]):
        raise WorkUnitError("issue is both open and completed")
    for completion in completed:
        for evidence in completion["evidence"]:
            if (
                evidence["commit"] != completion["completion_commit"]
                or evidence["tree"] != completion["completion_tree"]
            ):
                raise WorkUnitError(
                    f"completed issue evidence commit mismatch: {completion['issue']}"
                )
    if set(issues) & set(manifest["closed_contract_owners"]):
        raise WorkUnitError("closed contract owner owns an active work unit")
    known_integration_owners = EXPECTED_ISSUES | set(manifest["closed_contract_owners"])
    for entry in entries:
        if entry["integration_owner"] not in known_integration_owners:
            raise WorkUnitError(
                f"unknown integration owner: {entry['issue']}:{entry['integration_owner']}"
            )

    for entry in entries:
        validate_authority_sources(entry["authority_sources"], f"{entry['issue']}.authority_sources")
        for unit in entry["units"]:
            for field in ("owned_paths", "consumed_paths", "generated_surfaces"):
                validate_repository_paths(unit[field], f"{unit['id']}.{field}")
            for product, surfaces in unit.get("product_receipt_surfaces", {}).items():
                for field in ("artifact_path", "evidence_path"):
                    validate_repository_path(
                        surfaces[field],
                        f"{unit['id']}.product_receipt_surfaces.{product}.{field}",
                    )
    governance_entries = [entry for entry in entries if entry["issue"] == "#173"]
    if len(governance_entries) != 1:
        raise WorkUnitError("governance work-unit entry is missing or non-unique")
    missing_governance_surfaces = GOVERNANCE_WORK_UNIT_AUTHORITY_SURFACES - set(
        governance_entries[0]["authority_sources"]
    )
    if missing_governance_surfaces:
        raise WorkUnitError(
            "governance work-unit authority closure missing enforcement surface: "
            + ",".join(sorted(missing_governance_surfaces))
        )
    for product, receipt in manifest["product_receipts"].items():
        if receipt["status"] == "available":
            for field in ("artifact_path", "evidence_path"):
                validate_repository_path(receipt[field], f"product_receipts.{product}.{field}")

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
            if set(unit.get("product_receipt_surfaces", {})) != set(unit["owned_products"]):
                raise WorkUnitError(f"product receipt surface census mismatch: {identifier}")
            for generated in unit["generated_surfaces"]:
                if any(path_conflicts(generated, owned) for owned in unit["owned_paths"]):
                    raise WorkUnitError(f"generated surface must remain integration-owned: {identifier}:{generated}")

    accepted_decisions: dict[str, dict[str, Any]] | None = None
    accepted_receipts: dict[str, dict[str, Any]] | None = None
    for identifier, unit in units.items():
        completion = unit["dependency_completion"]
        if completion["status"] == "accepted":
            if accepted_decisions is None or accepted_receipts is None:
                accepted_decisions = {
                    entry["id"]: entry
                    for entry in load(root / DECISION_REGISTER).get("decisions", [])
                }
                accepted_receipts = {
                    entry["id"]: entry
                    for entry in load(root / REVIEW_RECEIPTS).get("receipts", [])
                }
            decision = accepted_decisions.get(completion["decision_id"])
            receipt = accepted_receipts.get(completion["receipt_id"])
            if (
                decision is None
                or decision.get("authority_status") != "accepted"
                or decision.get("review", {}).get("outcome") != "accepted"
            ):
                raise WorkUnitError(
                    f"dependency completion decision is not accepted: {identifier}"
                )
            if unit_issue[identifier] not in decision.get("owner_issues", []):
                raise WorkUnitError(
                    f"dependency completion owner mismatch: {identifier}"
                )
            if (
                receipt is None
                or receipt.get("decision_id") != completion["decision_id"]
                or receipt.get("owner_issue") != unit_issue[identifier]
                or completion["receipt_id"] not in decision.get("review", {}).get("receipt_ids", [])
                or receipt.get("verdict") != "accepted"
                or receipt.get("acceptance", {}).get("status") != "committed"
                or receipt.get("connected_verification", {}).get("status") != "verified"
                or receipt.get("connected_verification", {}).get("conclusion") != "success"
            ):
                raise WorkUnitError(
                    f"dependency completion receipt is not accepted: {identifier}"
                )
        if completion["status"] == "accepted" and unit["state"] != "ready":
            raise WorkUnitError(f"accepted dependency gate is not ready: {identifier}")
        if unit["readiness"] == "executable" and unit["state"] != "ready":
            raise WorkUnitError(f"non-ready unit is executable: {identifier}")
        for dependency in unit["depends_on"]:
            if dependency not in units:
                raise WorkUnitError(f"unknown dependency: {identifier}:{dependency}")
        for peer in unit["serialized_with"]:
            if peer not in units:
                raise WorkUnitError(f"unknown serialization peer: {identifier}:{peer}")
            if identifier not in units[peer]["serialized_with"]:
                raise WorkUnitError(f"asymmetric serialization: {identifier}:{peer}")
    _acyclic(units)
    for identifier, unit in units.items():
        if unit["readiness"] != "executable":
            continue
        for dependency in unit["depends_on"]:
            dependency_unit = units[dependency]
            if dependency_unit["dependency_completion"]["status"] != "accepted":
                raise WorkUnitError(
                    f"executable unit has incomplete dependency: {identifier}:{dependency}"
                )
    if not REQUIRED_SQLITE_PRODUCTS.issubset(product_owners):
        raise WorkUnitError("required SQLite product owner is missing")
    if set(manifest["product_receipts"]) != set(product_owners):
        raise WorkUnitError("product receipt inventory differs from product owners")
    for product, receipt in manifest["product_receipts"].items():
        expected_contract = "cxxlens." + product.replace(".", "-") + ".v1"
        if (receipt["contract"] != expected_contract or
                receipt["receipt_profile"] != "exact-producer-commit-tree-artifact-and-evidence-digests"):
            raise WorkUnitError(f"product receipt contract drift: {product}")
        if receipt["status"] == "available":
            producer_id = product_owners[product]
            _authenticate_available_receipt(root, product, receipt, producer_id, units[producer_id])

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
