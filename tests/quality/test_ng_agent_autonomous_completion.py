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

    def _evidence(self, outcome: str = "completed") -> dict:
        authority = metric._authority(ROOT)
        scenarios = metric._scenario_set(metric._catalog(ROOT))
        rows = []
        for scenario in scenarios:
            row = {"scenario_id": scenario["scenario_id"], "outcome": outcome}
            if outcome == "completed":
                row.update(
                    {
                        "bounded_completion": True,
                        "context_digest": "sha256:" + "1" * 64,
                        "command_digest": "sha256:" + "2" * 64,
                        "receipt_digest": "sha256:" + "3" * 64,
                    }
                )
            elif outcome == "safe-stop":
                row.update(
                    {
                        "reason_code": "missing-evidence",
                        "completion_plan_digest": "sha256:" + "4" * 64,
                    }
                )
            else:
                row.update(
                    {
                        "reason_code": "execution-failed",
                        "receipt_digest": "sha256:" + "5" * 64,
                    }
                )
            rows.append(row)
        return {
            "schema": "cxxlens.agent-autonomous-completion-evidence.v1",
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
        evidence["scenarios"][0].pop("receipt_digest")
        with self.assertRaisesRegex(metric.AgentAutonomousCompletionError, "receipt_digest"):
            metric._report(ROOT, evidence=evidence)


if __name__ == "__main__":
    unittest.main()
