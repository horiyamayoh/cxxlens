#!/usr/bin/env python3
"""Positive and fail-closed tests for development governance v2."""

from __future__ import annotations

import copy
import hashlib
import pathlib
import shutil
import sys
import tempfile
import unittest
from unittest import mock

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_development_decisions import (  # noqa: E402
    DecisionRegisterError,
    RECEIPTS,
    RECEIPT_SCHEMA,
    REGISTER,
    SCHEMA,
    authority_digest,
    canonical_review_comment,
    _verify_connected_receipt,
    validate,
)


class DevelopmentDecisionTest(unittest.TestCase):
    def copied_root(self, temporary: str) -> pathlib.Path:
        root = pathlib.Path(temporary)
        for relative in (REGISTER, SCHEMA, RECEIPTS, RECEIPT_SCHEMA):
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        register = yaml.safe_load((ROOT / REGISTER).read_text(encoding="utf-8"))
        for entry in register["decisions"]:
            for reference in entry["authority_refs"]:
                destination = root / reference
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text("authority\n", encoding="utf-8")
        return root

    @staticmethod
    def rewrite(root: pathlib.Path, mutate) -> None:
        path = root / REGISTER
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
        mutate(value)
        path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")

    def accepted_receipt_root(self, temporary: str) -> tuple[pathlib.Path, dict]:
        root = self.copied_root(temporary)
        register = yaml.safe_load((root / REGISTER).read_text(encoding="utf-8"))
        decision = register["decisions"][2]
        receipt_id = "review-receipt.source-closure.test.v1"
        decision["authority_status"] = "accepted"
        decision["review"].update({"outcome": "accepted", "reviewer": "codex-independent-source", "receipt_ids": [receipt_id], "references": ["https://github.com/horiyamayoh/cxxlens/issues/261#issuecomment-1"]})
        files = [{"path": path, "blob": "a" * 40} for path in decision["authority_refs"]]
        allowed = {str(REGISTER), str(RECEIPTS), "docs/design/SHA256SUMS", "schemas/cxxlens_ng_work_units.yaml"}
        allowed.update(path for path in decision["authority_refs"] if path.startswith("docs/design/adr/") or path.startswith("schemas/"))
        review_output = "ACCEPT\nP0=0 P1=0 P2=1\nP2-DOC-LIMIT"
        receipt = {"id": receipt_id, "decision_id": decision["id"], "owner_issue": "#261", "candidate_commit": "b" * 40, "candidate_tree": "c" * 40, "candidate_git_author_email": "owner@example.com", "candidate_github_login": "candidate-owner", "authority_files": files, "authority_digest": authority_digest(files), "comment_url": "https://github.com/horiyamayoh/cxxlens/issues/261#issuecomment-1", "comment_body_sha256": "sha256:" + "d" * 64, "comment_author_login": "independent-reviewer", "author": "repository-owner", "reviewer": "codex-independent-source", "reviewer_github_login": "independent-reviewer", "reviewer_provenance": "isolated-read-only-codex-exec", "reviewer_session": "12345678-1234-1234-1234-123456789abc", "reviewer_invocation": "codex-exec-ephemeral-sandbox-read-only", "review_output": review_output, "review_output_sha256": "sha256:" + hashlib.sha256(review_output.encode()).hexdigest(), "verdict": "accepted", "findings": {"p0": 0, "p1": 0, "p2": 1}, "finding_ids": ["P2-DOC-LIMIT"], "verification_limits": ["production qualification not executed"], "connected_verification": {"status": "verified", "run_id": 1, "run_url": "https://github.com/horiyamayoh/cxxlens/actions/runs/1", "run_commit": "b" * 40, "workflow_id": 1, "workflow_path": ".github/workflows/autonomy-fast.yml", "workflow_name": "Autonomy fast", "event": "push", "conclusion": "success"}, "acceptance": {"status": "committed", "derivation": "first-descendant-containing-receipt", "allowed_changed_paths": sorted(allowed)}}
        (root / REGISTER).write_text(yaml.safe_dump(register, sort_keys=False), encoding="utf-8")
        document = yaml.safe_load((root / RECEIPTS).read_text(encoding="utf-8"))
        document["receipts"] = [receipt]
        (root / RECEIPTS).write_text(yaml.safe_dump(document, sort_keys=False), encoding="utf-8")
        return root, receipt

    def test_repository_register_is_valid(self) -> None:
        validate(ROOT)

    def test_duplicate_decision_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["decisions"].append(copy.deepcopy(value["decisions"][0])))
            with self.assertRaisesRegex(DecisionRegisterError, "duplicate decision IDs"):
                validate(root, verify_git=False)

    def test_high_risk_self_review_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["decisions"][0]["review"].update({"mode": "self", "outcome": "not-required"}))
            with self.assertRaisesRegex(DecisionRegisterError, "independent review"):
                validate(root, verify_git=False)

    def test_rejecting_review_is_preserved_as_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["decisions"][0]["review"].__setitem__("outcome", "pending"))
            with self.assertRaisesRegex(DecisionRegisterError, "rewritten to pending"):
                validate(root, verify_git=False)

    def test_accepted_authority_without_receipt_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            def mutate(value) -> None:
                value["decisions"][2]["authority_status"] = "accepted"
                value["decisions"][2]["review"]["outcome"] = "accepted"
            self.rewrite(root, mutate)
            with self.assertRaisesRegex(DecisionRegisterError, "not atomic"):
                validate(root, verify_git=False)

    def test_active_unaccepted_authority_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["decisions"][2].__setitem__("activation", "active"))
            with self.assertRaisesRegex(DecisionRegisterError, "unaccepted authority is active"):
                validate(root, verify_git=False)

    def test_qualification_before_implementation_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["decisions"][2].__setitem__("qualification_status", "qualified"))
            with self.assertRaisesRegex(DecisionRegisterError, "qualification precedes implementation"):
                validate(root, verify_git=False)

    def test_receipt_authority_closure_cannot_be_claimant_selected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root, receipt = self.accepted_receipt_root(temporary)
            receipt["authority_files"].pop()
            receipt["authority_digest"] = authority_digest(receipt["authority_files"])
            document = yaml.safe_load((root / RECEIPTS).read_text(encoding="utf-8"))
            document["receipts"] = [receipt]
            (root / RECEIPTS).write_text(yaml.safe_dump(document, sort_keys=False), encoding="utf-8")
            with self.assertRaisesRegex(DecisionRegisterError, "authority closure"):
                validate(root, verify_git=False)

    def test_comment_projection_binds_verdict_census_findings_and_limits(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            _, receipt = self.accepted_receipt_root(temporary)
            accepted = canonical_review_comment(receipt)
            receipt["verdict"] = "rejected"
            receipt["findings"]["p1"] = 1
            receipt["finding_ids"].append("P1-COUNTEREXAMPLE")
            receipt["verification_limits"].append("connected CI unavailable")
            self.assertNotEqual(accepted, canonical_review_comment(receipt))
            self.assertNotEqual(hashlib.sha256(accepted.encode()).hexdigest(), hashlib.sha256(canonical_review_comment(receipt).encode()).hexdigest())

    def test_candidate_and_reviewer_github_identity_must_differ(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root, receipt = self.accepted_receipt_root(temporary)
            receipt["candidate_github_login"] = receipt["reviewer_github_login"]
            document = yaml.safe_load((root / RECEIPTS).read_text(encoding="utf-8"))
            document["receipts"] = [receipt]
            (root / RECEIPTS).write_text(
                yaml.safe_dump(document, sort_keys=False), encoding="utf-8"
            )
            with self.assertRaisesRegex(DecisionRegisterError, "GitHub identities"):
                validate(root, verify_git=False)

    def test_connected_verifier_authenticates_candidate_and_workflow_objects(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            _, receipt = self.accepted_receipt_root(temporary)
            body = canonical_review_comment(receipt)
            receipt["comment_body_sha256"] = "sha256:" + hashlib.sha256(
                body.encode("utf-8")
            ).hexdigest()
            responses = [
                {
                    "body": body,
                    "html_url": receipt["comment_url"],
                    "user": {"login": receipt["reviewer_github_login"]},
                },
                {
                    "sha": receipt["candidate_commit"],
                    "author": {"login": receipt["candidate_github_login"]},
                    "committer": {"login": receipt["candidate_github_login"]},
                },
                {
                    "id": 1,
                    "html_url": receipt["connected_verification"]["run_url"],
                    "head_sha": receipt["candidate_commit"],
                    "workflow_id": 1,
                    "name": "Autonomy fast",
                    "event": "push",
                    "conclusion": "success",
                },
                {
                    "id": 1,
                    "name": "Autonomy fast",
                    "path": ".github/workflows/autonomy-fast.yml",
                    "state": "active",
                },
            ]
            with mock.patch(
                "check_ng_development_decisions._github_json",
                side_effect=responses,
            ):
                _verify_connected_receipt(receipt, "token")
            responses[1]["author"]["login"] = "forged-candidate"
            with mock.patch(
                "check_ng_development_decisions._github_json",
                side_effect=responses,
            ):
                with self.assertRaisesRegex(DecisionRegisterError, "candidate identity"):
                    _verify_connected_receipt(receipt, "token")


if __name__ == "__main__":
    unittest.main()
