#!/usr/bin/env python3
"""Tests for the real, receipt-bound #277 golden command runner."""

from __future__ import annotations

import json
import pathlib
import sys
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_agent_autonomous_completion as metric  # noqa: E402
import run_ng_agent_autonomous_completion as runner  # noqa: E402


class AgentAutonomousCompletionRunnerTests(unittest.TestCase):
    def _input(self, *, bad_scenario: str | None = None) -> dict:
        authority = runner._authority(ROOT)
        scenarios = metric._scenario_set(metric._catalog(ROOT))
        commands = []
        for scenario in scenarios:
            scenario_id = scenario["scenario_id"]
            if scenario_id == bad_scenario:
                code = "import sys; sys.stdout.write('not-a-result-receipt')"
            else:
                result = {
                    "schema": runner.RESULT_SCHEMA_ID,
                    "scenario_id": scenario_id,
                    "outcome": "completed",
                    "bounded_completion": True,
                    "reason_code": "none",
                    "completion_plan": [],
                }
                serialized = json.dumps(result, sort_keys=True)
                code = f"import sys; sys.stdout.write({serialized!r})"
            commands.append(
                {
                    "scenario_id": scenario_id,
                    "command": {
                        "argv": [sys.executable, "-c", code],
                        "environment": [],
                    },
                }
            )
        return {
            "schema": runner.INPUT_SCHEMA_ID,
            "document_version": "1.0.0",
            "authority": {
                key: authority[key] for key in ("revision", "tree", "catalog_digest")
            },
            "scenarios": commands,
        }

    def test_real_process_receipts_produce_evaluated_report(self) -> None:
        # The test source checkout is intentionally dirty while this test file
        # is being developed.  The runner's production CLI still enforces the
        # clean-authority boundary; the subprocesses themselves execute in
        # fresh clones of the exact committed HEAD.
        with mock.patch.object(runner, "_require_clean_authority"):
            evidence = runner.run(ROOT, self._input())
        report = metric._report(ROOT, evidence=evidence)
        metric.validate_report(ROOT, report)
        self.assertEqual(report["status"], "evaluated")
        self.assertEqual(report["value_percent"], 100.0)
        self.assertEqual(report["population"]["completed"], 9)
        for row in evidence["scenarios"]:
            self.assertEqual(row["execution_witness"]["process"]["terminal_state"], "exited")
            self.assertEqual(row["execution_witness"]["process"]["exit_status"], 0)
            self.assertTrue(row["execution_witness"]["process"]["stdout"]["complete"])

    def test_invalid_command_result_is_a_real_failed_outcome(self) -> None:
        with mock.patch.object(runner, "_require_clean_authority"):
            evidence = runner.run(
                ROOT,
                self._input(bad_scenario="agent.golden-recipe.v1"),
            )
        report = metric._report(ROOT, evidence=evidence)
        metric.validate_report(ROOT, report)
        self.assertEqual(report["status"], "evaluated")
        self.assertEqual(report["population"]["completed"], 8)
        self.assertEqual(report["population"]["failed"], 1)
        failed = next(
            row for row in report["outcomes"] if row["scenario_id"] == "agent.golden-recipe.v1"
        )
        self.assertEqual(failed["outcome"], "failed")
        self.assertEqual(failed["reason_code"], "execution-result-invalid")

    def test_stale_authority_input_is_rejected_before_execution(self) -> None:
        value = self._input()
        value["authority"]["tree"] = "0" * 40
        with mock.patch.object(runner, "_require_clean_authority"):
            with self.assertRaisesRegex(runner.AutonomousCompletionRunnerError, "authority"):
                runner.run(ROOT, value)

    def test_dirty_authority_is_rejected(self) -> None:
        with mock.patch.object(
            runner,
            "_git",
            side_effect=lambda _root, *arguments: "uncommitted" if arguments[0] == "status" else "",
        ):
            with self.assertRaisesRegex(runner.AutonomousCompletionRunnerError, "dirty"):
                runner._require_clean_authority(ROOT)

    def test_input_scenario_census_cannot_be_reordered_or_duplicated(self) -> None:
        value = self._input()
        value["scenarios"] = list(reversed(value["scenarios"]))
        with mock.patch.object(runner, "_require_clean_authority"):
            with self.assertRaisesRegex(runner.AutonomousCompletionRunnerError, "census"):
                runner.run(ROOT, value)


if __name__ == "__main__":
    unittest.main()
