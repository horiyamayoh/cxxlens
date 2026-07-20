#!/usr/bin/env python3
"""Positive and fail-closed tests for installed Clang 22 materialization."""

from __future__ import annotations

import codecs
import copy
import hashlib
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
        request_bytes = materialization.canonical_json(request)
        return materialization.sample_report(
            ROOT,
            request,
            request_bytes=request_bytes,
        )

    def validate_report(
        self,
        request: dict | None,
        report: dict,
        *,
        request_bytes: bytes | None = None,
        runtime_raw_occurrences: dict | None = None,
        store_failure_authority: dict | None = None,
        postpublish_failure_authority: dict | None = None,
    ) -> None:
        if request_bytes is None:
            if request is None:
                self.fail("raw-input compact failures require explicit request bytes")
            request_bytes = materialization.canonical_json(request)
        materialization.validate_report(
            ROOT,
            request,
            report,
            request_bytes=request_bytes,
            runtime_raw_occurrences=runtime_raw_occurrences,
            store_failure_authority=store_failure_authority,
            postpublish_failure_authority=postpublish_failure_authority,
        )

    def matrix(self) -> list[tuple[dict, dict]]:
        entries = []
        for configuration in ("static", "shared"):
            for backend in ("memory", "sqlite"):
                request = self.request(configuration, backend)
                entries.append((request, self.report(request)))
        return entries

    @staticmethod
    def rebind_publication_record(record: dict) -> None:
        record["publication_id"] = materialization.canonical_identity_digest(
            "publication",
            [
                record["series_id"],
                record["snapshot_id"],
                record["sequence"],
                record["parent_publication"] or "",
            ],
        )

    @staticmethod
    def rebind_reopened_semantic_projection(projection: dict) -> None:
        semantic_fields = {
            key: projection[key]
            for key in (
                "backend",
                "snapshot_manifest",
                "snapshot_manifest_digest",
                "descriptors",
                "partition_binding_multiset_digest",
                "row_multiset_digest",
                "claim_annotation_multiset_digest",
                "coverage_multiset_digest",
                "unresolved_digest",
                "closure_digest",
                "cursor_projection_digest",
                "canonical_export_digest",
            )
        }
        projection["semantic_projection_digest"] = materialization._digest_projection(
            "cxxlens.clang22-reopened-semantic-projection.v1",
            semantic_fields,
        )

    @staticmethod
    def rebind_reopened_handle_projection(projection: dict) -> None:
        projection["handle_projection_digest"] = materialization._digest_projection(
            "cxxlens.clang22-reopened-handle-projection.v1",
            {
                key: value
                for key, value in projection.items()
                if key != "handle_projection_digest"
            },
        )

    def test_strict_json_loader_rejects_lexical_ambiguity(self) -> None:
        self.assertEqual(
            materialization.load_strict_json_bytes(
                b' {"outer":{"value":1.0},"items":[true,null],"exponent":1e2,'
                b'"uint64":18446744073709551615} \n',
                "request",
            ),
            {
                "outer": {"value": 1},
                "items": [True, None],
                "exponent": 100,
                "uint64": (1 << 64) - 1,
            },
        )
        invalid = {
            "top-duplicate": b'{"value":1,"value":2}',
            "nested-duplicate": b'{"outer":{"value":1,"value":2}}',
            "invalid-utf8": b'{"value":"\xff"}',
            "second-value": b'{} {}',
            "trailing-garbage": b'{}x',
            "bom": b'\xef\xbb\xbf{}',
            "non-finite": b'{"value":NaN}',
            "fractional": b'{"value":1.5}',
            "positive-integer-overflow": b'{"value":18446744073709551616}',
            "negative-integer-overflow": b'{"value":-9223372036854775809}',
            "huge-exponent": b'{"value":1e400}',
            "adversarial-positive-exponent": b'{"value":1e1000000000}',
            "adversarial-negative-exponent": b'{"value":-1e1000000000}',
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

    def test_raw_input_observation_binds_exact_transport_bytes(self) -> None:
        request = self.request()
        canonical = materialization.canonical_json(request)
        transported = b" \n" + canonical + b"\n\t"
        self.assertEqual(
            materialization.load_strict_json_bytes(transported, "request"),
            request,
        )
        report = materialization.sample_report(
            ROOT,
            request,
            request_bytes=transported,
        )
        self.validate_report(request, report, request_bytes=transported)

        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "raw input observation differs from exact transport bytes",
        ):
            self.validate_report(request, report, request_bytes=canonical)

        mutations = {
            "size": lambda observation: observation.__setitem__(
                "observed_size_bytes", observation["observed_size_bytes"] + 1
            ),
            "digest": lambda observation: observation.__setitem__(
                "observed_prefix_digest", "sha256:" + "0" * 64
            ),
            "complete": lambda observation: observation.__setitem__(
                "complete", False
            ),
        }
        for label, mutate in mutations.items():
            drift = copy.deepcopy(report)
            mutate(drift["raw_input_observation"])
            with self.subTest(label=label):
                with self.assertRaises(materialization.MaterializationError) as caught:
                    self.validate_report(
                        request,
                        drift,
                        request_bytes=transported,
                    )
                self.assertEqual(
                    caught.exception.code,
                    "materialization.report-invalid",
                )

        original_limit = materialization.RAW_INPUT_BYTE_LIMIT
        try:
            materialization.RAW_INPUT_BYTE_LIMIT = 4
            oversized = materialization.raw_input_observation(b"abcdef")
        finally:
            materialization.RAW_INPUT_BYTE_LIMIT = original_limit
        self.assertEqual(oversized["observed_size_bytes"], 5)
        self.assertEqual(
            oversized["observed_prefix_digest"],
            materialization.content_digest(b"abcde"),
        )
        self.assertFalse(oversized["complete"])

    def test_compact_failure_binding_phase_code_and_effects_are_closed(self) -> None:
        undecodable = b'{"request":'
        raw_failure = materialization.compact_failure_report(
            undecodable,
            request=None,
            phase="json-decode",
            code="materialization.request-invalid",
        )
        self.validate_report(None, raw_failure, request_bytes=undecodable)

        raw_mutations = {
            "worker": lambda report: report["effects"].__setitem__(
                "worker_launch_count", 1
            ),
            "draft": lambda report: report["effects"].__setitem__(
                "store_draft_state", "discarded"
            ),
            "head": lambda report: report["effects"].update(
                {
                    "head_observation": "present",
                    "observed_head_publication": "publication:forged",
                }
            ),
            "phase": lambda report: report["error"].__setitem__(
                "phase", "worker-launch"
            ),
        }
        for label, mutate in raw_mutations.items():
            drift = copy.deepcopy(raw_failure)
            mutate(drift)
            with self.subTest(raw_effect=label):
                with self.assertRaises(materialization.MaterializationError):
                    self.validate_report(None, drift, request_bytes=undecodable)

        request = self.request()
        request_bytes = materialization.canonical_json(request)
        bound_failure = materialization.compact_failure_report(
            request_bytes,
            request=request,
            phase="worker-launch",
            code="materialization.worker-failure",
        )
        self.validate_report(
            request,
            bound_failure,
            request_bytes=request_bytes,
        )

        bound_mutations = {
            "binding": lambda report: report["binding"]["request"].__setitem__(
                "request_digest", "semantic-v2:sha256:" + "0" * 64
            ),
            "code": lambda report: report["error"].__setitem__(
                "code", "materialization.claim-invalid"
            ),
            "head": lambda report: report["effects"].update(
                {
                    "head_observation": "present",
                    "observed_head_publication": "publication:forged",
                }
            ),
            "commit": lambda report: report["effects"].__setitem__(
                "committed_transaction_count", 1
            ),
        }
        for label, mutate in bound_mutations.items():
            drift = copy.deepcopy(bound_failure)
            mutate(drift)
            with self.subTest(bound_effect=label):
                with self.assertRaises(materialization.MaterializationError):
                    self.validate_report(
                        request,
                        drift,
                        request_bytes=request_bytes,
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
                    self.validate_report(request, report)
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

    def test_machine_v2_and_descriptor_id_engine_inventory_are_exact(self) -> None:
        request = self.request()
        self.assertEqual(
            request["schema"],
            "cxxlens.clang22-materialization-request.v2",
        )
        self.assertEqual(request["request_version"], "2.1.0")
        self.assertEqual(request["tool"]["interface_version"], "2.1.0")
        self.assertEqual(request["worker"]["protocol_minor"], 1)
        self.assertEqual(
            request["worker"]["required_features"],
            ["task-input-chunks-v1"],
        )
        self.assertEqual(
            request["trust_policy"]["required_features"],
            request["worker"]["required_features"],
        )
        report = self.report(request)
        self.assertEqual(report["report_version"], "2.1.0")
        self.assertEqual(
            report["provider"]["required_features"],
            request["worker"]["required_features"],
        )
        inventory = request["engine"]["admitted_descriptors"]
        self.assertEqual(
            [binding["descriptor_id"] for binding in inventory],
            materialization.ADMITTED_DESCRIPTOR_IDS,
        )
        payload = b"".join(
            (
                binding["descriptor_id"]
                + "="
                + binding["runtime_descriptor_digest"]
                + "\n"
            ).encode("utf-8")
            for binding in inventory
        )
        self.assertEqual(
            request["engine"]["engine_registry_digest"],
            materialization.semantic_digest(
                "cxxlens.relation-registry.v1",
                payload,
            ),
        )
        self.assertNotEqual(
            request["engine"]["engine_registry_digest"],
            request["registry"]["authority_registry_digest"],
        )

        mutations = {
            "relation-name-alias": lambda drift: drift["engine"][
                "admitted_descriptors"
            ][0].__setitem__("descriptor_id", "build_compile_unit"),
            "duplicate": lambda drift: drift["engine"][
                "admitted_descriptors"
            ][1].update(drift["engine"]["admitted_descriptors"][0]),
            "reordered": lambda drift: drift["engine"][
                "admitted_descriptors"
            ].reverse(),
            "authority-digest-alias": lambda drift: drift["engine"].__setitem__(
                "engine_registry_digest",
                drift["registry"]["authority_registry_digest"],
            ),
        }
        for label, mutate in mutations.items():
            drift = self.request()
            mutate(drift)
            materialization.bind_request_identity(drift)
            with self.subTest(label=label):
                with self.assertRaises(materialization.MaterializationError):
                    materialization.validate_request(ROOT, drift)

        legacy = self.request()
        legacy["schema"] = "cxxlens.clang22-materialization-request.v1"
        legacy["request_version"] = "1.0.0"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.request-invalid",
        ):
            materialization.validate_request(ROOT, legacy)

    def test_complete_selector_and_memory_genesis_policy_are_fail_closed(self) -> None:
        request = self.request()
        selector_mutations = {
            "catalog_id": "catalog:forged",
            "engine_generation_id": "engine-generation:sha256:" + "0" * 64,
            "condition_universe_id": "condition-universe:forged",
            "relation_registry_digest": "semantic-v2:sha256:" + "1" * 64,
            "interpretation_policy_digest": "semantic-v2:sha256:" + "2" * 64,
            "trust_policy_digest": "semantic-v2:sha256:" + "3" * 64,
        }
        for field, value in selector_mutations.items():
            drift = self.request()
            drift["publication"]["selector"][field] = value
            drift["publication"]["series_id"] = materialization.expected_series_id(
                drift["publication"]["selector"]
            )
            materialization.bind_request_identity(drift)
            with self.subTest(selector_field=field):
                with self.assertRaisesRegex(
                    materialization.MaterializationError,
                    "complete Store selector or task policy binding differs",
                ):
                    materialization.validate_request(ROOT, drift)

        alternate_channel = self.request()
        old_semantic_digest = alternate_channel["semantic_request_digest"]
        alternate_channel["publication"]["selector"][
            "channel_id"
        ] = "channel:alternate"
        alternate_channel["publication"]["series_id"] = (
            materialization.expected_series_id(
                alternate_channel["publication"]["selector"]
            )
        )
        materialization.bind_request_identity(alternate_channel)
        materialization.validate_request(ROOT, alternate_channel)
        self.assertNotEqual(
            alternate_channel["semantic_request_digest"],
            old_semantic_digest,
        )

        memory_append = self.request(backend="memory")
        memory_append["publication"].update(
            {
                "genesis": False,
                "expected_parent_publication": "publication:parent",
            }
        )
        materialization.bind_request_identity(memory_append)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization request",
        ):
            materialization.validate_request(ROOT, memory_append)

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
            self.validate_report(request, report)

    def test_measured_occurrence_configuration_binding_is_fail_closed(self) -> None:
        request = self.request("static", "memory")
        report = self.report(request)
        report["installation"]["measured"]["configuration"] = "shared"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "installed occurrence binding|materialization report",
        ):
            self.validate_report(request, report)

    def test_occurrence_manifest_is_closed_ordered_and_digest_bound(self) -> None:
        manifest = materialization.fixture_occurrence_manifest(
            ROOT,
            source_revision="1" * 40,
            source_tree="2" * 40,
            configuration="static",
            tool_digest="sha256:" + "1" * 64,
            worker_digest="sha256:" + "1" * 64,
        )
        materialization.validate_occurrence_manifest(ROOT, manifest)
        self.assertEqual(
            [row["role"] for row in manifest["files"]],
            [
                "materializer-executable",
                "worker-executable",
                "relation-registry",
                "project-catalog-contract",
                "portable-provider-task-contract",
                "provider-protocol",
                "provider-runtime-contract",
                "snapshot-store-contract",
                "materialization-contract",
                "materialization-contract-schema",
                "materialization-request-schema",
                "materialization-report-schema",
            ],
        )
        self.assertNotIn(
            materialization.OCCURRENCE_MANIFEST_PATH,
            [row["path"] for row in manifest["files"]],
        )

        mutations = []
        self_entry = copy.deepcopy(manifest)
        self_entry["files"][-1]["path"] = materialization.OCCURRENCE_MANIFEST_PATH
        mutations.append(self_entry)
        wrong_role = copy.deepcopy(manifest)
        wrong_role["files"][2]["role"] = "authority"
        mutations.append(wrong_role)
        wrong_order = copy.deepcopy(manifest)
        wrong_order["files"][2], wrong_order["files"][3] = (
            wrong_order["files"][3],
            wrong_order["files"][2],
        )
        mutations.append(wrong_order)
        digest_drift = copy.deepcopy(manifest)
        digest_drift["occurrence_payload_digest"] = "sha256:" + "0" * 64
        mutations.append(digest_drift)
        authority_digest_drift = copy.deepcopy(manifest)
        authority_digest_drift["files"][2]["digest"] = "sha256:" + "0" * 64
        authority_digest_drift["occurrence_payload_digest"] = (
            materialization.content_digest(
                materialization.canonical_json(
                    {
                        key: value
                        for key, value in authority_digest_drift.items()
                        if key != "occurrence_payload_digest"
                    }
                )
            )
        )
        mutations.append(authority_digest_drift)
        for mutated in mutations:
            with self.subTest(files=mutated["files"][:3]):
                with self.assertRaises(materialization.MaterializationError):
                    materialization.validate_occurrence_manifest(ROOT, mutated)

        request = self.request()
        for legacy_field in ("prefix_manifest_digest", "relocated_prefix_digest"):
            legacy = self.report(request)
            legacy["installation"][legacy_field] = "sha256:" + "0" * 64
            with self.subTest(legacy_field=legacy_field):
                with self.assertRaises(materialization.MaterializationError):
                    self.validate_report(request, legacy)

    def test_occurrence_inventory_is_configuration_closed_and_explicitly_measured(self) -> None:
        shared = materialization.fixture_occurrence_manifest(
            ROOT,
            source_revision="1" * 40,
            source_tree="2" * 40,
            configuration="shared",
            tool_digest="sha256:" + "2" * 64,
            worker_digest="sha256:" + "2" * 64,
        )
        materialization.validate_occurrence_manifest(ROOT, shared)
        self.assertEqual(len(shared["files"]), 18)
        self.assertEqual(
            [row["role"] for row in shared["files"][12:]],
            [
                "base",
                "kernel",
                "query",
                "recipes",
                "provider-sdk",
                "clang22-provider-sdk",
            ],
        )

        lib64 = copy.deepcopy(shared)
        for row in lib64["files"][12:]:
            row["path"] = row["path"].replace("lib/", "lib64/") + ".22.1"
        lib64["occurrence_payload_digest"] = materialization.content_digest(
            materialization.canonical_json(
                {
                    key: value
                    for key, value in lib64.items()
                    if key != "occurrence_payload_digest"
                }
            )
        )
        materialization.validate_occurrence_manifest(ROOT, lib64)

        for label, path in (
            ("external-system-library", "lib/libstdc++.so.6"),
            ("absolute", "/usr/lib/libcxxlens_base.so"),
            ("escape", "lib/../libcxxlens_base.so"),
            ("wrong-prefix", "share/libcxxlens_base.so"),
        ):
            changed = copy.deepcopy(shared)
            changed["files"][12]["path"] = path
            changed["occurrence_payload_digest"] = materialization.content_digest(
                materialization.canonical_json(
                    {
                        key: value
                        for key, value in changed.items()
                        if key != "occurrence_payload_digest"
                    }
                )
            )
            with self.subTest(path=label):
                with self.assertRaises(materialization.MaterializationError):
                    materialization.validate_occurrence_manifest(ROOT, changed)

        request = self.request("shared", "memory")
        report = self.report(request)
        measured = report["installation"]["measured"]
        self.assertEqual(measured["files"], shared["files"])
        drift = copy.deepcopy(report)
        drift_files = drift["installation"]["measured"]["files"]
        drift_files[12]["digest"] = "sha256:" + "f" * 64
        drift["installation"]["measured"]["inventory_digest"] = (
            materialization.content_digest(
                materialization.canonical_json(drift_files)
            )
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "installed occurrence binding|occurrence payload digest",
        ):
            self.validate_report(request, drift)

    def test_sqlite_path_is_canonical_relative_utf8_even_after_digest_rebind(self) -> None:
        invalid_paths = [
            ".",
            "./x",
            "a//b",
            "a\\b",
            "C:/x",
            "",
            "a/./b",
            "a/../b",
            "/root/store.sqlite",
            "D:store.sqlite",
            "a/\x00/b",
            "db/e\u0301.sqlite",
        ]
        for path in invalid_paths:
            request = self.request("static", "sqlite")
            request["publication"]["sqlite_path"] = path
            materialization.bind_request_identity(request)
            with self.subTest(path=repr(path)):
                with self.assertRaises(materialization.MaterializationError):
                    materialization.validate_request(ROOT, request)

        boundary = self.request("static", "sqlite")
        boundary["publication"]["sqlite_path"] = (
            "a" * materialization.MAXIMUM_SQLITE_RELATIVE_PATH_UTF8_BYTES
        )
        materialization.bind_request_identity(boundary)
        materialization.validate_request(ROOT, boundary)

        over = copy.deepcopy(boundary)
        over["publication"]["sqlite_path"] += "a"
        materialization.bind_request_identity(over)
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_request(ROOT, over)

    def test_request_v21_features_and_utf8_byte_caps_are_fail_closed(self) -> None:
        for path, value in (
            (("tool", "interface_version"), "2.0.0"),
            (("worker", "protocol_minor"), 0),
            (("worker", "required_features"), []),
            (("trust_policy", "protocol_minor"), 0),
            (("trust_policy", "required_features"), ["legacy-input"]),
        ):
            request = self.request()
            request[path[0]][path[1]] = value
            with self.subTest(path=path):
                with self.assertRaises(materialization.MaterializationError):
                    materialization.validate_request(ROOT, request)

        legacy_occurrence = self.request()
        legacy_occurrence["tool"]["prefix_manifest_digest"] = "sha256:" + "0" * 64
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_request(ROOT, legacy_occurrence)

        request_schema = materialization.load(ROOT / materialization.REQUEST_SCHEMA)
        boundary = self.request("static", "sqlite")
        boundary["project"]["logical_root"] = "project://" + "a" * (
            materialization.MAXIMUM_LOGICAL_PATH_UTF8_BYTES - len("project://")
        )
        boundary["publication"]["sqlite_path"] = (
            "a" * materialization.MAXIMUM_SQLITE_RELATIVE_PATH_UTF8_BYTES
        )
        boundary["tasks"][0]["effective_argv"][0] = "\U0001f600" * 512
        materialization.validate_request_utf8_byte_limits(boundary, request_schema)

        too_long_logical = copy.deepcopy(boundary)
        too_long_logical["project"]["logical_root"] += "a"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "logical-path UTF-8 byte limit",
        ):
            materialization.validate_request_utf8_byte_limits(
                too_long_logical,
                request_schema,
            )

        too_long_sqlite = copy.deepcopy(boundary)
        too_long_sqlite["publication"]["sqlite_path"] += "a"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "sqlite_path exceeds the UTF-8 byte limit",
        ):
            materialization.validate_request_utf8_byte_limits(
                too_long_sqlite,
                request_schema,
            )

        too_long_argv = copy.deepcopy(boundary)
        too_long_argv["tasks"][0]["effective_argv"][0] += "a"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            r"effective_argv\[0\].*UTF-8 byte limit",
        ):
            materialization.validate_request_utf8_byte_limits(
                too_long_argv,
                request_schema,
            )

    def test_task_input_and_runtime_receipts_reject_every_leaf_drift(self) -> None:
        request = self.request()

        def changed_digest(value: str) -> str:
            return value[:-1] + ("0" if value[-1] != "0" else "1")

        input_mutations = {
            "protocol_version": "1.0.0",
            "required_feature": "legacy-input",
            "task_input_codec": "cxxlens.clang22.task.v2",
            "logical_input_bytes": lambda value: value + 1,
            "logical_input_digest": changed_digest,
            "canonical_chunk_bytes": lambda value: value - 1,
            "chunk_count": lambda value: value + 1,
            "ordered_chunk_payload_digest_set_digest": changed_digest,
        }
        for field, replacement in input_mutations.items():
            report = self.report(request)
            receipt = report["task_results"][0]["input_transfer"]
            receipt[field] = (
                replacement(receipt[field]) if callable(replacement) else replacement
            )
            with self.subTest(input_transfer=field):
                with self.assertRaises(materialization.MaterializationError):
                    self.validate_report(request, report)

        runtime_mutations = {
            "raw_frame_stream_bytes": lambda value: value + 1,
            "raw_frame_stream_digest": changed_digest,
            "frame_count": lambda value: value + 1,
            "frame_transcript_digest": changed_digest,
            "sealed_transcript_digest": changed_digest,
        }
        for field, replacement in runtime_mutations.items():
            report = self.report(request)
            receipt = report["task_results"][0]["runtime_receipt"]
            receipt[field] = replacement(receipt[field])
            with self.subTest(runtime_receipt=field):
                with self.assertRaises(materialization.MaterializationError):
                    self.validate_report(request, report)

        for legacy_field in ("transcript", "transcript_digest", "raw_frame_digest"):
            report = self.report(request)
            report["task_results"][0][legacy_field] = {}
            with self.subTest(legacy_field=legacy_field):
                with self.assertRaises(materialization.MaterializationError):
                    self.validate_report(request, report)

    def test_runtime_raw_occurrence_is_the_only_seal_authority(self) -> None:
        request = self.request()
        raw_occurrences = materialization.fixture_runtime_raw_occurrences(
            ROOT,
            request,
        )
        task = request["tasks"][0]
        key = materialization.task_execution_key(task)
        raw_observation = materialization.derive_runtime_observation(
            ROOT,
            request,
            task,
            raw_occurrences[key],
        )
        sealed = raw_observation["sealed_transcript"]
        self.assertEqual(
            sealed["unresolved_records"],
            materialization.fixture_task_unresolved_records(task),
        )
        self.assertEqual(
            sealed["evidence_records"],
            materialization.fixture_task_evidence_records(task),
        )
        self.assertEqual(len(sealed["evidence_records"]), 3)

        report = self.report(request)
        batch = next(
            row
            for row in report["task_results"][0]["batches"]
            if row["descriptor_id"] == "cc.call_direct_target.v1"
        )
        batch.update(
            {
                "row_count": 0,
                "ordered_chunk_digests": [],
                "row_bindings": [],
                "provenance_edge_digests": [],
            }
        )
        report["provenance"].update(
            {
                "edge_count": 2,
                "canonical_claim_count": 2,
                "canonical_claims_with_exact_input_edges": 2,
            }
        )
        materialization.rebind_report_digest_chain(ROOT, request, report)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.transcript-invalid",
        ):
            self.validate_report(
                request,
                report,
                runtime_raw_occurrences=raw_occurrences,
            )

        receipt_forgery = self.report(request)
        receipt = receipt_forgery["task_results"][0]["runtime_receipt"]
        receipt["sealed_transcript_digest"] = (
            receipt["sealed_transcript_digest"][:-1]
            + ("0" if receipt["sealed_transcript_digest"][-1] != "0" else "1")
        )
        materialization.rebind_report_digest_chain(
            ROOT,
            request,
            receipt_forgery,
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.transcript-invalid",
        ):
            self.validate_report(
                request,
                receipt_forgery,
                runtime_raw_occurrences=raw_occurrences,
            )

        corrupt_raw = dict(raw_occurrences)
        corrupt_raw[key] = corrupt_raw[key][:-1]
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.transcript-invalid",
        ):
            self.validate_report(
                request,
                self.report(request),
                runtime_raw_occurrences=corrupt_raw,
            )

    def test_runtime_raw_identity_cannot_replace_installed_provider_authority(self) -> None:
        request = self.request()
        trusted_identity = materialization.expected_runtime_provider_identity(
            ROOT,
            request,
        )
        attacks = (
            {
                "provider_id": "evil.provider",
                "provider_version": "9.9.9",
            },
            {"provider_binary_digest": "sha256:" + "d" * 64},
            {"provider_semantic_contract_digest": "sha256:" + "e" * 64},
        )
        for attack in attacks:
            with self.subTest(attack=attack):
                report = self.report(request)
                result = report["task_results"][0]
                task = request["tasks"][0]
                identity = copy.deepcopy(trusted_identity)
                identity.update(attack)
                authorized_batches = materialization.runtime_authorized_batches(
                    ROOT,
                    request,
                )
                raw_stdout = materialization.provider_runtime.encode_runtime_private_fixture(
                    materialization.load(ROOT / materialization.PROVIDER_PROTOCOL),
                    identity,
                    task["provider_task_id"],
                    authorized_batches,
                    {
                        batch["batch_id"]: [
                            row["row_canonical_form"]
                            for row in batch["row_bindings"]
                        ]
                        for batch in result["batches"]
                    },
                    result["coverage"]["transport_records"]
                    + result["coverage"]["semantic_records"],
                    materialization.fixture_task_unresolved_records(result),
                    materialization.fixture_task_evidence_records(result),
                )
                attacker_observation = (
                    materialization.provider_runtime.derive_runtime_private_observation(
                        materialization.load(ROOT / materialization.PROVIDER_PROTOCOL),
                        raw_stdout,
                        task["provider_task_id"],
                        expected_provider_identity=identity,
                        authorized_batches=authorized_batches,
                    )
                )
                result["runtime_receipt"] = (
                    materialization.materialization_runtime_receipt(
                        attacker_observation["receipt"]
                    )
                )
                materialization.rebind_report_digest_chain(ROOT, request, report)
                runtime_occurrences = (
                    materialization.fixture_runtime_raw_occurrences(ROOT, request)
                )
                runtime_occurrences[
                    materialization.task_execution_key(task)
                ] = raw_stdout
                with self.assertRaisesRegex(
                    materialization.MaterializationError,
                    "provider hello differs from independent expected identity",
                ):
                    self.validate_report(
                        request,
                        report,
                        runtime_raw_occurrences=runtime_occurrences,
                    )

    def test_transport_and_semantic_coverage_are_exact_separate_planes(self) -> None:
        request = self.request()

        def mutations(report: dict) -> list[tuple[str, dict]]:
            result = report["task_results"][0]
            coverage = result["coverage"]
            values: list[tuple[str, dict]] = []
            missing = copy.deepcopy(report)
            missing["task_results"][0]["coverage"]["semantic_records"].pop()
            values.append(("missing", missing))
            duplicate = copy.deepcopy(report)
            duplicate["task_results"][0]["coverage"]["semantic_records"][1] = (
                copy.deepcopy(
                    duplicate["task_results"][0]["coverage"]["semantic_records"][0]
                )
            )
            values.append(("duplicate", duplicate))
            extra = copy.deepcopy(report)
            extra["task_results"][0]["coverage"]["semantic_records"].append(
                copy.deepcopy(coverage["semantic_records"][-1])
            )
            values.append(("extra", extra))
            for name, field, replacement in (
                ("renamed", "kind", "cc.renamed"),
                ("wrong-task", "id", "task:semantic-v2:sha256:" + "0" * 64),
                ("state", "state", "failed"),
                ("reason", "reason", "filtered"),
            ):
                changed = copy.deepcopy(report)
                changed["task_results"][0]["coverage"]["semantic_records"][0][
                    field
                ] = replacement
                values.append((name, changed))
            swapped = copy.deepcopy(report)
            swapped_coverage = swapped["task_results"][0]["coverage"]
            (
                swapped_coverage["transport_records"],
                swapped_coverage["semantic_records"],
            ) = (
                swapped_coverage["semantic_records"],
                swapped_coverage["transport_records"],
            )
            values.append(("plane-swap", swapped))
            return values

        for name, report in mutations(self.report(request)):
            with self.subTest(mutation=name):
                with self.assertRaises(materialization.MaterializationError):
                    self.validate_report(request, report)

        two_task_request = self.request(translation_unit_count=2)
        filtered = self.report(two_task_request)
        filtered["side_channels"]["coverage"]["record_count"] -= 1
        with self.assertRaises(materialization.MaterializationError):
            self.validate_report(two_task_request, filtered)

    def test_guarantee_profile_and_modalities_are_closed_everywhere(self) -> None:
        request = self.request()
        report = self.report(request)
        guarantee = report["side_channels"]["guarantee"]
        self.assertEqual(guarantee["assumptions"], [])
        self.assertEqual(
            guarantee["verification_modalities"],
            materialization.GUARANTEE_MODALITIES,
        )
        self.assertEqual(
            report["task_results"][0]["side_channel_components"][
                "guarantee_profile_digest"
            ],
            materialization.expected_guarantee_profile_digest(),
        )

        for modality in (
            "future-modality.v1",
            "query-parity.v1",
            "store-reopen.v1",
            "successful-publication.v1",
        ):
            changed = self.report(request)
            changed["side_channels"]["guarantee"]["verification_modalities"][
                -1
            ] = modality
            with self.subTest(global_modality=modality):
                with self.assertRaises(materialization.MaterializationError):
                    self.validate_report(request, changed)

            embedded = self.report(request)
            embedded["store"]["claim_envelopes"][0]["guarantee"][
                "verification_modalities"
            ][-1] = modality
            with self.subTest(embedded_modality=modality):
                with self.assertRaises(materialization.MaterializationError):
                    self.validate_report(request, embedded)

        assumed = self.report(request)
        assumed["side_channels"]["guarantee"]["assumptions"] = ["hidden"]
        with self.assertRaises(materialization.MaterializationError):
            self.validate_report(request, assumed)

        wrong_profile = self.report(request)
        wrong_profile["task_results"][0]["side_channel_components"][
            "guarantee_profile_id"
        ] = "other.profile.v1"
        with self.assertRaises(materialization.MaterializationError):
            self.validate_report(request, wrong_profile)

    def test_store_failure_causes_use_only_registered_operation_tuples(self) -> None:
        request = self.request("static", "sqlite")
        report = materialization.stale_parent_report(ROOT, request)
        self.validate_report(request, report)
        cause = report["publication"]["store_failure"]["cause"]
        self.assertEqual(cause["operation"], "writer_publish")
        self.assertIsNone(cause["access_path"])
        self.assertEqual(cause["field"], request["publication"]["series_id"])
        self.assertEqual(cause["detail"], {"kind": "stable", "value": ""})

        for field, replacement in (
            ("operation", "writer_validate"),
            ("code", "store.imaginary"),
            ("field", "publication"),
            ("detail", {"kind": "stable", "value": "diagnostic prose"}),
        ):
            changed = materialization.stale_parent_report(ROOT, request)
            changed["publication"]["store_failure"]["cause"][field] = replacement
            with self.subTest(cause_field=field):
                with self.assertRaises(materialization.MaterializationError):
                    self.validate_report(request, changed)

        opaque = {
            "kind": "opaque",
            "byte_count": 4,
            "digest": materialization.content_digest(b"disk"),
            "diagnostic": "disk",
        }
        materialization._validate_store_detail_observation(opaque)
        opaque["byte_count"] += 1
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "opaque Store detail receipt differs",
        ):
            materialization._validate_store_detail_observation(opaque)

        report_schema = materialization.load(ROOT / materialization.REPORT_SCHEMA)
        cause_schema = {
            "$schema": "https://json-schema.org/draft/2020-12/schema",
            "$ref": "#/$defs/store_failure_cause",
            "$defs": report_schema["$defs"],
        }
        mismatch = {
            "kind": "verification_mismatch",
            "operation": "verify_projection",
            "access_path": "open-snapshot",
            "projection": "snapshot-manifest",
            "expected_digest": "sha256:" + "1" * 64,
            "actual_digest": "sha256:" + "2" * 64,
        }
        materialization.validate_schema(mismatch, cause_schema, "mismatch cause")
        fabricated = copy.deepcopy(mismatch)
        fabricated["code"] = "store.corrupt"
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_schema(
                fabricated,
                cause_schema,
                "mismatch cause",
            )

    def test_prepublication_compact_store_failure_retains_exact_first_cause(self) -> None:
        request = self.request("static", "sqlite")
        request_bytes = materialization.canonical_json(request)
        report_schema = materialization.load(ROOT / materialization.REPORT_SCHEMA)
        operations = {
            "store-open": ["store_open"],
            "store-stage": [
                "head_current",
                "writer_begin",
                "partition_stage",
                "closure_stage",
                "writer_validate",
            ],
        }
        for phase, phase_operations in operations.items():
            for operation in phase_operations:
                cause = {
                    "kind": "sdk_error",
                    "operation": operation,
                    "access_path": None,
                    "code": "store.sqlite-failure",
                    "field": operation,
                    "detail": {"kind": "stable", "value": "fixture-code"},
                }
                report = materialization.compact_failure_report(
                    request_bytes,
                    request=request,
                    phase=phase,
                    code="materialization.store-failure",
                    store_failure_cause=cause,
                )
                with self.subTest(phase=phase, operation=operation):
                    self.validate_report(
                        request,
                        report,
                        request_bytes=request_bytes,
                        store_failure_authority=cause,
                    )
                    changed = copy.deepcopy(report)
                    changed["effects"]["store_failure_cause"]["operation"] = (
                        "writer_publish"
                    )
                    with self.assertRaises(materialization.MaterializationError):
                        self.validate_report(
                            request,
                            changed,
                            request_bytes=request_bytes,
                            store_failure_authority=cause,
                        )

                    tuple_mutations = {
                        "access-path": ("access_path", "current-selector"),
                        "sdk-code": ("code", "diagnostic prose"),
                        "sdk-field": ("field", ""),
                        "detail-shape": (
                            "detail",
                            {"kind": "stable", "value": "x", "extra": "x"},
                        ),
                    }
                    for label, (field, value) in tuple_mutations.items():
                        tuple_drift = copy.deepcopy(report)
                        tuple_drift["effects"]["store_failure_cause"][field] = value
                        with self.subTest(tuple_field=label):
                            with self.assertRaises(
                                materialization.MaterializationError
                            ):
                                self.validate_report(
                                    request,
                                    tuple_drift,
                                    request_bytes=request_bytes,
                                    store_failure_authority=cause,
                                )

                    valid_tuple_mutations = {
                        "alternate-valid-code": ("code", "store.corrupt"),
                        "alternate-valid-field": ("field", "database"),
                        "alternate-valid-detail": (
                            "detail",
                            {"kind": "stable", "value": "alternate-sdk-detail"},
                        ),
                    }
                    for label, (field, value) in valid_tuple_mutations.items():
                        tuple_attack = copy.deepcopy(report)
                        tuple_attack["effects"]["store_failure_cause"][field] = value
                        materialization.validate_schema(
                            tuple_attack,
                            report_schema,
                            "materialization report",
                        )
                        with self.subTest(valid_tuple_attack=label):
                            with self.assertRaisesRegex(
                                materialization.MaterializationError,
                                "compact Store failure cause",
                            ):
                                self.validate_report(
                                    request,
                                    tuple_attack,
                                    request_bytes=request_bytes,
                                    store_failure_authority=cause,
                                )

        missing = materialization.compact_failure_report(
            request_bytes,
            request=request,
            phase="store-open",
            code="materialization.store-failure",
        )
        with self.assertRaises(materialization.MaterializationError):
            self.validate_report(request, missing, request_bytes=request_bytes)

        non_store = materialization.compact_failure_report(
            request_bytes,
            request=request,
            phase="worker-launch",
            code="materialization.worker-failure",
        )
        non_store["effects"]["store_failure_cause"] = {
            "kind": "sdk_error",
            "operation": "store_open",
            "access_path": None,
            "code": "store.sqlite-failure",
            "field": "database",
            "detail": {"kind": "stable", "value": ""},
        }
        with self.assertRaises(materialization.MaterializationError):
            self.validate_report(request, non_store, request_bytes=request_bytes)

        bad_opaque = materialization.compact_failure_report(
            request_bytes,
            request=request,
            phase="store-open",
            code="materialization.store-failure",
            store_failure_cause={
                "kind": "sdk_error",
                "operation": "store_open",
                "access_path": None,
                "code": "store.sqlite-failure",
                "field": "database",
                "detail": {
                    "kind": "opaque",
                    "byte_count": 5,
                    "digest": materialization.content_digest(b"disk"),
                    "diagnostic": "disk",
                },
            },
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "opaque Store detail receipt differs",
        ):
            self.validate_report(
                request,
                bad_opaque,
                request_bytes=request_bytes,
                store_failure_authority=bad_opaque["effects"][
                    "store_failure_cause"
                ],
            )

    def test_writer_publish_tuple_classifier_is_complete_and_fail_closed(self) -> None:
        request = self.request("static", "sqlite")
        execution_receipt_schema = materialization.load(
            ROOT
            / "schemas/cxxlens_ng_clang22_materialization_execution_receipt.schema.yaml"
        )
        publication = materialization.stale_parent_report(
            ROOT, request
        )["publication"]
        candidate_snapshot = publication["candidate_snapshot_id"]
        candidate_publication = publication["candidate_identity"]["publication_id"]
        series = request["publication"]["series_id"]
        empty = {"kind": "stable", "value": ""}
        opaque = {
            "kind": "opaque",
            "byte_count": 4,
            "digest": materialization.content_digest(b"disk"),
            "diagnostic": "disk",
        }

        cases = [
            ("store.publication-conflict", series, empty, "stale_parent", "rejected_stale"),
            ("store.counter-overflow", "publication_sequence", empty, "counter_overflow", "rejected_store_failure"),
            ("store.counter-overflow", "physical_generation", empty, "counter_overflow", "rejected_store_failure"),
            ("store.hash-collision", candidate_snapshot, empty, "hash_collision", "rejected_store_failure"),
            ("store.snapshot-ambiguous", candidate_snapshot, empty, "persistence_corrupt", "rejected_store_failure"),
            ("store.sqlite-failure", "database", opaque, "persistence_io", "publication_outcome_unknown"),
        ]
        corruption = {
            "sqlite": [
                "backend",
                "column-count",
                "publication-row",
                "series-head-count",
                "series-head",
                "series-head-sequence",
            ],
            candidate_publication: [
                "authority-record",
                "duplicate-publication-id",
                "parent",
                "parent-sequence",
            ],
            series: ["duplicate-sequence", "series-roots", "series-head-cas"],
        }
        cases.extend(
            (
                "store.corrupt",
                field,
                {"kind": "stable", "value": detail},
                "persistence_corrupt",
                "rejected_store_failure",
            )
            for field, details in corruption.items()
            for detail in details
        )
        for code, field, detail, category, outcome in cases:
            changed = copy.deepcopy(publication)
            changed["store_failure"]["cause"] = {
                "kind": "sdk_error",
                "operation": "writer_publish",
                "access_path": None,
                "code": code,
                "field": field,
                "detail": copy.deepcopy(detail),
            }
            with self.subTest(code=code, field=field, detail=detail):
                self.assertEqual(
                    materialization.classify_writer_publish_failure(request, changed),
                    (category, outcome),
                )

        invariant_breaches = [
            ("store.transaction-state", "publish", empty),
            ("store.corrupt", "publication", {"kind": "stable", "value": "identity"}),
            ("store.publish-stale-parent", series, empty),
            ("store.imaginary", "database", empty),
        ]
        for code, field, detail in invariant_breaches:
            changed = copy.deepcopy(publication)
            changed["store_failure"]["cause"] = {
                "kind": "sdk_error",
                "operation": "writer_publish",
                "access_path": None,
                "code": code,
                "field": field,
                "detail": detail,
            }
            with self.subTest(invariant=code):
                with self.assertRaisesRegex(
                    materialization.WriterPublishInvariantBreach,
                    "invariant breach",
                ):
                    materialization.classify_writer_publish_failure(request, changed)
                disposition = (
                    materialization.writer_publish_invariant_breach_disposition(
                        request, changed
                    )
                )
                self.assertEqual(
                    disposition,
                    {
                        "schema": (
                            "cxxlens.clang22-materialization-execution-receipt.v1"
                        ),
                        "actual_exit_status": 2,
                        "exact_stdout_byte_count": 0,
                        "stdout_sha256": materialization.content_digest(b""),
                        "parsed_response_count": 0,
                        "stderr_sha256": materialization.content_digest(
                            str(
                                materialization.WriterPublishInvariantBreach(
                                    "writer_publish tuple is unlisted or an invariant breach requiring exit two"
                                )
                            ).encode("utf-8")
                        ),
                    },
                )
                materialization.validate_schema(
                    disposition,
                    execution_receipt_schema,
                    "writer-publish invariant execution receipt",
                )

        memory = copy.deepcopy(publication)
        memory["backend"] = "memory"
        with self.assertRaisesRegex(
            materialization.WriterPublishInvariantBreach,
            "invariant breach",
        ):
            materialization.classify_writer_publish_failure(request, memory)
        memory_disposition = (
            materialization.writer_publish_invariant_breach_disposition(
                request, memory
            )
        )
        self.assertEqual(
            memory_disposition,
            {
                "schema": "cxxlens.clang22-materialization-execution-receipt.v1",
                "actual_exit_status": 2,
                "exact_stdout_byte_count": 0,
                "stdout_sha256": materialization.content_digest(b""),
                "parsed_response_count": 0,
                "stderr_sha256": materialization.content_digest(
                    str(
                        materialization.WriterPublishInvariantBreach(
                            "writer_publish tuple is an invariant breach requiring exit two"
                        )
                    ).encode("utf-8")
                ),
            },
        )
        materialization.validate_schema(
            memory_disposition,
            execution_receipt_schema,
            "memory writer-publish invariant execution receipt",
        )
        with self.assertRaisesRegex(ValueError, "typed response"):
            materialization.writer_publish_invariant_breach_disposition(
                request,
                publication,
            )

    def test_postpublish_classifier_preserves_first_sdk_or_projection_cause(self) -> None:
        paths = list(materialization.POSTPUBLISH_ACCESS_PATHS)
        matrix_projections = {
            projection
            for path_bindings in materialization.POSTPUBLISH_MISMATCH_BINDINGS.values()
            for projections in path_bindings.values()
            for projection in projections
        }
        self.assertEqual(
            matrix_projections,
            set(materialization.POSTPUBLISH_RETAINED_DIGEST_FIELDS)
            | {
                "publication-semantic-fields",
                "physical-generation-transition",
                "descriptor-inventory",
                "open-snapshot-return-binding",
                "cross-path-semantic-projection",
            },
        )

        def receipt(path: str, status: str = "present") -> dict:
            return {
                "access_path": path,
                "status": status,
                "sdk_code": (
                    None if status in {"present", "not_attempted"}
                    else "store.sqlite-failure"
                ),
                "sdk_field": (
                    None if status in {"present", "not_attempted"} else "database"
                ),
            }

        def authority(failure: dict) -> dict:
            return copy.deepcopy(
                materialization._postpublish_first_cause_authority(failure)
            )

        opened = {
            "close_reopen_status": "opened",
            "open_error_code": None,
            "open_error_field": None,
            "handle_receipts": [receipt(path) for path in paths],
        }
        reopen_failed = {
            "close_reopen_status": "open_failed",
            "open_error_code": "store.sqlite-failure",
            "open_error_field": "open",
            "handle_receipts": [receipt(path, "not_attempted") for path in paths],
        }
        reopen_failure = {
            "stage": "close-reopen",
            "access_path": None,
            "code": "materialization.store-failure",
            "cause": {
                "kind": "sdk_error",
                "operation": "store_reopen",
                "access_path": None,
                "code": "store.sqlite-failure",
                "field": "open",
                "detail": {"kind": "stable", "value": "reopen-observation"},
            },
            "diagnostic_digest": "sha256:" + "0" * 64,
        }
        self.assertEqual(
            materialization.classify_postpublish_failure(
                reopen_failed,
                reopen_failure,
                first_cause_authority=authority(reopen_failure),
            ),
            "committed_unverified",
        )
        reopen_detail_drift = copy.deepcopy(reopen_failure)
        reopen_detail_drift["cause"]["detail"]["value"] += "-forged"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "source-private observation",
        ):
            materialization.classify_postpublish_failure(
                reopen_failed,
                reopen_detail_drift,
                first_cause_authority=authority(reopen_failure),
            )

        for index, access_path in enumerate(paths):
            attempt = copy.deepcopy(opened)
            attempt["handle_receipts"][index] = receipt(access_path, "error")
            failure = {
                "stage": access_path,
                "access_path": access_path,
                "code": "materialization.store-failure",
                "cause": {
                    "kind": "sdk_error",
                    "operation": materialization.POSTPUBLISH_PATH_OPERATIONS[
                        access_path
                    ],
                    "access_path": access_path,
                    "code": "store.sqlite-failure",
                    "field": "database",
                    "detail": {
                        "kind": "stable",
                        "value": f"exact-{access_path}-detail",
                    },
                },
                "diagnostic_digest": "sha256:" + str(index + 1) * 64,
            }
            observed = authority(failure)
            with self.subTest(operation=failure["cause"]["operation"]):
                self.assertEqual(
                    materialization.classify_postpublish_failure(
                        attempt,
                        failure,
                        first_cause_authority=observed,
                    ),
                    "committed_unverified",
                )
                detail_drift = copy.deepcopy(failure)
                detail_drift["cause"]["detail"]["value"] += "-forged"
                with self.assertRaisesRegex(
                    materialization.MaterializationError,
                    "source-private observation",
                ):
                    materialization.classify_postpublish_failure(
                        attempt,
                        detail_drift,
                        first_cause_authority=observed,
                    )

        first_attempt = copy.deepcopy(opened)
        first_attempt["handle_receipts"][0] = receipt("current-selector", "error")
        first_attempt["handle_receipts"][1] = receipt("open-publication", "error")
        later_failure = {
            "stage": "open-publication",
            "access_path": "open-publication",
            "code": "materialization.store-failure",
            "cause": {
                "kind": "sdk_error",
                "operation": "verify_open_publication",
                "access_path": "open-publication",
                "code": "store.sqlite-failure",
                "field": "database",
                "detail": {"kind": "stable", "value": "later"},
            },
            "diagnostic_digest": "sha256:" + "4" * 64,
        }
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "first reopened handle failure",
        ):
            materialization.classify_postpublish_failure(
                first_attempt,
                later_failure,
                first_cause_authority=authority(later_failure),
            )
        first_failure = copy.deepcopy(later_failure)
        first_failure["stage"] = "current-selector"
        first_failure["access_path"] = "current-selector"
        first_failure["cause"]["operation"] = "verify_current"
        first_failure["cause"]["access_path"] = "current-selector"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "caller-provided.*is not first",
        ):
            materialization.classify_postpublish_failure(
                first_attempt,
                first_failure,
                first_attempt["handle_receipts"][1],
                first_cause_authority=authority(first_failure),
            )

        for stage, path_bindings in materialization.POSTPUBLISH_MISMATCH_BINDINGS.items():
            for access_path, projections in path_bindings.items():
                for projection in projections:
                    mismatch = {
                        "stage": stage,
                        "access_path": access_path,
                        "code": "materialization.store-failure",
                        "cause": {
                            "kind": "verification_mismatch",
                            "operation": "verify_projection",
                            "access_path": access_path,
                            "projection": projection,
                            "expected_digest": "sha256:" + "a" * 64,
                            "actual_digest": "sha256:" + "b" * 64,
                        },
                        "diagnostic_digest": "sha256:" + "5" * 64,
                    }
                    with self.subTest(
                        stage=stage,
                        access_path=access_path,
                        projection=projection,
                    ):
                        self.assertEqual(
                            materialization.classify_postpublish_failure(
                                opened,
                                mismatch,
                                first_cause_authority=authority(mismatch),
                            ),
                            "committed_unverified",
                        )

        invalid_bindings = []
        wrong_stage = {
            "stage": "rows",
            "access_path": "open-snapshot",
            "code": "materialization.store-failure",
            "cause": {
                "kind": "verification_mismatch",
                "operation": "verify_projection",
                "access_path": "open-snapshot",
                "projection": "snapshot-manifest",
                "expected_digest": "sha256:" + "a" * 64,
                "actual_digest": "sha256:" + "b" * 64,
            },
            "diagnostic_digest": "sha256:" + "6" * 64,
        }
        invalid_bindings.append(wrong_stage)
        wrong_access = copy.deepcopy(wrong_stage)
        wrong_access["stage"] = "publication-binding"
        wrong_access["cause"]["projection"] = "publication-semantic-fields"
        invalid_bindings.append(wrong_access)
        cause_path_drift = copy.deepcopy(wrong_stage)
        cause_path_drift["stage"] = "snapshot-binding"
        cause_path_drift["cause"]["access_path"] = "current-selector"
        invalid_bindings.append(cause_path_drift)
        aliased = copy.deepcopy(wrong_stage)
        aliased["stage"] = "snapshot-binding"
        aliased["cause"]["actual_digest"] = aliased["cause"]["expected_digest"]
        invalid_bindings.append(aliased)
        for index, invalid in enumerate(invalid_bindings):
            with self.subTest(invalid_projection_binding=index):
                with self.assertRaisesRegex(
                    materialization.MaterializationError,
                    "exact projection mismatch",
                ):
                    materialization.classify_postpublish_failure(
                        opened,
                        invalid,
                        first_cause_authority=authority(invalid),
                    )

        exact_mismatch = {
            "stage": "snapshot-binding",
            "access_path": "open-snapshot",
            "code": "materialization.store-failure",
            "cause": {
                "kind": "verification_mismatch",
                "operation": "verify_projection",
                "access_path": "open-snapshot",
                "projection": "snapshot-manifest",
                "expected_digest": "sha256:" + "7" * 64,
                "actual_digest": "sha256:" + "8" * 64,
            },
            "diagnostic_digest": "sha256:" + "9" * 64,
        }
        observed_mismatch = authority(exact_mismatch)
        for field in ("expected_digest", "actual_digest"):
            changed = copy.deepcopy(exact_mismatch)
            changed["cause"][field] = "sha256:" + "f" * 64
            with self.subTest(recomputed_digest=field):
                with self.assertRaisesRegex(
                    materialization.MaterializationError,
                    "source-private observation",
                ):
                    materialization.classify_postpublish_failure(
                        opened,
                        changed,
                        first_cause_authority=observed_mismatch,
                    )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "source-private observation",
        ):
            materialization.classify_postpublish_failure(
                opened,
                exact_mismatch,
                first_cause_authority=None,
            )

    def test_committed_unverified_validator_binds_source_private_sdk_detail(
        self,
    ) -> None:
        request = self.request("static", "sqlite")
        report = self.report(request)
        publication = report["publication"]
        invocation_record = publication["invocation_committed_record"]
        reopened = report["semantic_verification"]["reopened_store"]
        receipts = copy.deepcopy(reopened["handle_receipts"])
        failed_receipt = receipts[2]
        failed_receipt.update(
            {
                "status": "error",
                "sdk_code": "store.sqlite-failure",
                "sdk_field": "database",
                "projection": None,
            }
        )
        cause = {
            "kind": "sdk_error",
            "operation": "verify_open_snapshot",
            "access_path": "open-snapshot",
            "code": "store.sqlite-failure",
            "field": "database",
            "detail": {
                "kind": "opaque",
                "byte_count": len(b"exact sdk diagnostic"),
                "digest": materialization.content_digest(b"exact sdk diagnostic"),
                "diagnostic": "exact sdk diagnostic",
            },
        }
        diagnostic_digest = materialization.content_digest(
            b"post-publish verification failed"
        )
        failure = {
            "stage": "open-snapshot",
            "access_path": "open-snapshot",
            "code": "materialization.store-failure",
            "cause": copy.deepcopy(cause),
            "diagnostic_digest": diagnostic_digest,
        }
        not_applicable = {
            "status": "not_applicable",
            "requested_publication_id": None,
            "record": None,
            "error_code": None,
            "error_field": None,
        }
        present_current = {
            "status": "present",
            "requested_publication_id": None,
            "record": copy.deepcopy(invocation_record),
            "error_code": None,
            "error_field": None,
        }
        present_candidate = {
            "status": "present",
            "requested_publication_id": invocation_record["publication_id"],
            "record": copy.deepcopy(invocation_record),
            "error_code": None,
            "error_field": None,
        }
        publication.update(
            {
                "outcome": "committed_unverified",
                "store_failure": {
                    "category": "post_commit_verification",
                    "cause": copy.deepcopy(cause),
                    "diagnostic_digest": diagnostic_digest,
                },
                "sqlite_reopen_status": "opened",
                "recovery_receipt": {
                    "selector": materialization._expected_recovery_selector(request),
                    "reopen_status": "opened",
                    "open_error_code": None,
                    "open_error_field": None,
                    "current": present_current,
                    "expected_parent": not_applicable,
                    "candidate": present_candidate,
                },
            }
        )
        report["result"] = "failed"
        report["process_exit_status"] = 1
        report["error"] = {
            "code": "materialization.store-failure",
            "phase": "post-publication-verification",
            "subject": invocation_record["publication_id"],
            "diagnostic": "post-publish verification failed",
        }
        report["semantic_verification"] = {
            "status": "committed_unverified",
            "reopened_store": None,
            "reopen_attempt": {
                "backend": "sqlite",
                "close_reopen_status": "opened",
                "open_error_code": None,
                "open_error_field": None,
                "handle_receipts": receipts,
            },
            "failure": failure,
        }
        first_cause_authority = copy.deepcopy(
            materialization._postpublish_first_cause_authority(failure)
        )
        self.validate_report(
            request,
            report,
            postpublish_failure_authority=first_cause_authority,
        )

        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "source-private observation",
        ):
            self.validate_report(request, report)

        forged = copy.deepcopy(report)
        forged["semantic_verification"]["failure"]["cause"]["detail"][
            "diagnostic"
        ] = "forged sdk diagnostic"
        forged_detail = forged["semantic_verification"]["failure"]["cause"][
            "detail"
        ]
        forged_detail["byte_count"] = len(forged_detail["diagnostic"].encode())
        forged_detail["digest"] = materialization.content_digest(
            forged_detail["diagnostic"].encode()
        )
        forged["publication"]["store_failure"]["cause"] = copy.deepcopy(
            forged["semantic_verification"]["failure"]["cause"]
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "source-private observation",
        ):
            self.validate_report(
                request,
                forged,
                postpublish_failure_authority=first_cause_authority,
            )

    def test_committed_unverified_retains_closure_projection_mismatch(self) -> None:
        request = self.request("static", "sqlite")
        report = self.report(request)
        publication = report["publication"]
        invocation_record = publication["invocation_committed_record"]
        reopened = report["semantic_verification"]["reopened_store"]
        receipts = copy.deepcopy(reopened["handle_receipts"])

        current_projection = receipts[0]["projection"]
        expected_closure_digest = current_projection["closure_digest"]
        actual_closure_digest = materialization.semantic_digest(
            "cxxlens.fixture.reopened-closure-mismatch.v1",
            "observed-closure",
        )
        current_projection["closure_digest"] = actual_closure_digest
        self.rebind_reopened_semantic_projection(current_projection)
        self.rebind_reopened_handle_projection(current_projection)

        cause = {
            "kind": "verification_mismatch",
            "operation": "verify_projection",
            "access_path": "current-selector",
            "projection": "closure",
            "expected_digest": expected_closure_digest,
            "actual_digest": actual_closure_digest,
        }
        diagnostic = "post-publish closure verification failed"
        diagnostic_digest = materialization.content_digest(diagnostic.encode())
        failure = {
            "stage": "closure",
            "access_path": "current-selector",
            "code": "materialization.store-failure",
            "cause": copy.deepcopy(cause),
            "diagnostic_digest": diagnostic_digest,
        }
        not_applicable = {
            "status": "not_applicable",
            "requested_publication_id": None,
            "record": None,
            "error_code": None,
            "error_field": None,
        }
        present_current = {
            "status": "present",
            "requested_publication_id": None,
            "record": copy.deepcopy(invocation_record),
            "error_code": None,
            "error_field": None,
        }
        present_candidate = {
            "status": "present",
            "requested_publication_id": invocation_record["publication_id"],
            "record": copy.deepcopy(invocation_record),
            "error_code": None,
            "error_field": None,
        }
        publication.update(
            {
                "outcome": "committed_unverified",
                "store_failure": {
                    "category": "post_commit_verification",
                    "cause": copy.deepcopy(cause),
                    "diagnostic_digest": diagnostic_digest,
                },
                "sqlite_reopen_status": "opened",
                "recovery_receipt": {
                    "selector": materialization._expected_recovery_selector(request),
                    "reopen_status": "opened",
                    "open_error_code": None,
                    "open_error_field": None,
                    "current": present_current,
                    "expected_parent": not_applicable,
                    "candidate": present_candidate,
                },
            }
        )
        report["result"] = "failed"
        report["process_exit_status"] = 1
        report["error"] = {
            "code": "materialization.store-failure",
            "phase": "post-publication-verification",
            "subject": invocation_record["publication_id"],
            "diagnostic": diagnostic,
        }
        report["semantic_verification"] = {
            "status": "committed_unverified",
            "reopened_store": None,
            "reopen_attempt": {
                "backend": "sqlite",
                "close_reopen_status": "opened",
                "open_error_code": None,
                "open_error_field": None,
                "handle_receipts": receipts,
            },
            "failure": failure,
        }
        first_cause_authority = copy.deepcopy(
            materialization._postpublish_first_cause_authority(failure)
        )
        self.validate_report(
            request,
            report,
            postpublish_failure_authority=first_cause_authority,
        )

        forged_projection = copy.deepcopy(report)
        forged_closure = forged_projection["semantic_verification"][
            "reopen_attempt"
        ]["handle_receipts"][0]["projection"]
        forged_closure["closure_digest"] = materialization.semantic_digest(
            "cxxlens.fixture.reopened-closure-mismatch.v1",
            "forged-retained-closure",
        )
        self.rebind_reopened_semantic_projection(forged_closure)
        self.rebind_reopened_handle_projection(forged_closure)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "retained projection mismatch",
        ):
            self.validate_report(
                request,
                forged_projection,
                postpublish_failure_authority=first_cause_authority,
            )

        wrong_stage = copy.deepcopy(report)
        wrong_stage["semantic_verification"]["failure"]["stage"] = "unresolved"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "exact projection mismatch",
        ):
            self.validate_report(
                request,
                wrong_stage,
                postpublish_failure_authority=(
                    materialization._postpublish_first_cause_authority(wrong_stage["semantic_verification"]["failure"])
                ),
            )

        forged_actual = copy.deepcopy(report)
        forged_actual["semantic_verification"]["failure"]["cause"][
            "actual_digest"
        ] = materialization.semantic_digest(
            "cxxlens.fixture.reopened-closure-mismatch.v1",
            "forged-closure",
        )
        forged_actual["publication"]["store_failure"]["cause"] = copy.deepcopy(
            forged_actual["semantic_verification"]["failure"]["cause"]
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "source-private observation",
        ):
            self.validate_report(
                request,
                forged_actual,
                postpublish_failure_authority=first_cause_authority,
            )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "retained projection mismatch",
        ):
            self.validate_report(
                request,
                forged_actual,
                postpublish_failure_authority=(
                    materialization._postpublish_first_cause_authority(
                        forged_actual["semantic_verification"]["failure"]
                    )
                ),
            )

    def test_unsealed_or_partial_group_and_batch_are_rejected(self) -> None:
        request = self.request()
        unsealed = self.report(request)
        unsealed["task_results"][0]["groups"][0]["sealed"] = False
        with self.assertRaisesRegex(
            materialization.MaterializationError, "materialization.group-incomplete"
        ):
            self.validate_report(request, unsealed)

        missing_batch = self.report(request)
        missing_batch["task_results"][0]["batches"].pop()
        with self.assertRaisesRegex(
            materialization.MaterializationError, "materialization report"
        ):
            self.validate_report(request, missing_batch)

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
        canonical_raw = (
            materialization.bind_fixture_runtime_occurrences_for_report(
                ROOT, request, canonical
            )
        )
        materialization.rebind_report_digest_chain(ROOT, request, canonical)
        self.validate_report(
            request,
            canonical,
            runtime_raw_occurrences=canonical_raw,
        )

        observation = self.report(request)
        empty_batch(observation, "frontend.clang22.type_observation.v2")
        observation_raw = (
            materialization.bind_fixture_runtime_occurrences_for_report(
                ROOT, request, observation
            )
        )
        materialization.rebind_report_digest_chain(ROOT, request, observation)
        self.validate_report(
            request,
            observation,
            runtime_raw_occurrences=observation_raw,
        )

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
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            self.validate_report(request, zero_with_chunk)

        zero_with_binding = self.report(request)
        target = batch(zero_with_binding)
        target.update(
            {
                "row_count": 0,
                "ordered_chunk_digests": [],
                "provenance_edge_digests": [],
            }
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            self.validate_report(request, zero_with_binding)

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
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            self.validate_report(request, zero_with_provenance)

        nonzero_without_chunk = self.report(request)
        batch(nonzero_without_chunk)["ordered_chunk_digests"] = []
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            self.validate_report(request, nonzero_without_chunk)

        nonzero_without_binding = self.report(request)
        batch(nonzero_without_binding)["row_bindings"] = []
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            self.validate_report(request, nonzero_without_binding)

        nonzero_without_provenance = self.report(request)
        batch(nonzero_without_provenance)["provenance_edge_digests"] = []
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            self.validate_report(request, nonzero_without_provenance)

    def test_span_id_range_and_absent_accounting_are_fail_closed(self) -> None:
        request = self.request()
        mismatched = self.report(request)
        mismatched["span_validation"]["recomputed_id_mismatch_count"] = 1
        with self.assertRaisesRegex(
            materialization.MaterializationError, "materialization.span-invalid"
        ):
            self.validate_report(request, mismatched)

        absent = self.report(request)
        absent["span_validation"]["absent_bundle_count"] = 1
        with self.assertRaisesRegex(
            materialization.MaterializationError, "absent span accounting"
        ):
            self.validate_report(request, absent)

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
            self.validate_report(request, exact_with_accounted_absence)

    def test_detailed_report_rejects_nonexact_absent_call_span(self) -> None:
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
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            self.validate_report(request, report)

    def test_observation_span_census_and_unique_count_are_fail_closed(self) -> None:
        request = self.request()
        census_bypass = self.report(request)
        census_bypass["span_validation"]["observed_bundle_count"] = 1
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "validated span row census differs|observation/span bundle census differs",
        ):
            self.validate_report(request, census_bypass)

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
            self.validate_report(request, unique_bypass)

    def test_detailed_report_rejects_nonexact_unresolved_category_accounting(
        self,
    ) -> None:
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
            "materialization.report-invalid",
        ):
            self.validate_report(request, report)

    def test_stale_parent_cannot_forge_success_and_valid_failure_preserves_head(self) -> None:
        request = self.request("static", "sqlite")
        forged = self.report(request)
        forged["publication"]["expected_parent_publication"] = (
            "publication:sha256:" + "0" * 64
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError, "materialization.stale-parent"
        ):
            self.validate_report(request, forged)

        failed = materialization.stale_parent_report(ROOT, request)
        self.validate_report(request, failed)
        publication = failed["publication"]
        self.assertEqual(publication["outcome"], "rejected_stale")
        self.assertEqual(publication["candidate_identity_state"], "constructed")
        self.assertEqual(publication["invocation_commit_state"], "not_committed")
        self.assertEqual(publication["committed_transaction_count"], 0)
        self.assertIsNone(publication["invocation_committed_record"])
        self.assertEqual(publication["candidate_visibility"], "absent")
        self.assertTrue(publication["prior_history_retained"])
        self.assertEqual(
            publication["recovery_receipt"]["candidate"]["status"],
            "not_found",
        )
        self.assertEqual(failed["semantic_verification"]["status"], "not_published")

    def test_passed_sqlite_requires_reopen_and_memory_forbids_false_claim(self) -> None:
        sqlite_request = self.request("static", "sqlite")
        sqlite_report = self.report(sqlite_request)
        sqlite_report["publication"]["sqlite_reopen_status"] = "not_applicable"
        with self.assertRaisesRegex(
            materialization.MaterializationError, "materialization report"
        ):
            self.validate_report(sqlite_request, sqlite_report)

        memory_request = self.request("static", "memory")
        memory_report = self.report(memory_request)
        memory_report["publication"]["sqlite_reopen_status"] = "opened"
        with self.assertRaisesRegex(
            materialization.MaterializationError, "materialization report"
        ):
            self.validate_report(memory_request, memory_report)

    def test_open_snapshot_receipt_may_return_same_snapshot_from_another_series(
        self,
    ) -> None:
        request = self.request("static", "sqlite")
        report = self.report(request)
        receipt = report["semantic_verification"]["reopened_store"][
            "handle_receipts"
        ][2]
        record = receipt["projection"]["publication_record"]
        record.update(
            {
                "series_id": "snapshot-series:sha256:" + "f" * 64,
                "sequence": 7,
                "physical_generation": 9,
                "parent_publication": "publication:sha256:" + "e" * 64,
            }
        )
        self.rebind_publication_record(record)
        self.rebind_reopened_handle_projection(receipt["projection"])

        self.validate_report(request, report)

    def test_reopened_handle_publication_and_semantic_drift_is_rejected(
        self,
    ) -> None:
        request = self.request("static", "sqlite")

        for index, access_path in ((0, "current-selector"), (1, "open-publication")):
            report = self.report(request)
            receipt = report["semantic_verification"]["reopened_store"][
                "handle_receipts"
            ][index]
            record = receipt["projection"]["publication_record"]
            record["snapshot_id"] = "snapshot:sha256:" + "d" * 64
            self.rebind_publication_record(record)
            self.rebind_reopened_handle_projection(receipt["projection"])
            with self.subTest(access_path=access_path):
                with self.assertRaisesRegex(
                    materialization.MaterializationError,
                    f"{access_path} recovery publication transition differs",
                ):
                    self.validate_report(request, report)

        snapshot_drift = self.report(request)
        snapshot_receipt = snapshot_drift["semantic_verification"]["reopened_store"][
            "handle_receipts"
        ][2]
        snapshot_record = snapshot_receipt["projection"]["publication_record"]
        snapshot_record["snapshot_id"] = "snapshot:sha256:" + "c" * 64
        self.rebind_publication_record(snapshot_record)
        self.rebind_reopened_handle_projection(snapshot_receipt["projection"])
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "open-snapshot returned a different semantic snapshot",
        ):
            self.validate_report(request, snapshot_drift)

        generation_regression = self.report(request)
        current_receipt = generation_regression["semantic_verification"][
            "reopened_store"
        ]["handle_receipts"][0]
        current_receipt["projection"]["publication_record"][
            "physical_generation"
        ] = 0
        self.rebind_reopened_handle_projection(current_receipt["projection"])
        with self.assertRaises(materialization.MaterializationError) as caught:
            self.validate_report(request, generation_regression)
        self.assertEqual(caught.exception.code, "materialization.report-invalid")

        semantic_drift = self.report(request)
        semantic_receipt = semantic_drift["semantic_verification"]["reopened_store"][
            "handle_receipts"
        ][2]
        semantic_receipt["projection"]["canonical_export_digest"] = (
            "sha256:" + "b" * 64
        )
        self.rebind_reopened_semantic_projection(semantic_receipt["projection"])
        self.rebind_reopened_handle_projection(semantic_receipt["projection"])
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "reopened handle semantic snapshot projection differs",
        ):
            self.validate_report(request, semantic_drift)

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

    def test_qualification_three_tuple_binds_exact_raw_request_bytes(self) -> None:
        entries = []
        for configuration in ("static", "shared"):
            for backend in ("memory", "sqlite"):
                request = self.request(configuration, backend)
                request_bytes = (
                    b" \n" + materialization.canonical_json(request) + b"\n\t"
                )
                report = materialization.sample_report(
                    ROOT,
                    request,
                    request_bytes=request_bytes,
                )
                entries.append((request, report, request_bytes))

        materialization.validate_qualification_matrix(ROOT, entries)

        request, report, _ = entries[0]
        wrong_bytes = copy.deepcopy(entries)
        wrong_bytes[0] = (
            request,
            report,
            materialization.canonical_json(request),
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "raw input observation differs from exact transport bytes",
        ):
            materialization.validate_qualification_matrix(ROOT, wrong_bytes)

        non_bytes = copy.deepcopy(entries)
        non_bytes[0] = (request, report, "not-bytes")
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "qualification exact request artifact is not bytes",
        ):
            materialization.validate_qualification_matrix(ROOT, non_bytes)

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
            self.validate_report(request, wrong_source)

        wrong_authority = self.report(request)
        wrong_authority["authority_digests"][0]["digest"] = "sha256:" + "0" * 64
        with self.assertRaisesRegex(
            materialization.MaterializationError, "authority digests"
        ):
            self.validate_report(request, wrong_authority)

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
            self.validate_report(request, report)

        digest_drift = self.report(request)
        digest_drift["base_claims"]["descriptor_results"][0]["row_set_digest"] = (
            "semantic-v2:sha256:" + "0" * 64
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.claim-invalid",
        ):
            self.validate_report(request, digest_drift)

    def test_base_row_context_and_span_bundle_edges_cannot_be_swapped(self) -> None:
        request = self.request(translation_unit_count=2)
        report = self.report(request)
        self.validate_report(request, report)

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
            self.validate_report(request, swapped_rows)

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
            self.validate_report(request, swapped_span)

    def test_genesis_parent_binding_is_exact(self) -> None:
        genesis = self.request()
        genesis["publication"]["genesis"] = True
        genesis["publication"]["expected_parent_publication"] = None
        materialization.bind_request_identity(genesis)
        materialization.validate_request(ROOT, genesis)
        self.validate_report(genesis, self.report(genesis))

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
        self.validate_report(request, report)

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
        materialization.bind_engine_policy_and_selector_identities(
            global_catalog_drift
        )
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

    def test_worker_v3_maximum_projection_proof_is_schema_derived(self) -> None:
        request_schema = materialization.load(ROOT / materialization.REQUEST_SCHEMA)
        proof = materialization.maximum_worker_task_v3_projection_proof(
            request_schema
        )
        contract = materialization.load(ROOT / materialization.CONTRACT)
        self.assertEqual(
            proof,
            contract["project_and_tasks"]["worker_task_v3"][
                "maximum_projection_proof"
            ],
        )
        self.assertEqual(
            proof["maximum_projection_bytes"],
            proof["outer_tuple_framing_bytes"]
            + sum(proof["component_maximum_bytes"].values()),
        )
        self.assertEqual(
            proof["saturated_vector"]["byte_count"],
            proof["maximum_projection_bytes"],
        )
        self.assertLessEqual(
            proof["maximum_projection_bytes"],
            materialization.MAXIMUM_TASK_INPUT_BYTES,
        )

        saturated = bytearray()
        streamed_bytes = materialization.stream_worker_task_v3_saturated_vector(
            request_schema, saturated.extend
        )
        self.assertEqual(streamed_bytes, len(saturated))
        self.assertEqual(streamed_bytes, proof["maximum_projection_bytes"])
        self.assertEqual(
            "sha256:" + hashlib.sha256(saturated).hexdigest(),
            proof["saturated_vector"]["digest"],
        )

        decoded_items = 0

        def read_u64(data: memoryview, offset: int, limit: int) -> tuple[int, int]:
            self.assertLessEqual(offset + 8, limit)
            return int.from_bytes(data[offset : offset + 8], "big"), offset + 8

        def decode_item(
            data: memoryview,
            offset: int,
            limit: int,
        ) -> tuple[int, int]:
            nonlocal decoded_items
            self.assertLess(offset, limit)
            tag = data[offset]
            decoded_items += 1
            offset += 1
            if tag == 0:
                return tag, offset
            if tag == 1:
                self.assertLess(offset, limit)
                self.assertIn(data[offset], (0, 1))
                return tag, offset + 1
            if tag == 2:
                self.assertLess(offset, limit)
                self.assertIn(data[offset], (0, 1))
                width, offset = read_u64(data, offset + 1, limit)
                self.assertGreaterEqual(width, 1)
                self.assertLessEqual(width, 8)
                self.assertLessEqual(offset + width, limit)
                return tag, offset + width
            if tag in (3, 4):
                size, offset = read_u64(data, offset, limit)
                end = offset + size
                self.assertLessEqual(end, limit)
                if tag == 4:
                    decoder = codecs.getincrementaldecoder("utf-8")("strict")
                    while offset < end:
                        chunk_end = min(offset + 1_048_576, end)
                        decoder.decode(bytes(data[offset:chunk_end]), final=False)
                        offset = chunk_end
                    decoder.decode(b"", final=True)
                return tag, end
            self.assertEqual(tag, 5)
            count, offset = read_u64(data, offset, limit)
            for _ in range(count):
                item_size, offset = read_u64(data, offset, limit)
                item_end = offset + item_size
                self.assertLessEqual(item_end, limit)
                _, decoded_end = decode_item(data, offset, item_end)
                self.assertEqual(decoded_end, item_end)
                offset = item_end
            return tag, offset

        saturated_view = memoryview(saturated)
        top_tag, decoded_end = decode_item(
            saturated_view, 0, len(saturated_view)
        )
        self.assertEqual(top_tag, 5)
        self.assertEqual(int.from_bytes(saturated_view[1:9], "big"), 5)
        self.assertEqual(decoded_end, len(saturated_view))
        self.assertGreater(decoded_items, proof["maximum_expanded_leaf_value_count"])
        saturated_view.release()
        del saturated

        bounded_request = self.request(translation_unit_count=2)
        for task in bounded_request["tasks"]:
            self.assertLessEqual(
                len(
                    materialization.worker_task_v3_projection(
                        bounded_request, task
                    )
                ),
                proof["maximum_projection_bytes"],
            )

        exact_limit = proof["maximum_projection_bytes"]
        at_limit = materialization.maximum_worker_task_v3_projection_proof(
            request_schema,
            transfer_limit=exact_limit,
        )
        self.assertEqual(at_limit["margin_bytes"], 0)
        plus_one = materialization.maximum_worker_task_v3_projection_proof(
            request_schema,
            transfer_limit=exact_limit + 1,
        )
        self.assertEqual(plus_one["margin_bytes"], 1)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "exceeds transfer limit",
        ):
            materialization.maximum_worker_task_v3_projection_proof(
                request_schema,
                transfer_limit=exact_limit - 1,
            )

        missing_bound = copy.deepcopy(request_schema)
        missing_bound["$defs"]["strong_id"].pop(
            "x-cxxlens-max-utf8-bytes"
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "missing a finite UTF-8 byte bound",
        ):
            materialization.maximum_worker_task_v3_projection_proof(missing_bound)

        missing_count = copy.deepcopy(request_schema)
        missing_count["properties"]["project"]["properties"][
            "catalog_compile_units"
        ].pop("maxItems")
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "lacks one finite homogeneous array bound",
        ):
            materialization.maximum_worker_task_v3_projection_proof(missing_count)

        missing_global_required = copy.deepcopy(request_schema)
        missing_global_required["properties"]["project"]["required"].remove(
            "catalog_id"
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "exact required task.v3 global catalog field census",
        ):
            materialization.maximum_worker_task_v3_projection_proof(
                missing_global_required
            )

        missing_global_property = copy.deepcopy(request_schema)
        missing_global_property["properties"]["project"]["properties"].pop(
            "catalog_id"
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "exact required task.v3 global catalog field census",
        ):
            materialization.maximum_worker_task_v3_projection_proof(
                missing_global_property
            )

        missing_nested_required = copy.deepcopy(request_schema)
        missing_nested_required["properties"]["tasks"]["items"]["properties"][
            "source"
        ]["required"].remove("content_digest")
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "exact object field census",
        ):
            materialization.maximum_worker_task_v3_projection_proof(
                missing_nested_required
            )

        missing_nested_property = copy.deepcopy(request_schema)
        missing_nested_property["properties"]["tasks"]["items"]["properties"][
            "source"
        ]["properties"].pop("content_digest")
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "exact object field census",
        ):
            materialization.maximum_worker_task_v3_projection_proof(
                missing_nested_property
            )

        forged_contract = copy.deepcopy(contract)
        forged_contract["project_and_tasks"]["worker_task_v3"][
            "maximum_projection_proof"
        ]["maximum_projection_bytes"] += 1
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "worker task v3 binding differs",
        ):
            materialization.validate_contract_exact(
                forged_contract, request_schema
            )

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
        materialization.bind_engine_policy_and_selector_identities(aliased)
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
            self.validate_report(request, duplicate_result)

        missing_result = self.report(request)
        missing_result["task_results"].pop()
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "not every requested task has one result",
        ):
            self.validate_report(request, missing_result)

        extra_result = self.report(request)
        extra_copy = copy.deepcopy(extra_result["task_results"][0])
        extra_copy["provider_execution_id"] = "provider-execution:extra"
        extra_result["task_results"].append(extra_copy)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "not every requested task has one result",
        ):
            self.validate_report(request, extra_result)

        selected_drift = self.report(request)
        selected_drift["task_results"][0]["selected_catalog_compile_unit_id"] = (
            request["tasks"][1]["selected_catalog_compile_unit_id"]
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "task report differs at selected_catalog_compile_unit_id",
        ):
            self.validate_report(request, selected_drift)

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
        materialization.bind_engine_policy_and_selector_identities(request)
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
                    self.validate_report(request, report)

    def test_store_condition_canonical_form_uses_utf8_byte_lengths(self) -> None:
        condition = {
            "universe": "条件:世界",
            "fragments": ["alpha", "β", "条件"],
        }
        expected = "13:条件:世界;5:alpha;2:β;6:条件"
        self.assertEqual(
            materialization.claim_condition_canonical_form(condition),
            expected,
        )
        self.assertNotEqual(
            expected,
            "5:条件:世界;5:alpha;1:β;2:条件",
        )

    def test_store_claim_partition_snapshot_and_publication_dag_is_exact(
        self,
    ) -> None:
        request = self.request()

        def claim_partitions(report: dict) -> list[dict]:
            return [
                partition
                for partition in report["store"]["partitions"]
                if partition["stored_claim_refs"]
            ]

        def add_unreferenced_content(report: dict) -> None:
            claim_partitions(report)[0]["claim_content_digests"].append(
                "claim-content:sha256:" + "0" * 64
            )

        def drop_stored_claim_ref(report: dict) -> None:
            claim_partitions(report)[0]["stored_claim_refs"].pop()

        def substitute_stored_claim_ref(report: dict) -> None:
            first, second = claim_partitions(report)[:2]
            first["stored_claim_refs"][0] = second["stored_claim_refs"][0]

        mutations = {
            "unreferenced-content": add_unreferenced_content,
            "dropped-stored-claim-ref": drop_stored_claim_ref,
            "substituted-stored-claim-ref": substitute_stored_claim_ref,
            "coverage-key": lambda report: report["store"]["partitions"][0][
                "coverage_units"
            ][0].__setitem__(
                "key", "materialization-base-descriptor:sha256:" + "1" * 64
            ),
            "partition-id": lambda report: report["store"]["partitions"][
                0
            ].__setitem__("partition_id", "partition:sha256:" + "2" * 64),
            "snapshot-id": lambda report: report["store"][
                "snapshot_manifest"
            ].__setitem__("snapshot_id", "snapshot:sha256:" + "3" * 64),
            "publication-id": lambda report: report["publication"][
                "invocation_committed_record"
            ].__setitem__("publication_id", "publication:sha256:" + "4" * 64),
        }
        for label, mutate in mutations.items():
            report = self.report(request)
            mutate(report)
            with self.subTest(label=label):
                with self.assertRaises(materialization.MaterializationError):
                    self.validate_report(request, report)

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
            self.validate_report(request, missing)

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
            self.validate_report(request, canonical_extra)

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
            self.validate_report(request, forged_exact)

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
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            self.validate_report(request, under)

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
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
        ):
            self.validate_report(request, pairing)

        matrix = self.matrix()
        matrix[0] = (request, under)
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "materialization.report-invalid",
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
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "worker row/provenance occurrence binding differs|row is attributed to another task",
        ):
            materialization.rebind_report_digest_chain(ROOT, request, report)

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
        runtime_raw = materialization.bind_fixture_runtime_occurrences_for_report(
            ROOT,
            request,
            report,
        )
        materialization.rebind_report_digest_chain(ROOT, request, report)
        self.validate_report(
            request,
            report,
            runtime_raw_occurrences=runtime_raw,
        )

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
            self.validate_report(
                request,
                report,
                runtime_raw_occurrences=runtime_raw,
            )

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

    def test_machine_contract_requires_bounded_two_phase_report_lifecycle(self) -> None:
        accepted = materialization.load(ROOT / materialization.CONTRACT)
        materialization.validate_contract_exact(copy.deepcopy(accepted))

        contract = copy.deepcopy(accepted)
        contract["surface"]["resource_limits"]["report_construction"].pop(
            "lifecycle_order"
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "bounded two-phase report lifecycle differs",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(accepted)
        contract["surface"]["resource_limits"]["report_construction"][
            "prepublication_forbidden_claims"
        ].remove("physical-generation")
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "bounded two-phase report lifecycle differs",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(accepted)
        contract["surface"]["resource_limits"]["report_construction"][
            "publication_dependent_source"
        ] = "caller-projection-permitted"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "bounded two-phase report lifecycle differs",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(accepted)
        contract["surface"]["resource_limits"]["report_construction"][
            "publication_attempt_boundary"
        ] = "after-publish-return"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "bounded two-phase report lifecycle differs",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(accepted)
        contract["surface"]["resource_limits"]["report_construction"][
            "capacity_reservation"
        ]["bound"] = "unchecked-current-outcome-only"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "bounded two-phase report lifecycle differs",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(accepted)
        contract["surface"]["resource_limits"]["report_construction"][
            "committed_verified_reopen_order"
        ].reverse()
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "bounded two-phase report lifecycle differs",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(accepted)
        contract["surface"]["resource_limits"]["report_construction"][
            "stdout_authority"
        ]["operating-system-atomicity"] = "required"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "bounded two-phase report lifecycle differs",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(accepted)
        contract["surface"]["resource_limits"]["report_construction"][
            "stdout_authority"
        ]["partial-or-short-write"] = "authoritative"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "bounded two-phase report lifecycle differs",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(accepted)
        contract["surface"]["process_exit"][
            "post_publication_attempt_finalization_failure"
        ] = "compact-zero-effect-permitted"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "installed machine surface is not exact",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(accepted)
        contract["surface"]["resource_limits"]["allocation_failure"][
            "prepublication_report_construction"
        ] = "exit-two-only"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "installed machine surface is not exact",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(accepted)
        contract["report"]["response_union"]["compact_failure"][
            "report_construction_phase"
        ] = "prepublication-zero-effect-only"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "compact report-construction boundary differs",
        ):
            materialization.validate_contract_exact(contract)

        contract = copy.deepcopy(accepted)
        contract["acceptance"].append("bounded-spool-before-publication")
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "legacy prepublication-complete report lifecycle was reintroduced",
        ):
            materialization.validate_contract_exact(contract)

        contract_schema = materialization.load(ROOT / materialization.CONTRACT_SCHEMA)
        contract = copy.deepcopy(accepted)
        contract["surface"]["resource_limits"]["report_construction"][
            "legacy_complete_report"
        ] = "allowed"
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "Additional properties are not allowed",
        ):
            materialization.validate_schema(
                contract,
                contract_schema,
                "materialization contract",
                error_code="materialization.report-invalid",
            )

        contract = copy.deepcopy(accepted)
        contract["surface"]["resource_limits"]["report_construction"] = (
            "bounded-spool-before-publication"
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "is not of type 'object'",
        ):
            materialization.validate_schema(
                contract,
                contract_schema,
                "materialization contract",
                error_code="materialization.report-invalid",
            )

    def test_authority_text_rejects_legacy_complete_report_before_publication(self) -> None:
        design_text = (ROOT / materialization.INTEGRATED_DESIGN).read_text(
            encoding="utf-8"
        )
        adr_text = (ROOT / materialization.DECISION_ADR).read_text(encoding="utf-8")
        contract_text = (ROOT / materialization.CONTRACT).read_text(encoding="utf-8")
        materialization.validate_report_lifecycle_authority_text(
            design_text,
            adr_text,
            contract_text,
        )

        for legacy in materialization.FORBIDDEN_REPORT_LIFECYCLE_TEXT:
            for source in ("design", "adr", "contract"):
                with self.subTest(legacy=legacy, source=source):
                    texts = {
                        "design": design_text,
                        "adr": adr_text,
                        "contract": contract_text,
                    }
                    texts[source] += "\n" + legacy
                    with self.assertRaisesRegex(
                        materialization.MaterializationError,
                        "legacy report lifecycle text was reintroduced",
                    ):
                        materialization.validate_report_lifecycle_authority_text(
                            texts["design"],
                            texts["adr"],
                            texts["contract"],
                        )

        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "ADR two-phase report lifecycle marker is missing: DF-0194",
        ):
            materialization.validate_report_lifecycle_authority_text(
                design_text,
                adr_text.replace("DF-0194", "DF-missing"),
                contract_text,
            )

        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "integrated design two-phase report lifecycle marker is missing: DF-0194",
        ):
            materialization.validate_report_lifecycle_authority_text(
                design_text.replace("DF-0194", "DF-missing"),
                adr_text,
                contract_text,
            )


if __name__ == "__main__":
    unittest.main()
