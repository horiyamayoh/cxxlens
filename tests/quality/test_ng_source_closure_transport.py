#!/usr/bin/env python3
"""Positive and negative tests for dedicated source-closure transport."""

from __future__ import annotations

import pathlib
import shutil
import sys
import tempfile
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_source_closure_transport import (  # noqa: E402
    ADR,
    CONTRACT,
    PROTOCOL,
    PROTOCOL_SCHEMA,
    REQUEST,
    SCHEMA,
    TASK,
    MANIFEST_SCHEMA,
    SourceClosureTransportError,
    blob_receipts_digest,
    content_projection_digest,
    request_v2_2_projection,
    semantic_digest,
    task_v4_projection,
    validate,
    validate_request_binding,
    transfer_digest,
)


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
            "normalized_invocation_digest": "sha256:" + "4" * 64,
            "toolchain_digest": "sha256:" + "5" * 64,
            "environment_digest": "sha256:" + "6" * 64,
        }
        closure = {
            "source_closure_id": closure_id,
            "source_closure_digest": semantic,
            "manifest_digest": "semantic-v2:sha256:" + "7" * 64,
        }
        extension = {
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
        request = {
            "schema": "cxxlens.clang22-materialization-request.v2_2",
            "request_version": "2.2.0",
            "required_features": ["task-input-chunks-v1", "task-source-closure-v1"],
            "base_request_v2_1": {"tasks": [base]},
            "source_closures": [closure],
            "task_extensions": [extension],
        }
        request["request_digest"] = semantic_digest(
            "cxxlens.clang22.materialization-request.v2_2",
            request_v2_2_projection(request),
        )
        request["request_id"] = "materialization-request:" + request["request_digest"]
        return request

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

    def test_adr_0101_identity_and_request_binding_are_constructible(self) -> None:
        validate_request_binding(self.bound_request())

    def test_duplicate_and_dangling_relationships_are_rejected(self) -> None:
        request = self.bound_request()
        request["source_closures"].append(dict(request["source_closures"][0]))
        with self.assertRaisesRegex(SourceClosureTransportError, "duplicate"):
            validate_request_binding(request)

    def test_request_and_task_semantic_identity_tamper_is_rejected(self) -> None:
        request = self.bound_request()
        request["request_digest"] = "semantic-v2:sha256:" + "9" * 64
        with self.assertRaisesRegex(SourceClosureTransportError, "request v2.2"):
            validate_request_binding(request)
        request = self.bound_request()
        request["task_extensions"][0]["task_v4_digest"] = (
            "semantic-v2:sha256:" + "9" * 64
        )
        with self.assertRaisesRegex(SourceClosureTransportError, "task v4"):
            validate_request_binding(request)
        request = self.bound_request()
        request["task_extensions"][0]["source_closure"]["id"] = (
            "source-closure:semantic-v2:sha256:" + "9" * 64
        )
        with self.assertRaisesRegex(SourceClosureTransportError, "does not resolve"):
            validate_request_binding(request)

    def test_traversal_and_utf8_byte_overflow_are_rejected(self) -> None:
        request = self.bound_request()
        request["task_extensions"][0]["main_logical_path"] = (
            "project://src/../ambient.hpp"
        )
        with self.assertRaisesRegex(SourceClosureTransportError, "segments"):
            validate_request_binding(request)
        request = self.bound_request()
        request["task_extensions"][0]["main_logical_path"] = "project://" + "é" * 4090
        with self.assertRaisesRegex(SourceClosureTransportError, "UTF-8"):
            validate_request_binding(request)

    def test_bounded_terminal_seal_uses_one_digest_for_4096_blobs(self) -> None:
        receipts = [
            {
                "blob_ordinal": index,
                "blob_digest": "sha256:" + f"{index:064x}",
                "size_bytes": 1,
            }
            for index in range(4096)
        ]
        request = self.bound_request()
        receipts_digest = blob_receipts_digest(receipts)
        projection = {
            "session_id": "session:1",
            "task_id": request["task_extensions"][0]["task_id"],
            "task_v4_digest": request["task_extensions"][0]["task_v4_digest"],
            "manifest_digest": "semantic-v2:sha256:" + "7" * 64,
            "blob_receipts_digest": receipts_digest,
            "blob_count": 4096,
            "total_bytes": 4096,
            "closure_digest": "semantic-v2:sha256:" + "1" * 64,
        }
        self.assertRegex(receipts_digest, r"^semantic-v2:sha256:[0-9a-f]{64}$")
        self.assertRegex(transfer_digest(projection), r"^semantic-v2:sha256:[0-9a-f]{64}$")

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

    def test_acceptance_requires_atomic_protocol_activation(self) -> None:
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
                ),
            )
            adr = root / ADR
            adr.write_text(
                adr.read_text(encoding="utf-8").replace(
                    "- Status: Proposed for independent review", "- Status: Accepted"
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(SourceClosureTransportError, "reviewed exact main commit"):
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
