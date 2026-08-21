#!/usr/bin/env python3
"""Validate CI freshness authority and emit bounded freshness/evaluation reports."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import pathlib
import subprocess
import sys
from typing import Any

import jsonschema
import yaml

from check_ng_release_evidence_bundle import (  # noqa: E402
    ReleaseEvidenceError,
    load_document,
    validate_bundle,
)


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONTRACT = pathlib.Path("schemas/cxxlens_ng_autonomy_ci.yaml")
SCHEMA = pathlib.Path("schemas/cxxlens_ng_autonomy_ci.schema.yaml")


class AutonomyCiError(ValueError):
    """A stale, promotable, or structurally invalid CI configuration."""


class UniqueKeyLoader(yaml.SafeLoader):
    """Safe YAML loader that rejects duplicate mapping keys."""


def _construct_unique_mapping(
    loader: UniqueKeyLoader, node: yaml.MappingNode, deep: bool = False
) -> dict[Any, Any]:
    loader.flatten_mapping(node)
    result: dict[Any, Any] = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in result:
            raise yaml.constructor.ConstructorError(
                "while constructing a mapping",
                node.start_mark,
                f"duplicate mapping key: {key!r}",
                key_node.start_mark,
            )
        result[key] = loader.construct_object(value_node, deep=deep)
    return result


UniqueKeyLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _construct_unique_mapping
)


def load(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = yaml.load(path.read_text(encoding="utf-8"), Loader=UniqueKeyLoader)
    except (OSError, UnicodeError, yaml.YAMLError) as error:
        raise AutonomyCiError(f"cannot load {path}: {error}") from error
    if not isinstance(value, dict):
        raise AutonomyCiError(f"expected mapping: {path}")
    return value


def git(root: pathlib.Path, *arguments: str) -> str:
    try:
        result = subprocess.run(["git", "-C", str(root), *arguments], check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as error:
        raise AutonomyCiError(f"git binding unavailable: {' '.join(arguments)}") from error
    return result.stdout.strip()


def triggers(document: dict[str, Any]) -> dict[str, Any]:
    value = document.get("on", document.get(True))
    if not isinstance(value, dict):
        raise AutonomyCiError("workflow trigger mapping missing")
    return value


def step_by(
    job: dict[str, Any], *, identifier: str | None = None, name: str | None = None
) -> dict[str, Any]:
    matches = [
        step
        for step in job.get("steps", [])
        if (identifier is None or step.get("id") == identifier)
        and (name is None or step.get("name") == name)
    ]
    if len(matches) != 1:
        raise AutonomyCiError(f"workflow step is missing or ambiguous: {identifier or name}")
    return matches[0]


def validate(root: pathlib.Path) -> dict[str, Any]:
    contract = load(root / CONTRACT)
    schema = load(root / SCHEMA)
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(contract)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise AutonomyCiError(f"schema validation failed: {error.message}") from error
    fast = load(root / contract["fast"]["workflow"])
    heavy = load(root / contract["heavy"]["workflow"])
    nightly = load(root / contract["nightly"]["workflow"])
    release = load(root / contract["release"]["workflow"])
    fast_on = triggers(fast)
    if set(fast_on) != {"push", "workflow_dispatch"} or fast_on["push"].get("branches") != ["main"]:
        raise AutonomyCiError("fast workflow is not every main SHA plus explicit dispatch")
    if "concurrency" in fast:
        raise AutonomyCiError("fast workflow may not cancel or coalesce commits")
    heavy_on = triggers(heavy)
    if set(heavy_on) != {"workflow_run"} or heavy_on.get("workflow_run", {}).get("workflows") != ["Autonomy fast"]:
        raise AutonomyCiError("heavy workflow is not connected to Autonomy fast")
    if heavy.get("concurrency") != {"group": "autonomy-heavy-latest-main", "cancel-in-progress": True}:
        raise AutonomyCiError("heavy coalescing policy drift")
    freshness = heavy["jobs"].get("freshness", {})
    refresh = step_by(freshness, name="Refresh current main authority")
    if refresh.get("run") != "git fetch --no-tags origin main":
        raise AutonomyCiError("heavy preflight main refresh drift")
    authenticate = step_by(freshness, name="Authenticate canonical fast workflow")
    if authenticate.get("env") != {
        "GH_TOKEN": "${{ github.token }}",
        "EVENT_WORKFLOW_ID": "${{ github.event.workflow_run.workflow_id }}",
    } or not all(
        marker in authenticate.get("run", "")
        for marker in (
            "actions/workflows/autonomy-fast.yml",
            "--jq .id",
            'test "${EVENT_WORKFLOW_ID}" = "${expected_id}"',
        )
    ):
        raise AutonomyCiError("heavy canonical fast workflow authentication drift")
    classify = step_by(freshness, identifier="classify")
    if classify.get("env") != {"EVENT_CANDIDATE": "${{ github.event.workflow_run.head_sha }}"} or "classify-heavy" not in classify.get("run", "") or "DISPATCH" in classify.get("run", ""):
        raise AutonomyCiError("heavy freshness classifier structure drift")
    heavy_job = heavy["jobs"].get("exact-latest-heavy", {})
    if heavy_job.get("if") != "needs.freshness.outputs.disposition == 'current'":
        raise AutonomyCiError("heavy current-candidate job guard drift")
    postflight = step_by(heavy_job, identifier="postflight")
    if postflight.get("if") != "always()" or "git fetch --no-tags origin main" not in postflight.get("run", "") or "classify-heavy" not in postflight.get("run", ""):
        raise AutonomyCiError("heavy postflight structure drift")
    uploads = [step for step in heavy_job.get("steps", []) if isinstance(step.get("uses"), str) and "upload-artifact" in step["uses"] and "provisional" in step.get("with", {}).get("name", "")]
    if len(uploads) != 1 or uploads[0].get("if") != "success() && steps.postflight.outputs.disposition == 'current'":
        raise AutonomyCiError("heavy provisional artifact guard drift")
    if not any("run_gate.py full" in step.get("run", "") for step in heavy_job.get("steps", [])):
        raise AutonomyCiError("heavy full gate missing")
    if contract["heavy"].get("artifact_authority") != "provisional-consumer-reauthenticates-current-origin-main":
        raise AutonomyCiError("heavy artifact is incorrectly authoritative")
    nightly_on = triggers(nightly)
    if "schedule" not in nightly_on or "workflow_dispatch" not in nightly_on:
        raise AutonomyCiError("Nightly lacks schedule/dispatch latest-main entry")
    if contract["nightly"].get("valid_events") != ["schedule", "workflow_dispatch"] or contract["nightly"].get("ineligible_events") != ["workflow_call"]:
        raise AutonomyCiError("Nightly release event eligibility drift")
    latest = nightly.get("jobs", {}).get("latest-main", {})
    if latest.get("if") != (
        "github.event_name == 'schedule' || github.event_name == 'workflow_dispatch' "
        "|| github.event_name == 'workflow_call'"
    ):
        raise AutonomyCiError(
            "Nightly does not execute the legacy workflow_call compatibility producer"
        )
    latest_text = json.dumps(latest, sort_keys=True)
    if "ref: main" not in (root / contract["nightly"]["workflow"]).read_text(encoding="utf-8") or "git rev-parse origin/main" not in latest_text:
        raise AutonomyCiError("Nightly latest-main binding missing")
    candidate_expression = "${{ needs.latest-main.outputs.candidate_sha }}"
    for name, job in nightly.get("jobs", {}).items():
        if name == "latest-main":
            continue
        needs = job.get("needs", [])
        needs = [needs] if isinstance(needs, str) else needs
        checkout = [step for step in job.get("steps", []) if isinstance(step.get("uses"), str) and step["uses"].startswith("actions/checkout@")]
        if "latest-main" not in needs or len(checkout) != 1 or checkout[0].get("with", {}).get("ref") != candidate_expression:
            raise AutonomyCiError(f"Nightly job is not exact-candidate bound: {name}")
    owner_workflows = {
        "gr": ("#167", "cxxlens-ng-release-qualification-${{ inputs.candidate_sha }}", "gr"),
        "terminal_scope": (
            "#179",
            "cxxlens-ng-production-scope-closure-${{ inputs.candidate_sha }}",
            "terminal_scope",
        ),
    }
    for owner_name, (owner_issue, artifact_name, role) in owner_workflows.items():
        owner_contract = contract[owner_name]
        owner = load(root / owner_contract["workflow"])
        owner_on = triggers(owner)
        if set(owner_on) != {"workflow_dispatch"}:
            raise AutonomyCiError(f"{owner_name} owner workflow is not dispatch-only")
        if owner.get("permissions") != {"contents": "read", "actions": "read"}:
            raise AutonomyCiError(f"{owner_name} owner workflow permissions drift")
        jobs = owner.get("jobs", {})
        if set(jobs) != {"collect-owner-report"}:
            raise AutonomyCiError(f"{owner_name} owner workflow job census drift")
        job = jobs["collect-owner-report"]
        if job.get("runs-on") != "ubuntu-24.04":
            raise AutonomyCiError(f"{owner_name} owner workflow runner drift")
        inputs = owner_on["workflow_dispatch"].get("inputs", {})
        if set(inputs) != {"candidate_sha", "selection_digest", "source_run_id", "source_artifact_id"}:
            raise AutonomyCiError(f"{owner_name} owner workflow input census drift")
        text = json.dumps(job, sort_keys=True)
        for marker in (
            "actions/runs/${SOURCE_RUN_ID}",
            "actions/workflows/quality.yml",
            "actions/artifacts/${SOURCE_ARTIFACT_ID}",
            "actions/artifacts/${SOURCE_ARTIFACT_ID}/zip",
            "check_ng_release_owner_handoff.py check",
            f"--role {role}",
            artifact_name,
        ):
            if marker not in text:
                raise AutonomyCiError(f"{owner_name} owner workflow marker is missing: {marker}")
    release_on = triggers(release)
    if set(release_on) != {"workflow_dispatch"}:
        raise AutonomyCiError("release evaluation is not dispatch-only")
    if set(release_on["workflow_dispatch"].get("inputs", {})) != {
        "candidate_sha",
        "selection_json",
    }:
        raise AutonomyCiError("release evaluation input census drift")
    if release.get("concurrency", {}).get("cancel-in-progress") is not False:
        raise AutonomyCiError("release evaluation may not be cancelled")
    if set(release.get("jobs", {})) != {"exact-current-evaluation"}:
        raise AutonomyCiError("release evaluation job census drift")
    release_job = release["jobs"]["exact-current-evaluation"]
    release_checkout = [
        step
        for step in release_job.get("steps", [])
        if isinstance(step.get("uses"), str) and step["uses"].startswith("actions/checkout@")
    ]
    if len(release_checkout) != 1 or release_checkout[0].get("with", {}).get("ref") != "${{ inputs.candidate_sha }}":
        raise AutonomyCiError("release checkout is not exact-candidate bound")
    freshness_step = step_by(
        release_job, name="Reject non-current or non-dispatch release evaluation"
    )
    freshness_run = freshness_step.get("run", "")
    required_freshness = (
        'test "${GITHUB_EVENT_NAME}" = "workflow_dispatch"',
        'test "$(git rev-parse HEAD)" = "${CANDIDATE_SHA}"',
        'test "$(git rev-parse origin/main)" = "${CANDIDATE_SHA}"',
    )
    if freshness_step.get("env") != {"CANDIDATE_SHA": "${{ inputs.candidate_sha }}"} or any(
        command not in freshness_run for command in required_freshness
    ):
        raise AutonomyCiError("release exact-main freshness step drift")
    evaluation_step = step_by(
        release_job, name="Validate release contracts and emit bounded aggregate evaluation"
    )
    evaluation_run = evaluation_step.get("run", "")
    if (
        "check_ng_release_contract.py check" not in evaluation_run
        or "check_ng_autonomy_ci.py release-evaluation" not in evaluation_run
        or "--bundle" not in evaluation_run
        or "--selection" not in evaluation_run
        or "--artifact-root" not in evaluation_run
    ):
        raise AutonomyCiError("release exact-main evaluation structure drift")
    release_text = json.dumps(release_job, sort_keys=True)
    for marker in (
        "collect_ng_release_evidence.py collect",
        "check_ng_release_evidence_bundle.py check",
    ):
        if marker not in release_text:
            raise AutonomyCiError("release exact-main evaluation structure drift")
    authenticate_step = step_by(release_job, identifier="authenticate")
    if authenticate_step.get("continue-on-error") is not True:
        raise AutonomyCiError("release evidence authentication must fail closed to not-qualified")
    if contract["release"]["composition"] != {"aggregate_decision": "#173", "gr_execution_contract": "#167", "scope_closure_contract": "#179"}:
        raise AutonomyCiError("release role composition drift")
    if contract["release"]["current_capability"] != "authenticated-evidence-handoff-no-gr":
        raise AutonomyCiError("release workflow overclaims current capability")
    return contract


def write_report(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")


def classify_heavy(root: pathlib.Path, candidate: str, output: pathlib.Path, report: pathlib.Path) -> None:
    if len(candidate) != 40 or any(character not in "0123456789abcdef" for character in candidate):
        raise AutonomyCiError("candidate SHA is not full lowercase hex")
    current = git(root, "rev-parse", "origin/main")
    disposition = "current" if candidate == current else "superseded"
    with output.open("a", encoding="utf-8") as stream:
        stream.write(f"disposition={disposition}\ncandidate_sha={candidate}\n")
    write_report(report, {"schema": "cxxlens.autonomy-heavy-freshness.v1", "candidate_sha": candidate, "current_origin_main": current, "disposition": disposition, "heavy_evidence_authority": disposition == "current"})


def release_evaluation(
    root: pathlib.Path,
    candidate: str,
    report: pathlib.Path,
    bundle_path: pathlib.Path | None = None,
    selection_path: pathlib.Path | None = None,
    artifact_root: pathlib.Path | None = None,
) -> None:
    current = git(root, "rev-parse", "origin/main")
    if candidate != current or git(root, "rev-parse", "HEAD") != candidate:
        raise AutonomyCiError("release candidate is not exact current origin/main")
    missing_inputs = [
        "exact-successful-heavy",
        "exact-successful-nightly",
        "authenticated-gr-report",
        "authenticated-terminal-scope-report",
    ]
    status = "not-qualified"
    bundle_digest = None
    selection_digest_value = None
    if bundle_path is not None:
        try:
            if selection_path is None or artifact_root is None:
                raise AutonomyCiError(
                    "release evidence bundle authentication requires selection and artifact root"
                )
            selection_value = load_document(selection_path)
            bundle_value = load_document(bundle_path)
            validate_bundle(bundle_value, selection_value, root, artifact_root)
        except ReleaseEvidenceError as error:
            raise AutonomyCiError(f"release evidence bundle authentication failed: {error}") from error
        candidate_value = bundle_value.get("candidate")
        if not isinstance(candidate_value, dict) or candidate_value.get("sha") != candidate:
            raise AutonomyCiError("release evidence bundle candidate differs")
        if bundle_value.get("gr_issued") is not False or bundle_value.get("production_qualification") != "not-claimed":
            raise AutonomyCiError("release evidence bundle overclaims production authority")
        selection_digest_value = bundle_value.get("selection_digest")
        if not isinstance(selection_digest_value, str) or not selection_digest_value.startswith("sha256:"):
            raise AutonomyCiError("release evidence bundle selection digest is invalid")
        bundle_digest = "sha256:" + hashlib.sha256(bundle_path.read_bytes()).hexdigest()
        if bundle_value.get("outcome") == "qualified-inputs-ready":
            status = "qualified"
            missing_inputs = []
        elif bundle_value.get("outcome") == "superseded":
            missing_inputs = ["current-origin-main-producer-evidence"]
        elif bundle_value.get("outcome") == "not-qualified":
            roles = bundle_value.get("roles", {})
            missing_inputs = [
                label
                for role, label in (
                    ("heavy", "exact-successful-heavy"),
                    ("nightly", "exact-successful-nightly"),
                    ("gr", "authenticated-gr-report"),
                    ("terminal_scope", "authenticated-terminal-scope-report"),
                )
                if not isinstance(roles.get(role), dict)
                or roles[role].get("disposition") != "qualified"
            ]
            if not missing_inputs:
                missing_inputs = ["authenticated-release-evidence-bundle"]
        else:
            raise AutonomyCiError("release evidence bundle has unsupported outcome")
    value: dict[str, Any] = {
        "schema": "cxxlens.autonomy-release-evaluation.v1",
        "created_at": datetime.datetime.now(datetime.UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "candidate_sha": candidate,
        "current_origin_main": current,
        "status": status,
        "gr_issued": False,
        "roles": {"aggregate": "#173", "gr": "#167", "scope_closure": "#179"},
        "missing_inputs": missing_inputs,
        "production_qualification": "not-claimed",
    }
    if bundle_digest is not None:
        value["authenticated_bundle_digest"] = bundle_digest
        value["selection_digest"] = selection_digest_value
    write_report(report, value)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "classify-heavy", "release-evaluation"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--candidate")
    parser.add_argument("--github-output", type=pathlib.Path)
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("--bundle", type=pathlib.Path)
    parser.add_argument("--selection", type=pathlib.Path)
    parser.add_argument("--artifact-root", type=pathlib.Path)
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        validate(root)
        if args.command == "classify-heavy":
            if not args.candidate or not args.github_output or not args.report:
                raise AutonomyCiError("classify-heavy requires candidate, github output, and report")
            classify_heavy(root, args.candidate, args.github_output, args.report)
        elif args.command == "release-evaluation":
            if not args.candidate or not args.report:
                raise AutonomyCiError("release-evaluation requires candidate and report")
            release_evaluation(
                root,
                args.candidate,
                args.report,
                args.bundle,
                args.selection,
                args.artifact_root,
            )
        else:
            print("autonomy-ci: ok")
    except AutonomyCiError as error:
        print(f"autonomy-ci: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
