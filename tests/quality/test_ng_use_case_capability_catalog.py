#!/usr/bin/env python3
"""Contract and adversarial tests for the readiness-derived #275 projection."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/quality"))

import check_ng_use_case_capability_catalog as catalog  # noqa: E402


class UseCaseCapabilityCatalogTests(unittest.TestCase):
    def readiness(self) -> dict:
        document = catalog.load_yaml(ROOT / catalog.READINESS_PATH)
        self.assertIsInstance(document, dict)
        return document

    def test_main_readiness_projects_to_deterministic_demand_registry(self) -> None:
        report = catalog.build_report(ROOT)
        self.assertEqual(report["source"]["tracking_issue"], "#275")
        self.assertEqual(report["projection"]["status"], "declaration-only-not-qualified")
        self.assertEqual(len(report["use_cases"]), 7)
        self.assertEqual(
            report["use_cases"][0]["id"],
            "abi-ir-binary-evidence",
        )
        self.assertIn(
            {"id": "source-closure", "demanded_by": ["repository-semantic-query"]},
            report["capability_registry"],
        )
        self.assertEqual(
            report["use_cases"],
            sorted(report["use_cases"], key=lambda entry: entry["id"]),
        )
        catalog.validate_report(report)

    def test_duplicate_use_case_is_rejected(self) -> None:
        readiness = self.readiness()
        families = readiness["product_direction"]["roadmap"]["use_case_families"]
        families.append(copy.deepcopy(families[0]))
        with self.assertRaisesRegex(catalog.CatalogError, "duplicate or invalid use-case ID"):
            catalog.project_use_cases(readiness)

    def test_duplicate_capability_reference_is_rejected(self) -> None:
        readiness = self.readiness()
        families = readiness["product_direction"]["roadmap"]["use_case_families"]
        families[0]["capabilities"].append(families[0]["capabilities"][0])
        with self.assertRaisesRegex(catalog.CatalogError, "duplicate capability reference"):
            catalog.project_use_cases(readiness)

    def test_wrong_authority_contract_is_rejected(self) -> None:
        readiness = self.readiness()
        readiness["product_direction"]["contract"] = "invented.contract.v1"
        with self.assertRaisesRegex(catalog.CatalogError, "accepted direction"):
            catalog.project_use_cases(readiness)

    def test_report_cannot_claim_provider_or_qualification_bindings(self) -> None:
        report = catalog.build_report(ROOT)
        report["projection"]["excludes"].remove("provider-bindings")
        with self.assertRaises(catalog.CatalogError):
            catalog.validate_report(report)


if __name__ == "__main__":
    unittest.main()
