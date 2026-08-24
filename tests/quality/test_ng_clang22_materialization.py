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
import check_ng_source_closure_transport as closure_transport  # noqa: E402


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

    def test_materializer_semantics_uses_product_source_identity(self) -> None:
        request = self.request()
        direct_basis = materialization.expected_direct_basis(request)
        tool = request["tool"]
        expected = materialization.semantic_digest(
            "cxxlens.clang22-materializer-semantics.v1",
            materialization._canonical_tuple(
                materialization._canonical_string(tool[field])
                for field in (
                    "executable",
                    "interface_version",
                    "distribution_version",
                    "source_revision",
                    "source_tree",
                )
            ),
        )
        self.assertEqual(direct_basis["materializer_semantics_digest"], expected)

        physical_occurrence = copy.deepcopy(request)
        physical_occurrence["tool"]["installed_executable_digest"] = (
            "sha256:" + "9" * 64
        )
        physical_occurrence["tool"]["occurrence_manifest_digest"] = (
            "sha256:" + "8" * 64
        )
        physical_occurrence["tool"]["package_configuration"] = "shared"
        self.assertEqual(
            materialization.expected_direct_basis(physical_occurrence)[
                "materializer_semantics_digest"
            ],
            expected,
        )

        different_source = copy.deepcopy(request)
        different_source["tool"]["source_tree"] = "f" * 40
        self.assertNotEqual(
            materialization.expected_direct_basis(different_source)[
                "materializer_semantics_digest"
            ],
            expected,
        )

    def test_installed_ingress_contract_declares_all_process_binding_authority(self) -> None:
        schema_paths = (
            ROOT / materialization.REQUEST_SCHEMA,
            ROOT / "schemas" / "cxxlens_ng_clang22_materialization_request_v2_2.schema.yaml",
        )
        expected_environment = {
            "session_id": "CXXLENS_PROVIDER_SOURCE_CLOSURE_SESSION_ID",
            "task_id": "CXXLENS_PROVIDER_SOURCE_CLOSURE_TASK_ID",
            "task_v4_digest": "CXXLENS_PROVIDER_SOURCE_CLOSURE_TASK_V4_DIGEST",
            "source_closure_id": "CXXLENS_PROVIDER_SOURCE_CLOSURE_ID",
            "source_closure_digest": "CXXLENS_PROVIDER_SOURCE_CLOSURE_DIGEST",
            "manifest_digest": "CXXLENS_PROVIDER_SOURCE_CLOSURE_MANIFEST_DIGEST",
            "transfer_digest": "CXXLENS_PROVIDER_SOURCE_CLOSURE_TRANSFER_DIGEST",
            "stream_id": "CXXLENS_PROVIDER_SOURCE_CLOSURE_STREAM_ID",
            "first_sequence": "CXXLENS_PROVIDER_SOURCE_CLOSURE_FIRST_SEQUENCE",
            "binding_digest": "CXXLENS_PROVIDER_SOURCE_CLOSURE_BINDING_DIGEST",
        }
        for schema_path in schema_paths:
            document = yaml.safe_load(schema_path.read_text(encoding="utf-8"))
            ingress = document["x-cxxlens-installed-ingress-contract"]
            self.assertEqual(
                ingress["mode_environment"], "CXXLENS_PROVIDER_INGRESS_MODE"
            )
            self.assertEqual(ingress["mode"], "task-v4-source-closure-v2")
            self.assertEqual(ingress["fd_roles"]["read"]["environment"],
                             "CXXLENS_PROVIDER_SOURCE_CLOSURE_READ_FD")
            self.assertEqual(ingress["fd_roles"]["write"]["environment"],
                             "CXXLENS_PROVIDER_SOURCE_CLOSURE_WRITE_FD")
            self.assertEqual(ingress["binding_environment"], expected_environment)
            self.assertEqual(
                ingress["binding_environment_encoding"]["stream_id"],
                "uint64-canonical-decimal",
            )
            self.assertEqual(
                ingress["binding_environment_encoding"]["first_sequence"],
                {"encoding": "uint64-canonical-decimal", "exact": 0},
            )
            expected_role_encoding = {
                "session_id": "provider-session-id",
                "task_id": "task-id",
                "task_v4_digest": "semantic-v2-digest",
                "source_closure_id": "source-closure-id",
                "source_closure_digest": "semantic-v2-digest",
                "manifest_digest": "semantic-v2-digest",
                "transfer_digest": "semantic-v2-digest",
                "stream_id": "uint64-canonical-decimal",
                "first_sequence": "uint64-canonical-decimal",
            }
            binding_environment_encoding = ingress["binding_environment_encoding"]
            self.assertEqual(
                set(binding_environment_encoding) - {"binding_digest"},
                set(expected_role_encoding),
            )
            self.assertEqual(
                {
                    role: (
                        binding_environment_encoding[role]["encoding"]
                        if isinstance(binding_environment_encoding[role], dict)
                        else binding_environment_encoding[role]
                    )
                    for role in expected_role_encoding
                },
                expected_role_encoding,
            )
            binding_encoding = ingress["binding_environment_encoding"]["binding_digest"]
            self.assertEqual(binding_encoding["type"], "process-channel-sha256")
            self.assertEqual(binding_encoding["prefix"], "process-channel:sha256:")
            self.assertEqual(binding_encoding["hash"], "sha256")
            self.assertEqual(binding_encoding["identity_domain"], "process-channel")
            self.assertEqual(
                binding_encoding["projection_encoding"], "cxxlens-canonical-tuple-v1"
            )
            self.assertEqual(
                [
                    (
                        field["name"],
                        field["kind"],
                        field.get("encoding"),
                        field.get("exact"),
                    )
                    for field in binding_encoding["fields"]
                ],
                [
                    ("mode", "utf8_string", None, "task-v4-source-closure-v2"),
                    ("task_id", "utf8_string", "task-id", None),
                    ("session_id", "utf8_string", "provider-session-id", None),
                    ("task_v4_digest", "utf8_string", "semantic-v2-digest", None),
                    ("source_closure_id", "utf8_string", "source-closure-id", None),
                    ("source_closure_digest", "utf8_string", "semantic-v2-digest", None),
                    ("manifest_digest", "utf8_string", "semantic-v2-digest", None),
                    ("transfer_digest", "utf8_string", "semantic-v2-digest", None),
                    ("stream_id", "utf8_string", "uint64-canonical-decimal", None),
                    ("first_sequence", "utf8_string", "uint64-canonical-decimal", None),
                    ("read_descriptor", "canonical_integer", None, None),
                    ("write_descriptor", "canonical_integer", None, None),
                    ("read_device", "utf8_string", "uint64-canonical-decimal", None),
                    ("read_inode", "utf8_string", "uint64-canonical-decimal", None),
                    ("read_mode", "utf8_string", "uint32-canonical-decimal", None),
                    ("write_device", "utf8_string", "uint64-canonical-decimal", None),
                    ("write_inode", "utf8_string", "uint64-canonical-decimal", None),
                    ("write_mode", "utf8_string", "uint32-canonical-decimal", None),
                ],
            )
            projection_names = [field["name"] for field in binding_encoding["fields"]]
            expected_projection_names = [
                "mode",
                "task_id",
                "session_id",
                "task_v4_digest",
                "source_closure_id",
                "source_closure_digest",
                "manifest_digest",
                "transfer_digest",
                "stream_id",
                "first_sequence",
                "read_descriptor",
                "write_descriptor",
                "read_device",
                "read_inode",
                "read_mode",
                "write_device",
                "write_inode",
                "write_mode",
            ]
            self.assertEqual(projection_names, expected_projection_names)
            self.assertEqual(len(projection_names), len(set(projection_names)))
            self.assertEqual(
                set(projection_names) & (set(expected_environment) - {"binding_digest"}),
                set(expected_environment) - {"binding_digest"},
            )

            def assert_projection_order(document: dict) -> None:
                fields = document["x-cxxlens-installed-ingress-contract"][
                    "binding_environment_encoding"
                ]["binding_digest"]["fields"]
                names = [field["name"] for field in fields]
                if names != expected_projection_names or len(names) != len(set(names)):
                    raise AssertionError("process-channel projection is not exact")

            assert_projection_order(document)
            missing = copy.deepcopy(document)
            del missing["x-cxxlens-installed-ingress-contract"][
                "binding_environment_encoding"
            ]["binding_digest"]["fields"][0]
            with self.assertRaises(AssertionError):
                assert_projection_order(missing)
            extra = copy.deepcopy(document)
            extra["x-cxxlens-installed-ingress-contract"][
                "binding_environment_encoding"
            ]["binding_digest"]["fields"].append({"name": "unexpected"})
            with self.assertRaises(AssertionError):
                assert_projection_order(extra)
            duplicate = copy.deepcopy(document)
            duplicate["x-cxxlens-installed-ingress-contract"][
                "binding_environment_encoding"
            ]["binding_digest"]["fields"].append(
                duplicate["x-cxxlens-installed-ingress-contract"][
                    "binding_environment_encoding"
                ]["binding_digest"]["fields"][0]
            )
            with self.assertRaises(AssertionError):
                assert_projection_order(duplicate)
            reordered = copy.deepcopy(document)
            fields = reordered["x-cxxlens-installed-ingress-contract"][
                "binding_environment_encoding"
            ]["binding_digest"]["fields"]
            fields[0], fields[1] = fields[1], fields[0]
            with self.assertRaises(AssertionError):
                assert_projection_order(reordered)
            self.assertEqual(
                binding_encoding["endpoint_identity"],
                {
                    "read": {
                        "descriptor_environment": "CXXLENS_PROVIDER_SOURCE_CLOSURE_READ_FD",
                        "fstat_fields": ["read_device", "read_inode", "read_mode"],
                    },
                    "write": {
                        "descriptor_environment": "CXXLENS_PROVIDER_SOURCE_CLOSURE_WRITE_FD",
                        "fstat_fields": ["write_device", "write_inode", "write_mode"],
                    },
                },
            )
            self.assertEqual(ingress["fallback"], "forbidden")
            self.assertEqual(ingress["caller_override"], "forbidden")
            self.assertEqual(
                ingress["id_binding"],
                "exact-value-cross-check; substring-reconstruction-forbidden",
            )
            self.assertEqual(
                document["x-cxxlens-identity-domains"],
                {
                    "inherited_request_v2": "cxxlens.clang22-materialization-request.v2",
                    "base_claim_row_v1": "cxxlens.base-claim-row.v1",
                },
            )

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

    def test_request_bound_compact_report_validates_v2_2_task_and_closure_identity(
        self,
    ) -> None:
        request, _manifest = closure_transport.complete_request_witness(ROOT)
        request_bytes = materialization.canonical_json(request)
        report = materialization.compact_failure_report(
            request_bytes,
            request=request,
            phase="worker-launch",
            code="materialization.worker-failure",
        )
        materialization.validate_report(
            ROOT,
            request,
            report,
            request_bytes=request_bytes,
        )

        forged = copy.deepcopy(request)
        forged["task_extensions"][0]["source_closure"]["manifest_digest"] = (
            "semantic-v2:sha256:" + "0" * 64
        )
        forged_report = materialization.compact_failure_report(
            materialization.canonical_json(forged),
            request=forged,
            phase="worker-launch",
            code="materialization.worker-failure",
        )
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_report(
                ROOT,
                forged,
                forged_report,
                request_bytes=materialization.canonical_json(forged),
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
        def reseal(value: dict) -> None:
            payload = {
                key: copy.deepcopy(item)
                for key, item in value.items()
                if key != "occurrence_payload_digest"
            }
            value["occurrence_payload_digest"] = materialization.content_digest(
                materialization.canonical_json(payload)
            )

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
        reseal(changed_order)
        materialization.validate_occurrence_manifest(ROOT, changed_order)

        additional_artifact = copy.deepcopy(manifest)
        additional_artifact["files"].append(
            {
                "role": "supplemental-contract",
                "path": "share/cxxlens/schemas/supplemental-contract.yaml",
                "digest": "sha256:" + "3" * 64,
            }
        )
        reseal(additional_artifact)
        materialization.validate_occurrence_manifest(ROOT, additional_artifact)

        duplicate_role = copy.deepcopy(additional_artifact)
        duplicate_role["files"].append(
            {
                "role": "supplemental-contract",
                "path": "share/cxxlens/schemas/another-contract.yaml",
                "digest": "sha256:" + "4" * 64,
            }
        )
        reseal(duplicate_role)
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_occurrence_manifest(ROOT, duplicate_role)

        duplicate_path = copy.deepcopy(additional_artifact)
        duplicate_path["files"].append(
            {
                "role": "another-contract",
                "path": "share/cxxlens/schemas/supplemental-contract.yaml",
                "digest": "sha256:" + "4" * 64,
            }
        )
        reseal(duplicate_path)
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_occurrence_manifest(ROOT, duplicate_path)

        missing_required = copy.deepcopy(manifest)
        missing_required["files"] = [
            row
            for row in missing_required["files"]
            if row["role"] != "provider-protocol"
        ]
        reseal(missing_required)
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_occurrence_manifest(ROOT, missing_required)

        static_runtime = copy.deepcopy(manifest)
        static_runtime["files"].append(
            {
                "role": "supplemental-runtime",
                "path": "lib/libcxxlens_supplemental.so.1",
                "digest": "sha256:" + "5" * 64,
            }
        )
        reseal(static_runtime)
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_occurrence_manifest(ROOT, static_runtime)

        shared = materialization.fixture_occurrence_manifest(
            ROOT,
            source_revision="a" * 40,
            source_tree="b" * 40,
            configuration="shared",
            tool_digest="sha256:" + "1" * 64,
            worker_digest="sha256:" + "2" * 64,
        )
        materialization.validate_occurrence_manifest(ROOT, shared)
        shared["files"] = [row for row in shared["files"] if row["role"] != "query"]
        reseal(shared)
        with self.assertRaises(materialization.MaterializationError):
            materialization.validate_occurrence_manifest(ROOT, shared)

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
