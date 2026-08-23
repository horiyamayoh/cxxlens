#!/usr/bin/env python3
"""Focused product tests for the Protocol 2/request 2.2 materializer boundary."""

from __future__ import annotations

import copy
import hashlib
import pathlib
import sys
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_clang22_materialization as materialization  # noqa: E402
import check_ng_provider_protocol as protocol  # noqa: E402


class MaterializationProtocol2Tests(unittest.TestCase):
    @staticmethod
    def request(
        *,
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

    def test_request_schema_is_v2_2_and_does_not_embed_source_bytes(self) -> None:
        request = yaml.safe_load(
            (ROOT / materialization.REQUEST_SCHEMA).read_text(encoding="utf-8")
        )
        self.assertEqual(
            request["properties"]["request_version"]["const"], "2.2.0"
        )
        request_text = (ROOT / materialization.REQUEST_SCHEMA).read_text(
            encoding="utf-8"
        )
        self.assertNotIn("content_base64", request_text)
        worker = request["properties"]["worker"]["properties"]
        self.assertEqual(worker["protocol_major"]["const"], 2)
        self.assertEqual(worker["protocol_minor"]["const"], 0)

    def test_task_v4_schema_has_metadata_only_source_binding(self) -> None:
        task = yaml.safe_load(
            (ROOT / materialization.TASK_V4_SCHEMA).read_text(encoding="utf-8")
        )
        self.assertEqual(
            task["properties"]["schema"]["const"], "cxxlens.clang22.task.v4"
        )
        self.assertIn("base_task_digest", task["required"])
        self.assertEqual(
            {
                field
                for field in task["required"]
                if field.startswith("base_task_") and field.endswith("_digest")
            },
            {"base_task_digest"},
        )
        self.assertNotIn(
            "content_base64",
            (ROOT / materialization.TASK_V4_SCHEMA).read_text(encoding="utf-8"),
        )

    def test_materializer_checker_rejects_protocol_downgrade(self) -> None:
        protocol_contract = yaml.safe_load(
            (ROOT / materialization.PROVIDER_PROTOCOL).read_text(encoding="utf-8")
        )
        changed = copy.deepcopy(protocol_contract)
        changed["compatibility"]["accepted_minor"] = 1
        with self.assertRaises(protocol.ProviderContractError):
            protocol.validate_task_input_authority(changed)
        result = materialization.validate_v2_2_documents(ROOT)
        self.assertEqual(result["versioning"]["request"], "2.2.0")
        self.assertNotEqual(
            changed["compatibility"]["accepted_minor"],
            protocol_contract["compatibility"]["accepted_minor"],
        )

    def test_task_v4_projection_has_metadata_only_source_binding(self) -> None:
        task = {
            "schema": "cxxlens.clang22.task.v4",
            "task_id": "task:semantic-v2:sha256:" + "a" * 64,
            "selected_catalog_compile_unit_id": "catalog-compile-unit:one",
            "compile_unit_id": "compile-unit:one",
            "source": {
                "source_snapshot_id": "snapshot:one",
                "file_id": "file:one",
                "logical_path": "project://src/main.cc",
                "content_digest": "sha256:" + "b" * 64,
                "size_bytes": 1,
                "encoding": "utf8",
                "line_index_id": "line-index:one",
                "read_only": True,
            },
        }
        request = {
            "project": {
                "catalog_id": "catalog:one",
                "catalog_digest": "semantic-v2:sha256:" + "c" * 64,
                "logical_root": "project://",
                "catalog_environment_digest": "sha256:" + "d" * 64,
                "catalog_compile_units": [],
            },
            "tasks": [task],
        }
        projection = materialization.worker_task_v4_projection(request, task)
        self.assertNotIn(b"content_base64", projection)
        self.assertIn(b"cxxlens.clang22.task.v4", projection)
        legacy = copy.deepcopy(request)
        legacy["tasks"][0]["source"]["content_base64"] = "YQ=="
        with self.assertRaises(materialization.MaterializationError):
            materialization.worker_task_v4_projection(
                legacy, legacy["tasks"][0]
            )

    def test_protocol2_contract_preserves_wire_and_content_integrity(self) -> None:
        contract = protocol.load_yaml(ROOT / protocol.CONTRACT)
        protocol.validate_contract_shape(contract)
        self.assertEqual(contract["wire"]["fixed_header_bytes"], protocol.FRAME.size)
        self.assertEqual(contract["wire"]["checksums"], "independent-full-sha256")
        # Implementation bytes are not part of the protocol authority.  The
        # product contract retains only the fixed wire framing and payload
        # integrity checks above.
        self.assertNotIn("implementation_byte_sha256", contract["compatibility"])
        self.assertNotIn("implementation_byte_identity", contract["compatibility"])

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
            "duplicate": b'{"value":1,"value":2}',
            "invalid-utf8": b'{"value":"\xff"}',
            "second-value": b'{} {}',
            "trailing-garbage": b'{}x',
            "bom": b'\xef\xbb\xbf{}',
            "non-finite": b'{"value":NaN}',
            "fractional": b'{"value":1.5}',
            "integer-overflow": b'{"value":18446744073709551616}',
            "invalid-unicode-scalar": b'{"value":"\\ud800"}',
        }
        for label, raw in invalid.items():
            with self.subTest(label=label):
                with self.assertRaises(materialization.MaterializationError) as caught:
                    materialization.load_strict_json_bytes(raw, "request")
                self.assertEqual(caught.exception.code, "materialization.request-invalid")

    def test_raw_input_observation_binds_exact_transport_bytes(self) -> None:
        request_bytes = b" \n" + materialization.canonical_json(self.request()) + b"\n\t"
        observation = materialization.raw_input_observation(request_bytes)
        materialization.validate_raw_input_observation(observation, request_bytes)

        changed = copy.deepcopy(observation)
        changed["observed_prefix_digest"] = "sha256:" + "0" * 64
        with self.assertRaisesRegex(
            materialization.MaterializationError,
            "raw input observation differs from exact transport bytes",
        ):
            materialization.validate_raw_input_observation(changed, request_bytes)

        original_limit = materialization.RAW_INPUT_BYTE_LIMIT
        try:
            materialization.RAW_INPUT_BYTE_LIMIT = 4
            bounded = materialization.raw_input_observation(b"abcdef")
        finally:
            materialization.RAW_INPUT_BYTE_LIMIT = original_limit
        self.assertEqual(bounded["observed_size_bytes"], 5)
        self.assertEqual(
            bounded["observed_prefix_digest"],
            materialization.content_digest(b"abcde"),
        )
        self.assertFalse(bounded["complete"])

    def test_compact_failure_preserves_zero_effect_boundary(self) -> None:
        request = self.request()
        request_bytes = materialization.canonical_json(request)
        report = materialization.compact_failure_report(
            request_bytes,
            request=request,
            phase="worker-launch",
            code="materialization.worker-failure",
        )
        materialization.validate_schema(
            report,
            materialization.load(ROOT / materialization.REPORT_SCHEMA),
            "materialization report",
            error_code="materialization.report-invalid",
        )
        materialization.validate_compact_store_failure_cause(
            report["error"], report["effects"], None
        )
        self.assertEqual(report["binding"]["state"], "request-bound")
        self.assertFalse(report["effects"]["publication_attempted"])
        self.assertEqual(report["effects"]["committed_transaction_count"], 0)
        self.assertTrue(report["effects"]["prior_history_retained"])

        forged = copy.deepcopy(report)
        forged["effects"]["store_failure_cause"] = {
            "kind": "sdk_error",
            "operation": "store_open",
        }
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_compact_store_failure_cause(
                forged["error"], forged["effects"], None
            )

    def test_observation_v2_native_codec_binds_payload_and_span(self) -> None:
        request = self.request()
        task = request["tasks"][0]
        source = task["source"]
        span = {
            "span_id": materialization.source_span_identity(
                source["source_snapshot_id"], source["file_id"], 0, 3, "declaration"
            ),
            "snapshot": source["source_snapshot_id"],
            "file": source["file_id"],
            "begin": 0,
            "end": 3,
            "role": "declaration",
            "read_only": source["read_only"],
        }
        record = {
            "kind": "entity",
            "final_relation_compile_unit_id": task["compile_unit_id"],
            "semantic_key": "clang-usr:c:@F@f#",
            "payload": {
                "symbol.kind": "function",
                "symbol.qualified_name": "f",
            },
            "primary_span": span,
            "origin_chain": [],
            "exact_equivalence": True,
            "limitation": None,
        }
        descriptor, row = materialization.observation_v2_native_row(
            ROOT, record, task
        )
        self.assertEqual(descriptor, "frontend.clang22.entity_observation.v2")
        self.assertEqual(row["semantic_key"], b"clang-usr:c:@F@f#")
        self.assertEqual(row["source"], span["span_id"])

        reordered = copy.deepcopy(record)
        reordered["payload"] = {
            "symbol.qualified_name": "f",
            "symbol.kind": "function",
        }
        _, reordered_row = materialization.observation_v2_native_row(
            ROOT, reordered, task
        )
        self.assertEqual(reordered_row, row)

        changed = copy.deepcopy(record)
        changed["payload"]["symbol.qualified_name"] = "g"
        _, changed_row = materialization.observation_v2_native_row(
            ROOT, changed, task
        )
        self.assertNotEqual(changed_row["payload_digest"], row["payload_digest"])

        wrong_task = copy.deepcopy(record)
        wrong_task["final_relation_compile_unit_id"] = "compile-unit:wrong"
        with self.assertRaises(materialization.MaterializationError):
            materialization.observation_v2_native_row(ROOT, wrong_task, task)

    def test_primary_span_bundle_is_all_or_none_and_identity_bound(self) -> None:
        request = self.request()
        source = request["tasks"][0]["source"]
        bundle = {
            "span_id": materialization.source_span_identity(
                source["source_snapshot_id"], source["file_id"], 0, 3, "expression"
            ),
            "snapshot": source["source_snapshot_id"],
            "file": source["file_id"],
            "begin": 0,
            "end": 3,
            "role": "expression",
            "read_only": source["read_only"],
        }
        self.assertEqual(
            materialization.validate_primary_span_bundle(bundle, source), "present"
        )
        self.assertEqual(
            materialization.source_span_base_row(bundle, source)["span"],
            bundle["span_id"],
        )
        for mutation in (
            lambda value: value.pop("read_only"),
            lambda value: value.update({"end": source["size_bytes"] + 1}),
            lambda value: value.update({"span_id": "source-span:forged"}),
        ):
            changed = copy.deepcopy(bundle)
            mutation(changed)
            with self.assertRaisesRegex(
                materialization.MaterializationError, "materialization.span-invalid"
            ):
                materialization.validate_primary_span_bundle(changed, source)

    def test_occurrence_manifest_is_closed_and_digest_bound(self) -> None:
        manifest = materialization.fixture_occurrence_manifest(
            ROOT,
            source_revision="a" * 40,
            source_tree="b" * 40,
            configuration="static",
            tool_digest="sha256:" + "1" * 64,
            worker_digest="sha256:" + "2" * 64,
        )
        materialization.validate_occurrence_manifest(ROOT, manifest)

        changed_digest = copy.deepcopy(manifest)
        changed_digest["occurrence_payload_digest"] = "sha256:" + "0" * 64
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_occurrence_manifest(ROOT, changed_digest)

        changed_order = copy.deepcopy(manifest)
        changed_order["files"][2], changed_order["files"][3] = (
            changed_order["files"][3],
            changed_order["files"][2],
        )
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_occurrence_manifest(ROOT, changed_order)

    def test_task_v4_resource_proof_is_schema_derived_and_deterministic(self) -> None:
        shared_schema_path = ROOT / "schemas/cxxlens_ng_clang22_materialization_request.schema.yaml"
        request_schema = yaml.safe_load(shared_schema_path.read_text(encoding="utf-8"))
        proof = materialization.maximum_worker_task_v4_projection_proof(request_schema)
        saturated = bytearray()
        streamed = materialization.stream_worker_task_v4_saturated_vector(
            request_schema, saturated.extend
        )
        self.assertEqual(streamed, proof["maximum_projection_bytes"])
        self.assertEqual(streamed, len(saturated))
        self.assertEqual(
            proof["saturated_vector"]["digest"],
            "sha256:" + hashlib.sha256(saturated).hexdigest(),
        )
        with self.assertRaisesRegex(
            materialization.MaterializationError, "exceeds transfer limit"
        ):
            materialization.maximum_worker_task_v4_projection_proof(
                request_schema,
                transfer_limit=proof["maximum_projection_bytes"] - 1,
            )

    def test_report_fixture_preserves_provenance_coverage_store_determinism(self) -> None:
        for backend in ("memory", "sqlite"):
            with self.subTest(backend=backend):
                request = self.request(backend=backend)
                request_bytes = materialization.canonical_json(request)
                first = materialization.sample_report(
                    ROOT, request, request_bytes=request_bytes
                )
                second = materialization.sample_report(
                    ROOT, request, request_bytes=request_bytes
                )
                self.assertEqual(first, second)
                self.assertIn("coverage", first["side_channels"])
                self.assertIn("transport_coverage", first["side_channels"])
                self.assertIn("guarantee", first["side_channels"])
                self.assertIn("provenance", first)
                self.assertIn("store", first)
                self.assertEqual(first["provenance"]["orphan_count"], 0)
                self.assertEqual(first["provenance"]["ambiguous_count"], 0)
                self.assertEqual(first["side_channels"]["unresolved"]["blocking_count"], 0)
                self.assertEqual(first["publication"]["backend"], backend)

    def test_observation_equivalence_census_rejects_typed_semantic_drift(self) -> None:
        request = self.request()
        report = materialization.sample_report(
            ROOT, request, request_bytes=materialization.canonical_json(request)
        )
        batch = next(
            batch
            for batch in report["task_results"][0]["batches"]
            if batch["descriptor_id"] == "frontend.clang22.entity_observation.v2"
        )
        materialization.validate_observation_equivalence_census(
            batch["descriptor_id"],
            batch["observation_equivalence_census"],
            batch["row_bindings"],
        )
        changed = copy.deepcopy(batch["observation_equivalence_census"])
        changed["rows"][0]["limitation"] = "forged limitation"
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_observation_equivalence_census(
                batch["descriptor_id"], changed, batch["row_bindings"]
            )


if __name__ == "__main__":
    unittest.main()
