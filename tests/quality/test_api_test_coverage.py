#!/usr/bin/env python3
"""Positive and negative fixtures for complete-API executable test coverage."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_api_test_coverage import (  # noqa: E402
    CoverageError,
    generate_manifest,
    validate_manifest,
)


class ApiTestCoverageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.catalog = yaml.safe_load(
            (ROOT / "schemas/cxxlens_public_api_contract.yaml").read_text(encoding="utf-8")
        )
        cls.manifest = generate_manifest(cls.catalog)
        cls.completions = [
            yaml.safe_load((ROOT / path).read_text(encoding="utf-8"))
            for path in cls.manifest["completion_manifests"]
        ]
        cls.cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")

    def validate(self, manifest: dict, completions: list[dict] | None = None) -> dict:
        return validate_manifest(
            ROOT,
            self.catalog,
            manifest,
            self.completions if completions is None else completions,
            self.cmake,
        )

    def assert_invalid(
        self, manifest: dict, pattern: str, completions: list[dict] | None = None
    ) -> None:
        with self.assertRaisesRegex(CoverageError, pattern):
            self.validate(manifest, completions)

    def test_complete_coverage_report_has_zero_gaps(self) -> None:
        report = self.validate(self.manifest)
        self.assertEqual(report["complete_api_count"], 47)
        self.assertEqual(report["certified_api_count"], 47)
        self.assertEqual(report["uncovered_api_ids"], [])
        self.assertEqual(report["uncovered_member_refs"], [])
        self.assertEqual(report["uncovered_use_case_ids"], [])

    def test_complete_api_without_unit_test_is_rejected(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["apis"][0]["unit_test_id"] = "install.consumer"
        self.assert_invalid(manifest, "requires one unit test")

    def test_complete_api_without_acceptance_test_is_rejected(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["apis"][0]["acceptance_test_id"] = "unit.version"
        self.assert_invalid(manifest, "requires one acceptance test")

    def test_uncovered_use_case_is_rejected(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        row = next(item for item in manifest["apis"] if item["covered_use_cases"])
        row["covered_use_cases"].pop()
        self.assert_invalid(manifest, "declared use-case coverage is incomplete")

    def test_registered_test_without_api_reference_is_rejected(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["apis"].pop()
        self.assert_invalid(manifest, "coverage API set differs")

    def test_complete_api_outside_completion_vectors_is_rejected(self) -> None:
        completions = copy.deepcopy(self.completions)
        completions[0]["conformant_catalog_ids"].pop()
        self.assert_invalid(self.manifest, "completion manifest union differs", completions)

    def test_partial_family_member_coverage_is_rejected(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        family = next(row for row in manifest["apis"] if len(row["covered_member_indexes"]) > 1)
        family["covered_member_indexes"].pop()
        self.assert_invalid(manifest, "family member coverage is incomplete")

    def test_fixture_category_without_concrete_evidence_is_rejected(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["apis"][0]["fixtures"].pop()
        self.assert_invalid(manifest, "fixture evidence is incomplete")


if __name__ == "__main__":
    unittest.main()
