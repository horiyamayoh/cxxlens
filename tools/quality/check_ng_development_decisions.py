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


def _validate_acceptance_commit(root: pathlib.Path, receipt: dict[str, Any]) -> None:
    acceptance = receipt["acceptance"]
    if acceptance["status"] != "committed":
        return
    candidate = receipt["candidate_commit"]
    if acceptance["derivation"] != "first-descendant-containing-receipt":
        raise DecisionRegisterError(f"committed acceptance lacks deterministic derivation: {receipt['id']}")
    if _git(root, "merge-base", candidate, "HEAD") != candidate:
        raise DecisionRegisterError(f"acceptance does not descend from candidate: {receipt['id']}")
    commits = _git(root, "log", "--reverse", "--ancestry-path", "--format=%H", f"{candidate}..HEAD", "--", str(RECEIPTS)).splitlines()
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
    changed = set(_git(root, "diff", "--name-only", candidate, commit).splitlines())
    allowed = set(acceptance["allowed_changed_paths"])
    if not changed or not changed <= allowed:
        raise DecisionRegisterError(f"acceptance commit is not status/receipt-only: {receipt['id']}")


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
        "authority_digest": receipt["authority_digest"], "author": receipt["author"],
        "reviewer": receipt["reviewer"], "reviewer_provenance": receipt["reviewer_provenance"],
        "reviewer_session": receipt["reviewer_session"], "verdict": receipt["verdict"],
        "findings": receipt["findings"], "qualification": "production qualification not claimed",
    }
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def verify_connected(root: pathlib.Path, token: str) -> None:
    if not token:
        raise DecisionRegisterError("connected GitHub verification requires a token")
    validate(root)
    document = _load(root / RECEIPTS)
    for receipt in document["receipts"]:
        connected = receipt["connected_verification"]
        if connected["status"] != "verified":
            continue
        match = REVIEW_REF.fullmatch(receipt["comment_url"])
        if match is None or f"#{match.group(1)}" != receipt["owner_issue"]:
            raise DecisionRegisterError(f"review comment belongs to a foreign issue: {receipt['id']}")
        comment = _github_json(f"https://api.github.com/repos/horiyamayoh/cxxlens/issues/comments/{match.group(2)}", token)
        body = comment.get("body")
        if not isinstance(body, str) or body != canonical_review_comment(receipt) or "sha256:" + hashlib.sha256(body.encode("utf-8")).hexdigest() != receipt["comment_body_sha256"]:
            raise DecisionRegisterError(f"GitHub review comment body mismatch: {receipt['id']}")
        if comment.get("html_url") != receipt["comment_url"] or comment.get("user", {}).get("login") != receipt["comment_author_login"]:
            raise DecisionRegisterError(f"GitHub review comment identity mismatch: {receipt['id']}")
        run = _github_json(f"https://api.github.com/repos/horiyamayoh/cxxlens/actions/runs/{connected['run_id']}", token)
        if (run.get("id") != connected["run_id"] or run.get("html_url") != connected["run_url"] or
                run.get("head_sha") != receipt["candidate_commit"] or run.get("head_sha") != connected["run_commit"] or
                run.get("name") != connected["workflow_name"] or run.get("event") != connected["event"] or
                run.get("conclusion") != "success"):
            raise DecisionRegisterError(f"connected CI run mismatch: {receipt['id']}")


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
            if not (root / reference).is_file():
                raise DecisionRegisterError(f"decision authority does not exist: {entry['id']}:{reference}")
        review = entry["review"]
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
            if entry["authority_status"] != "accepted" or review["outcome"] != "accepted" or not review["receipt_ids"]:
                raise DecisionRegisterError(f"accepted authority and review are not atomic: {entry['id']}")
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
            if receipt["reviewer"] in {receipt["author"], receipt["comment_author_login"], receipt["candidate_git_author_email"]}:
                raise DecisionRegisterError(f"review receipt reviewer is not process-independent: {receipt_id}")
            static_allowed = {str(REGISTER), str(RECEIPTS), "docs/design/SHA256SUMS", "schemas/cxxlens_ng_work_units.yaml"}
            static_allowed.update(path for path in entry["authority_refs"] if path.startswith("docs/design/adr/") or path.startswith("schemas/"))
            if set(receipt["acceptance"]["allowed_changed_paths"]) != static_allowed:
                raise DecisionRegisterError(f"review receipt acceptance allowlist mismatch: {receipt_id}")
            if review["outcome"] == "accepted":
                if receipt["verdict"] != "accepted" or receipt["findings"]["p0"] or receipt["findings"]["p1"]:
                    raise DecisionRegisterError(f"accepted review has unresolved P0/P1: {receipt_id}")
                connected = receipt["connected_verification"]
                if connected["status"] != "verified" or connected["conclusion"] != "success" or connected["run_commit"] != receipt["candidate_commit"]:
                    raise DecisionRegisterError(f"accepted review lacks connected exact-candidate verification: {receipt_id}")
            if verify_git:
                _validate_receipt_git(root, receipt)
                _validate_acceptance_commit(root, receipt)
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
