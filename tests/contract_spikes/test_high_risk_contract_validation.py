#!/usr/bin/env python3
"""Positive and fail-closed tests for the issue #52 validation gate."""

from __future__ import annotations

import copy
import json
import pathlib
import sys
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/quality"))

from check_high_risk_contract_validation import (  # noqa: E402
    ValidationError,
    validate,
)


def load_yaml(path: str) -> dict:
    return yaml.safe_load((ROOT / path).read_text(encoding="utf-8"))


class HighRiskContractValidationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.document = load_yaml("schemas/cxxlens_high_risk_contract_validation.yaml")
        cls.schema = load_yaml("schemas/cxxlens_high_risk_contract_validation.schema.yaml")
        cls.candidates = load_yaml("schemas/cxxlens_package_contract_candidates.yaml")
        cls.catalog = load_yaml("schemas/cxxlens_public_api_contract.yaml")
        cls.evidence = json.loads(
            (ROOT / "tests/contract_spikes/high_risk_validation_evidence.json").read_text(encoding="utf-8")
        )

    def check(self, document: dict, evidence: dict | None = None) -> None:
        validate(
            document,
            self.schema,
            self.candidates,
            self.catalog,
            evidence or self.evidence,
            ROOT,
            reproduce=False,
        )

    def assert_invalid(self, document: dict, code: str, evidence: dict | None = None) -> None:
        with self.assertRaisesRegex(ValidationError, code):
            self.check(document, evidence)

    def test_positive_all_candidates_domains_and_reproducible_evidence(self) -> None:
        self.check(self.document)
        self.assertEqual(len(self.document["candidate_snapshot"]), 9)
        self.assertEqual(len(self.document["domains"]), 7)
        self.assertEqual(self.evidence["summary"], {"validated": 7, "validated_with_change": 0, "rejected": 0})

    def test_candidate_fingerprint_drift_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["candidate_snapshot"][0]["fingerprint"] = "sha256:" + "0" * 64
        self.assert_invalid(document, "high-risk.candidate-drift")

    def test_missing_ambiguous_fixture_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        del document["domains"][0]["fixtures"]["ambiguous"]
        self.assert_invalid(document, "high-risk.schema")

    def test_shell_command_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["domains"][0]["commands"][0]["argv"] = ["sh", "-c", "echo unsafe"]
        self.assert_invalid(document, "high-risk.shell-command")

    def test_production_artifact_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["domains"][0]["retained_artifacts"] = ["src/graph/virtual_candidate_resolver.cpp"]
        self.assert_invalid(document, "high-risk.production-artifact")

    def test_unresolved_result_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["domains"][0]["result"] = "validated-with-change"
        document["domains"][0]["contract_changes"] = ["pending owner update"]
        self.assert_invalid(document, "high-risk.unresolved-result")

    def test_evidence_digest_drift_is_rejected(self) -> None:
        evidence = copy.deepcopy(self.evidence)
        evidence["results"]["graph"]["metrics"]["edge_count"] += 1
        self.assert_invalid(self.document, "high-risk.evidence-digest", evidence)


if __name__ == "__main__":
    unittest.main()
