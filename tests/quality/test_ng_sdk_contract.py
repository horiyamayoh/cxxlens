#!/usr/bin/env python3
"""Fail-closed catalog tests for the next-generation author SDK."""

from __future__ import annotations

import copy
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

import jsonschema


ROOT = pathlib.Path(__file__).resolve().parents[2]
DOCTOR_MAX_TEXT_LENGTH = 512
DOCTOR_MAX_COLLECTION_ITEMS = 128
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_sdk_contract import (  # noqa: E402
    SdkContractError,
    admitted_generated_relations,
    canonical_relation,
    load_yaml,
    validate_boundaries,
    validate_catalog,
)
from relation_idl_compiler import render  # noqa: E402


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
        cls.doctor_catalog = load_yaml(
            ROOT / "schemas/cxxlens_ng_sdk_doctor_catalog.yaml"
        )
        cls.doctor_catalog_schema = load_yaml(
            ROOT / "schemas/cxxlens_ng_sdk_doctor_catalog.schema.yaml"
        )
        cls.doctor_project_schema = load_yaml(
            ROOT / "schemas/cxxlens_ng_sdk_doctor_project.schema.yaml"
        )
        cls.doctor_resolution_schema = load_yaml(
            ROOT / "schemas/cxxlens_ng_sdk_doctor_resolution.schema.yaml"
        )
        cls.doctor_relation_presence_schema = load_yaml(
            ROOT / "schemas/cxxlens_ng_sdk_doctor_relation_presence.schema.yaml"
        )
        cls.support_matrix = load_yaml(ROOT / "schemas/cxxlens_support_matrix.yaml")

    @staticmethod
    def doctor_project() -> dict[str, object]:
        catalog_digest = "semantic-v2:sha256:" + "c" * 64
        candidate = {
            "candidate_id": "semantic-v2:sha256:" + "a" * 64,
            "provider_id": "cxxlens.clang22.reference",
            "provider_version": "2.0.0",
            "package_identity": "cxxlens.clang22.package",
            "provider_manifest_digest": "sha256:" + "1" * 64,
            "provider_binary_digest": "sha256:" + "2" * 64,
            "provider_semantic_contract_digest": "sha256:" + "3" * 64,
            "protocol": {"major": 2, "minor": 0},
            "features": ["task-input-chunks-v2", "task-source-closure-v2"],
            "relations": ["cc.call_site.v1", "cc.entity.v1"],
            "interpretations": ["cc.clang22-canonical-1"],
            "sandbox": {
                "minimum": "enforced",
                "policy_digest": "sha256:" + "4" * 64,
            },
            "trust": {
                "state": "verified",
                "registry_sequence": 7,
                "certificate_id": "certificate.provider.one",
                "trust_anchor_id": "cxxlens.production-root.v1",
                "signature_digest": "sha256:" + "5" * 64,
                "revocation": {
                    "state": "not-revoked",
                    "effective_sequence": None,
                    "reason": None,
                },
            },
        }
        return {
            "schema": "cxxlens.sdk-doctor-project.v2",
            "document_version": "2.0.0",
            "project": {
                "project_id": "project.example",
                "catalog_id": "catalog:" + catalog_digest,
                "catalog_digest": catalog_digest,
                "logical_root": "project://example",
                "environment_digest": "sha256:" + "6" * 64,
                "environment": {
                    "release_version": "1.0.0",
                    "surface": "provider-sdk",
                    "os": "linux",
                    "architecture": "x86_64",
                    "compiler_provider_major": "clang22",
                    "linkage": "static",
                },
                "source_input": {
                    "source_snapshot_id": "source-snapshot.example",
                    "compilation_database_id": "compilation-database.example",
                },
                "provider_candidates": [candidate],
                "store": {"backend": "memory", "format": "cxxlens.snapshot.v3"},
            },
        }

    @staticmethod
    def doctor_proved_resolution() -> dict[str, object]:
        return {
            "schema": "cxxlens.sdk-doctor-resolution.v2",
            "document_version": "2.0.0",
            "catalog_binding": {
                "id": "cxxlens.sdk-doctor-catalog.v1",
                "document_version": "1.0.0",
            },
            "use_case_id": "cxxlens.clang22.materialize-and-query.v1",
            "consumer": "semantic-query-consumer",
            "question": "Can this project be materialized?",
            "result": {
                "state": "proved",
                "reason_code": "doctor.none",
                "explanation": "Every capability is available.",
                "guarantee": "The declared product context is sufficient.",
            },
            "capability_path": [
                {
                    "id": "input.project-catalog.v1",
                    "kind": "input",
                    "requires": [],
                    "state": "proved",
                    "reason_code": "doctor.none",
                }
            ],
            "missing": [],
            "completion_plan": [],
            "preserved_semantics": {
                "closure": ["dependency-graph-closed"],
                "conflict": [],
                "coverage": ["input.project-catalog.v1"],
                "differential_disagreement": [],
                "guarantee": ["declared-context-only"],
                "logical_explain": ["input.project-catalog.v1"],
                "physical_explain": [],
                "provenance": ["cxxlens.sdk-doctor-catalog.v1"],
                "unresolved": [],
            },
        }

    @staticmethod
    def doctor_relation_presence() -> dict[str, object]:
        return {
            "schema": "cxxlens.sdk-doctor-relation-presence.v2",
            "document_version": "2.0.0",
            "catalog_binding": {
                "id": "cxxlens.sdk-doctor-catalog.v1",
                "document_version": "1.0.0",
            },
            "mode": "relation-presence",
            "requested": 1,
            "missing": 0,
            "state": "proved",
            "components": [
                {"id": "cc.entity.v1", "state": "proved", "reason_code": "none"}
            ],
        }

    def test_catalog_and_ordinary_boundary_are_valid(self) -> None:
        validate_catalog(ROOT, self.catalog)
        validate_boundaries(ROOT)

    def test_missing_author_path_is_rejected(self) -> None:
        catalog = copy.deepcopy(self.catalog)
        catalog["author_paths"] = []
        with self.assertRaisesRegex(SdkContractError, "schema validation"):
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
        self.assertGreater(len(generated), 0)
        for relation, relative in generated:
            self.assertEqual(relation.get("cpp_projection"), "installed-static")
            self.assertTrue((ROOT / relative).is_file())

    def test_dynamic_only_relation_is_not_a_cpp_generation_surface(self) -> None:
        dynamic = next(
            row
            for row in self.registry["relations"]
            if row["name"] == "frontend.clang22.entity_observation"
        )
        self.assertEqual(dynamic["cpp_projection"], "dynamic-only")
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

    def test_dynamic_descriptor_digests_preserve_registry_1_4_bindings(self) -> None:
        expected = {
            "frontend.clang22.call_observation":
                "07ea48a7f00e80972ba59c14ee96f916772ad9ed57fc84e313e3958f08fa548a",
            "frontend.clang22.entity_observation":
                "4a5012801fcde26110a9f6350177d74d7d6975edde96337d4d3918ca7a004d51",
            "frontend.clang22.type_observation":
                "53c54f967eb041e75ea98463c212d259fed0d3a310038ac9c93209749e72387f",
        }
        actual = {}
        for relation in self.registry["relations"]:
            if relation["name"] not in expected:
                continue
            payload = json.dumps(
                canonical_relation(relation),
                ensure_ascii=False,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("utf-8")
            actual[relation["name"]] = hashlib.sha256(payload).hexdigest()
        self.assertEqual(actual, expected)

    def test_sdk_binding_rejects_unclassified_null_generated_tag(self) -> None:
        registry = copy.deepcopy(self.registry)
        dynamic = next(
            row
            for row in registry["relations"]
            if row["name"] == "frontend.clang22.entity_observation"
        )
        dynamic.pop("cpp_projection")
        with self.assertRaisesRegex(
            SdkContractError, "tag/projection classification differs"
        ):
            admitted_generated_relations(self.catalog, registry)

    def test_sdk_binding_rejects_unadmitted_installed_static_relation(self) -> None:
        catalog = copy.deepcopy(self.catalog)
        header = "include/cxxlens/relations/source_origin.hpp"
        entry = next(
            row for row in catalog["entries"] if row["id"] == "public.relation-static"
        )
        entry["headers"].remove(header)
        with self.assertRaisesRegex(
            SdkContractError, "installed-static registry headers lack catalog admission"
        ):
            admitted_generated_relations(catalog, self.registry)

    def test_lifetime_contract_cannot_be_omitted(self) -> None:
        catalog = copy.deepcopy(self.catalog)
        catalog["entries"][0].pop("lifetime")
        with self.assertRaisesRegex(SdkContractError, "schema validation"):
            validate_catalog(ROOT, catalog)

    def test_catalog_keeps_product_semantics_without_governance_metadata(self) -> None:
        for entry in self.catalog["entries"]:
            self.assertNotIn("implementation_evidence", entry)
        for path in self.catalog["author_paths"]:
            self.assertNotIn("implementation", path)
            self.assertNotIn("harness", path)
            self.assertIn("positive_example", path)
            self.assertIn("negative_example", path)

    def test_doctor_catalog_and_document_schemas_are_valid(self) -> None:
        schemas = (
            self.doctor_catalog_schema,
            self.doctor_project_schema,
            self.doctor_resolution_schema,
            self.doctor_relation_presence_schema,
        )
        for schema in schemas:
            jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(self.doctor_catalog_schema).validate(
            self.doctor_catalog
        )

        capability_ids: set[str] = set()
        capability_by_id: dict[str, dict[str, object]] = {}
        for capability in self.doctor_catalog["capabilities"]:
            self.assertNotIn(capability["id"], capability_ids)
            self.assertTrue(set(capability["requires"]).issubset(capability_ids))
            capability_ids.add(capability["id"])
            capability_by_id[capability["id"]] = capability
        use_case_ids: set[str] = set()
        for use_case in self.doctor_catalog["use_cases"]:
            self.assertNotIn(use_case["id"], use_case_ids)
            use_case_ids.add(use_case["id"])
            path = use_case["capability_path"]
            self.assertTrue(set(path).issubset(capability_ids))
            path_positions = {
                capability_id: index for index, capability_id in enumerate(path)
            }
            for capability_id in path:
                self.assertTrue(
                    all(
                        dependency in path_positions
                        and path_positions[dependency] < path_positions[capability_id]
                        for dependency in capability_by_id[capability_id]["requires"]
                    )
                )

        tuple_fields = self.doctor_catalog["provider_support"][
            "support_tuple_fields"
        ]
        catalog_tuples = {
            tuple(row[field] for field in tuple_fields)
            for row in self.doctor_catalog["provider_support"]["supported_tuples"]
        }
        matrix_tuples = {
            tuple(row[field] for field in tuple_fields)
            for row in self.support_matrix["entries"]
        }
        self.assertEqual(catalog_tuples, matrix_tuples)

        command_ids = [command["id"] for command in self.doctor_catalog["commands"]]
        self.assertEqual(len(command_ids), len(set(command_ids)))
        self.assertEqual(set(command_ids), {"relation-presence", "missing"})

        non_product = copy.deepcopy(self.doctor_catalog)
        non_product["non_product_field"] = {}
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(self.doctor_catalog_schema).validate(
                non_product
            )

    def test_doctor_schemas_declare_runtime_resource_bounds(self) -> None:
        schemas = {
            "catalog": self.doctor_catalog_schema,
            "project": self.doctor_project_schema,
            "resolution": self.doctor_resolution_schema,
            "relation-presence": self.doctor_relation_presence_schema,
        }

        def inspect(value: object, schema_name: str, path: str) -> None:
            if isinstance(value, dict):
                value_type = value.get("type")
                with self.subTest(schema=schema_name, path=path):
                    if value_type == "array":
                        self.assertEqual(
                            value.get("maxItems"), DOCTOR_MAX_COLLECTION_ITEMS
                        )
                    elif value_type == "object":
                        self.assertEqual(
                            value.get("maxProperties"),
                            DOCTOR_MAX_COLLECTION_ITEMS,
                        )
                        self.assertIs(value.get("additionalProperties"), False)
                    elif value_type == "string":
                        if "maxLength" in value:
                            self.assertLessEqual(
                                value["maxLength"], DOCTOR_MAX_TEXT_LENGTH
                            )
                        elif isinstance(value.get("const"), str):
                            self.assertLessEqual(
                                len(value["const"]), DOCTOR_MAX_TEXT_LENGTH
                            )
                        else:
                            enum_values = value.get("enum")
                            self.assertIsInstance(enum_values, list)
                            self.assertTrue(
                                enum_values
                                and all(
                                    isinstance(item, str)
                                    and len(item) <= DOCTOR_MAX_TEXT_LENGTH
                                    for item in enum_values
                                )
                            )
                for key, child in value.items():
                    inspect(child, schema_name, f"{path}/{key}")
            elif isinstance(value, list):
                for index, child in enumerate(value):
                    inspect(child, schema_name, f"{path}/{index}")

        for schema_name, schema in schemas.items():
            inspect(schema, schema_name, "$")

    def test_doctor_project_schema_enforces_runtime_resource_boundaries(self) -> None:
        validator = jsonschema.Draft202012Validator(self.doctor_project_schema)

        maximum_text = self.doctor_project()
        prefix = "project://"
        maximum_text["project"]["logical_root"] = prefix + "x" * (
            DOCTOR_MAX_TEXT_LENGTH - len(prefix)
        )
        validator.validate(maximum_text)
        too_much_text = copy.deepcopy(maximum_text)
        too_much_text["project"]["logical_root"] += "x"
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(too_much_text)

        maximum_candidates = self.doctor_project()
        candidate_template = maximum_candidates["project"]["provider_candidates"][0]
        maximum_candidates["project"]["provider_candidates"] = []
        for index in range(DOCTOR_MAX_COLLECTION_ITEMS):
            candidate = copy.deepcopy(candidate_template)
            candidate["candidate_id"] = (
                "semantic-v2:sha256:" + f"{index:064x}"
            )
            maximum_candidates["project"]["provider_candidates"].append(candidate)
        validator.validate(maximum_candidates)
        too_many_candidates = copy.deepcopy(maximum_candidates)
        extra_candidate = copy.deepcopy(candidate_template)
        extra_candidate["candidate_id"] = (
            "semantic-v2:sha256:"
            + f"{DOCTOR_MAX_COLLECTION_ITEMS:064x}"
        )
        too_many_candidates["project"]["provider_candidates"].append(
            extra_candidate
        )
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(too_many_candidates)

        maximum_features = self.doctor_project()
        maximum_features["project"]["provider_candidates"][0]["features"] = [
            f"feature.{index}" for index in range(DOCTOR_MAX_COLLECTION_ITEMS)
        ]
        validator.validate(maximum_features)
        too_many_features = copy.deepcopy(maximum_features)
        too_many_features["project"]["provider_candidates"][0]["features"].append(
            f"feature.{DOCTOR_MAX_COLLECTION_ITEMS}"
        )
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(too_many_features)

    def test_doctor_catalog_schema_enforces_runtime_resource_boundaries(self) -> None:
        validator = jsonschema.Draft202012Validator(self.doctor_catalog_schema)

        maximum_question = copy.deepcopy(self.doctor_catalog)
        maximum_question["use_cases"][0]["question"] = (
            "x" * DOCTOR_MAX_TEXT_LENGTH
        )
        validator.validate(maximum_question)
        too_much_question = copy.deepcopy(maximum_question)
        too_much_question["use_cases"][0]["question"] += "x"
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(too_much_question)

        maximum_use_cases = copy.deepcopy(self.doctor_catalog)
        use_case_template = maximum_use_cases["use_cases"][0]
        maximum_use_cases["use_cases"] = []
        for index in range(DOCTOR_MAX_COLLECTION_ITEMS):
            use_case = copy.deepcopy(use_case_template)
            use_case["id"] = f"use.case.{index}"
            maximum_use_cases["use_cases"].append(use_case)
        validator.validate(maximum_use_cases)
        too_many_use_cases = copy.deepcopy(maximum_use_cases)
        extra_use_case = copy.deepcopy(use_case_template)
        extra_use_case["id"] = f"use.case.{DOCTOR_MAX_COLLECTION_ITEMS}"
        too_many_use_cases["use_cases"].append(extra_use_case)
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(too_many_use_cases)

    def test_doctor_output_schemas_enforce_runtime_resource_boundaries(self) -> None:
        resolution_validator = jsonschema.Draft202012Validator(
            self.doctor_resolution_schema
        )
        maximum_resolution = self.doctor_proved_resolution()
        maximum_resolution["question"] = "x" * DOCTOR_MAX_TEXT_LENGTH
        maximum_resolution["capability_path"] = [
            {
                "id": f"capability.{index}",
                "kind": "input",
                "requires": [],
                "state": "proved",
                "reason_code": "doctor.none",
            }
            for index in range(DOCTOR_MAX_COLLECTION_ITEMS)
        ]
        maximum_resolution["preserved_semantics"]["coverage"] = [
            f"capability.{index}" for index in range(DOCTOR_MAX_COLLECTION_ITEMS)
        ]
        resolution_validator.validate(maximum_resolution)

        too_much_resolution_text = copy.deepcopy(maximum_resolution)
        too_much_resolution_text["question"] += "x"
        with self.assertRaises(jsonschema.ValidationError):
            resolution_validator.validate(too_much_resolution_text)

        too_many_capabilities = copy.deepcopy(maximum_resolution)
        too_many_capabilities["capability_path"].append(
            {
                "id": f"capability.{DOCTOR_MAX_COLLECTION_ITEMS}",
                "kind": "input",
                "requires": [],
                "state": "proved",
                "reason_code": "doctor.none",
            }
        )
        with self.assertRaises(jsonschema.ValidationError):
            resolution_validator.validate(too_many_capabilities)

        too_much_coverage = copy.deepcopy(maximum_resolution)
        too_much_coverage["preserved_semantics"]["coverage"].append(
            f"capability.{DOCTOR_MAX_COLLECTION_ITEMS}"
        )
        with self.assertRaises(jsonschema.ValidationError):
            resolution_validator.validate(too_much_coverage)

        relation_validator = jsonschema.Draft202012Validator(
            self.doctor_relation_presence_schema
        )
        maximum_relations = self.doctor_relation_presence()
        maximum_relations["requested"] = DOCTOR_MAX_COLLECTION_ITEMS
        maximum_relations["components"] = [
            {
                "id": f"cc.entity_{index}.v1",
                "state": "proved",
                "reason_code": "none",
            }
            for index in range(DOCTOR_MAX_COLLECTION_ITEMS)
        ]
        relation_validator.validate(maximum_relations)

        too_many_relation_components = copy.deepcopy(maximum_relations)
        too_many_relation_components["components"].append(
            {
                "id": f"cc.entity_{DOCTOR_MAX_COLLECTION_ITEMS}.v1",
                "state": "proved",
                "reason_code": "none",
            }
        )
        with self.assertRaises(jsonschema.ValidationError):
            relation_validator.validate(too_many_relation_components)

        too_many_requested_relations = copy.deepcopy(maximum_relations)
        too_many_requested_relations["requested"] = (
            DOCTOR_MAX_COLLECTION_ITEMS + 1
        )
        with self.assertRaises(jsonschema.ValidationError):
            relation_validator.validate(too_many_requested_relations)

        too_many_missing_relations = copy.deepcopy(maximum_relations)
        too_many_missing_relations["missing"] = DOCTOR_MAX_COLLECTION_ITEMS + 1
        with self.assertRaises(jsonschema.ValidationError):
            relation_validator.validate(too_many_missing_relations)

    def test_doctor_project_accepts_zero_or_more_typed_provider_candidates(self) -> None:
        validator = jsonschema.Draft202012Validator(self.doctor_project_schema)
        project = self.doctor_project()
        validator.validate(project)

        no_provider = copy.deepcopy(project)
        no_provider["project"]["provider_candidates"] = []
        validator.validate(no_provider)

        second_provider = copy.deepcopy(
            project["project"]["provider_candidates"][0]
        )
        second_provider["candidate_id"] = "semantic-v2:sha256:" + "b" * 64
        second_provider["provider_binary_digest"] = "sha256:" + "7" * 64
        project["project"]["provider_candidates"].append(second_provider)
        validator.validate(project)

        opaque_identity = self.doctor_project()
        opaque_identity["project"]["provider_candidates"][0]["candidate_id"] = (
            "provider.candidate.one"
        )
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(opaque_identity)

    def test_doctor_project_rejects_incomplete_or_self_inconsistent_authority(self) -> None:
        validator = jsonschema.Draft202012Validator(self.doctor_project_schema)

        for required in (
            "provider_manifest_digest",
            "provider_binary_digest",
            "provider_semantic_contract_digest",
            "protocol",
            "features",
            "relations",
            "interpretations",
            "sandbox",
            "trust",
        ):
            project = self.doctor_project()
            project["project"]["provider_candidates"][0].pop(required)
            with self.subTest(required=required), self.assertRaises(
                jsonschema.ValidationError
            ):
                validator.validate(project)

        revoked = self.doctor_project()
        revoked_trust = revoked["project"]["provider_candidates"][0]["trust"]
        revoked_trust["revocation"] = {
            "state": "revoked",
            "effective_sequence": 8,
            "reason": "provider-key-compromise",
        }
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(revoked)

        missing_tuple = self.doctor_project()
        missing_tuple["project"]["environment"].pop("linkage")
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(missing_tuple)

    def test_doctor_documents_reject_non_product_fields(self) -> None:
        validator = jsonschema.Draft202012Validator(self.doctor_project_schema)
        project = self.doctor_project()
        project["project"]["non_product_field"] = {}
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(project)

    def test_doctor_resolution_schema_admits_typed_reachable_conflict(self) -> None:
        resolution = {
            "schema": "cxxlens.sdk-doctor-resolution.v2",
            "document_version": "2.0.0",
            "catalog_binding": {
                "id": "cxxlens.sdk-doctor-catalog.v1",
                "document_version": "1.0.0",
            },
            "use_case_id": "cxxlens.clang22.materialize-and-query.v1",
            "consumer": "semantic-query-consumer",
            "question": "Can this project be materialized?",
            "result": {
                "state": "conflicting",
                "reason_code": "doctor.conflicting-capability",
                "explanation": "Two valid provider authorities disagree.",
                "guarantee": "No provider is selected from a conflict.",
            },
            "capability_path": [
                {
                    "id": "provider.protocol.v2",
                    "kind": "provider",
                    "requires": ["input.project-catalog.v1"],
                    "state": "conflicting",
                    "reason_code": "doctor.conflicting-capability",
                }
            ],
            "missing": [
                {
                    "capability_id": "provider.protocol.v2",
                    "reason_code": "doctor.conflicting-capability",
                    "explanation": "Provider authority is ambiguous.",
                }
            ],
            "completion_plan": [
                {
                    "id": "completion.provider.protocol.v2",
                    "requires": ["input.project-catalog.v1"],
                    "action": "Resolve the provider authority conflict.",
                    "unlocks": "provider.protocol.v2",
                }
            ],
            "preserved_semantics": {
                "closure": ["dependency-graph-open"],
                "conflict": [
                    {
                        "capability_id": "provider.protocol.v2",
                        "candidate_ids": [
                            "semantic-v2:sha256:" + "a" * 64,
                            "semantic-v2:sha256:" + "b" * 64,
                        ],
                        "reason_code": "doctor.conflicting-capability",
                    }
                ],
                "coverage": ["provider.protocol.v2"],
                "differential_disagreement": [],
                "guarantee": ["no-conflict-fallback"],
                "logical_explain": ["provider.protocol.v2"],
                "physical_explain": [],
                "provenance": ["cxxlens.sdk-doctor-catalog.v1"],
                "unresolved": ["provider.protocol.v2"],
            },
        }
        validator = jsonschema.Draft202012Validator(self.doctor_resolution_schema)
        validator.validate(resolution)

        duplicate = copy.deepcopy(resolution)
        duplicate["preserved_semantics"]["conflict"][0]["candidate_ids"] = [
            "semantic-v2:sha256:" + "a" * 64,
            "semantic-v2:sha256:" + "a" * 64,
        ]
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(duplicate)

        missing_conflict = copy.deepcopy(resolution)
        missing_conflict["preserved_semantics"]["conflict"] = []
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(missing_conflict)

        wrong_conflict_reason = copy.deepcopy(resolution)
        wrong_conflict_reason["result"]["reason_code"] = "doctor.unknown"
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(wrong_conflict_reason)

        conflict_on_proved = copy.deepcopy(resolution)
        conflict_on_proved["result"]["state"] = "proved"
        conflict_on_proved["result"]["reason_code"] = "doctor.none"
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(conflict_on_proved)

    def test_doctor_relation_presence_schema_binds_product_catalog(self) -> None:
        document = {
            "schema": "cxxlens.sdk-doctor-relation-presence.v2",
            "document_version": "2.0.0",
            "catalog_binding": {
                "id": "cxxlens.sdk-doctor-catalog.v1",
                "document_version": "1.0.0",
            },
            "mode": "relation-presence",
            "requested": 1,
            "missing": 0,
            "state": "proved",
            "components": [
                {"id": "cc.entity.v1", "state": "proved", "reason_code": "none"}
            ],
        }
        jsonschema.Draft202012Validator(
            self.doctor_relation_presence_schema
        ).validate(document)

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
