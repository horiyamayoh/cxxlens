#!/usr/bin/env python3
"""Generate and validate the bounded authority-derived agent context.

This slice consumes the readiness-derived #275 declaration projection and the
explicit #261 packet template.  It never fills missing templates from another
use case, and it never turns the #276 witness projection into an acceptance.
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

import check_ng_use_case_capability_catalog as catalog
import check_ng_design_feedback as design_feedback
import check_ng_git_authority as git_authority


ROOT = pathlib.Path(__file__).resolve().parents[2]
READINESS_PATH = pathlib.Path("schemas/cxxlens_ng_api_development_readiness.yaml")
READINESS_SCHEMA_PATH = pathlib.Path(
    "schemas/cxxlens_ng_api_development_readiness.schema.yaml"
)
CONTEXT_SCHEMA_PATH = pathlib.Path("schemas/cxxlens_ng_agent_context.schema.yaml")
DESIGN_FEEDBACK_SCHEMA_PATH = pathlib.Path(
    "schemas/cxxlens_ng_design_feedback_record.schema.yaml"
)
DF_0261_RECORD_PATH = pathlib.Path(
    "docs/development/implementation-learning/records/df-0261-source-closure-vfs.md"
)
PACKET_TEMPLATE_KEY = "first_packet"
GENERATOR_PATH = pathlib.Path("tools/quality/check_ng_agent_context.py")
PROJECTION_ARTIFACT = "cxxlens-ng-agent-context-277-${revision}"
PROJECTION_AUTHORITY = "non-authoritative-projection"
PROJECTION_RELEASE_AUTHORITY = "none"
CANONICAL_ID = re.compile(r"^[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*\.v[0-9]+(?:_[0-9]+)?$")
HEX40 = re.compile(r"^[0-9a-f]{40}$")
ISSUE = re.compile(r"^#[0-9]+$")


class AgentContextError(ValueError):
    """A fail-closed context generation or validation error."""


def fail(message: str) -> None:
    raise AgentContextError(message)


def load_yaml(path: pathlib.Path) -> Any:
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, yaml.YAMLError) as error:
        raise AgentContextError(f"cannot load YAML: {path}: {error}") from error


def validate_schema(document: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(
            schema,
            format_checker=jsonschema.Draft202012Validator.FORMAT_CHECKER,
        ).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise AgentContextError(f"{label} schema validation failed: {error.message}") from error


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_bytes(value)).hexdigest()


def file_digest(path: pathlib.Path) -> str:
    try:
        return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
    except (OSError, UnicodeError) as error:
        raise AgentContextError(f"cannot digest authority file: {path}") from error


def git_value(root: pathlib.Path, expression: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "rev-parse", expression],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise AgentContextError(f"cannot bind exact source {expression}") from error
    value = result.stdout.strip()
    if not HEX40.fullmatch(value):
        raise AgentContextError(f"git returned a non-canonical {expression}")
    return value


def worktree_status(root: pathlib.Path) -> list[str]:
    """Return every tracked or untracked worktree entry before source binding."""
    try:
        result = subprocess.run(
            [
                "git",
                "-C",
                str(root),
                "status",
                "--porcelain=v1",
                "--untracked-files=all",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise AgentContextError("agent-context.worktree-status-unavailable") from error
    return [line for line in result.stdout.splitlines() if line]


def require_clean_worktree(root: pathlib.Path) -> None:
    changes = worktree_status(root)
    if changes:
        fail(
            "agent-context.worktree-dirty: exact-bound context requires a clean "
            f"tracked/untracked worktree ({len(changes)} entries)"
        )


def canonical_repository_path(value: Any) -> bool:
    if not isinstance(value, str) or not value:
        return False
    if value.startswith(("/", "\\")) or "\\" in value or "\x00" in value:
        return False
    parts = value.split("/")
    return all(part not in {"", ".", "..", ".git"} for part in parts)


def path_conflicts(left: str, right: str) -> bool:
    left_parts = tuple(left.split("/"))
    right_parts = tuple(right.split("/"))
    return (
        left_parts == right_parts
        or left_parts[: len(right_parts)] == right_parts
        or right_parts[: len(left_parts)] == left_parts
    )


def validate_path_set(values: Any, field: str, *, reject_overlap: bool) -> None:
    if not isinstance(values, list) or not values:
        fail(f"agent-context.{field}-missing")
    if any(not isinstance(value, str) for value in values):
        fail(f"agent-context.{field}-noncanonical")
    if len(values) != len(set(values)):
        fail(f"agent-context.{field}-duplicate")
    for value in values:
        if not canonical_repository_path(value):
            fail(f"agent-context.{field}-noncanonical:{value!r}")
    if reject_overlap:
        for index, left in enumerate(values):
            for right in values[index + 1 :]:
                if path_conflicts(left, right):
                    fail(f"agent-context.write-path-overlap:{left}:{right}")


def load_authorities(root: pathlib.Path) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    readiness = load_yaml(root / READINESS_PATH)
    readiness_schema = load_yaml(root / READINESS_SCHEMA_PATH)
    context_schema = load_yaml(root / CONTEXT_SCHEMA_PATH)
    if not isinstance(readiness, dict) or not isinstance(readiness_schema, dict):
        fail("agent-context.readiness-authority-invalid")
    validate_schema(readiness, readiness_schema, "readiness authority")
    if not isinstance(context_schema, dict):
        fail("agent-context.schema-authority-invalid")
    try:
        jsonschema.Draft202012Validator.check_schema(context_schema)
    except jsonschema.SchemaError as error:
        fail(f"agent-context schema is invalid: {error.message}")
    return readiness, readiness_schema, context_schema


def select_source(
    root: pathlib.Path,
    readiness: dict[str, Any],
    use_case_id: str,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    if CANONICAL_ID.fullmatch(use_case_id) is None:
        fail(f"agent-context.use-case-id-invalid:{use_case_id}")
    try:
        report = catalog.build_report(root)
    except catalog.CatalogError as error:
        fail(f"agent-context.demand-source-invalid:{error}")
    families = readiness.get("product_direction", {}).get("roadmap", {}).get(
        "use_case_families"
    )
    if not isinstance(families, list):
        fail("agent-context.use-case-authority-missing")
    matches = [
        family
        for family in families
        if isinstance(family, dict) and family.get("use_case_id") == use_case_id
    ]
    if len(matches) != 1:
        fail(f"agent-context.use-case-not-admitted:{use_case_id}")
    family = matches[0]
    catalog_matches = [
        entry
        for entry in report["use_cases"]
        if entry.get("id") == family.get("id")
    ]
    if len(catalog_matches) != 1:
        fail(f"agent-context.demand-closure-missing:{use_case_id}")
    catalog_entry = catalog_matches[0]
    if catalog_entry["capabilities"] != sorted(family.get("capabilities", [])):
        fail(f"agent-context.demand-closure-capability-drift:{use_case_id}")
    if catalog_entry["disposition"] != family.get("disposition"):
        fail(f"agent-context.demand-closure-disposition-drift:{use_case_id}")
    if catalog_entry["tracking_issue"] != family.get("tracking_issue"):
        fail(f"agent-context.demand-closure-owner-drift:{use_case_id}")
    agent_context = readiness.get("product_direction", {}).get("agent_context")
    template = agent_context.get(PACKET_TEMPLATE_KEY) if isinstance(agent_context, dict) else None
    if not isinstance(template, dict) or template.get("use_case_id") != use_case_id:
        fail(f"agent-context.template-unavailable:{use_case_id}")
    if agent_context.get("contract") != "agent.minimal-context.v1":
        fail("agent-context.contract-authority-mismatch")
    if agent_context.get("tracking_issue") != "#277":
        fail("agent-context.tracking-issue-authority-mismatch")
    projection = agent_context.get("projection")
    expected_projection = {
        "contract": "cxxlens.ng-agent-context.v1",
        "issue": "#277",
        "packet_issue": "#261",
        "use_case_id": use_case_id,
        "demand_closure_issue": "#275",
        "constructibility_issue": "#276",
        "design_feedback_record": "DF-0261",
        "output": {
            "json": "cxxlens-ng-agent-context-issue-277.json",
            "markdown": "cxxlens-ng-agent-context-issue-277.md",
        },
        "generator": GENERATOR_PATH.as_posix(),
        "artifact": PROJECTION_ARTIFACT,
        "authority": PROJECTION_AUTHORITY,
        "release_authority": PROJECTION_RELEASE_AUTHORITY,
        "consumer": "developer-context-only",
        "excluded_from": [
            "cxxlens-ng-api-development-readiness-report",
            "cxxlens-ng-release-qualification",
            "issue-closure",
        ],
        "authority_scope": "exact-authority-derived-developer-projection",
        "clean_source_required": True,
        "stale_policy": "reject",
    }
    if projection != expected_projection:
        fail("agent-context.projection-authority-mismatch")
    if agent_context.get("generator") != "tools/quality/check_ng_api_development_readiness.py":
        fail("agent-context.authoritative-generator-mismatch")
    return family, template, report


def validate_capability_path(family: dict[str, Any]) -> list[dict[str, Any]]:
    path = family.get("capability_path")
    if not isinstance(path, list) or not path:
        fail("agent-context.capability-path-missing")
    by_id: dict[str, dict[str, Any]] = {}
    for row in path:
        if not isinstance(row, dict):
            fail("agent-context.capability-path-entry-invalid")
        identifier = row.get("id")
        if not isinstance(identifier, str) or CANONICAL_ID.fullmatch(identifier) is None:
            fail(f"agent-context.capability-id-invalid:{identifier!r}")
        if identifier in by_id:
            fail(f"agent-context.capability-id-duplicate:{identifier}")
        requires = row.get("requires")
        owner_issue = row.get("owner_issue")
        if not isinstance(requires, list) or not isinstance(owner_issue, str) or not ISSUE.fullmatch(owner_issue):
            fail(f"agent-context.capability-owner-or-dependency-missing:{identifier}")
        if any(not isinstance(dependency, str) for dependency in requires):
            fail(f"agent-context.capability-dependency-invalid:{identifier}")
        if len(requires) != len(set(requires)):
            fail(f"agent-context.capability-dependency-duplicate:{identifier}")
        by_id[identifier] = row
    completed: set[str] = set()
    for row in path:
        identifier = row["id"]
        for dependency in row["requires"]:
            if dependency not in by_id:
                fail(f"agent-context.capability-dependency-unknown:{identifier}:{dependency}")
            if dependency not in completed:
                fail(f"agent-context.capability-dependency-forward:{identifier}:{dependency}")
        completed.add(identifier)
    return [
        {
            "id": row["id"],
            "kind": row["kind"],
            "requires": list(row["requires"]),
            "disposition": row["disposition"],
            "owner_issue": row["owner_issue"],
        }
        for row in path
    ]


def authority_contract_ids(
    product: dict[str, Any],
    family: dict[str, Any],
    capability_path: list[dict[str, Any]],
    agent_context: dict[str, Any],
    gate: dict[str, Any],
) -> list[str]:
    expected = [
        product.get("contract"),
        family.get("use_case_id"),
        *(row["id"] for row in capability_path),
        agent_context.get("contract"),
        gate.get("contract"),
    ]
    if any(
        not isinstance(identifier, str) or CANONICAL_ID.fullmatch(identifier) is None
        for identifier in expected
    ):
        fail("agent-context.machine-contract-authority-invalid")
    return expected


def bind_authority_reading(
    root: pathlib.Path, paths: Any
) -> list[dict[str, str]]:
    validate_path_set(paths, "authority-reading-set", reject_overlap=False)
    root = root.resolve()
    bindings: list[dict[str, str]] = []
    for relative in paths:
        try:
            mode, blob, content = git_authority.bind_head_blob(root, relative)
        except git_authority.GitAuthorityError as error:
            code = str(error).removeprefix("git-authority.")
            if code.startswith("path-missing:"):
                fail(f"agent-context.authority-reading-path-missing:{relative}")
            fail(f"agent-context.authority-reading-{code}")
        bindings.append(
            {
                "path": relative,
                "mode": mode,
                "blob": blob,
                "digest": git_authority.sha256_digest(content),
            }
        )
    return bindings


def validate_design_feedback_metadata(metadata: dict[str, Any]) -> None:
    if metadata.get("id") != "DF-0261":
        fail("agent-context.design-feedback-record-id-mismatch")
    if metadata.get("status") != "proposed":
        fail("agent-context.design-feedback-status-mismatch")
    if metadata.get("implementation_disposition") != "blocked":
        fail("agent-context.design-feedback-disposition-mismatch")
    review = metadata.get("review")
    if not isinstance(review, dict) or review.get("status") != "pending":
        fail("agent-context.design-feedback-review-status-mismatch")
    if metadata.get("resolution_refs") != []:
        fail("agent-context.design-feedback-resolution-refs-mismatch")


def bind_design_feedback(root: pathlib.Path) -> list[dict[str, Any]]:
    path = root / DF_0261_RECORD_PATH
    if not path.is_file():
        fail(f"agent-context.design-feedback-record-missing:{DF_0261_RECORD_PATH}")
    try:
        schema = design_feedback.load_mapping(root / DESIGN_FEEDBACK_SCHEMA_PATH)
        record = design_feedback.validate_record(root, path, schema)
    except (
        design_feedback.DesignFeedbackError,
        OSError,
        UnicodeError,
        yaml.YAMLError,
    ) as error:
        fail(f"agent-context.design-feedback-record-invalid:{error}")
    metadata = record.metadata
    validate_design_feedback_metadata(metadata)
    review = metadata["review"]
    return [
        {
            "id": metadata["id"],
            "path": DF_0261_RECORD_PATH.as_posix(),
            "digest": file_digest(path),
            "status": metadata["status"],
            "implementation_disposition": metadata["implementation_disposition"],
            "review_status": review["status"],
            "resolution_refs": list(metadata["resolution_refs"]),
        }
    ]


def build_context(
    root: pathlib.Path,
    *,
    use_case_id: str,
    issue: str,
    revision: str,
    tree: str,
) -> dict[str, Any]:
    require_clean_worktree(root)
    if not HEX40.fullmatch(revision) or not HEX40.fullmatch(tree):
        fail("agent-context.exact-sha-invalid")
    if (git_value(root, "HEAD"), git_value(root, "HEAD^{tree}")) != (revision, tree):
        fail("agent-context.source-revision-or-tree-mismatch")
    try:
        git_authority.require_head_bound_paths(
            root,
            (
                READINESS_PATH.as_posix(),
                READINESS_SCHEMA_PATH.as_posix(),
                CONTEXT_SCHEMA_PATH.as_posix(),
                catalog.CATALOG_SCHEMA_PATH.as_posix(),
                DESIGN_FEEDBACK_SCHEMA_PATH.as_posix(),
                DF_0261_RECORD_PATH.as_posix(),
                GENERATOR_PATH.as_posix(),
                pathlib.Path(catalog.__file__).resolve().relative_to(root).as_posix(),
                pathlib.Path(design_feedback.__file__).resolve().relative_to(root).as_posix(),
            ),
        )
    except (git_authority.GitAuthorityError, ValueError) as error:
        fail(f"agent-context.authority-source-{error}")
    readiness, _readiness_schema, context_schema = load_authorities(root)
    family, template, report = select_source(root, readiness, use_case_id)
    if template.get("issue") != issue:
        fail(f"agent-context.issue-binding-mismatch:{issue}:{template.get('issue')}")
    if issue != family.get("tracking_issue"):
        fail(f"agent-context.use-case-owner-mismatch:{issue}:{family.get('tracking_issue')}")
    product = readiness["product_direction"]
    result_contract = product["result_contract"]
    agent_context = product["agent_context"]
    if family.get("expected_result_states") != result_contract["states"]:
        fail("agent-context.result-state-algebra-drift")
    capability_path = validate_capability_path(family)
    template_capability_path = template.get("capability_path")
    if template_capability_path != [row["id"] for row in capability_path]:
        fail("agent-context.template-capability-path-drift")
    authority_reading_bindings = bind_authority_reading(
        root, template.get("authority_reading_set")
    )
    validate_path_set(template.get("allowed_write_paths"), "write-path", reject_overlap=True)
    feedback = template.get("known_design_feedback")
    issue_feedback = f"issue-{issue.removeprefix('#')}"
    if (
        not isinstance(feedback, list)
        or "DF-0261" not in feedback
        or issue_feedback not in feedback
    ):
        fail("agent-context.design-feedback-binding-missing")
    design_feedback_records = bind_design_feedback(root)
    constructibility = template.get("constructibility")
    gate = product.get("constructibility_gate")
    if not isinstance(constructibility, dict) or not isinstance(gate, dict):
        fail("agent-context.constructibility-authority-missing")
    if constructibility.get("gate_issue") != gate.get("tracking_issue"):
        fail("agent-context.constructibility-gate-binding-mismatch")
    if constructibility.get("disposition") != "blocked":
        fail("agent-context.constructibility-promotion-forbidden")
    if constructibility.get("gate_issue") != "#276":
        fail("agent-context.constructibility-gate-issue-mismatch")
    if gate.get("contract") != "development.constructibility-gate.v1":
        fail("agent-context.constructibility-contract-mismatch")
    constructibility_projection = {
        "contract": gate["contract"],
        "tracking_issue": gate["tracking_issue"],
        "applies_to": list(gate["applies_to"]),
        "required_witnesses": list(gate["required_witnesses"]),
        "acceptance_rule": gate["acceptance_rule"],
        "disposition": constructibility["disposition"],
        "reason": constructibility["reason"],
        "gate_issue": constructibility["gate_issue"],
        "authority_digest": digest(gate),
    }
    gap = family.get("tracked_gap")
    if not isinstance(gap, dict):
        fail("agent-context.capability-gap-missing")
    if gap.get("owner_issue") != issue or family.get("disposition") != "blocked":
        fail("agent-context.capability-gap-owner-or-disposition-mismatch")
    if template.get("completion_plan") != gap.get("completion_plan"):
        fail("agent-context.completion-plan-drift")
    if family.get("preserved_semantics") != result_contract["preserved_semantics"]:
        fail("agent-context.preserved-semantics-drift")
    exact_contract_ids = authority_contract_ids(
        product, family, capability_path, agent_context, gate
    )
    if template.get("exact_contract_ids") != exact_contract_ids:
        fail("agent-context.contract-id-authority-mismatch")
    demand_source = {
        "schema": report["schema"],
        "document_version": report["document_version"],
        "readiness_schema": report["source"]["readiness_schema"],
        "contract": report["source"]["contract"],
        "tracking_issue": report["source"]["tracking_issue"],
        "projection_scope": report["projection"]["scope"],
        "projection_status": report["projection"]["status"],
        "source_pointer": report["source"]["source_pointer"],
        "owner_issue": report["source"]["owner_issue"],
        "revision": report["source"]["revision"],
        "tree": report["source"]["tree"],
        "readiness_digest": report["source"]["readiness_digest"],
        "catalog_digest": digest(report),
    }
    if (demand_source["revision"], demand_source["tree"]) != (revision, tree):
        fail("agent-context.demand-source-stale")
    authority_projection = {
        "product_contract": product["contract"],
        "result_contract": result_contract,
        "use_case": family,
        "packet_template": template,
        "agent_context_projection": agent_context["projection"],
        "constructibility_gate": gate,
        "constructibility_projection": constructibility_projection,
        "demand_source": demand_source,
        "authority_reading_bindings": authority_reading_bindings,
        "design_feedback_records": design_feedback_records,
    }
    binding = {
        "revision": revision,
        "tree": tree,
        "readiness_path": READINESS_PATH.as_posix(),
        "readiness_digest": file_digest(root / READINESS_PATH),
        "readiness_schema_digest": file_digest(root / READINESS_SCHEMA_PATH),
        "demand_catalog_schema": catalog.CATALOG_SCHEMA_PATH.as_posix(),
        "demand_catalog_digest": file_digest(root / catalog.CATALOG_SCHEMA_PATH),
        "context_schema_digest": file_digest(root / CONTEXT_SCHEMA_PATH),
        "authority_projection_digest": digest(authority_projection),
        "generator": GENERATOR_PATH.as_posix(),
        "authority_scope": PROJECTION_AUTHORITY,
        "release_authority": PROJECTION_RELEASE_AUTHORITY,
        "worktree": "clean",
        "authority_reading_digest": digest(authority_reading_bindings),
        "design_feedback_records_digest": digest(design_feedback_records),
        "constructibility_authority_digest": digest(gate),
        "stale_policy": "reject",
    }
    packet = {
        "schema": "cxxlens.ng-agent-context.v1",
        "document_version": "1.0.0",
        "role": "bounded-non-authoritative-context-projection",
        "authority_scope": PROJECTION_AUTHORITY,
        "release_authority": PROJECTION_RELEASE_AUTHORITY,
        "packet_id": template["packet_id"],
        "issue": issue,
        "use_case_id": use_case_id,
        "consumer": family["consumer"],
        "goal": template["goal"],
        "expected_result_states": family["expected_result_states"],
        "preserved_semantics": family["preserved_semantics"],
        "capability_path": capability_path,
        "capability_gap": {
            "reason_code": gap["reason_code"],
            "owner_issue": gap["owner_issue"],
            "completion_plan": gap["completion_plan"],
            "reevaluation_trigger": gap["reevaluation_trigger"],
        },
        "exact_contract_ids": exact_contract_ids,
        "authority_reading_set": template["authority_reading_set"],
        "authority_reading_bindings": authority_reading_bindings,
        "allowed_write_paths": template["allowed_write_paths"],
        "required_evidence": template["required_evidence"],
        "known_design_feedback": feedback,
        "design_feedback_records": design_feedback_records,
        "constructibility": constructibility_projection,
        "forbidden_shortcuts": template["forbidden_shortcuts"],
        "completion_commands": template["completion_commands"],
        "blocked_reason": family["tracked_gap"]["reason_code"],
        "completion_plan": family["tracked_gap"]["completion_plan"],
        "demand_source": demand_source,
        "binding": binding,
    }
    packet["canonical_digest"] = digest(packet)
    validate_schema(packet, context_schema, "agent-context")
    return packet


def validate_context(
    root: pathlib.Path,
    packet: dict[str, Any],
    *,
    use_case_id: str,
    issue: str,
    revision: str,
    tree: str,
) -> None:
    expected = build_context(
        root,
        use_case_id=use_case_id,
        issue=issue,
        revision=revision,
        tree=tree,
    )
    if packet != expected:
        fail("agent-context.stale-or-not-machine-derived")
    without_digest = copy.deepcopy(packet)
    actual = without_digest.pop("canonical_digest", None)
    if actual != digest(without_digest):
        fail("agent-context.canonical-digest-mismatch")


def render_markdown(packet: dict[str, Any]) -> str:
    def bullets(values: list[str]) -> str:
        return "\n".join(f"- {value}" for value in values)

    path = " -> ".join(row["id"] for row in packet["capability_path"])
    constructibility = json.dumps(
        packet["constructibility"], ensure_ascii=False, sort_keys=True, indent=2
    )
    binding = json.dumps(packet["binding"], ensure_ascii=False, sort_keys=True, indent=2)
    demand_source = json.dumps(
        packet["demand_source"], ensure_ascii=False, sort_keys=True, indent=2
    )
    authority_reading = json.dumps(
        packet["authority_reading_bindings"],
        ensure_ascii=False,
        sort_keys=True,
        indent=2,
    )
    design_feedback = json.dumps(
        packet["design_feedback_records"],
        ensure_ascii=False,
        sort_keys=True,
        indent=2,
    )
    return (
        f"# cxxlens agent context: {packet['use_case_id']}\n\n"
        f"- Schema: `{packet['schema']}`\n"
        f"- Authority scope: `{packet['authority_scope']}`\n"
        f"- Release authority: `{packet['release_authority']}`\n"
        f"- Packet: `{packet['packet_id']}`\n"
        f"- Issue: `{packet['issue']}`\n"
        f"- Consumer: `{packet['consumer']}`\n"
        f"- Goal: `{packet['goal']}`\n"
        f"- Expected result states: {', '.join(f'`{value}`' for value in packet['expected_result_states'])}\n"
        f"- Preserved semantics: {', '.join(f'`{value}`' for value in packet['preserved_semantics'])}\n"
        f"- Blocked reason: `{packet['blocked_reason']}`\n"
        f"- Canonical digest: `{packet['canonical_digest']}`\n\n"
        "## Capability path\n\n"
        f"`{path}`\n\n"
        "## Capability gap\n\n"
        f"- Reason: `{packet['capability_gap']['reason_code']}`\n"
        f"- Owner: `{packet['capability_gap']['owner_issue']}`\n"
        f"- Reevaluation trigger: `{packet['capability_gap']['reevaluation_trigger']}`\n\n"
        "## Exact contract IDs\n\n"
        f"{bullets(packet['exact_contract_ids'])}\n\n"
        "## Minimum authority reading set\n\n"
        f"{bullets([f'`{value}`' for value in packet['authority_reading_set']])}\n\n"
        "## Individually bound authority reading files\n\n"
        f"```json\n{authority_reading}\n```\n\n"
        "## Allowed write paths\n\n"
        f"{bullets([f'`{value}`' for value in packet['allowed_write_paths']])}\n\n"
        "## Required evidence\n\n"
        f"{bullets(packet['required_evidence'])}\n\n"
        "## Known design feedback\n\n"
        f"{bullets(packet['known_design_feedback'])}\n\n"
        "## Bound design feedback records\n\n"
        f"```json\n{design_feedback}\n```\n\n"
        "## Constructibility\n\n"
        f"```json\n{constructibility}\n```\n\n"
        "## Forbidden shortcuts\n\n"
        f"{bullets(packet['forbidden_shortcuts'])}\n\n"
        "## Completion plan\n\n"
        + "\n".join(
            f"{index}. {value}"
            for index, value in enumerate(packet["completion_plan"], 1)
        )
        + "\n\n"
        "## Completion commands\n\n"
        f"{bullets(packet['completion_commands'])}\n\n"
        "## Demand source (#275)\n\n"
        f"```json\n{demand_source}\n```\n\n"
        "## Exact binding\n\n"
        f"```json\n{binding}\n```\n"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    plan = subparsers.add_parser("plan")
    plan.add_argument("--root", type=pathlib.Path, default=ROOT)
    plan.add_argument("--use-case", required=True)
    plan.add_argument("--issue", type=int, required=True)
    plan.add_argument("--expected-revision", required=True)
    plan.add_argument("--expected-tree", required=True)
    plan.add_argument("--output-json", type=pathlib.Path, required=True)
    plan.add_argument("--output-markdown", type=pathlib.Path, required=True)
    check = subparsers.add_parser("check")
    check.add_argument("--root", type=pathlib.Path, default=ROOT)
    check.add_argument("--use-case", required=True)
    check.add_argument("--issue", type=int, required=True)
    check.add_argument("--expected-revision", required=True)
    check.add_argument("--expected-tree", required=True)
    check.add_argument("--input-json", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    root = args.root.resolve()
    issue = f"#{args.issue}"
    try:
        packet = build_context(
            root,
            use_case_id=args.use_case,
            issue=issue,
            revision=args.expected_revision,
            tree=args.expected_tree,
        )
        if args.command == "plan":
            args.output_json.parent.mkdir(parents=True, exist_ok=True)
            args.output_markdown.parent.mkdir(parents=True, exist_ok=True)
            args.output_json.write_text(
                json.dumps(packet, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            args.output_markdown.write_text(render_markdown(packet), encoding="utf-8")
            print(f"wrote {args.output_json} and {args.output_markdown}")
        else:
            candidate = json.loads(args.input_json.read_text(encoding="utf-8"))
            if not isinstance(candidate, dict):
                fail("agent-context.input-not-object")
            validate_context(
                root,
                candidate,
                use_case_id=args.use_case,
                issue=issue,
                revision=args.expected_revision,
                tree=args.expected_tree,
            )
            print(f"validated {args.input_json}")
        return 0
    except (AgentContextError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"agent-context check failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
