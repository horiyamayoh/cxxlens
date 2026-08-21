#!/usr/bin/env python3
"""Positive and negative tests for dedicated source-closure transport."""

from __future__ import annotations

import hashlib
import pathlib
import shutil
import sys
import tempfile
import unittest

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_source_closure_transport import (  # noqa: E402
    ADR,
    CONTRACT,
    LEGACY_BINDINGS,
    PROTOCOL,
    PROTOCOL_SCHEMA,
    REQUEST,
    SCHEMA,
    TASK,
    MANIFEST_SCHEMA,
    SourceClosureTransportError,
    TransferStateWitness,
    blob_receipts_digest,
    canonical_json,
    cbor_encode,
    closure_digest,
    complete_request_witness,
    content_projection_digest,
    manifest_digest,
    request_v2_2_projection,
    semantic_digest,
    source_closure_file_id,
    task_v4_projection,
    trust_policy_digest,
    validate,
    validate_manifest,
    validate_reject_control,
    validate_wire_control,
    validate_request_binding,
    transfer_digest,
)
from check_ng_provider_protocol import encode_frame  # noqa: E402

SEMANTIC = "semantic-v2:sha256:" + "1" * 64
SESSION_ID = "provider-session:sha256:" + "2" * 64
TASK_ID = "task:" + SEMANTIC
CLEANUP_RECEIPT = "cleanup-receipt:" + SEMANTIC


