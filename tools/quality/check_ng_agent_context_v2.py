#!/usr/bin/env python3
"""Generate exact, safe-stopping agent packets from work-unit authority."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

import jsonschema

import check_ng_work_units as work_units


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCHEMA = pathlib.Path("schemas/cxxlens_ng_agent_context_v2.schema.yaml")
DECISIONS = pathlib.Path("schemas/cxxlens_ng_development_decision_register.yaml")
MANIFEST = work_units.MANIFEST
LEGACY_V1_GENERATOR = pathlib.Path("tools/quality/check_ng_agent_context.py")
LEGACY_V1_USE_CASE = "repository-semantic-query.explain-translation-unit.v1"
LEGACY_V1_ISSUE = "#261"
SYNTHETIC_REVISION = "0" * 40

_DECISION_GATED_RISKS = {
    "contract",
    "invariant",
    "security",
    "compatibility",
    "irreversible",
    "resource-bound",
}


class AgentContextV2Error(ValueError):
    """A fail-closed packet selection or binding violation."""


def _git(root: pathlib.Path, *arguments: str) -> str:
    try:
        result = subprocess.run(["git", "-C", str(root), *arguments], check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as error:
        raise AgentContextV2Error(f"git binding unavailable: {' '.join(arguments)}") from error
    return result.stdout.strip()


def _file_digest(path: pathlib.Path) -> str:
    try:
        return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError as error:
        raise AgentContextV2Error(f"reading-set path missing: {path}") from error


def _validate_packet_paths(packet: dict[str, Any]) -> None:
    path_lists = (
        ("allowed_write_paths", packet.get("allowed_write_paths", [])),
        ("integration_generated_surfaces", packet.get("integration_generated_surfaces", [])),
    )
    for field, values in path_lists:
        for index, value in enumerate(values):
            try:
                work_units.validate_repository_path(value, f"{field}[{index}]")
            except work_units.WorkUnitError as error:
                raise AgentContextV2Error(str(error)) from error
    for index, row in enumerate(packet.get("reading_set", [])):
        if isinstance(row, dict) and "path" in row:
            try:
                work_units.validate_repository_path(row["path"], f"reading_set[{index}].path")
            except work_units.WorkUnitError as error:
                raise AgentContextV2Error(str(error)) from error
    receipts = packet.get("required_product_receipts", {})
    if isinstance(receipts, dict):
        for product, receipt in receipts.items():
            if isinstance(receipt, dict) and receipt.get("status") == "available":
                for field in ("artifact_path", "evidence_path"):
                    try:
                        work_units.validate_repository_path(
                            receipt[field], f"required_product_receipts.{product}.{field}"
                        )
                    except work_units.WorkUnitError as error:
                        raise AgentContextV2Error(str(error)) from error


def _select(manifest: dict[str, Any], issue: str, unit_id: str) -> tuple[dict[str, Any], dict[str, Any]]:
    registered_units, unit_entries = _registered_units(manifest)
    normalized = issue if issue.startswith("#") else f"#{issue}"
    entries = [entry for entry in manifest["entries"] if entry["issue"] == normalized]
    if len(entries) != 1:
        raise AgentContextV2Error(f"unknown or non-unique issue: {normalized}")
    unit = registered_units.get(unit_id)
    if unit is None or unit_entries[unit_id]["issue"] != normalized:
        raise AgentContextV2Error(f"unknown or foreign unit: {normalized}:{unit_id}")
    return entries[0], unit


def _registered_units(
    manifest: dict[str, Any],
) -> tuple[dict[str, dict[str, Any]], dict[str, dict[str, Any]]]:
    """Build the exact unit registry and reject references outside it."""
    units: dict[str, dict[str, Any]] = {}
    unit_entries: dict[str, dict[str, Any]] = {}
    for entry in manifest["entries"]:
        for unit in entry["units"]:
            identifier = unit["id"]
            if identifier in units:
                raise AgentContextV2Error(f"duplicate registered work unit: {identifier}")
            if not identifier.startswith(f"wu-{entry['issue'][1:]}-"):
                raise AgentContextV2Error(f"registered work unit has foreign issue: {identifier}")
            units[identifier] = unit
            unit_entries[identifier] = entry
    if not units:
        raise AgentContextV2Error("no registered work units")
    for identifier, unit in units.items():
        for field in ("depends_on", "serialized_with"):
            for reference in unit[field]:
                if reference not in units:
                    raise AgentContextV2Error(
                        f"unknown work unit reference: {identifier}:{field}:{reference}"
                    )
    return units, unit_entries


def _decision_blockers(
    identifier: str,
    all_units: dict[str, dict[str, Any]],
    unit_entries: dict[str, dict[str, Any]],
    decision_register: dict[str, Any],
) -> list[str]:
    candidate = all_units[identifier]
    owner = unit_entries[identifier]
    if candidate["risk"] not in _DECISION_GATED_RISKS:
        return []
    authority_set = set(owner["authority_sources"])
    relevant = [
        decision
        for decision in decision_register["decisions"]
        if owner["issue"] in decision["owner_issues"]
        and authority_set.intersection(decision["authority_refs"])
    ]
    return [
        f"decision:{decision['id']}:{decision['authority_status']}:{decision['review']['outcome']}"
        for decision in relevant
        if decision["authority_status"] != "accepted"
        or decision["review"]["outcome"] != "accepted"
    ]


def _resolve_execution(
    manifest: dict[str, Any],
    unit: dict[str, Any],
    decision_register: dict[str, Any],
) -> tuple[str, list[str]]:
    all_units, unit_entries = _registered_units(manifest)
    pending = list(unit["depends_on"])
    blocked_dependencies: list[str] = []
    seen: set[str] = set()
    while pending:
        dependency = pending.pop()
        if dependency in seen:
            continue
        seen.add(dependency)
        dependency_unit = all_units[dependency]
        if dependency_unit["state"] != "ready":
            blocked_dependencies.append(f"dependency:{dependency}:{dependency_unit['state']}")
        blocked_dependencies.extend(
            f"dependency:{dependency}:{blocker}"
            for blocker in _decision_blockers(
                dependency,
                all_units,
                unit_entries,
                decision_register,
            )
        )
        pending.extend(dependency_unit["depends_on"])
    authority_blockers = _decision_blockers(
        unit["id"],
        all_units,
        unit_entries,
        decision_register,
    )
    product_blockers = [
        f"product:{product}:{manifest['product_receipts'][product]['status']}"
        for product in unit["consumed_products"]
        if manifest["product_receipts"][product]["status"] != "available"
    ]
    if unit["state"] == "ready" and (blocked_dependencies or authority_blockers or product_blockers):
        return "stop-blocked-by-dependency", sorted(
            [*blocked_dependencies, *authority_blockers, *product_blockers]
        )
    disposition = {
        "ready": "ready",
        "review-required": "stop-review-required",
        "blocked-by-authority": "stop-blocked-by-authority",
    }[unit["state"]]
    blockers = [] if unit["state"] == "ready" else [
        unit["state"],
        *sorted(blocked_dependencies),
        *sorted(authority_blockers),
        *sorted(product_blockers),
    ]
    return disposition, blockers


def _packet(root: pathlib.Path, manifest: dict[str, Any], entry: dict[str, Any], unit: dict[str, Any], *, synthetic: bool) -> dict[str, Any]:
    if synthetic:
        revision = tree = "0" * 40
        worktree = "synthetic-corpus"
    else:
        status = _git(root, "status", "--porcelain=v1", "--untracked-files=all")
        if status:
            raise AgentContextV2Error("dirty worktree cannot produce an execution packet")
        _git(root, "fetch", "--no-tags", "origin", "main")
        revision = _git(root, "rev-parse", "HEAD")
        origin_main = _git(root, "rev-parse", "origin/main")
        if revision != origin_main:
            raise AgentContextV2Error(
                "stale checkout cannot produce an execution packet: HEAD != origin/main"
            )
        tree = _git(root, "rev-parse", "HEAD^{tree}")
        worktree = "clean"
    decision_register = work_units.load(root / DECISIONS)
    disposition, blockers = _resolve_execution(manifest, unit, decision_register)
    reading_paths = sorted(set(entry["authority_sources"] + unit["consumed_paths"] + [str(DECISIONS)]))
    reading_set = [{"path": value, "sha256": _file_digest(root / value)} for value in reading_paths]
    return {
        "schema": "cxxlens.ng-agent-context.v2",
        "document_version": "2.0.0",
        "role": "bounded-non-authoritative-execution-packet",
        "issue": entry["issue"],
        "unit_id": unit["id"],
        "conflict_class": entry["conflict_class"],
        "integration_owner": entry["integration_owner"],
        "risk": unit["risk"],
        "execution_disposition": disposition,
        "blockers": blockers,
        "dependencies": unit["depends_on"],
        "owned_products": unit["owned_products"],
        "consumed_products": unit["consumed_products"],
        "required_product_receipts": {
            product: manifest["product_receipts"][product]
            for product in unit["consumed_products"]
        },
        "authority": {
            "revision": revision,
            "tree": tree,
            "worktree": worktree,
            "manifest_digest": _file_digest(root / work_units.MANIFEST),
            "authority_digest": entry["authority_digest"],
        },
        "reading_set": reading_set,
        "allowed_write_paths": unit["owned_paths"],
        "integration_generated_surfaces": unit["generated_surfaces"],
        "forbidden_shortcuts": unit["forbidden_shortcuts"],
        "completion_class": entry["completion_class"],
        "evidence_commands": unit["evidence_commands"],
        "completion_plan": unit["completion_plan"],
        "residual_qualification": entry["residual_qualification"],
    }


def validate_packet(root: pathlib.Path, packet: dict[str, Any]) -> None:
    schema = work_units.load(root / SCHEMA)
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(packet)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise AgentContextV2Error(f"agent context v2 schema validation failed: {error.message}") from error
    _validate_packet_paths(packet)


def _expected_reading_set(
    root: pathlib.Path,
    entry: dict[str, Any],
    unit: dict[str, Any],
) -> list[dict[str, str]]:
    reading_paths = sorted(
        set(entry["authority_sources"] + unit["consumed_paths"] + [str(DECISIONS)])
    )
    return [{"path": value, "sha256": _file_digest(root / value)} for value in reading_paths]


def _require_nonempty_actionable(packet: dict[str, Any]) -> None:
    for field in ("forbidden_shortcuts", "evidence_commands", "completion_plan"):
        values = packet[field]
        if not values or any(not isinstance(value, str) or not value.strip() for value in values):
            raise AgentContextV2Error(f"packet actionable field is empty: {field}")
    for field in ("residual_qualification",):
        value = packet[field]
        if not isinstance(value, str) or not value.strip():
            raise AgentContextV2Error(f"packet actionable field is empty: {field}")
    if not packet["reading_set"]:
        raise AgentContextV2Error("packet actionable field is empty: reading_set")
    if not packet["allowed_write_paths"]:
        raise AgentContextV2Error("packet actionable field is empty: allowed_write_paths")
    if packet["execution_disposition"] != "ready" and not packet["blockers"]:
        raise AgentContextV2Error("packet actionable field is empty: blockers")
    if any(not isinstance(value, str) or not value.strip() for value in packet["blockers"]):
        raise AgentContextV2Error("packet actionable field is empty: blockers")


def _validate_packet_semantics(
    root: pathlib.Path,
    manifest: dict[str, Any],
    entry: dict[str, Any],
    unit: dict[str, Any],
    packet: dict[str, Any],
    *,
    synthetic: bool,
) -> None:
    """Validate packet meaning against the registered authority projection."""
    registered_units, unit_entries = _registered_units(manifest)
    selected_entry, selected_unit = _select(manifest, entry["issue"], unit["id"])
    if selected_entry is not entry or selected_unit is not unit:
        raise AgentContextV2Error("packet selection is not the registered issue/unit object")
    if packet["issue"] != entry["issue"] or packet["unit_id"] != unit["id"]:
        raise AgentContextV2Error("packet issue/unit identity mismatch")
    if packet["unit_id"] not in registered_units:
        raise AgentContextV2Error(f"packet references unknown work unit: {packet['unit_id']}")
    if unit_entries[packet["unit_id"]]["issue"] != packet["issue"]:
        raise AgentContextV2Error("packet issue/unit registration mismatch")
    for dependency in packet["dependencies"]:
        if dependency not in registered_units:
            raise AgentContextV2Error(f"packet references unknown dependency: {dependency}")
    if packet["dependencies"] != unit["depends_on"]:
        raise AgentContextV2Error("packet dependency references drift")

    exact_fields = (
        "conflict_class",
        "integration_owner",
        "risk",
        "owned_products",
        "consumed_products",
        "forbidden_shortcuts",
        "completion_class",
        "evidence_commands",
        "completion_plan",
        "residual_qualification",
    )
    expected_fields = {
        "conflict_class": entry["conflict_class"],
        "integration_owner": entry["integration_owner"],
        "risk": unit["risk"],
        "owned_products": unit["owned_products"],
        "consumed_products": unit["consumed_products"],
        "forbidden_shortcuts": unit["forbidden_shortcuts"],
        "completion_class": entry["completion_class"],
        "evidence_commands": unit["evidence_commands"],
        "completion_plan": unit["completion_plan"],
        "residual_qualification": entry["residual_qualification"],
    }
    for field in exact_fields:
        if packet[field] != expected_fields[field]:
            raise AgentContextV2Error(f"packet authority projection drift: {field}")

    expected_product_receipts = {
        product: manifest["product_receipts"][product]
        for product in unit["consumed_products"]
    }
    if packet["required_product_receipts"] != expected_product_receipts:
        raise AgentContextV2Error("packet product receipt projection drift")

    expected_manifest_digest = _file_digest(root / MANIFEST)
    if packet["authority"]["manifest_digest"] != expected_manifest_digest:
        raise AgentContextV2Error("packet authority manifest digest mismatch")
    expected_authority_digest = work_units.canonical_digest(root, entry["authority_sources"])
    if expected_authority_digest != entry["authority_digest"]:
        raise AgentContextV2Error(f"registered authority digest drift: {entry['issue']}")
    if packet["authority"]["authority_digest"] != expected_authority_digest:
        raise AgentContextV2Error("packet authority digest mismatch")
    if packet["reading_set"] != _expected_reading_set(root, entry, unit):
        raise AgentContextV2Error("packet reading-set authority projection drift")

    expected_revision = SYNTHETIC_REVISION if synthetic else _git(root, "rev-parse", "HEAD")
    expected_tree = SYNTHETIC_REVISION if synthetic else _git(root, "rev-parse", "HEAD^{tree}")
    expected_worktree = "synthetic-corpus" if synthetic else "clean"
    if packet["authority"]["revision"] != expected_revision or packet["authority"]["tree"] != expected_tree:
        raise AgentContextV2Error("packet revision/tree authority binding mismatch")
    if packet["authority"]["worktree"] != expected_worktree:
        raise AgentContextV2Error("packet worktree authority binding mismatch")

    decision_register = work_units.load(root / DECISIONS)
    expected_disposition, expected_blockers = _resolve_execution(
        manifest, unit, decision_register
    )
    if packet["execution_disposition"] != expected_disposition or packet["blockers"] != expected_blockers:
        raise AgentContextV2Error("packet execution disposition/dependency binding drift")
    _require_nonempty_actionable(packet)


def _is_bounded_completion(
    root: pathlib.Path,
    manifest: dict[str, Any],
    entry: dict[str, Any],
    unit: dict[str, Any],
    packet: dict[str, Any],
    *,
    synthetic: bool,
) -> bool:
    """Select only packets whose meaning and actionable surface are complete."""
    _validate_packet_semantics(
        root,
        manifest,
        entry,
        unit,
        packet,
        synthetic=synthetic,
    )
    return bool(
        packet["issue"] == entry["issue"]
        and packet["unit_id"] == unit["id"]
        and packet["dependencies"] == unit["depends_on"]
        and packet["reading_set"]
        and packet["allowed_write_paths"]
        and packet["evidence_commands"]
        and packet["completion_plan"]
        and packet["residual_qualification"].strip()
    )


def _run_v1_command(command: list[str]) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(command, check=False, capture_output=True, text=True)
    except OSError as error:
        raise AgentContextV2Error("v1 compatibility command could not execute") from error


def _v1_failure(label: str, result: subprocess.CompletedProcess[str]) -> AgentContextV2Error:
    detail = (result.stderr or result.stdout or "").strip().splitlines()
    suffix = detail[-1] if detail else f"exit {result.returncode}"
    return AgentContextV2Error(f"v1 compatibility {label} failed: {suffix}")


def _run_v1_compatibility(root: pathlib.Path) -> dict[str, Any]:
    """Run the exact legacy v1 CLI plan/check without writing into the repository."""
    with tempfile.TemporaryDirectory(prefix="cxxlens-ng-agent-context-v1-") as temporary:
        output_root = pathlib.Path(temporary)
        authority_root = output_root / "repository"
        clone = _run_v1_command(
            ["git", "clone", "--no-local", str(root), str(authority_root)]
        )
        if clone.returncode != 0:
            raise _v1_failure("temporary authority clone", clone)
        revision = _git(authority_root, "rev-parse", "HEAD")
        tree = _git(authority_root, "rev-parse", "HEAD^{tree}")
        generator = authority_root / LEGACY_V1_GENERATOR
        packet_path = output_root / "packet.json"
        markdown_path = output_root / "packet.md"
        common = [
            "--root",
            str(authority_root),
            "--use-case",
            LEGACY_V1_USE_CASE,
            "--issue",
            LEGACY_V1_ISSUE.removeprefix("#"),
            "--expected-revision",
            revision,
            "--expected-tree",
            tree,
        ]
        plan = _run_v1_command(
            [
                sys.executable,
                str(generator),
                "plan",
                *common,
                "--output-json",
                str(packet_path),
                "--output-markdown",
                str(markdown_path),
            ]
        )
        if plan.returncode != 0:
            raise _v1_failure("plan", plan)
        try:
            packet = json.loads(packet_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise AgentContextV2Error("v1 compatibility plan did not produce a valid packet") from error
        if not isinstance(packet, dict) or packet.get("schema") != "cxxlens.ng-agent-context.v1":
            raise AgentContextV2Error("v1 compatibility plan produced a non-v1 packet")
        if packet.get("issue") != LEGACY_V1_ISSUE:
            raise AgentContextV2Error("v1 compatibility plan produced the wrong issue packet")
        check = _run_v1_command(
            [
                sys.executable,
                str(generator),
                "check",
                *common,
                "--input-json",
                str(packet_path),
            ]
        )
        if check.returncode != 0:
            raise _v1_failure("check", check)
        return {
            "status": "verified",
            "method": "exact-v1-plan-and-check",
            "issue": LEGACY_V1_ISSUE,
            "use_case": LEGACY_V1_USE_CASE,
            "plan_exit_code": plan.returncode,
            "check_exit_code": check.returncode,
            "packet_schema": packet["schema"],
            "packet_canonical_digest": packet.get("canonical_digest"),
            "authority_scope": "temporary-clean-head-clone",
            "authority_revision": revision,
            "authority_tree": tree,
            "output_scope": "temporary-directory",
        }


def build(root: pathlib.Path, issue: str, unit_id: str, *, synthetic: bool = False) -> dict[str, Any]:
    manifest = work_units.validate(root)
    entry, unit = _select(manifest, issue, unit_id)
    packet = _packet(root, manifest, entry, unit, synthetic=synthetic)
    validate_packet(root, packet)
    _validate_packet_semantics(
        root,
        manifest,
        entry,
        unit,
        packet,
        synthetic=synthetic,
    )
    return packet


def corpus(root: pathlib.Path) -> dict[str, Any]:
    manifest = work_units.validate(root)
    registered_units, unit_entries = _registered_units(manifest)
    packet_rows: list[tuple[dict[str, Any], dict[str, Any], dict[str, Any]]] = []
    for entry in manifest["entries"]:
        for unit in entry["units"]:
            packet = build(root, entry["issue"], unit["id"], synthetic=True)
            packet_rows.append((entry, unit, packet))
    if {unit["id"] for _, unit, _ in packet_rows} != set(registered_units):
        raise AgentContextV2Error("corpus contains an unknown or omitted work unit")
    if any(unit_entries[unit["id"]] is not entry for entry, unit, _ in packet_rows):
        raise AgentContextV2Error("corpus issue/unit registration is not exact")
    packets = [packet for _, _, packet in packet_rows]
    stopped = [packet for packet in packets if packet["execution_disposition"] != "ready"]
    safe_stops = [packet for packet in stopped if packet["blockers"] and packet["completion_plan"]]
    complete = [
        packet
        for entry, unit, packet in packet_rows
        if _is_bounded_completion(
            root,
            manifest,
            entry,
            unit,
            packet,
            synthetic=True,
        )
    ]
    stop_rate = 100 if not stopped else 100 * len(safe_stops) // len(stopped)
    completion_rate = 100 * len(complete) // len(packets)
    if stop_rate != 100:
        raise AgentContextV2Error(f"safe-stop rate below 100: {stop_rate}")
    if completion_rate < 80:
        raise AgentContextV2Error(f"bounded packet completion rate below 80: {completion_rate}")
    for invalid_issue, invalid_unit in (("#999", "wu-999-missing"), ("#173", "wu-261-source-closure-authority")):
        try:
            _select(manifest, invalid_issue, invalid_unit)
        except AgentContextV2Error:
            continue
        raise AgentContextV2Error("invalid corpus selection did not stop")
    v1_compatibility = _run_v1_compatibility(root)
    return {
        "schema": "cxxlens.ng-agent-context-corpus.v1",
        "packets": len(packets),
        "safe_stop_rate_percent": stop_rate,
        "bounded_packet_completion_rate_percent": completion_rate,
        "v1_issue_261_compatibility": v1_compatibility["status"],
        "v1_issue_261_compatibility_evidence": v1_compatibility,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("packet", "corpus"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--issue")
    parser.add_argument("--unit")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        if args.command == "corpus":
            result = corpus(root)
        else:
            if not args.issue or not args.unit:
                raise AgentContextV2Error("packet requires --issue and --unit")
            result = build(root, args.issue, args.unit)
        print(json.dumps(result, ensure_ascii=False, sort_keys=True, indent=2))
    except (AgentContextV2Error, work_units.WorkUnitError) as error:
        print(f"agent-context-v2: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
