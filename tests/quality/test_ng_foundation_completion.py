#!/usr/bin/env python3
"""Positive and fail-closed tests for the NG foundation completion gate."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_foundation_completion import (  # noqa: E402
    CompletionError,
    build_report,
    load_document,
    validate_documents,
)


class NgFoundationCompletionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = validate_documents(ROOT)
        cls.closed = {
            number: "closed" for number in cls.manifest["required_closed_issues"]
        }
        cls.closed.update({71: "open", 56: "open"})
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
        self.assertEqual(set(report["audits"].values()), {0})

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

    def test_missing_ci_job_is_rejected(self) -> None:
        with self.assertRaisesRegex(CompletionError, "CI job evidence differs"):
            self.report(ci_jobs=["build-test"])

    def test_report_schema_rejects_nonzero_audit(self) -> None:
        report = self.report()
        report["audits"]["legacy_assets"] = 1
        schema = load_document(
            ROOT / "schemas/cxxlens_ng_foundation_completion_report.schema.yaml"
        )
        import jsonschema

        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(schema).validate(report)


if __name__ == "__main__":
    unittest.main()
