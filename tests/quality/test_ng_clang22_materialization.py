#!/usr/bin/env python3
"""Positive and fail-closed tests for installed Clang 22 materialization."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))
sys.path.insert(0, str(ROOT / "tools" / "sdk"))

import check_ng_clang22_materialization as materialization  # noqa: E402
from relation_idl_compiler import (  # noqa: E402
    canonical_relation as idl_canonical_relation,
)


class NgClang22MaterializationTests(unittest.TestCase):
    def request(
        self,
        configuration: str = "static",
        backend: str = "memory",
        translation_unit_count: int = 1,
    ) -> dict:
        return materialization.sample_request(
            ROOT,
            configuration=configuration,
            backend=backend,
            translation_unit_count=translation_unit_count,
        )

    def report(self, request: dict) -> dict:
        return materialization.sample_report(ROOT, request)

    def matrix(self) -> list[tuple[dict, dict]]:
        entries = []
        for configuration in ("static", "shared"):
            for backend in ("memory", "sqlite"):
                request = self.request(configuration, backend)
                entries.append((request, self.report(request)))
        return entries

    def test_strict_json_loader_rejects_lexical_ambiguity(self) -> None:
        self.assertEqual(
            materialization.load_strict_json_bytes(
                b' {"outer":{"value":1},"items":[true,null]} \n',
                "request",
            ),
            {"outer": {"value": 1}, "items": [True, None]},
        )
        invalid = {
            "top-duplicate": b'{"value":1,"value":2}',
            "nested-duplicate": b'{"outer":{"value":1,"value":2}}',
            "invalid-utf8": b'{"value":"\xff"}',
            "second-value": b'{} {}',
            "trailing-garbage": b'{}x',
            "bom": b'\xef\xbb\xbf{}',
            "non-finite": b'{"value":NaN}',
            "invalid-unicode-scalar": b'{"value":"\\ud800"}',
        }
        for label, raw in invalid.items():
            with self.subTest(label=label):
                with self.assertRaises(materialization.MaterializationError) as caught:
                    materialization.load_strict_json_bytes(raw, "request")
                self.assertEqual(caught.exception.code, "materialization.request-invalid")
                self.assertIn("request: invalid JSON lexical form:", str(caught.exception))
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "request: expected one top-level JSON object",
        ):
            materialization.load_strict_json_bytes(b"[]", "request")

        report_invalid = {
            "duplicate": b'{"value":1,"value":2}',
            "invalid-utf8": b'{"value":"\xff"}',
            "trailing-value": b'{} {}',
            "non-object": b'[]',
        }
        for label, raw in report_invalid.items():
            with self.subTest(report_lexical=label):
                with self.assertRaises(materialization.MaterializationError) as caught:
                    materialization.load_strict_json_bytes(raw, "report")
                self.assertEqual(
                    caught.exception.code, "materialization.report-invalid"
                )

    def test_report_schema_errors_use_report_invalid_family(self) -> None:
        request = self.request()
        mutations = {
            "unknown": lambda report: report.__setitem__("unknown", True),
            "missing": lambda report: report.pop("provider"),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label):
                report = self.report(request)
                mutate(report)
                with self.assertRaises(materialization.MaterializationError) as caught:
                    materialization.validate_report(ROOT, request, report)
                self.assertEqual(
                    caught.exception.code,
                    "materialization.report-invalid",
                )

    def test_report_schema_binds_exact_observation_equivalence(self) -> None:
        request = self.request()
        schema = materialization.load(ROOT / materialization.REPORT_SCHEMA)

        coupled = self.report(request)
        type_batch = next(
            batch
            for batch in coupled["task_results"][0]["batches"]
            if batch["descriptor_id"]
            == "frontend.clang22.type_observation.v2"
        )
        type_batch["observation_equivalence_census"]["rows"][0][
            "limitation"
        ] = "exact rows cannot carry limitations"
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_schema(coupled, schema, "materialization report")

        nonexact = self.report(request)
        type_batch = next(
            batch
            for batch in nonexact["task_results"][0]["batches"]
            if batch["descriptor_id"]
            == "frontend.clang22.type_observation.v2"
        )
        row = type_batch["observation_equivalence_census"]["rows"][0]
        limitation = "type sugar cannot be reconstructed exactly"
        row.update(
            {
                "exact_equivalence": False,
                "limitation": limitation,
                "limitation_digest": materialization.content_digest(
                    limitation.encode("utf-8")
                ),
            }
        )
        type_batch["row_bindings"][0].update(
            {
                "exact_equivalence": False,
                "limitation_digest": row["limitation_digest"],
            }
        )
        materialization.rebind_report_digest_chain(ROOT, request, nonexact)
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_schema(nonexact, schema, "materialization report")

        reordered = self.report(request)
        reordered["side_channels"]["guarantee"][
            "observation_descriptor_censuses"
        ].reverse()
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_schema(reordered, schema, "materialization report")

    def test_repository_contract_and_exact_installed_matrix_pass(self) -> None:
        contract = materialization.validate_documents(ROOT)
        self.assertEqual(
            contract["surface"]["executable"], "cxxlens-clang22-materialize"
        )
        self.assertEqual(
            contract["surface"]["public_cpp_api"],
            "none",
        )
        materialization.validate_qualification_matrix(ROOT, self.matrix())

    def test_registry_descriptor_ids_and_both_digests_are_dynamic_and_exact(self) -> None:
        _, bindings = materialization.descriptor_bindings(ROOT)
        self.assertEqual(
            [row["descriptor_id"] for row in bindings],
            materialization.DESCRIPTOR_IDS,
        )
        self.assertEqual(
            [row["descriptor_version"] for row in bindings[3:]],
            ["2.0.0", "2.0.0", "2.0.0"],
        )
        self.assertTrue(
            all(row["contract_digest"].startswith("sha256:") for row in bindings)
        )
        self.assertTrue(
            all(
                row["runtime_descriptor_digest"].startswith("semantic-v2:sha256:")
                for row in bindings
            )
        )

    def test_observation_v2_native_codec_closes_payload_origin_and_row_identity(
        self,
    ) -> None:
        task = self.request()["tasks"][0]
        source = task["source"]
        span = {
            "span_id": materialization.source_span_identity(
                source["source_snapshot_id"],
                source["file_id"],
                0,
                3,
                "declaration",
            ),
            "snapshot": source["source_snapshot_id"],
            "file": source["file_id"],
            "begin": 0,
            "end": 3,
            "role": "declaration",
            "read_only": True,
        }
        record = {
            "kind": "entity",
            "final_relation_compile_unit_id": task["compile_unit_id"],
            "semantic_key": "clang-usr:c:@F@f#",
            "payload": {
                "symbol.qualified_name": "f",
                "symbol.kind": "function",
            },
            "primary_span": span,
            "origin_chain": [
                {
                    "kind": "macro-spelling-begin",
                    "logical_path": "include/macro.hpp",
                    "begin": 7,
                    "end": 8,
                    "read_only": True,
                },
                {
                    "kind": "macro-spelling-end",
                    "logical_path": "include/outer.hpp",
                    "begin": 11,
                    "end": 12,
                    "read_only": True,
                },
            ],
            "exact_equivalence": True,
            "limitation": None,
        }
        descriptor, row = materialization.observation_v2_native_row(
            ROOT,
            record,
            task,
        )
        self.assertEqual(descriptor, "frontend.clang22.entity_observation.v2")
        self.assertEqual(row["semantic_key"], record["semantic_key"].encode("utf-8"))
        self.assertEqual(row["source"], span["span_id"])
        self.assertNotIn("limitation", row)
        self.assertEqual(
            row["source_origin_chain"],
            materialization.observation_v2_origin_chain_bytes(record["origin_chain"]),
        )
        registry = materialization.load(ROOT / materialization.REGISTRY)
        relation = next(
            relation
            for relation in registry["relations"]
            if relation["descriptor_id"] == descriptor
        )
        self.assertEqual(
            row["observation"],
            materialization.derive_registry_row_identity(relation, row),
        )

        payload_reordered = copy.deepcopy(record)
        payload_reordered["payload"] = {
            "symbol.kind": "function",
            "symbol.qualified_name": "f",
        }
        _, reordered_row = materialization.observation_v2_native_row(
            ROOT,
            payload_reordered,
            task,
        )
        self.assertEqual(reordered_row, row)

        payload_changed = copy.deepcopy(record)
        payload_changed["payload"]["symbol.kind"] = "variable"
        _, changed_payload_row = materialization.observation_v2_native_row(
            ROOT,
            payload_changed,
            task,
        )
        self.assertNotEqual(changed_payload_row["payload_digest"], row["payload_digest"])
        self.assertNotEqual(changed_payload_row["observation"], row["observation"])

        origin_reversed = copy.deepcopy(record)
        origin_reversed["origin_chain"].reverse()
        _, reversed_origin_row = materialization.observation_v2_native_row(
            ROOT,
            origin_reversed,
            task,
        )
        self.assertNotEqual(
            reversed_origin_row["source_origin_chain"], row["source_origin_chain"]
        )
        self.assertNotEqual(reversed_origin_row["observation"], row["observation"])

        type_record = copy.deepcopy(record)
        type_record.update(
            {
                "kind": "type",
                "primary_span": None,
                "origin_chain": [],
            }
        )
        type_descriptor, type_row = materialization.observation_v2_native_row(
            ROOT,
            type_record,
            task,
        )
        self.assertEqual(type_descriptor, "frontend.clang22.type_observation.v2")
        self.assertNotIn("source", type_row)
        self.assertNotIn("source_origin_chain", type_row)

        two_tu = self.request(translation_unit_count=2)
        cross_task = copy.deepcopy(type_record)
        cross_task["final_relation_compile_unit_id"] = two_tu["tasks"][1][
            "compile_unit_id"
        ]
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "observation v2 compile unit is invalid",
        ):
            materialization.observation_v2_native_row(
                ROOT,
                cross_task,
                two_tu["tasks"][0],
            )

        invalid_origin = copy.deepcopy(record)
        invalid_origin["origin_chain"][0]["end"] = 1 << 63
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "observation v2 origin entry violates",
        ):
            materialization.observation_v2_native_row(ROOT, invalid_origin, task)

        exact_with_limitation = copy.deepcopy(record)
        exact_with_limitation["limitation"] = "unexpected limitation"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "exact-equivalence/limitation coupling",
        ):
            materialization.observation_v2_native_row(
                ROOT,
                exact_with_limitation,
                task,
            )

        nonexact_without_limitation = copy.deepcopy(record)
        nonexact_without_limitation.update(
            {"exact_equivalence": False, "limitation": None}
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "exact-equivalence/limitation coupling",
        ):
            materialization.observation_v2_native_row(
                ROOT,
                nonexact_without_limitation,
                task,
            )

        type_with_source = copy.deepcopy(type_record)
        type_with_source["primary_span"] = span
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "type observation cannot discard source",
        ):
            materialization.observation_v2_native_row(ROOT, type_with_source, task)

    def test_observation_v2_contract_rejects_v1_reuse_or_codec_drift(self) -> None:
        contract = copy.deepcopy(materialization.load(ROOT / materialization.CONTRACT))
        contract["relation_outputs"]["observation_v1"][
            "canonical_form_reuse"
        ] = "allowed"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "v1 or fallback became adoptable",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(materialization.load(ROOT / materialization.CONTRACT))
        contract["relation_outputs"]["observation_v2_native_codec"]["payload"][
            "digest_domain"
        ] = "clang22.observation-payload.v1"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "observation v2 native row codec differs",
        ):
            materialization.validate_contract_exact(contract)

        contract_schema = materialization.load(ROOT / materialization.CONTRACT_SCHEMA)
        missing_codec = copy.deepcopy(
            materialization.load(ROOT / materialization.CONTRACT)
        )
        missing_codec["relation_outputs"].pop("observation_v2_native_codec")
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_schema(
                missing_codec,
                contract_schema,
                "materialization contract",
            )

        drifted_codec = copy.deepcopy(
            materialization.load(ROOT / materialization.CONTRACT)
        )
        drifted_codec["relation_outputs"]["observation_v2_native_codec"][
            "payload"
        ]["digest_domain"] = "clang22.observation-payload.v1"
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_schema(
                drifted_codec,
                contract_schema,
                "materialization contract",
            )

    def test_descriptor_digest_mutation_is_rejected(self) -> None:
        request = self.request()
        request["registry"]["descriptors"][0]["runtime_descriptor_digest"] = (
            "semantic-v2:sha256:" + "0" * 64
        )
        materialization.bind_request_identity(request)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.descriptor-binding-mismatch",
        ):
            materialization.validate_request(ROOT, request)

    def test_contract_binds_identity_ownership_and_independent_span_validator(
        self,
    ) -> None:
        contract = copy.deepcopy(materialization.load(ROOT / materialization.CONTRACT))
        contract["identity"]["verification_ownership"][
            "checker_fixture_is_production_qualification"
        ] = True
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.identity-mismatch",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(materialization.load(ROOT / materialization.CONTRACT))
        contract["identity"]["derived_ids_and_digests_recomputed_and_compared"].remove(
            "line_index_id"
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.identity-mismatch",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(materialization.load(ROOT / materialization.CONTRACT))
        contract["identity"]["validated_or_cross_bound_caller_authority"].remove(
            "selected_catalog_compile_unit_id"
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.identity-mismatch",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(materialization.load(ROOT / materialization.CONTRACT))
        contract["source_identity"]["line_index"]["offsets"] = "caller-supplied"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.identity-mismatch",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(materialization.load(ROOT / materialization.CONTRACT))
        contract["span_adoption"]["independent_validator"]["independence"].remove(
            "relation-reference-absence-shortcut"
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.span-invalid",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(materialization.load(ROOT / materialization.CONTRACT))
        contract["claim_adoption"]["hard_reference_validation"]["base_claims"] = (
            materialization.BASE_DESCRIPTOR_IDS[1:]
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.claim-invalid",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(materialization.load(ROOT / materialization.CONTRACT))
        mapping = contract["span_adoption"]["registry_column_binding"][
            "abstract_to_registry_column_id"
        ]["frontend.clang22.entity_observation.v2"]
        mapping["span_id"] = mapping["snapshot"]
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.span-invalid",
        ):
            materialization.validate_contract_exact(contract)

        registry = copy.deepcopy(materialization.load(ROOT / materialization.REGISTRY))
        entity = next(
            relation
            for relation in registry["relations"]
            if relation["descriptor_id"]
            == "frontend.clang22.entity_observation.v2"
        )
        source = next(column for column in entity["columns"] if column["name"] == "source")
        source["type"] = "optional<utf8_string>"
        binding = materialization.load(ROOT / materialization.CONTRACT)["span_adoption"][
            "registry_column_binding"
        ]
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.span-invalid",
        ):
            materialization.validate_span_registry_columns(registry, binding)

        registry = copy.deepcopy(materialization.load(ROOT / materialization.REGISTRY))
        entity = next(
            relation
            for relation in registry["relations"]
            if relation["descriptor_id"]
            == "frontend.clang22.entity_observation.v2"
        )
        entity["row_constraints"]["all_or_none"][0].reverse()
        materialization.validate_span_registry_columns(registry, binding)
        self.assertEqual(
            materialization.canonical_relation(entity),
            idl_canonical_relation(entity),
        )

        registry = copy.deepcopy(materialization.load(ROOT / materialization.REGISTRY))
        entity = next(
            relation
            for relation in registry["relations"]
            if relation["descriptor_id"]
            == "frontend.clang22.entity_observation.v2"
        )
        origin = next(
            column
            for column in entity["columns"]
            if column["name"] == "source_origin_chain"
        )
        origin["type"] = "optional<utf8_string>"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "origin evidence registry separation differs",
        ):
            materialization.validate_span_registry_columns(registry, binding)

        registry = copy.deepcopy(materialization.load(ROOT / materialization.REGISTRY))
        entity = next(
            relation
            for relation in registry["relations"]
            if relation["descriptor_id"]
            == "frontend.clang22.entity_observation.v2"
        )
        entity["row_constraints"]["all_or_none"][0].append(
            "frontend.clang22.entity_observation.v2.source_origin_chain"
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "primary span all-or-none registry constraint differs",
        ):
            materialization.validate_span_registry_columns(registry, binding)

    def test_reverse_specialization_indices_are_non_dependency_edges(self) -> None:
        contract = copy.deepcopy(materialization.load(ROOT / materialization.CONTRACT))
        documents = materialization.materialization_dependency_documents(ROOT)
        materialization.validate_materialization_dependency_graph(contract, documents)

        portable_drift = copy.deepcopy(documents)
        portable_drift[materialization.PORTABLE_PROVIDER_TASK][
            "installed_specializations"
        ]["clang22_materialization"]["task_owner"] = "generic-runtime"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "specialization projection differs",
        ):
            materialization.validate_materialization_dependency_graph(
                contract, portable_drift
            )
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_schema(
                portable_drift[materialization.PORTABLE_PROVIDER_TASK],
                materialization.load(
                    ROOT
                    / "schemas/cxxlens_ng_portable_provider_task_contract.schema.yaml"
                ),
                "portable provider task",
            )

        portable_pointer_drift = copy.deepcopy(documents)
        portable_pointer_drift[materialization.PORTABLE_PROVIDER_TASK][
            "installed_specializations"
        ]["clang22_materialization"]["dependency"] = True
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "specialization projection differs",
        ):
            materialization.validate_materialization_dependency_graph(
                contract, portable_pointer_drift
            )

        protocol_drift = copy.deepcopy(documents)
        protocol_drift[materialization.PROVIDER_PROTOCOL]["adoption_boundary"][
            "raw_frame_redecode_or_private_codec_duplication"
        ] = "allowed"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "specialization projection differs",
        ):
            materialization.validate_materialization_dependency_graph(
                contract, protocol_drift
            )
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_schema(
                protocol_drift[materialization.PROVIDER_PROTOCOL],
                materialization.load(
                    ROOT / "schemas/cxxlens_ng_provider_protocol.schema.yaml"
                ),
                "provider protocol",
            )

        cyclic_contract = copy.deepcopy(contract)
        cyclic_contract["dependencies"].append(materialization.CONTRACT.as_posix())
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "exact generic authority set",
        ):
            materialization.validate_materialization_dependency_graph(
                cyclic_contract, documents
            )

    def test_runtime_specialization_semantic_drift_is_fail_closed(self) -> None:
        contract = copy.deepcopy(materialization.load(ROOT / materialization.CONTRACT))
        runtime_schema = materialization.load(
            ROOT / "schemas/cxxlens_ng_provider_runtime_contract.schema.yaml"
        )

        def reject(label: str, mutate) -> None:
            documents = materialization.materialization_dependency_documents(ROOT)
            mutate(documents[materialization.PROVIDER_RUNTIME])
            with self.subTest(label=label):
                with self.assertRaisesRegex(
                    materialization.MaterializationError,
                    "runtime Clang 22 specialization differs|specialization projection differs",
                ):
                    materialization.validate_materialization_dependency_graph(
                        contract, documents
                    )
                with self.assertRaises(materialization.MaterializationError):
                    materialization.validate_schema(
                        documents[materialization.PROVIDER_RUNTIME],
                        runtime_schema,
                        "provider runtime",
                    )

        reject(
            "v1-adoptability",
            lambda runtime: runtime["clang22"].__setitem__(
                "legacy_observations_v1", "adoptable"
            ),
        )
        reject(
            "observation-order-drift",
            lambda runtime: runtime["clang22"]["observations"].reverse(),
        )
        reject(
            "canonical-output-order-drift",
            lambda runtime: runtime["clang22"]["canonical_outputs"].reverse(),
        )
        reject(
            "missing-exact-output",
            lambda runtime: runtime["clang22"]["installed_materialization"][
                "exact_outputs"
            ].pop(),
        )

        def swap_exact_outputs(runtime: dict) -> None:
            outputs = runtime["clang22"]["installed_materialization"]["exact_outputs"]
            outputs[0], outputs[1] = outputs[1], outputs[0]

        reject("swapped-exact-output", swap_exact_outputs)
        reject(
            "dependency-group-drift",
            lambda runtime: runtime["clang22"]["installed_materialization"][
                "dependency_groups"
            ].reverse(),
        )
        reject(
            "partial-adoption-drift",
            lambda runtime: runtime["clang22"]["installed_materialization"].__setitem__(
                "partial_adoption", "declared_dependency_groups"
            ),
        )
        reject(
            "source-less-drift",
            lambda runtime: runtime["clang22"]["installed_materialization"].__setitem__(
                "source_less_observation", "drop"
            ),
        )
        reject(
            "call-site-drift",
            lambda runtime: runtime["clang22"]["installed_materialization"].__setitem__(
                "canonical_call_site_without_complete_primary_span", "allow"
            ),
        )
        reject(
            "adoption-projection-drift",
            lambda runtime: runtime["protocol_session"]["typed_validation"][
                "adoption_projection"
            ].__setitem__("reconstruction_from_public_frames", "allowed"),
        )

    def test_independent_primary_span_bundle_validator_is_all_or_none(self) -> None:
        source = self.request()["tasks"][0]["source"]
        self.assertEqual(
            materialization.validate_primary_span_bundle(None, source), "absent"
        )
        bundle = {
            "span_id": materialization.source_span_identity(
                source["source_snapshot_id"],
                source["file_id"],
                0,
                3,
                "expression",
            ),
            "snapshot": source["source_snapshot_id"],
            "file": source["file_id"],
            "begin": 0,
            "end": 3,
            "role": "expression",
            "read_only": False,
        }
        self.assertEqual(
            materialization.validate_primary_span_bundle(bundle, source), "present"
        )
        self.assertEqual(
            materialization.source_span_base_row(bundle, source),
            {
                "span": bundle["span_id"],
                "snapshot": bundle["snapshot"],
                "file": bundle["file"],
                "begin": 0,
                "end": 3,
                "role": "expression",
                "origin": None,
                "read_only": False,
            },
        )

        partial = copy.deepcopy(bundle)
        partial.pop("read_only")
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.span-invalid",
        ):
            materialization.validate_primary_span_bundle(partial, source)

        wrong_identity = copy.deepcopy(bundle)
        wrong_identity["span_id"] = "source-span:sha256:" + "0" * 64
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.span-invalid",
        ):
            materialization.validate_primary_span_bundle(wrong_identity, source)

        wrong_range = copy.deepcopy(bundle)
        wrong_range["end"] = source["size_bytes"] + 1
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.span-invalid",
        ):
            materialization.validate_primary_span_bundle(wrong_range, source)

        wrong_source = copy.deepcopy(bundle)
        wrong_source["file"] = "file:other.cpp"
        wrong_source["span_id"] = materialization.source_span_identity(
            wrong_source["snapshot"],
            wrong_source["file"],
            wrong_source["begin"],
            wrong_source["end"],
            wrong_source["role"],
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.span-invalid",
        ):
            materialization.validate_primary_span_bundle(wrong_source, source)

    def test_public_or_raw_frame_adoption_authority_is_rejected(self) -> None:
        contract = copy.deepcopy(materialization.load(ROOT / materialization.CONTRACT))
        contract["surface"]["public_cpp_api"] = "clang22-host-bridge"
        with self.assertRaisesRegex(
            materialization.MaterializationError, "installed machine surface"
        ):
            materialization.validate_contract_exact(contract)

        request = self.request()
        report = self.report(request)
        report["adoption"]["raw_frames"]["authority"] = "adoption-authority"
        with self.assertRaisesRegex(
            materialization.MaterializationError, "materialization report"
        ):
            materialization.validate_report(ROOT, request, report)

    def test_platform_configuration_binding_is_fail_closed(self) -> None:
        request = self.request("static", "memory")
        report = self.report(request)
        report["installation"]["platform"] = "linux-x86_64-shared"
        with self.assertRaisesRegex(
            materialization.MaterializationError, "platform/configuration"
        ):
            materialization.validate_report(ROOT, request, report)

    def test_unsealed_or_partial_group_and_batch_are_rejected(self) -> None:
        request = self.request()
        unsealed = self.report(request)
        unsealed["task_results"][0]["groups"][0]["sealed"] = False
        with self.assertRaisesRegex(
            materialization.MaterializationError, "materialization.group-incomplete"
        ):
            materialization.validate_report(ROOT, request, unsealed)

        missing_batch = self.report(request)
        missing_batch["task_results"][0]["batches"].pop()
        with self.assertRaisesRegex(
            materialization.MaterializationError, "materialization report"
        ):
            materialization.validate_report(ROOT, request, missing_batch)

    def test_zero_row_canonical_and_observation_batches_are_valid(self) -> None:
        request = self.request()

        def empty_batch(report: dict, descriptor: str) -> None:
            batch = next(
                row
                for row in report["task_results"][0]["batches"]
                if row["descriptor_id"] == descriptor
            )
            batch.update(
                {
                    "row_count": 0,
                    "ordered_chunk_digests": [],
                    "row_bindings": [],
                    "provenance_edge_digests": [],
                }
            )
            if descriptor in materialization.DESCRIPTOR_IDS[3:]:
                batch["observation_equivalence_census"] = (
                    materialization.observation_equivalence_census(
                        descriptor,
                        [],
                    )
                )

        canonical = self.report(request)
        empty_batch(canonical, "cc.call_direct_target.v1")
        canonical["provenance"].update(
            {
                "edge_count": 2,
                "canonical_claim_count": 2,
                "canonical_claims_with_exact_input_edges": 2,
            }
        )
        materialization.rebind_report_digest_chain(ROOT, request, canonical)
        materialization.validate_report(ROOT, request, canonical)

        observation = self.report(request)
        empty_batch(observation, "frontend.clang22.type_observation.v2")
        materialization.rebind_report_digest_chain(ROOT, request, observation)
        materialization.validate_report(ROOT, request, observation)

    def test_zero_row_batch_normalization_is_fail_closed(self) -> None:
        request = self.request()
        descriptor = "frontend.clang22.type_observation.v2"

        def batch(report: dict) -> dict:
            return next(
                row
                for row in report["task_results"][0]["batches"]
                if row["descriptor_id"] == descriptor
            )

        zero_with_chunk = self.report(request)
        target = batch(zero_with_chunk)
        target.update(
            {
                "row_count": 0,
                "row_bindings": [],
                "provenance_edge_digests": [],
                "observation_equivalence_census": (
                    materialization.observation_equivalence_census(descriptor, [])
                ),
            }
        )
        materialization.rebind_report_digest_chain(ROOT, request, zero_with_chunk)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            materialization.validate_report(ROOT, request, zero_with_chunk)

        zero_with_binding = self.report(request)
        target = batch(zero_with_binding)
        target.update(
            {
                "row_count": 0,
                "ordered_chunk_digests": [],
                "provenance_edge_digests": [],
            }
        )
        materialization.rebind_report_digest_chain(ROOT, request, zero_with_binding)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            materialization.validate_report(ROOT, request, zero_with_binding)

        zero_with_provenance = self.report(request)
        target = batch(zero_with_provenance)
        target.update(
            {
                "row_count": 0,
                "ordered_chunk_digests": [],
                "row_bindings": [],
                "observation_equivalence_census": (
                    materialization.observation_equivalence_census(descriptor, [])
                ),
            }
        )
        materialization.rebind_report_digest_chain(
            ROOT,
            request,
            zero_with_provenance,
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            materialization.validate_report(ROOT, request, zero_with_provenance)

        nonzero_without_chunk = self.report(request)
        batch(nonzero_without_chunk)["ordered_chunk_digests"] = []
        materialization.rebind_report_digest_chain(
            ROOT,
            request,
            nonzero_without_chunk,
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            materialization.validate_report(ROOT, request, nonzero_without_chunk)

        nonzero_without_binding = self.report(request)
        batch(nonzero_without_binding)["row_bindings"] = []
        materialization.rebind_report_digest_chain(
            ROOT,
            request,
            nonzero_without_binding,
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            materialization.validate_report(ROOT, request, nonzero_without_binding)

        nonzero_without_provenance = self.report(request)
        batch(nonzero_without_provenance)["provenance_edge_digests"] = []
        materialization.rebind_report_digest_chain(
            ROOT,
            request,
            nonzero_without_provenance,
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            materialization.validate_report(ROOT, request, nonzero_without_provenance)

    def test_span_id_range_and_absent_accounting_are_fail_closed(self) -> None:
        request = self.request()
        mismatched = self.report(request)
        mismatched["span_validation"]["recomputed_id_mismatch_count"] = 1
        with self.assertRaisesRegex(
            materialization.MaterializationError, "materialization.span-invalid"
        ):
            materialization.validate_report(ROOT, request, mismatched)

        absent = self.report(request)
        absent["span_validation"]["absent_bundle_count"] = 1
        with self.assertRaisesRegex(
            materialization.MaterializationError, "absent span accounting"
        ):
            materialization.validate_report(ROOT, request, absent)

        exact_with_accounted_absence = self.report(request)
        exact_with_accounted_absence["span_validation"].update(
            {
                "absent_bundle_count": 1,
                "call_absent_bundle_count": 1,
                "absent_bundle_unresolved_count": 1,
                "source_dependent_canonical_omission_count": 1,
            }
        )
        exact_with_accounted_absence["side_channels"]["unresolved"].update(
            {
                "record_count": 1,
                "categories": [materialization.PRIMARY_SPAN_ABSENCE_CATEGORY],
                "category_counts": {
                    materialization.PRIMARY_SPAN_ABSENCE_CATEGORY: 1
                },
            }
        )
        exact_call = next(
            batch
            for batch in exact_with_accounted_absence["task_results"][0]["batches"]
            if batch["descriptor_id"]
            == "frontend.clang22.call_observation.v2"
        )
        materialization.append_fixture_materialized_row(
            request["tasks"][0],
            exact_call,
            label="source-less-call",
            exact_equivalence=False,
            limitation="primary span bundle is absent",
        )
        materialization.rebind_report_digest_chain(
            ROOT,
            request,
            exact_with_accounted_absence,
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError, "materialization.report-invalid"
        ):
            materialization.validate_report(
                ROOT, request, exact_with_accounted_absence
            )

    def test_absent_call_span_is_retained_as_nonexact_without_call_site(self) -> None:
        request = self.request()
        report = self.report(request)
        report["span_validation"].update(
            {
                "absent_bundle_count": 1,
                "call_absent_bundle_count": 1,
                "absent_bundle_unresolved_count": 1,
                "source_dependent_canonical_omission_count": 1,
            }
        )
        call_observation = next(
            batch
            for batch in report["task_results"][0]["batches"]
            if batch["descriptor_id"]
            == "frontend.clang22.call_observation.v2"
        )
        materialization.append_fixture_materialized_row(
            request["tasks"][0],
            call_observation,
            label="source-less-call",
            exact_equivalence=False,
            limitation="primary span bundle is absent",
        )
        report["side_channels"]["coverage"]["state_counts"].update(
            {"covered": 2, "unresolved": 1}
        )
        report["side_channels"]["unresolved"].update(
            {
                "record_count": 1,
                "categories": [materialization.PRIMARY_SPAN_ABSENCE_CATEGORY],
                "category_counts": {
                    materialization.PRIMARY_SPAN_ABSENCE_CATEGORY: 1
                },
            }
        )
        report["side_channels"]["guarantee"]["approximation"] = (
            "under_approximation"
        )
        materialization.rebind_report_digest_chain(ROOT, request, report)
        materialization.validate_report(ROOT, request, report)

    def test_observation_span_census_and_unique_count_are_fail_closed(self) -> None:
        request = self.request()
        census_bypass = self.report(request)
        census_bypass["span_validation"]["observed_bundle_count"] = 1
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "validated span row census differs|observation/span bundle census differs",
        ):
            materialization.validate_report(ROOT, request, census_bypass)

        unique_bypass = self.report(request)
        unique_bypass["span_validation"].update(
            {
                "unique_bundle_count": 3,
                "constructed_source_span_claim_count": 3,
            }
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.span-invalid",
        ):
            materialization.validate_report(ROOT, request, unique_bypass)

    def test_absent_bundle_requires_exact_typed_unresolved_category(self) -> None:
        request = self.request()
        report = self.report(request)
        report["span_validation"].update(
            {
                "absent_bundle_count": 1,
                "call_absent_bundle_count": 1,
                "absent_bundle_unresolved_count": 1,
                "source_dependent_canonical_omission_count": 1,
            }
        )
        next(
            batch
            for batch in report["task_results"][0]["batches"]
            if batch["descriptor_id"]
            == "frontend.clang22.call_observation.v2"
        )["row_count"] = 2
        report["side_channels"]["unresolved"].update(
            {
                "record_count": 1,
                "categories": ["missing_input"],
                "category_counts": {"missing_input": 1},
            }
        )
        report["side_channels"]["guarantee"]["approximation"] = (
            "under_approximation"
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "typed unresolved category accounting is not exact",
        ):
            materialization.validate_report(ROOT, request, report)

    def test_stale_parent_cannot_forge_success_and_valid_failure_preserves_head(self) -> None:
        request = self.request("static", "sqlite")
        forged = self.report(request)
        forged["publication"]["observed_parent_publication"] = "publication:other"
        with self.assertRaisesRegex(
            materialization.MaterializationError, "materialization.stale-parent"
        ):
            materialization.validate_report(ROOT, request, forged)

        failed = materialization.stale_parent_report(ROOT, request)
        materialization.validate_report(ROOT, request, failed)
        self.assertFalse(failed["publication"]["committed"])
        self.assertTrue(failed["publication"]["prior_head_preserved"])

    def test_passed_sqlite_requires_reopen_and_memory_forbids_false_claim(self) -> None:
        sqlite_request = self.request("static", "sqlite")
        sqlite_report = self.report(sqlite_request)
        sqlite_report["publication"]["sqlite_reopened"] = False
        with self.assertRaisesRegex(
            materialization.MaterializationError, "materialization report"
        ):
            materialization.validate_report(ROOT, sqlite_request, sqlite_report)

        memory_request = self.request("static", "memory")
        memory_report = self.report(memory_request)
        memory_report["publication"]["sqlite_reopened"] = True
        with self.assertRaisesRegex(
            materialization.MaterializationError, "materialization report"
        ):
            materialization.validate_report(ROOT, memory_request, memory_report)

    def test_matrix_requires_static_shared_and_memory_sqlite_exactly_once(self) -> None:
        entries = self.matrix()
        materialization.validate_qualification_matrix(ROOT, entries)
        with self.assertRaisesRegex(
            materialization.MaterializationError, "installed matrix differs"
        ):
            materialization.validate_qualification_matrix(ROOT, entries[:-1])
        duplicate = entries[:-1] + [entries[0]]
        with self.assertRaisesRegex(
            materialization.MaterializationError, "installed matrix differs"
        ):
            materialization.validate_qualification_matrix(ROOT, duplicate)

    def test_semantic_request_digest_is_backend_independent_and_bound(self) -> None:
        memory = self.request("static", "memory")
        sqlite = self.request("static", "sqlite")
        self.assertNotEqual(memory["request_digest"], sqlite["request_digest"])
        self.assertEqual(
            memory["semantic_request_digest"], sqlite["semantic_request_digest"]
        )

        mutated = copy.deepcopy(memory)
        mutated["semantic_request_digest"] = "semantic-v2:sha256:" + "0" * 64
        with self.assertRaisesRegex(
            materialization.MaterializationError, "semantic request digest differs"
        ):
            materialization.validate_request(ROOT, mutated)

        entries = self.matrix()
        shared_sqlite_request, _ = entries[-1]
        shared_sqlite_request["tasks"][0]["budget"]["rows"] += 1
        materialization.bind_task_execution_identities(shared_sqlite_request)
        materialization.bind_request_identity(shared_sqlite_request)
        entries[-1] = (
            shared_sqlite_request,
            self.report(shared_sqlite_request),
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "matrix project/provider semantics differ|shared memory/SQLite semantic request digests differ",
        ):
            materialization.validate_qualification_matrix(ROOT, entries)

    def test_source_revision_tree_and_authority_digest_mutations_are_rejected(self) -> None:
        request = self.request()
        wrong_source = self.report(request)
        wrong_source["source"]["tree"] = "f" * 40
        with self.assertRaisesRegex(
            materialization.MaterializationError, "source revision/tree"
        ):
            materialization.validate_report(ROOT, request, wrong_source)

        wrong_authority = self.report(request)
        wrong_authority["authority_digests"][0]["digest"] = "sha256:" + "0" * 64
        with self.assertRaisesRegex(
            materialization.MaterializationError, "authority digests"
        ):
            materialization.validate_report(ROOT, request, wrong_authority)

    def test_source_content_size_and_digest_are_recomputed(self) -> None:
        request = self.request()
        request["tasks"][0]["source"]["size_bytes"] += 1
        materialization.bind_request_identity(request)
        with self.assertRaisesRegex(
            materialization.MaterializationError, "source size/content digest"
        ):
            materialization.validate_request(ROOT, request)

    def test_source_file_and_line_index_identities_are_bottom_up(self) -> None:
        request = self.request()
        source = request["tasks"][0]["source"]
        decoded = b"int main() { return 0; }\n"
        self.assertEqual(
            source["file_id"], materialization.file_identity(source["logical_path"])
        )
        self.assertEqual(
            source["line_index_id"], materialization.line_index_identity(decoded)
        )

        mutations = {
            "file-id": ("file_id", "file:sha256:" + "0" * 64),
            "line-index-id": ("line_index_id", "line-index:sha256:" + "0" * 64),
            "logical-path": ("logical_path", "project://src/../main.cpp"),
        }
        for label, (field, value) in mutations.items():
            drift = self.request()
            drift["tasks"][0]["source"][field] = value
            materialization.bind_request_identity(drift)
            with self.subTest(label=label):
                with self.assertRaisesRegex(
                    materialization.MaterializationError,
                    "materialization.identity-mismatch",
                ):
                    materialization.validate_request(ROOT, drift)

    def test_base_descriptor_and_report_bindings_are_fail_closed(self) -> None:
        descriptor_drift = self.request()
        descriptor_drift["registry"]["base_descriptors"][0], descriptor_drift[
            "registry"
        ]["base_descriptors"][1] = (
            descriptor_drift["registry"]["base_descriptors"][1],
            descriptor_drift["registry"]["base_descriptors"][0],
        )
        materialization.bind_request_identity(descriptor_drift)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.descriptor-binding-mismatch",
        ):
            materialization.validate_request(ROOT, descriptor_drift)

        request = self.request()
        report = self.report(request)
        report["base_claims"]["descriptor_results"][0], report["base_claims"][
            "descriptor_results"
        ][1] = (
            report["base_claims"]["descriptor_results"][1],
            report["base_claims"]["descriptor_results"][0],
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.claim-invalid",
        ):
            materialization.validate_report(ROOT, request, report)

        digest_drift = self.report(request)
        digest_drift["base_claims"]["descriptor_results"][0]["row_set_digest"] = (
            "semantic-v2:sha256:" + "0" * 64
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.claim-invalid",
        ):
            materialization.validate_report(ROOT, request, digest_drift)

    def test_base_row_context_and_span_bundle_edges_cannot_be_swapped(self) -> None:
        request = self.request(translation_unit_count=2)
        request["tasks"][0].update(
            {
                "condition_universe_id": "condition-universe:first",
                "condition_id": "condition:first",
            }
        )
        request["tasks"][1].update(
            {
                "condition_universe_id": "condition-universe:second",
                "condition_id": "condition:second",
            }
        )
        materialization.bind_provider_task_identities(request)
        materialization.bind_task_execution_identities(request)
        materialization.bind_request_identity(request)
        report = self.report(request)
        materialization.validate_report(ROOT, request, report)

        swapped_rows = copy.deepcopy(report)
        source_files = next(
            result
            for result in swapped_rows["base_claims"]["descriptor_results"]
            if result["descriptor_id"] == "source.file.v1"
        )
        bindings = source_files["row_envelope_bindings"]
        self.assertEqual(len(bindings), 2)
        original_binding_digest = source_files[
            "row_envelope_binding_set_digest"
        ]
        bindings[0]["origin_associations"], bindings[1]["origin_associations"] = (
            bindings[1]["origin_associations"],
            bindings[0]["origin_associations"],
        )
        source_files["row_envelope_binding_set_digest"] = (
            materialization.semantic_digest(
                "cxxlens.base-claim-row-envelope-binding-set.v2",
                materialization._canonical_projection_value(
                    {
                        "descriptor_id": "source.file.v1",
                        "row_envelope_bindings": bindings,
                    }
                ),
            )
        )
        self.assertNotEqual(
            source_files["row_envelope_binding_set_digest"],
            original_binding_digest,
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "base-claim census/binding differs",
        ):
            materialization.validate_report(ROOT, request, swapped_rows)

        swapped_span = copy.deepcopy(report)
        span_bindings = swapped_span["span_validation"][
            "validated_bundle_bindings"
        ]
        first_other_task = next(
            index
            for index, binding in enumerate(span_bindings)
            if binding["originating_task"]
            != span_bindings[0]["originating_task"]
        )
        span_bindings[0]["originating_task"], span_bindings[first_other_task][
            "originating_task"
        ] = (
            span_bindings[first_other_task]["originating_task"],
            span_bindings[0]["originating_task"],
        )
        span_bindings.sort(
            key=lambda item: (
                item["bundle"]["span_id"],
                materialization.task_semantic_context_key(
                    item["originating_task"]
                ),
                item["observation_descriptor_id"],
                item["observation_row_digest"],
                item["bundle_digest"],
            )
        )
        swapped_span["span_validation"]["bundle_task_binding_set_digest"] = (
            materialization.span_bundle_binding_set_digest(span_bindings)
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "primary span task source binding differs|span bundle originating task binding differs",
        ):
            materialization.validate_report(ROOT, request, swapped_span)

    def test_genesis_parent_binding_is_exact(self) -> None:
        genesis = self.request()
        genesis["publication"]["genesis"] = True
        genesis["publication"]["expected_parent_publication"] = None
        materialization.bind_request_identity(genesis)
        materialization.validate_request(ROOT, genesis)
        materialization.validate_report(ROOT, genesis, self.report(genesis))

        invalid_pairs = ((True, "publication:parent"), (False, None))
        for flag, parent in invalid_pairs:
            request = self.request()
            request["publication"]["genesis"] = flag
            request["publication"]["expected_parent_publication"] = parent
            materialization.bind_request_identity(request)
            with self.subTest(genesis=flag, parent=parent):
                with self.assertRaises(materialization.MaterializationError):
                    materialization.validate_request(ROOT, request)

    def test_two_tu_shared_task_id_uses_composite_result_correlation(self) -> None:
        request = self.request(translation_unit_count=2)
        materialization.validate_request(ROOT, request)
        project = request["project"]
        self.assertEqual(
            project["catalog_digest"],
            materialization.expected_project_catalog_digest(project),
        )
        self.assertEqual(project["catalog_id"], "catalog:" + project["catalog_digest"])
        self.assertEqual(
            project["catalog_compile_unit_census_digest"],
            materialization.expected_catalog_compile_unit_census_digest(project),
        )
        self.assertEqual(
            len({task["provider_task_id"] for task in request["tasks"]}), 1
        )
        self.assertEqual(
            len({task["task_input_digest"] for task in request["tasks"]}), 2
        )
        self.assertEqual(
            len({task["provider_execution_id"] for task in request["tasks"]}), 2
        )
        self.assertTrue(
            all(
                task["selected_catalog_compile_unit_id"] != task["compile_unit_id"]
                for task in request["tasks"]
            )
        )
        report = self.report(request)
        report["task_results"].reverse()
        materialization.validate_report(ROOT, request, report)

    def test_physical_execution_identity_is_excluded_from_base_semantics(self) -> None:
        static_request = self.request("static", translation_unit_count=2)
        shared_request = self.request("shared", translation_unit_count=2)
        self.assertNotEqual(
            [task["provider_execution_id"] for task in static_request["tasks"]],
            [task["provider_execution_id"] for task in shared_request["tasks"]],
        )
        self.assertEqual(
            self.report(static_request)["base_claims"],
            self.report(shared_request)["base_claims"],
        )

    def test_provider_task_and_worker_v3_inputs_are_bottom_up(self) -> None:
        request = self.request(translation_unit_count=2)
        expected_task_ids = {
            materialization.expected_provider_task_id(request, task)
            for task in request["tasks"]
        }
        self.assertEqual(expected_task_ids, {request["tasks"][0]["provider_task_id"]})
        for task in request["tasks"]:
            self.assertEqual(
                task["task_input_digest"],
                materialization.content_digest(
                    materialization.worker_task_v3_projection(request, task)
                ),
            )
            self.assertTrue(task["task_input_digest"].startswith("sha256:"))

        forged_task = self.request()
        forged_task["tasks"][0]["provider_task_id"] = (
            "task:semantic-v2:sha256:" + "0" * 64
        )
        materialization.bind_task_execution_identities(forged_task)
        materialization.bind_request_identity(forged_task)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "portable provider task identity differs",
        ):
            materialization.validate_request(ROOT, forged_task)

        global_catalog_drift = self.request(translation_unit_count=2)
        global_catalog_drift["project"]["catalog_compile_units"][1][
            "source_digest"
        ] = "sha256:" + "0" * 64
        materialization.bind_project_catalog_identity(global_catalog_drift["project"])
        for task in global_catalog_drift["tasks"]:
            task["catalog_id"] = global_catalog_drift["project"]["catalog_id"]
            task["catalog_digest"] = global_catalog_drift["project"]["catalog_digest"]
        materialization.bind_provider_task_identities(global_catalog_drift)
        materialization.bind_request_identity(global_catalog_drift)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "cxxlens.clang22.task.v3 input digest differs",
        ):
            materialization.validate_request(ROOT, global_catalog_drift)

    def test_worker_v3_budget_uses_shared_signed_int64_domain(self) -> None:
        boundary = self.request()
        boundary["tasks"][0]["budget"]["output_bytes"] = (1 << 63) - 1
        materialization.bind_task_execution_identities(boundary)
        materialization.bind_request_identity(boundary)
        materialization.validate_request(ROOT, boundary)

        overflow = self.request()
        overflow["tasks"][0]["budget"]["output_bytes"] = 1 << 63
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "outside the shared signed-int64 domain",
        ):
            materialization.bind_task_execution_identities(overflow)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.request-invalid",
        ):
            materialization.validate_request(ROOT, overflow)

    def test_catalog_selection_alias_census_and_payload_drift_are_rejected(self) -> None:
        unsorted = self.request(translation_unit_count=2)
        unsorted["project"]["catalog_compile_units"].reverse()
        materialization.bind_request_identity(unsorted)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "catalog compile-unit entries are not canonical and unique",
        ):
            materialization.validate_request(ROOT, unsorted)

        conflicting = self.request(translation_unit_count=2)
        conflicting["project"]["catalog_compile_units"][1][
            "catalog_compile_unit_id"
        ] = conflicting["project"]["catalog_compile_units"][0][
            "catalog_compile_unit_id"
        ]
        materialization.bind_request_identity(conflicting)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "catalog compile-unit entries are not canonical and unique",
        ):
            materialization.validate_request(ROOT, conflicting)

        duplicate_selection = self.request(translation_unit_count=2)
        duplicate_selection["tasks"][1]["selected_catalog_compile_unit_id"] = (
            duplicate_selection["tasks"][0]["selected_catalog_compile_unit_id"]
        )
        materialization.bind_task_execution_identities(duplicate_selection)
        materialization.bind_request_identity(duplicate_selection)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.catalog-census-mismatch",
        ):
            materialization.validate_request(ROOT, duplicate_selection)

        aliased = self.request()
        final_id = aliased["tasks"][0]["compile_unit_id"]
        aliased["project"]["catalog_compile_units"][0][
            "catalog_compile_unit_id"
        ] = final_id
        materialization.bind_project_catalog_identity(aliased["project"])
        aliased["tasks"][0].update(
            {
                "catalog_id": aliased["project"]["catalog_id"],
                "catalog_digest": aliased["project"]["catalog_digest"],
                "selected_catalog_compile_unit_id": final_id,
            }
        )
        materialization.bind_provider_task_identities(aliased)
        materialization.bind_task_execution_identities(aliased)
        materialization.bind_request_identity(aliased)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "catalog-local and final relation compile-unit IDs are aliased",
        ):
            materialization.validate_request(ROOT, aliased)

        task_mutations = {
            "invocation": (
                "normalized_invocation_digest",
                "sha256:" + "0" * 64,
            ),
            "environment": ("environment_digest", "sha256:" + "1" * 64),
        }
        for label, (field, value) in task_mutations.items():
            drift = self.request()
            drift["tasks"][0][field] = value
            materialization.bind_task_execution_identities(drift)
            materialization.bind_request_identity(drift)
            with self.subTest(label=label):
                with self.assertRaisesRegex(
                    materialization.MaterializationError,
                    (
                        "effective argv differs from normalized invocation digest"
                        if label == "invocation"
                        else "selected catalog entry payload differs"
                    ),
                ):
                    materialization.validate_request(ROOT, drift)

        source_drift = self.request()
        source_drift["tasks"][0]["source"]["content_digest"] = (
            "sha256:" + "2" * 64
        )
        materialization.bind_task_execution_identities(source_drift)
        materialization.bind_request_identity(source_drift)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "selected catalog entry payload differs",
        ):
            materialization.validate_request(ROOT, source_drift)

        forged_final = self.request()
        forged_final["tasks"][0]["compile_unit_id"] = "compile-unit:forged"
        materialization.bind_task_execution_identities(forged_final)
        materialization.bind_request_identity(forged_final)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "base row identity differs: build.compile_unit.v1",
        ):
            materialization.validate_request(ROOT, forged_final)

    def test_composite_execution_duplicates_missing_extra_and_result_drift_reject(
        self,
    ) -> None:
        duplicate_request = self.request(translation_unit_count=2)
        for field in materialization.TASK_EXECUTION_KEY_FIELDS:
            duplicate_request["tasks"][1][field] = duplicate_request["tasks"][0][field]
        materialization.bind_request_identity(duplicate_request)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "duplicate task execution tuple",
        ):
            materialization.validate_request(ROOT, duplicate_request)

        request = self.request(translation_unit_count=2)
        duplicate_result = self.report(request)
        extra_copy = copy.deepcopy(duplicate_result["task_results"][0])
        extra_copy["side_channel_digest"] = "sha256:" + "0" * 64
        duplicate_result["task_results"].append(extra_copy)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "not every requested task has one result",
        ):
            materialization.validate_report(ROOT, request, duplicate_result)

        missing_result = self.report(request)
        missing_result["task_results"].pop()
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "not every requested task has one result",
        ):
            materialization.validate_report(ROOT, request, missing_result)

        extra_result = self.report(request)
        extra_copy = copy.deepcopy(extra_result["task_results"][0])
        extra_copy["provider_execution_id"] = "provider-execution:extra"
        extra_result["task_results"].append(extra_copy)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "not every requested task has one result",
        ):
            materialization.validate_report(ROOT, request, extra_result)

        selected_drift = self.report(request)
        selected_drift["task_results"][0]["selected_catalog_compile_unit_id"] = (
            request["tasks"][1]["selected_catalog_compile_unit_id"]
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "task report differs at selected_catalog_compile_unit_id",
        ):
            materialization.validate_report(ROOT, request, selected_drift)

    def test_legacy_final_id_census_field_is_schema_rejected(self) -> None:
        legacy_fields = {
            "compile_unit_ids": lambda request: [
                request["tasks"][0]["compile_unit_id"]
            ],
            "compile_unit_census_digest": lambda request: (
                "semantic-v2:sha256:" + "0" * 64
            ),
        }
        for field, value in legacy_fields.items():
            request = self.request()
            request["project"][field] = value(request)
            materialization.bind_request_identity(request)
            with self.subTest(field=field):
                with self.assertRaisesRegex(
                    materialization.MaterializationError,
                    "materialization.request-invalid",
                ):
                    materialization.validate_request(ROOT, request)

    def test_self_consistent_request_rebinding_cannot_bypass_cross_bindings(self) -> None:
        task_binding = self.request()
        task_binding["tasks"][0]["catalog_id"] = "catalog:other"
        materialization.bind_request_identity(task_binding)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.task-binding-mismatch",
        ):
            materialization.validate_request(ROOT, task_binding)

        census_binding = self.request()
        census_binding["project"]["catalog_compile_unit_census_digest"] = (
            "semantic-v2:sha256:" + "0" * 64
        )
        materialization.bind_request_identity(census_binding)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.catalog-census-mismatch",
        ):
            materialization.validate_request(ROOT, census_binding)

    def test_condition_universe_is_part_of_the_portable_task_identity(self) -> None:
        request = self.request()
        old_task_id = request["tasks"][0]["provider_task_id"]
        request["tasks"][0]["condition_universe_id"] = "condition-universe:two"
        materialization.bind_task_execution_identities(request)
        materialization.bind_request_identity(request)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "portable provider task identity differs",
        ):
            materialization.validate_request(ROOT, request)

        materialization.bind_provider_task_identities(request)
        self.assertNotEqual(request["tasks"][0]["provider_task_id"], old_task_id)
        materialization.bind_task_execution_identities(request)
        materialization.bind_request_identity(request)
        materialization.validate_request(ROOT, request)

    def test_effective_argv_is_exactly_bound_to_catalog_and_final_identity(self) -> None:
        stale = self.request()
        stale["tasks"][0]["effective_argv"].insert(2, "-DMODE=other")
        materialization.bind_task_execution_identities(stale)
        materialization.bind_request_identity(stale)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "effective argv differs from normalized invocation digest",
        ):
            materialization.validate_request(ROOT, stale)

        reordered = self.request()
        reordered["tasks"][0]["effective_argv"] = [
            "clang++",
            reordered["tasks"][0]["effective_argv"][2],
            "-std=c++23",
        ]
        materialization.bind_task_execution_identities(reordered)
        materialization.bind_request_identity(reordered)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "effective argv differs from normalized invocation digest",
        ):
            materialization.validate_request(ROOT, reordered)

        rebound = self.request()
        old_final_id = rebound["tasks"][0]["compile_unit_id"]
        rebound["tasks"][0]["effective_argv"].insert(2, "-DMODE=other")
        new_invocation = materialization.expected_normalized_invocation_digest(
            rebound["tasks"][0]
        )
        rebound["tasks"][0]["normalized_invocation_digest"] = new_invocation
        rebound["project"]["catalog_compile_units"][0][
            "effective_invocation_digest"
        ] = new_invocation
        materialization.rebind_request_base_identities(ROOT, rebound)
        self.assertNotEqual(rebound["tasks"][0]["compile_unit_id"], old_final_id)
        materialization.validate_request(ROOT, rebound)

    def test_report_digest_dag_rejects_mutation_at_every_layer(self) -> None:
        request = self.request()

        def mutate_batch_leaf(report: dict) -> None:
            report["task_results"][0]["batches"][0][
                "claim_content_set_digest"
            ] = "semantic-v2:sha256:" + "0" * 64

        def mutate_batch(report: dict) -> None:
            report["task_results"][0]["batches"][0]["batch_digest"] = (
                "semantic-v2:sha256:" + "1" * 64
            )

        def mutate_group(report: dict) -> None:
            report["task_results"][0]["groups"][0]["batch_set_digest"] = (
                "semantic-v2:sha256:" + "2" * 64
            )

        def mutate_task_side(report: dict) -> None:
            report["task_results"][0]["side_channel_digest"] = (
                "semantic-v2:sha256:" + "3" * 64
            )

        def mutate_task_result(report: dict) -> None:
            report["task_results"][0]["task_result_digest"] = (
                "semantic-v2:sha256:" + "4" * 64
            )

        def mutate_task_set(report: dict) -> None:
            report["adoption"]["task_result_set_digest"] = (
                "semantic-v2:sha256:" + "5" * 64
            )

        def mutate_frames(report: dict) -> None:
            report["adoption"]["raw_frames"]["frame_set_digest"] = (
                "semantic-v2:sha256:" + "6" * 64
            )

        def mutate_coverage(report: dict) -> None:
            report["side_channels"]["coverage"]["digest"] = (
                "semantic-v2:sha256:" + "7" * 64
            )

        def mutate_guarantee(report: dict) -> None:
            report["side_channels"]["guarantee"]["digest"] = (
                "semantic-v2:sha256:" + "8" * 64
            )

        def mutate_stage(report: dict) -> None:
            report["claim_stages"][0]["claim_content_set_digest"] = (
                "semantic-v2:sha256:" + "9" * 64
            )

        def mutate_provenance(report: dict) -> None:
            report["provenance"]["edge_set_digest"] = (
                "semantic-v2:sha256:" + "a" * 64
            )

        mutations = {
            "batch-leaf": mutate_batch_leaf,
            "batch": mutate_batch,
            "group": mutate_group,
            "task-side": mutate_task_side,
            "task-result": mutate_task_result,
            "task-set": mutate_task_set,
            "raw-frames": mutate_frames,
            "coverage": mutate_coverage,
            "guarantee": mutate_guarantee,
            "claim-stage": mutate_stage,
            "global-provenance": mutate_provenance,
        }
        for label, mutate in mutations.items():
            with self.subTest(layer=label):
                report = self.report(request)
                mutate(report)
                with self.assertRaises(materialization.MaterializationError):
                    materialization.validate_report(ROOT, request, report)

    def test_observation_equivalence_census_is_typed_and_fail_closed(self) -> None:
        request = self.request()
        missing = self.report(request)
        next(
            batch
            for batch in missing["task_results"][0]["batches"]
            if batch["descriptor_id"]
            == "frontend.clang22.type_observation.v2"
        ).pop("observation_equivalence_census")
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization report",
        ):
            materialization.validate_report(ROOT, request, missing)

        canonical_extra = self.report(request)
        canonical_extra["task_results"][0]["batches"][0][
            "observation_equivalence_census"
        ] = copy.deepcopy(
            canonical_extra["task_results"][0]["batches"][3][
                "observation_equivalence_census"
            ]
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization report",
        ):
            materialization.validate_report(ROOT, request, canonical_extra)

        forged_exact = self.report(request)
        type_batch = next(
            batch
            for batch in forged_exact["task_results"][0]["batches"]
            if batch["descriptor_id"]
            == "frontend.clang22.type_observation.v2"
        )
        equivalence = type_batch["observation_equivalence_census"]["rows"][0]
        equivalence.update(
            {
                "exact_equivalence": False,
                "limitation": "type sugar cannot be reconstructed exactly",
                "limitation_digest": materialization.content_digest(
                    b"type sugar cannot be reconstructed exactly"
                ),
            }
        )
        type_batch["row_bindings"][0].update(
            {
                "exact_equivalence": False,
                "limitation_digest": equivalence["limitation_digest"],
            }
        )
        materialization.rebind_report_digest_chain(ROOT, request, forged_exact)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            materialization.validate_report(ROOT, request, forged_exact)

        under = self.report(request)
        type_batch = next(
            batch
            for batch in under["task_results"][0]["batches"]
            if batch["descriptor_id"]
            == "frontend.clang22.type_observation.v2"
        )
        equivalence = type_batch["observation_equivalence_census"]["rows"][0]
        equivalence.update(
            {
                "exact_equivalence": False,
                "limitation": "type sugar cannot be reconstructed exactly",
                "limitation_digest": materialization.content_digest(
                    b"type sugar cannot be reconstructed exactly"
                ),
            }
        )
        type_batch["row_bindings"][0].update(
            {
                "exact_equivalence": False,
                "limitation_digest": equivalence["limitation_digest"],
            }
        )
        under["side_channels"]["guarantee"]["approximation"] = (
            "under_approximation"
        )
        materialization.rebind_report_digest_chain(ROOT, request, under)
        materialization.validate_report(ROOT, request, under)

        pairing = self.report(request)
        type_batch = next(
            batch
            for batch in pairing["task_results"][0]["batches"]
            if batch["descriptor_id"]
            == "frontend.clang22.type_observation.v2"
        )
        materialization.append_fixture_materialized_row(
            request["tasks"][0],
            type_batch,
            label="nonexact-second-type",
            exact_equivalence=False,
            limitation="second type is intentionally non-exact",
        )
        pairing["side_channels"]["guarantee"]["approximation"] = (
            "under_approximation"
        )
        materialization.rebind_report_digest_chain(ROOT, request, pairing)
        materialization.validate_report(ROOT, request, pairing)
        equivalence_rows = type_batch["observation_equivalence_census"]["rows"]
        self.assertEqual(len(equivalence_rows), 2)
        fields = ("exact_equivalence", "limitation", "limitation_digest")
        first_values = tuple(equivalence_rows[0][field] for field in fields)
        second_values = tuple(equivalence_rows[1][field] for field in fields)
        for field, value in zip(fields, second_values, strict=True):
            equivalence_rows[0][field] = value
        for field, value in zip(fields, first_values, strict=True):
            equivalence_rows[1][field] = value
        materialization.rebind_report_digest_chain(ROOT, request, pairing)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "observation equivalence rows differ from adopted row bindings",
        ):
            materialization.validate_report(ROOT, request, pairing)

        matrix = self.matrix()
        matrix[0] = (request, under)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "qualification requires exact observation equivalence",
        ):
            materialization.validate_qualification_matrix(ROOT, matrix)

    def test_row_and_span_observation_task_associations_cannot_be_repaired_by_hashes(
        self,
    ) -> None:
        request = self.request(translation_unit_count=2)
        report = self.report(request)
        first_result = report["task_results"][0]
        first_batch = first_result["batches"][0]
        first_batch["row_bindings"][0].update(
            {
                "originating_task": materialization.task_semantic_context(
                    request["tasks"][1]
                ),
                "final_relation_compile_unit_id": request["tasks"][1][
                    "compile_unit_id"
                ],
            }
        )
        materialization.rebind_report_digest_chain(ROOT, request, report)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "row is attributed to another task",
        ):
            materialization.validate_report(ROOT, request, report)

        request = self.request()
        report = self.report(request)
        task = request["tasks"][0]
        batches = {
            batch["descriptor_id"]: batch
            for batch in report["task_results"][0]["batches"]
        }
        source = task["source"]
        second_bundle = {
            "span_id": materialization.source_span_identity(
                source["source_snapshot_id"],
                source["file_id"],
                4,
                7,
                "expression",
            ),
            "snapshot": source["source_snapshot_id"],
            "file": source["file_id"],
            "begin": 4,
            "end": 7,
            "role": "expression",
            "read_only": source["read_only"],
        }
        materialization.append_fixture_materialized_row(
            task,
            batches["frontend.clang22.call_observation.v2"],
            label="second-call-observation",
            primary_span_bundle=second_bundle,
        )
        materialization.append_fixture_materialized_row(
            task,
            batches["cc.call_site.v1"],
            label="second-call-site",
        )
        second_observation = next(
            binding
            for binding in batches[
                "frontend.clang22.call_observation.v2"
            ]["row_bindings"]
            if binding["primary_span_bundle_digest"]
            == materialization.source_span_bundle_digest(second_bundle)
        )
        spans = report["span_validation"]["validated_bundle_bindings"]
        second_span_row = materialization.source_span_base_row(
            second_bundle,
            source,
        )
        spans.append(
            {
                "bundle": second_bundle,
                "bundle_digest": materialization.source_span_bundle_digest(
                    second_bundle
                ),
                "row_digest": materialization.base_claim_row_digest(
                    "source.span.v1",
                    second_span_row,
                ),
                "observation_descriptor_id": (
                    "frontend.clang22.call_observation.v2"
                ),
                "observation_row_digest": second_observation["row_digest"],
                "originating_task": materialization.task_semantic_context(task),
            }
        )
        spans.sort(
            key=lambda item: (
                item["bundle"]["span_id"],
                materialization.task_semantic_context_key(
                    item["originating_task"]
                ),
                item["observation_descriptor_id"],
                item["observation_row_digest"],
                item["bundle_digest"],
            )
        )
        report["span_validation"].update(
            {
                "observed_bundle_count": 3,
                "unique_bundle_count": 2,
                "constructed_source_span_claim_count": 2,
            }
        )
        report["provenance"].update(
            {
                "edge_count": 4,
                "canonical_claim_count": 4,
                "canonical_claims_with_exact_input_edges": 4,
            }
        )
        materialization.rebind_report_digest_chain(ROOT, request, report)
        materialization.validate_report(ROOT, request, report)

        same_task_calls = [
            binding
            for binding in spans
            if binding["observation_descriptor_id"]
            == "frontend.clang22.call_observation.v2"
        ]
        self.assertEqual(len(same_task_calls), 2)
        same_task_calls[0]["observation_row_digest"], same_task_calls[1][
            "observation_row_digest"
        ] = (
            same_task_calls[1]["observation_row_digest"],
            same_task_calls[0]["observation_row_digest"],
        )
        spans.sort(
            key=lambda item: (
                item["bundle"]["span_id"],
                materialization.task_semantic_context_key(
                    item["originating_task"]
                ),
                item["observation_descriptor_id"],
                item["observation_row_digest"],
                item["bundle_digest"],
            )
        )
        materialization.rebind_report_digest_chain(ROOT, request, report)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "span bundle is not bound to its adopted observation row",
        ):
            materialization.validate_report(ROOT, request, report)

    def test_machine_contract_rejects_invocation_digest_and_exactness_drift(self) -> None:
        contract = copy.deepcopy(materialization.load(ROOT / materialization.CONTRACT))
        contract["project_and_tasks"]["effective_invocation_codec"]["projection"].reverse()
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "effective invocation codec",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(materialization.load(ROOT / materialization.CONTRACT))
        contract["report"]["digest_chain"]["guarantee"]["base-or-claim-stage-back-edge"] = (
            "allowed"
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "report digest chain differs",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(materialization.load(ROOT / materialization.CONTRACT))
        contract["qualification"].pop("exact_observation_equivalence")
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "installed qualification matrix differs",
        ):
            materialization.validate_contract_exact(contract)


if __name__ == "__main__":
    unittest.main()
