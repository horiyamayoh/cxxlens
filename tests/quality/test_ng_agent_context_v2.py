#!/usr/bin/env python3
"""Tests for exactly selected agent-context v2 packets."""

from __future__ import annotations

import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_agent_context_v2 import AgentContextV2Error, _select, build, corpus  # noqa: E402
import check_ng_work_units as work_units  # noqa: E402


class AgentContextV2Test(unittest.TestCase):
    def test_every_registered_unit_builds_a_schema_valid_packet(self) -> None:
        report = corpus(ROOT)
        self.assertGreaterEqual(report["packets"], 9)
        self.assertEqual(report["safe_stop_rate_percent"], 100)
        self.assertGreaterEqual(report["bounded_packet_completion_rate_percent"], 80)
        self.assertEqual(report["v1_issue_261_compatibility"], "preserved")

    def test_issue_and_unit_must_match_exactly(self) -> None:
        manifest = work_units.validate(ROOT)
        with self.assertRaisesRegex(AgentContextV2Error, "foreign unit"):
            _select(manifest, "#173", "wu-261-source-closure-authority")

    def test_blocked_unit_has_actionable_stop(self) -> None:
        packet = build(ROOT, "202", "wu-202-normalization-effect", synthetic=True)
        self.assertEqual(packet["execution_disposition"], "stop-blocked-by-authority")
        self.assertIn("blocked-by-authority", packet["blockers"])
        self.assertTrue(packet["completion_plan"])

    def test_dependency_blocked_unit_excludes_integration_owned_generated_surfaces(self) -> None:
        packet = build(ROOT, "#277", "wu-277-context-v2", synthetic=True)
        self.assertEqual(packet["execution_disposition"], "stop-blocked-by-dependency")
        self.assertIn("dependency:wu-173-governance:review-required", packet["blockers"])
        self.assertNotIn("schemas/cxxlens_asset_migration_ledger.json", packet["allowed_write_paths"])
        self.assertIn("schemas/cxxlens_asset_migration_ledger.json", packet["integration_generated_surfaces"])

    def test_ready_state_cannot_override_rejected_governance_authority(self) -> None:
        manifest = work_units.validate(ROOT)
        entry, unit = _select(manifest, "#173", "wu-173-governance")
        unit["state"] = "ready"
        from check_ng_agent_context_v2 import _packet
        packet = _packet(ROOT, manifest, entry, unit, synthetic=True)
        self.assertEqual(packet["execution_disposition"], "stop-blocked-by-dependency")
        self.assertTrue(any(value.startswith("decision:decision.delivery.direct-main:proposed:rejected") for value in packet["blockers"]))


if __name__ == "__main__":
    unittest.main()
