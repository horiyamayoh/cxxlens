#!/usr/bin/env python3
"""Executable provider wire, streaming, atomicity, and planning contract."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import re
import struct
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
# Protocol 2 is the only wire authority. Legacy Protocol 1 is represented only
# by fail-closed rejection markers in the compatibility contract; it is never
# loaded as a compatibility shim.
CONTRACT = pathlib.Path("schemas/cxxlens_ng_provider_protocol_v2.yaml")
CONTRACT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_provider_protocol_v2.schema.yaml")
MANIFEST_SCHEMA = pathlib.Path("schemas/cxxlens_ng_provider_manifest.schema.yaml")

FRAME = struct.Struct(">4sHHHHQQIQ32s32s")
MAX_CONTROL = 65536
MAX_PAYLOAD = 16777216
MAX_TASK_INPUT_CHUNK = 1048576
MAX_LOGICAL_TASK_INPUT = 67108864
MAX_TASK_INPUT_CHUNKS = 64
PROTOCOL_MAJOR = 2
PROTOCOL_MINOR = 0
SHARED_COVERAGE_AUTHORITY = {
    "validator": "single-shared-provider-transcript-validator",
    "specialization_awareness": "forbidden",
    "exact_record_fields": ["kind", "id", "state", "reason"],
    "accepted_states": ["covered", "excluded", "failed", "not_applicable", "unresolved"],
    "record_identity": ["kind", "id"],
    "duplicate_identity": "reject",
    "task_transport_record": {
        "exact_projection": {
            "kind": "task",
            "id": "exact-provider-task-id",
            "state": "covered",
            "reason": "empty",
        },
        "cardinality": "exactly-one",
        "mutation_rejection": [
            "missing",
            "duplicate",
            "renamed",
            "wrong-task",
            "non-covered",
            "nonempty-reason",
        ],
    },
    "non_transport_records": {
        "retention": "exact-decoded-records-in-wire-order",
        "unknown_or_extra_semantic": "retain-losslessly-without-interpretation",
        "discard": "forbidden",
        "reclassification": "forbidden",
    },
    "immutable_seal": (
        "complete-retained-record-set-value-owned-by-shared-validation-pass"
    ),
}
REUSE_FIELDS = (
    "provider_id",
    "provider_version",
    "semantic_contract_digest",
    "binary_digest",
    "protocol_major",
    "relation_descriptor_digests",
    "input_partition_digests",
    "condition_universe",
    "interpretation",
    "model_assumption_pack",
)
FIXED_POINT_FIELDS = {
    "monotone_lattice_id",
    "join_operator_id",
    "convergence_predicate_id",
    "max_iterations",
    "execution_budget",
}


class ProviderContractError(ValueError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code


def fail(code: str, message: str) -> None:
    raise ProviderContractError(code, message)


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail("provider.document-invalid", str(path))
    return value


def schema_validate(value: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(value)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail("provider.schema-invalid", f"{label}: {error.message}")


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")


def digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value)).hexdigest()


def byte_digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _require_exact_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    if set(value) != expected:
        fail("provider.task-input-invalid", f"{label} exact fields")


def _require_uint(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        fail("provider.task-input-invalid", f"{label} unsigned integer")
    return value


def validate_shared_coverage_records(
    task_id: str, records: Any
) -> list[dict[str, str]]:
    """Validate generic transport coverage and retain every opaque semantic record."""

    if not isinstance(task_id, str) or not task_id or "\0" in task_id:
        fail("provider.coverage-incomplete", "task identity")
    if not isinstance(records, list):
        fail("provider.coverage-incomplete", "coverage record set")
    retained: list[dict[str, str]] = []
    identities: set[tuple[str, str]] = set()
    task_records = 0
    accepted_states = set(SHARED_COVERAGE_AUTHORITY["accepted_states"])
    for record in records:
        if not isinstance(record, dict) or set(record) != {
            "kind",
            "id",
            "state",
            "reason",
        }:
            fail("provider.coverage-incomplete", "coverage exact fields")
        if not all(isinstance(record[field], str) for field in record):
            fail("provider.coverage-incomplete", "coverage text field")
        if (
            not record["kind"]
            or not record["id"]
            or "\0" in record["kind"]
            or "\0" in record["id"]
            or record["state"] not in accepted_states
        ):
            fail("provider.coverage-incomplete", "coverage value")
        identity = (record["kind"], record["id"])
        if identity in identities:
            fail("provider.coverage-incomplete", "duplicate coverage identity")
        identities.add(identity)
        if record["kind"] == "task":
            task_records += 1
            if record != {
                "kind": "task",
                "id": task_id,
                "state": "covered",
                "reason": "",
            }:
                fail("provider.coverage-incomplete", "task transport record")
        retained.append(copy.deepcopy(record))
    if task_records != 1:
        fail("provider.coverage-incomplete", "exactly one task transport record")
    return retained


def validate_shared_coverage_authority(contract: dict[str, Any]) -> None:
    if contract.get("schema") != "cxxlens.provider-protocol.v2":
        fail("provider.coverage-authority-invalid", "Protocol 2 authority is required")
    # Protocol 2 keeps the detailed coverage projection in task/report
    # contracts. This shared runtime helper only validates the product-level
    # authority identity and never consults a legacy metadata block.


def validate_task_input_chunks(value: dict[str, Any]) -> dict[str, Any]:
    """Independent executable oracle for the Protocol 2 input seal."""
    _require_exact_keys(
        value,
        {
            "protocol_minor",
            "features",
            "task_id",
            "input_digest",
            "descriptor",
            "chunks",
            "credit_sequence",
            "close_sequence",
        },
        "transfer",
    )
    if value["protocol_minor"] != PROTOCOL_MINOR:
        fail("provider.protocol-minor-mismatch", str(value["protocol_minor"]))
    if value["features"] != ["task-input-chunks-v2"]:
        fail("provider.required-feature-missing", "task-input-chunks-v2")
    task_id = value["task_id"]
    input_digest = value["input_digest"]
    if not isinstance(task_id, str) or not task_id:
        fail("provider.task-input-invalid", "task_id")
    if not isinstance(input_digest, str) or not re.fullmatch(
        r"sha256:[0-9a-f]{64}", input_digest
    ):
        fail("provider.task-input-invalid", "input_digest")

    descriptor = value["descriptor"]
    if not isinstance(descriptor, dict):
        fail("provider.task-input-invalid", "descriptor occurrence")
    _require_exact_keys(descriptor, {"sequence", "control"}, "descriptor occurrence")
    if _require_uint(descriptor["sequence"], "descriptor sequence") != 3:
        fail("provider.protocol-state-invalid", "input_descriptor sequence")
    control = descriptor["control"]
    if not isinstance(control, dict):
        fail("provider.task-input-invalid", "descriptor control")
    _require_exact_keys(
        control,
        {
            "schema",
            "task_id",
            "input_digest",
            "total_bytes",
            "chunk_bytes",
            "chunk_count",
        },
        "descriptor control",
    )
    if control["schema"] != "cxxlens.provider-control.input-descriptor.v1":
        fail("provider.task-input-invalid", "descriptor schema")
    if control["task_id"] != task_id or control["input_digest"] != input_digest:
        fail("provider.task-binding-mismatch", "input descriptor")
    total_bytes = _require_uint(control["total_bytes"], "total_bytes")
    chunk_bytes = _require_uint(control["chunk_bytes"], "chunk_bytes")
    chunk_count = _require_uint(control["chunk_count"], "chunk_count")
    if total_bytes > MAX_LOGICAL_TASK_INPUT:
        fail("provider.task-input-invalid", "logical input limit")
    if chunk_bytes < 1 or chunk_bytes > MAX_TASK_INPUT_CHUNK:
        fail("provider.task-input-invalid", "chunk payload limit")
    expected_count = 0 if total_bytes == 0 else (total_bytes + chunk_bytes - 1) // chunk_bytes
    if chunk_count != expected_count or chunk_count > MAX_TASK_INPUT_CHUNKS:
        fail("provider.task-input-invalid", "chunk count")

    chunks = value["chunks"]
    if not isinstance(chunks, list) or len(chunks) != chunk_count:
        fail("provider.protocol-state-invalid", "missing or extra input chunk")
    streamed = hashlib.sha256()
    offset = 0
    for index, occurrence in enumerate(chunks):
        if not isinstance(occurrence, dict):
            fail("provider.task-input-invalid", "chunk occurrence")
        _require_exact_keys(
            occurrence,
            {"sequence", "control", "payload", "payload_digest"},
            "chunk occurrence",
        )
        if _require_uint(occurrence["sequence"], "chunk sequence") != 4 + index:
            fail("provider.protocol-state-invalid", "input chunk sequence")
        chunk_control = occurrence["control"]
        if not isinstance(chunk_control, dict):
            fail("provider.task-input-invalid", "chunk control")
        _require_exact_keys(
            chunk_control,
            {
                "schema",
                "task_id",
                "input_digest",
                "chunk_index",
                "offset",
                "byte_count",
            },
            "chunk control",
        )
        if chunk_control["schema"] != "cxxlens.provider-control.input-chunk.v1":
            fail("provider.task-input-invalid", "chunk schema")
        if (
            chunk_control["task_id"] != task_id
            or chunk_control["input_digest"] != input_digest
        ):
            fail("provider.task-binding-mismatch", "input chunk")
        chunk_index = _require_uint(chunk_control["chunk_index"], "chunk_index")
        chunk_offset = _require_uint(chunk_control["offset"], "offset")
        byte_count = _require_uint(chunk_control["byte_count"], "byte_count")
        if chunk_index != index or chunk_offset != offset:
            fail("provider.protocol-state-invalid", "input chunk index or offset")
        expected_bytes = min(chunk_bytes, total_bytes - offset)
        if byte_count != expected_bytes or byte_count < 1:
            fail("provider.task-input-invalid", "input chunk byte_count")
        payload = occurrence["payload"]
        if not isinstance(payload, bytes) or len(payload) != byte_count:
            fail("provider.task-input-invalid", "input chunk payload length")
        if occurrence["payload_digest"] != byte_digest(payload):
            fail("provider.checksum-mismatch", "input chunk payload")
        streamed.update(payload)
        offset += byte_count

    if offset != total_bytes or "sha256:" + streamed.hexdigest() != input_digest:
        fail("provider.task-binding-mismatch", "terminal input seal")
    credit_sequence = _require_uint(value["credit_sequence"], "credit_sequence")
    close_sequence = _require_uint(value["close_sequence"], "close_sequence")
    if credit_sequence != 4 + chunk_count or close_sequence != 5 + chunk_count:
        fail("provider.protocol-state-invalid", "credit or close sequence")
    return {
        "task_id": task_id,
        "input_digest": input_digest,
        "total_bytes": total_bytes,
        "chunk_count": chunk_count,
        "sealed": True,
    }


def reference_bool_column_payload(values: list[bool]) -> bytes:
    """Independent little-endian reference for the v1 bool column payload."""
    if not values or len(values) > 256:
        fail("provider.columnar-invalid", "reference row bound")
    validity = bytearray((len(values) + 7) // 8)
    encoded_values = bytearray()
    for index, value in enumerate(values):
        validity[index // 8] |= 1 << (index % 8)
        encoded_values.append(1 if value else 0)
    unknown = bytes(len(validity))
    reason_offsets = bytes((len(values) + 1) * 4)
    sections = (
        bytes(validity),
        unknown,
        b"",
        bytes(encoded_values),
        reason_offsets,
        b"",
    )
    return (
        b"CXCC"
        + bytes((1, 0, 0, 0))
        + b"".join(struct.pack("<I", len(section)) for section in sections)
        + b"".join(sections)
    )


def validate_columnar_reference() -> None:
    expected = bytes.fromhex(
        "4358434301000000010000000100000000000000020000000c00000000000000"
        "03000100000000000000000000000000"
    )
    actual = reference_bool_column_payload([True, False])
    if actual != expected:
        fail("provider.columnar-reference-mismatch", actual.hex())
    sizes = struct.unpack("<6I", actual[8:32])
    if sizes != (1, 1, 0, 2, 12, 0) or sum(sizes) + 32 != len(actual):
        fail("provider.columnar-reference-mismatch", "section accounting")
    if actual[32] & 0xFC or actual[33] != 0 or actual[34:36] != b"\x01\x00":
        fail("provider.columnar-reference-mismatch", "validity/value ordering")


def _cbor_head(major: int, value: int) -> bytes:
    if value < 24:
        return bytes([(major << 5) | value])
    if value <= 0xFF:
        return bytes([(major << 5) | 24, value])
    if value <= 0xFFFF:
        return bytes([(major << 5) | 25]) + value.to_bytes(2, "big")
    if value <= 0xFFFFFFFF:
        return bytes([(major << 5) | 26]) + value.to_bytes(4, "big")
    return bytes([(major << 5) | 27]) + value.to_bytes(8, "big")


def cbor_encode(value: Any) -> bytes:
    if value is None:
        return b"\xf6"
    if value is False:
        return b"\xf4"
    if value is True:
        return b"\xf5"
    if isinstance(value, int):
        return _cbor_head(0, value) if value >= 0 else _cbor_head(1, -1 - value)
    if isinstance(value, bytes):
        return _cbor_head(2, len(value)) + value
    if isinstance(value, str):
        raw = value.encode("utf-8", errors="strict")
        return _cbor_head(3, len(raw)) + raw
    if isinstance(value, list):
        return _cbor_head(4, len(value)) + b"".join(cbor_encode(item) for item in value)
    if isinstance(value, dict):
        rows = [(cbor_encode(key), cbor_encode(item)) for key, item in value.items()]
        rows.sort(key=lambda row: (len(row[0]), row[0]))
        return _cbor_head(5, len(rows)) + b"".join(key + item for key, item in rows)
    fail("provider.cbor-type-unsupported", type(value).__name__)


def _cbor_argument(data: bytes, offset: int, additional: int) -> tuple[int, int]:
    if additional < 24:
        return additional, offset
    sizes = {24: 1, 25: 2, 26: 4, 27: 8}
    if additional not in sizes:
        fail("provider.malformed-frame", "indefinite or reserved CBOR argument")
    size = sizes[additional]
    if offset + size > len(data):
        fail("provider.truncated-stream", "CBOR argument")
    value = int.from_bytes(data[offset : offset + size], "big")
    if value < (24 if size == 1 else 1 << (8 * (size // 2))):
        fail("provider.malformed-frame", "non-shortest CBOR argument")
    return value, offset + size


def _cbor_parse(data: bytes, offset: int, depth: int = 0) -> tuple[Any, int]:
    if depth > 64 or offset >= len(data):
        fail("provider.truncated-stream", "CBOR value")
    initial = data[offset]
    offset += 1
    major, additional = initial >> 5, initial & 31
    if major == 7:
        if initial == 0xF4:
            return False, offset
        if initial == 0xF5:
            return True, offset
        if initial == 0xF6:
            return None, offset
        fail("provider.malformed-frame", "CBOR float/simple/tag unsupported")
    argument, offset = _cbor_argument(data, offset, additional)
    if major == 0:
        return argument, offset
    if major == 1:
        return -1 - argument, offset
    if major in (2, 3):
        if argument > MAX_CONTROL or offset + argument > len(data):
            fail("provider.truncated-stream", "CBOR bytes/text")
        raw = data[offset : offset + argument]
        if major == 2:
            return raw, offset + argument
        try:
            return raw.decode("utf-8", errors="strict"), offset + argument
        except UnicodeDecodeError as error:
            fail("provider.malformed-frame", f"invalid UTF-8: {error}")
    if major == 4:
        values = []
        for _ in range(argument):
            item, offset = _cbor_parse(data, offset, depth + 1)
            values.append(item)
        return values, offset
    if major == 5:
        value: dict[Any, Any] = {}
        encoded_keys: list[bytes] = []
        for _ in range(argument):
            start = offset
            key, offset = _cbor_parse(data, offset, depth + 1)
            encoded = data[start:offset]
            try:
                duplicate = key in value
            except TypeError:
                fail("provider.malformed-frame", "unhashable CBOR map key")
            if duplicate:
                fail("provider.malformed-frame", "duplicate CBOR map key")
            item, offset = _cbor_parse(data, offset, depth + 1)
            try:
                value[key] = item
            except TypeError:
                fail("provider.malformed-frame", "unhashable CBOR map key")
            encoded_keys.append(encoded)
        if encoded_keys != sorted(encoded_keys, key=lambda row: (len(row), row)):
            fail("provider.malformed-frame", "noncanonical CBOR map order")
        return value, offset
    fail("provider.malformed-frame", "CBOR tag unsupported")


def cbor_decode(data: bytes) -> Any:
    value, offset = _cbor_parse(data, 0)
    if offset != len(data) or cbor_encode(value) != data:
        fail("provider.malformed-frame", "noncanonical or trailing CBOR")
    return value


def _wire_flag(name: str) -> int:
    """Return the Protocol 2 wire flag assignment."""

    # These values are wire-format facts, not implementation-byte bindings.
    return {
        "required_extension": 1,
        "optional_extension": 2,
        "compressed_payload": 4,
        "end_of_stream": 8,
    }[name]


def encode_frame(
    control: Any,
    payload: bytes = b"",
    *,
    message_type: int = 1,
    flags: int = 0,
    stream_id: int = 0,
    sequence: int = 0,
    protocol_major: int = PROTOCOL_MAJOR,
    protocol_minor: int = PROTOCOL_MINOR,
) -> bytes:
    control_bytes = cbor_encode(control)
    if len(control_bytes) > MAX_CONTROL or len(payload) > MAX_PAYLOAD:
        fail("provider.output-limit", "frame encode limit")
    return FRAME.pack(
        b"CXXP",
        protocol_major,
        protocol_minor,
        message_type,
        flags,
        stream_id,
        sequence,
        len(control_bytes),
        len(payload),
        hashlib.sha256(control_bytes).digest(),
        hashlib.sha256(payload).digest(),
    ) + control_bytes + payload


def _known_message_ids(contract: dict[str, Any]) -> set[int]:
    return {row["id"] for row in contract["message_types"]["registry"]}


def decode_frame(
    contract: dict[str, Any],
    data: bytes,
    *,
    negotiated_minor: int = 0,
) -> dict[str, Any]:
    if len(data) < FRAME.size:
        fail("provider.truncated-stream", "fixed header")
    fields = FRAME.unpack(data[: FRAME.size])
    magic, major, minor, message_type, flags, stream, sequence = fields[:7]
    control_length, payload_length, control_hash, payload_hash = fields[7:]
    if magic != b"CXXP":
        fail("provider.malformed-frame", "magic")
    compatibility = contract["compatibility"]
    expected_major = int(compatibility["accepted_major"])
    expected_minor = int(compatibility["accepted_minor"])
    if major != expected_major:
        fail("provider.protocol-major-mismatch", str(major))
    if minor != expected_minor or negotiated_minor != expected_minor:
        fail("provider.protocol-minor-mismatch", str(minor))
    known_flags = 0
    # Bits 1 and 2 are retained as required/optional extension channels so
    # checksum/length accounting remains fail-closed; they are not advertised
    # as supported Protocol 2 flags.
    known_flags = (
        _wire_flag("end_of_stream")
        | _wire_flag("required_extension")
        | _wire_flag("optional_extension")
    )
    if flags & ~known_flags:
        if flags & _wire_flag("required_extension"):
            fail("provider.unknown-required-extension", str(flags))
        fail("provider.invalid-frame-flags", str(flags))
    if control_length > MAX_CONTROL or payload_length > MAX_PAYLOAD:
        fail("provider.output-limit", "declared frame length")
    total = FRAME.size + control_length + payload_length
    if len(data) != total:
        fail("provider.truncated-stream", "frame body")
    control = data[FRAME.size : FRAME.size + control_length]
    payload = data[FRAME.size + control_length :]
    if hashlib.sha256(control).digest() != control_hash:
        fail("provider.checksum-mismatch", "control")
    if hashlib.sha256(payload).digest() != payload_hash:
        fail("provider.checksum-mismatch", "payload")
    decoded = cbor_decode(control)
    required = flags & _wire_flag("required_extension")
    optional = flags & _wire_flag("optional_extension")
    compressed = flags & _wire_flag("compressed_payload")
    eos = flags & _wire_flag("end_of_stream")
    if required and optional:
        fail("provider.invalid-frame-flags", "conflicting extension flags")
    if compressed:
        fail("provider.unsupported-compression", "no negotiated codec")
    if required:
        fail("provider.unknown-required-extension", str(message_type))
    known_message = message_type in _known_message_ids(contract)
    if not known_message and not optional:
        fail("provider.unknown-message-type", str(message_type))
    if known_message and optional:
        fail("provider.invalid-frame-flags", "optional flag on base message")
    if optional and eos:
        fail("provider.invalid-frame-flags", "optional extension cannot terminate")
    result = {
        "protocol_major": major,
        "protocol_minor": minor,
        "message_type": message_type,
        "flags": flags,
        "stream_id": stream,
        "sequence": sequence,
        "control": decoded,
        "payload_hex": payload.hex(),
    }
    if not known_message:
        result["skipped_optional"] = True
        result["accounted_bytes"] = len(data)
    return result


def _replace_frame_lengths(
    data: bytes, *, control_length: int | None = None, payload_length: int | None = None
) -> bytes:
    fields = list(FRAME.unpack(data[: FRAME.size]))
    if control_length is not None:
        fields[7] = control_length
    if payload_length is not None:
        fields[8] = payload_length
    return FRAME.pack(*fields) + data[FRAME.size :]


def _frame_with_raw_control(control: bytes, message_type: int = 1, flags: int = 0) -> bytes:
    payload = b""
    return FRAME.pack(
        b"CXXP", PROTOCOL_MAJOR, PROTOCOL_MINOR, message_type, flags, 0, 0, len(control), 0,
        hashlib.sha256(control).digest(), hashlib.sha256(payload).digest(),
    ) + control


def mutate_frame(mutation: str) -> bytes:
    base = encode_frame({"message": "hello", "value": 1}, b"data")
    if mutation == "truncate-fixed-header":
        return base[:20]
    if mutation == "replace-magic":
        return b"BAD!" + base[4:]
    if mutation == "oversized-control-length":
        return _replace_frame_lengths(base, control_length=MAX_CONTROL + 1)
    if mutation == "oversized-payload-length":
        return _replace_frame_lengths(base, payload_length=MAX_PAYLOAD + 1)
    if mutation == "corrupt-control-checksum":
        return base[:40] + bytes([base[40] ^ 1]) + base[41:]
    if mutation == "corrupt-payload-checksum":
        return base[:72] + bytes([base[72] ^ 1]) + base[73:]
    if mutation == "truncate-control":
        control_length = FRAME.unpack(base[: FRAME.size])[7]
        return base[: FRAME.size + control_length - 1]
    if mutation == "truncate-payload":
        return base[:-1]
    if mutation == "noncanonical-cbor-integer":
        return _frame_with_raw_control(b"\x18\x01")
    if mutation == "duplicate-cbor-key":
        return _frame_with_raw_control(b"\xa2\x61a\x01\x61a\x02")
    if mutation == "indefinite-cbor":
        return _frame_with_raw_control(b"\x9f\x01\xff")
    if mutation == "unknown-required-message":
        return encode_frame({}, message_type=65000, flags=1)
    invalid_utf8 = {
        "utf8-isolated-continuation": b"\x61\x80",
        "utf8-truncated-sequence": b"\x61\xc2",
        "utf8-overlong-2": b"\x62\xc0\x80",
        "utf8-overlong-3": b"\x63\xe0\x80\x80",
        "utf8-overlong-4": b"\x64\xf0\x80\x80\x80",
        "utf8-surrogate": b"\x63\xed\xa0\x80",
        "utf8-out-of-range": b"\x64\xf4\x90\x80\x80",
    }
    if mutation in invalid_utf8:
        return _frame_with_raw_control(invalid_utf8[mutation])
    fail("provider.fuzz-mutation-unknown", mutation)

def sample_manifest() -> dict[str, Any]:
    return {
        "schema": "cxxlens.provider-manifest.v1",
        "provider_id": "provider.cc.clang22",
        "provider_version": "1.0.0",
        "package_identity": "pkg:cxxlens/provider-cc-clang22@1.0.0",
        "provider_binary_digest": "sha256:" + "a" * 64,
        "provider_semantic_contract_digest": "sha256:" + "b" * 64,
        "publisher": "cxxlens.project",
        "license": "Apache-2.0 WITH LLVM-exception",
        "signature": None,
        "protocol_range": {"major": PROTOCOL_MAJOR, "minimum_minor": PROTOCOL_MINOR, "maximum_minor": PROTOCOL_MINOR, "required_features": ["streaming"], "optional_features": ["resume"]},
        "platform_tuples": ["linux-x86_64"],
        "offered_relations": ["cc.entity.v1"],
        "required_relations": [],
        "interpretation_domains": ["cc.canonical-1"],
        "invalidation_contract": "sha256:" + "c" * 64,
        "determinism_contract": "sha256:" + "d" * 64,
        "resource_class": "frontend-medium",
        "sandbox_minimum": "process-isolated",
        "requested_qualifications": ["schema-conformant"],
        "trust_flags": ["requires-trusted-registry-for-standard-authority"],
        "task_stage": {"input": "observation", "output": "assertion"},
    }


def validate_task_input_authority(contract: dict[str, Any]) -> None:
    compatibility = contract.get("compatibility", {})
    if any(
        compatibility.get(field) != expected
        for field, expected in (
            ("protocol_major", PROTOCOL_MAJOR),
            ("protocol_minor", PROTOCOL_MINOR),
            ("accepted_major", PROTOCOL_MAJOR),
            ("accepted_minor", PROTOCOL_MINOR),
        )
    ):
        fail("provider.task-input-authority-invalid", "current protocol version")
    if compatibility.get("downgrade") != "reject":
        fail("provider.task-input-authority-invalid", "downgrade policy")
    if compatibility.get("legacy_protocol_1") != "rejected-before-payload":
        fail("provider.task-input-authority-invalid", "legacy protocol policy")
    if compatibility.get("legacy_request_2_1_task_v3") != "rejected-before-payload":
        fail("provider.task-input-authority-invalid", "legacy request/task policy")
    capabilities = contract.get("capabilities", {})
    for feature in (
		"task-input-chunks-v2",
        "durable-resume-token",
        "heartbeat",
        "progress-rate-enforcement",
        "spill-staging",
        "long-run-fault-recovery",
    ):
        if capabilities.get(feature) not in {"required", "required-for-NG1"}:
            fail("provider.task-input-authority-invalid", feature)

    limits = contract["wire"]["limits"]
    if limits.get("control_bytes") != MAX_CONTROL or limits.get("payload_bytes") != MAX_PAYLOAD:
        fail("provider.task-input-authority-invalid", "wire limits")
    if limits.get("closure_chunk_bytes") != MAX_TASK_INPUT_CHUNK:
        fail("provider.task-input-authority-invalid", "closure chunk limit")
    transfer = contract["request_task"]
    if transfer.get("request_version") != "2.2.0":
        fail("provider.task-input-authority-invalid", "request version")
    if transfer.get("task_schema") != "cxxlens.clang22.task.v4":
        fail("provider.task-input-authority-invalid", "task version")
    if transfer.get("source_bytes_in_request") != "forbidden":
        fail("provider.task-input-authority-invalid", "source bytes in request")
    if transfer.get("content_base64") != "forbidden":
        fail("provider.task-input-authority-invalid", "source Base64 in request")

    closure = contract.get("source_closure_transport", {})
    if closure.get("ambient_filesystem_fallback") != "forbidden":
        fail("provider.task-input-authority-invalid", "ambient filesystem")
    bounds = closure.get("bounds", {})
    if bounds.get("chunk_bytes") != MAX_TASK_INPUT_CHUNK:
        fail("provider.task-input-authority-invalid", "source closure chunk bound")
    if closure.get("complete_closure_memory_copy") != "forbidden":
        fail("provider.task-input-authority-invalid", "closure memory bound")


def validate_contract_shape(contract: dict[str, Any]) -> None:
    validate_columnar_reference()
    validate_task_input_authority(contract)
    validate_shared_coverage_authority(contract)
    if FRAME.size != 104 or contract["wire"]["fixed_header_bytes"] != FRAME.size:
        fail("provider.wire-header-size-invalid", str(FRAME.size))
    if sum(row["bytes"] for row in contract["wire"]["fixed_header_fields"]) != FRAME.size:
        fail("provider.wire-header-layout-invalid", "field bytes")
    rows = contract["message_types"]["registry"]
    if not rows or len({row["id"] for row in rows}) != len(rows):
        fail("provider.message-registry-invalid", "message IDs")
    if {row["id"] for row in rows} != set(range(1, 30)):
        fail("provider.message-registry-invalid", "Protocol 2 message IDs")
    if contract["message_types"].get("unknown_required") != "reject":
        fail("provider.message-registry-invalid", "unknown required message policy")
    if contract["message_types"].get("unknown_optional") != "skip-and-account":
        fail("provider.message-registry-invalid", "unknown optional message policy")
    if contract.get("ng1", {}).get("heartbeat_message_id") != 23:
        fail("provider.ng1-heartbeat-invalid", "heartbeat message ID")
    if contract.get("ng1", {}).get("resume_token_cross_use") != "forbidden":
        fail("provider.ng1-resume-invalid", "resume token cross-use")
    wire = contract["wire"]
    if wire.get("magic_ascii") != "CXXP" or wire.get("fixed_header_bytes") != FRAME.size:
        fail("provider.wire-header-size-invalid", "Protocol 2 wire header")
    if wire.get("checksums") != "independent-full-sha256":
        fail("provider.checksum-policy-invalid", "Protocol 2 checksums")
    encoding = wire.get("control_encoding", {})
    for field, expected in {
        "standard": "RFC-8949",
        "mode": "deterministic-closed-subset",
        "duplicate_map_key": "reject",
        "indefinite_length": "reject",
        "floats": "reject",
        "tags": "reject",
        "invalid_utf8": "reject",
    }.items():
        if encoding.get(field) != expected:
            fail("provider.control-encoding-invalid", field)
    if wire.get("flags", {}).get("supported") != ["end-of-stream"]:
        fail("provider.flags-invalid", "Protocol 2 supported flags")


def validate_ng1_v2_contract(contract: dict[str, Any]) -> None:
    """Check NG1 product invariants without a repository-operation census."""

    ng1 = contract.get("ng1")
    if not isinstance(ng1, dict):
        fail("provider.ng1-contract-invalid", "NG1 section")
    if ng1.get("heartbeat_message_id") != 23:
        fail("provider.ng1-heartbeat-invalid", "heartbeat message ID")
    if ng1.get("heartbeat_clock") != "host-injected-monotonic-only":
        fail("provider.ng1-heartbeat-invalid", "heartbeat clock")
    if ng1.get("resume_token_cross_use") != "forbidden":
        fail("provider.ng1-resume-invalid", "resume token cross-use")
    if ng1.get("stale_or_foreign_token") != "typed-reject":
        fail("provider.ng1-resume-invalid", "stale token policy")
    if ng1.get("worker_crash") != (
        "kill-process-group-cleanup-private-spool-no-publication"
    ):
        fail("provider.ng1-crash-invalid", "worker crash effect")


def validate_all(root: pathlib.Path) -> tuple[dict[str, Any], list[dict[str, Any]], int, int]:
    contract = load_yaml(root / CONTRACT)
    if contract.get("schema") != "cxxlens.provider-protocol.v2":
        fail("provider.protocol-authority-invalid", "Protocol 2 contract is not selected")
    schema_validate(
        contract,
        load_yaml(root / CONTRACT_SCHEMA),
        "Protocol 2 provider contract",
    )
    validate_ng1_v2_contract(contract)
    schema_validate(sample_manifest(), load_yaml(root / MANIFEST_SCHEMA), "provider manifest")
    validate_contract_shape(contract)
    # Exercise the current wire registry directly; no external compatibility
    # corpus is loaded as an authority.
    results: list[dict[str, Any]] = []
    for row in contract["message_types"]["registry"]:
        message_type = row["id"]
        frame = encode_frame(
            {"message_type": message_type},
            message_type=message_type,
            sequence=message_type,
        )
        decoded = decode_frame(contract, frame)
        if (
            decoded["message_type"] != message_type
            or decoded["protocol_major"] != PROTOCOL_MAJOR
            or decoded["protocol_minor"] != PROTOCOL_MINOR
        ):
            fail("provider.wire-round-trip-invalid", str(message_type))
        results.append({"message_type": message_type, "round_trip": True})

    fault_mutations = {
        "truncate-fixed-header": "provider.truncated-stream",
        "replace-magic": "provider.malformed-frame",
        "oversized-control-length": "provider.output-limit",
        "oversized-payload-length": "provider.output-limit",
        "corrupt-control-checksum": "provider.checksum-mismatch",
        "corrupt-payload-checksum": "provider.checksum-mismatch",
        "truncate-control": "provider.truncated-stream",
        "truncate-payload": "provider.truncated-stream",
        "noncanonical-cbor-integer": "provider.malformed-frame",
        "duplicate-cbor-key": "provider.malformed-frame",
        "indefinite-cbor": "provider.malformed-frame",
        "unknown-required-message": "provider.unknown-required-extension",
    }
    faults = 0
    for mutation, expected_reason in fault_mutations.items():
        try:
            decode_frame(contract, mutate_frame(mutation))
        except ProviderContractError as error:
            if error.code != expected_reason:
                fail("provider.wire-fault-reason-mismatch", mutation)
            faults += 1
        else:
            fail("provider.wire-fault-accepted", mutation)
    return contract, results, len(results), faults


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    args = parser.parse_args()
    contract, results, comparisons, faults = validate_all(args.root.resolve())
    print(
        f"verified Protocol 2 provider contract: {len(results)} message round trips, "
        f"{faults} wire faults, {comparisons} registry entries, {digest(contract)}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ProviderContractError) as error:
        print(f"provider protocol failure: {error}", file=sys.stderr)
        raise SystemExit(1) from error
