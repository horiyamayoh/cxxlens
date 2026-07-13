#!/usr/bin/env python3
"""Positive, negative, ordering, and full-regression tests for the API catalog."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from validate_api_contract import ContractError, canonical_summary, render_inventory, validate_document


class ApiContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.document = yaml.safe_load(
            (ROOT / "schemas" / "cxxlens_public_api_contract.yaml").read_text(encoding="utf-8")
        )
        cls.inventory = (ROOT / "docs" / "design" / "api_catalog_inventory.md").read_text(
            encoding="utf-8"
        )

    def mutated(self) -> dict:
        return copy.deepcopy(self.document)

    def assert_invalid(self, document: dict, pattern: str) -> None:
        with self.assertRaisesRegex(ContractError, pattern):
            validate_document(document)

    def test_positive_full_catalog_and_inventory(self) -> None:
        summary = validate_document(self.document, self.inventory)
        self.assertEqual(summary["package_count"], 22)
        self.assertEqual(summary["api_entry_count"], 124)
        self.assertEqual(
            summary["implementation_state_counts"],
            {"conformant": 47, "implemented": 0, "unimplemented": 77},
        )
        self.assertEqual(
            summary["contract_state_counts"],
            {"candidate": 9, "unresolved": 13},
        )

    def test_duplicate_id_fixture(self) -> None:
        document = self.mutated()
        document["packages"][0]["apis"][1]["id"] = "API-CORE-001"
        self.assert_invalid(document, "duplicate API id")

    def test_dangling_fact_fixture(self) -> None:
        document = self.mutated()
        document["packages"][0]["apis"][0]["requires"] = {"facts": ["not_a_fact"], "apis": []}
        self.assert_invalid(document, "dangling fact")

    def test_dangling_capability_fixture(self) -> None:
        document = self.mutated()
        document["packages"][0]["apis"][0]["requires"] = {
            "capabilities": ["not.a.capability"],
            "apis": [],
        }
        self.assert_invalid(document, "dangling capability")

    def test_dangling_traceability_fixture(self) -> None:
        document = self.mutated()
        document["packages"][0]["apis"][0]["requirements"] = ["FR-999"]
        self.assert_invalid(document, "dangling requirement")

    def test_invalid_phase_fixture(self) -> None:
        document = self.mutated()
        document["packages"][0]["apis"][0]["phase"] = "soon"
        self.assert_invalid(document, "invalid phase")

    def test_inventory_mismatch_fixture(self) -> None:
        with self.assertRaisesRegex(ContractError, "inventory"):
            validate_document(self.document, self.inventory + "stale\n")

    def test_ready_signature_missing_fixture(self) -> None:
        document = self.mutated()
        api = next(
            api
            for package in document["packages"]
            for api in package["apis"]
            if api["declaration"]["status"] == "unresolved"
        )
        api["readiness"] = {"state": "ready", "blockers": []}
        self.assert_invalid(document, "unresolved declaration cannot be ready")

    def test_atomic_unit_split_fixture(self) -> None:
        document = self.mutated()
        first = document["packages"][0]["apis"][0]
        second = next(
            api
            for api in document["packages"][0]["apis"]
            if api["readiness"]["state"] == "blocked"
        )
        second["atomic_unit"]["id"] = first["atomic_unit"]["id"]
        document["summary"]["atomic_unit_count"] -= 1
        self.assert_invalid(document, "split readiness")

    def test_package_candidate_requires_owner_issue(self) -> None:
        document = self.mutated()
        package = document["packages"][0]
        package["contract"]["state"] = "candidate"
        package["contract"]["transition_issue"] = "#44"
        self.assert_invalid(document, "candidate transition requires")

    def test_package_issue_cannot_freeze_contract(self) -> None:
        document = self.mutated()
        package = document["packages"][0]
        package["contract"]["state"] = "frozen"
        package["contract"]["transition_issue"] = package["contract"]["owner_issue"]
        self.assert_invalid(document, "only #54 may freeze")

    def test_api_dependency_cycle_fixture(self) -> None:
        document = self.mutated()
        first, second = document["packages"][0]["apis"][:2]
        first["requires"] = {"apis": [second["id"]]}
        second["requires"] = {"apis": [first["id"]]}
        self.assert_invalid(document, "dependency cycle")

    def test_canonical_summary_ignores_catalog_order(self) -> None:
        document = self.mutated()
        document["packages"].reverse()
        for package in document["packages"]:
            package["apis"].reverse()
        self.assertEqual(canonical_summary(self.document), canonical_summary(document))
        self.assertEqual(render_inventory(self.document), render_inventory(document))


if __name__ == "__main__":
    unittest.main()
