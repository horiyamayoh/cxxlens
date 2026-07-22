#!/usr/bin/env python3
from __future__ import annotations

import copy
import hashlib
import inspect
import json
import pathlib
import subprocess
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_provider_runtime as provider_runtime  # noqa: E402


class NgProviderRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = provider_runtime.load(
            ROOT / "schemas/cxxlens_ng_provider_runtime_contract.yaml"
        )
        cls.protocol = provider_runtime.load(
            ROOT / "schemas/cxxlens_ng_provider_protocol.yaml"
        )
        registry = provider_runtime.load(
            ROOT / "schemas/cxxlens_ng_relation_registry.yaml"
        )
        cls.relations = {
            relation["descriptor_id"]: relation for relation in registry["relations"]
        }

    def test_repository_contract(self) -> None:
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/quality/check_ng_provider_runtime.py"),
                "check",
                "--root",
                str(ROOT),
            ],
            check=True,
        )

    def test_task_codec_requires_exactly_one_task_v3_marker(self) -> None:
        codec = (ROOT / "src/llvm/clang22/provider_task_v3.cpp").read_text(
            encoding="utf-8"
        )
        provider_runtime.validate_task_codec_markers(codec)
        for drift in (
            codec.replace(provider_runtime.WORKER_TASK_CODEC_V3, "task-v3-missing"),
            codec + "\n" + provider_runtime.WORKER_TASK_CODEC_V3,
        ):
            with self.assertRaisesRegex(
                provider_runtime.ContractError, "exactly one installed task.v3"
            ):
                provider_runtime.validate_task_codec_markers(drift)

    def test_task_codec_rejects_legacy_task_v2_alias(self) -> None:
        codec = (ROOT / "src/llvm/clang22/provider_task_v3.cpp").read_text(
            encoding="utf-8"
        )
        with self.assertRaisesRegex(
            provider_runtime.ContractError, "task.v2 codec remains adoptable"
        ):
            provider_runtime.validate_task_codec_markers(
                codec + "\n" + provider_runtime.LEGACY_WORKER_TASK_CODEC_V2
            )

    def test_host_input_profiles_bind_protocol_minor_zero_and_one(self) -> None:
        provider_runtime.validate_host_input_authority(self.contract, self.protocol)

    def test_host_input_authority_rejects_compatibility_and_security_drift(self) -> None:
        drifts = []
        changed = copy.deepcopy(self.contract)
        changed["protocol_session"]["host_input_profiles"]["1.0"][
            "exact_frames"
        ].insert(3, "input_chunk")
        drifts.append(changed)
        changed = copy.deepcopy(self.contract)
        changed["protocol_session"]["host_input_profiles"]["1.1"][
            "required_features"
        ] = []
        drifts.append(changed)
        changed = copy.deepcopy(self.contract)
        changed["protocol_session"]["host_input_transfer"]["limits"][
            "logical_input_bytes"
        ] += 1
        drifts.append(changed)
        changed = copy.deepcopy(self.contract)
        changed["protocol_session"]["host_input_transfer"][
            "ambient_input_side_channel"
        ] = "allowed"
        drifts.append(changed)
        changed = copy.deepcopy(self.contract)
        changed["protocol_session"]["typed_validation"]["host_input_credit"] = (
            "input-and-output"
        )
        drifts.append(changed)
        for changed in drifts:
            with self.assertRaises(provider_runtime.ContractError):
                provider_runtime.validate_host_input_authority(
                    changed, self.protocol
                )

    @staticmethod
    def indexed_control(
        schema: str, fields: list[str], records: list[dict[str, str]]
    ) -> dict[str, object]:
        rows = sorted([[record[field] for field in fields] for record in records])
        control: dict[str, object] = {"schema": schema, "record_count": len(rows)}
        for index, row in enumerate(rows):
            for field, value in zip(fields, row, strict=True):
                control[f"{index}.{field}"] = value
        return control

    @staticmethod
    def fixture_scalar(type_name: str, seed: str) -> object:
        while type_name.startswith("optional<") and type_name.endswith(">"):
            type_name = type_name[len("optional<") : -1]
        scalar = type_name.split("<", 1)[0]
        if scalar == "bool":
            return True
        if scalar == "int64":
            return -7
        if scalar == "uint64":
            return 7
        if scalar in {"bytes", "set"}:
            return b"" if scalar == "set" else seed.encode("utf-8")
        if scalar == "digest":
            return "semantic-v2:sha256:" + hashlib.sha256(seed.encode()).hexdigest()
        if scalar == "semantic_version":
            return "1.0.0"
        if scalar == "closed_symbol":
            if "cc.canonicalization-state/1" in type_name:
                return "canonicalized"
            return "fixture"
        return f"fixture:{seed}"

    @staticmethod
    def encoded_present_column(
        authority: dict[str, object],
        column: dict[str, object],
        task_id: str,
        value: object,
    ) -> tuple[dict[str, object], bytes]:
        _, scalar_kind, _ = provider_runtime._parse_authorized_type(column["type"])
        encoding = provider_runtime._column_encoding(scalar_kind)
        validity = b"\x01"
        unknown = b"\x00"
        auxiliary = b""
        values = b""
        if scalar_kind == provider_runtime.SCALAR_KINDS["bool"]:
            values = b"\x01" if value else b"\x00"
        elif scalar_kind == provider_runtime.SCALAR_KINDS["int64"]:
            values = int(value).to_bytes(8, "little", signed=True)
        elif scalar_kind == provider_runtime.SCALAR_KINDS["uint64"]:
            values = int(value).to_bytes(8, "little")
        elif encoding == "dictionary-index-u32-le":
            encoded = str(value).encode("utf-8")
            auxiliary = (
                (1).to_bytes(4, "little")
                + (0).to_bytes(4, "little")
                + len(encoded).to_bytes(4, "little")
                + encoded
            )
            values = (0).to_bytes(4, "little")
        else:
            encoded = value if isinstance(value, bytes) else str(value).encode("utf-8")
            auxiliary = (0).to_bytes(4, "little") + len(encoded).to_bytes(4, "little")
            values = encoded
        reason_offsets = b"\0" * 8
        sections = [validity, unknown, auxiliary, values, reason_offsets, b""]
        payload = (
            b"CXCC\x01"
            + bytes([scalar_kind])
            + b"\0\0"
            + b"".join(len(section).to_bytes(4, "little") for section in sections)
            + b"".join(sections)
        )
        payload_digest = "sha256:" + hashlib.sha256(payload).hexdigest()
        control: dict[str, object] = {
            "task_id": task_id,
            "dependency_group_id": authority["dependency_group_id"],
            "atomic_output_group_id": authority["atomic_output_group_id"],
            "batch_id": authority["batch_id"],
            "descriptor_id": authority["descriptor_id"],
            "descriptor_digest": authority["descriptor_digest"],
            "column_id": column["id"],
            "row_offset": 0,
            "row_count": 1,
            "chunk_index": 0,
            "encoding": encoding,
            "payload_digest": payload_digest,
        }
        control["chunk_digest"] = provider_runtime._semantic_fields_digest(
            "cxxlens.provider-column-chunk.v2",
            "cxxlens.provider-column-chunk-digest.v2",
            list(control.items()),
        )
        return control, payload

    @staticmethod
    def encoded_batch_end(
        authority: dict[str, object],
        task_id: str,
        chunks: list[tuple[dict[str, object], bytes]],
        *,
        reverse_chunk_summary: bool = False,
    ) -> tuple[dict[str, object], bytes]:
        chunks_by_column = {
            control["column_id"]: (control, payload) for control, payload in chunks
        }
        columns = [
            {
                "column_id": column["id"],
                "payload_bytes": (
                    len(chunks_by_column[column["id"]][1])
                    if column["id"] in chunks_by_column
                    else 0
                ),
                "chunk_count": 1 if column["id"] in chunks_by_column else 0,
            }
            for column in authority["columns"]
        ]
        ordered_digests = [control["chunk_digest"] for control, _ in chunks]
        if reverse_chunk_summary:
            ordered_digests.reverse()
        row_count = 1 if chunks else 0
        control: dict[str, object] = {
            "task_id": task_id,
            "dependency_group_id": authority["dependency_group_id"],
            "atomic_output_group_id": authority["atomic_output_group_id"],
            "batch_id": authority["batch_id"],
            "descriptor_id": authority["descriptor_id"],
            "descriptor_digest": authority["descriptor_digest"],
            "row_count": row_count,
            "column_count": len(columns),
            "chunk_count": len(ordered_digests),
        }
        column_projection = provider_runtime._canonical_tuple(
            provider_runtime._canonical_tuple(
                (
                    provider_runtime._digest_text_field(
                        "column_id", column["column_id"]
                    ),
                    provider_runtime._digest_u64_field(
                        "payload_bytes", column["payload_bytes"]
                    ),
                    provider_runtime._digest_u64_field(
                        "chunk_count", column["chunk_count"]
                    ),
                )
            )
            for column in columns
        )
        digest_projection = provider_runtime._canonical_tuple(
            provider_runtime._digest_text_field("chunk_digest", digest)
            for digest in ordered_digests
        )
        control["batch_digest"] = provider_runtime._semantic_fields_digest(
            "cxxlens.provider-columnar-batch.v2",
            "cxxlens.provider-columnar-batch-digest.v2",
            list(control.items()),
            (
                provider_runtime._digest_value_field("columns", column_projection),
                provider_runtime._digest_value_field(
                    "ordered_chunk_digests", digest_projection
                ),
            ),
        )
        payload = bytearray(b"CXBE\x01\0\0\0")
        for column in columns:
            encoded_id = column["column_id"].encode("utf-8")
            payload.extend(len(encoded_id).to_bytes(2, "little"))
            payload.extend(encoded_id)
            payload.extend(column["payload_bytes"].to_bytes(8, "little"))
            payload.extend(column["chunk_count"].to_bytes(8, "little"))
        for digest in ordered_digests:
            encoded_digest = digest.encode("utf-8")
            payload.extend(len(encoded_digest).to_bytes(2, "little"))
            payload.extend(encoded_digest)
        return control, bytes(payload)

    @classmethod
    def authorized_batches(cls) -> list[dict[str, object]]:
        descriptor_ids = [
            "cc.call_direct_target.v1",
            "cc.call_site.v1",
            "cc.entity.v1",
            "frontend.clang22.call_observation.v2",
            "frontend.clang22.entity_observation.v2",
            "frontend.clang22.type_observation.v2",
        ]
        return [
            {
                "descriptor_id": descriptor_id,
                "descriptor_digest": "semantic-v2:sha256:"
                + hashlib.sha256((descriptor_id + "|descriptor").encode()).hexdigest(),
                "dependency_group_id": (
                    "canonical" if descriptor_id.startswith("cc.") else "observation"
                ),
                "atomic_output_group_id": "clang22-atomic",
                "batch_id": descriptor_id + "-batch",
                "columns": [
                    {
                        "id": column["id"],
                        "type": column["type"],
                        "required": column["required"],
                    }
                    for column in cls.relations[descriptor_id]["columns"]
                ],
            }
            for descriptor_id in descriptor_ids
        ]

    @classmethod
    def provider_identity(
        cls,
        *,
        provider_id: str = "provider.fixture",
        provider_version: str = "1.0.0",
        provider_binary_digest: str = "sha256:" + "a" * 64,
        provider_semantic_contract_digest: str = "sha256:" + "b" * 64,
    ) -> dict[str, object]:
        return {
            "provider_id": provider_id,
            "provider_version": provider_version,
            "provider_binary_digest": provider_binary_digest,
            "provider_semantic_contract_digest": provider_semantic_contract_digest,
            "protocol_major": 1,
            "protocol_minor": 1,
            "required_features": ["task-input-chunks-v1"],
            "sandbox_policy_digest": "sha256:" + "c" * 64,
            "offered_relations": sorted(
                batch["descriptor_id"] for batch in cls.authorized_batches()
            ),
        }

    @classmethod
    def runtime_receipt_fixture(
        cls,
        *,
        provider_id: str = "provider.fixture",
        provider_version: str = "1.0.0",
        provider_binary_digest: str = "sha256:" + "a" * 64,
        provider_semantic_contract_digest: str = "sha256:" + "b" * 64,
        extra_reason: str = "retain-without-interpretation",
        mutation: str | None = None,
    ) -> tuple[bytes, str, list[dict[str, object]], dict[str, object]]:
        task_id = "task:runtime-receipt"
        authorized_batches = cls.authorized_batches()
        provider_identity = cls.provider_identity(
            provider_id=provider_id,
            provider_version=provider_version,
            provider_binary_digest=provider_binary_digest,
            provider_semantic_contract_digest=provider_semantic_contract_digest,
        )
        manifest = json.dumps(
            provider_identity,
            separators=(",", ":"),
            sort_keys=True,
        )
        coverage = cls.indexed_control(
            "cxxlens.provider-control.coverage.v1",
            provider_runtime.COVERAGE_FIELDS,
            [
                {"kind": "task", "id": task_id, "state": "covered", "reason": ""},
                {
                    "kind": "future.semantic",
                    "id": "opaque|id",
                    "state": "unresolved",
                    "reason": extra_reason,
                },
            ],
        )
        unresolved = cls.indexed_control(
            "cxxlens.provider-control.unresolved.v1",
            provider_runtime.UNRESOLVED_FIELDS,
            [
                {
                    "code": "provider.future-unresolved",
                    "subject": task_id,
                    "detail": "opaque",
                }
            ],
        )
        evidence = cls.indexed_control(
            "cxxlens.provider-control.evidence.v1",
            provider_runtime.EVIDENCE_FIELDS,
            [
                {
                    "kind": "provider.fixture",
                    "subject": task_id,
                    "producer": "quality-checker",
                    "summary": "runtime-owned",
                }
            ],
        )
        frames: list[tuple[int, object, bytes]] = [
            (1, manifest, b""),
            (
                3,
                {
                    "schema": "cxxlens.provider-control.schema-negotiate.v1",
                    "protocol_schema": "cxxlens.provider-protocol.v1",
                    "protocol_minor": 1,
                },
                b"",
            ),
            (
                5,
                {
                    "schema": "cxxlens.provider-control.task-accepted.v1",
                    "provider_id": provider_id,
                    "provider_version": provider_version,
                    "task_id": task_id,
                },
                b"",
            ),
        ]
        for batch_index, authority in enumerate(authorized_batches):
            if mutation == "omit-authorized-batch" and batch_index == len(authorized_batches) - 1:
                continue
            frames.append(
                (
                    9,
                    {
                        "schema": "cxxlens.provider-control.batch-begin.v1",
                        "task_id": task_id,
                        "descriptor_id": authority["descriptor_id"],
                        "descriptor_digest": authority["descriptor_digest"],
                        "dependency_group_id": authority["dependency_group_id"],
                        "atomic_output_group_id": authority["atomic_output_group_id"],
                        "batch_id": authority["batch_id"],
                    },
                    b"",
                )
            )
            chunks: list[tuple[dict[str, object], bytes]] = []
            for column_index, column in enumerate(authority["columns"]):
                chunk_task = (
                    "task:wrong-binding"
                    if mutation == "wrong-chunk-task" and batch_index == 0 and column_index == 0
                    else task_id
                )
                chunk = cls.encoded_present_column(
                    authority,
                    column,
                    chunk_task,
                    cls.fixture_scalar(
                        column["type"], f"{authority['descriptor_id']}|{column['id']}"
                    ),
                )
                if mutation == "wrong-payload-digest" and batch_index == 0 and column_index == 0:
                    changed_control = dict(chunk[0])
                    changed_control["payload_digest"] = "sha256:" + "f" * 64
                    semantic_fields = [
                        (field, changed_control[field])
                        for field in provider_runtime.CHUNK_CONTROL_FIELDS
                        if field != "chunk_digest"
                    ]
                    changed_control["chunk_digest"] = provider_runtime._semantic_fields_digest(
                        "cxxlens.provider-column-chunk.v2",
                        "cxxlens.provider-column-chunk-digest.v2",
                        semantic_fields,
                    )
                    chunk = changed_control, chunk[1]
                chunks.append(chunk)
                frames.append((10, chunk[0], chunk[1]))
            end_control, end_payload = cls.encoded_batch_end(
                authority,
                task_id,
                chunks,
                reverse_chunk_summary=(
                    mutation == "reordered-batch-summary" and batch_index == 0
                ),
            )
            if mutation != "missing-batch-end" or batch_index != 0:
                frames.append((11, end_control, end_payload))
        frames.extend(
            [
                (14, coverage, b""),
                (15, unresolved, b""),
                (17, evidence, b""),
                (
                    20,
                    {
                        "schema": "cxxlens.provider-control.task-complete.v1",
                        "task_id": task_id,
                        "status": "complete",
                    },
                    b"",
                ),
            ]
        )
        raw_stdout = b"".join(
            provider_runtime.encode_frame(
                control,
                payload,
                protocol_minor=1,
                message_type=message_type,
                stream_id=1,
                sequence=sequence,
            )
            for sequence, (message_type, control, payload) in enumerate(frames)
        )
        return raw_stdout, task_id, authorized_batches, provider_identity

    def test_runtime_private_receipt_derives_all_three_authorities(self) -> None:
        raw_stdout, task_id, authorized_batches, provider_identity = (
            self.runtime_receipt_fixture()
        )
        observation = provider_runtime.derive_runtime_private_observation(
            self.protocol,
            raw_stdout,
            task_id,
            expected_provider_identity=provider_identity,
            authorized_batches=authorized_batches,
        )
        receipt = observation["receipt"]
        self.assertTrue(raw_stdout.startswith(b"CXXP"))
        self.assertEqual(list(receipt), provider_runtime.RECEIPT_FIELDS)
        self.assertEqual(receipt["raw_stdout_byte_count"], len(raw_stdout))
        self.assertGreater(receipt["decoded_frame_count"], 25)
        self.assertEqual(len(observation["sealed_transcript"]["batches"]), 6)
        self.assertTrue(
            all(
                len(batch["row_canonical_forms"]) == 1
                for batch in observation["sealed_transcript"]["batches"]
            )
        )
        sealed = observation["sealed_transcript"]
        shared_fixture_raw = provider_runtime.encode_runtime_private_fixture(
            self.protocol,
            provider_identity,
            task_id,
            authorized_batches,
            {
                batch["batch_id"]: batch["row_canonical_forms"]
                for batch in sealed["batches"]
            },
            sealed["coverage_records"],
            sealed["unresolved_records"],
            sealed["evidence_records"],
        )
        self.assertEqual(shared_fixture_raw, raw_stdout)
        self.assertEqual(
            provider_runtime.derive_runtime_private_observation(
                self.protocol,
                shared_fixture_raw,
                task_id,
                expected_provider_identity=provider_identity,
                authorized_batches=authorized_batches,
            )["sealed_transcript"],
            sealed,
        )
        self.assertEqual(
            observation["validated_provider_identity"],
            provider_identity,
        )
        self.assertTrue(receipt["raw_stdout_sha256"].startswith("sha256:"))
        self.assertTrue(
            receipt["frame_transcript_digest"].startswith("semantic-v2:sha256:")
        )
        self.assertTrue(
            receipt["sealed_transcript_digest"].startswith("semantic-v2:sha256:")
        )
        self.assertEqual(
            provider_runtime.validate_runtime_private_receipt(
                self.protocol,
                raw_stdout,
                task_id,
                receipt,
                expected_provider_identity=provider_identity,
                authorized_batches=authorized_batches,
                public_semantic_digest="semantic-v2:sha256:" + "f" * 64,
            ),
            receipt,
        )

    def test_runtime_receipt_rejects_raw_frame_and_sealed_mutations(self) -> None:
        raw_stdout, task_id, authorized_batches, provider_identity = (
            self.runtime_receipt_fixture()
        )
        receipt = provider_runtime.derive_runtime_private_receipt(
            self.protocol,
            raw_stdout,
            task_id,
            expected_provider_identity=provider_identity,
            authorized_batches=authorized_batches,
        )
        corrupted_raw = raw_stdout[:-1] + bytes([raw_stdout[-1] ^ 1])
        changed_frame_raw, _, _, _ = self.runtime_receipt_fixture(
            provider_id="provider.changed"
        )
        changed_seal_raw, _, _, _ = self.runtime_receipt_fixture(
            extra_reason="retained-semantic-mutation"
        )
        retained_mutation = provider_runtime.derive_runtime_private_receipt(
            self.protocol,
            changed_seal_raw,
            task_id,
            expected_provider_identity=provider_identity,
            authorized_batches=authorized_batches,
        )
        self.assertNotEqual(
            retained_mutation["sealed_transcript_digest"],
            receipt["sealed_transcript_digest"],
        )
        for changed_raw in (corrupted_raw, changed_frame_raw, changed_seal_raw):
            with self.subTest(raw=changed_raw):
                with self.assertRaises(provider_runtime.ContractError):
                    provider_runtime.validate_runtime_private_receipt(
                        self.protocol,
                        changed_raw,
                        task_id,
                        receipt,
                        expected_provider_identity=provider_identity,
                        authorized_batches=authorized_batches,
                    )

    def test_runtime_receipt_requires_independent_selected_provider_identity(self) -> None:
        trusted = self.provider_identity()
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
                raw_stdout, task_id, authorized_batches, _ = (
                    self.runtime_receipt_fixture(**attack)
                )
                with self.assertRaisesRegex(
                    provider_runtime.ContractError,
                    "provider hello differs from independent expected identity",
                ):
                    provider_runtime.derive_runtime_private_observation(
                        self.protocol,
                        raw_stdout,
                        task_id,
                        expected_provider_identity=trusted,
                        authorized_batches=authorized_batches,
                    )

    def test_runtime_receipt_rejects_batch_binding_digest_order_and_census_drift(self) -> None:
        for mutation, reason in (
            ("wrong-chunk-task", "authorized batch binding"),
            ("wrong-payload-digest", "payload digest differs"),
            ("reordered-batch-summary", "chunk order differs"),
            ("missing-batch-end", "no immediate batch end"),
            ("omit-authorized-batch", "omits an authorized output batch"),
        ):
            with self.subTest(mutation=mutation):
                raw_stdout, task_id, authorized_batches, provider_identity = (
                    self.runtime_receipt_fixture(mutation=mutation)
                )
                with self.assertRaisesRegex(provider_runtime.ContractError, reason):
                    provider_runtime.derive_runtime_private_observation(
                        self.protocol,
                        raw_stdout,
                        task_id,
                        expected_provider_identity=provider_identity,
                        authorized_batches=authorized_batches,
                    )

    def test_runtime_receipt_rejects_downstream_reconstruction_attack(self) -> None:
        parameters = inspect.signature(
            provider_runtime.derive_runtime_private_receipt
        ).parameters
        self.assertNotIn("decoded_frames", parameters)
        self.assertNotIn("sealed_transcript", parameters)
        self.assertNotIn("same_pass_marker", parameters)
        self.assertIn("expected_provider_identity", parameters)
        self.assertIs(
            parameters["expected_provider_identity"].default,
            inspect.Parameter.empty,
        )
        handcrafted = {
            "raw_stdout_byte_count": 12,
            "raw_stdout_sha256": "sha256:" + "1" * 64,
            "decoded_frame_count": 7,
            "frame_transcript_digest": "semantic-v2:sha256:" + "2" * 64,
            "sealed_transcript_digest": "semantic-v2:sha256:" + "3" * 64,
        }
        with self.assertRaisesRegex(
            provider_runtime.ContractError, "truncated fixed header"
        ):
            provider_runtime.validate_runtime_private_receipt(
                self.protocol,
                b"not-a-frame",
                "task:runtime-receipt",
                handcrafted,
                expected_provider_identity=self.provider_identity(),
            )
        raw_stdout, task_id, authorized_batches, provider_identity = (
            self.runtime_receipt_fixture()
        )
        header = list(provider_runtime.FRAME.unpack(raw_stdout[: provider_runtime.FRAME.size]))
        header[8] = self.protocol["wire"]["limits"]["payload_bytes"] + 1
        oversized = (
            provider_runtime.FRAME.pack(*header)
            + raw_stdout[provider_runtime.FRAME.size :]
        )
        with self.assertRaisesRegex(
            provider_runtime.ContractError, "exceeds negotiated limits"
        ):
            provider_runtime.derive_runtime_private_receipt(
                self.protocol,
                oversized,
                task_id,
                expected_provider_identity=provider_identity,
                authorized_batches=authorized_batches,
            )

    def test_runtime_receipt_rejects_extra_field_and_public_alias(self) -> None:
        raw_stdout, task_id, authorized_batches, provider_identity = (
            self.runtime_receipt_fixture()
        )
        receipt = provider_runtime.derive_runtime_private_receipt(
            self.protocol,
            raw_stdout,
            task_id,
            expected_provider_identity=provider_identity,
            authorized_batches=authorized_batches,
        )
        extra = copy.deepcopy(receipt)
        extra["report_self_consistent"] = True
        with self.assertRaisesRegex(
            provider_runtime.ContractError, "exact projection is not closed"
        ):
            provider_runtime.validate_runtime_private_receipt(
                self.protocol,
                raw_stdout,
                task_id,
                extra,
                expected_provider_identity=provider_identity,
                authorized_batches=authorized_batches,
            )
        with self.assertRaisesRegex(provider_runtime.ContractError, "aliases"):
            provider_runtime.validate_runtime_private_receipt(
                self.protocol,
                raw_stdout,
                task_id,
                receipt,
                expected_provider_identity=provider_identity,
                authorized_batches=authorized_batches,
                public_semantic_digest=receipt["sealed_transcript_digest"],
            )

    def test_runtime_receipt_authority_rejects_projection_or_alias_drift(self) -> None:
        drifts = []
        changed = copy.deepcopy(self.contract)
        changed["protocol_session"]["runtime_private_receipt"][
            "expected_provider_identity"
        ]["exact_fields"].remove("provider_binary_digest")
        drifts.append(changed)
        changed = copy.deepcopy(self.contract)
        changed["protocol_session"]["runtime_private_receipt"]["construction"] = (
            "separate-report-pass"
        )
        drifts.append(changed)
        changed = copy.deepcopy(self.contract)
        changed["protocol_session"]["runtime_private_receipt"]["frame_transcript"][
            "domain"
        ] = "cxxlens.provider-frame-transcript.v1"
        drifts.append(changed)
        changed = copy.deepcopy(self.contract)
        changed["protocol_session"]["runtime_private_receipt"]["sealed_transcript"][
            "exact_fields"
        ].remove("coverage_records")
        drifts.append(changed)
        changed = copy.deepcopy(self.contract)
        changed["protocol_session"]["runtime_private_receipt"][
            "public_process_execution_report_semantic_digest"
        ]["alias_for_any_receipt_field"] = True
        drifts.append(changed)
        changed = copy.deepcopy(self.contract)
        changed["protocol_session"]["runtime_private_receipt"][
            "report-supplied-digest-or-self-consistency"
        ] = "allowed"
        drifts.append(changed)
        changed = copy.deepcopy(self.contract)
        changed["report"]["public_semantic_digest_role"] = "receipt-self-consistency"
        drifts.append(changed)
        for changed in drifts:
            with self.assertRaises(provider_runtime.ContractError):
                provider_runtime.validate_runtime_private_receipt_authority(changed)


if __name__ == "__main__":
    unittest.main()
