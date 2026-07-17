#!/usr/bin/env python3
"""Fail-closed catalog tests for the next-generation author SDK."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest

import jsonschema


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

    def test_flagship_recipe_execution_completeness_ownership_is_exact(self) -> None:
        catalog = copy.deepcopy(self.catalog)
        recipe = next(row for row in catalog["entries"] if row["id"] == "public.recipe")
        recipe["owner_issue"] = "#73"
        with self.assertRaisesRegex(SdkContractError, "Issue #104"):
            validate_catalog(ROOT, catalog)

    def test_implemented_error_code_cannot_be_omitted(self) -> None:
        catalog = copy.deepcopy(self.catalog)
        provider = next(
            row for row in catalog["entries"] if row["id"] == "public.provider-sdk"
        )
        provider["errors"].remove("provider.manifest-invalid")
        with self.assertRaisesRegex(SdkContractError, "error codes are absent"):
            validate_catalog(ROOT, catalog)

    def test_query_result_row_requires_structured_contributor_guarantees(self) -> None:
        schema = load_yaml(
            ROOT / "schemas/cxxlens_ng_query_execution_result.schema.yaml"
        )
        row_schema = {
            "$schema": schema["$schema"],
            "$defs": schema["$defs"],
            "$ref": "#/$defs/row",
        }
        row = {
            "values": {},
            "multiplicity": 1,
            "condition_universe": "build-matrix",
            "condition_fragments": ["debug"],
            "contributor_guarantees": [
                {
                    "approximation": "exact",
                    "scope": "project",
                    "assumptions": "assumptions:none",
                    "verification_modalities": ["schema_validated"],
                }
            ],
            "interpretation": "cc.canonical-1",
            "claim_contributors": ["assertion:one"],
            "producer_contracts": [
                {
                    "id": "provider.one",
                    "semantic_contract": "sha256:" + "a" * 64,
                }
            ],
            "provenance": ["evidence:one"],
        }
        jsonschema.Draft202012Validator(row_schema).validate(row)
        missing = copy.deepcopy(row)
        missing.pop("contributor_guarantees")
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(row_schema).validate(missing)


if __name__ == "__main__":
    unittest.main()
