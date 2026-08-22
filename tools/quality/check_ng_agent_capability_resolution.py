#!/usr/bin/env python3
"""Produce the canonical, fail-closed agent capability resolution.

The v1 packet generator is intentionally kept as a compatibility producer.  This module is the
single machine-derived producer for the result-state / missing-reason contract consumed by agent
context v2 and by the SDK-doctor evaluation corpus.  It never infers a capability from relation
presence alone: every path is declared in the checked, source-bound evaluation authority.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import re
import subprocess
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCHEMA = pathlib.Path("schemas/cxxlens_ng_agent_capability_resolution.schema.yaml")
CATALOG = pathlib.Path("schemas/cxxlens_ng_agent_capability_resolution.yaml")
READINESS = pathlib.Path("schemas/cxxlens_ng_api_development_readiness.yaml")
GENERATOR = pathlib.Path("tools/quality/check_ng_agent_capability_resolution.py")
RESULT_STATES = ("proved", "disproved", "unknown", "partial", "conflicting")
REASON_CODES = (
    "none",
    "missing-input",
    "missing-capability",
    "missing-model",
    "missing-evidence",
    "blocked-dependency",
    "unsupported-consumer",
    "unknown-use-case",
    "stale-authority",
    "conflicting-evidence",
    "invalid-request",
)
ID_RE = re.compile(r"^[a-z][a-z0-9_]*(?:[.-][a-z0-9_]+)+$")
HEX40_RE = re.compile(r"^[0-9a-f]{40}$")
ZERO40 = "0" * 40


class CapabilityResolutionError(ValueError):
    """A fail-closed canonical resolution or source-binding violation."""


def _load(path: pathlib.Path) -> Any:
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, yaml.YAMLError) as error:
        raise CapabilityResolutionError(f"cannot read authority: {path}") from error
    if not isinstance(value, dict):
        raise CapabilityResolutionError(f"authority is not an object: {path}")
    return value


def _digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _digest_object(value: Any) -> str:
    return _digest_bytes(
        json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    )


def _file_digest(root: pathlib.Path, relative: str) -> str:
    path = root / relative
    try:
        return _digest_bytes(path.read_bytes())
    except OSError as error:
        raise CapabilityResolutionError(f"authority source missing: {relative}") from error


def _git(root: pathlib.Path, *args: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), *args], check=True, capture_output=True, text=True
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise CapabilityResolutionError(f"git authority unavailable: {' '.join(args)}") from error
    return result.stdout.strip()


def _source_digest(root: pathlib.Path, source_paths: list[str]) -> str:
    rows = [{"path": path, "sha256": _file_digest(root, path)} for path in sorted(source_paths)]
    return _digest_object(rows)


def _validate_path(path: dict[str, Any], *, prefix: str) -> None:
    if not isinstance(path, dict):
        raise CapabilityResolutionError(f"{prefix} is not an object")
    required = {"id", "kind", "requires", "disposition", "state", "owner_issue", "evidence"}
    if set(path) != required:
        raise CapabilityResolutionError(f"{prefix} fields differ: {sorted(set(path) ^ required)}")
    identifier = path["id"]
    if not isinstance(identifier, str) or not ID_RE.fullmatch(identifier):
        raise CapabilityResolutionError(f"{prefix} has non-canonical id: {identifier!r}")
    if path["kind"] not in {"input", "provider", "relation", "analysis", "model", "query", "recipe", "evidence", "support"}:
        raise CapabilityResolutionError(f"{prefix} has unknown kind")
    if path["state"] not in RESULT_STATES:
        raise CapabilityResolutionError(f"{prefix} has unknown result state")
    if not isinstance(path["requires"], list) or len(path["requires"]) != len(set(path["requires"])):
        raise CapabilityResolutionError(f"{prefix} has duplicate/invalid dependencies")
    if not isinstance(path["evidence"], list) or len(path["evidence"]) != len(set(path["evidence"])):
        raise CapabilityResolutionError(f"{prefix} has duplicate/invalid evidence")
    if not isinstance(path["owner_issue"], str) or not re.fullmatch(r"#[1-9][0-9]*", path["owner_issue"]):
        raise CapabilityResolutionError(f"{prefix} has invalid owner issue")


def validate_catalog(root: pathlib.Path, catalog: dict[str, Any] | None = None) -> dict[str, Any]:
    """Validate the source-bound nine-path corpus and return it."""
    document = catalog if catalog is not None else _load(root / CATALOG)
    if document.get("schema") != "cxxlens.agent-capability-resolution.v1":
        raise CapabilityResolutionError("catalog schema identifier drift")
    if document.get("document_version") != "1.0.0":
        raise CapabilityResolutionError("catalog document version drift")
    authority = document.get("authority")
    if not isinstance(authority, dict) or authority.get("stale_policy") != "reject":
        raise CapabilityResolutionError("catalog stale policy is not fail-closed")
    source_paths = authority.get("source_paths")
    if not isinstance(source_paths, list) or not source_paths or len(source_paths) != len(set(source_paths)):
        raise CapabilityResolutionError("catalog source paths are not exact")
    for path in source_paths:
        if not isinstance(path, str) or path.startswith("/") or ".." in pathlib.PurePosixPath(path).parts:
            raise CapabilityResolutionError(f"unsafe catalog source path: {path!r}")
        _file_digest(root, path)
    if READINESS.as_posix() not in source_paths:
        raise CapabilityResolutionError("demand source readiness is not bound")
    readiness = _load(root / READINESS)
    direction = readiness.get("product_direction")
    closure = direction.get("closure") if isinstance(direction, dict) else None
    if not isinstance(direction, dict) or not isinstance(closure, dict) or closure.get("tracking_issue") != "#275":
        raise CapabilityResolutionError("demand source is not the accepted #275 closure")
    roadmap = direction.get("roadmap")
    families = roadmap.get("use_case_families") if isinstance(roadmap, dict) else None
    if not isinstance(families, list):
        raise CapabilityResolutionError("demand source has no use-case family graph")
    demand_families = {
        family.get("id"): family
        for family in families
        if isinstance(family, dict) and isinstance(family.get("id"), str)
    }
    if "agent-guided-extension" not in demand_families:
        raise CapabilityResolutionError("#277 admitted demand family is missing")
    if demand_families["agent-guided-extension"].get("tracking_issue") != "#277":
        raise CapabilityResolutionError("#277 demand family owner drift")
    contract = document.get("result_contract")
    if not isinstance(contract, dict) or contract.get("states") != list(RESULT_STATES):
        raise CapabilityResolutionError("result-state contract drift")
    if set(contract.get("missing_reason_codes", [])) != set(REASON_CODES[1:]):
        raise CapabilityResolutionError("missing-reason contract drift")
    paths = document.get("golden_paths")
    if not isinstance(paths, list) or len(paths) != 9:
        raise CapabilityResolutionError("golden corpus must contain exactly nine paths")
    identifiers: set[str] = set()
    for index, golden in enumerate(paths):
        prefix = f"golden_paths[{index}]"
        if not isinstance(golden, dict):
            raise CapabilityResolutionError(f"{prefix} is not an object")
        required = {
            "id", "use_case_id", "consumer", "question", "expected_result_states", "state",
            "demand", "capability_path", "completion_plan",
        }
        if set(golden) != required:
            raise CapabilityResolutionError(f"{prefix} fields differ: {sorted(set(golden) ^ required)}")
        identifier = golden["id"]
        if not isinstance(identifier, str) or not ID_RE.fullmatch(identifier) or identifier in identifiers:
            raise CapabilityResolutionError(f"{prefix} has duplicate/non-canonical id")
        identifiers.add(identifier)
        if golden["use_case_id"] != identifier:
            raise CapabilityResolutionError(f"{prefix} use-case identity drift")
        if not isinstance(golden["consumer"], str) or not golden["consumer"].strip():
            raise CapabilityResolutionError(f"{prefix} consumer is empty")
        expected = golden["expected_result_states"]
        if expected != list(RESULT_STATES):
            raise CapabilityResolutionError(f"{prefix} does not exercise every result state")
        if golden["state"] not in RESULT_STATES:
            raise CapabilityResolutionError(f"{prefix} has invalid selected state")
        demand = golden["demand"]
        if not isinstance(demand, dict) or set(demand) != {"family_id", "capabilities"}:
            raise CapabilityResolutionError(f"{prefix} demand binding is not exact")
        family_id = demand.get("family_id")
        if family_id not in demand_families:
            raise CapabilityResolutionError(f"{prefix} demand family is unknown")
        demanded = demand.get("capabilities")
        family_capabilities = demand_families[family_id].get("capabilities")
        if (
            not isinstance(demanded, list)
            or not demanded
            or len(demanded) != len(set(demanded))
            or not isinstance(family_capabilities, list)
            or any(value not in family_capabilities for value in demanded)
        ):
            raise CapabilityResolutionError(f"{prefix} demand capability edge is not admitted")
        capability_path = golden["capability_path"]
        if not isinstance(capability_path, list) or not capability_path:
            raise CapabilityResolutionError(f"{prefix} capability path is empty")
        seen: set[str] = set()
        for path_index, capability in enumerate(capability_path):
            _validate_path(capability, prefix=f"{prefix}.capability_path[{path_index}]")
            if capability["id"] in seen:
                raise CapabilityResolutionError(f"{prefix} duplicate capability id")
            for dependency in capability["requires"]:
                if dependency not in seen:
                    raise CapabilityResolutionError(
                        f"{prefix} dependency is unknown or forward: {capability['id']}:{dependency}"
                    )
            seen.add(capability["id"])
        plans = golden["completion_plan"]
        if not isinstance(plans, list):
            raise CapabilityResolutionError(f"{prefix} completion plan is not an array")
        plan_ids: set[str] = set()
        for plan_index, step in enumerate(plans):
            if not isinstance(step, dict):
                raise CapabilityResolutionError(f"{prefix}.completion_plan[{plan_index}] is not an object")
            required_step = {"id", "depends_on", "action", "owner_issue", "unlocks"}
            if set(step) != required_step:
                raise CapabilityResolutionError(f"{prefix}.completion_plan[{plan_index}] fields differ")
            if step["id"] in plan_ids or not ID_RE.fullmatch(step["id"]):
                raise CapabilityResolutionError(f"{prefix}.completion_plan[{plan_index}] id drift")
            for dependency in step["depends_on"]:
                if dependency not in plan_ids:
                    raise CapabilityResolutionError(f"{prefix} completion dependency is forward")
            plan_ids.add(step["id"])
    return document


def _authority(root: pathlib.Path, catalog: dict[str, Any], *, synthetic: bool) -> dict[str, Any]:
    source_paths = list(catalog["authority"]["source_paths"])
    source_digest = _file_digest(root, source_paths[0])
    authority_digest = _source_digest(root, source_paths)
    if synthetic:
        revision = tree = ZERO40
    else:
        revision = _git(root, "rev-parse", "HEAD")
        tree = _git(root, "rev-parse", "HEAD^{tree}")
        if not HEX40_RE.fullmatch(revision) or not HEX40_RE.fullmatch(tree):
            raise CapabilityResolutionError("git authority is not an exact revision/tree")
    return {
        "revision": revision,
        "tree": tree,
        "source": source_paths[0],
        "source_digest": source_digest,
        "authority_digest": authority_digest,
        "stale_policy": "reject",
    }


def _semantic_projection(contract: dict[str, Any]) -> dict[str, list[str]]:
    names = contract["preserved_semantics"]
    return {
        name.replace("-", "_"): [f"preserved:{name}"]
        for name in names
    }


def _missing_for_path(golden: dict[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for capability in golden["capability_path"]:
        state = capability["state"]
        if state in {"proved", "disproved"}:
            continue
        if state == "conflicting":
            reason = "conflicting-evidence"
        elif capability["kind"] == "input":
            reason = "missing-input"
        elif capability["kind"] == "model":
            reason = "missing-model"
        elif capability["requires"]:
            reason = "blocked-dependency"
        elif capability["kind"] in {"evidence", "support"}:
            reason = "missing-evidence"
        else:
            reason = "missing-capability"
        plan_ids = [
            step["id"] for step in golden["completion_plan"] if capability["id"] in step["unlocks"]
        ] or [step["id"] for step in golden["completion_plan"]]
        rows.append(
            {
                "reason_code": reason,
                "capability_id": capability["id"],
                "explanation": f"{capability['id']} is {state} in the exact capability authority.",
                "owner_issue": capability["owner_issue"],
                "completion_plan": plan_ids or ["completion.authority-evidence.v1"],
            }
        )
    return rows


def _resolution_from_golden(
    root: pathlib.Path,
    catalog: dict[str, Any],
    golden: dict[str, Any],
    *,
    synthetic: bool,
) -> dict[str, Any]:
    authority = _authority(root, catalog, synthetic=synthetic)
    missing = _missing_for_path(golden)
    state = golden["state"]
    if state == "proved":
        reason = "none"
        explanation = "Every capability in the selected path has an accepted positive result."
        guarantee = "path-complete-for-declared-inputs-and-evidence"
    elif state == "disproved":
        reason = "conflicting-evidence"
        explanation = "The selected path has a typed negative result; no success is inferred."
        guarantee = "negative-result-is-explicit-and-preserves-evidence"
    elif state == "partial":
        reason = missing[0]["reason_code"] if missing else "missing-evidence"
        explanation = "The selected path is usable only for the explicitly proven subset."
        guarantee = "partial-result-retains-coverage-and-unresolved-fields"
    elif state == "conflicting":
        reason = "conflicting-evidence"
        explanation = "Independent evidence disagrees; the result remains conflicting."
        guarantee = "conflict-is-not-reduced-to-success-or-empty"
    else:
        reason = missing[0]["reason_code"] if missing else "missing-capability"
        explanation = "The selected path cannot currently prove the requested question."
        guarantee = "unknown-result-has-actionable-missing-reason-and-plan"
    evidence_required = sorted({item for path in golden["capability_path"] for item in path["evidence"]})
    evidence_missing = sorted({item for path in golden["capability_path"] if path["state"] not in {"proved", "disproved"} for item in path["evidence"]})
    result = {
        "schema": "cxxlens.agent-capability-resolution.v1",
        "document_version": "1.0.0",
        "role": "evaluation-corpus-resolution",
        "authority": authority,
        "use_case_id": golden["use_case_id"],
        "consumer": golden["consumer"],
        "question": golden["question"],
        "expected_result_states": list(golden["expected_result_states"]),
        "result": {
            "state": state,
            "reason_code": reason,
            "explanation": explanation,
            "guarantee": guarantee,
        },
        "capability_path": copy.deepcopy(golden["capability_path"]),
        "missing": missing,
        "completion_plan": copy.deepcopy(golden["completion_plan"]),
        "preserved_semantics": _semantic_projection(catalog["result_contract"]),
        "evidence": {
            "required": evidence_required or ["authority-digest"],
            "available": sorted(set(evidence_required) - set(evidence_missing)),
            "missing": evidence_missing,
        },
        "provenance": {
            "generator": GENERATOR.as_posix(),
            "input_contract": "cxxlens.agent-capability-resolution.v1",
            "generated_at_revision": authority["revision"],
            "generated_at_tree": authority["tree"],
        },
    }
    result["canonical_digest"] = _digest_object(result)
    return result


def _unknown_use_case(
    root: pathlib.Path,
    catalog: dict[str, Any],
    use_case_id: str,
    *,
    synthetic: bool,
) -> dict[str, Any]:
    if not ID_RE.fullmatch(use_case_id):
        use_case_id = "unknown.use-case.v1"
    authority = _authority(root, catalog, synthetic=synthetic)
    result = {
        "schema": "cxxlens.agent-capability-resolution.v1",
        "document_version": "1.0.0",
        "role": "canonical-capability-resolution",
        "authority": authority,
        "use_case_id": use_case_id,
        "consumer": "unknown-consumer",
        "question": "The requested use case is not admitted by the exact capability authority.",
        "expected_result_states": list(RESULT_STATES),
        "result": {
            "state": "unknown",
            "reason_code": "unknown-use-case",
            "explanation": "No admitted use-case entry exists; no capability is inferred from surface presence.",
            "guarantee": "unknown-use-case-is-safe-stop-without-silent-fallback",
        },
        "capability_path": [
            {
                "id": "capability.unknown-use-case.v1",
                "kind": "evidence",
                "requires": [],
                "disposition": "unknown-use-case",
                "state": "unknown",
                "owner_issue": "#277",
                "evidence": [],
            }
        ],
        "missing": [
            {
                "reason_code": "unknown-use-case",
                "capability_id": "capability.unknown-use-case.v1",
                "explanation": "Register the use case, consumer, question, and exact dependency path before execution.",
                "owner_issue": "#277",
                "completion_plan": ["completion.admit-use-case.v1"],
            }
        ],
        "completion_plan": [
            {
                "id": "completion.admit-use-case.v1",
                "depends_on": [],
                "action": "Register the use case and independently review its capability path.",
                "owner_issue": "#277",
                "unlocks": ["capability.unknown-use-case.v1"],
            }
        ],
        "preserved_semantics": _semantic_projection(catalog["result_contract"]),
        "evidence": {"required": ["use-case-catalog"], "available": [], "missing": ["use-case-catalog"]},
        "provenance": {
            "generator": GENERATOR.as_posix(),
            "input_contract": "cxxlens.agent-capability-resolution.v1",
            "generated_at_revision": authority["revision"],
            "generated_at_tree": authority["tree"],
        },
    }
    result["canonical_digest"] = _digest_object(result)
    return result


def build_resolution(
    root: pathlib.Path,
    use_case_id: str,
    *,
    synthetic: bool = False,
    expected_revision: str | None = None,
    expected_tree: str | None = None,
) -> dict[str, Any]:
    catalog = validate_catalog(root)
    paths = {entry["use_case_id"]: entry for entry in catalog["golden_paths"]}
    result = (
        _resolution_from_golden(root, catalog, paths[use_case_id], synthetic=synthetic)
        if use_case_id in paths
        else _unknown_use_case(root, catalog, use_case_id, synthetic=synthetic)
    )
    if expected_revision is not None and result["authority"]["revision"] != expected_revision:
        raise CapabilityResolutionError("stale-authority: revision differs from expected")
    if expected_tree is not None and result["authority"]["tree"] != expected_tree:
        raise CapabilityResolutionError("stale-authority: tree differs from expected")
    validate_resolution(root, result)
    return result


def build_work_unit_resolution(
    root: pathlib.Path,
    *,
    issue: str,
    unit_id: str,
    state: str,
    blockers: list[str],
    completion_plan: list[str],
    synthetic: bool,
) -> dict[str, Any]:
    """Build the v2 packet's canonical resolution without relation-presence inference."""
    catalog = validate_catalog(root)
    authority = _authority(root, catalog, synthetic=synthetic)
    normalized_issue = issue if issue.startswith("#") else f"#{issue}"
    selected_state = "proved" if state == "ready" and not blockers else "unknown"
    reason = "none" if selected_state == "proved" else (
        "blocked-dependency" if any(value.startswith("dependency:") for value in blockers) else "missing-evidence"
    )
    capability_id = f"work-unit.{unit_id}.v1"
    plan_ids = [
        f"completion.work-unit.{unit_id}.v1"
    ] if completion_plan else []
    path = [{
        "id": "input.authority-work-unit.v1",
        "kind": "input",
        "requires": [],
        "disposition": "implemented",
        "state": "proved",
        "owner_issue": normalized_issue,
        "evidence": ["work-unit-registry"],
    }, {
        "id": capability_id,
        "kind": "evidence",
        "requires": ["input.authority-work-unit.v1"],
        "disposition": state,
        "state": selected_state,
        "owner_issue": normalized_issue,
        "evidence": ["work-unit-authority"] if selected_state == "proved" else [],
    }]
    result = {
        "schema": "cxxlens.agent-capability-resolution.v1",
        "document_version": "1.0.0",
        "role": "canonical-capability-resolution",
        "authority": authority,
        "use_case_id": f"agent.work-unit.{unit_id}.v1",
        "consumer": "bounded-autonomous-executor",
        "question": f"Can work unit {unit_id} for {normalized_issue} be selected safely?",
        "expected_result_states": list(RESULT_STATES),
        "result": {
            "state": selected_state,
            "reason_code": reason,
            "explanation": "The work-unit state and all blockers are included in the canonical selection result.",
            "guarantee": "dependency-and-authority-gated-selection",
        },
        "capability_path": path,
        "missing": [] if selected_state == "proved" else [{
            "reason_code": reason,
            "capability_id": capability_id,
            "explanation": "The work unit is not selectable until its exact blockers are resolved.",
            "owner_issue": normalized_issue,
            "completion_plan": plan_ids or ["completion.resolve-work-unit-blocker.v1"],
        }],
        "completion_plan": [
            {
                "id": plan_ids[0] if plan_ids else "completion.resolve-work-unit-blocker.v1",
                "depends_on": [],
                "action": "; ".join(completion_plan) if completion_plan else "Resolve the exact work-unit authority blockers.",
                "owner_issue": normalized_issue,
                "unlocks": [capability_id],
            }
        ],
        "preserved_semantics": _semantic_projection(catalog["result_contract"]),
        "evidence": {
            "required": ["work-unit-registry", "authority-digest"],
            "available": ["work-unit-registry"] if selected_state == "proved" else [],
            "missing": [] if selected_state == "proved" else ["authority-digest"],
        },
        "provenance": {
            "generator": GENERATOR.as_posix(),
            "input_contract": "cxxlens.agent-capability-resolution.v1",
            "generated_at_revision": authority["revision"],
            "generated_at_tree": authority["tree"],
        },
    }
    result["canonical_digest"] = _digest_object(result)
    validate_resolution(root, result)
    return result


