#!/usr/bin/env python3
"""Fail-closed catalog tests for the next-generation author SDK."""

from __future__ import annotations

import copy
import json
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
        cls.project_catalog_contract = load_yaml(
            ROOT / "schemas/cxxlens_ng_project_catalog_contract.yaml"
        )
        cls.project_catalog_schema = load_yaml(
            ROOT / "schemas/cxxlens_ng_project_catalog_contract.schema.yaml"
        )
        cls.provider_task_contract = load_yaml(
            ROOT / "schemas/cxxlens_ng_portable_provider_task_contract.yaml"
        )
        cls.provider_task_schema = load_yaml(
            ROOT / "schemas/cxxlens_ng_portable_provider_task_contract.schema.yaml"
        )
        cls.provider_manifest_schema = load_yaml(
            ROOT / "schemas/cxxlens_ng_provider_manifest.schema.yaml"
        )

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

    def test_flagship_recipe_closed_world_ownership_is_exact(self) -> None:
        catalog = copy.deepcopy(self.catalog)
        recipe = next(row for row in catalog["entries"] if row["id"] == "public.recipe")
        recipe["owner_issue"] = "#104"
        with self.assertRaisesRegex(SdkContractError, "Issue #136"):
            validate_catalog(ROOT, catalog)

    def test_project_catalog_projection_cannot_drop_source_digest(self) -> None:
        contract = copy.deepcopy(self.project_catalog_contract)
        contract["canonical_projection"]["entry_fields"].remove("source_digest")
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(self.project_catalog_schema).validate(contract)

    def test_project_catalog_duplicate_policy_is_fail_closed(self) -> None:
        contract = copy.deepcopy(self.project_catalog_contract)
        contract["canonical_projection"]["duplicate_policy"] = "first-wins"
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(self.project_catalog_schema).validate(contract)

    def test_provider_task_projection_cannot_drop_condition(self) -> None:
        contract = copy.deepcopy(self.provider_task_contract)
        contract["task_projection"]["fields"].remove("condition")
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(self.provider_task_schema).validate(contract)

    def test_provider_batch_begin_cannot_drop_task_id(self) -> None:
        contract = copy.deepcopy(self.provider_task_contract)
        contract["protocol"]["batch_begin"].remove("task_id")
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(self.provider_task_schema).validate(contract)

    def test_provider_manifest_schema_rejects_runtime_invalid_major_zero(self) -> None:
        version = self.provider_manifest_schema["$defs"]["version"]
        validator = jsonschema.Draft202012Validator(version)
        validator.validate("1.0.0")
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate("0.1.0")

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
            "contributor_edges": [
                {
                    "claim_contributor": "assertion:one",
                    "producer": {
                        "id": "provider.one",
                        "semantic_contract": "sha256:" + "a" * 64,
                    },
                    "provenance": "evidence:one",
                    "guarantee": {
                        "approximation": "exact",
                        "scope": "project",
                        "assumptions": "assumptions:none",
                        "verification_modalities": ["schema_validated"],
                    },
                    "condition_universe": "build-matrix",
                    "condition_fragments": ["debug"],
                    "interpretation": "cc.canonical-1",
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
        missing_edge = copy.deepcopy(row)
        missing_edge.pop("contributor_edges")
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(row_schema).validate(missing_edge)

    def test_query_summary_requires_lossless_fragment_index(self) -> None:
        schema = load_yaml(
            ROOT / "schemas/cxxlens_ng_query_execution_result.schema.yaml"
        )
        summary_schema = {
            "$schema": schema["$schema"],
            "$defs": schema["$defs"],
            "$ref": "#/$defs/summary_guarantee",
        }
        digest = "semantic-v2:sha256:" + "a" * 64
        guarantee = {
            "approximation": "exact",
            "scope": "project",
            "assumptions": "assumptions:none",
            "verification_modalities": ["schema_validated"],
        }
        fragment = {
            "guarantee": guarantee,
            "condition_universe": "build-matrix",
            "condition_fragments": ["debug"],
            "interpretation": "cc.canonical-1",
            "assumptions": ["assumptions:none"],
            "claim_contributors": ["assertion:one"],
            "producer_contracts": [
                {
                    "id": "provider.one",
                    "semantic_contract": "sha256:" + "a" * 64,
                }
            ],
            "provenance": ["evidence:one"],
            "coverage_states": ["covered"],
            "closure_ids": ["closure:one"],
            "condition_partition_complete": True,
            "conflicting": False,
            "unresolved": False,
            "requires_closure": True,
        }
        summary = {
            "approximation": "exact",
            "scope": "project",
            "condition_partition": {
                "universe": "build-matrix",
                "alternatives": ["debug"],
            },
            "interpretation_partitions": ["cc.canonical-1"],
            "assumptions": ["assumptions:none"],
            "verification_modalities": ["schema_validated"],
            "fragment_count": 1,
            "fragment_set_digest": digest,
            "drill_down_ref": "fragments:" + digest,
            "fragments": [fragment],
        }
        validator = jsonschema.Draft202012Validator(summary_schema)
        validator.validate(summary)
        canonical = json.dumps(summary, sort_keys=True, separators=(",", ":"))
        decoded = json.loads(canonical)
        validator.validate(decoded)
        self.assertEqual(
            canonical, json.dumps(decoded, sort_keys=True, separators=(",", ":"))
        )
        for required in ("condition_partition", "fragment_set_digest", "fragments"):
            missing = copy.deepcopy(summary)
            missing.pop(required)
            with self.subTest(required=required), self.assertRaises(
                jsonschema.ValidationError
            ):
                validator.validate(missing)


if __name__ == "__main__":
    unittest.main()
