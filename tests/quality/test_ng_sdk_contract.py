#!/usr/bin/env python3
"""Fail-closed catalog tests for the next-generation author SDK."""

from __future__ import annotations

import copy
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

import jsonschema


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_sdk_contract import (  # noqa: E402
    SdkContractError,
    admitted_generated_relations,
    canonical_relation,
    load_yaml,
    render,
    validate_boundaries,
    validate_catalog,
    validate_generated_relation_header,
    validate_project_catalog_worker_decomposition,
    validate_store_identity_decomposition,
)


class NgSdkContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.catalog = load_yaml(ROOT / "schemas/cxxlens_ng_public_api_catalog.yaml")
        cls.registry = load_yaml(ROOT / "schemas/cxxlens_ng_relation_registry.yaml")
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

    def test_generation_covers_every_catalog_admitted_relation_header(self) -> None:
        admitted = {
            header
            for collection in (self.catalog["packages"], self.catalog["entries"])
            for row in collection
            for header in row["headers"]
            if header.startswith("include/cxxlens/relations/")
        }
        generated = admitted_generated_relations(self.catalog, self.registry)
        self.assertEqual(
            {relative.as_posix() for _, relative in generated}, admitted
        )
        self.assertEqual(len(generated), 11)
        for relation, relative in generated:
            self.assertNotEqual(relation.get("api_surface"), "dynamic_only")
            validate_generated_relation_header(
                relation, ROOT / relative, label=relative.as_posix()
            )

    def test_dynamic_only_relation_is_not_a_cpp_generation_surface(self) -> None:
        dynamic = next(
            row
            for row in self.registry["relations"]
            if row["name"] == "frontend.clang22.entity_observation"
        )
        self.assertEqual(dynamic["api_surface"], "dynamic_only")
        self.assertIsNone(dynamic["generated_cpp_tag"])
        with self.assertRaisesRegex(ValueError, "dynamic-only relation"):
            render(dynamic)

        with tempfile.TemporaryDirectory(
            prefix="cxxlens-dynamic-relation-test-"
        ) as directory:
            output = pathlib.Path(directory) / "forbidden.hpp"
            completed = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/sdk/relation_idl_compiler.py"),
                    "--registry",
                    str(ROOT / "schemas/cxxlens_ng_relation_registry.yaml"),
                    "--relation",
                    str(dynamic["name"]),
                    "--output",
                    str(output),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(completed.returncode, 2)
            self.assertIn("dynamic-only relation", completed.stderr)
            self.assertFalse(output.exists())

    def test_dynamic_row_constraint_canonicalization_ignores_set_order(self) -> None:
        left = next(
            copy.deepcopy(row)
            for row in self.registry["relations"]
            if row["name"] == "frontend.clang22.call_observation"
        )
        right = copy.deepcopy(left)
        right["row_constraints"]["all_or_none"][0].reverse()
        self.assertEqual(canonical_relation(left), canonical_relation(right))

    def test_sdk_binding_rejects_unclassified_null_generated_tag(self) -> None:
        registry = copy.deepcopy(self.registry)
        dynamic = next(
            row
            for row in registry["relations"]
            if row["name"] == "frontend.clang22.entity_observation"
        )
        dynamic.pop("api_surface")
        with self.assertRaisesRegex(
            SdkContractError, "tag/dynamic-only classification differs"
        ):
            admitted_generated_relations(self.catalog, registry)

    def test_manual_edit_of_generated_header_is_rejected(self) -> None:
        relation, relative = next(
            row
            for row in admitted_generated_relations(self.catalog, self.registry)
            if row[0]["name"] == "cc.entity"
        )
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-generated-header-test-"
        ) as directory:
            candidate = pathlib.Path(directory) / relative.name
            candidate.write_text(
                (ROOT / relative).read_text(encoding="utf-8")
                + "// forbidden manual edit\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                SdkContractError, "committed generated relation is stale"
            ):
                validate_generated_relation_header(
                    relation, candidate, label=relative.as_posix()
                )

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

    def test_project_catalog_cannot_alias_catalog_and_relation_unit_ids(self) -> None:
        contract = copy.deepcopy(self.project_catalog_contract)
        contract["value_types"]["compile_unit_entry"]["identity"] = (
            "final-build-compile-unit-relation-id"
        )
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(self.project_catalog_schema).validate(contract)

        contract = copy.deepcopy(self.project_catalog_contract)
        contract["identity_boundary"]["implicit_equality_alias"] = "required"
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(self.project_catalog_schema).validate(contract)

        contract = copy.deepcopy(self.project_catalog_contract)
        contract["consumers"].pop("build_compile_unit")
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(self.project_catalog_schema).validate(contract)

    def test_project_catalog_worker_decomposition_is_exact(self) -> None:
        worker = "auto request = decode_task_input(validated->payload);"
        decoder = "auto catalog = sdk::project_catalog::make(root, digest, units);"
        validate_project_catalog_worker_decomposition(worker, decoder)

        with self.assertRaisesRegex(SdkContractError, "task.v3 decoder"):
            validate_project_catalog_worker_decomposition("", decoder)
        with self.assertRaisesRegex(SdkContractError, "shared project catalog loader"):
            validate_project_catalog_worker_decomposition(worker, "")

    def test_store_identity_decomposition_is_exact(self) -> None:
        store = """
#include "store_identity_internal.hpp"
std::string snapshot_identity(const snapshot_manifest& value) {
    return *detail::snapshot_manifest_identity(value);
}
std::string publication_identity(const publication_record& value) {
    return *detail::publication_record_identity(value.series_id, value.snapshot_id, 1, {});
}
"""
        identity = """
result<std::string> snapshot_manifest_identity(const snapshot_manifest& value) {
    return canonical_identity_digest("snapshot", fields);
}
result<std::string> publication_record_identity(const std::string& series_id) {
    return canonical_identity_digest("publication", fields);
}
"""
        validate_store_identity_decomposition(store, identity)

        with self.assertRaisesRegex(SdkContractError, "snapshot identity wrapper bypasses"):
            validate_store_identity_decomposition(
                store.replace(
                    "detail::snapshot_manifest_identity(value)",
                    'canonical_identity_digest("snapshot", fields)',
                ),
                identity,
            )
        with self.assertRaisesRegex(SdkContractError, "canonical snapshot tuple"):
            validate_store_identity_decomposition(
                store,
                identity.replace(
                    'canonical_identity_digest("snapshot", fields)',
                    'canonical_identity_digest("legacy-snapshot", fields)',
                ),
            )

    def test_provider_task_projection_cannot_drop_condition(self) -> None:
        contract = copy.deepcopy(self.provider_task_contract)
        contract["task_projection"]["fields"].remove("condition")
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(self.provider_task_schema).validate(contract)

    def test_provider_task_id_cannot_become_execution_occurrence_identity(self) -> None:
        contract = copy.deepcopy(self.provider_task_contract)
        contract["execution_identity"]["task_id_scope"] = "execution-occurrence"
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(self.provider_task_schema).validate(contract)

        contract = copy.deepcopy(self.provider_task_contract)
        contract["execution_identity"]["result_correlation"] = "task-id-only"
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
        duplicate = copy.deepcopy(row)
        duplicate["contributor_guarantees"].append(
            copy.deepcopy(duplicate["contributor_guarantees"][0])
        )
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(row_schema).validate(duplicate)

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
