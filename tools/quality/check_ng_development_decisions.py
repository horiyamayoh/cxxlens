#!/usr/bin/env python3
"""Fail-closed validation for development decisions and review receipts."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import urllib.error
import urllib.request
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
REGISTER = pathlib.Path("schemas/cxxlens_ng_development_decision_register.yaml")
SCHEMA = pathlib.Path("schemas/cxxlens_ng_development_decision_register.schema.yaml")
RECEIPTS = pathlib.Path("schemas/cxxlens_ng_development_review_receipts.yaml")
RECEIPT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_development_review_receipts.schema.yaml")
WIP_INVENTORY = pathlib.Path("schemas/cxxlens_ng_wip_inventory.yaml")
HIGH_RISK = {"contract", "invariant", "security", "compatibility", "irreversible", "resource-bound"}
REVIEW_REF = re.compile(r"^https://github\.com/horiyamayoh/cxxlens/issues/([1-9][0-9]*)#issuecomment-([1-9][0-9]*)$")
DIRECT_MAIN_DECISION_ID = "decision.delivery.direct-main"
DIRECT_MAIN_OWNER_ISSUES = frozenset({"#173"})
DIRECT_MAIN_CONTRACT_IDS = frozenset(
    {"development.delivery.v2", "development.review-receipt.v1"}
)
DIRECT_MAIN_CHOICE = "main-atomic-commit-post-update-ci"
# These files jointly define the direct-main route and its executable
# enforcement.  Keeping this census in the checker prevents a review receipt
# from accepting the prose contract while silently omitting a checker or
# schema that can change the route's meaning.
GOVERNANCE_ENFORCEMENT_SURFACES = frozenset(
    {
        "AGENTS.md",
        ".github/workflows/autonomy-fast.yml",
        ".github/workflows/autonomy-heavy.yml",
        ".github/workflows/autonomy-release-evaluation.yml",
        "docs/design/adr/0094-risk-tiered-goal-authorization.md",
        "docs/design/adr/0105-direct-main-review-and-release-governance.md",
        "docs/development/decision-review-register.md",
        "schemas/cxxlens_ng_autonomy_ci.yaml",
        "schemas/cxxlens_ng_autonomy_ci.schema.yaml",
        "schemas/cxxlens_ng_autonomy_constructibility.yaml",
        "schemas/cxxlens_ng_autonomy_constructibility.schema.yaml",
        "schemas/cxxlens_ng_development_decision_register.yaml",
        "schemas/cxxlens_ng_development_decision_register.schema.yaml",
        "schemas/cxxlens_ng_development_review_receipts.yaml",
        "schemas/cxxlens_ng_development_review_receipts.schema.yaml",
        "schemas/cxxlens_ng_wip_inventory.yaml",
        "schemas/cxxlens_ng_wip_inventory.schema.yaml",
        "schemas/cxxlens_ng_work_units.yaml",
        "schemas/cxxlens_ng_work_units.schema.yaml",
        "tools/quality/check_ng_autonomy_ci.py",
        "tools/quality/check_ng_autonomy_constructibility.py",
        "tools/quality/check_ng_development_decisions.py",
        "tools/quality/check_ng_wip_inventory.py",
        "tools/quality/check_ng_work_units.py",
    }
)
NON_NORMATIVE_AUTHORITY_COMPONENTS = frozenset(
    {"implementation-learning", "archive", "archives"}
)
EVIDENCE_ONLY_AUTHORITY_COMPONENTS = frozenset(
    {"artifacts", "evidence", "evidence-only", "reports", "work-unit-evidence"}
)


class DecisionRegisterError(ValueError):
    """A fail-closed development-governance violation."""


class UniqueKeyLoader(yaml.SafeLoader):
    """YAML loader that rejects duplicate mapping keys."""


def _mapping(loader: UniqueKeyLoader, node: yaml.MappingNode, deep: bool = False) -> dict[Any, Any]:
    loader.flatten_mapping(node)
    result: dict[Any, Any] = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in result:
            raise DecisionRegisterError(f"duplicate YAML key: {key}")
        result[key] = loader.construct_object(value_node, deep=deep)
    return result


UniqueKeyLoader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _mapping)


def _load(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = yaml.load(path.read_text(encoding="utf-8"), Loader=UniqueKeyLoader)
    except (OSError, UnicodeError, yaml.YAMLError) as error:
        raise DecisionRegisterError(f"cannot load {path}: {error}") from error
    if not isinstance(value, dict):
        raise DecisionRegisterError(f"expected mapping: {path}")
    return value


def _schema_validate(document: dict[str, Any], schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise DecisionRegisterError(f"{label} schema validation failed: {error.message}") from error


def _git(root: pathlib.Path, *arguments: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), *arguments],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise DecisionRegisterError(f"git authority unavailable: {' '.join(arguments)}") from error
    return result.stdout.strip()


def authority_digest(files: list[dict[str, str]]) -> str:
    canonical = json.dumps(
        sorted(files, key=lambda item: item["path"]),
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return "sha256:" + hashlib.sha256(canonical).hexdigest()


def _authority_source_violation(reference: str) -> str | None:
    components = set(pathlib.PurePosixPath(reference).parts)
    if components.intersection(NON_NORMATIVE_AUTHORITY_COMPONENTS):
        return "non-normative or archived"
    if components.intersection(EVIDENCE_ONLY_AUTHORITY_COMPONENTS):
        return "evidence-only"
    return None


def _is_direct_main_governance_decision(entry: dict[str, Any]) -> bool:
    return (
        entry["id"] == DIRECT_MAIN_DECISION_ID
        and set(entry["owner_issues"]) == DIRECT_MAIN_OWNER_ISSUES
        and entry["risk"] == "contract"
        and set(entry["contract_ids"]) == DIRECT_MAIN_CONTRACT_IDS
        and entry["choice"] == DIRECT_MAIN_CHOICE
        and entry["decision_status"] == "decided"
    )


def _validate_receipt_git(root: pathlib.Path, receipt: dict[str, Any]) -> None:
    commit = receipt["candidate_commit"]
    if _git(root, "show", "-s", "--format=%T", commit) != receipt["candidate_tree"]:
        raise DecisionRegisterError(f"review receipt candidate tree mismatch: {receipt['id']}")
    if _git(root, "show", "-s", "--format=%ae", commit) != receipt["candidate_git_author_email"]:
        raise DecisionRegisterError(f"review receipt candidate author mismatch: {receipt['id']}")
    for authority in receipt["authority_files"]:
        actual = _git(root, "rev-parse", f"{commit}:{authority['path']}")
        if actual != authority["blob"]:
            raise DecisionRegisterError(f"review receipt authority blob mismatch: {receipt['id']}:{authority['path']}")
    if authority_digest(receipt["authority_files"]) != receipt["authority_digest"]:
        raise DecisionRegisterError(f"review receipt authority digest mismatch: {receipt['id']}")


def _validate_acceptance_commit(root: pathlib.Path, receipt: dict[str, Any]) -> str | None:
    acceptance = receipt["acceptance"]
    if acceptance["status"] != "committed":
        return None
    candidate = receipt["candidate_commit"]
    if acceptance["derivation"] != "first-descendant-containing-receipt":
        raise DecisionRegisterError(f"committed acceptance lacks deterministic derivation: {receipt['id']}")
    if _git(root, "merge-base", candidate, "HEAD") != candidate:
        raise DecisionRegisterError(f"acceptance does not descend from candidate: {receipt['id']}")
    commits = _git(root, "rev-list", "--first-parent", "--reverse", f"{candidate}..HEAD").splitlines()
    commit = None
    for possible in commits:
        document = yaml.load(_git(root, "show", f"{possible}:{RECEIPTS}"), Loader=UniqueKeyLoader)
        if any(entry.get("id") == receipt["id"] for entry in document.get("receipts", [])):
            commit = possible
            break
    if commit is None:
        raise DecisionRegisterError(f"acceptance receipt has no descendant commit: {receipt['id']}")
    if _git(root, "rev-parse", f"{commit}^") != candidate:
        raise DecisionRegisterError(f"acceptance is not the immediate direct-main child: {receipt['id']}")
    if len(_git(root, "rev-list", "--parents", "-n", "1", commit).split()) != 2:
        raise DecisionRegisterError(f"acceptance commit is not single-parent: {receipt['id']}")
    changed = set(_git(root, "diff", "--name-only", candidate, commit).splitlines())
    allowed = set(acceptance["allowed_changed_paths"])
    if not changed or not changed <= allowed:
        raise DecisionRegisterError(f"acceptance commit is not status/receipt-only: {receipt['id']}")
    authority_paths = {item["path"] for item in receipt["authority_files"]}
    mandatory = {
        str(RECEIPTS), str(REGISTER), "schemas/cxxlens_ng_work_units.yaml",
        "docs/design/SHA256SUMS",
    }
    for path in authority_paths:
        before_text = _git(root, "show", f"{candidate}:{path}")
        if "- Status: Proposed for independent review" in before_text:
            mandatory.add(path)
            continue
        if path.endswith((".yaml", ".yml")):
            value = yaml.load(before_text, Loader=UniqueKeyLoader)
            if isinstance(value, dict) and value.get("maturity") == "proposed":
                mandatory.add(path)
    if not mandatory <= changed:
        missing = ",".join(sorted(mandatory - changed))
        raise DecisionRegisterError(
            f"acceptance omits mandatory status transition: {receipt['id']}:{missing}"
        )
    def authority_transition(before_value: Any, after_value: Any) -> bool:
        if before_value == after_value:
            return True
        if not isinstance(before_value, dict) or not isinstance(after_value, dict):
            return False
        expected = json.loads(json.dumps(before_value))
        if expected.get("maturity") != "proposed" or after_value.get("maturity") != "accepted":
            return False
        expected["maturity"] = "accepted"
        authority = expected.get("authority")
        if not isinstance(authority, dict) or "review" not in authority:
            return False
        authority["review"] = {
            "status": "complete",
            "reviewer": receipt["reviewer"],
            "ref": receipt["comment_url"],
            "exact_main_commit": receipt["candidate_commit"],
        }
        if "review_findings" in expected:
            expected["review_findings"] = {
                "status": "resolved",
                "exact_main_commit": receipt["candidate_commit"],
                "ref": receipt["comment_url"],
                "reviewer": receipt["reviewer"],
                "receipt_id": receipt["id"],
                "required_resolutions": expected["review_findings"]["required_resolutions"],
            }
        return expected == after_value

    def register_transition(before_value: dict[str, Any], after_value: dict[str, Any]) -> bool:
        expected = json.loads(json.dumps(before_value))
        targets = [
            decision
            for decision in expected.get("decisions", [])
            if decision.get("id") == receipt["decision_id"]
        ]
        if len(targets) != 1:
            return False
        target = targets[0]
        target["authority_status"] = "accepted"
        target["review"] = {
            "mode": "independent",
            "outcome": "accepted",
            "author": receipt["author"],
            "reviewer": receipt["reviewer"],
            "receipt_ids": [receipt["id"]],
            "references": [receipt["comment_url"]],
        }
        return expected == after_value

    def work_unit_projection(value: dict[str, Any]) -> dict[str, Any]:
        result = json.loads(json.dumps(value))
        for entry in result.get("entries", []):
            if set(entry.get("authority_sources", [])).intersection(authority_paths):
                entry.pop("authority_digest", None)
        return result

    def receipt_registry_transition(
        before_value: dict[str, Any], after_value: dict[str, Any]
    ) -> bool:
        expected = json.loads(json.dumps(before_value))
        if any(
            existing.get("id") == receipt["id"]
            for existing in expected.get("receipts", [])
        ):
            return False
        expected.get("receipts", []).append(receipt)
        return expected == after_value

    for path in changed:
        if path == "docs/design/SHA256SUMS":
            continue
        before = _git(root, "show", f"{candidate}:{path}")
        after = _git(root, "show", f"{commit}:{path}")
        if path == str(RECEIPTS):
            if not receipt_registry_transition(
                yaml.load(before, Loader=UniqueKeyLoader),
                yaml.load(after, Loader=UniqueKeyLoader),
            ):
                raise DecisionRegisterError(
                    f"acceptance rewrites review receipt history: {receipt['id']}"
                )
            continue
        if path == str(REGISTER):
            if not register_transition(yaml.load(before, Loader=UniqueKeyLoader), yaml.load(after, Loader=UniqueKeyLoader)):
                raise DecisionRegisterError(f"acceptance changes decision semantics: {receipt['id']}")
            continue
        if path == "schemas/cxxlens_ng_work_units.yaml":
            if work_unit_projection(yaml.load(before, Loader=UniqueKeyLoader)) != work_unit_projection(yaml.load(after, Loader=UniqueKeyLoader)):
                raise DecisionRegisterError(f"acceptance changes work-unit semantics: {receipt['id']}")
            continue
        if path not in authority_paths:
            raise DecisionRegisterError(f"acceptance changes non-authority path: {receipt['id']}:{path}")
        if path.endswith((".yaml", ".yml")):
            if not authority_transition(yaml.load(before, Loader=UniqueKeyLoader), yaml.load(after, Loader=UniqueKeyLoader)):
                raise DecisionRegisterError(f"acceptance changes authority semantics: {receipt['id']}:{path}")
        else:
            before_lines = before.splitlines()
            after_lines = after.splitlines()
            differences = [
                (left, right)
                for left, right in zip(before_lines, after_lines, strict=False)
                if left != right
            ]
            if len(before_lines) != len(after_lines) or differences != [
                ("- Status: Proposed for independent review", "- Status: Accepted")
            ]:
                raise DecisionRegisterError(f"acceptance changes authority prose: {receipt['id']}:{path}")
    return commit


def _validate_current_authority_projection(
    root: pathlib.Path, receipt: dict[str, Any], accepted_commit: str
) -> None:
    """Reject an accepted receipt after any post-accept authority drift.

    The candidate commit is intentionally not used as the comparison
    baseline: the status-only acceptance commit is allowed to change
    Proposed authority files.  The first descendant containing the receipt
    is the accepted projection; every current authority blob must remain
    byte-identical to that projection thereafter.
    """
    for authority in receipt["authority_files"]:
        path = authority["path"]
        try:
            accepted_blob = _git(root, "rev-parse", f"{accepted_commit}:{path}")
            current_blob = _git(root, "rev-parse", f"HEAD:{path}")
        except DecisionRegisterError as error:
            raise DecisionRegisterError(
                f"current authority projection drift: {receipt['id']}:{path}"
            ) from error
        if current_blob != accepted_blob:
            raise DecisionRegisterError(
                f"current authority projection drift: {receipt['id']}:{path}"
            )


def _github_json(url: str, token: str) -> dict[str, Any]:
    request = urllib.request.Request(
        url,
        headers={"Accept": "application/vnd.github+json", "Authorization": f"Bearer {token}", "X-GitHub-Api-Version": "2022-11-28"},
    )
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            value = json.load(response)
    except (OSError, urllib.error.URLError, json.JSONDecodeError) as error:
        raise DecisionRegisterError(f"connected GitHub verification unavailable: {error}") from error
    if not isinstance(value, dict):
        raise DecisionRegisterError("connected GitHub response is not an object")
    return value


def canonical_review_comment(receipt: dict[str, Any]) -> str:
    value = {
        "schema": "cxxlens.authenticated-review-comment.v1",
        "receipt_id": receipt["id"], "decision_id": receipt["decision_id"],
        "owner_issue": receipt["owner_issue"], "candidate_commit": receipt["candidate_commit"],
        "candidate_tree": receipt["candidate_tree"], "candidate_git_author_email": receipt["candidate_git_author_email"],
        "candidate_github_login": receipt["candidate_github_login"],
        "authority_digest": receipt["authority_digest"], "author": receipt["author"],
        "reviewer": receipt["reviewer"], "reviewer_github_login": receipt["reviewer_github_login"], "reviewer_provenance": receipt["reviewer_provenance"],
        "reviewer_session": receipt["reviewer_session"], "reviewer_invocation": receipt["reviewer_invocation"],
        "review_output": receipt["review_output"],
        "review_output_sha256": receipt["review_output_sha256"], "verdict": receipt["verdict"],
        "findings": receipt["findings"], "finding_ids": receipt["finding_ids"],
        "verification_limits": receipt["verification_limits"],
        "qualification": "production qualification not claimed",
    }
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _verify_connected_receipt(receipt: dict[str, Any], token: str) -> None:
    connected = receipt["connected_verification"]
    match = REVIEW_REF.fullmatch(receipt["comment_url"])
    if match is None or f"#{match.group(1)}" != receipt["owner_issue"]:
        raise DecisionRegisterError(f"review comment belongs to a foreign issue: {receipt['id']}")
    comment = _github_json(
        f"https://api.github.com/repos/horiyamayoh/cxxlens/issues/comments/{match.group(2)}",
        token,
    )
    body = comment.get("body")
    if not isinstance(body, str) or body != canonical_review_comment(receipt) or "sha256:" + hashlib.sha256(body.encode("utf-8")).hexdigest() != receipt["comment_body_sha256"]:
        raise DecisionRegisterError(f"GitHub review comment body mismatch: {receipt['id']}")
    if comment.get("html_url") != receipt["comment_url"] or comment.get("user", {}).get("login") != receipt["comment_author_login"] or receipt["comment_author_login"] != receipt["reviewer_github_login"]:
        raise DecisionRegisterError(f"GitHub review comment identity mismatch: {receipt['id']}")
    candidate = _github_json(
        "https://api.github.com/repos/horiyamayoh/cxxlens/commits/"
        f"{receipt['candidate_commit']}",
        token,
    )
    candidate_author = candidate.get("author", {}).get("login")
    candidate_committer = candidate.get("committer", {}).get("login")
    if (
        candidate.get("sha") != receipt["candidate_commit"]
        or candidate_author != receipt["candidate_github_login"]
        or receipt["reviewer_github_login"] in {candidate_author, candidate_committer}
    ):
        raise DecisionRegisterError(f"GitHub candidate identity mismatch: {receipt['id']}")
    run = _github_json(f"https://api.github.com/repos/horiyamayoh/cxxlens/actions/runs/{connected['run_id']}", token)
    if (run.get("id") != connected["run_id"] or run.get("html_url") != connected["run_url"] or
            run.get("head_sha") != receipt["candidate_commit"] or run.get("head_sha") != connected["run_commit"] or
            run.get("workflow_id") != connected["workflow_id"] or
            run.get("name") != connected["workflow_name"] or run.get("event") != connected["event"] or
            run.get("conclusion") != "success"):
        raise DecisionRegisterError(f"connected CI run mismatch: {receipt['id']}")
    workflow = _github_json(
        "https://api.github.com/repos/horiyamayoh/cxxlens/actions/workflows/"
        f"{connected['workflow_id']}",
        token,
    )
    if (
        workflow.get("id") != connected["workflow_id"]
        or workflow.get("name") != connected["workflow_name"]
        or workflow.get("path") != connected["workflow_path"]
        or workflow.get("state") != "active"
    ):
        raise DecisionRegisterError(f"connected CI workflow identity mismatch: {receipt['id']}")


def verify_connected(root: pathlib.Path, token: str) -> None:
    if not token:
        raise DecisionRegisterError("connected GitHub verification requires a token")
    validate(root)
    document = _load(root / RECEIPTS)
    for receipt in document["receipts"]:
        if receipt["connected_verification"]["status"] == "verified":
            _verify_connected_receipt(receipt, token)


def validate(root: pathlib.Path, *, verify_git: bool = True) -> dict[str, Any]:
    register = _load(root / REGISTER)
    schema = _load(root / SCHEMA)
    receipt_document = _load(root / RECEIPTS)
    receipt_schema = _load(root / RECEIPT_SCHEMA)
    raw_decisions = register.get("decisions")
    if isinstance(raw_decisions, list):
        raw_ids = [entry.get("id") for entry in raw_decisions if isinstance(entry, dict)]
        if len(raw_ids) != len(set(raw_ids)):
            raise DecisionRegisterError("duplicate decision IDs")
    raw_receipts = receipt_document.get("receipts")
    if isinstance(raw_receipts, list):
        raw_receipt_ids = [entry.get("id") for entry in raw_receipts if isinstance(entry, dict)]
        if len(raw_receipt_ids) != len(set(raw_receipt_ids)):
            raise DecisionRegisterError("duplicate review receipt IDs")
    _schema_validate(register, schema, "decision register")
    _schema_validate(receipt_document, receipt_schema, "review receipt")

    decisions = register["decisions"]
    identifiers = [entry["id"] for entry in decisions]
    required = {
        "decision.delivery.direct-main",
        "decision.release.composed-authority",
        "decision.source-closure.dedicated-transport",
        "decision.store.streaming-candidate",
        "decision.sqlite.read-mapping-lifecycle",
        "decision.sqlite.normalization-effect-profile",
        "decision.provider.ng1-after-source-closure-registry",
        "decision.sdk-doctor.capability-boundary",
        "decision.release.authenticated-evidence-handoff",
    }
    if len(identifiers) != len(set(identifiers)):
        raise DecisionRegisterError("duplicate decision IDs")
    if set(identifiers) != required:
        raise DecisionRegisterError("decision inventory differs from the repository set")

    receipts = receipt_document["receipts"]
    receipt_ids = [entry["id"] for entry in receipts]
    if len(receipt_ids) != len(set(receipt_ids)):
        raise DecisionRegisterError("duplicate review receipt IDs")
    by_receipt = {entry["id"]: entry for entry in receipts}
    referenced_receipts: set[str] = set()

    for entry in decisions:
        for reference in entry["authority_refs"]:
            violation = _authority_source_violation(reference)
            if violation is not None:
                raise DecisionRegisterError(
                    f"forbidden authority source path ({violation}): "
                    f"{entry['id']}:{reference}"
                )
            if not (root / reference).is_file():
                raise DecisionRegisterError(f"decision authority does not exist: {entry['id']}:{reference}")
        if entry["id"] == DIRECT_MAIN_DECISION_ID:
            missing = GOVERNANCE_ENFORCEMENT_SURFACES - set(entry["authority_refs"])
            if missing:
                raise DecisionRegisterError(
                    "direct-main governance authority closure missing enforcement "
                    f"surface: {','.join(sorted(missing))}"
                )
        review = entry["review"]
        if verify_git and review["outcome"] == "pending":
            for commit in _git(root, "log", "--first-parent", "--format=%H", "--", str(REGISTER)).splitlines()[1:]:
                historical = yaml.load(_git(root, "show", f"{commit}:{REGISTER}"), Loader=UniqueKeyLoader)
                prior = {item["id"]: item for item in historical.get("decisions", [])}.get(entry["id"])
                if prior and prior.get("review", {}).get("outcome") == "rejected":
                    raise DecisionRegisterError(f"rejected review was rewritten to pending: {entry['id']}")
        if entry["risk"] in HIGH_RISK and (review["mode"] != "independent" or review["outcome"] == "not-required"):
            raise DecisionRegisterError(f"high-risk decision lacks independent review: {entry['id']}")
        if review["outcome"] in {"rejected", "accepted"}:
            if review["reviewer"] in (None, review["author"]):
                raise DecisionRegisterError(f"reviewer is not independent: {entry['id']}")
            if not review["references"] or not all(REVIEW_REF.fullmatch(ref) for ref in review["references"]):
                raise DecisionRegisterError(f"review reference is not canonical: {entry['id']}")
        if review["outcome"] == "pending" and review["receipt_ids"]:
            raise DecisionRegisterError(f"pending review references a receipt: {entry['id']}")
        if review["outcome"] == "pending" and review["references"]:
            raise DecisionRegisterError(f"rejected review history was rewritten to pending: {entry['id']}")
        if entry["authority_status"] == "accepted" or review["outcome"] == "accepted":
            if entry["decision_status"] != "decided":
                raise DecisionRegisterError(
                    f"accepted authority requires decision_status=decided: {entry['id']}"
                )
            if entry["authority_status"] != "accepted" or review["outcome"] != "accepted" or not review["receipt_ids"]:
                raise DecisionRegisterError(f"accepted authority and review are not atomic: {entry['id']}")
        if entry["activation"] == "active-by-workflow-amendment" and not _is_direct_main_governance_decision(entry):
            raise DecisionRegisterError(
                "active-by-workflow-amendment is restricted to the exact "
                f"direct-main governance decision: {entry['id']}"
            )
        if entry["activation"] == "active" and entry["authority_status"] != "accepted":
            raise DecisionRegisterError(f"unaccepted authority is active: {entry['id']}")
        if entry["qualification_status"] == "qualified" and entry["implementation_status"] != "complete":
            raise DecisionRegisterError(f"qualification precedes implementation: {entry['id']}")
        for receipt_id in review["receipt_ids"]:
            receipt = by_receipt.get(receipt_id)
            if receipt is None or receipt["decision_id"] != entry["id"]:
                raise DecisionRegisterError(f"unknown or foreign review receipt: {entry['id']}:{receipt_id}")
            referenced_receipts.add(receipt_id)
            if receipt["owner_issue"] not in entry["owner_issues"]:
                raise DecisionRegisterError(f"review receipt owner issue mismatch: {receipt_id}")
            if {authority["path"] for authority in receipt["authority_files"]} != set(entry["authority_refs"]):
                raise DecisionRegisterError(f"review receipt authority closure mismatch: {receipt_id}")
            if receipt["author"] != review["author"] or receipt["reviewer"] != review["reviewer"]:
                raise DecisionRegisterError(f"review receipt identity mismatch: {receipt_id}")
            if receipt["candidate_github_login"] == receipt["reviewer_github_login"]:
                raise DecisionRegisterError(f"review receipt GitHub identities are not independent: {receipt_id}")
            if receipt["reviewer"] in {receipt["author"], receipt["candidate_git_author_email"]}:
                raise DecisionRegisterError(f"review receipt reviewer is not process-independent: {receipt_id}")
            expected_output_digest = "sha256:" + hashlib.sha256(
                receipt["review_output"].encode("utf-8")
            ).hexdigest()
            if receipt["review_output_sha256"] != expected_output_digest:
                raise DecisionRegisterError(f"review output digest mismatch: {receipt_id}")
            static_allowed = {str(REGISTER), str(RECEIPTS), "docs/design/SHA256SUMS", "schemas/cxxlens_ng_work_units.yaml"}
            static_allowed.update(path for path in entry["authority_refs"] if path.startswith("docs/design/adr/") or path.startswith("schemas/"))
            if set(receipt["acceptance"]["allowed_changed_paths"]) != static_allowed:
                raise DecisionRegisterError(f"review receipt acceptance allowlist mismatch: {receipt_id}")
            severity_ids = {severity: [item for item in receipt["finding_ids"] if item.startswith(severity.upper() + "-")] for severity in ("p0", "p1", "p2")}
            if any(len(severity_ids[severity]) != receipt["findings"][severity] for severity in severity_ids):
                raise DecisionRegisterError(f"review finding census/detail mismatch: {receipt_id}")
            if review["outcome"] == "accepted":
                if receipt["acceptance"]["status"] != "committed":
                    raise DecisionRegisterError(f"accepted review bypasses acceptance commit: {receipt_id}")
                if receipt["verdict"] != "accepted" or receipt["findings"]["p0"] or receipt["findings"]["p1"]:
                    raise DecisionRegisterError(f"accepted review has unresolved P0/P1: {receipt_id}")
                connected = receipt["connected_verification"]
                if connected["status"] != "verified" or connected["conclusion"] != "success" or connected["run_commit"] != receipt["candidate_commit"]:
                    raise DecisionRegisterError(f"accepted review lacks connected exact-candidate verification: {receipt_id}")
            if verify_git:
                _validate_receipt_git(root, receipt)
                accepted_commit = _validate_acceptance_commit(root, receipt)
                if review["outcome"] == "accepted":
                    if accepted_commit is None:
                        raise DecisionRegisterError(
                            f"accepted review has no accepted authority projection: {receipt_id}"
                        )
                    _validate_current_authority_projection(
                        root, receipt, accepted_commit
                    )
    if referenced_receipts != set(receipt_ids):
        raise DecisionRegisterError("orphan review receipt")

    wip_refs = [entry["ref"] for entry in register["preserved_wip"]]
    if len(wip_refs) != len(set(wip_refs)):
        raise DecisionRegisterError("duplicate preserved WIP refs")
    if verify_git:
        inventory = _load(root / WIP_INVENTORY)
        inventoried = {entry["ref"]: entry["head"] for entry in inventory.get("refs", [])}
        for entry in register["preserved_wip"]:
            result = subprocess.run(["git", "-C", str(root), "rev-parse", "--verify", entry["ref"]], capture_output=True, text=True)
            actual = result.stdout.strip() if result.returncode == 0 else inventoried.get(entry["ref"])
            if actual != entry["exact_head"]:
                raise DecisionRegisterError(f"preserved WIP head replaced: {entry['ref']}")
    return register


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check", "verify-connected"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--github-token", default=None)
    args = parser.parse_args()
    try:
        if args.command == "verify-connected":
            import os
            verify_connected(args.root.resolve(), args.github_token or os.environ.get("GITHUB_TOKEN", ""))
        else:
            validate(args.root.resolve())
    except DecisionRegisterError as error:
        print(f"development-decision-register: {error}", file=sys.stderr)
        return 1
    print("development-decision-register: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
