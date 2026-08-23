#!/usr/bin/env python3
"""Schema-level regression tests for the public catalog metadata boundary."""

from __future__ import annotations

import copy
import pathlib
import unittest

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]


def load(relative: str) -> dict:
    value = yaml.safe_load((ROOT / relative).read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise AssertionError(f"expected mapping: {relative}")
    return value


class PublicCatalogMetadataCleanupTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.catalog = load("schemas/cxxlens_ng_public_api_catalog.yaml")
        cls.catalog_schema = load("schemas/cxxlens_ng_public_api_catalog.schema.yaml")

    def test_current_documents_validate_without_repository_operation_metadata(self) -> None:
        jsonschema.Draft202012Validator(self.catalog_schema).validate(self.catalog)

        self.assertNotIn("owner", self.catalog["authority"])
        for entry in self.catalog["entries"]:
            self.assertNotIn("implementation_evidence", entry)

    def test_removed_metadata_is_rejected_by_the_direct_schemas(self) -> None:
        catalog = copy.deepcopy(self.catalog)
        catalog["authority"]["repository_metadata"] = "#66"
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(self.catalog_schema).validate(catalog)

        catalog = copy.deepcopy(self.catalog)
        catalog["entries"][0]["implementation_evidence"] = ["src/sdk/common.cpp"]
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(self.catalog_schema).validate(catalog)

    def test_product_surface_and_acceptance_shape_remain_present(self) -> None:
        self.assertTrue(self.catalog["author_paths"])
        for path in self.catalog["author_paths"]:
            self.assertIn("positive_example", path)
            self.assertIn("negative_example", path)
            self.assertIn("negative_mode", path)
        for entry in self.catalog["entries"]:
            self.assertTrue(entry["symbols"])
            self.assertTrue(entry["invariants"])


if __name__ == "__main__":
    unittest.main()