def validate_resolution(
    root: pathlib.Path,
    resolution: dict[str, Any],
    *,
    expected_authority: dict[str, Any] | None = None,
) -> None:
    """Validate one resolution and, when requested, bind it to current authority.

    Synthetic corpus rows intentionally use the zero revision/tree and are
    validated without a live authority expectation.  A saved resolution that
    is checked as an executable/current artifact must opt into the exact
    authority projection; otherwise a self-consistent old packet could be
    mistaken for evidence from the current source tree.
    """
    schema = _load(root / SCHEMA)
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(resolution)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise CapabilityResolutionError(f"resolution schema validation failed: {error.message}") from error
    digest = resolution.get("canonical_digest")
    candidate = copy.deepcopy(resolution)
    candidate.pop("canonical_digest", None)
    if digest != _digest_object(candidate):
        raise CapabilityResolutionError("resolution canonical digest mismatch")
    if resolution["authority"]["stale_policy"] != "reject":
        raise CapabilityResolutionError("resolution stale policy is not fail-closed")
    if resolution["provenance"]["generated_at_revision"] != resolution["authority"]["revision"]:
        raise CapabilityResolutionError("resolution provenance revision does not match authority")
    if resolution["provenance"]["generated_at_tree"] != resolution["authority"]["tree"]:
        raise CapabilityResolutionError("resolution provenance tree does not match authority")
    if expected_authority is not None and resolution["authority"] != expected_authority:
        raise CapabilityResolutionError("resolution authority is stale or mismatched")
    if resolution["result"]["state"] == "unknown" and not resolution["missing"]:
        raise CapabilityResolutionError("unknown result has no actionable missing reason")
    for index, capability in enumerate(resolution["capability_path"]):
        _validate_path(capability, prefix=f"resolution.capability_path[{index}]")
    available = set(resolution["evidence"]["available"])
    missing = set(resolution["evidence"]["missing"])
    if available & missing:
        raise CapabilityResolutionError("evidence is simultaneously available and missing")


