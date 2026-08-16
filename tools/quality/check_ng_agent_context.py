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


ROOT = pathlib.Path(__file__).resolve().parents[2]
READINESS_PATH = pathlib.Path("schemas/cxxlens_ng_api_development_readiness.yaml")
READINESS_SCHEMA_PATH = pathlib.Path(
    "schemas/cxxlens_ng_api_development_readiness.schema.yaml"
)
CONTEXT_SCHEMA_PATH = pathlib.Path("schemas/cxxlens_ng_agent_context.schema.yaml")
PACKET_TEMPLATE_KEY = "first_packet"
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


def canonical_repository_path(value: Any) -> bool:
    if not isinstance(value, str) or not value:
        return False
    if value.startswith(("/", "\\")) or "\\" in value or "\x00" in value:
        return False
    parts = value.split("/")
    return all(part not in {"", ".", ".."} for part in parts)


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


def build_context(
    root: pathlib.Path,
    *,
    use_case_id: str,
    issue: str,
    revision: str,
    tree: str,
) -> dict[str, Any]:
    if not HEX40.fullmatch(revision) or not HEX40.fullmatch(tree):
        fail("agent-context.exact-sha-invalid")
    if (git_value(root, "HEAD"), git_value(root, "HEAD^{tree}")) != (revision, tree):
        fail("agent-context.source-revision-or-tree-mismatch")
    readiness, _readiness_schema, context_schema = load_authorities(root)
    family, template, report = select_source(root, readiness, use_case_id)
    if template.get("issue") != issue:
        fail(f"agent-context.issue-binding-mismatch:{issue}:{template.get('issue')}")
    if issue != family.get("tracking_issue"):
        fail(f"agent-context.use-case-owner-mismatch:{issue}:{family.get('tracking_issue')}")
    product = readiness["product_direction"]
    result_contract = product["result_contract"]
    if family.get("expected_result_states") != result_contract["states"]:
        fail("agent-context.result-state-algebra-drift")
    capability_path = validate_capability_path(family)
    template_capability_path = template.get("capability_path")
    if template_capability_path != [row["id"] for row in capability_path]:
        fail("agent-context.template-capability-path-drift")
    validate_path_set(template.get("authority_reading_set"), "authority-reading-set", reject_overlap=False)
    validate_path_set(template.get("allowed_write_paths"), "write-path", reject_overlap=True)
    feedback = template.get("known_design_feedback")
    issue_feedback = f"issue-{issue.removeprefix('#')}"
    if (
        not isinstance(feedback, list)
        or "DF-0261" not in feedback
        or issue_feedback not in feedback
    ):
        fail("agent-context.design-feedback-binding-missing")
    constructibility = template.get("constructibility")
    gate = product.get("constructibility_gate")
    if not isinstance(constructibility, dict) or not isinstance(gate, dict):
        fail("agent-context.constructibility-authority-missing")
    if constructibility.get("gate_issue") != gate.get("tracking_issue"):
        fail("agent-context.constructibility-gate-binding-mismatch")
    if constructibility.get("disposition") != "blocked":
        fail("agent-context.constructibility-promotion-forbidden")
    gap = family.get("tracked_gap")
    if not isinstance(gap, dict):
        fail("agent-context.capability-gap-missing")
    if gap.get("owner_issue") != issue or family.get("disposition") != "blocked":
        fail("agent-context.capability-gap-owner-or-disposition-mismatch")
    if template.get("completion_plan") != gap.get("completion_plan"):
        fail("agent-context.completion-plan-drift")
    if family.get("preserved_semantics") != result_contract["preserved_semantics"]:
        fail("agent-context.preserved-semantics-drift")
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
    binding = {
        "revision": revision,
        "tree": tree,
        "readiness_path": READINESS_PATH.as_posix(),
        "readiness_digest": file_digest(root / READINESS_PATH),
        "readiness_schema_digest": file_digest(root / READINESS_SCHEMA_PATH),
        "demand_catalog_schema": catalog.CATALOG_SCHEMA_PATH.as_posix(),
        "demand_catalog_digest": file_digest(root / catalog.CATALOG_SCHEMA_PATH),
        "context_schema_digest": file_digest(root / CONTEXT_SCHEMA_PATH),
        "authority_projection_digest": digest(
            {
                "product_contract": product["contract"],
                "result_contract": result_contract,
                "use_case": family,
                "packet_template": template,
                "constructibility_gate": gate,
                "demand_source": demand_source,
            }
        ),
        "stale_policy": "reject",
    }
    packet = {
        "schema": "cxxlens.ng-agent-context.v1",
        "document_version": "1.0.0",
        "role": "bounded-authority-derived-context",
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
        "exact_contract_ids": template["exact_contract_ids"],
        "authority_reading_set": template["authority_reading_set"],
        "allowed_write_paths": template["allowed_write_paths"],
        "required_evidence": template["required_evidence"],
        "known_design_feedback": feedback,
        "constructibility": constructibility,
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
    return (
        f"# cxxlens agent context: {packet['use_case_id']}\n\n"
        f"- Schema: `{packet['schema']}`\n"
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
        "## Allowed write paths\n\n"
        f"{bullets([f'`{value}`' for value in packet['allowed_write_paths']])}\n\n"
        "## Required evidence\n\n"
        f"{bullets(packet['required_evidence'])}\n\n"
        "## Known design feedback\n\n"
        f"{bullets(packet['known_design_feedback'])}\n\n"
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
