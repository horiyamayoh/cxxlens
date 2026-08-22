#!/usr/bin/env python3
"""Contract tests for the receipt-bound #277 autonomous completion metric."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_agent_autonomous_completion as metric  # noqa: E402


class AgentAutonomousCompletionMetricTests(unittest.TestCase):
    def test_without_execution_receipts_is_not_evaluated(self) -> None:
        report = metric._report(ROOT, evidence=None)
        metric.validate_report(ROOT, report)
        self.assertEqual(report["metric"], "agent-autonomous-completion-rate")
        self.assertEqual(report["status"], "not-evaluated")
        self.assertIsNone(report["value_percent"])
        self.assertEqual(report["population"]["denominator"], 9)
        self.assertEqual(report["population"]["not_evaluated"], 9)
        self.assertEqual(report["qualification"], "not-qualification-evidence")
        self.assertEqual(report["provenance"]["evidence_source"], "none")

    def test_not_evaluated_report_cannot_claim_receipt_provenance(self) -> None:
        report = metric._report(ROOT, evidence=None)
        report["provenance"]["evidence_source"] = "provided-receipts"
        candidate = copy.deepcopy(report)
        candidate.pop("canonical_digest")
        report["canonical_digest"] = metric._digest_object(candidate)
        with self.assertRaisesRegex(metric.AgentAutonomousCompletionError, "evidence source"):
            metric.validate_report(ROOT, report)

    def test_not_evaluated_receipt_census_stays_unqualified(self) -> None:
        authority = metric._authority(ROOT)
        evidence = {
            "schema": "cxxlens.agent-autonomous-completion-evidence.v2",
            "authority": {
                key: authority[key] for key in ("revision", "tree", "catalog_digest")
            },
            "scenarios": [
                {"scenario_id": scenario["scenario_id"], "outcome": "not-evaluated"}
                for scenario in metric._scenario_set(metric._catalog(ROOT))
            ],
        }
        report = metric._report(ROOT, evidence=evidence)
        metric.validate_report(ROOT, report)
        self.assertEqual(report["status"], "not-evaluated")
        self.assertIsNone(report["value_percent"])
        self.assertEqual(report["evidence_disposition"], "execution-receipts-required")
        self.assertEqual(report["provenance"]["evidence_source"], "none")

    def _evidence(self, outcome: str = "completed") -> dict:
        authority = metric._authority(ROOT)
        scenarios = metric._scenario_set(metric._catalog(ROOT))
        rows = []
        for scenario in scenarios:
            scenario_id = scenario["scenario_id"]
            reason_code = {
                "completed": "none",
                "failed": "execution-failed",
                "safe-stop": "missing-evidence",
            }[outcome]
            completion_plan = (
                ["Acquire the missing scenario evidence and rerun the exact command."]
                if outcome == "safe-stop"
                else []
            )
            terminal_state = "exited" if outcome != "safe-stop" else "not-launched"
            exit_status = 0 if outcome == "completed" else (1 if outcome == "failed" else None)
            witness = {
                "schema": "cxxlens.agent-autonomous-completion-execution-witness.v1",
                "scenario_id": scenario_id,
                "authority": {
                    key: authority[key]
                    for key in ("revision", "tree", "catalog_digest")
                },
                "context": metric.capability.build_resolution(ROOT, scenario_id),
                "command": {
                    "argv": ["cxxlens-agent-eval", "--scenario", scenario_id],
                    "working_directory": "repository-root",
                    "environment": [],
                },
                "process": {
                    "terminal_state": terminal_state,
                    "exit_status": exit_status,
                    "signal": None,
                    "stdout": {
                        "byte_count": 0,
                        "digest": metric._digest_bytes(b""),
                        "complete": True,
                    },
                    "stderr": {
                        "byte_count": 0,
                        "digest": metric._digest_bytes(b""),
                        "complete": True,
                    },
                },
                "result": {
                    "outcome": outcome,
                    "bounded_completion": outcome == "completed",
                    "reason_code": reason_code,
                    "completion_plan": completion_plan,
                },
            }
            normalized = metric._normalize_execution_witness(
                ROOT,
                witness,
                authority,
                scenario,
                outcome,
                input_context=True,
            )
            row = {
                "scenario_id": scenario_id,
                "outcome": outcome,
                "bounded_completion": outcome == "completed",
                "context_digest": normalized["context"]["digest"],
                "command_digest": metric._digest_object(normalized["command"]),
                "receipt_digest": metric._digest_object(normalized),
                "execution_witness": witness,
            }
            if outcome in {"failed", "safe-stop"}:
                row["reason_code"] = reason_code
            if outcome == "safe-stop":
                row["completion_plan_digest"] = metric._digest_object(completion_plan)
            rows.append(row)
        return {
            "schema": "cxxlens.agent-autonomous-completion-evidence.v2",
            "authority": {
                key: authority[key] for key in ("revision", "tree", "catalog_digest")
            },
            "scenarios": rows,
        }

    def test_exact_completed_receipts_produce_a_measured_rate(self) -> None:
        report = metric._report(ROOT, evidence=self._evidence())
        metric.validate_report(ROOT, report)
        self.assertEqual(report["status"], "evaluated")
        self.assertEqual(report["value_percent"], 100.0)
        self.assertEqual(report["population"]["completed"], 9)

    def test_safe_stop_is_counted_and_not_promoted_to_completion(self) -> None:
        report = metric._report(ROOT, evidence=self._evidence("safe-stop"))
        metric.validate_report(ROOT, report)
        self.assertEqual(report["value_percent"], 0.0)
        self.assertEqual(report["population"]["safe_stop"], 9)

    def test_duplicate_or_stale_receipt_census_is_rejected(self) -> None:
        evidence = self._evidence()
        evidence["scenarios"].append(copy.deepcopy(evidence["scenarios"][0]))
        with self.assertRaisesRegex(metric.AgentAutonomousCompletionError, "census"):
            metric._report(ROOT, evidence=evidence)
        evidence = self._evidence()
        evidence["authority"]["tree"] = "0" * 40
        with self.assertRaisesRegex(metric.AgentAutonomousCompletionError, "stale"):
            metric._report(ROOT, evidence=evidence)

    def test_completed_without_receipt_witness_is_rejected(self) -> None:
        evidence = self._evidence()
        evidence["scenarios"][0].pop("execution_witness")
        with self.assertRaisesRegex(metric.AgentAutonomousCompletionError, "execution witness"):
            metric._report(ROOT, evidence=evidence)

    def test_digest_shaped_synthetic_completed_rows_are_rejected(self) -> None:
        authority = metric._authority(ROOT)
        rows = [
            {
                "scenario_id": scenario["scenario_id"],
                "outcome": "completed",
                "bounded_completion": True,
                "context_digest": "sha256:" + "1" * 64,
                "command_digest": "sha256:" + "2" * 64,
                "receipt_digest": "sha256:" + "3" * 64,
            }
            for scenario in metric._scenario_set(metric._catalog(ROOT))
        ]
        evidence = {
            "schema": "cxxlens.agent-autonomous-completion-evidence.v2",
            "authority": {
                key: authority[key] for key in ("revision", "tree", "catalog_digest")
            },
            "scenarios": rows,
        }
        with self.assertRaisesRegex(metric.AgentAutonomousCompletionError, "execution witness"):
            metric._report(ROOT, evidence=evidence)

    def test_context_command_and_receipt_digests_are_derived_from_witness(self) -> None:
        for field, message in (
            ("context_digest", "context digest"),
            ("command_digest", "command digest"),
            ("receipt_digest", "receipt digest"),
        ):
            with self.subTest(field=field):
                evidence = self._evidence()
                evidence["scenarios"][0][field] = "sha256:" + "f" * 64
                with self.assertRaisesRegex(metric.AgentAutonomousCompletionError, message):
                    metric._report(ROOT, evidence=evidence)

    def test_checked_report_rejects_removed_or_tampered_witness(self) -> None:
        report = metric._report(ROOT, evidence=self._evidence())
        report["outcomes"][0].pop("execution_witness")
        candidate = copy.deepcopy(report)
        candidate.pop("canonical_digest")
        report["canonical_digest"] = metric._digest_object(candidate)
        with self.assertRaisesRegex(metric.AgentAutonomousCompletionError, "execution_witness"):
            metric.validate_report(ROOT, report)

        report = metric._report(ROOT, evidence=self._evidence())
        report["outcomes"][0]["execution_witness"]["process"]["exit_status"] = 7
        candidate = copy.deepcopy(report)
        candidate.pop("canonical_digest")
        report["canonical_digest"] = metric._digest_object(candidate)
        with self.assertRaisesRegex(metric.AgentAutonomousCompletionError, "successful"):
            metric.validate_report(ROOT, report)


if __name__ == "__main__":
    unittest.main()