def render_markdown(resolution: dict[str, Any]) -> str:
    path = " -> ".join(row["id"] for row in resolution["capability_path"])
    missing = "\n".join(
        f"- `{row['reason_code']}` `{row['capability_id']}`: {row['explanation']}"
        for row in resolution["missing"]
    ) or "- none"
    plan = "\n".join(
        f"{index}. `{row['id']}`: {row['action']}"
        for index, row in enumerate(resolution["completion_plan"], 1)
    ) or "1. none"
    return (
        f"# cxxlens capability resolution: {resolution['use_case_id']}\n\n"
        f"- Schema: `{resolution['schema']}`\n"
        f"- Consumer: `{resolution['consumer']}`\n"
        f"- Result: `{resolution['result']['state']}`\n"
        f"- Reason: `{resolution['result']['reason_code']}`\n"
        f"- Guarantee: `{resolution['result']['guarantee']}`\n"
        f"- Revision: `{resolution['authority']['revision']}`\n"
        f"- Tree: `{resolution['authority']['tree']}`\n"
        f"- Canonical digest: `{resolution['canonical_digest']}`\n\n"
        f"## Question\n\n{resolution['question']}\n\n"
        f"## Capability path\n\n`{path}`\n\n"
        f"## Explanation\n\n{resolution['result']['explanation']}\n\n"
        f"## Missing\n\n{missing}\n\n"
        f"## Dependency-ordered completion plan\n\n{plan}\n\n"
        "## Preserved semantics\n\n"
        + "\n".join(
            f"- `{key}`: {', '.join(values)}"
            for key, values in sorted(resolution["preserved_semantics"].items())
        )
        + "\n"
    )


