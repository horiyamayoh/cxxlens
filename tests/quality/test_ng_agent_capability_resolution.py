#!/usr/bin/env python3
"""Contract tests for the canonical agent capability resolution and nine-path corpus."""

from __future__ import annotations

import copy
import json
import pathlib
import shutil
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
import sys

sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_agent_capability_resolution as resolution  # noqa: E402


class AgentCapabilityResolutionTests(unittest.TestCase):
    def test_corpus_has_nine_paths_and_all_result_states(self) -> None:
        report = resolution.corpus(ROOT)
        self.assertEqual(report["paths"], 9)
        self.assertEqual(report["safe_stop_rate_percent"], 100)
        self.assertTrue(report["all_result_states_exercised"])
        self.assertEqual(
            set(report["result_state_counts"]),
            set(resolution.RESULT_STATES),
        )
        for value in resolution.RESULT_STATES:
            self.assertGreater(report["result_state_counts"][value], 0)

    def test_json_and_markdown_are_projections_of_one_resolution(self) -> None:
        value = resolution.build_resolution(
            ROOT,
            "agent.golden-actionable-unknown.v1",
            synthetic=True,
        )
        resolution.validate_resolution(ROOT, value)
        markdown = resolution.render_markdown(value)
        self.assertIn("`unknown`", markdown)
        self.assertNotIn("unknown-use-case", markdown)
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            json_path = root / "resolution.json"
            markdown_path = root / "resolution.md"
            json_path.write_text(
                json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
                encoding="utf-8",
            )
            markdown_path.write_text(markdown, encoding="utf-8")
            self.assertEqual(
                json_path.read_text(encoding="utf-8"),
                json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
            )
            self.assertEqual(markdown_path.read_text(encoding="utf-8"), markdown)

    def test_unknown_use_case_is_actionable_and_fail_closed(self) -> None:
        value = resolution.build_resolution(ROOT, "future.use-case.v1", synthetic=True)
        self.assertEqual(value["result"]["state"], "unknown")
        self.assertEqual(value["result"]["reason_code"], "unknown-use-case")
        self.assertEqual(value["missing"][0]["owner_issue"], "#277")
        self.assertTrue(value["completion_plan"])
        resolution.validate_resolution(ROOT, value)

    def test_stale_revision_and_tree_are_rejected(self) -> None:
        with self.assertRaisesRegex(resolution.CapabilityResolutionError, "stale-authority"):
            resolution.build_resolution(
                ROOT,
                "agent.golden-relation.v1",
                synthetic=True,
                expected_revision="1" * 40,
            )
        with self.assertRaisesRegex(resolution.CapabilityResolutionError, "stale-authority"):
            resolution.build_resolution(
                ROOT,
                "agent.golden-relation.v1",
                synthetic=True,
                expected_tree="1" * 40,
            )

    def test_catalog_bytes_are_part_of_the_authority_digest(self) -> None:
        catalog = copy.deepcopy(resolution.validate_catalog(ROOT))
        self.assertIn(resolution.CATALOG.as_posix(), catalog["authority"]["source_paths"])
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for relative in catalog["authority"]["source_paths"]:
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / relative, destination)
            original = resolution._authority(root, catalog, synthetic=True)
            catalog_path = root / resolution.CATALOG
            catalog_path.write_bytes(catalog_path.read_bytes() + b"\n# authority drift\n")
            changed = resolution._authority(root, catalog, synthetic=True)
            self.assertNotEqual(
                original["authority_digest"], changed["authority_digest"]
            )

    def test_forward_dependency_and_path_drift_fail_closed(self) -> None:
        catalog = copy.deepcopy(resolution.validate_catalog(ROOT))
        catalog["golden_paths"][0]["capability_path"][1]["requires"] = [
            "future.capability.v1"
        ]
        with self.assertRaisesRegex(resolution.CapabilityResolutionError, "unknown or forward"):
            resolution.validate_catalog(ROOT, catalog)

    def test_demand_binding_must_reference_the_admitted_issue_277_family(self) -> None:
        catalog = copy.deepcopy(resolution.validate_catalog(ROOT))
        catalog["golden_paths"][0]["demand"]["family_id"] = "invented-family"
        with self.assertRaisesRegex(resolution.CapabilityResolutionError, "demand family is unknown"):
            resolution.validate_catalog(ROOT, catalog)
        catalog = copy.deepcopy(resolution.validate_catalog(ROOT))
        catalog["golden_paths"][0]["demand"]["capabilities"].append("invented-capability")
        with self.assertRaisesRegex(resolution.CapabilityResolutionError, "demand capability edge"):
            resolution.validate_catalog(ROOT, catalog)


if __name__ == "__main__":
    unittest.main()
