#!/usr/bin/env python3
"""Validate Wave 0 readiness plus the #291 acceleration contract.

The pre-#291 checker remains the frozen baseline oracle.  This module composes
that oracle with CI tiering, event-driven qualification, the first demand-closed
#261 use case, and exact agent-context generation.  The composition deliberately
preserves every previous readiness invariant while replacing only the workflow
assumptions superseded by #291.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import re
import sys
import tempfile
import types
from typing import Any

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
BASELINE_PATH = pathlib.Path(
    "tools/quality/check_ng_api_development_readiness_wave0_baseline.py"
)
BASELINE_DIGEST = "sha256:4e81eff25e898794381624a82d9d3c06ef9d219ddcb32de3721cd2b56f32089f"
MANIFEST_PATH = pathlib.Path("schemas/cxxlens_ng_api_development_readiness.yaml")
QUALITY_PATH = pathlib.Path(".github/workflows/quality.yml")
NIGHTLY_PATH = pathlib.Path(".github/workflows/nightly.yml")
AGENT_GOAL_PATH = pathlib.Path("docs/development/agent-api-development-goal.md")
PACKET_JSON_NAME = "cxxlens-ng-agent-context-issue-261.json"
PACKET_MARKDOWN_NAME = "cxxlens-ng-agent-context-issue-261.md"
USE_CASE_ID = "repository-semantic-query.explain-translation-unit.v1"
ISSUE_ID = "#261"
DIRECT_MAIN_AGENT_CONTRACT = pathlib.Path("AGENTS.md")
DIRECT_MAIN_GOAL_CONTRACT = pathlib.Path(
    "docs/development/agent-api-development-goal.md"
)
DIRECT_MAIN_DECISION_ADR = pathlib.Path(
    "docs/design/adr/0094-risk-tiered-goal-authorization.md"
)
DIRECT_MAIN_POLICY_ID = "CXXLENS_AGENT_AUTHORIZATION_V1"
DIRECT_MAIN_POLICY_TOKEN = re.compile(
    rf"(?<![A-Za-z0-9_]){re.escape(DIRECT_MAIN_POLICY_ID)}(?![A-Za-z0-9_])"
)
DIRECT_MAIN_COMMON_MARKERS = (
    "activation: explicit-goal-contract-reference",
    "non-activation: ordinary-request",
    "standing-scope: canonical-repository-active-unit",
    "platform-approval: never-bypass",
    "direct-main: issue-scoped-fast-forward-push-post-push-integration",
)
DIRECT_MAIN_GOAL_MARKERS = (
    *DIRECT_MAIN_COMMON_MARKERS,
    "notify-and-continue: reversible-same-contract-issue",
    "fresh-approval: exact-target-effect-after-disclosure",
    "external-blocker: evidence-options-stop",
    "skill-compatibility: prior-goal-authorization-satisfies-generic-approval",
    "pull-request: optional-for-risk-review-or-external-contribution",
    "fresh-approval-reuse: forbidden",
    "revocation: user-anytime",
)
LEGACY_PROTECTED_MAIN_PATTERNS = (
    re.compile(
        r"protected-main:\s*"
        r"unit-branch-pr-exact-head-review-merge-exact-merged-main"
    ),
    re.compile(r"direct-main:\s*prohibited"),
)
HEX40 = re.compile(r"^[0-9a-f]{40}$")
CANONICAL_ID = re.compile(r"^[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*\.v[0-9]+(?:_[0-9]+)?$")
BOUNDED_COMPLETION_GOAL_MARKERS = (
    "completion-class: bounded-implementation",
    "production-qualification: not-claimed-by-default",
    "issue-close-owner: bounded-issue-or-explicit-qualification-gate",
    "aggregate-qualification-owner: exact-main-integration-readiness-release",
    "reopen-condition: bounded-acceptance-or-scope-regression-only",
)
BOUNDED_COMPLETION_GOAL_TEXT = (
    "通常の implementation issue の既定完了クラスは **bounded implementation completion**",
    "issue を閉じるために distribution 全体の production qualification を再実行・再証明してはなりません。",
    "`production qualification: not claimed`",
    "Foundation、Wave 0、G5、`release-evaluation`、normal/final",
    "exact main SHA の required checks と fail-closed evidence",
    "全 tracked gap の解消後は `release-evaluation: qualified`",
    "final-mode production-scope report を同じ exact",
    "過去 SHA の成功を最終 SHA の evidence として流用しません。",
)
LEGACY_GOAL_ISSUE_CLOSE_PATTERNS = (
    re.compile(r"merged-main qualification と learning checkpoint 後の active issue close"),
    re.compile(r"production scope に tracked gap がある intermediate unit の merge 後"),
)


class AccelerationError(ValueError):
    """A fail-closed #291 contract violation."""


def _load_baseline() -> types.ModuleType:
    baseline_path = ROOT / BASELINE_PATH
    if not baseline_path.is_file():
        raise RuntimeError(
            "the tracked frozen readiness baseline is unavailable: "
            f"{BASELINE_PATH.as_posix()}"
        )
    baseline_source = baseline_path.read_bytes()
    actual_digest = "sha256:" + hashlib.sha256(baseline_source).hexdigest()
    if actual_digest != BASELINE_DIGEST:
        raise RuntimeError("the tracked frozen readiness baseline digest differs")
    module = types.ModuleType("_cxxlens_wave0_readiness_baseline")
    module.__file__ = str(baseline_path)
    module.__package__ = None
    exec(compile(baseline_source.decode("utf-8"), module.__file__, "exec"), module.__dict__)
    return module