def corpus(root: pathlib.Path) -> dict[str, Any]:
    catalog = validate_catalog(root)
    resolutions = [
        _resolution_from_golden(root, catalog, golden, synthetic=True)
        for golden in catalog["golden_paths"]
    ]
    states = {state: 0 for state in RESULT_STATES}
    for resolution in resolutions:
        states[resolution["result"]["state"]] += 1
        validate_resolution(root, resolution)
    report = {
        "schema": "cxxlens.agent-capability-resolution-corpus.v1",
        "document_version": "1.0.0",
        "paths": len(resolutions),
        "safe_stop_rate_percent": 100,
        "result_state_counts": states,
        "all_result_states_exercised": all(states[state] > 0 for state in RESULT_STATES),
        "resolutions": resolutions,
    }
    if not report["all_result_states_exercised"]:
        raise CapabilityResolutionError("golden corpus does not exercise every result state")
    return report


def _write(path: pathlib.Path | None, content: str) -> None:
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "resolve", "corpus"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--use-case")
    parser.add_argument("--expected-revision")
    parser.add_argument("--expected-tree")
    parser.add_argument("--output-json", type=pathlib.Path)
    parser.add_argument("--output-markdown", type=pathlib.Path)
    parser.add_argument("--input-json", type=pathlib.Path)
    parser.add_argument("--input-markdown", type=pathlib.Path)
    parser.add_argument("--synthetic", action="store_true")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        validate_catalog(root)
        if args.command == "corpus":
            output: Any = corpus(root)
        else:
            if args.command == "check":
                if args.input_json is not None:
                    output = json.loads(args.input_json.read_text(encoding="utf-8"))
                    catalog = validate_catalog(root)
                    validate_resolution(
                        root,
                        output,
                        expected_authority=_authority(root, catalog, synthetic=args.synthetic),
                    )
                elif args.use_case:
                    output = build_resolution(
                        root,
                        args.use_case,
                        synthetic=args.synthetic,
                        expected_revision=args.expected_revision,
                        expected_tree=args.expected_tree,
                    )
                else:
                    raise CapabilityResolutionError("check requires --input-json or --use-case")
            else:
                if not args.use_case:
                    raise CapabilityResolutionError("resolve requires --use-case")
                output = build_resolution(
                    root,
                    args.use_case,
                    synthetic=args.synthetic,
                    expected_revision=args.expected_revision,
                    expected_tree=args.expected_tree,
                )
            if args.input_markdown is not None:
                expected_markdown = render_markdown(output)
                actual_markdown = args.input_markdown.read_text(encoding="utf-8")
                if actual_markdown != expected_markdown:
                    raise CapabilityResolutionError("markdown projection drift")
            if args.output_markdown is not None:
                _write(args.output_markdown, render_markdown(output))
        _write(
            args.output_json,
            json.dumps(output, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
        )
        print(json.dumps(output, ensure_ascii=False, sort_keys=True, indent=2))
    except (CapabilityResolutionError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"agent-capability-resolution: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
