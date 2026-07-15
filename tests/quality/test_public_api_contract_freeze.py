#!/usr/bin/env python3
"""Positive and fail-closed tests for the public API Contract Freeze gate."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest

import jsonschema


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_public_api_contract_freeze import (  # noqa: E402
    FreezeError,
    load_yaml,
    schema_validate,
    validate_downstream_edges,
    validate_manifest,
)


class PublicApiContractFreezeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.schema = load_yaml(
            ROOT / "schemas/cxxlens_public_api_contract_freeze.schema.yaml"
        )
        cls.manifest = load_yaml(
            ROOT / "schemas/cxxlens_public_api_contract_freeze.yaml"
        )
        cls.catalog = load_yaml(ROOT / "schemas/cxxlens_public_api_contract.yaml")

    def test_positive_freeze_is_complete_superseded_provenance(self) -> None:
        validated = validate_manifest(ROOT)
        self.assertFalse(validated["phase_c_authorized"])
        self.assertEqual(validated["state"], "superseded")
        self.assertEqual(validated["summary"]["apis"], 124)
        self.assertEqual(validated["summary"]["contract_states"], {"frozen": 22})

    def test_schema_rejects_a_partial_freeze(self) -> None:
        document = copy.deepcopy(self.manifest)
        document["summary"]["apis"] = 123
        with self.assertRaisesRegex(FreezeError, "schema validation"):
            schema_validate(document, self.schema)

    def test_downstream_api_omission_is_rejected(self) -> None:
        catalog = copy.deepcopy(self.catalog)
        catalog["packages"][0]["apis"].pop()
        with self.assertRaisesRegex(FreezeError, "coverage differs"):
            validate_downstream_edges(ROOT, catalog)

    def test_supersession_is_exact(self) -> None:
        self.assertEqual(self.manifest["supersession"]["issue"], "#57")
        self.assertEqual(self.manifest["supersession"]["tracking_issue"], "#56")
        self.assertFalse(
            self.manifest["supersession"]["legacy_new_work_authorized"]
        )


if __name__ == "__main__":
    try:
        unittest.main()
    except jsonschema.ValidationError as error:
        raise SystemExit(str(error)) from error
