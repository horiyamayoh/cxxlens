#!/usr/bin/env python3
"""Tests for exact NG1 provider qualification certificate binding."""

from __future__ import annotations

import copy
import hashlib
import json
import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_provider_ng1 import (  # noqa: E402
    CONTRACT,
    CONTRACT_SCHEMA,
    PROTOCOL,
    QUALIFICATION_REPORT_SCHEMA,
    VECTORS,
    VECTORS_SCHEMA,
    document_digest,
    load_yaml,
)
from check_ng_provider_ng1_qualification import (  # noqa: E402
    Ng1QualificationError,
    git_binding,
    validate_report,
)


class Ng1QualificationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = load_yaml(ROOT / CONTRACT)
        cls.case_outcomes = cls.contract["qualification"]["required_case_outcomes"]

    @staticmethod
    def expected_case(case_id: str, outcome: str) -> dict[str, str]:
        if outcome == "accepted":
            return {"id": case_id, "decision": "accepted"}
        if outcome.startswith("provider."):
            return {"id": case_id, "decision": "rejected", "reason_code": outcome}
        return {"id": case_id, "decision": "recovery", "outcome": outcome}

    @classmethod
    def report_for_current_tree(cls, provider_binary: pathlib.Path) -> dict[str, object]:
        revision, tree = git_binding(ROOT)
        provider_contract = load_yaml(ROOT / CONTRACT)
        cases = [
            cls.expected_case(case_id, outcome)
            for case_id, outcome in cls.case_outcomes.items()
        ]
        return {
            "schema": "cxxlens.provider-ng1-qualification.v1",
            "document_version": "1.0.0",
            "authority": {
                "contract": CONTRACT.as_posix(),
                "vectors": VECTORS.as_posix(),
                "decision_issue": "#233",
                "implementation_issue": "#183",
            },
            "binding": {
                "revision": revision,
                "tree": tree,
                "provider_binary_digest": "sha256:"
                + hashlib.sha256(provider_binary.read_bytes()).hexdigest(),
                "provider_binary_digest_source": "host-measured-executable-bytes",
                "provider_semantic_contract_digest": document_digest(provider_contract),
                "provider_semantic_contract_digest_source": "selected-contract-digest",
                "protocol_minor": 1,
                "protocol_contract_digest": document_digest(load_yaml(ROOT / PROTOCOL)),
                "hardening_contract_digest": document_digest(provider_contract),
                "hardening_contract_schema_digest": document_digest(
                    load_yaml(ROOT / CONTRACT_SCHEMA)
                ),
                "report_schema_digest": document_digest(
                    load_yaml(ROOT / QUALIFICATION_REPORT_SCHEMA)
                ),
                "vectors_digest": document_digest(load_yaml(ROOT / VECTORS)),
                "vectors_schema_digest": document_digest(
                    load_yaml(ROOT / VECTORS_SCHEMA)
                ),
            },
            "profiles": [
                {
                    "profile": profile,
                    "status": "green",
                    "evidence_digest": "sha256:" + evidence_digit * 64,
                    "cases": cases,
                }
                for profile, evidence_digit in (("static", "4"), ("shared", "5"))
            ],
            "status": "green",
        }

    def test_live_binding_and_measured_digests_are_required(self) -> None:
        provider_binary = pathlib.Path(sys.executable)
        report = self.report_for_current_tree(provider_binary)
        with tempfile.TemporaryDirectory(prefix="cxxlens-ng1-report-") as temporary:
            report_path = pathlib.Path(temporary) / "qualification.json"
            report_path.write_text(json.dumps(report), encoding="utf-8")
            validate_report(ROOT, report_path, provider_binary, ROOT / CONTRACT)

            stale_revision = copy.deepcopy(report)
            stale_revision["binding"]["revision"] = "0" * 40
            report_path.write_text(json.dumps(stale_revision), encoding="utf-8")
            with self.assertRaisesRegex(
                Ng1QualificationError, "report revision differs from the exact Git HEAD"
            ):
                validate_report(ROOT, report_path, provider_binary, ROOT / CONTRACT)

            stale_binary = copy.deepcopy(report)
            stale_binary["binding"]["provider_binary_digest"] = "sha256:" + "0" * 64
            report_path.write_text(json.dumps(stale_binary), encoding="utf-8")
            with self.assertRaisesRegex(
                Ng1QualificationError,
                "provider binary digest differs from host-measured executable bytes",
            ):
                validate_report(ROOT, report_path, provider_binary, ROOT / CONTRACT)


if __name__ == "__main__":
    unittest.main()