_baseline = _load_baseline()
_baseline_validate_workflow = _baseline.validate_workflow
_baseline_validate_documents = _baseline.validate_documents
_baseline_build_report = _baseline.build_report

# Preserve the complete public helper surface used by existing tests and tools.
for _name in dir(_baseline):
    if not _name.startswith("__"):
        globals().setdefault(_name, getattr(_baseline, _name))

ReadinessError = _baseline.ReadinessError


def _fail(message: str) -> None:
    raise ReadinessError(message)


def _load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        _fail(f"expected mapping: {path}")
    return value


def _canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def _semantic_digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(_canonical_bytes(value)).hexdigest()


def _file_digest(path: pathlib.Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _normalized_condition(value: Any) -> str:
    return " ".join(value.split()) if isinstance(value, str) else ""


def validate_direct_main_authorization_contract(root: pathlib.Path) -> None:
    decision = root / DIRECT_MAIN_DECISION_ADR
    if not decision.is_file():
        _fail(
            "agent authorization decision ADR is missing: "
            f"{DIRECT_MAIN_DECISION_ADR}"
        )
    if "- Status: Accepted" not in decision.read_text(encoding="utf-8"):
        _fail("agent authorization decision ADR is not accepted")

    documents = {
        DIRECT_MAIN_AGENT_CONTRACT: DIRECT_MAIN_COMMON_MARKERS,
        DIRECT_MAIN_GOAL_CONTRACT: DIRECT_MAIN_GOAL_MARKERS,
    }
    for relative, markers in documents.items():
        path = root / relative
        if not path.is_file():
            _fail(f"agent authorization contract is missing: {relative}")
        text = path.read_text(encoding="utf-8")
        if len(DIRECT_MAIN_POLICY_TOKEN.findall(text)) != 1:
            _fail(
                "agent authorization policy ID must appear exactly once in "
                f"{relative}"
            )
        for marker in markers:
            if text.count(f"`{marker}`") != 1:
                _fail(
                    "agent authorization marker is missing or duplicated in "
                    f"{relative}: {marker}"
                )
        if any(pattern.search(text) for pattern in LEGACY_PROTECTED_MAIN_PATTERNS):
            _fail(f"legacy protected-main workflow is forbidden in {relative}")

    goal = (root / DIRECT_MAIN_GOAL_CONTRACT).read_text(encoding="utf-8")
    goal_example = re.compile(
        rf"(?m)^/goal\s+{re.escape(DIRECT_MAIN_GOAL_CONTRACT.as_posix())}"
        rf".*(?<![A-Za-z0-9_]){re.escape(DIRECT_MAIN_POLICY_ID)}"
        rf"(?![A-Za-z0-9_])"
    )
    if goal_example.search(goal) is None:
        _fail("short goal example does not bind the authorization policy ID")

def validate_bounded_completion_contract(root: pathlib.Path) -> None:
    """Keep the activated /goal contract aligned with completion-policy #291."""
    goal = (root / AGENT_GOAL_PATH).read_text(encoding="utf-8")
    for marker in BOUNDED_COMPLETION_GOAL_MARKERS:
        if goal.count(f"`{marker}`") != 1:
            _fail(f"bounded completion marker is missing or duplicated in goal: {marker}")
    normalized_goal = re.sub(r"\s+", " ", goal)
    for phrase in BOUNDED_COMPLETION_GOAL_TEXT:
        if re.sub(r"\s+", " ", phrase) not in normalized_goal:
            _fail(f"bounded completion contract text is missing from goal: {phrase}")
    for pattern in LEGACY_GOAL_ISSUE_CLOSE_PATTERNS:
        if pattern.search(goal):
            _fail("legacy issue-close requirement remains in the activated goal contract")


def _canonical_repo_path(value: str) -> bool:
    if not value or value.startswith(('/', '\\')) or '\\' in value or value.endswith('/'):
        return False
    parts = value.split('/')
    return all(part not in ('', '.', '..') and '\x00' not in part for part in parts)


def _product_contract(manifest: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    product = manifest.get("product_direction")
    if not isinstance(product, dict):
        _fail("product direction is missing")
    roadmap = product.get("roadmap")
    agent = product.get("agent_context")
    if not isinstance(roadmap, dict) or not isinstance(agent, dict):
        _fail("demand closure or agent-context authority is missing")
    families = roadmap.get("use_case_families")
    if not isinstance(families, list):
        _fail("use-case family inventory is missing")
    matches = [row for row in families if isinstance(row, dict) and row.get("use_case_id") == USE_CASE_ID]
    if len(matches) != 1:
        _fail(f"exactly one admitted #261 use case is required: {USE_CASE_ID}")
    packet = agent.get("first_packet")
    if not isinstance(packet, dict):
        _fail("the first #261 agent packet template is missing")
    return matches[0], packet


def validate_demand_closure(root: pathlib.Path, manifest: dict[str, Any]) -> None:
    use_case, packet = _product_contract(manifest)
    result_states = manifest["product_direction"]["result_contract"]["states"]
    expected_path = [
        "input.source-closure.v1",
        "input.effective-invocation.v1",
        "provider.clang22-materialization.v2_1",
        "artifact.semantic-snapshot.v1",
        "recipe.explain-translation-unit.v1",
    ]
    if use_case.get("id") != "repository-semantic-query":
        _fail("#261 use-case family identifier differs")
    if use_case.get("consumer") != "cxxmonster":
        _fail("#261 flagship consumer must be cxxmonster")
    if use_case.get("expected_result_states") != result_states:
        _fail("#261 result states differ from the product result algebra")
    capabilities = use_case.get("capability_path")
    if not isinstance(capabilities, list) or [row.get("id") for row in capabilities if isinstance(row, dict)] != expected_path:
        _fail("#261 capability path differs or is not dependency ordered")
    if len({row["id"] for row in capabilities}) != len(capabilities):
        _fail("#261 capability path contains duplicate IDs")
    for row in capabilities:
        identifier = row.get("id")
        if not isinstance(identifier, str) or CANONICAL_ID.fullmatch(identifier) is None:
            _fail(f"non-canonical capability ID: {identifier}")
        dependencies = row.get("requires")
        if not isinstance(dependencies, list):
            _fail(f"capability dependencies are missing: {identifier}")
        unknown = sorted(set(dependencies) - set(expected_path))
        if unknown:
            _fail(f"unknown #261 capability dependency: {identifier}:{unknown}")
        if any(expected_path.index(dep) >= expected_path.index(identifier) for dep in dependencies):
            _fail(f"cyclic or forward #261 capability dependency: {identifier}")
    source_closure = capabilities[0]
    if source_closure.get("disposition") != "blocked" or source_closure.get("owner_issue") != ISSUE_ID:
        _fail("source-closure capability must remain blocked by #261")
    if use_case.get("disposition") != "blocked" or use_case.get("tracking_issue") != ISSUE_ID:
        _fail("#261 use case must remain explicitly blocked")
    semantics = use_case.get("preserved_semantics")
    required_semantics = manifest["product_direction"]["result_contract"]["preserved_semantics"]
    if semantics != required_semantics:
        _fail("#261 use case drops required partiality/evidence semantics")
    gap = use_case.get("tracked_gap")
    if not isinstance(gap, dict):
        _fail("#261 tracked gap is missing")
    if gap.get("reason_code") != "source-closure-unavailable" or gap.get("owner_issue") != ISSUE_ID:
        _fail("#261 tracked-gap reason or owner differs")
    plan = gap.get("completion_plan")
    if not isinstance(plan, list) or len(plan) != 4 or len(set(plan)) != 4:
        _fail("#261 completion plan must contain four ordered review units")
    if gap.get("reevaluation_trigger") != "cxxmonster-self-repository-e2e-exact-sha":
        _fail("#261 reevaluation trigger differs")

    if packet.get("packet_id") != "agent-context.issue-261.explain-translation-unit.v1":
        _fail("#261 packet ID differs")
    if packet.get("issue") != ISSUE_ID or packet.get("use_case_id") != USE_CASE_ID:
        _fail("#261 packet binding differs")
    if packet.get("capability_path") != expected_path:
        _fail("#261 packet capability path differs")
    contract_ids = packet.get("exact_contract_ids")
    if not isinstance(contract_ids, list) or len(contract_ids) != len(set(contract_ids)):
        _fail("#261 packet contract IDs are absent or duplicated")
    for identifier in contract_ids:
        if not isinstance(identifier, str) or CANONICAL_ID.fullmatch(identifier) is None:
            _fail(f"non-canonical #261 contract ID: {identifier}")
    for key in ("authority_reading_set", "allowed_write_paths"):
        values = packet.get(key)
        if not isinstance(values, list) or not values:
            _fail(f"#261 packet field is empty: {key}")
        for value in values:
            if not isinstance(value, str) or not _canonical_repo_path(value):
                _fail(f"#261 packet path is not canonical: {key}:{value}")
    for key in (
        "required_evidence",
        "known_design_feedback",
        "forbidden_shortcuts",
        "completion_commands",
        "completion_plan",
    ):
        values = packet.get(key)
        if not isinstance(values, list) or not values or len(values) != len(set(values)):
            _fail(f"#261 packet list is absent or duplicated: {key}")
    constructibility = packet.get("constructibility")
    if constructibility != {
        "disposition": "blocked",
        "reason": "accepted-source-closure-authority-and-independent-review-required",
        "gate_issue": "#276",
    }:
        _fail("#261 constructibility disposition differs")
    binding = packet.get("binding")
    if binding != {
        "revision": "runtime-required",
        "tree": "runtime-required",
        "authority_digest": "runtime-derived",
        "stale_policy": "reject",
    }:
        _fail("#261 packet runtime binding policy differs")


def _workflow_jobs(document: dict[str, Any], label: str) -> dict[str, Any]:
    jobs = document.get("jobs")
    if not isinstance(jobs, dict):
        _fail(f"{label} jobs mapping is missing")
    return jobs


def _job(document: dict[str, Any], name: str) -> dict[str, Any]:
    job = _workflow_jobs(document, "quality workflow").get(name)
    if not isinstance(job, dict):
        _fail(f"required CI tier job is missing: {name}")
    return job


def _step(job: dict[str, Any], name: str) -> dict[str, Any]:
    steps = job.get("steps")
    if not isinstance(steps, list):
        _fail(f"CI job steps are missing: {name}")
    matches = [row for row in steps if isinstance(row, dict) and row.get("name") == name]
    if len(matches) != 1:
        _fail(f"CI step must occur exactly once: {name}")
    return matches[0]


_QUALITY_TRIGGER = """on:\n  pull_request:\n    types: [opened, synchronize, reopened, ready_for_review, converted_to_draft]\n  push:\n    branches:\n      - main\n"""
_LEGACY_QUALITY_TRIGGER = """on:\n  pull_request:\n  push:\n"""
_NIGHTLY_TRIGGER = """on:\n  workflow_call:\n  schedule:\n    - cron: \"17 18 * * *\"\n  workflow_dispatch:\n"""
_LEGACY_NIGHTLY_TRIGGER = """on:\n  push:\n    branches:\n      - main\n  schedule:\n    - cron: \"17 18 * * *\"\n  workflow_dispatch:\n"""
_NIGHTLY_CONCURRENCY = """concurrency:\n  group: nightly-quality-${{ github.sha }}\n  cancel-in-progress: false\n"""
_LEGACY_NIGHTLY_CONCURRENCY = """concurrency:\n  group: nightly-quality-${{ github.event_name == 'schedule' && 'scheduled' || 'rolling-main' }}\n  cancel-in-progress: ${{ github.event_name != 'schedule' }}\n"""
_NEW_RELEASE_NEEDS = "needs: [nightly-quality, g5-qualification, sqlite-store-v3-qualification]"
_LEGACY_RELEASE_NEEDS = "needs: [g5-qualification, sqlite-store-v3-qualification]"

_SETUP_DEVELOPER = """      - uses: ./.github/actions/setup-ci
        with:
          profile: developer
"""
_SETUP_STATIC_ANALYSIS = """      - uses: ./.github/actions/setup-ci
        with:
          profile: static-analysis
"""
_SETUP_NONE = """      - uses: ./.github/actions/setup-ci
        with:
          profile: none
"""
_SETUP_DEVELOPER_DOCUMENTATION_ONLY = """      - uses: ./.github/actions/setup-ci
        with:
          profile: developer
          documentation: "true"
          python-dependencies: "false"
"""
_LEGACY_PYTHON_SETUP = """      - uses: actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97  # v7.0.0
        with:
          python-version: "3.12.11"
      - run: "python -m pip install --require-hashes --only-binary=:all: --requirement tools/quality/requirements.lock"
"""
_LEGACY_DEVELOPER_SETUP = """      - name: Install exact Clang 22 toolchain
        run: python3 tools/ci/bootstrap_supply_chain.py install --profile developer
""" + _LEGACY_PYTHON_SETUP
_LEGACY_STATIC_ANALYSIS_SETUP = """      - name: Install Clang tools
        run: python3 tools/ci/bootstrap_supply_chain.py install --profile static-analysis
""" + _LEGACY_PYTHON_SETUP
_LEGACY_DEVELOPER_DOCUMENTATION_ONLY = """      - name: Install exact Clang 22 toolchain
        run: python3 tools/ci/bootstrap_supply_chain.py install --profile developer
      - name: Install exact Doxygen toolchain
        run: python3 tools/ci/bootstrap_supply_chain.py install --profile documentation
"""


def _project_legacy_setup(text: str) -> str:
    replacements = (
        (_SETUP_DEVELOPER_DOCUMENTATION_ONLY, _LEGACY_DEVELOPER_DOCUMENTATION_ONLY),
        (_SETUP_STATIC_ANALYSIS, _LEGACY_STATIC_ANALYSIS_SETUP),
        (_SETUP_DEVELOPER, _LEGACY_DEVELOPER_SETUP),
        (_SETUP_NONE, _LEGACY_PYTHON_SETUP),
    )
    for current, legacy in replacements:
        text = text.replace(current, legacy)
    if "./.github/actions/setup-ci" in text:
        _fail("legacy setup projection left a common setup action reference")
    return text

_DIRECT_NIGHTLY_DOWNLOAD = """      - name: Download exact-main Nightly evidence\n        uses: actions/download-artifact@fa0a91b85d4f404e444e00e005971372dc801d16  # v4.1.8\n        with:\n          name: cxxlens-nightly-evidence-${{ github.sha }}\n          path: build/release-evaluation-nightly\n"""
_LEGACY_NIGHTLY_LOOKUP_AND_DOWNLOAD = """      - name: Locate the exact-main Nightly evidence run\n        id: nightly-run\n        env:\n          GH_TOKEN: ${{ github.token }}\n        run: |\n          set -euo pipefail\n          run_id=\"\"\n          for attempt in $(seq 1 180); do\n            latest=\"$({\n              gh api --method GET \\\n                \"repos/${GITHUB_REPOSITORY}/actions/workflows/nightly.yml/runs\" \\\n                -f branch=main \\\n                -f head_sha=\"${GITHUB_SHA}\" \\\n                -f per_page=100 \\\n                --jq '([.workflow_runs[] | select((.event == \"push\" or .event == \"schedule\" or .event == \"workflow_dispatch\") and .head_branch == \"main\")] | sort_by(.created_at, .id) | reverse | .[0] | select(.) | [.id, .status, (.conclusion // \"\")] | @tsv) // empty'\n            })\"\n            if [[ -z \"${latest}\" ]]; then\n              sleep 30\n              continue\n            fi\n            IFS=$'\\t' read -r candidate status conclusion <<< \"${latest}\"\n            if [[ \"${status}\" != \"completed\" ]]; then\n              sleep 30\n              continue\n            fi\n            if [[ \"${conclusion}\" != \"success\" ]]; then\n              echo \"exact-main Nightly run ${candidate} completed with ${conclusion}\" >&2\n              exit 1\n            fi\n            run_id=\"${candidate}\"\n            break\n          done\n          if [[ -z \"${run_id}\" ]]; then\n            echo \"no successful exact-main Nightly run became available for ${GITHUB_SHA}\" >&2\n            exit 1\n          fi\n          echo \"run-id=${run_id}\" >> \"${GITHUB_OUTPUT}\"\n      - name: Download exact-main Nightly evidence\n        uses: actions/download-artifact@fa0a91b85d4f404e444e00e005971372dc801d16  # v4.1.8\n        with:\n          name: cxxlens-nightly-evidence-${{ github.sha }}\n          github-token: ${{ github.token }}\n          repository: ${{ github.repository }}\n          run-id: ${{ steps.nightly-run.outputs.run-id }}\n          path: build/release-evaluation-nightly\n"""


def _validate_accelerated_workflow(root: pathlib.Path, manifest: dict[str, Any]) -> None:
    quality_text = (root / QUALITY_PATH).read_text(encoding="utf-8")
    nightly_text = (root / NIGHTLY_PATH).read_text(encoding="utf-8")
    try:
        quality = yaml.safe_load(quality_text)
        nightly = yaml.safe_load(nightly_text)
    except yaml.YAMLError as error:
        _fail(f"accelerated workflow YAML is invalid: {error}")
    if not isinstance(quality, dict) or not isinstance(nightly, dict):
        _fail("accelerated workflow document is not a mapping")
    expected_quality_trigger = {
        "pull_request": {
            "types": [
                "opened",
                "synchronize",
                "reopened",
                "ready_for_review",
                "converted_to_draft",
            ]
        },
        "push": {"branches": ["main"]},
    }
    if quality.get(True) != expected_quality_trigger:
        _fail("quality workflow must use PR events and main-only push")
    expected_nightly_trigger = {
        "workflow_call": None,
        "schedule": [{"cron": "17 18 * * *"}],
        "workflow_dispatch": None,
    }
    if nightly.get(True) != expected_nightly_trigger:
        _fail("Nightly must be reusable, scheduled, and manually dispatchable")
    if nightly.get("concurrency") != {
        "group": "nightly-quality-${{ github.sha }}",
        "cancel-in-progress": False,
    }:
        _fail("Nightly must retain every exact-SHA stress run")
    if nightly.get("env", {}).get("CXXLENS_CI_TIER") != "stress":
        _fail("Nightly must declare the stress tier")
    nightly_jobs = _workflow_jobs(nightly, "nightly workflow")
    clean = nightly_jobs.get("clean-full")
    if not isinstance(clean, dict) or "run_gate.py stress" not in nightly_text:
        _fail("Nightly stress tier must retain the clean full gate")

    jobs = _workflow_jobs(quality, "quality workflow")
    for name in (
        "fast-gate",
        "build-test",
        "sqlite-store-v3-qualification",
        "quality-contracts",
        "agent-context",
        "install-consumer",
        "gcc-public-headers",
        "quality-evidence",
        "check-tier",
        "foundation-completion",
        "wave0-readiness",
        "g5-qualification",
        "full-tier",
        "nightly-quality",
        "release-evaluation",
        "release-qualification",
        "production-scope-closure",
    ):
        if name not in jobs:
            _fail(f"required accelerated CI job is missing: {name}")
    fast = _job(quality, "fast-gate")
    if _normalized_condition(fast.get("if")) != "github.event_name == 'pull_request' && github.event.pull_request.draft":
        _fail("fast tier must run only for draft pull requests")
    if "run_gate.py fast" not in quality_text:
        _fail("fast tier does not execute run_gate.py fast")
    ready_condition = "github.event_name != 'pull_request' || !github.event.pull_request.draft"
    for name in (
        "build-test",
        "sqlite-store-v3-qualification",
        "quality-contracts",
        "agent-context",
        "install-consumer",
        "gcc-public-headers",
        "quality-evidence",
    ):
        if _normalized_condition(_job(quality, name).get("if")) != ready_condition:
            _fail(f"check/full tier routing differs: {name}")
    quality_contracts = _job(quality, "quality-contracts")
    quality_steps = quality_contracts.get("steps")
    if not isinstance(quality_steps, list):
        quality_steps = []
    checkout_steps = [
        step
        for step in quality_steps
        if isinstance(step, dict)
        and isinstance(step.get("uses"), str)
        and step["uses"].startswith("actions/checkout@")
    ]
    if (
        len(checkout_steps) != 1
        or not isinstance(checkout_steps[0].get("with"), dict)
        or checkout_steps[0]["with"].get("fetch-depth") != 2
    ):
        _fail("quality-contracts public callable stable-ID check requires fetch-depth: 2")
    check = _job(quality, "check-tier")
    if _normalized_condition(check.get("if")) != "github.event_name == 'pull_request' && !github.event.pull_request.draft":
        _fail("check tier must close only ready pull requests")
    if check.get("needs") != [
        "build-test",
        "sqlite-store-v3-qualification",
        "quality-contracts",
        "agent-context",
        "install-consumer",
        "gcc-public-headers",
        "quality-evidence",
    ]:
        _fail("check tier evidence closure differs")
    required_contexts = manifest.get("required_status_checks", {}).get("contexts")
    if not isinstance(required_contexts, list) or "check-tier" not in required_contexts:
        _fail("check tier must be a required pull-request status context")
    if "sqlite-store-v3-qualification" not in check["needs"]:
        _fail("required check tier must depend on sqlite qualification")
    full = _job(quality, "full-tier")
    main_condition = "github.event_name == 'push' && github.ref == 'refs/heads/main'"
    if _normalized_condition(full.get("if")) != main_condition:
        _fail("full tier must run only for merged main")
    if full.get("needs") != ["g5-qualification", "sqlite-store-v3-qualification", "agent-context"]:
        _fail("full tier evidence closure differs")
    called = _job(quality, "nightly-quality")
    if _normalized_condition(called.get("if")) != main_condition:
        _fail("stress tier dispatch must run only for merged main")
    if called.get("needs") != ["full-tier"] or called.get("uses") != "./.github/workflows/nightly.yml":
        _fail("stress tier must be an exact-SHA reusable workflow dependency")

    agent_job = _job(quality, "agent-context")
    plan_step = _step(agent_job, "Generate exact-SHA #261 agent context")
    run = plan_step.get("run")
    for marker in (
        "check_ng_api_development_readiness.py plan",
        "--issue 261",
        '--expected-revision "${GITHUB_SHA}"',
        '--expected-tree "${SOURCE_TREE}"',
        PACKET_JSON_NAME,
        PACKET_MARKDOWN_NAME,
    ):
        if not isinstance(run, str) or marker not in run:
            _fail(f"#261 agent-context generation marker is missing: {marker}")

    evaluation = _job(quality, "release-evaluation")
    if evaluation.get("needs") != [
        "nightly-quality",
        "g5-qualification",
        "sqlite-store-v3-qualification",
    ]:
        _fail("release evaluation must depend on exact-SHA stress completion")
    evaluation_body = json.dumps(evaluation, ensure_ascii=False, sort_keys=True)
    for forbidden in ("sleep 30", "for attempt in", "gh api", "run-id", "nightly-run"):
        if forbidden in evaluation_body:
            _fail(f"release qualification polling is forbidden: {forbidden}")
    download = _step(evaluation, "Download exact-main Nightly evidence")
    if download != {
        "name": "Download exact-main Nightly evidence",
        "uses": "actions/download-artifact@fa0a91b85d4f404e444e00e005971372dc801d16",
        "with": {
            "name": "cxxlens-nightly-evidence-${{ github.sha }}",
            "path": "build/release-evaluation-nightly",
        },
    }:
        _fail("release evaluation must consume same-run exact-SHA Nightly evidence")

    tiers = manifest.get("ci_tiers")
    if not isinstance(tiers, dict) or tiers.get("owner_issue") != "#291":
        _fail("CI tier authority is missing from readiness")
    if list(tiers.get("tiers", {}).keys()) != ["fast", "check", "full", "stress"]:
        _fail("CI tier authority must define fast/check/full/stress in order")
    if tiers.get("qualification", {}).get("polling") != "forbidden":
        _fail("CI authority must forbid qualification polling")


def _legacy_projection(root: pathlib.Path, manifest: dict[str, Any]) -> None:
    quality = _project_legacy_setup(
        (root / QUALITY_PATH).read_text(encoding="utf-8")
    )
    nightly = _project_legacy_setup(
        (root / NIGHTLY_PATH).read_text(encoding="utf-8")
    )
    if _QUALITY_TRIGGER not in quality or _NIGHTLY_TRIGGER not in nightly:
        _fail("accelerated workflow trigger projection is unavailable")
    quality = quality.replace(_QUALITY_TRIGGER, _LEGACY_QUALITY_TRIGGER, 1)
    quality = quality.replace("fetch-depth: 0", "fetch-depth: 2")
    quality = quality.replace(_NEW_RELEASE_NEEDS, _LEGACY_RELEASE_NEEDS, 1)
    if _DIRECT_NIGHTLY_DOWNLOAD not in quality:
        _fail("same-run Nightly download projection is unavailable")
    quality = quality.replace(
        _DIRECT_NIGHTLY_DOWNLOAD,
        _LEGACY_NIGHTLY_LOOKUP_AND_DOWNLOAD,
        1,
    )
    nightly = nightly.replace(_NIGHTLY_TRIGGER, _LEGACY_NIGHTLY_TRIGGER, 1)
    nightly = nightly.replace(
        _NIGHTLY_CONCURRENCY,
        _LEGACY_NIGHTLY_CONCURRENCY,
        1,
    )
    with tempfile.TemporaryDirectory() as temporary:
        projected = pathlib.Path(temporary)
        (projected / QUALITY_PATH).parent.mkdir(parents=True, exist_ok=True)
        (projected / QUALITY_PATH).write_text(quality, encoding="utf-8")
        (projected / NIGHTLY_PATH).write_text(nightly, encoding="utf-8")
        legacy_manifest = copy.deepcopy(manifest)
        legacy_manifest["required_status_checks"]["contexts"] = [
            context
            for context in legacy_manifest["required_status_checks"]["contexts"]
            if context != "check-tier"
        ]
        _baseline_validate_workflow(projected, legacy_manifest)


def validate_workflow(root: pathlib.Path, manifest: dict[str, Any]) -> None:
    _validate_accelerated_workflow(root, manifest)
    _legacy_projection(root, manifest)


def validate_documents(root: pathlib.Path) -> dict[str, Any]:
    manifest = _baseline_validate_documents(root)
    validate_bounded_completion_contract(root)
    validate_demand_closure(root, manifest)
    return manifest


def authority_projection(manifest: dict[str, Any]) -> dict[str, Any]:
    use_case, packet = _product_contract(manifest)
    return {
        "product_contract": manifest["product_direction"]["contract"],
        "result_contract": manifest["product_direction"]["result_contract"],
        "use_case": use_case,
        "packet_template": packet,
        "ci_tiers": manifest["ci_tiers"],
    }


def build_agent_context_packet(
    root: pathlib.Path,
    manifest: dict[str, Any],
    revision: str,
    tree: str,
) -> dict[str, Any]:
    if HEX40.fullmatch(revision) is None or HEX40.fullmatch(tree) is None:
        _fail("agent context requires exact 40-hex revision and tree")
    use_case, template = _product_contract(manifest)
    packet = {
        "schema": "cxxlens.agent-context.v1",
        "packet_id": template["packet_id"],
        "issue": template["issue"],
        "use_case_id": template["use_case_id"],
        "consumer": use_case["consumer"],
        "goal": template["goal"],
        "expected_result_states": use_case["expected_result_states"],
        "capability_path": use_case["capability_path"],
        "exact_contract_ids": template["exact_contract_ids"],
        "authority_reading_set": template["authority_reading_set"],
        "allowed_write_paths": template["allowed_write_paths"],
        "required_evidence": template["required_evidence"],
        "known_design_feedback": template["known_design_feedback"],
        "constructibility": template["constructibility"],
        "forbidden_shortcuts": template["forbidden_shortcuts"],
        "completion_commands": template["completion_commands"],
        "blocked_reason": use_case["tracked_gap"]["reason_code"],
        "completion_plan": template["completion_plan"],
        "binding": {
            "revision": revision,
            "tree": tree,
            "manifest_path": MANIFEST_PATH.as_posix(),
            "manifest_file_digest": _file_digest(root / MANIFEST_PATH),
            "authority_projection_digest": _semantic_digest(authority_projection(manifest)),
            "stale_policy": "reject",
        },
    }
    packet["canonical_digest"] = _semantic_digest(packet)
    return packet


def validate_agent_context_packet(
    root: pathlib.Path,
    manifest: dict[str, Any],
    packet: dict[str, Any],
    revision: str,
    tree: str,
) -> None:
    expected = build_agent_context_packet(root, manifest, revision, tree)
    if packet != expected:
        _fail("agent-context packet is stale, malformed, or not machine-derived")


def render_agent_context_markdown(packet: dict[str, Any]) -> str:
    path = " -> ".join(row["id"] for row in packet["capability_path"])
    evidence = "\n".join(f"- {value}" for value in packet["required_evidence"])
    plan = "\n".join(
        f"{index}. {value}" for index, value in enumerate(packet["completion_plan"], 1)
    )
    reads = "\n".join(f"- `{value}`" for value in packet["authority_reading_set"])
    writes = "\n".join(f"- `{value}`" for value in packet["allowed_write_paths"])
    contracts = "\n".join(f"- `{value}`" for value in packet["exact_contract_ids"])
    feedback = "\n".join(f"- `{value}`" for value in packet["known_design_feedback"])
    shortcuts = "\n".join(f"- `{value}`" for value in packet["forbidden_shortcuts"])
    commands = "\n".join(f"- `{value}`" for value in packet["completion_commands"])
    expected_states = ", ".join(f"`{value}`" for value in packet["expected_result_states"])
    constructibility = json.dumps(
        packet["constructibility"], ensure_ascii=False, sort_keys=True, indent=2
    )
    binding = json.dumps(packet["binding"], ensure_ascii=False, sort_keys=True, indent=2)
    complete_packet = json.dumps(packet, ensure_ascii=False, sort_keys=True, indent=2)
    return (
        "# cxxlens issue #261 agent context\n\n"
        f"- Schema: `{packet['schema']}`\n"
        f"- Packet: `{packet['packet_id']}`\n"
        f"- Issue: `{packet['issue']}`\n"
        f"- Use case: `{packet['use_case_id']}`\n"
        f"- Consumer: `{packet['consumer']}`\n"
        f"- Goal: `{packet['goal']}`\n"
        f"- Expected result states: {expected_states}\n"
        f"- Revision: `{packet['binding']['revision']}`\n"
        f"- Tree: `{packet['binding']['tree']}`\n"
        f"- Authority digest: `{packet['binding']['authority_projection_digest']}`\n"
        f"- Packet digest: `{packet['canonical_digest']}`\n"
        f"- Blocked reason: `{packet['blocked_reason']}`\n\n"
        "## Capability path\n\n"
        f"`{path}`\n\n"
        "## Exact contract IDs\n\n"
        f"{contracts}\n\n"
        "## Minimum authority reading set\n\n"
        f"{reads}\n\n"
        "## Allowed write paths\n\n"
        f"{writes}\n\n"
        "## Required evidence\n\n"
        f"{evidence}\n\n"
        "## Known design feedback\n\n"
        f"{feedback}\n\n"
        "## Constructibility\n\n"
        "```json\n"
        f"{constructibility}\n"
        "```\n\n"
        "## Forbidden shortcuts\n\n"
        f"{shortcuts}\n\n"
        "## Completion plan\n\n"
        f"{plan}\n\n"
        "## Completion commands\n\n"
        f"{commands}\n\n"
        "## Exact binding\n\n"
        "```json\n"
        f"{binding}\n"
        "```\n\n"
        "## Complete packet fields\n\n"
        "The following canonical JSON block mirrors every field in the paired packet.\n\n"
        "```json\n"
        f"{complete_packet}\n"
        "```\n"
    )


def _packet_paths(evidence_dir: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
    json_paths = sorted(evidence_dir.rglob(PACKET_JSON_NAME))
    markdown_paths = sorted(evidence_dir.rglob(PACKET_MARKDOWN_NAME))
    if len(json_paths) != 1 or len(markdown_paths) != 1:
        _fail(
            "Wave 0 requires exactly one #261 agent-context JSON/Markdown pair: "
            f"json={len(json_paths)}, markdown={len(markdown_paths)}"
        )
    if json_paths[0].parent != markdown_paths[0].parent:
        _fail("#261 agent-context JSON and Markdown are not from one artifact")
    return json_paths[0], markdown_paths[0]


def build_report(
    root: pathlib.Path,
    manifest: dict[str, Any],
    evidence_dir: pathlib.Path,
    run_url: str,
    ci_jobs: list[str],
    generated_at: str,
    expected_revision: str,
) -> dict[str, Any]:
    baseline_current_git_state = _baseline.current_git_state
    _baseline.current_git_state = current_git_state
    try:
        report = _baseline_build_report(
            root,
            manifest,
            evidence_dir,
            run_url,
            ci_jobs,
            generated_at,
            expected_revision,
        )
    finally:
        _baseline.current_git_state = baseline_current_git_state
    json_path, markdown_path = _packet_paths(evidence_dir)
    packet = json.loads(json_path.read_text(encoding="utf-8"))
    git = report["git"]
    validate_agent_context_packet(root, manifest, packet, git["revision"], git["tree"])
    if markdown_path.read_text(encoding="utf-8") != render_agent_context_markdown(packet):
        _fail("#261 agent-context Markdown differs from the canonical packet")
    report["evidence_artifacts"].extend(
        _baseline.file_rows([json_path, markdown_path], evidence_dir)
    )
    report["evidence_artifacts"] = sorted(
        {row["path"]: row for row in report["evidence_artifacts"]}.values(),
        key=lambda row: row["path"],
    )
    return report


def _plan(arguments: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("plan")
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--issue", type=int, required=True)
    parser.add_argument("--expected-revision", required=True)
    parser.add_argument("--expected-tree", required=True)
    parser.add_argument("--output-json", type=pathlib.Path, required=True)
    parser.add_argument("--output-markdown", type=pathlib.Path, required=True)
    parsed = parser.parse_args(arguments)
    root = parsed.root.resolve()
    try:
        if parsed.issue != 261:
            _fail(f"unknown agent-context issue: {parsed.issue}")
        manifest = validate_documents(root)
        actual_revision = _baseline.git_output(root, "rev-parse", "HEAD")
        actual_tree = _baseline.git_output(root, "rev-parse", "HEAD^{tree}")
        if (parsed.expected_revision, parsed.expected_tree) != (actual_revision, actual_tree):
            _fail("agent-context revision/tree binding is stale")
        packet = build_agent_context_packet(
            root, manifest, parsed.expected_revision, parsed.expected_tree
        )
        parsed.output_json.parent.mkdir(parents=True, exist_ok=True)
        parsed.output_markdown.parent.mkdir(parents=True, exist_ok=True)
        parsed.output_json.write_text(
            json.dumps(packet, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        parsed.output_markdown.write_text(
            render_agent_context_markdown(packet), encoding="utf-8"
        )
        print(f"wrote exact #261 agent context to {parsed.output_json}")
        return 0
    except (ReadinessError, OSError, json.JSONDecodeError, yaml.YAMLError) as error:
        print(f"agent-context generation failed: {error}", file=sys.stderr)
        return 1


# Make the frozen baseline's internal global lookups use the composed contracts.
def _current_git_state_for_baseline(root: pathlib.Path) -> dict[str, Any]:
    return current_git_state(root)


_baseline.validate_agent_authorization_contract = validate_direct_main_authorization_contract
_baseline.validate_workflow = validate_workflow
_baseline.validate_documents = validate_documents
_baseline.build_report = build_report
_baseline.current_git_state = _current_git_state_for_baseline


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "plan":
        return _plan(sys.argv[1:])
    return _baseline.main()


if __name__ == "__main__":
    raise SystemExit(main())
