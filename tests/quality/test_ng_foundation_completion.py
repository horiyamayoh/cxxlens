#!/usr/bin/env python3
"""Positive and fail-closed tests for the NG foundation completion gate."""

from __future__ import annotations

import copy
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_foundation_completion import (  # noqa: E402
    CompletionError,
    build_report,
    load_document,
    validate_audit_report,
    validate_documents,
)
import check_ng_foundation_audits as foundation_audits  # noqa: E402


class NgFoundationCompletionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = validate_documents(ROOT)
        cls.closed = {
            number: "closed" for number in cls.manifest["required_closed_issues"]
        }
        cls.closed.update({71: "closed", 56: "closed"})
        cls.git_state = {
            "revision": "1" * 40,
            "tree": "2" * 40,
            "branch": "main",
            "clean": True,
        }

    def report(self, **changes: object) -> dict:
        arguments = {
            "git_state": self.git_state,
            "issue_states": self.closed,
            "repository": "horiyamayoh/cxxlens",
            "run_url": "https://github.com/horiyamayoh/cxxlens/actions/runs/1",
            "ci_jobs": self.manifest["evidence"]["required_ci_jobs"],
            "generated_at": "2026-07-16T00:00:00Z",
        }
        arguments.update(changes)
        return build_report(ROOT, self.manifest, **arguments)

    def test_static_foundation_contract_is_complete(self) -> None:
        self.assertEqual(self.manifest["maturity"], "implemented")

    def test_report_binds_clean_revision_tree_and_zero_audits(self) -> None:
        report = self.report()
        self.assertEqual(report["result"], "passed")
        self.assertTrue(report["git"]["clean"])
        for entry in report["audits"].values():
            self.assertEqual(entry["revision"], self.git_state["revision"])
            self.assertEqual(entry["tree"], self.git_state["tree"])
            self.assertEqual(entry["count"], 0)
            self.assertEqual(entry["finding_ids"], [])

    def test_audit_target_digest_is_reproducible(self) -> None:
        entry = self.report()["audits"]["legacy_assets"]
        self.assertEqual(
            entry["target_digest"],
            foundation_audits.target_digest(ROOT, entry["targets"]),
        )

    def test_dirty_checkout_is_rejected(self) -> None:
        git_state = copy.deepcopy(self.git_state)
        git_state["clean"] = False
        with self.assertRaisesRegex(CompletionError, "clean main checkout"):
            self.report(git_state=git_state)

    def test_wrong_workflow_revision_is_rejected(self) -> None:
        with self.assertRaisesRegex(CompletionError, "workflow revision"):
            self.report(expected_revision="3" * 40)

    def test_open_child_issue_is_rejected(self) -> None:
        states = dict(self.closed)
        states[73] = "open"
        with self.assertRaisesRegex(CompletionError, "not closed"):
            self.report(issue_states=states)

    def test_synthetic_current_blocker_is_measured_and_rejected(self) -> None:
        states = dict(self.closed)
        states[999] = "open"
        with self.assertRaisesRegex(CompletionError, "github.issue.open:999"):
            self.report(issue_states=states)

    def test_missing_ci_job_is_rejected(self) -> None:
        with self.assertRaisesRegex(CompletionError, "CI job evidence differs"):
            self.report(ci_jobs=["build-test"])

    def test_report_schema_rejects_nonzero_audit(self) -> None:
        report = self.report()
        report["audits"]["legacy_assets"]["count"] = 1
        report["audits"]["legacy_assets"]["finding_ids"] = ["synthetic.finding"]
        schema = load_document(
            ROOT / "schemas/cxxlens_ng_foundation_completion_report.schema.yaml"
        )
        import jsonschema

        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(schema).validate(report)

    def audit_report(self) -> dict:
        report = self.report()
        return {
            "schema": "cxxlens.ng-foundation-audit-report.v1",
            "revision": self.git_state["revision"],
            "tree": self.git_state["tree"],
            "audits": report["audits"],
        }

    def test_subreport_revision_mismatch_is_rejected(self) -> None:
        report = self.audit_report()
        report["audits"]["legacy_assets"]["revision"] = "3" * 40
        with self.assertRaisesRegex(CompletionError, "audit revision differs"):
            validate_audit_report(ROOT, report, self.git_state)

    def test_subreport_count_finding_mismatch_is_rejected(self) -> None:
        report = self.audit_report()
        report["audits"]["legacy_assets"]["finding_ids"] = ["synthetic.finding"]
        with self.assertRaisesRegex(CompletionError, "count differs"):
            validate_audit_report(ROOT, report, self.git_state)

    def test_malformed_checker_identity_is_rejected(self) -> None:
        report = self.audit_report()
        report["audits"]["legacy_assets"]["checker"] = ""
        with self.assertRaisesRegex(CompletionError, "schema validation failed"):
            validate_audit_report(ROOT, report, self.git_state)

    def test_unowned_contract_is_measured(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            contract = root / "schemas/synthetic.yaml"
            contract.parent.mkdir(parents=True)
            contract.write_text("schema: synthetic\n", encoding="utf-8")
            _, findings = foundation_audits.unowned_contract_findings(
                root, ["schemas/synthetic.yaml"]
            )
        self.assertEqual(findings, ["contract.unowned:schemas/synthetic.yaml"])

    def test_documentation_checksum_drift_is_measured(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / "docs/design").mkdir(parents=True)
            (root / "synthetic.md").write_text("current\n", encoding="utf-8")
            (root / "docs/design/SHA256SUMS").write_text("stale\n", encoding="utf-8")
            with mock.patch.object(
                foundation_audits.verify_checksums,
                "PACKAGE_PATHS",
                ("synthetic.md",),
            ):
                _, findings = foundation_audits.documentation_drift_findings(root)
        self.assertEqual(
            findings,
            ["documentation.checksum-drift:docs/design/SHA256SUMS"],
        )


if __name__ == "__main__":
    unittest.main()
