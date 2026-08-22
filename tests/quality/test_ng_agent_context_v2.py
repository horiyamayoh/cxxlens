#!/usr/bin/env python3
"""Tests for exactly selected agent-context v2 packets."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_agent_context_v2 import (  # noqa: E402
    AgentContextV2Error,
    _packet,
    _is_bounded_completion,
    _registered_units,
    _select,
    build,
    corpus,
    validate_packet,
)
import check_ng_work_units as work_units  # noqa: E402


class AgentContextV2Test(unittest.TestCase):
    def test_every_registered_unit_builds_a_schema_valid_packet(self) -> None:
        report = corpus(ROOT)
        self.assertGreaterEqual(report["packets"], 9)
        self.assertEqual(report["safe_stop_rate_percent"], 100)
        self.assertGreaterEqual(report["bounded_packet_completion_rate_percent"], 80)
        self.assertEqual(report["v1_issue_261_compatibility"], "verified")
        compatibility = report["v1_issue_261_compatibility_evidence"]
        self.assertEqual(compatibility["method"], "exact-v1-plan-and-check")
        self.assertEqual(compatibility["packet_schema"], "cxxlens.ng-agent-context.v1")
        self.assertEqual(compatibility["issue"], "#261")
        self.assertEqual(compatibility["plan_exit_code"], 0)
        self.assertEqual(compatibility["check_exit_code"], 0)
        self.assertEqual(compatibility["authority_scope"], "temporary-clean-head-clone")
        self.assertEqual(compatibility["output_scope"], "temporary-directory")

    def test_completion_selection_requires_registered_semantic_binding(self) -> None:
        manifest = work_units.validate(ROOT)
        entry, unit = _select(manifest, "#277", "wu-277-context-v2")
        packet = build(ROOT, "#277", "wu-277-context-v2", synthetic=True)
        self.assertTrue(_is_bounded_completion(ROOT, manifest, entry, unit, packet, synthetic=True))
        mutations = (
            ("issue", lambda value: value.__setitem__("issue", "#261"), "issue/unit identity"),
            (
                "dependency",
                lambda value: value.__setitem__("dependencies", ["wu-999-unknown"]),
                "unknown dependency",
            ),
            (
                "manifest-digest",
                lambda value: value["authority"].__setitem__("manifest_digest", "sha256:" + "0" * 64),
                "manifest digest",
            ),
            (
                "actionable",
                lambda value: value.__setitem__("completion_plan", [" "]),
                "(actionable field|authority projection drift)",
            ),
        )
        for name, mutate, message in mutations:
            with self.subTest(name=name):
                candidate = copy.deepcopy(packet)
                mutate(candidate)
                with self.assertRaisesRegex(AgentContextV2Error, message):
                    _is_bounded_completion(ROOT, manifest, entry, unit, candidate, synthetic=True)

    def test_packet_consumes_canonical_capability_resolution(self) -> None:
        packet = build(ROOT, "#277", "wu-277-context-v2", synthetic=True)
        resolution = packet["capability_resolution"]
        self.assertEqual(resolution["schema"], "cxxlens.agent-capability-resolution.v1")
        self.assertEqual(resolution["result"]["state"], "unknown")
        self.assertTrue(resolution["missing"])
        candidate = copy.deepcopy(packet)
        candidate["capability_resolution"]["result"]["reason_code"] = "none"
        with self.assertRaisesRegex(AgentContextV2Error, "capability resolution projection drift"):
            from check_ng_agent_context_v2 import _validate_packet_semantics

            manifest = work_units.validate(ROOT)
            entry, unit = _select(manifest, "#277", "wu-277-context-v2")
            _validate_packet_semantics(
                ROOT, manifest, entry, unit, candidate, synthetic=True
            )

    def test_unknown_work_unit_reference_cannot_enter_corpus_registry(self) -> None:
        manifest = work_units.validate(ROOT)
        mutated = copy.deepcopy(manifest)
        mutated["entries"][0]["units"][0]["depends_on"] = ["wu-999-unknown"]
        with self.assertRaisesRegex(AgentContextV2Error, "unknown work unit reference"):
            _registered_units(mutated)

    def test_v1_compatibility_failure_is_not_reported_as_preserved(self) -> None:
        with mock.patch(
            "check_ng_agent_context_v2._run_v1_compatibility",
            side_effect=AgentContextV2Error("synthetic v1 failure"),
        ):
            with self.assertRaisesRegex(AgentContextV2Error, "synthetic v1 failure"):
                corpus(ROOT)

    def test_issue_and_unit_must_match_exactly(self) -> None:
        manifest = work_units.validate(ROOT)
        with self.assertRaisesRegex(AgentContextV2Error, "foreign unit"):
            _select(manifest, "#173", "wu-261-source-closure-authority")

    def test_unsafe_work_unit_path_cannot_generate_a_packet(self) -> None:
        manifest = work_units.validate(ROOT)
        entry, unit = _select(manifest, "#277", "wu-277-context-v2")
        unit["owned_paths"] = [".git/HEAD", *unit["owned_paths"][1:]]
        with self.assertRaisesRegex(AgentContextV2Error, "schema validation failed"):
            validate_packet(ROOT, _packet(ROOT, manifest, entry, unit, synthetic=True))

    def test_unsafe_packet_paths_are_rejected(self) -> None:
        packet = build(ROOT, "#277", "wu-277-context-v2", synthetic=True)
        mutations = (
            ("allowed_write_paths", lambda value: value["allowed_write_paths"].__setitem__(0, "nested/.git/path")),
            ("reading_set", lambda value: value["reading_set"][0].__setitem__("path", "nested//path")),
            ("integration_generated_surfaces", lambda value: value["integration_generated_surfaces"].__setitem__(0, "nul\x00path")),
        )
        for field, mutate in mutations:
            with self.subTest(field=field):
                candidate = copy.deepcopy(packet)
                mutate(candidate)
                with self.assertRaisesRegex(AgentContextV2Error, "schema validation failed"):
                    validate_packet(ROOT, candidate)

    def test_whitespace_only_actionable_fields_are_rejected_by_schema(self) -> None:
        packet = build(ROOT, "#277", "wu-277-context-v2", synthetic=True)
        for field in ("forbidden_shortcuts", "evidence_commands", "completion_plan"):
            with self.subTest(field=field):
                candidate = copy.deepcopy(packet)
                candidate[field] = [" "]
                with self.assertRaisesRegex(AgentContextV2Error, "schema validation failed"):
                    validate_packet(ROOT, candidate)

    def test_schema_requires_blocker_for_nonready_disposition(self) -> None:
        packet = build(ROOT, "#202", "wu-202-normalization-effect", synthetic=True)
        self.assertNotEqual(packet["execution_disposition"], "ready")
        candidate = copy.deepcopy(packet)
        candidate["blockers"] = []
        with self.assertRaisesRegex(AgentContextV2Error, "schema validation failed"):
            validate_packet(ROOT, candidate)

    def test_blocked_unit_has_actionable_stop(self) -> None:
        packet = build(ROOT, "202", "wu-202-normalization-effect", synthetic=True)
        self.assertEqual(packet["execution_disposition"], "stop-blocked-by-authority")
        self.assertIn("blocked-by-authority", packet["blockers"])
        self.assertTrue(packet["completion_plan"])
        self.assertEqual(packet["consumed_products"], ["sqlite.logical-read-receipt"])
        self.assertEqual(packet["required_product_receipts"]["sqlite.logical-read-receipt"]["status"], "pending")

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

    def test_downstream_ready_state_cannot_bypass_dependency_authority(self) -> None:
        manifest = work_units.validate(ROOT)
        governance_entry, governance = _select(manifest, "#173", "wu-173-governance")
        governance["state"] = "ready"
        entry, unit = _select(manifest, "#277", "wu-277-context-v2")
        from check_ng_agent_context_v2 import _packet
        packet = _packet(ROOT, manifest, entry, unit, synthetic=True)
        self.assertEqual(packet["execution_disposition"], "stop-blocked-by-dependency")
        self.assertTrue(any("dependency:wu-173-governance:decision:" in value for value in packet["blockers"]))

    def test_clean_stale_checkout_cannot_emit_execution_packet(self) -> None:
        manifest = work_units.validate(ROOT)
        entry, unit = _select(manifest, "#173", "wu-173-governance")
        responses = {
            ("status", "--porcelain=v1", "--untracked-files=all"): "",
            ("fetch", "--no-tags", "origin", "main"): "",
            ("rev-parse", "HEAD"): "0" * 40,
            ("rev-parse", "origin/main"): "1" * 40,
        }
        from check_ng_agent_context_v2 import _packet
        with mock.patch(
            "check_ng_agent_context_v2._git",
            side_effect=lambda _root, *arguments: responses[arguments],
        ):
            with self.assertRaisesRegex(AgentContextV2Error, "stale checkout"):
                _packet(ROOT, manifest, entry, unit, synthetic=False)


if __name__ == "__main__":
    unittest.main()
