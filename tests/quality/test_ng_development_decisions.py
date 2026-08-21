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
    GOVERNANCE_ENFORCEMENT_SURFACES,
    RECEIPTS,
    RECEIPT_SCHEMA,
    REGISTER,
    SCHEMA,
    authority_digest,
    canonical_review_comment,
    reviewer_context_digest,
    _validate_current_authority_projection,
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
                if destination.exists():
                    continue
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
        receipt["reviewer_context_sha256"] = reviewer_context_digest(receipt)
        (root / REGISTER).write_text(yaml.safe_dump(register, sort_keys=False), encoding="utf-8")
        document = yaml.safe_load((root / RECEIPTS).read_text(encoding="utf-8"))
        document["receipts"] = [receipt]
        (root / RECEIPTS).write_text(yaml.safe_dump(document, sort_keys=False), encoding="utf-8")
        return root, receipt

    def test_repository_register_is_valid(self) -> None:
        validate(ROOT)

    def test_direct_main_authority_closure_requires_enforcement_surfaces(self) -> None:
        required_roots = (
            "AGENTS.md",
            "docs/design/adr/0094-risk-tiered-goal-authorization.md",
            "docs/design/adr/0105-direct-main-review-and-release-governance.md",
            ".github/workflows/nightly.yml",
            ".github/workflows/quality.yml",
            "schemas/cxxlens_ng_release_evidence_bundle.schema.yaml",
            "schemas/cxxlens_ng_release_evidence_selection.schema.yaml",
            "schemas/cxxlens_ng_release_qualification.yaml",
            "tools/quality/check_ng_release_evidence_bundle.py",
        )
        self.assertTrue(set(required_roots) <= GOVERNANCE_ENFORCEMENT_SURFACES)
        for missing in required_roots:
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as temporary:
                root = self.copied_root(temporary)

                def mutate(value) -> None:
                    decision = value["decisions"][0]
                    decision["authority_refs"].remove(missing)

                self.rewrite(root, mutate)
                with self.assertRaisesRegex(
                    DecisionRegisterError,
                    "direct-main governance authority closure missing enforcement surface",
                ):
                    validate(root, verify_git=False)

    def test_accepted_receipt_rejects_current_authority_projection_drift(self) -> None:
        receipt = {
            "id": "review-receipt.governance-drift.test.v1",
            "authority_files": [{"path": "authority.md", "blob": "a" * 40}],
        }

        def fake_git(_root: pathlib.Path, *arguments: str) -> str:
            if arguments == ("rev-parse", f"{'c' * 40}:authority.md"):
                return "a" * 40
            if arguments == ("rev-parse", "HEAD:authority.md"):
                return "b" * 40
            raise AssertionError(arguments)

        with mock.patch(
            "check_ng_development_decisions._git", side_effect=fake_git
        ):
            with self.assertRaisesRegex(
                DecisionRegisterError, "current authority projection drift"
            ):
                _validate_current_authority_projection(
                    pathlib.Path("/tmp/governance-test"), receipt, "c" * 40
                )

    def test_duplicate_decision_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["decisions"].append(copy.deepcopy(value["decisions"][0])))
            with self.assertRaisesRegex(DecisionRegisterError, "duplicate decision IDs"):
                validate(root, verify_git=False)

    def test_reviewer_session_and_artifact_reuse_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root, receipt = self.accepted_receipt_root(temporary)
            duplicate = copy.deepcopy(receipt)
            duplicate["id"] = "review-receipt.source-closure-duplicate-session.v1"
            document = yaml.safe_load((root / RECEIPTS).read_text(encoding="utf-8"))
            document["receipts"] = [receipt, duplicate]
            (root / RECEIPTS).write_text(
                yaml.safe_dump(document, sort_keys=False), encoding="utf-8"
            )
            with self.assertRaisesRegex(DecisionRegisterError, "duplicate reviewer session"):
                validate(root, verify_git=False)

        with tempfile.TemporaryDirectory() as temporary:
            root, receipt = self.accepted_receipt_root(temporary)
            duplicate = copy.deepcopy(receipt)
            duplicate["id"] = "review-receipt.source-closure-duplicate-artifact.v1"
            duplicate["reviewer_session"] = "22345678-1234-1234-1234-123456789abc"
            duplicate["reviewer_context_sha256"] = reviewer_context_digest(duplicate)
            document = yaml.safe_load((root / RECEIPTS).read_text(encoding="utf-8"))
            document["receipts"] = [receipt, duplicate]
            (root / RECEIPTS).write_text(
                yaml.safe_dump(document, sort_keys=False), encoding="utf-8"
            )
            with self.assertRaisesRegex(DecisionRegisterError, "duplicate review artifact"):
                validate(root, verify_git=False)

    def test_rejected_receipt_history_is_preserved_when_a_new_receipt_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root, receipt = self.accepted_receipt_root(temporary)
            previous = copy.deepcopy(receipt)
            previous["id"] = "review-receipt.source-closure-rejected-history.v1"
            previous["comment_url"] = "https://github.com/horiyamayoh/cxxlens/issues/261#issuecomment-2"
            previous["comment_body_sha256"] = "sha256:" + "e" * 64
            previous["reviewer"] = "codex-independent-previous"
            previous["reviewer_github_login"] = "previous-reviewer"
            previous["reviewer_session"] = "22345678-1234-1234-1234-123456789abc"
            previous["review_output"] = "REJECT\nP0=0 P1=1 P2=0\nP1-PRIOR"
            previous["review_output_sha256"] = "sha256:" + hashlib.sha256(
                previous["review_output"].encode()
            ).hexdigest()
            previous["verdict"] = "rejected"
            previous["findings"] = {"p0": 0, "p1": 1, "p2": 0}
            previous["finding_ids"] = ["P1-PRIOR"]
            previous["connected_verification"] = {
                "status": "pending",
                "run_id": None,
                "run_url": None,
                "run_commit": None,
                "workflow_id": None,
                "workflow_path": "pending",
                "workflow_name": "pending",
                "event": "pending",
                "conclusion": "pending",
            }
            previous["acceptance"] = {
                "status": "not-applicable",
                "derivation": "not-applicable",
                "allowed_changed_paths": [],
            }
            previous["reviewer_context_sha256"] = reviewer_context_digest(previous)

            register = yaml.safe_load((root / REGISTER).read_text(encoding="utf-8"))
            decision = next(
                item for item in register["decisions"] if item["id"] == receipt["decision_id"]
            )
            decision["review"]["receipt_ids"] = [previous["id"], receipt["id"]]
            decision["review"]["references"] = [
                previous["comment_url"], receipt["comment_url"]
            ]
            (root / REGISTER).write_text(
                yaml.safe_dump(register, sort_keys=False), encoding="utf-8"
            )
            document = yaml.safe_load((root / RECEIPTS).read_text(encoding="utf-8"))
            document["receipts"] = [previous, receipt]
            (root / RECEIPTS).write_text(
                yaml.safe_dump(document, sort_keys=False), encoding="utf-8"
            )
            validate(root, verify_git=False)

    def test_finding_ids_must_use_p0_p1_or_p2_severity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root, receipt = self.accepted_receipt_root(temporary)
            receipt["finding_ids"].append("P3-UNKNOWN")
            document = yaml.safe_load((root / RECEIPTS).read_text(encoding="utf-8"))
            document["receipts"] = [receipt]
            (root / RECEIPTS).write_text(
                yaml.safe_dump(document, sort_keys=False), encoding="utf-8"
            )
            with self.assertRaisesRegex(DecisionRegisterError, "review receipt schema validation failed"):
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

    def test_authority_sources_reject_learning_archive_and_evidence_paths(self) -> None:
        for reference in (
            "docs/development/implementation-learning/records/df-9999-test.md",
            "docs/archive/historical.md",
            "docs/development/work-unit-evidence/focused.json",
        ):
            with self.subTest(reference=reference), tempfile.TemporaryDirectory() as temporary:
                root = self.copied_root(temporary)
                path = root / reference
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("not authoritative\n", encoding="utf-8")

                def mutate(value) -> None:
                    value["decisions"][0]["authority_refs"].append(reference)

                self.rewrite(root, mutate)
                with self.assertRaisesRegex(
                    DecisionRegisterError,
                    "schema validation failed|forbidden authority source path",
                ):
                    validate(root, verify_git=False)

    def test_accepted_authority_requires_decided_decision(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root, _ = self.accepted_receipt_root(temporary)
            self.rewrite(
                root,
                lambda value: value["decisions"][2].__setitem__(
                    "decision_status", "undecided"
                ),
            )
            with self.assertRaisesRegex(
                DecisionRegisterError, "accepted authority requires decision_status=decided"
            ):
                validate(root, verify_git=False)

    def test_workflow_amendment_activation_is_restricted_to_exact_direct_main(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["decisions"][1].__setitem__(
                    "activation", "active-by-workflow-amendment"
                ),
            )
            with self.assertRaisesRegex(
                DecisionRegisterError, "active-by-workflow-amendment"
            ):
                validate(root, verify_git=False)

        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)

            def mutate(value) -> None:
                value["decisions"][0]["contract_ids"] = ["development.delivery.v3"]

            self.rewrite(root, mutate)
            with self.assertRaisesRegex(
                DecisionRegisterError, "active-by-workflow-amendment"
            ):
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

    def test_accepted_review_requires_connected_exact_candidate_verification(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root, receipt = self.accepted_receipt_root(temporary)
            receipt["connected_verification"] = {
                "status": "pending",
                "run_id": None,
                "run_url": None,
                "run_commit": None,
                "workflow_id": None,
                "workflow_path": "pending",
                "workflow_name": "pending",
                "event": "pending",
                "conclusion": "pending",
            }
            document = yaml.safe_load((root / RECEIPTS).read_text(encoding="utf-8"))
            document["receipts"] = [receipt]
            (root / RECEIPTS).write_text(
                yaml.safe_dump(document, sort_keys=False), encoding="utf-8"
            )
            with self.assertRaisesRegex(
                DecisionRegisterError, "connected exact-candidate verification"
            ):
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

    def test_reviewer_process_identity_must_differ(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root, receipt = self.accepted_receipt_root(temporary)
            receipt["reviewer"] = receipt["author"]
            receipt["reviewer_context_sha256"] = reviewer_context_digest(receipt)
            document = yaml.safe_load((root / RECEIPTS).read_text(encoding="utf-8"))
            document["receipts"] = [receipt]
            (root / RECEIPTS).write_text(
                yaml.safe_dump(document, sort_keys=False), encoding="utf-8"
            )
            with self.assertRaisesRegex(DecisionRegisterError, "schema validation failed|process-independent"):
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
