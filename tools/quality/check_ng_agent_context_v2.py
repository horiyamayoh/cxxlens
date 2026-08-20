#!/usr/bin/env python3
"""Generate exact, safe-stopping agent packets from work-unit authority."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
from typing import Any

import jsonschema

import check_ng_work_units as work_units


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCHEMA = pathlib.Path("schemas/cxxlens_ng_agent_context_v2.schema.yaml")
DECISIONS = pathlib.Path("schemas/cxxlens_ng_development_decision_register.yaml")


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


def _select(manifest: dict[str, Any], issue: str, unit_id: str) -> tuple[dict[str, Any], dict[str, Any]]:
    normalized = issue if issue.startswith("#") else f"#{issue}"
    entries = [entry for entry in manifest["entries"] if entry["issue"] == normalized]
    if len(entries) != 1:
        raise AgentContextV2Error(f"unknown or non-unique issue: {normalized}")
    units = [unit for unit in entries[0]["units"] if unit["id"] == unit_id]
    if len(units) != 1:
        raise AgentContextV2Error(f"unknown or foreign unit: {normalized}:{unit_id}")
    return entries[0], units[0]


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
    all_units = {candidate["id"]: candidate for owner in manifest["entries"] for candidate in owner["units"]}
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
        pending.extend(dependency_unit["depends_on"])
    state = unit["state"]
    decision_register = work_units.load(root / DECISIONS)
    authority_set = set(entry["authority_sources"])
    relevant_decisions = [decision for decision in decision_register["decisions"] if entry["issue"] in decision["owner_issues"] and authority_set.intersection(decision["authority_refs"])]
    authority_blockers = [f"decision:{decision['id']}:{decision['authority_status']}:{decision['review']['outcome']}" for decision in relevant_decisions if unit["risk"] in {"contract", "invariant", "security", "compatibility", "irreversible", "resource-bound"} and (decision["authority_status"] != "accepted" or decision["review"]["outcome"] != "accepted")]
    if state == "ready" and (blocked_dependencies or authority_blockers):
        disposition = "stop-blocked-by-dependency"
        blockers = sorted([*blocked_dependencies, *authority_blockers])
    else:
        disposition = {"ready": "ready", "review-required": "stop-review-required", "blocked-by-authority": "stop-blocked-by-authority"}[state]
        blockers = [] if state == "ready" else [
            state,
            *sorted(blocked_dependencies),
            *sorted(authority_blockers),
        ]
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


def build(root: pathlib.Path, issue: str, unit_id: str, *, synthetic: bool = False) -> dict[str, Any]:
    manifest = work_units.validate(root)
    entry, unit = _select(manifest, issue, unit_id)
    packet = _packet(root, manifest, entry, unit, synthetic=synthetic)
    validate_packet(root, packet)
    return packet


def corpus(root: pathlib.Path) -> dict[str, Any]:
    manifest = work_units.validate(root)
    packets: list[dict[str, Any]] = []
    for entry in manifest["entries"]:
        for unit in entry["units"]:
            packets.append(build(root, entry["issue"], unit["id"], synthetic=True))
    stopped = [packet for packet in packets if packet["execution_disposition"] != "ready"]
    safe_stops = [packet for packet in stopped if packet["blockers"] and packet["completion_plan"]]
    complete = [packet for packet in packets if packet["reading_set"] and packet["allowed_write_paths"] and packet["evidence_commands"]]
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
    return {"schema": "cxxlens.ng-agent-context-corpus.v1", "packets": len(packets), "safe_stop_rate_percent": stop_rate, "bounded_packet_completion_rate_percent": completion_rate, "v1_issue_261_compatibility": "preserved"}


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
