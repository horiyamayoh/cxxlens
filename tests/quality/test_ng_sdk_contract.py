#!/usr/bin/env python3
"""Fail-closed catalog tests for the next-generation author SDK."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_sdk_contract import (  # noqa: E402
    SdkContractError,
    load_yaml,
    validate_boundaries,
    validate_catalog,
)


class NgSdkContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.catalog = load_yaml(ROOT / "schemas/cxxlens_ng_public_api_catalog.yaml")

    def test_exact_catalog_and_ordinary_boundary_are_valid(self) -> None:
        validate_catalog(ROOT, self.catalog)
        validate_boundaries(ROOT)

    def test_missing_author_path_is_rejected(self) -> None:
        catalog = copy.deepcopy(self.catalog)
        catalog["author_paths"].pop()
        with self.assertRaisesRegex(SdkContractError, "schema validation|path set"):
            validate_catalog(ROOT, catalog)

    def test_duplicate_public_entry_is_rejected(self) -> None:
        catalog = copy.deepcopy(self.catalog)
        catalog["entries"].append(copy.deepcopy(catalog["entries"][0]))
        with self.assertRaisesRegex(SdkContractError, "duplicate public entry"):
            validate_catalog(ROOT, catalog)

    def test_lifetime_contract_cannot_be_omitted(self) -> None:
        catalog = copy.deepcopy(self.catalog)
        catalog["entries"][0].pop("lifetime")
        with self.assertRaisesRegex(SdkContractError, "schema validation"):
            validate_catalog(ROOT, catalog)

    def test_flagship_recipe_ownership_cannot_be_claimed_early(self) -> None:
        catalog = copy.deepcopy(self.catalog)
        recipe = next(row for row in catalog["entries"] if row["id"] == "public.recipe")
        recipe["owner_issue"] = "#66"
        with self.assertRaisesRegex(SdkContractError, "Issue #73"):
            validate_catalog(ROOT, catalog)

    def test_implemented_error_code_cannot_be_omitted(self) -> None:
        catalog = copy.deepcopy(self.catalog)
        provider = next(
            row for row in catalog["entries"] if row["id"] == "public.provider-sdk"
        )
        provider["errors"].remove("provider.manifest-invalid")
        with self.assertRaisesRegex(SdkContractError, "error codes are absent"):
            validate_catalog(ROOT, catalog)


if __name__ == "__main__":
    unittest.main()
