#!/usr/bin/env python3
"""Positive and fail-closed tests for global public-contract conventions."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_global_contract_conventions import (  # noqa: E402
    ConventionError,
    digest,
    generate_ownership,
    validate_conventions,
    validate_ownership,
)


def load(name: str) -> dict:
    return yaml.safe_load((ROOT / "schemas" / name).read_text(encoding="utf-8"))


class GlobalContractConventionsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.catalog = load("cxxlens_public_api_contract.yaml")
        cls.conventions = load("cxxlens_global_contract_conventions.yaml")
        cls.conventions_schema = load("cxxlens_global_contract_conventions.schema.yaml")
        cls.ownership = load("cxxlens_contract_ownership.yaml")
        cls.ownership_schema = load("cxxlens_contract_ownership.schema.yaml")
        cls.expected = generate_ownership(cls.catalog, cls.conventions, ROOT)

    def assert_conventions_invalid(self, document: dict, pattern: str) -> None:
        with self.assertRaisesRegex(ConventionError, pattern):
            validate_conventions(document, self.conventions_schema, self.catalog, ROOT)

    def assert_ownership_invalid(self, document: dict, pattern: str) -> None:
        unsigned = copy.deepcopy(document)
        unsigned.pop("semantic_digest", None)
        document["semantic_digest"] = digest(unsigned)
        with self.assertRaisesRegex(ConventionError, pattern):
            validate_ownership(
                document,
                self.ownership_schema,
                self.expected,
                self.catalog,
                ROOT,
            )

    def test_positive_complete_conventions_and_ownership(self) -> None:
        validate_conventions(
            self.conventions, self.conventions_schema, self.catalog, ROOT
        )
        validate_ownership(
            self.ownership,
            self.ownership_schema,
            self.expected,
            self.catalog,
            ROOT,
        )
        self.assertEqual(self.ownership["summary"]["public_type_count"], 179)
        self.assertEqual(self.ownership["summary"]["shared_component_count"], 66)
        self.assertEqual(self.ownership["summary"]["provider_subject_count"], 24)
        self.assertGreaterEqual(self.ownership["summary"]["schema_count"], 56)

    def test_missing_package_owner_is_rejected(self) -> None:
        document = copy.deepcopy(self.conventions)
        document["package_candidate_owners"][0]["packages"].remove("core")
        self.assert_conventions_invalid(document, "cover all 22")

    def test_duplicate_package_owner_is_rejected(self) -> None:
        document = copy.deepcopy(self.conventions)
        document["package_candidate_owners"][1]["packages"].append("core")
        self.assert_conventions_invalid(document, "duplicate package candidate owner")

    def test_package_issue_cannot_authorize_freeze(self) -> None:
        document = copy.deepcopy(self.conventions)
        transition = next(
            row
            for row in document["state_machine"]["transitions"]
            if row["to"] == "frozen"
        )
        transition["authority"] = "package_candidate_owner"
        self.assert_conventions_invalid(document, "transition")

    def test_invalid_result_combination_is_rejected(self) -> None:
        document = copy.deepcopy(self.conventions)
        row = next(row for row in document["result_decisions"] if row["id"] == "partial")
        row["coverage_complete"] = True
        self.assert_conventions_invalid(document, "invalid result decision combination")

    def test_dangling_provider_owner_is_rejected(self) -> None:
        document = copy.deepcopy(self.ownership)
        document["providers"][0]["owner_atomic_unit"] = "AU-NONE-999"
        self.assert_ownership_invalid(document, "dangling owner atomic unit")

    def test_duplicate_schema_owner_is_rejected(self) -> None:
        document = copy.deepcopy(self.ownership)
        duplicate = copy.deepcopy(document["schemas"][0])
        duplicate["id"] += ":duplicate"
        document["schemas"].append(duplicate)
        document["summary"]["schema_count"] += 1
        self.assert_ownership_invalid(document, "more than one owner")

    def test_unstable_registry_order_is_rejected(self) -> None:
        document = copy.deepcopy(self.ownership)
        document["public_types"][0], document["public_types"][1] = (
            document["public_types"][1],
            document["public_types"][0],
        )
        self.assert_ownership_invalid(document, "stale or incomplete")


if __name__ == "__main__":
    unittest.main()