class SourceClosureTransportTest(unittest.TestCase):
    @staticmethod
    def bound_request() -> dict:
        semantic = "semantic-v2:sha256:" + "1" * 64
        content = "sha256:" + "2" * 64
        base_task_id = "task:semantic-v2:sha256:" + "3" * 64
        closure_id = "source-closure:" + semantic
        base = {
            "provider_task_id": base_task_id,
            "task_input_digest": content,
            "normalized_invocation_digest": "semantic-v2:sha256:" + "4" * 64,
            "toolchain_digest": "semantic-v2:sha256:" + "5" * 64,
            "environment_digest": "sha256:" + "6" * 64,
            "working_directory": "project://src",
            "source": {
                "file_id": source_closure_file_id("project://src/main.cpp"),
                "logical_path": "project://src/main.cpp",
                "content_digest": content,
                "size_bytes": 1,
                "encoding": "utf8",
                "read_only": True,
            },
            "sandbox": {"minimum": "enforced", "policy_digest": "sha256:" + "8" * 64},
        }
        closure = {
            "source_closure_id": closure_id,
            "source_closure_digest": semantic,
            "manifest_digest": "semantic-v2:sha256:" + "7" * 64,
        }
        extension = {
            "schema": "cxxlens.clang22.task.v4",
            "base_task_index": 0,
            "base_provider_task_id": base_task_id,
            "base_task_v3_digest": content_projection_digest(base),
            "open_task": {
                field: base[field]
                for field in (
                    "task_input_digest",
                    "normalized_invocation_digest",
                    "toolchain_digest",
                    "environment_digest",
                )
            },
            "source_closure": {
                "id": closure_id,
                "digest": semantic,
                "manifest_digest": closure["manifest_digest"],
            },
            "main_logical_path": "project://src/main.cpp",
            "logical_working_directory": "project://src",
        }
        extension["task_v4_digest"] = semantic_digest(
            "cxxlens.clang22.task.v4", task_v4_projection(extension)
        )
        extension["task_id"] = "task:" + extension["task_v4_digest"]
        worker = {"provider_id": "cxxlens.clang22.reference", "provider_version": "1.0.0", "semantic_contract_digest": "sha256:" + "9" * 64, "protocol_major": 1, "protocol_minor": 2, "required_features": ["task-input-chunks-v1", "task-source-closure-v1"], "sandbox_policy_digest": "sha256:" + "8" * 64}
        trust = {"policy_id": "cxxlens.clang22-installed-native-worker-trust.v1", "execution_profile": "trust.native-worker", "provider_id": worker["provider_id"], "provider_version": worker["provider_version"], "semantic_contract_digest": worker["semantic_contract_digest"], "protocol_major": 1, "protocol_minor": 2, "required_features": list(worker["required_features"]), "required_qualification": "canonical-semantic-qualified", "worker_sandbox_policy_digest": worker["sandbox_policy_digest"], "task_sandbox_requirements": [base["sandbox"]], "trust_policy_digest": "pending"}
        trust["trust_policy_digest"] = trust_policy_digest(trust)
        request = {
            "schema": "cxxlens.clang22-materialization-request.v2_2",
            "request_version": "2.2.0",
            "required_features": ["task-input-chunks-v1", "task-source-closure-v1"],
            "materialization_request_id": "id:base",
            "semantic_request_digest": semantic,
            "tool": {}, "worker": worker, "project": {}, "registry": {}, "engine": {},
            "interpretation_policy": {}, "trust_policy": trust, "group_topology": {},
            "tasks": [base], "publication": {},
            "source_closures": [closure],
            "task_extensions": [extension],
        }
        request["request_digest"] = semantic_digest(
            "cxxlens.clang22.materialization-request.v2_2",
            request_v2_2_projection(request),
        )
        request["request_id"] = "materialization-request:" + request["request_digest"]
        return request

    @staticmethod
    def bind_manifest(request: dict) -> dict:
        base = request["tasks"][0]
        source = base["source"]
        member = {
            "file_id": source["file_id"],
            "logical_path": source["logical_path"],
            "role": "main",
            "encoding": source["encoding"],
            "size_bytes": source["size_bytes"],
            "content_digest": source["content_digest"],
            "read_only": source["read_only"],
        }
        blob = {
            "content_digest": source["content_digest"],
            "size_bytes": source["size_bytes"],
        }
        digest = closure_digest([member], [blob])
        manifest = {
            "schema": "cxxlens.source-closure-manifest.v1",
            "closure_id": "source-closure:" + digest,
            "closure_digest": digest,
            "members": [member],
            "blobs": [blob],
        }
        closure = request["source_closures"][0]
        closure.update(
            {
                "source_closure_id": manifest["closure_id"],
                "source_closure_digest": digest,
                "manifest_digest": manifest_digest(manifest),
                "member_count": 1,
                "blob_count": 1,
                "unique_blob_bytes": source["size_bytes"],
            }
        )
        extension = request["task_extensions"][0]
        extension["base_task_v3_digest"] = content_projection_digest(base)
        extension["source_closure"] = {
            "id": closure["source_closure_id"],
            "digest": closure["source_closure_digest"],
            "manifest_digest": closure["manifest_digest"],
        }
        extension["main_logical_path"] = source["logical_path"]
        extension["logical_working_directory"] = base["working_directory"]
        extension["task_v4_digest"] = semantic_digest(
            "cxxlens.clang22.task.v4", task_v4_projection(extension)
        )
        extension["task_id"] = "task:" + extension["task_v4_digest"]
        request["request_digest"] = semantic_digest(
            "cxxlens.clang22.materialization-request.v2_2",
            request_v2_2_projection(request),
        )
        request["request_id"] = "materialization-request:" + request["request_digest"]
        return manifest

    @staticmethod
    def reseal_request(request: dict) -> None:
        for extension in request["task_extensions"]:
            extension["task_v4_digest"] = semantic_digest(
                "cxxlens.clang22.task.v4", task_v4_projection(extension)
            )
            extension["task_id"] = "task:" + extension["task_v4_digest"]
        request["request_digest"] = semantic_digest(
            "cxxlens.clang22.materialization-request.v2_2",
            request_v2_2_projection(request),
        )
        request["request_id"] = "materialization-request:" + request["request_digest"]

    def copied_root(self, temporary: str) -> pathlib.Path:
        root = pathlib.Path(temporary)
        legacy = tuple(
            pathlib.Path(path)
            for path in (
                "schemas/cxxlens_ng_clang22_materialization_request.schema.yaml",
                "schemas/cxxlens_ng_provider_protocol.schema.yaml",
                "src/llvm/clang22/provider_task_v3.hpp",
                "src/llvm/clang22/provider_task_v3.cpp",
                "src/llvm/clang22/materialization_request_v2_1.cpp",
            )
        )
        for relative in (
            ADR,
            CONTRACT,
            PROTOCOL,
            REQUEST,
            SCHEMA,
            TASK,
            MANIFEST_SCHEMA,
            *legacy,
        ):
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        return root

    @staticmethod
    def rewrite(root: pathlib.Path, relative: pathlib.Path, mutate) -> None:
        path = root / relative
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
        mutate(value)
        path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")

    def test_repository_contract_is_valid(self) -> None:
        validate(ROOT)

    def test_complete_v2_2_request_witness_is_schema_valid_and_bound(self) -> None:
        request, manifest = complete_request_witness(ROOT)
        self.assertEqual(request["request_version"], "2.2.0")
        self.assertNotIn("content_base64", request["tasks"][0]["source"])
        request_schema = yaml.safe_load(
            (ROOT / REQUEST).read_text(encoding="utf-8")
        )
        legacy_request_schema = yaml.safe_load(
            (ROOT / LEGACY_BINDINGS["request_schema_sha256"]).read_text(
                encoding="utf-8"
            )
        )
        task_schema = yaml.safe_load((ROOT / TASK).read_text(encoding="utf-8"))
        schema_store = {
            request_schema["$id"]: request_schema,
            legacy_request_schema["$id"]: legacy_request_schema,
            task_schema["$id"]: task_schema,
            "https://cxxlens.dev/schemas/cxxlens_ng_provider_task_v4.schema.yaml": task_schema,
        }
        resolver = jsonschema.RefResolver.from_schema(
            request_schema, store=schema_store
        )
        jsonschema.Draft202012Validator(
            request_schema, resolver=resolver
        ).validate(request)
        validate_manifest(
            manifest, yaml.safe_load((ROOT / MANIFEST_SCHEMA).read_text())
        )
        validate_request_binding(request, [manifest])

    def test_maximum_manifest_constructibility_witness_is_within_transport_bound(self) -> None:
        members = []
        blobs_by_digest = {}
        for index in range(4096):
            if index == 0:
                logical_path = "project://a-main.cpp"
                role = "main"
            else:
                suffix = f"{index:04d}.hpp"
                prefix = "project://z-generated/"
                logical_path = prefix + ('"' * (4096 - len(prefix) - len(suffix))) + suffix
                role = "header"
            payload = f"source-closure-boundary-{index}".encode("utf-8")
            content_digest = "sha256:" + hashlib.sha256(payload).hexdigest()
            member = {
                "file_id": source_closure_file_id(logical_path),
                "logical_path": logical_path,
                "role": role,
                "encoding": "utf8",
                "size_bytes": len(payload),
                "content_digest": content_digest,
                "read_only": True,
            }
            members.append(member)
            blobs_by_digest[content_digest] = {
                "content_digest": content_digest,
                "size_bytes": len(payload),
            }
        blobs = [blobs_by_digest[digest] for digest in sorted(blobs_by_digest)]
        manifest = {
            "schema": "cxxlens.source-closure-manifest.v1",
            "closure_digest": closure_digest(members, blobs),
            "members": members,
            "blobs": blobs,
        }
        manifest["closure_id"] = "source-closure:" + manifest["closure_digest"]
        manifest_bytes = canonical_json(manifest)
        self.assertGreater(len(manifest_bytes), 32 * 1024 * 1024)
        self.assertLessEqual(len(manifest_bytes), 40 * 1024 * 1024)
        self.assertLessEqual((len(manifest_bytes) + (1 << 20) - 1) // (1 << 20), 40)
        schema = yaml.safe_load((ROOT / MANIFEST_SCHEMA).read_text(encoding="utf-8"))
        validate_manifest(manifest, schema)

    def test_adr_0101_identity_and_request_binding_are_constructible(self) -> None:
        request = self.bound_request()
        manifest = self.bind_manifest(request)
        validate_request_binding(request, [manifest])
        task_schema = yaml.safe_load((ROOT / TASK).read_text(encoding="utf-8"))
        jsonschema.Draft202012Validator(task_schema).validate(request["task_extensions"][0])

    def test_task_v4_schema_rejects_empty_logical_path_segments(self) -> None:
        request = self.bound_request()
        task_schema = yaml.safe_load((ROOT / TASK).read_text(encoding="utf-8"))
        validator = jsonschema.Draft202012Validator(task_schema)
        for field in ("main_logical_path", "logical_working_directory"):
            with self.subTest(field=field):
                task = dict(request["task_extensions"][0])
                task[field] = "project://src//invalid"
                with self.assertRaises(jsonschema.ValidationError):
                    validator.validate(task)

    def test_manifest_schema_accepts_project_path_and_rejects_extra_field(self) -> None:
        schema = yaml.safe_load((ROOT / MANIFEST_SCHEMA).read_text(encoding="utf-8"))
        semantic = "semantic-v2:sha256:" + "1" * 64
        content = "sha256:" + "2" * 64
        manifest = {
            "schema": "cxxlens.source-closure-manifest.v1",
            "closure_id": "source-closure:" + semantic,
            "closure_digest": semantic,
            "members": [{
                "file_id": source_closure_file_id("project://src/main.cpp"),
                "logical_path": "project://src/main.cpp",
                "role": "main",
                "encoding": "utf8",
                "size_bytes": 1,
                "content_digest": content,
                "read_only": True,
            }],
            "blobs": [{"content_digest": content, "size_bytes": 1}],
        }
        jsonschema.Draft202012Validator(schema).validate(manifest)
        manifest["rogue"] = True
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.Draft202012Validator(schema).validate(manifest)

    def test_manifest_rejects_strict_path_and_file_id_counterexamples(self) -> None:
        schema = yaml.safe_load((ROOT / MANIFEST_SCHEMA).read_text(encoding="utf-8"))
        content = "sha256:" + "2" * 64
        blob = {"content_digest": content, "size_bytes": 1}

        wrong_file_id_member = {
            "file_id": "file:sha256:" + "3" * 64,
            "logical_path": "project://src/main.cpp",
            "role": "main",
            "encoding": "utf8",
            "size_bytes": 1,
            "content_digest": content,
            "read_only": True,
        }
        wrong_file_id_digest = closure_digest([wrong_file_id_member], [blob])
        wrong_file_id_manifest = {
            "schema": "cxxlens.source-closure-manifest.v1",
            "closure_id": "source-closure:" + wrong_file_id_digest,
            "closure_digest": wrong_file_id_digest,
            "members": [wrong_file_id_member],
            "blobs": [blob],
        }
        with self.assertRaisesRegex(SourceClosureTransportError, "file_id"):
            validate_manifest(wrong_file_id_manifest, schema)

        for logical_path, expected_message in (
            ("project://src//main.cpp", "schema invalid"),
            ("project://" + "é" * 2044, "UTF-8"),
        ):
            with self.subTest(logical_path=logical_path):
                member = {
                    **wrong_file_id_member,
                    "file_id": "file:sha256:" + "4" * 64,
                    "logical_path": logical_path,
                }
                digest = closure_digest([member], [blob])
                manifest = {
                    "schema": "cxxlens.source-closure-manifest.v1",
                    "closure_id": "source-closure:" + digest,
                    "closure_digest": digest,
                    "members": [member],
                    "blobs": [blob],
                }
                with self.assertRaisesRegex(SourceClosureTransportError, expected_message):
                    validate_manifest(manifest, schema)

    def test_manifest_file_id_derivation_matches_shared_identity_vector(self) -> None:
        self.assertEqual(
            source_closure_file_id("project://main.cpp"),
            "file:sha256:83e065cbf0d8f742fe73a01155b02057c0de0fbe747f88b35ea5e96efe8faf06",
        )

    def test_manifest_semantic_tamper_and_orphan_are_rejected(self) -> None:
        schema = yaml.safe_load((ROOT / MANIFEST_SCHEMA).read_text(encoding="utf-8"))
        content = "sha256:" + "2" * 64
        members = [{"file_id": source_closure_file_id("project://src/main.cpp"), "logical_path": "project://src/main.cpp", "role": "main", "encoding": "utf8", "size_bytes": 1, "content_digest": content, "read_only": True}]
        blobs = [{"content_digest": content, "size_bytes": 1}]
        semantic = closure_digest(members, blobs)
        manifest = {"schema": "cxxlens.source-closure-manifest.v1", "closure_id": "source-closure:" + semantic, "closure_digest": semantic, "members": members, "blobs": blobs}
        validate_manifest(manifest, schema)
        manifest["members"][0]["role"] = "header"
        with self.assertRaisesRegex(SourceClosureTransportError, "exactly one main"):
            validate_manifest(manifest, schema)
        manifest["members"][0]["role"] = "main"
        manifest["blobs"].append({"content_digest": "sha256:" + "4" * 64, "size_bytes": 1})
        with self.assertRaisesRegex(SourceClosureTransportError, "orphan"):
            validate_manifest(manifest, schema)

    def test_manifest_role_tamper_with_main_preserved_is_digest_rejected(self) -> None:
        schema = yaml.safe_load((ROOT / MANIFEST_SCHEMA).read_text(encoding="utf-8"))
        content = "sha256:" + "2" * 64
        members = [
            {"file_id": source_closure_file_id("project://src/main.cpp"), "logical_path": "project://src/main.cpp", "role": "main", "encoding": "utf8", "size_bytes": 1, "content_digest": content, "read_only": True},
            {"file_id": source_closure_file_id("project://src/z.hpp"), "logical_path": "project://src/z.hpp", "role": "header", "encoding": "utf8", "size_bytes": 1, "content_digest": content, "read_only": True},
        ]
        blobs = [{"content_digest": content, "size_bytes": 1}]
        digest = closure_digest(members, blobs)
        manifest = {"schema": "cxxlens.source-closure-manifest.v1", "closure_id": "source-closure:" + digest, "closure_digest": digest, "members": members, "blobs": blobs}
        validate_manifest(manifest, schema)
        manifest["members"][1]["role"] = "forced-include"
        with self.assertRaisesRegex(SourceClosureTransportError, "closure digest"):
            validate_manifest(manifest, schema)

    def test_reject_fields_and_phase_are_executable(self) -> None:
        contract = yaml.safe_load((ROOT / CONTRACT).read_text(encoding="utf-8"))
        control = {"session_id": SESSION_ID, "task_id": TASK_ID, "failure_phase": "before-manifest", "reason_code": "source-closure.protocol-state-invalid", "observed_counters": {"observed-control-frame-count": 1}, "cleanup_receipt": CLEANUP_RECEIPT}
        validate_reject_control(control, contract)
        control["observed_counters"] = {"received-payload-bytes": 1}
        with self.assertRaisesRegex(SourceClosureTransportError, "phase-authentic|bytes"):
            validate_reject_control(control, contract)
        control["observed_counters"] = {"observed-control-frame-count": True}
        with self.assertRaisesRegex(SourceClosureTransportError, "uint64"):
            validate_reject_control(control, contract)
        control["observed_counters"] = {"observed-control-frame-count": 1 << 80}
        with self.assertRaisesRegex(SourceClosureTransportError, "uint64"):
            validate_reject_control(control, contract)

    def test_local_only_failure_cannot_be_emitted_as_wire_reject(self) -> None:
        contract = yaml.safe_load((ROOT / CONTRACT).read_text(encoding="utf-8"))
        control = {
            "session_id": SESSION_ID,
            "task_id": TASK_ID,
            "failure_phase": "local-only",
            "reason_code": "source-closure.ambient-fallback-denied",
            "observed_counters": {},
            "cleanup_receipt": CLEANUP_RECEIPT,
        }
        with self.assertRaisesRegex(SourceClosureTransportError, "local-only"):
            validate_reject_control(control, contract)

    def test_nonreject_wire_controls_are_closed_typed_and_state_bound(self) -> None:
        contract = yaml.safe_load((ROOT / CONTRACT).read_text(encoding="utf-8"))
        semantic = "semantic-v2:sha256:" + "1" * 64
        control = {"kind": "chunk", "session_id": SESSION_ID, "task_id": TASK_ID, "manifest_digest": semantic, "chunk_index": 0, "offset": 0, "byte_count": 1}
        validate_wire_control("source_closure_manifest", control, b"x", "manifest-open", contract)
        with self.assertRaisesRegex(SourceClosureTransportError, "illegal"):
            validate_wire_control("source_closure_manifest", control, b"x", "task-v4-sealed", contract)
        control["byte_count"] = True
        with self.assertRaisesRegex(SourceClosureTransportError, "uint64"):
            validate_wire_control("source_closure_manifest", control, b"x", "manifest-open", contract)
        control["byte_count"] = 2
        with self.assertRaisesRegex(SourceClosureTransportError, "byte count"):
            validate_wire_control("source_closure_manifest", control, b"x", "manifest-open", contract)

    def test_wire_descriptor_bounds_ids_and_payload_types_are_rejected(self) -> None:
        contract = yaml.safe_load((ROOT / CONTRACT).read_text(encoding="utf-8"))
        semantic = "semantic-v2:sha256:" + "1" * 64
        content = "sha256:" + "2" * 64
        blob = {"session_id": SESSION_ID, "task_id": TASK_ID, "closure_digest": semantic, "blob_ordinal": 0, "blob_digest": content, "total_bytes": 16777217, "chunk_bytes": 1048576, "chunk_count": 17}
        with self.assertRaisesRegex(SourceClosureTransportError, "byte bound"):
            validate_wire_control("source_closure_blob", blob, b"", "manifest-validated", contract)
        blob.update({"total_bytes": 1, "chunk_bytes": 1, "chunk_count": 1})
        with self.assertRaisesRegex(SourceClosureTransportError, "byte string"):
            validate_wire_control("source_closure_blob", blob, "", "manifest-validated", contract)
        descriptor = {"kind": "descriptor", "session_id": SESSION_ID, "task_id": TASK_ID, "task_v4_digest": semantic, "closure_id": "x", "closure_digest": semantic, "manifest_digest": semantic, "total_bytes": 1, "chunk_bytes": 1, "chunk_count": 1}
        with self.assertRaisesRegex(SourceClosureTransportError, "source closure ID"):
            validate_wire_control("source_closure_manifest", descriptor, b"", "task-v4-sealed", contract)

    def test_zero_byte_blob_has_descriptor_only_wire_representation(self) -> None:
        contract = yaml.safe_load((ROOT / CONTRACT).read_text(encoding="utf-8"))
        semantic = "semantic-v2:sha256:" + "1" * 64
        content = "sha256:" + "2" * 64
        blob = {"session_id": SESSION_ID, "task_id": TASK_ID, "closure_digest": semantic, "blob_ordinal": 0, "blob_digest": content, "total_bytes": 0, "chunk_bytes": 1048576, "chunk_count": 0}
        validate_wire_control("source_closure_blob", blob, b"", "manifest-validated", contract)

    def test_trust_policy_digest_and_worker_parity_are_enforced(self) -> None:
        request = self.bound_request()
        manifest = self.bind_manifest(request)
        validate_request_binding(request, [manifest])
        request["trust_policy"]["protocol_minor"] = 1
        with self.assertRaisesRegex(SourceClosureTransportError, "parity"):
            validate_request_binding(request, [manifest])

    def test_duplicate_and_dangling_relationships_are_rejected(self) -> None:
        request = self.bound_request()
        manifest = self.bind_manifest(request)
        request["source_closures"].append(dict(request["source_closures"][0]))
        with self.assertRaisesRegex(SourceClosureTransportError, "duplicate"):
            validate_request_binding(request, [manifest])

    def test_request_and_task_semantic_identity_tamper_is_rejected(self) -> None:
        request = self.bound_request()
        manifest = self.bind_manifest(request)
        request["request_digest"] = "semantic-v2:sha256:" + "9" * 64
        with self.assertRaisesRegex(SourceClosureTransportError, "request v2.2"):
            validate_request_binding(request, [manifest])
        request = self.bound_request()
        manifest = self.bind_manifest(request)
        request["task_extensions"][0]["task_v4_digest"] = (
            "semantic-v2:sha256:" + "9" * 64
        )
        with self.assertRaisesRegex(SourceClosureTransportError, "task v4"):
            validate_request_binding(request, [manifest])
        request = self.bound_request()
        manifest = self.bind_manifest(request)
        request["task_extensions"][0]["source_closure"]["id"] = (
            "source-closure:semantic-v2:sha256:" + "9" * 64
        )
        with self.assertRaisesRegex(SourceClosureTransportError, "does not resolve"):
            validate_request_binding(request, [manifest])

    def test_traversal_and_utf8_byte_overflow_are_rejected(self) -> None:
        request = self.bound_request()
        manifest = self.bind_manifest(request)
        request["task_extensions"][0]["main_logical_path"] = (
            "project://src/../ambient.hpp"
        )
        with self.assertRaisesRegex(SourceClosureTransportError, "segments"):
            validate_request_binding(request, [manifest])
        request = self.bound_request()
        manifest = self.bind_manifest(request)
        request["task_extensions"][0]["main_logical_path"] = "project://" + "é" * 4090
        with self.assertRaisesRegex(SourceClosureTransportError, "UTF-8"):
            validate_request_binding(request, [manifest])

    def test_resealed_path_census_and_orphan_counterexamples_are_rejected(self) -> None:
        request = self.bound_request()
        manifest = self.bind_manifest(request)
        request["task_extensions"][0]["main_logical_path"] = "project://src/other.hpp"
        self.reseal_request(request)
        with self.assertRaisesRegex(SourceClosureTransportError, "base path"):
            validate_request_binding(request, [manifest])

        request = self.bound_request()
        manifest = self.bind_manifest(request)
        request["source_closures"][0]["member_count"] = 4096
        self.reseal_request(request)
        with self.assertRaisesRegex(SourceClosureTransportError, "census/manifest"):
            validate_request_binding(request, [manifest])

        request = self.bound_request()
        manifest = self.bind_manifest(request)
        extra = dict(request["source_closures"][0])
        extra["source_closure_id"] = "source-closure:semantic-v2:sha256:" + "9" * 64
        extra["source_closure_digest"] = "semantic-v2:sha256:" + "9" * 64
        request["source_closures"].append(extra)
        self.reseal_request(request)
        with self.assertRaisesRegex(SourceClosureTransportError, "manifest census"):
            validate_request_binding(request, [manifest])

    def test_reject_terminal_requires_typed_identity_and_cleanup_receipt(self) -> None:
        contract = yaml.safe_load((ROOT / CONTRACT).read_text(encoding="utf-8"))
        control = {
            "session_id": {},
            "task_id": None,
            "failure_phase": "before-manifest",
            "reason_code": "source-closure.required-feature-missing",
            "observed_counters": {"observed-control-frame-count": 0},
            "cleanup_receipt": ["fabricated"],
        }
        with self.assertRaisesRegex(SourceClosureTransportError, "typed ID"):
            validate_reject_control(control, contract)

    def test_transfer_state_witness_binds_outer_task_before_first_frame(self) -> None:
        contract = yaml.safe_load((ROOT / CONTRACT).read_text(encoding="utf-8"))
        schema = yaml.safe_load(
            (ROOT / MANIFEST_SCHEMA).read_text(encoding="utf-8")
        )
        request = self.bound_request()
        manifest = self.bind_manifest(request)
        extension = request["task_extensions"][0]
        payload = canonical_json(manifest)
        descriptor = {
            "kind": "descriptor",
            "session_id": SESSION_ID,
            "task_id": extension["task_id"],
            "task_v4_digest": extension["task_v4_digest"],
            "closure_id": extension["source_closure"]["id"],
            "closure_digest": extension["source_closure"]["digest"],
            "manifest_digest": extension["source_closure"]["manifest_digest"],
            "total_bytes": len(payload),
            "chunk_bytes": len(payload),
            "chunk_count": 1,
        }
        exact = TransferStateWitness.for_task_extension(
            session_id=SESSION_ID,
            task_extension=extension,
            manifest_schema=schema,
        )
        exact.apply("source_closure_manifest", descriptor, b"", contract)
        self.assertEqual(exact.state, "manifest-open")

        foreign = dict(descriptor)
        foreign["task_id"] = "task:semantic-v2:sha256:" + "f" * 64
        rebound = TransferStateWitness.for_task_extension(
            session_id=SESSION_ID,
            task_extension=extension,
            manifest_schema=schema,
        )
        with self.assertRaisesRegex(
            SourceClosureTransportError, "identity binding mismatch"
        ):
            rebound.apply(
                "source_closure_manifest", foreign, b"", contract
            )

    def test_transfer_state_witness_rejects_foreign_gap_and_zero_manifest(self) -> None:
        contract = yaml.safe_load((ROOT / CONTRACT).read_text(encoding="utf-8"))
        witness = TransferStateWitness(
            session_id=SESSION_ID,
            task_id=TASK_ID,
            task_v4_digest=SEMANTIC,
            closure_id="source-closure:" + SEMANTIC,
            closure_digest=SEMANTIC,
            manifest_digest=SEMANTIC,
            manifest_schema=yaml.safe_load((ROOT / MANIFEST_SCHEMA).read_text(encoding="utf-8")),
        )
        descriptor = {
            "kind": "descriptor", "session_id": SESSION_ID, "task_id": TASK_ID,
            "task_v4_digest": SEMANTIC, "closure_id": "source-closure:" + SEMANTIC,
            "closure_digest": SEMANTIC, "manifest_digest": SEMANTIC,
            "total_bytes": 1, "chunk_bytes": 1, "chunk_count": 1,
        }
        witness.apply("source_closure_manifest", descriptor, b"", contract)
        chunk = {
            "kind": "chunk", "session_id": SESSION_ID, "task_id": TASK_ID,
            "manifest_digest": SEMANTIC, "chunk_index": 9, "offset": 9,
            "byte_count": 1,
        }
        with self.assertRaisesRegex(SourceClosureTransportError, "contiguous"):
            witness.apply("source_closure_manifest", chunk, b"x", contract)
        chunk.update({"chunk_index": 0, "offset": 0, "session_id": "provider-session:sha256:" + "9" * 64})
        with self.assertRaisesRegex(SourceClosureTransportError, "binding"):
            witness.apply("source_closure_manifest", chunk, b"x", contract)
        descriptor.update({"total_bytes": 0, "chunk_count": 0})
        with self.assertRaisesRegex(SourceClosureTransportError, "one or more"):
            validate_wire_control(
                "source_closure_manifest", descriptor, b"", "task-v4-sealed", contract
            )

    def test_transfer_state_witness_recomputes_manifest_and_reject_is_terminal(self) -> None:
        contract = yaml.safe_load((ROOT / CONTRACT).read_text(encoding="utf-8"))
        content = "sha256:" + __import__("hashlib").sha256(b"x").hexdigest()
        members = [{"file_id": source_closure_file_id("project://src/main.cpp"), "logical_path": "project://src/main.cpp", "role": "main", "encoding": "utf8", "size_bytes": 1, "content_digest": content, "read_only": True}]
        blobs = [{"content_digest": content, "size_bytes": 1}]
        closure = closure_digest(members, blobs)
        manifest = {"schema": "cxxlens.source-closure-manifest.v1", "closure_id": "source-closure:" + closure, "closure_digest": closure, "members": members, "blobs": blobs}
        payload = canonical_json(manifest)
        digest = manifest_digest(manifest)
        schema = yaml.safe_load((ROOT / MANIFEST_SCHEMA).read_text(encoding="utf-8"))
        witness = TransferStateWitness(session_id=SESSION_ID, task_id=TASK_ID, task_v4_digest=SEMANTIC, closure_id=manifest["closure_id"], closure_digest=closure, manifest_digest=digest, manifest_schema=schema)
        descriptor = {"kind": "descriptor", "session_id": SESSION_ID, "task_id": TASK_ID, "task_v4_digest": SEMANTIC, "closure_id": manifest["closure_id"], "closure_digest": closure, "manifest_digest": digest, "total_bytes": len(payload), "chunk_bytes": len(payload), "chunk_count": 1}
        witness.apply("source_closure_manifest", descriptor, b"", contract)
        chunk = {"kind": "chunk", "session_id": SESSION_ID, "task_id": TASK_ID, "manifest_digest": digest, "chunk_index": 0, "offset": 0, "byte_count": len(payload)}
        with self.assertRaisesRegex(SourceClosureTransportError, "digest|canonical"):
            witness.apply("source_closure_manifest", chunk, payload[:-1] + b" ", contract)

        rejected = TransferStateWitness(session_id=SESSION_ID, task_id=TASK_ID, task_v4_digest=SEMANTIC, closure_id=manifest["closure_id"], closure_digest=closure, manifest_digest=digest, manifest_schema=schema)
        reject = {"session_id": SESSION_ID, "task_id": TASK_ID, "failure_phase": "before-manifest", "reason_code": "source-closure.required-feature-missing", "observed_counters": {"observed-control-frame-count": 0}, "cleanup_receipt": CLEANUP_RECEIPT}
        rejected.apply("source_closure_reject", reject, b"", contract)
        with self.assertRaisesRegex(SourceClosureTransportError, "terminal"):
            rejected.apply("source_closure_manifest", descriptor, b"", contract)

    def test_transfer_digest_is_canonical_mapping_order_independent(self) -> None:
        projection = {"session_id": SESSION_ID, "task_id": TASK_ID, "task_v4_digest": SEMANTIC, "manifest_digest": SEMANTIC, "blob_receipts_digest": SEMANTIC, "blob_count": 0, "total_bytes": 0, "closure_digest": SEMANTIC}
        self.assertEqual(transfer_digest(projection), transfer_digest(dict(reversed(list(projection.items())))))

    def test_transfer_witness_validates_manifest_chunk_profile_and_closure_sealed_reject(self) -> None:
        contract = yaml.safe_load((ROOT / CONTRACT).read_text(encoding="utf-8"))
        schema = yaml.safe_load((ROOT / MANIFEST_SCHEMA).read_text(encoding="utf-8"))
        content = "sha256:" + __import__("hashlib").sha256(b"x").hexdigest()
        members = [{"file_id": source_closure_file_id("project://src/main.cpp"), "logical_path": "project://src/main.cpp", "role": "main", "encoding": "utf8", "size_bytes": 1, "content_digest": content, "read_only": True}]
        blobs = [{"content_digest": content, "size_bytes": 1}]
        closure = closure_digest(members, blobs)
        manifest = {"schema": "cxxlens.source-closure-manifest.v1", "closure_id": "source-closure:" + closure, "closure_digest": closure, "members": members, "blobs": blobs}
        payload = canonical_json(manifest)
        digest = manifest_digest(manifest)

        def new_witness() -> TransferStateWitness:
            return TransferStateWitness(session_id=SESSION_ID, task_id=TASK_ID, task_v4_digest=SEMANTIC, closure_id=manifest["closure_id"], closure_digest=closure, manifest_digest=digest, manifest_schema=schema)

        descriptor = {"kind": "descriptor", "session_id": SESSION_ID, "task_id": TASK_ID, "task_v4_digest": SEMANTIC, "closure_id": manifest["closure_id"], "closure_digest": closure, "manifest_digest": digest, "total_bytes": len(payload), "chunk_bytes": len(payload) - 1, "chunk_count": 2}
        wrong_chunk = {"kind": "chunk", "session_id": SESSION_ID, "task_id": TASK_ID, "manifest_digest": digest, "chunk_index": 0, "offset": 0, "byte_count": len(payload)}
        chunk_witness = new_witness()
        chunk_witness.apply("source_closure_manifest", descriptor, b"", contract)
        with self.assertRaisesRegex(SourceClosureTransportError, "chunk size"):
            chunk_witness.apply("source_closure_manifest", wrong_chunk, payload, contract)

        descriptor.update({"chunk_bytes": len(payload), "chunk_count": 1})
        manifest_chunk = {**wrong_chunk, "byte_count": len(payload)}
        witness = new_witness()
        witness.apply("source_closure_manifest", descriptor, b"", contract)
        witness.apply("source_closure_manifest", manifest_chunk, payload, contract)
        witness.apply("source_closure_blob", {"session_id": SESSION_ID, "task_id": TASK_ID, "closure_digest": closure, "blob_ordinal": 0, "blob_digest": content, "total_bytes": 1, "chunk_bytes": 1, "chunk_count": 1}, b"", contract)
        witness.apply("source_closure_chunk", {"session_id": SESSION_ID, "task_id": TASK_ID, "blob_ordinal": 0, "blob_digest": content, "chunk_index": 0, "offset": 0, "byte_count": 1}, b"x", contract)
        receipts = blob_receipts_digest([{"blob_ordinal": 0, "blob_digest": content, "size_bytes": 1}])
        transfer = transfer_digest({"session_id": SESSION_ID, "task_id": TASK_ID, "task_v4_digest": SEMANTIC, "manifest_digest": digest, "blob_receipts_digest": receipts, "blob_count": 1, "total_bytes": 1, "closure_digest": closure})
        witness.apply("source_closure_seal", {"session_id": SESSION_ID, "task_id": TASK_ID, "task_v4_digest": SEMANTIC, "manifest_digest": digest, "blob_receipts_digest": receipts, "blob_count": 1, "total_bytes": 1, "closure_digest": closure, "transfer_digest": transfer}, b"", contract)
        phase = contract["failure_phase_matrix"]["closure-sealed"]
        witness.apply("source_closure_reject", {"session_id": SESSION_ID, "task_id": TASK_ID, "failure_phase": "closure-sealed", "reason_code": phase["allowed"][0], "observed_counters": {name: 0 for name in phase["counters"]}, "cleanup_receipt": CLEANUP_RECEIPT}, b"", contract)
        self.assertEqual(witness.state, "rejected")

        invalid_members = [
            {**members[0], "file_id": source_closure_file_id("project://z/main.cpp"), "logical_path": "project://z/main.cpp"},
            {**members[0], "file_id": source_closure_file_id("project://a/header.hpp"), "logical_path": "project://a/header.hpp", "role": "header"},
        ]
        invalid_closure = closure_digest(invalid_members, blobs)
        invalid_manifest = {"schema": "cxxlens.source-closure-manifest.v1", "closure_id": "source-closure:" + invalid_closure, "closure_digest": invalid_closure, "members": invalid_members, "blobs": blobs}
        invalid_payload = canonical_json(invalid_manifest)
        invalid_digest = manifest_digest(invalid_manifest)
        invalid = TransferStateWitness(session_id=SESSION_ID, task_id=TASK_ID, task_v4_digest=SEMANTIC, closure_id=invalid_manifest["closure_id"], closure_digest=invalid_closure, manifest_digest=invalid_digest, manifest_schema=schema)
        invalid.apply("source_closure_manifest", {"kind": "descriptor", "session_id": SESSION_ID, "task_id": TASK_ID, "task_v4_digest": SEMANTIC, "closure_id": invalid_manifest["closure_id"], "closure_digest": invalid_closure, "manifest_digest": invalid_digest, "total_bytes": len(invalid_payload), "chunk_bytes": len(invalid_payload), "chunk_count": 1}, b"", contract)
        with self.assertRaisesRegex(SourceClosureTransportError, "member order"):
            invalid.apply("source_closure_manifest", {"kind": "chunk", "session_id": SESSION_ID, "task_id": TASK_ID, "manifest_digest": invalid_digest, "chunk_index": 0, "offset": 0, "byte_count": len(invalid_payload)}, invalid_payload, contract)

    def test_manifest_main_metadata_must_equal_base_source(self) -> None:
        request = self.bound_request()
        manifest = self.bind_manifest(request)
        request["tasks"][0]["source"]["content_digest"] = "sha256:" + "e" * 64
        request["tasks"][0]["task_input_digest"] = "sha256:" + "d" * 64
        extension = request["task_extensions"][0]
        extension["base_task_v3_digest"] = content_projection_digest(request["tasks"][0])
        extension["open_task"]["task_input_digest"] = request["tasks"][0]["task_input_digest"]
        self.reseal_request(request)
        with self.assertRaisesRegex(SourceClosureTransportError, "main source"):
            validate_request_binding(request, [manifest])

    def test_bounded_terminal_seal_uses_one_digest_for_4096_blobs(self) -> None:
        receipts = [
            {
                "blob_ordinal": index,
                "blob_digest": "sha256:" + f"{index:064x}",
                "size_bytes": 12288,
            }
            for index in range(4096)
        ]
        self.assertEqual(sum(receipt["size_bytes"] for receipt in receipts), 50331648)
        request = self.bound_request()
        receipts_digest = blob_receipts_digest(receipts)
        projection = {
            "session_id": SESSION_ID,
            "task_id": request["task_extensions"][0]["task_id"],
            "task_v4_digest": request["task_extensions"][0]["task_v4_digest"],
            "manifest_digest": "semantic-v2:sha256:" + "7" * 64,
            "blob_receipts_digest": receipts_digest,
            "blob_count": 4096,
            "total_bytes": 50331648,
            "closure_digest": "semantic-v2:sha256:" + "1" * 64,
        }
        seal_control = {**projection, "transfer_digest": transfer_digest(projection)}
        encoded_seal = cbor_encode(seal_control)
        encoded_frame = encode_frame(
            seal_control,
            message_type=27,
            stream_id=1,
            sequence=29,
            protocol_minor=1,
        )
        self.assertRegex(receipts_digest, r"^semantic-v2:sha256:[0-9a-f]{64}$")
        self.assertRegex(transfer_digest(projection), r"^semantic-v2:sha256:[0-9a-f]{64}$")
        self.assertLessEqual(len(encoded_seal), 65536)
        self.assertEqual(len(encoded_frame), len(encoded_seal) + 104)
        self.assertLessEqual(len(encoded_frame), 16842856)

    def test_source_and_provider_control_limits_must_remain_equal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                SCHEMA,
                lambda value: value["properties"]["wire_controls"]["properties"][
                    "common"
                ]["const"].update({"control_bytes": 65535}),
            )
            self.rewrite(
                root,
                CONTRACT,
                lambda value: value["wire_controls"]["common"].update(
                    {"control_bytes": 65535}
                ),
            )
            with self.assertRaisesRegex(SourceClosureTransportError, "control-byte"):
                validate(root)

    def test_message_collision_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                CONTRACT,
                lambda value: value["message_registry"]["proposed"][0].update({"id": 23}),
            )
            with self.assertRaisesRegex(SourceClosureTransportError, "schema|collides"):
                validate(root)

    def test_protocol_downgrade_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                CONTRACT,
                lambda value: value["versions"]["provider_protocol"].update(
                    {"downgrade": "fallback"}
                ),
            )
            with self.assertRaisesRegex(SourceClosureTransportError, "version|schema"):
                validate(root)

    def test_normative_limit_and_ambient_mutation_are_rejected(self) -> None:
        for label, mutate in (
            (
                "ambient",
                lambda value: value["identity"].update({"ambient_filesystem": "allowed"}),
            ),
            (
                "chunks",
                lambda value: value["limits"].update(
                    {"maximum_blob_chunk_frames": 48}
                ),
            ),
        ):
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                root = self.copied_root(temporary)
                self.rewrite(root, CONTRACT, mutate)
                with self.assertRaisesRegex(SourceClosureTransportError, "schema|chunk"):
                    validate(root)

    def test_legacy_authority_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            path = root / "src/llvm/clang22/provider_task_v3.hpp"
            path.write_text(path.read_text(encoding="utf-8") + "\n", encoding="utf-8")
            with self.assertRaisesRegex(SourceClosureTransportError, "legacy"):
                validate(root)

    def test_acceptance_requires_authenticated_decision_receipt(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                CONTRACT,
                lambda value: (
                    value.update({"maturity": "accepted"}),
                    value["authority"].update(
                        {
                            "review": {
                                "status": "complete",
                                "reviewer": "independent-reviewer",
                                "ref": "https://github.com/horiyamayoh/cxxlens/issues/261#issuecomment-1",
                                "exact_main_commit": "1" * 40,
                            }
                        }
                    ),
                    value["review_findings"].update(
                        {
                            "status": "resolved",
                            "exact_main_commit": "1" * 40,
                            "ref": "https://github.com/horiyamayoh/cxxlens/issues/261#issuecomment-1",
                            "reviewer": "independent-reviewer",
                            "receipt_id": "review-receipt.source-closure.v1",
                        }
                    ),
                ),
            )
            adr = root / ADR
            adr.write_text(
                adr.read_text(encoding="utf-8").replace(
                    "- Status: Proposed for independent review", "- Status: Accepted"
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(Exception, "development_decision_register"):
                validate(root)

    def test_cross_task_cache_activation_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                CONTRACT,
                lambda value: value["cache"].update({"cross_task_v1": "enabled"}),
            )
            with self.assertRaisesRegex(SourceClosureTransportError, "schema|cache"):
                validate(root)

    def test_acceptance_without_review_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, CONTRACT, lambda value: value.update({"maturity": "accepted"}))
            with self.assertRaisesRegex(SourceClosureTransportError, "accepted authority"):
                validate(root)


if __name__ == "__main__":
    unittest.main()
