#!/usr/bin/env python3
"""Validate the NG process provider runtime and Clang 22 worker contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys
from collections.abc import Iterable
from typing import Any

import jsonschema
import yaml

from check_ng_provider_protocol import (
    FRAME,
    ProviderContractError,
    decode_frame,
    encode_frame,
    validate_shared_coverage_authority,
    validate_shared_coverage_records,
)


class ContractError(ValueError):
    pass


WORKER_TASK_CODEC_V3 = "cxxlens.clang22.task.v3"
LEGACY_WORKER_TASK_CODEC_V2 = "cxxlens.clang22.task.v2"
SAME_PASS_MARKER = "same-shared-validation-pass-that-constructs-immutable-seal"
RECEIPT_FIELDS = [
    "raw_stdout_byte_count",
    "raw_stdout_sha256",
    "decoded_frame_count",
    "frame_transcript_digest",
    "sealed_transcript_digest",
]
FRAME_TRANSCRIPT_FIELDS = [
    "protocol_major",
    "protocol_minor",
    "flags",
    "message_type",
    "stream_id",
    "sequence",
    "control_digest",
    "payload_digest",
]
SEALED_TRANSCRIPT_FIELDS = [
    "task_id",
    "terminal",
    "batches",
    "coverage_records",
    "unresolved_records",
    "evidence_records",
]
SEALED_BATCH_FIELDS = [
    "task_id",
    "descriptor_id",
    "descriptor_digest",
    "dependency_group_id",
    "atomic_output_group_id",
    "batch_id",
    "batch_digest",
    "ordered_chunk_digests",
    "row_canonical_forms",
]
AUTHORIZED_BATCH_FIELDS = [
    "descriptor_id",
    "descriptor_digest",
    "dependency_group_id",
    "atomic_output_group_id",
    "batch_id",
    "columns",
]
AUTHORIZED_COLUMN_FIELDS = ["id", "type", "required"]
EXPECTED_PROVIDER_IDENTITY_FIELDS = [
    "provider_id",
    "provider_version",
    "provider_binary_digest",
    "provider_semantic_contract_digest",
    "protocol_major",
    "protocol_minor",
    "required_features",
    "sandbox_policy_digest",
    "offered_relations",
]
CHUNK_CONTROL_FIELDS = [
    "task_id",
    "dependency_group_id",
    "atomic_output_group_id",
    "batch_id",
    "descriptor_id",
    "descriptor_digest",
    "column_id",
    "row_offset",
    "row_count",
    "chunk_index",
    "encoding",
    "payload_digest",
    "chunk_digest",
]
BATCH_END_CONTROL_FIELDS = [
    "task_id",
    "dependency_group_id",
    "atomic_output_group_id",
    "batch_id",
    "descriptor_id",
    "descriptor_digest",
    "row_count",
    "column_count",
    "chunk_count",
    "batch_digest",
]
COVERAGE_FIELDS = ["kind", "id", "state", "reason"]
UNRESOLVED_FIELDS = ["code", "subject", "detail"]
EVIDENCE_FIELDS = ["kind", "subject", "producer", "summary"]
MAX_RUNTIME_RECEIPT_STDOUT_BYTES = 1 << 30
MAX_RUNTIME_RECEIPT_FRAMES = 4096
MAX_RUNTIME_RECEIPT_ROWS = 1 << 20
SCALAR_KINDS = {
    "bool": 0,
    "int64": 1,
    "uint64": 2,
    "utf8_string": 3,
    "bytes": 4,
    "digest": 5,
    "semantic_version": 6,
    "typed_id": 7,
    "open_symbol": 8,
    "condition_ref": 9,
    "source_span_id": 10,
    "evidence_id": 11,
    "closed_symbol": 12,
    "set": 13,
}
RUNTIME_PRIVATE_RECEIPT_AUTHORITY = {
    "visibility": "runtime-private",
    "exact_fields": RECEIPT_FIELDS,
    "additional_fields": "reject",
    "construction": SAME_PASS_MARKER,
    "verification_source": (
        "runtime-observed-raw-bytes-decoded-frame-values-and-immutable-seal-only"
    ),
    "report-supplied-digest-or-self-consistency": "forbidden",
    "expected_provider_identity": {
        "exact_fields": EXPECTED_PROVIDER_IDENTITY_FIELDS,
        "source": (
            "independently-validated-selected-manifest-session-and-launcher-authority-"
            "never-provider-stdout"
        ),
        "hello": "exact-closed-projection-before-task-acceptance",
        "task_accepted": "exact-provider-id-version-cross-binding",
    },
    "raw_stdout": {
        "observation_order": "before-frame-decode-or-move",
        "byte_count": "exact-uint64",
        "digest": "sha256-of-exact-observed-bytes",
    },
    "frame_transcript": {
        "domain": "cxxlens.provider-frame-transcript.v2",
        "digest": "cxxlens-semantic-digest-v2",
        "encoding": "cxxlens-canonical-tuple-v1",
        "exact_frame_fields": FRAME_TRANSCRIPT_FIELDS,
        "order": "decoded-wire-order-with-explicit-count",
        "source": "same-decoded-frame-values-consumed-by-shared-validator",
    },
    "sealed_transcript": {
        "domain": "cxxlens.provider-sealed-transcript.v1",
        "digest": "cxxlens-semantic-digest-v2",
        "encoding": "cxxlens-canonical-tuple-v1",
        "exact_fields": SEALED_TRANSCRIPT_FIELDS,
        "exact_batch_fields": SEALED_BATCH_FIELDS,
        "exact_coverage_fields": COVERAGE_FIELDS,
        "exact_unresolved_fields": UNRESOLVED_FIELDS,
        "exact_evidence_fields": EVIDENCE_FIELDS,
        "ordering": [
            "batch-seal-order",
            "row-order",
            "retained-coverage-wire-order",
            "retained-unresolved-wire-order",
            "retained-evidence-wire-order",
        ],
        "source": "immutable-value-owned-seal-from-same-shared-validation-pass",
    },
    "public_process_execution_report_semantic_digest": {
        "role": "diagnostic-public-report-identity-only",
        "alias_for_any_receipt_field": False,
        "raw-frame-frame-transcript-sealed-transcript-or-adoption-authority": False,
    },
}


def validate_task_codec_markers(codec: str) -> None:
    if codec.count(WORKER_TASK_CODEC_V3) != 1:
        raise ContractError(
            "Clang 22 task codec must bind exactly one installed task.v3 codec marker"
        )
    if LEGACY_WORKER_TASK_CODEC_V2 in codec:
        raise ContractError("legacy Clang 22 task.v2 codec remains adoptable")


def load(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ContractError(f"expected mapping: {path}")
    return value


def _length(value: int) -> bytes:
    return value.to_bytes(8, byteorder="big", signed=False)


def _canonical_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return b"\x04" + _length(len(encoded)) + encoded


def _canonical_bytes(value: bytes) -> bytes:
    return b"\x03" + _length(len(value)) + value


def _canonical_integer(value: int) -> bytes:
    if isinstance(value, bool) or value < 0 or value > (1 << 64) - 1:
        raise ContractError("runtime receipt integer is outside uint64")
    magnitude = value
    width = max(1, (magnitude.bit_length() + 7) // 8)
    return (
        b"\x02"
        + b"\x00"
        + _length(width)
        + magnitude.to_bytes(width, byteorder="big", signed=False)
    )


def _canonical_tuple(values: Iterable[bytes]) -> bytes:
    items = list(values)
    output = bytearray(b"\x05" + _length(len(items)))
    for item in items:
        output.extend(_length(len(item)))
        output.extend(item)
    return bytes(output)


def _canonical_value(value: Any) -> bytes:
    if isinstance(value, str):
        return _canonical_string(value)
    if isinstance(value, int) and not isinstance(value, bool):
        return _canonical_integer(value)
    if isinstance(value, bytes):
        return _canonical_bytes(value)
    if isinstance(value, list):
        return _canonical_tuple(_canonical_value(item) for item in value)
    raise ContractError(f"runtime receipt projection type is unsupported: {type(value)}")


def _semantic_digest(domain: str, projection: bytes) -> str:
    framed = _canonical_tuple(
        (
            _canonical_string("cxxlens-semantic-digest-v2"),
            _canonical_string(domain),
            _canonical_bytes(projection),
        )
    )
    return "semantic-v2:sha256:" + hashlib.sha256(framed).hexdigest()


def _require_exact_mapping(value: Any, fields: list[str], label: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != set(fields):
        raise ContractError(f"{label} exact projection is not closed")
    return value


def _require_text_record(value: Any, fields: list[str], label: str) -> dict[str, str]:
    record = _require_exact_mapping(value, fields, label)
    if not all(isinstance(record[field], str) for field in fields):
        raise ContractError(f"{label} contains a non-text field")
    return record


def _decode_provider_stdout(
    protocol: dict[str, Any], raw_stdout: bytes
) -> list[dict[str, Any]]:
    """Bound and decode the exact provider stdout occurrence once, in wire order."""

    if not isinstance(raw_stdout, bytes) or not raw_stdout:
        raise ContractError("raw provider stdout is not a nonempty exact byte occurrence")
    if len(raw_stdout) > MAX_RUNTIME_RECEIPT_STDOUT_BYTES:
        raise ContractError("raw provider stdout exceeds the receipt decode bound")
    decoded: list[dict[str, Any]] = []
    offset = 0
    while offset < len(raw_stdout):
        if len(decoded) >= MAX_RUNTIME_RECEIPT_FRAMES:
            raise ContractError("raw provider stdout exceeds the frame-count bound")
        remaining = len(raw_stdout) - offset
        if remaining < FRAME.size:
            raise ContractError("raw provider stdout has a truncated fixed header")
        header = FRAME.unpack(raw_stdout[offset : offset + FRAME.size])
        control_length = header[7]
        payload_length = header[8]
        if (
            control_length > protocol["wire"]["limits"]["control_bytes"]
            or payload_length > protocol["wire"]["limits"]["payload_bytes"]
        ):
            raise ContractError("raw provider stdout frame exceeds negotiated limits")
        frame_bytes = FRAME.size + control_length + payload_length
        if frame_bytes > remaining:
            raise ContractError("raw provider stdout has a truncated frame body")
        occurrence = raw_stdout[offset : offset + frame_bytes]
        try:
            frame = decode_frame(protocol, occurrence, negotiated_minor=1)
        except ProviderContractError as error:
            raise ContractError(f"raw provider stdout decode failed: {error}") from error
        frame["control_digest"] = "sha256:" + header[9].hex()
        frame["payload_digest"] = "sha256:" + header[10].hex()
        frame["payload"] = bytes.fromhex(frame.pop("payload_hex"))
        decoded.append(frame)
        offset += frame_bytes
    return decoded


def _single_control_record(
    control: Any, schema: str, fields: list[str], label: str
) -> dict[str, str]:
    expected = ["schema", *fields]
    record = _require_exact_mapping(control, expected, label)
    if record["schema"] != schema or not all(
        isinstance(record[field], str) for field in fields
    ):
        raise ContractError(f"{label} schema or field type differs")
    return {field: record[field] for field in fields}


def _indexed_control_records(
    control: Any, schema: str, fields: list[str], label: str
) -> list[dict[str, str]]:
    if not isinstance(control, dict) or control.get("schema") != schema:
        raise ContractError(f"{label} record-set schema differs")
    count = control.get("record_count")
    if isinstance(count, bool) or not isinstance(count, int) or not 0 <= count <= 4096:
        raise ContractError(f"{label} record count is invalid")
    expected = {"schema", "record_count"}
    expected.update(f"{index}.{field}" for index in range(count) for field in fields)
    if set(control) != expected:
        raise ContractError(f"{label} indexed record projection is not closed")
    records: list[dict[str, str]] = []
    rows: list[list[str]] = []
    for index in range(count):
        row = [control[f"{index}.{field}"] for field in fields]
        if not all(isinstance(value, str) for value in row):
            raise ContractError(f"{label} record contains a non-text field")
        rows.append(row)
        records.append(dict(zip(fields, row, strict=True)))
    if rows != sorted(rows) or len(rows) != len({tuple(row) for row in rows}):
        raise ContractError(f"{label} record order or identity is noncanonical")
    return records


def _canonical_digest(value: Any) -> bool:
    return isinstance(value, str) and re.fullmatch(
        r"(?:sha256|semantic-v2:sha256):[0-9a-f]{64}", value
    ) is not None


def _uint64(value: Any, label: str, maximum: int = (1 << 64) - 1) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or value < 0
        or value > maximum
    ):
        raise ContractError(f"{label} is not a bounded unsigned integer")
    return value


def _parse_authorized_type(type_name: Any) -> tuple[str, int, bool]:
    if not isinstance(type_name, str) or not type_name:
        raise ContractError("authorized column type is invalid")
    optional = type_name.startswith("optional<") and type_name.endswith(">")
    base = type_name[len("optional<") : -1] if optional else type_name
    if "<" in base:
        scalar, separator, parameter = base.partition("<")
        if not separator or not parameter.endswith(">"):
            raise ContractError("authorized parameterized column type is malformed")
        parameter = parameter[:-1]
    else:
        scalar, parameter = base, None
    if re.fullmatch(r"[a-z][a-z0-9_]*", scalar) is None or scalar not in SCALAR_KINDS:
        raise ContractError("authorized column type is outside the closed scalar registry")
    if scalar in {"typed_id", "open_symbol", "closed_symbol", "set"}:
        if not parameter:
            raise ContractError("authorized parameterized column type has no parameter")
    elif parameter is not None:
        raise ContractError("authorized scalar column has an unexpected parameter")
    return base, SCALAR_KINDS[scalar], optional


def _authorized_batch_index(value: Any) -> dict[str, dict[str, Any]]:
    if value is None:
        value = []
    if not isinstance(value, list) or len(value) > MAX_RUNTIME_RECEIPT_FRAMES:
        raise ContractError("authorized batch authority is not a bounded list")
    indexed: dict[str, dict[str, Any]] = {}
    for item in value:
        batch = _require_exact_mapping(item, AUTHORIZED_BATCH_FIELDS, "authorized batch")
        for field in AUTHORIZED_BATCH_FIELDS[:-1]:
            if not isinstance(batch[field], str) or not batch[field] or "\0" in batch[field]:
                raise ContractError(f"authorized batch {field} is invalid")
        if not _canonical_digest(batch["descriptor_digest"]):
            raise ContractError("authorized batch descriptor digest is not canonical")
        columns = batch["columns"]
        if not isinstance(columns, list) or not columns or len(columns) > 4096:
            raise ContractError("authorized descriptor columns are not a bounded nonempty list")
        normalized_columns: list[dict[str, Any]] = []
        column_ids: set[str] = set()
        for item_column in columns:
            column = _require_exact_mapping(
                item_column, AUTHORIZED_COLUMN_FIELDS, "authorized column"
            )
            if (
                not isinstance(column["id"], str)
                or not column["id"]
                or "\0" in column["id"]
                or column["id"] in column_ids
                or not isinstance(column["required"], bool)
            ):
                raise ContractError("authorized descriptor column identity differs")
            base, scalar_kind, optional = _parse_authorized_type(column["type"])
            if not column["required"] and not optional:
                raise ContractError("nonrequired authorized column is not optional")
            column_ids.add(column["id"])
            normalized_columns.append(
                {
                    **column,
                    "base_type": base,
                    "scalar_kind": scalar_kind,
                    "optional": optional,
                }
            )
        if batch["batch_id"] in indexed:
            raise ContractError("authorized batch identity is duplicated")
        indexed[batch["batch_id"]] = {**batch, "columns": normalized_columns}
    return indexed


def _expected_provider_identity(
    value: Any,
    authority_by_batch: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    """Validate the independent selected-manifest/session identity projection."""

    identity = _require_exact_mapping(
        value,
        EXPECTED_PROVIDER_IDENTITY_FIELDS,
        "expected provider identity",
    )
    for field in ("provider_id", "provider_version"):
        if (
            not isinstance(identity[field], str)
            or not identity[field]
            or "\0" in identity[field]
        ):
            raise ContractError(f"expected provider identity {field} is invalid")
    if re.fullmatch(
        r"[1-9][0-9]*\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)",
        identity["provider_version"],
    ) is None:
        raise ContractError("expected provider version is not canonical")
    for field in (
        "provider_binary_digest",
        "provider_semantic_contract_digest",
        "sandbox_policy_digest",
    ):
        if not isinstance(identity[field], str) or re.fullmatch(
            r"sha256:[0-9a-f]{64}", identity[field]
        ) is None:
            raise ContractError(f"expected provider identity {field} is not canonical")
    for field in ("protocol_major", "protocol_minor"):
        _uint64(identity[field], f"expected provider identity {field}", 65535)
    if identity["protocol_major"] != 1 or identity["protocol_minor"] != 1:
        raise ContractError("expected provider negotiated protocol is not exact 1.1")
    for field in ("required_features", "offered_relations"):
        values = identity[field]
        if (
            not isinstance(values, list)
            or not all(
                isinstance(item, str) and item and "\0" not in item for item in values
            )
            or values != sorted(set(values))
        ):
            raise ContractError(f"expected provider identity {field} is not canonical")
    if "task-input-chunks-v1" not in identity["required_features"]:
        raise ContractError("expected provider identity omits the negotiated input feature")
    authorized_descriptors = {
        authority["descriptor_id"] for authority in authority_by_batch.values()
    }
    if not authorized_descriptors.issubset(identity["offered_relations"]):
        raise ContractError("expected provider relation offers omit an authorized batch")
    return identity


def _digest_text_field(name: str, value: str) -> bytes:
    return _canonical_tuple((_canonical_string(name), _canonical_string(value)))


def _digest_u64_field(name: str, value: int) -> bytes:
    return _canonical_tuple(
        (_canonical_string(name), _canonical_bytes(value.to_bytes(8, "big")))
    )


def _digest_value_field(name: str, canonical_value: bytes) -> bytes:
    return _canonical_tuple((_canonical_string(name), canonical_value))


def _semantic_fields_digest(
    domain: str,
    schema: str,
    fields: list[tuple[str, str | int]],
    additional_fields: Iterable[bytes] = (),
) -> str:
    projection = [_digest_text_field("schema", schema)]
    for name, value in fields:
        projection.append(
            _digest_text_field(name, value)
            if isinstance(value, str)
            else _digest_u64_field(name, value)
        )
    projection.extend(additional_fields)
    return _semantic_digest(domain, _canonical_tuple(projection))


def _read_little(data: bytes, offset: int, width: int, label: str) -> int:
    if offset < 0 or width < 0 or offset > len(data) or len(data) - offset < width:
        raise ContractError(f"{label} is truncated")
    return int.from_bytes(data[offset : offset + width], "little")


def _strict_utf8(data: bytes, label: str) -> str:
    try:
        value = data.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise ContractError(f"{label} is not strict UTF-8") from error
    if any(0xD800 <= ord(character) <= 0xDFFF for character in value):
        raise ContractError(f"{label} contains a surrogate")
    return value


def _control_free(value: str) -> bool:
    return bool(value) and all(
        ord(character) >= 0x20 and ord(character) != 0x7F for character in value
    )


def _column_encoding(scalar_kind: int) -> str:
    if scalar_kind == SCALAR_KINDS["bool"]:
        return "fixed-width-bool-u8"
    if scalar_kind == SCALAR_KINDS["int64"]:
        return "fixed-width-i64-le"
    if scalar_kind == SCALAR_KINDS["uint64"]:
        return "fixed-width-u64-le"
    if scalar_kind in {SCALAR_KINDS["open_symbol"], SCALAR_KINDS["closed_symbol"]}:
        return "dictionary-index-u32-le"
    if scalar_kind in {SCALAR_KINDS["bytes"], SCALAR_KINDS["set"]}:
        return "bytes-offsets-u32-le"
    return "utf8-offsets-u32-le"


def _decode_offsets(
    encoded: bytes, data_size: int, entry_count: int, label: str
) -> list[int]:
    if len(encoded) != (entry_count + 1) * 4:
        raise ContractError(f"{label} offset table shape differs")
    values = [
        _read_little(encoded, index * 4, 4, label)
        for index in range(entry_count + 1)
    ]
    if values[0] != 0 or values[-1] != data_size or values != sorted(values):
        raise ContractError(f"{label} offsets are not canonical")
    return values


def _validate_set_bytes(value: bytes) -> None:
    offset = 0
    previous: bytes | None = None
    while offset < len(value):
        length = _read_little(value, offset, 4, "set value")
        offset += 4
        if length == 0 or length > len(value) - offset:
            raise ContractError("set value element length differs")
        item = value[offset : offset + length]
        _strict_utf8(item, "set value")
        if previous is not None and item <= previous:
            raise ContractError("set value order is not canonical")
        previous = item
        offset += length


def _validate_scalar(column: dict[str, Any], value: Any) -> None:
    base = column["base_type"]
    scalar = base.split("<", 1)[0]
    if scalar == "digest" and not _canonical_digest(value):
        raise ContractError("column digest value is not canonical")
    if scalar == "semantic_version" and (
        not isinstance(value, str)
        or re.fullmatch(
            r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)",
            value,
        )
        is None
    ):
        raise ContractError("column semantic version is not canonical")
    if scalar in {
        "typed_id",
        "open_symbol",
        "closed_symbol",
        "condition_ref",
        "source_span_id",
        "evidence_id",
    } and (not isinstance(value, str) or not _control_free(value)):
        raise ContractError("column identity or symbol contains control text")
    if scalar == "set":
        _validate_set_bytes(value)


def _decode_column_chunk(
    frame: dict[str, Any], column: dict[str, Any]
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    control = _require_exact_mapping(
        frame["control"], CHUNK_CONTROL_FIELDS, "column chunk control"
    )
    text_fields = [
        field
        for field in CHUNK_CONTROL_FIELDS
        if field not in {"row_offset", "row_count", "chunk_index"}
    ]
    if not all(isinstance(control[field], str) and control[field] for field in text_fields):
        raise ContractError("column chunk text binding differs")
    row_offset = _uint64(control["row_offset"], "column chunk row offset")
    row_count = _uint64(control["row_count"], "column chunk row count", (1 << 32) - 1)
    chunk_index = _uint64(control["chunk_index"], "column chunk index")
    if row_count == 0 or control["column_id"] != column["id"]:
        raise ContractError("column chunk column or row count differs")
    if control["encoding"] != _column_encoding(column["scalar_kind"]):
        raise ContractError("column chunk encoding differs from descriptor authority")
    if not all(
        _canonical_digest(control[field])
        for field in ("descriptor_digest", "payload_digest", "chunk_digest")
    ):
        raise ContractError("column chunk digest field is not canonical")

    payload = frame["payload"]
    if (
        len(payload) < 32
        or payload[:5] != b"CXCC\x01"
        or payload[5] != column["scalar_kind"]
        or payload[6:8] != b"\0\0"
    ):
        raise ContractError("column chunk payload header differs")
    sizes = [_read_little(payload, 8 + index * 4, 4, "column chunk") for index in range(6)]
    if 32 + sum(sizes) != len(payload):
        raise ContractError("column chunk section lengths differ")
    sections: list[bytes] = []
    offset = 32
    for size in sizes:
        sections.append(payload[offset : offset + size])
        offset += size

    bitmap_size = (row_count + 7) // 8
    scalar_kind = column["scalar_kind"]
    dictionary = control["encoding"] == "dictionary-index-u32-le"
    variable = not dictionary and scalar_kind not in {
        SCALAR_KINDS["bool"],
        SCALAR_KINDS["int64"],
        SCALAR_KINDS["uint64"],
    }
    fixed = not variable and not dictionary
    fixed_width = 1 if scalar_kind == SCALAR_KINDS["bool"] else 8
    if (
        len(sections[0]) != bitmap_size
        or len(sections[1]) != bitmap_size
        or (variable and len(sections[2]) != (row_count + 1) * 4)
        or (fixed and sections[2])
        or (fixed and len(sections[3]) != row_count * fixed_width)
        or (dictionary and len(sections[2]) < 8)
        or (dictionary and len(sections[3]) != row_count * 4)
        or len(sections[4]) != (row_count + 1) * 4
    ):
        raise ContractError("column chunk payload shape differs")
    unused_bits = row_count % 8
    if unused_bits:
        used_mask = (1 << unused_bits) - 1
        if sections[0][-1] & ~used_mask or sections[1][-1] & ~used_mask:
            raise ContractError("column chunk unused bitmap bits are nonzero")

    value_offsets = (
        _decode_offsets(sections[2], len(sections[3]), row_count, "column value")
        if variable
        else []
    )
    dictionary_entries: list[str] = []
    if dictionary:
        count = _read_little(sections[2], 0, 4, "column dictionary")
        offset_bytes = (count + 1) * 4
        if offset_bytes > len(sections[2]) - 4:
            raise ContractError("column dictionary shape differs")
        dictionary_bytes = sections[2][4 + offset_bytes :]
        dictionary_offsets = _decode_offsets(
            sections[2][4 : 4 + offset_bytes],
            len(dictionary_bytes),
            count,
            "column dictionary",
        )
        for index in range(count):
            entry = _strict_utf8(
                dictionary_bytes[dictionary_offsets[index] : dictionary_offsets[index + 1]],
                "column dictionary",
            )
            if dictionary_entries and entry <= dictionary_entries[-1]:
                raise ContractError("column dictionary order is not canonical")
            dictionary_entries.append(entry)
    unknown_offsets = _decode_offsets(
        sections[4], len(sections[5]), row_count, "column unknown reason"
    )

    cells: list[dict[str, Any]] = []
    for index in range(row_count):
        mask = 1 << (index % 8)
        present = bool(sections[0][index // 8] & mask)
        unknown = bool(sections[1][index // 8] & mask)
        if present and unknown:
            raise ContractError("column chunk cell has conflicting states")
        if not present and not unknown and not column["optional"]:
            raise ContractError("column chunk has an absent nonoptional cell")
        value_begin = value_offsets[index] if variable else 0
        value_end = value_offsets[index + 1] if variable else 0
        reason_begin = unknown_offsets[index]
        reason_end = unknown_offsets[index + 1]
        if (variable and not present and value_begin != value_end) or (
            not unknown and reason_begin != reason_end
        ):
            raise ContractError("column chunk null storage is noncanonical")
        if fixed and not present:
            encoded = sections[3][index * fixed_width : (index + 1) * fixed_width]
            if any(encoded):
                raise ContractError("column chunk fixed null storage is nonzero")

        cell: dict[str, Any] = {
            "state": "present" if present else "unknown" if unknown else "absent",
            "type": column["type"],
        }
        if present:
            if scalar_kind == SCALAR_KINDS["bool"]:
                encoded_bool = sections[3][index]
                if encoded_bool > 1:
                    raise ContractError("column chunk boolean is not canonical")
                decoded_value: Any = bool(encoded_bool)
            elif scalar_kind == SCALAR_KINDS["int64"]:
                decoded_value = int.from_bytes(
                    sections[3][index * 8 : (index + 1) * 8], "little", signed=True
                )
            elif scalar_kind == SCALAR_KINDS["uint64"]:
                decoded_value = _read_little(sections[3], index * 8, 8, "column integer")
            elif dictionary:
                dictionary_index = _read_little(
                    sections[3], index * 4, 4, "column dictionary index"
                )
                if dictionary_index >= len(dictionary_entries):
                    raise ContractError("column chunk dictionary index is out of range")
                decoded_value = dictionary_entries[dictionary_index]
            else:
                encoded_value = sections[3][value_begin:value_end]
                if scalar_kind in {SCALAR_KINDS["bytes"], SCALAR_KINDS["set"]}:
                    decoded_value = encoded_value
                else:
                    decoded_value = _strict_utf8(encoded_value, "column value")
            _validate_scalar(column, decoded_value)
            cell["value"] = (
                decoded_value.hex() if isinstance(decoded_value, bytes) else decoded_value
            )
        elif unknown:
            reason = _strict_utf8(sections[5][reason_begin:reason_end], "column unknown reason")
            if not _control_free(reason):
                raise ContractError("column chunk unknown reason is invalid")
            cell["unknown_reason"] = reason
        if dictionary and not present and _read_little(
            sections[3], index * 4, 4, "column dictionary null"
        ) != 0:
            raise ContractError("column chunk dictionary null storage is nonzero")
        cells.append(cell)

    actual_payload_digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    if control["payload_digest"] != actual_payload_digest:
        raise ContractError("column chunk payload digest differs")
    semantic_fields = [
        (field, control[field])
        for field in CHUNK_CONTROL_FIELDS
        if field != "chunk_digest"
    ]
    expected_chunk_digest = _semantic_fields_digest(
        "cxxlens.provider-column-chunk.v2",
        "cxxlens.provider-column-chunk-digest.v2",
        semantic_fields,
    )
    if control["chunk_digest"] != expected_chunk_digest:
        raise ContractError("column chunk semantic digest differs")
    return (
        {
            **control,
            "row_offset": row_offset,
            "row_count": row_count,
            "chunk_index": chunk_index,
        },
        cells,
    )


def _decode_batch_end(
    frame: dict[str, Any], authority: dict[str, Any]
) -> dict[str, Any]:
    control = _require_exact_mapping(
        frame["control"], BATCH_END_CONTROL_FIELDS, "batch end control"
    )
    for field in BATCH_END_CONTROL_FIELDS:
        if field in {"row_count", "column_count", "chunk_count"}:
            _uint64(control[field], f"batch end {field}")
        elif not isinstance(control[field], str) or not control[field]:
            raise ContractError(f"batch end {field} differs")
    if not all(
        _canonical_digest(control[field])
        for field in ("descriptor_digest", "batch_digest")
    ):
        raise ContractError("batch end digest field is not canonical")
    if control["column_count"] != len(authority["columns"]):
        raise ContractError("batch end column count differs from descriptor authority")

    payload = frame["payload"]
    if len(payload) < 8 or payload[:5] != b"CXBE\x01" or payload[5:8] != b"\0\0\0":
        raise ContractError("batch end payload header differs")
    offset = 8
    columns: list[dict[str, Any]] = []
    for expected_column in authority["columns"]:
        length = _read_little(payload, offset, 2, "batch end column")
        offset += 2
        if length == 0 or length > len(payload) - offset:
            raise ContractError("batch end column identity is truncated")
        column_id = _strict_utf8(payload[offset : offset + length], "batch end column")
        offset += length
        payload_bytes = _read_little(payload, offset, 8, "batch end column bytes")
        offset += 8
        chunk_count = _read_little(payload, offset, 8, "batch end column chunks")
        offset += 8
        if column_id != expected_column["id"]:
            raise ContractError("batch end column order differs from descriptor authority")
        columns.append(
            {
                "column_id": column_id,
                "payload_bytes": payload_bytes,
                "chunk_count": chunk_count,
            }
        )
    ordered_chunk_digests: list[str] = []
    for _ in range(control["chunk_count"]):
        length = _read_little(payload, offset, 2, "batch end chunk digest")
        offset += 2
        if length == 0 or length > len(payload) - offset:
            raise ContractError("batch end chunk digest is truncated")
        digest = _strict_utf8(payload[offset : offset + length], "batch end chunk digest")
        offset += length
        if not _canonical_digest(digest):
            raise ContractError("batch end chunk digest is not canonical")
        ordered_chunk_digests.append(digest)
    if offset != len(payload) or sum(column["chunk_count"] for column in columns) != control[
        "chunk_count"
    ]:
        raise ContractError("batch end payload count or trailing bytes differ")
    empty = control["row_count"] == 0
    if empty != (control["chunk_count"] == 0) or any(
        empty != (column["chunk_count"] == 0)
        or (empty and column["payload_bytes"] != 0)
        for column in columns
    ):
        raise ContractError("batch end empty-batch summary differs")

    semantic_fields = [
        (field, control[field])
        for field in BATCH_END_CONTROL_FIELDS
        if field != "batch_digest"
    ]
    column_projection = _canonical_tuple(
        _canonical_tuple(
            (
                _digest_text_field("column_id", column["column_id"]),
                _digest_u64_field("payload_bytes", column["payload_bytes"]),
                _digest_u64_field("chunk_count", column["chunk_count"]),
            )
        )
        for column in columns
    )
    digest_projection = _canonical_tuple(
        _digest_text_field("chunk_digest", digest)
        for digest in ordered_chunk_digests
    )
    expected_batch_digest = _semantic_fields_digest(
        "cxxlens.provider-columnar-batch.v2",
        "cxxlens.provider-columnar-batch-digest.v2",
        semantic_fields,
        (
            _digest_value_field("columns", column_projection),
            _digest_value_field("ordered_chunk_digests", digest_projection),
        ),
    )
    if control["batch_digest"] != expected_batch_digest:
        raise ContractError("batch end semantic digest differs")
    return {
        **control,
        "columns": columns,
        "ordered_chunk_digests": ordered_chunk_digests,
    }


def _row_canonical_form(
    descriptor_id: str, columns: list[dict[str, Any]], row_index: int
) -> str:
    cells = {
        column["id"]: column["cells"][row_index]
        for column in sorted(columns, key=lambda item: item["id"])
    }
    return json.dumps(
        {"cells": cells, "descriptor_id": descriptor_id},
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    )


def _parse_fixture_rows(
    authority: dict[str, Any], row_canonical_forms: Any
) -> list[dict[str, dict[str, Any]]]:
    if (
        not isinstance(row_canonical_forms, list)
        or len(row_canonical_forms) > MAX_RUNTIME_RECEIPT_ROWS
    ):
        raise ContractError("fixture batch rows are not a bounded list")
    columns = {column["id"]: column for column in authority["columns"]}
    rows: list[dict[str, dict[str, Any]]] = []
    for canonical_form in row_canonical_forms:
        if not isinstance(canonical_form, str):
            raise ContractError("fixture row canonical form is not text")

        def reject_duplicate(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
            output: dict[str, Any] = {}
            for key, value in pairs:
                if key in output:
                    raise ContractError("fixture row has a duplicate JSON member")
                output[key] = value
            return output

        try:
            row = json.loads(
                canonical_form,
                object_pairs_hook=reject_duplicate,
                parse_constant=lambda token: (_ for _ in ()).throw(
                    ContractError(f"fixture row has a nonfinite number: {token}")
                ),
            )
        except (json.JSONDecodeError, UnicodeError) as error:
            raise ContractError("fixture row canonical JSON is invalid") from error
        if (
            not isinstance(row, dict)
            or set(row) != {"cells", "descriptor_id"}
            or row["descriptor_id"] != authority["descriptor_id"]
            or not isinstance(row["cells"], dict)
            or set(row["cells"]) != set(columns)
            or json.dumps(
                row, ensure_ascii=False, separators=(",", ":"), sort_keys=True
            )
            != canonical_form
        ):
            raise ContractError("fixture row canonical projection differs")
        decoded_cells: dict[str, dict[str, Any]] = {}
        for column_id, column in columns.items():
            cell = row["cells"][column_id]
            if not isinstance(cell, dict) or cell.get("type") != column["type"]:
                raise ContractError("fixture row cell type differs")
            state = cell.get("state")
            expected_fields = {"state", "type"}
            decoded_cell: dict[str, Any] = {"state": state, "type": column["type"]}
            if state == "present":
                expected_fields.add("value")
                value = cell.get("value")
                scalar = column["base_type"].split("<", 1)[0]
                if scalar in {"bytes", "set"}:
                    if (
                        not isinstance(value, str)
                        or len(value) % 2
                        or re.fullmatch(r"[0-9a-f]*", value) is None
                    ):
                        raise ContractError("fixture row byte cell is not lowercase hex")
                    value = bytes.fromhex(value)
                elif scalar == "bool":
                    if not isinstance(value, bool):
                        raise ContractError("fixture row boolean cell differs")
                elif scalar == "int64":
                    if (
                        isinstance(value, bool)
                        or not isinstance(value, int)
                        or not -(1 << 63) <= value < (1 << 63)
                    ):
                        raise ContractError("fixture row signed cell differs")
                elif scalar == "uint64":
                    _uint64(value, "fixture row unsigned cell")
                elif not isinstance(value, str):
                    raise ContractError("fixture row text cell differs")
                _validate_scalar(column, value)
                decoded_cell["value"] = value
            elif state == "absent":
                if not column["optional"]:
                    raise ContractError("fixture row has an absent nonoptional cell")
            elif state == "unknown":
                expected_fields.add("unknown_reason")
                reason = cell.get("unknown_reason")
                if not isinstance(reason, str) or not _control_free(reason):
                    raise ContractError("fixture row unknown reason differs")
                decoded_cell["unknown_reason"] = reason
            else:
                raise ContractError("fixture row cell state differs")
            if set(cell) != expected_fields:
                raise ContractError("fixture row cell projection is not closed")
            decoded_cells[column_id] = decoded_cell
        rows.append(decoded_cells)
    return rows


def _encode_fixture_column_chunk(
    authority: dict[str, Any],
    column: dict[str, Any],
    task_id: str,
    cells: list[dict[str, Any]],
    row_offset: int,
    chunk_index: int,
) -> tuple[dict[str, Any], bytes]:
    row_count = len(cells)
    if not 0 < row_count <= 256:
        raise ContractError("fixture column chunk row count is outside the SDK window")
    bitmap_size = (row_count + 7) // 8
    validity = bytearray(bitmap_size)
    unknown = bytearray(bitmap_size)
    value_auxiliary = bytearray()
    values = bytearray()
    reason_offsets = bytearray((0).to_bytes(4, "little"))
    reasons = bytearray()
    scalar_kind = column["scalar_kind"]
    encoding = _column_encoding(scalar_kind)
    dictionary = encoding == "dictionary-index-u32-le"
    variable = not dictionary and scalar_kind not in {
        SCALAR_KINDS["bool"],
        SCALAR_KINDS["int64"],
        SCALAR_KINDS["uint64"],
    }
    dictionary_indexes: dict[str, int] = {}
    if dictionary:
        entries = sorted(
            {cell["value"] for cell in cells if cell["state"] == "present"}
        )
        dictionary_indexes = {entry: index for index, entry in enumerate(entries)}
        value_auxiliary.extend(len(entries).to_bytes(4, "little"))
        value_auxiliary.extend((0).to_bytes(4, "little"))
        dictionary_bytes = bytearray()
        for entry in entries:
            dictionary_bytes.extend(entry.encode("utf-8"))
            value_auxiliary.extend(len(dictionary_bytes).to_bytes(4, "little"))
        value_auxiliary.extend(dictionary_bytes)
    elif variable:
        value_auxiliary.extend((0).to_bytes(4, "little"))

    for index, cell in enumerate(cells):
        mask = 1 << (index % 8)
        if cell["state"] == "present":
            validity[index // 8] |= mask
        elif cell["state"] == "unknown":
            unknown[index // 8] |= mask
        value = cell.get("value")
        if scalar_kind == SCALAR_KINDS["bool"]:
            values.append(1 if cell["state"] == "present" and value else 0)
        elif scalar_kind == SCALAR_KINDS["int64"]:
            values.extend(
                (value if cell["state"] == "present" else 0).to_bytes(
                    8, "little", signed=True
                )
            )
        elif scalar_kind == SCALAR_KINDS["uint64"]:
            values.extend(
                (value if cell["state"] == "present" else 0).to_bytes(8, "little")
            )
        elif dictionary:
            dictionary_index = (
                dictionary_indexes[value] if cell["state"] == "present" else 0
            )
            values.extend(dictionary_index.to_bytes(4, "little"))
        else:
            if cell["state"] == "present":
                encoded = value if isinstance(value, bytes) else value.encode("utf-8")
                values.extend(encoded)
            value_auxiliary.extend(len(values).to_bytes(4, "little"))
        if cell["state"] == "unknown":
            reasons.extend(cell["unknown_reason"].encode("utf-8"))
        reason_offsets.extend(len(reasons).to_bytes(4, "little"))

    sections = [
        bytes(validity),
        bytes(unknown),
        bytes(value_auxiliary),
        bytes(values),
        bytes(reason_offsets),
        bytes(reasons),
    ]
    payload = (
        b"CXCC\x01"
        + bytes([scalar_kind])
        + b"\0\0"
        + b"".join(len(section).to_bytes(4, "little") for section in sections)
        + b"".join(sections)
    )
    payload_digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    fields: list[tuple[str, str | int]] = [
        ("task_id", task_id),
        ("dependency_group_id", authority["dependency_group_id"]),
        ("atomic_output_group_id", authority["atomic_output_group_id"]),
        ("batch_id", authority["batch_id"]),
        ("descriptor_id", authority["descriptor_id"]),
        ("descriptor_digest", authority["descriptor_digest"]),
        ("column_id", column["id"]),
        ("row_offset", row_offset),
        ("row_count", row_count),
        ("chunk_index", chunk_index),
        ("encoding", encoding),
        ("payload_digest", payload_digest),
    ]
    control = dict(fields)
    control["chunk_digest"] = _semantic_fields_digest(
        "cxxlens.provider-column-chunk.v2",
        "cxxlens.provider-column-chunk-digest.v2",
        fields,
    )
    return control, payload


def _encode_fixture_batch_end(
    authority: dict[str, Any],
    task_id: str,
    row_count: int,
    columns: list[dict[str, Any]],
    ordered_chunk_digests: list[str],
) -> tuple[dict[str, Any], bytes]:
    fields: list[tuple[str, str | int]] = [
        ("task_id", task_id),
        ("dependency_group_id", authority["dependency_group_id"]),
        ("atomic_output_group_id", authority["atomic_output_group_id"]),
        ("batch_id", authority["batch_id"]),
        ("descriptor_id", authority["descriptor_id"]),
        ("descriptor_digest", authority["descriptor_digest"]),
        ("row_count", row_count),
        ("column_count", len(columns)),
        ("chunk_count", len(ordered_chunk_digests)),
    ]
    column_projection = _canonical_tuple(
        _canonical_tuple(
            (
                _digest_text_field("column_id", column["column_id"]),
                _digest_u64_field("payload_bytes", column["payload_bytes"]),
                _digest_u64_field("chunk_count", column["chunk_count"]),
            )
        )
        for column in columns
    )
    digest_projection = _canonical_tuple(
        _digest_text_field("chunk_digest", digest)
        for digest in ordered_chunk_digests
    )
    control = dict(fields)
    control["batch_digest"] = _semantic_fields_digest(
        "cxxlens.provider-columnar-batch.v2",
        "cxxlens.provider-columnar-batch-digest.v2",
        fields,
        (
            _digest_value_field("columns", column_projection),
            _digest_value_field("ordered_chunk_digests", digest_projection),
        ),
    )
    payload = bytearray(b"CXBE\x01\0\0\0")
    for column in columns:
        encoded_id = column["column_id"].encode("utf-8")
        payload.extend(len(encoded_id).to_bytes(2, "little"))
        payload.extend(encoded_id)
        payload.extend(column["payload_bytes"].to_bytes(8, "little"))
        payload.extend(column["chunk_count"].to_bytes(8, "little"))
    for digest in ordered_chunk_digests:
        encoded_digest = digest.encode("utf-8")
        payload.extend(len(encoded_digest).to_bytes(2, "little"))
        payload.extend(encoded_digest)
    return control, bytes(payload)


def _fixture_indexed_control(
    schema: str, fields: list[str], records: Any, label: str
) -> dict[str, Any]:
    if not isinstance(records, list):
        raise ContractError(f"fixture {label} records are not a list")
    rows: list[list[str]] = []
    for record in records:
        exact = _require_text_record(record, fields, f"fixture {label}")
        rows.append([exact[field] for field in fields])
    rows.sort()
    if len(rows) != len({tuple(row) for row in rows}):
        raise ContractError(f"fixture {label} record identity is duplicated")
    control: dict[str, Any] = {"schema": schema, "record_count": len(rows)}
    for index, row in enumerate(rows):
        for field, item in zip(fields, row, strict=True):
            control[f"{index}.{field}"] = item
    return control


def encode_runtime_private_fixture(
    protocol: dict[str, Any],
    expected_provider_identity: Any,
    task_id: str,
    authorized_batches: Any,
    batch_rows: Any,
    coverage_records: Any,
    unresolved_records: Any,
    evidence_records: Any,
) -> bytes:
    """Encode actual CXXP/CXCC/CXBE bytes for quality fixtures, never adoption input."""

    validate_shared_coverage_authority(protocol)
    authority_by_batch = _authorized_batch_index(authorized_batches)
    identity = _expected_provider_identity(
        expected_provider_identity,
        authority_by_batch,
    )
    if not isinstance(task_id, str) or not task_id or "\0" in task_id:
        raise ContractError("fixture task identity is invalid")
    if not isinstance(batch_rows, dict) or set(batch_rows) != set(authority_by_batch):
        raise ContractError("fixture batch row census differs from authorized batches")
    manifest = json.dumps(
        identity,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    )
    frames: list[tuple[int, Any, bytes]] = [
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
                "provider_id": identity["provider_id"],
                "provider_version": identity["provider_version"],
                "task_id": task_id,
            },
            b"",
        ),
    ]
    for authority in authority_by_batch.values():
        frames.append(
            (
                9,
                {
                    "schema": "cxxlens.provider-control.batch-begin.v1",
                    "task_id": task_id,
                    **{
                        field: authority[field]
                        for field in AUTHORIZED_BATCH_FIELDS[:-1]
                    },
                },
                b"",
            )
        )
        rows = _parse_fixture_rows(authority, batch_rows[authority["batch_id"]])
        columns = [
            {"column_id": column["id"], "payload_bytes": 0, "chunk_count": 0}
            for column in authority["columns"]
        ]
        ordered_chunk_digests: list[str] = []
        for row_offset in range(0, len(rows), 256):
            window = rows[row_offset : row_offset + 256]
            for column_index, column in enumerate(authority["columns"]):
                chunk_control, chunk_payload = _encode_fixture_column_chunk(
                    authority,
                    column,
                    task_id,
                    [row[column["id"]] for row in window],
                    row_offset,
                    columns[column_index]["chunk_count"],
                )
                frames.append((10, chunk_control, chunk_payload))
                columns[column_index]["payload_bytes"] += len(chunk_payload)
                columns[column_index]["chunk_count"] += 1
                ordered_chunk_digests.append(chunk_control["chunk_digest"])
        terminal_control, terminal_payload = _encode_fixture_batch_end(
            authority, task_id, len(rows), columns, ordered_chunk_digests
        )
        frames.append((11, terminal_control, terminal_payload))
    frames.extend(
        (
            (
                14,
                _fixture_indexed_control(
                    "cxxlens.provider-control.coverage.v1",
                    COVERAGE_FIELDS,
                    coverage_records,
                    "coverage",
                ),
                b"",
            ),
            (
                15,
                _fixture_indexed_control(
                    "cxxlens.provider-control.unresolved.v1",
                    UNRESOLVED_FIELDS,
                    unresolved_records,
                    "unresolved",
                ),
                b"",
            ),
            (
                17,
                _fixture_indexed_control(
                    "cxxlens.provider-control.evidence.v1",
                    EVIDENCE_FIELDS,
                    evidence_records,
                    "evidence",
                ),
                b"",
            ),
            (
                20,
                {
                    "schema": "cxxlens.provider-control.task-complete.v1",
                    "task_id": task_id,
                    "status": "complete",
                },
                b"",
            ),
        )
    )
    return b"".join(
        encode_frame(
            control,
            payload,
            protocol_minor=1,
            message_type=message_type,
            stream_id=1,
            sequence=sequence,
        )
        for sequence, (message_type, control, payload) in enumerate(frames)
    )


def _seal_batch(
    decoded_frames: list[dict[str, Any]],
    begin_index: int,
    expected_task_id: str,
    authority: dict[str, Any],
) -> tuple[dict[str, Any], int]:
    begin_frame = decoded_frames[begin_index]
    begin = _single_control_record(
        begin_frame["control"],
        "cxxlens.provider-control.batch-begin.v1",
        [
            "task_id",
            "descriptor_id",
            "descriptor_digest",
            "dependency_group_id",
            "atomic_output_group_id",
            "batch_id",
        ],
        "batch begin",
    )
    expected_begin = {
        "task_id": expected_task_id,
        **{field: authority[field] for field in AUTHORIZED_BATCH_FIELDS[:-1]},
    }
    if begin != expected_begin or begin_frame["payload"]:
        raise ContractError("batch begin differs from authorized batch")

    columns = [
        {**column, "cells": [], "payload_bytes": 0, "chunk_count": 0}
        for column in authority["columns"]
    ]
    ordered_chunk_digests: list[str] = []
    row_count = 0
    index = begin_index + 1
    while index < len(decoded_frames) and decoded_frames[index]["message_type"] == 10:
        expected_column_index = len(ordered_chunk_digests) % len(columns)
        column = columns[expected_column_index]
        chunk, cells = _decode_column_chunk(decoded_frames[index], column)
        if any(
            chunk[field] != expected_begin[field]
            for field in (
                "task_id",
                "dependency_group_id",
                "atomic_output_group_id",
                "batch_id",
                "descriptor_id",
                "descriptor_digest",
            )
        ):
            raise ContractError("column chunk differs from authorized batch binding")
        cycle_start = expected_column_index == 0
        expected_row_offset = len(column["cells"])
        if chunk["row_offset"] != expected_row_offset or chunk["chunk_index"] != column[
            "chunk_count"
        ]:
            raise ContractError("column chunk offset or index is not contiguous")
        if cycle_start:
            row_count = chunk["row_offset"] + chunk["row_count"]
        elif chunk["row_offset"] + chunk["row_count"] != row_count:
            raise ContractError("column chunk row window differs within a descriptor cycle")
        column["cells"].extend(cells)
        column["payload_bytes"] += len(decoded_frames[index]["payload"])
        column["chunk_count"] += 1
        ordered_chunk_digests.append(chunk["chunk_digest"])
        index += 1
    if len(ordered_chunk_digests) % len(columns) != 0:
        raise ContractError("column chunk cycle ends before every descriptor column")
    if index >= len(decoded_frames) or decoded_frames[index]["message_type"] != 11:
        raise ContractError("authorized batch has no immediate batch end")
    terminal = _decode_batch_end(decoded_frames[index], authority)
    if any(
        terminal[field] != expected_begin[field]
        for field in (
            "task_id",
            "dependency_group_id",
            "atomic_output_group_id",
            "batch_id",
            "descriptor_id",
            "descriptor_digest",
        )
    ):
        raise ContractError("batch end differs from authorized batch binding")
    if terminal["row_count"] > MAX_RUNTIME_RECEIPT_ROWS:
        raise ContractError("batch end exceeds the receipt row bound")
    if (
        terminal["row_count"] != row_count
        or terminal["ordered_chunk_digests"] != ordered_chunk_digests
    ):
        raise ContractError("batch end row count or chunk order differs")
    for terminal_column, column in zip(terminal["columns"], columns, strict=True):
        if (
            terminal_column["payload_bytes"] != column["payload_bytes"]
            or terminal_column["chunk_count"] != column["chunk_count"]
            or len(column["cells"]) != terminal["row_count"]
        ):
            raise ContractError("batch end column summary differs from decoded chunks")
    rows = [
        _row_canonical_form(authority["descriptor_id"], columns, row_index)
        for row_index in range(terminal["row_count"])
    ]
    return (
        {
            "task_id": expected_task_id,
            "descriptor_id": authority["descriptor_id"],
            "descriptor_digest": authority["descriptor_digest"],
            "dependency_group_id": authority["dependency_group_id"],
            "atomic_output_group_id": authority["atomic_output_group_id"],
            "batch_id": authority["batch_id"],
            "batch_digest": terminal["batch_digest"],
            "ordered_chunk_digests": ordered_chunk_digests,
            "row_canonical_forms": rows,
        },
        index + 1,
    )


def _seal_decoded_transcript(
    decoded_frames: list[dict[str, Any]],
    expected_task_id: str,
    expected_provider_identity: Any,
    authorized_batches: Any = None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Run the specialization-blind typed success path and construct its seal."""

    if not isinstance(expected_task_id, str) or not expected_task_id or "\0" in expected_task_id:
        raise ContractError("runtime receipt expected task identity is invalid")
    authority_by_batch = _authorized_batch_index(authorized_batches)
    expected_identity = _expected_provider_identity(
        expected_provider_identity,
        authority_by_batch,
    )
    if len(decoded_frames) < 7 or [
        frame["message_type"] for frame in decoded_frames[:3]
    ] != [1, 3, 5]:
        raise ContractError("provider transcript typed prefix order differs")
    for index, frame in enumerate(decoded_frames):
        if (
            frame["protocol_major"] != 1
            or frame["protocol_minor"] != 1
            or frame["flags"] != 0
            or frame["stream_id"] != 1
            or frame["sequence"] != index
        ):
            raise ContractError("provider transcript frame binding differs")

    manifest_text = decoded_frames[0]["control"]
    if not isinstance(manifest_text, str):
        raise ContractError("provider hello is not a canonical manifest text occurrence")
    try:
        manifest = json.loads(manifest_text)
    except json.JSONDecodeError as error:
        raise ContractError("provider hello manifest JSON is invalid") from error
    if not isinstance(manifest, dict):
        raise ContractError("provider hello manifest is not an object")
    canonical_expected_identity = json.dumps(
        expected_identity,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    )
    if manifest != expected_identity or manifest_text != canonical_expected_identity:
        raise ContractError("provider hello differs from independent expected identity")
    if decoded_frames[0]["payload"]:
        raise ContractError("provider hello has a payload")

    schema = decoded_frames[1]["control"]
    if schema != {
        "schema": "cxxlens.provider-control.schema-negotiate.v1",
        "protocol_schema": "cxxlens.provider-protocol.v1",
        "protocol_minor": 1,
    } or decoded_frames[1]["payload"]:
        raise ContractError("provider schema negotiation differs")
    accepted = _single_control_record(
        decoded_frames[2]["control"],
        "cxxlens.provider-control.task-accepted.v1",
        ["provider_id", "provider_version", "task_id"],
        "task accepted",
    )
    if (
        accepted["provider_id"] != expected_identity["provider_id"]
        or accepted["provider_version"] != expected_identity["provider_version"]
        or accepted["task_id"] != expected_task_id
        or decoded_frames[2]["payload"]
    ):
        raise ContractError("provider task acceptance identity differs")

    sealed_batches: list[dict[str, Any]] = []
    seen_batches: set[str] = set()
    side_channel_index = 3
    while (
        side_channel_index < len(decoded_frames)
        and decoded_frames[side_channel_index]["message_type"] == 9
    ):
        control = decoded_frames[side_channel_index]["control"]
        if not isinstance(control, dict):
            raise ContractError("batch begin control is not a mapping")
        batch_id = control.get("batch_id")
        authority = authority_by_batch.get(batch_id)
        if authority is None or batch_id in seen_batches:
            raise ContractError("provider batch is unauthorized or duplicated")
        sealed_batch, side_channel_index = _seal_batch(
            decoded_frames, side_channel_index, expected_task_id, authority
        )
        seen_batches.add(batch_id)
        sealed_batches.append(sealed_batch)

    if seen_batches != set(authority_by_batch):
        raise ContractError("provider transcript omits an authorized output batch")
    if [
        frame["message_type"] for frame in decoded_frames[side_channel_index:]
    ] != [14, 15, 17, 20]:
        raise ContractError("provider transcript typed side-channel order differs")
    coverage_frame, unresolved_frame, evidence_frame, complete_frame = decoded_frames[
        side_channel_index:
    ]
    if any(
        frame["payload"]
        for frame in (coverage_frame, unresolved_frame, evidence_frame, complete_frame)
    ):
        raise ContractError("provider transcript side channel has a payload")
    coverage = _indexed_control_records(
        coverage_frame["control"],
        "cxxlens.provider-control.coverage.v1",
        COVERAGE_FIELDS,
        "coverage",
    )
    try:
        retained_coverage = validate_shared_coverage_records(
            expected_task_id, coverage
        )
    except ProviderContractError as error:
        raise ContractError(f"provider transcript coverage failed: {error}") from error
    unresolved = _indexed_control_records(
        unresolved_frame["control"],
        "cxxlens.provider-control.unresolved.v1",
        UNRESOLVED_FIELDS,
        "unresolved",
    )
    evidence = _indexed_control_records(
        evidence_frame["control"],
        "cxxlens.provider-control.evidence.v1",
        EVIDENCE_FIELDS,
        "evidence",
    )
    complete = _single_control_record(
        complete_frame["control"],
        "cxxlens.provider-control.task-complete.v1",
        ["task_id", "status"],
        "task complete",
    )
    if complete != {"task_id": expected_task_id, "status": "complete"}:
        raise ContractError("provider transcript terminal binding differs")
    return (
        {
            "task_id": expected_task_id,
            "terminal": "provider.success",
            "batches": sealed_batches,
            "coverage_records": retained_coverage,
            "unresolved_records": unresolved,
            "evidence_records": evidence,
        },
        manifest,
    )


def _frame_transcript_projection(frames: Any) -> bytes:
    if not isinstance(frames, list):
        raise ContractError("decoded frame transcript is not a list")
    encoded: list[bytes] = []
    for index, value in enumerate(frames):
        frame = _require_exact_mapping(value, FRAME_TRANSCRIPT_FIELDS, "decoded frame")
        for field in FRAME_TRANSCRIPT_FIELDS[:6]:
            number = frame[field]
            maximum = 65535 if field in FRAME_TRANSCRIPT_FIELDS[:4] else (1 << 64) - 1
            if (
                isinstance(number, bool)
                or not isinstance(number, int)
                or number < 0
                or number > maximum
            ):
                raise ContractError(f"decoded frame {field} is not unsigned")
        if frame["sequence"] != index:
            raise ContractError("decoded frame sequence is not contiguous")
        for field in ("control_digest", "payload_digest"):
            if not isinstance(frame[field], str) or not re.fullmatch(
                r"sha256:[0-9a-f]{64}", frame[field]
            ):
                raise ContractError(f"decoded frame {field} is not canonical")
        encoded.append(
            _canonical_tuple(_canonical_value(frame[field]) for field in FRAME_TRANSCRIPT_FIELDS)
        )
    return _canonical_tuple(encoded)


def _sealed_transcript_projection(sealed: Any) -> bytes:
    value = _require_exact_mapping(sealed, SEALED_TRANSCRIPT_FIELDS, "sealed transcript")
    task_id = value["task_id"]
    terminal = value["terminal"]
    if not isinstance(task_id, str) or not task_id or not isinstance(terminal, str) or not terminal:
        raise ContractError("sealed transcript task or terminal is invalid")
    batches = value["batches"]
    if not isinstance(batches, list):
        raise ContractError("sealed transcript batches are not a list")
    batch_values: list[list[Any]] = []
    for batch_value in batches:
        batch = _require_exact_mapping(batch_value, SEALED_BATCH_FIELDS, "sealed batch")
        if batch["task_id"] != task_id:
            raise ContractError("sealed batch task binding differs")
        if not all(
            isinstance(batch[field], str) and batch[field]
            for field in SEALED_BATCH_FIELDS[:7]
        ):
            raise ContractError("sealed batch identity field is invalid")
        if not isinstance(batch["ordered_chunk_digests"], list) or not isinstance(
            batch["row_canonical_forms"], list
        ):
            raise ContractError("sealed batch repeated field is invalid")
        if not all(
            isinstance(item, str)
            for field in ("ordered_chunk_digests", "row_canonical_forms")
            for item in batch[field]
        ):
            raise ContractError("sealed batch repeated value is invalid")
        batch_values.append([batch[field] for field in SEALED_BATCH_FIELDS])

    coverage = value["coverage_records"]
    try:
        retained_coverage = validate_shared_coverage_records(task_id, coverage)
    except ProviderContractError as error:
        raise ContractError(f"sealed transcript coverage failed: {error}") from error
    if retained_coverage != coverage:
        raise ContractError("sealed transcript coverage was not losslessly retained")
    unresolved = value["unresolved_records"]
    evidence = value["evidence_records"]
    if not isinstance(unresolved, list) or not isinstance(evidence, list):
        raise ContractError("sealed transcript side channel is not a list")
    unresolved_values = [
        [
            _require_text_record(record, UNRESOLVED_FIELDS, "unresolved")[field]
            for field in UNRESOLVED_FIELDS
        ]
        for record in unresolved
    ]
    evidence_values = [
        [
            _require_text_record(record, EVIDENCE_FIELDS, "evidence")[field]
            for field in EVIDENCE_FIELDS
        ]
        for record in evidence
    ]
    coverage_values = [
        [record[field] for field in COVERAGE_FIELDS] for record in coverage
    ]
    return _canonical_value(
        [task_id, terminal, batch_values, coverage_values, unresolved_values, evidence_values]
    )


def derive_runtime_private_observation(
    protocol: dict[str, Any],
    raw_stdout: bytes,
    expected_task_id: str,
    *,
    expected_provider_identity: Any,
    authorized_batches: Any = None,
) -> dict[str, Any]:
    """Derive one receipt and its seal from an exact raw occurrence in one typed pass."""

    if not isinstance(raw_stdout, bytes):
        raise ContractError("raw provider stdout is not exact bytes")
    try:
        validate_shared_coverage_authority(protocol)
    except ProviderContractError as error:
        raise ContractError(f"provider coverage authority failed: {error}") from error
    raw_byte_count = len(raw_stdout)
    raw_sha256 = "sha256:" + hashlib.sha256(raw_stdout).hexdigest()
    decoded_frames = _decode_provider_stdout(protocol, raw_stdout)
    sealed_transcript, validated_provider_identity = _seal_decoded_transcript(
        decoded_frames,
        expected_task_id,
        expected_provider_identity,
        authorized_batches,
    )
    projected_frames = [
        {field: frame[field] for field in FRAME_TRANSCRIPT_FIELDS}
        for frame in decoded_frames
    ]
    frame_projection = _frame_transcript_projection(projected_frames)
    sealed_projection = _sealed_transcript_projection(sealed_transcript)
    receipt = {
        "raw_stdout_byte_count": raw_byte_count,
        "raw_stdout_sha256": raw_sha256,
        "decoded_frame_count": len(decoded_frames),
        "frame_transcript_digest": _semantic_digest(
            "cxxlens.provider-frame-transcript.v2", frame_projection
        ),
        "sealed_transcript_digest": _semantic_digest(
            "cxxlens.provider-sealed-transcript.v1", sealed_projection
        ),
    }
    return {
        "receipt": receipt,
        "sealed_transcript": sealed_transcript,
        "validated_provider_identity": validated_provider_identity,
    }


def derive_runtime_private_receipt(
    protocol: dict[str, Any],
    raw_stdout: bytes,
    expected_task_id: str,
    *,
    expected_provider_identity: Any,
    authorized_batches: Any = None,
) -> dict[str, Any]:
    """Return only the receipt leaves from the raw-only observation path."""

    return derive_runtime_private_observation(
        protocol,
        raw_stdout,
        expected_task_id,
        expected_provider_identity=expected_provider_identity,
        authorized_batches=authorized_batches,
    )["receipt"]


def validate_runtime_private_receipt(
    protocol: dict[str, Any],
    raw_stdout: bytes,
    expected_task_id: str,
    receipt: Any,
    *,
    expected_provider_identity: Any,
    authorized_batches: Any = None,
    public_semantic_digest: str | None = None,
) -> dict[str, Any]:
    supplied = _require_exact_mapping(receipt, RECEIPT_FIELDS, "runtime receipt")
    expected = derive_runtime_private_receipt(
        protocol,
        raw_stdout,
        expected_task_id,
        expected_provider_identity=expected_provider_identity,
        authorized_batches=authorized_batches,
    )
    if supplied != expected:
        raise ContractError("runtime receipt differs from runtime-owned evidence")
    if public_semantic_digest is not None and public_semantic_digest in {
        supplied["raw_stdout_sha256"],
        supplied["frame_transcript_digest"],
        supplied["sealed_transcript_digest"],
    }:
        raise ContractError("public process semantic digest aliases receipt authority")
    return expected


def validate_runtime_private_receipt_authority(contract: dict[str, Any]) -> None:
    authority = contract["protocol_session"].get("runtime_private_receipt")
    if authority != RUNTIME_PRIVATE_RECEIPT_AUTHORITY:
        raise ContractError("runtime-private receipt authority is not exact")
    if contract["report"].get("public_semantic_digest_role") != (
        "diagnostic-report-identity-only-never-runtime-private-receipt-or-adoption-authority"
    ):
        raise ContractError("public process semantic digest became receipt authority")
    required_acceptance = {
        "shared-validator-requires-one-task-transport-record-and-losslessly-retains-unknown-semantic-coverage",
        "runtime-private-receipt-is-derived-in-the-same-shared-validation-pass",
        "public-process-semantic-digest-never-aliases-runtime-private-receipt-authority",
    }
    if not required_acceptance.issubset(contract["acceptance"]):
        raise ContractError("runtime receipt acceptance markers are incomplete")


def validate_host_input_authority(
    contract: dict[str, Any], protocol: dict[str, Any]
) -> None:
    if protocol["compatibility"]["current"] != "1.1.0":
        raise ContractError("provider runtime does not bind Provider Protocol 1.1.0")
    profiles = contract["protocol_session"]["host_input_profiles"]
    if profiles.get("1.0") != {
        "protocol_minor": 0,
        "required_features": [],
        "exact_frames": [
            "hello_ack",
            "schema_negotiate",
            "open_task",
            "credit",
            "close",
        ],
        "exact_sequences": [0, 1, 2, 3, 4],
        "payload_policy": "open-task-only",
        "compatibility": "existing-public-five-frame-vector-api-unchanged-wrapper",
    }:
        raise ContractError("provider runtime minor 0 host transcript is not exact")
    if profiles.get("1.1") != {
        "protocol_minor": 1,
        "required_features": ["task-input-chunks-v1"],
        "exact_frame_pattern": [
            "hello_ack",
            "schema_negotiate",
            "open_task",
            "input_descriptor",
            "input_chunk-zero-or-more",
            "credit",
            "close",
        ],
        "fixed_prefix_sequences": [0, 1, 2, 3],
        "chunk_sequence": "four-plus-chunk-index",
        "credit_sequence": "four-plus-chunk-count",
        "close_sequence": "five-plus-chunk-count",
        "payload_policy": "input-chunk-only",
        "open_task_payload": "empty",
    }:
        raise ContractError("provider runtime minor 1 host transcript is not exact")
    transfer = contract["protocol_session"]["host_input_transfer"]
    protocol_transfer = protocol["task_input_transfer"]
    if transfer["limits"] != {
        "chunk_payload_bytes": protocol_transfer["limits"][
            "maximum_chunk_payload_bytes"
        ],
        "logical_input_bytes": protocol_transfer["limits"][
            "maximum_logical_input_bytes"
        ],
        "chunks": protocol_transfer["limits"]["maximum_input_chunks"],
    }:
        raise ContractError("provider runtime task input limits diverge from protocol")
    exact_transfer = {
        "activation": {
            "protocol_minor": 1,
            "required_features": ["task-input-chunks-v1"],
        },
        "limits": {
            "chunk_payload_bytes": 1048576,
            "logical_input_bytes": 67108864,
            "chunks": 64,
        },
        "descriptor_control": "cxxlens.provider-control.input-descriptor.v1-exact-five-fields",
        "chunk_control": "cxxlens.provider-control.input-chunk.v1-exact-five-fields",
        "sequence_and_shape": "contiguous-index-offset-length-nonfinal-size-and-final-remainder",
        "zero_input": {
            "total_bytes": 0,
            "chunk_count": 0,
            "input_chunks": 0,
            "input_digest": "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        },
        "final_seal": "exact-total-length-and-streaming-sha256-equal-open-task-input-digest",
        "task_accepted": "only-after-shared-seal-task-decode-and-bottom-up-binding",
        "output_credit": "separate-provider-output-only",
        "execution_transport_bytes": "separate-stdout-stderr-accounting-only",
        "ambient_input_side_channel": "forbidden",
        "raw_frames_and_spool": "diagnostic-transport-occurrences-only",
        "semantic_authority": "detached-decoded-task-and-existing-output-seal-only",
        "shared_incremental_core": "host-encoder-worker-decoder-process-runtime-conformance-validator",
        "production_full_input_vector_adapter": "forbidden",
        "existing_public_signatures": "unchanged",
        "minor_0_public_vector_api": "bounded-wrapper-over-shared-incremental-core",
    }
    if transfer != exact_transfer:
        raise ContractError("provider runtime task input transfer authority is not exact")
    typed = contract["protocol_session"]["typed_validation"]
    if typed["host_input_incremental_core"] != (
        "same-transition-digest-length-and-budget-state-for-host-worker-runtime-and-conformance"
    ):
        raise ContractError("provider runtime does not require one incremental core")
    if typed["host_input_seal"] != (
        "required-before-task-accepted-and-semantic-task-use"
    ):
        raise ContractError("provider runtime task acceptance is not seal-gated")
    if typed["host_input_side_channel"] != (
        "ambient-path-fd-environment-shared-memory-forbidden"
    ):
        raise ContractError("provider runtime permits an ambient input side channel")
    if typed["host_input_credit"] != "provider-output-only-never-input-accounting":
        raise ContractError("provider runtime mixes input accounting with output credit")


def validate(root: pathlib.Path) -> None:
    contract = load(root / "schemas/cxxlens_ng_provider_runtime_contract.yaml")
    contract_schema = load(
        root / "schemas/cxxlens_ng_provider_runtime_contract.schema.yaml"
    )
    report_schema = load(
        root / "schemas/cxxlens_ng_provider_execution_report.schema.yaml"
    )
    jsonschema.Draft202012Validator.check_schema(contract_schema)
    jsonschema.Draft202012Validator(contract_schema).validate(contract)
    protocol = load(root / "schemas/cxxlens_ng_provider_protocol.yaml")
    validate_host_input_authority(contract, protocol)
    validate_runtime_private_receipt_authority(contract)
    budget = contract["runtime"]["budget"]
    if budget != {
        "host_input_transfer": {
            "logical_input_bytes": "provider-protocol-profile-limit-before-task-acceptance",
            "chunk_payload_bytes": "provider-protocol-per-chunk-limit-before-allocation",
            "chunk_frames": "provider-protocol-count-limit-and-contiguous-state",
            "accounting": "shared-incremental-host-input-core",
            "exhaustion": "fail-before-task-accepted-without-semantic-adoption",
        },
        "logical_surface_independent": {
            "output_bytes": "sum-control-and-payload-for-non-lifecycle-provider-output-frames",
            "rows": "task-global-sum-of-validated-batch-end-row-count",
            "diagnostics": "task-global-decoded-unresolved-record-count",
            "validator": "shared-logical-process-production-validator",
            "exhaustion": "provider.output-limit-without-success-adoption",
        },
        "process_isolation_only": {
            "wall_ms": "monotonic-deadline-process-group-kill",
            "cpu_ms": "rlimit-cpu-rounded-up-to-seconds",
            "address_space_bytes": "rlimit-as-not-rss",
            "transport_bytes": "combined-stdout-stderr-drain-and-rlimit-fsize",
            "open_files": "rlimit-nofile",
            "subprocesses": "rlimit-nproc",
        },
        "wire_credit": "provider-output-only-independent-from-host-input-logical-and-transport-byte-budgets",
        "direct_run_worker": "trusted-callback-logical-limits-and-cooperative-cancellation-only",
        "unsupported_created_file_count": "omitted-not-claimed",
    }:
        raise ContractError("provider execution budget surface contract is not exact")
    selection = contract["selection"]
    if selection["candidate_identity"] != {
        "schema": "cxxlens.provider-candidate.v1",
        "digest": "cxxlens-semantic-digest-v2",
        "fields": [
            "full-canonical-manifest",
            "ordered-executable-argv",
            "authoritative-path",
            "trust-verdict",
            "certification-verdict",
            "canonical-certified-qualifications",
            "canonical-sandbox-report",
            "validation-error",
        ],
        "source_binding": "decision-source-plus-candidate-digest",
    }:
        raise ContractError("provider candidate identity projection is not exact")
    if selection["candidate_order"] != [
        "discovery-source-precedence",
        "provider-id",
        "provider-version",
        "binary-digest",
        "candidate-digest",
    ]:
        raise ContractError("provider candidate order is not a strict canonical total order")
    executable_binding = contract["runtime"]["launch"]["executable_binding"]
    if executable_binding != {
        "source_resolution": "single-open-or-working-directory-openat",
        "measured_image": "copied-sealed-executable-memfd",
        "digest_subject": "exact-sealed-image-bytes",
        "execution": "execveat-verified-fd-at-empty-path",
        "path_reopen_after_measurement": "forbidden",
        "rename_symlink_in_place_mutation": "verified-image-or-reject",
    }:
        raise ContractError("provider executable measurement is not bound to executed bytes")
    if contract["runtime"]["sandbox"]["evidence_digest_v3"] != [
        "resolved-policy-canonical-form",
        "recomputed-policy-digest",
        "measured-executable-digest",
        "achieved-assurance",
        "invocation-budget-limits",
        "exact-applied-mechanisms",
    ]:
        raise ContractError("sandbox evidence v3 omits measured executable identity")
    jsonschema.Draft202012Validator.check_schema(report_schema)
    fallback_identity = load(
        root / "schemas/cxxlens_ng_clang22_fallback_identity.yaml"
    )
    fallback_identity_schema = load(
        root / "schemas/cxxlens_ng_clang22_fallback_identity.schema.yaml"
    )
    jsonschema.Draft202012Validator.check_schema(fallback_identity_schema)
    jsonschema.Draft202012Validator(fallback_identity_schema).validate(fallback_identity)
    expected_fallback_vectors = {
        "normal-usr-golden",
        "function-overloads",
        "special-members-and-operators",
        "template-primary-specializations",
        "constrained-overloads",
        "same-signature-redeclaration",
        "definition-preference",
        "cross-tu-order-permutation",
        "unanchored-opaque",
        "toolchain-change",
    }
    if {vector["id"] for vector in fallback_identity["vectors"]} != expected_fallback_vectors:
        raise ContractError("Clang 22 fallback identity conformance vectors are incomplete")

    sample_report = {
        "schema": "cxxlens.provider-execution-report.v1",
        "terminal": "provider.binary-identity-mismatch",
        "provider": {
            "id": "cxxlens.clang22.reference",
            "version": "1.0.0",
            "binary_digest": "sha256:" + "a" * 64,
            "semantic_contract_digest": "sha256:" + "b" * 64,
        },
        "input_binding": {
            "task": "sha256:" + "c" * 64,
            "invocation": "sha256:" + "d" * 64,
            "toolchain": "sha256:" + "e" * 64,
            "environment": "sha256:" + "f" * 64,
        },
        "measured_executable_digest": "sha256:" + "a" * 64,
        "sandbox": {
            "platform": "linux-glibc",
            "mechanisms": ["no-shell-argv-exec"],
            "achieved": "enforced",
            "policy_digest": "sha256:" + "1" * 64,
            "evidence_digest": "sha256:" + "2" * 64,
        },
        "frames": {
            "count": 0,
            "last_sequence": None,
            "transcript_digest": "sha256:" + "3" * 64,
        },
        "diagnostics": [],
        "semantic_digest": "sha256:" + "4" * 64,
    }
    jsonschema.Draft202012Validator(report_schema).validate(sample_report)
    stable_terminals = set(contract["terminal"]["stable"])
    schema_terminals = set(report_schema["properties"]["terminal"]["enum"])
    if schema_terminals != stable_terminals:
        raise ContractError("execution report terminal enum diverges from runtime registry")
    invalid_report = dict(sample_report)
    invalid_report["terminal"] = "provider.unknown-reason"
    if not list(jsonschema.Draft202012Validator(report_schema).iter_errors(invalid_report)):
        raise ContractError("execution report schema accepted an unregistered terminal")
    runtime_source = (root / "src/sdk/provider_runtime.cpp").read_text(encoding="utf-8")
    terminal_block = runtime_source.split(
        "constexpr std::array stable_terminal_reasons{", 1
    )
    if len(terminal_block) != 2:
        raise ContractError("runtime stable terminal registry is missing")
    cpp_terminals = set(
        re.findall(r'std::string_view\{"((?:provider|security)\.[a-z0-9-]+)"\}',
                   terminal_block[1].split("};", 1)[0])
    )
    if cpp_terminals != stable_terminals:
        raise ContractError("C++ terminal registry diverges from runtime authority")

    required = {
        "include/cxxlens/sdk/provider.hpp": (
            "class process_provider_runtime",
            "class provider_process_port",
            "select_provider",
            "expected_binary_digest",
            "provider_fallback_policy",
            "certified_qualifications",
            "candidate_digest",
            "class provider_selection",
            "selected_candidate() const",
            "authority_request() const",
        ),
        "src/runtime/provider_process_adapter.cpp": (
            "provider.binary-identity-mismatch",
            "make_verified_executable",
            "working-directory-open",
            "MFD_ALLOW_SEALING",
            "F_SEAL_WRITE",
            "SYS_execveat",
            "AT_EMPTY_PATH",
            "resolve_sandbox_policy",
            "sandbox_evidence_digest",
            "security.sandbox-insufficient",
            "CLOSE_RANGE_UNSHARE",
            "AUDIT_ARCH_X86_64",
            "SECCOMP_RET_KILL_PROCESS",
            "close_inherited_descriptors",
        ),
        "src/sdk/provider_runtime.cpp": (
            "provider.timeout",
            "provider.cancelled",
            "provider.binary-identity-mismatch",
            "provider.protocol-state-invalid",
            "provider.credit-exceeded",
            "provider.batch-invalid",
            "provider.required-feature-missing",
            "provider.protocol-minor-mismatch",
            "request.selection.validate()",
            "effective_sandbox",
            "security.sandbox-policy-mismatch",
            "validate_provider_transcript",
            "allowed_failure_terminal",
            "validated_success_",
            "decode_task_accepted_metadata",
            "decode_batch_begin_metadata",
            "decode_coverage_metadata",
            "decode_unresolved_metadata",
            "decode_evidence_metadata",
            "decode_task_complete_metadata",
            "decode_task_failed_metadata",
            "encode_host_transcript",
            "CXXLENS_PROVIDER_MANIFEST",
            "CXXLENS_PROVIDER_TASK_INPUT_DIGEST",
        ),
        "src/sdk/provider_validation_internal.hpp": (
            "transcript_validation_request",
            "validate_provider_transcript",
        ),
        "src/sdk/provider.cpp": (
            "provider.fallback-policy-mismatch",
            "cxxlens.provider-fallback-policy.v1",
            "provider.unknown-required-extension",
            "provider.invalid-frame-flags",
            "provider.unsupported-compression",
            "encode_control_text",
            "encode_task_accepted_metadata",
            "encode_batch_begin_metadata",
            "encode_coverage_metadata",
            "encode_unresolved_metadata",
            "encode_evidence_metadata",
            "decode_schema_negotiate_metadata",
            "decode_open_task_metadata",
            "decode_credit_metadata",
            "decode_close_metadata",
            "validate_host_transcript",
            "provider.host-transcript-invalid",
            "value.summary",
            "encode_column_chunk",
            "decode_columnar_batch_end",
            "valid_utf8",
            "control-utf8",
            "no-shell-argv-exec",
            "network-syscall-deny",
            "inherited-fd-close-range",
            "seccomp-audit-arch",
            "candidate_identity_digest",
            "duplicate-canonical-candidate",
        ),
        "src/llvm/clang22/provider_sdk.cpp": (
            "getExpansionRange",
            "getImmediateExpansionRange",
            "native.source-origin-invalid",
        ),
        "src/llvm/clang22/provider_worker.cpp": (
            "cc.call_site",
            "ignored-or-gcc-specific-option",
            "derive_domain_identity",
            "call.direct_callee",
            "symbol.is_definition",
            "symbol.is_canonical_declaration",
            "provider.entity-redeclaration-incompatible",
            "indirect_member_pointer",
            "virtual_member",
            "provider.call-kind-target-inconsistent",
            "source_origin_chain",
            "observation_dedup_key",
            "const auto key = observation_dedup_key(observation);",
            "call_occurrence_class",
            "ordered_observations",
            "source_snapshot",
            "clang22.declaration-fallback.v2",
            "make_declaration_identity",
            "canonical_source_anchor",
            "symbol.identity_confidence",
            "call.direct_callee_identity_confidence",
            "provider.declaration-identity-unresolved",
            "validate_host_transcript",
            "CXXLENS_PROVIDER_PROTOCOL_MINOR",
        ),
        "src/llvm/clang22/observation_v2.cpp": (
            "frontend.clang22.entity_observation",
            "make_observation_v2_row",
        ),
        "src/llvm/clang22/provider_task_v3.cpp": (
            WORKER_TASK_CODEC_V3,
            "task_v3_projection",
            "reconstruct_provider_task",
        ),
        "CMakeLists.txt": (
            "cxxlens-clang-worker-22",
            "cxxlens_clang22_materialization_codecs",
            "cxxlens_ng_provider_runtime_contract.yaml",
        ),
        "tests/fixtures/provider_process_fixture.cpp": (
            "validate_host_transcript",
            "CXXLENS_PROVIDER_MANIFEST",
            "CXXLENS_PROVIDER_PROTOCOL_MINOR",
        ),
        "tests/unit/sdk/provider_runtime_test.cpp": (
            "check_verified_executable_binding",
            "verified-old",
            "measured and executed as one image",
        ),
        "tests/adapter/clang22/provider_normalizer_test.cpp": (
            "one TWICE macro expansion collapsed its two same-callee calls",
            "two TWICE macro expansions did not preserve four call occurrences",
            "same-expansion macro call IDs depend on observation input order",
        ),
    }
    for relative, markers in required.items():
        path = root / relative
        if not path.is_file():
            raise ContractError(f"missing provider runtime evidence: {relative}")
        text = path.read_text(encoding="utf-8")
        missing = [marker for marker in markers if marker not in text]
        if missing:
            raise ContractError(f"{relative} lacks markers: {missing}")

    codec = (root / "src/llvm/clang22/provider_task_v3.cpp").read_text(encoding="utf-8")
    validate_task_codec_markers(codec)
    worker = (root / "src/llvm/clang22/provider_worker.cpp").read_text(encoding="utf-8")
    for forbidden in ("call.direct_callee_anchor", "builder.set<relation::anchor>"):
        if forbidden in worker:
            raise ContractError(
                f"Clang normalizer leaks occurrence anchor into semantic entity identity: {forbidden}"
            )

    catalog = load(root / "schemas/cxxlens_ng_public_api_catalog.yaml")
    entries = {entry["id"]: entry for entry in catalog["entries"]}
    runtime = entries.get("public.provider-runtime")
    if runtime is None or runtime["status"] != "implemented" or runtime["owner_issue"] != "#151":
        raise ContractError("public.provider-runtime is not an implemented Issue #151 entry")
    required_budget_invariants = {
        "shared-validator-enforces-logical-output-bytes-rows-and-diagnostic-records-before-success-adoption",
        "process-port-separately-enforces-wall-cpu-address-space-transport-open-file-and-subprocess-resources",
        "address-space-budget-is-rlimit-as-not-rss",
        "created-file-count-is-not-claimed",
    }
    if not required_budget_invariants.issubset(runtime.get("invariants", [])):
        raise ContractError("provider runtime catalog omits Issue #123 budget invariants")
    if "docs/design/adr/0087-provider-budget-surface-parity.md" not in runtime.get(
        "implementation_evidence", []
    ):
        raise ContractError("provider runtime catalog omits Issue #123 evidence")

    header = (root / "include/cxxlens/sdk/provider.hpp").read_text(encoding="utf-8")
    for marker in (
        "address_space_bytes",
        "transport_bytes",
        "result<void> validate() const",
        "set_output_budget",
        "counts_toward_output_budget",
        "Resource preemption is provided only",
    ):
        if marker not in header:
            raise ContractError(f"provider budget public marker is missing: {marker}")
    if "rss_bytes" in header or "created_files" in header:
        raise ContractError("provider budget still claims RSS or created-file count enforcement")
    process_source = (root / "src/runtime/provider_process_adapter.cpp").read_text(
        encoding="utf-8"
    )
    for marker in (
        "RLIMIT_AS, invocation.budget.address_space_bytes",
        "RLIMIT_FSIZE, invocation.budget.transport_bytes",
        "invocation.budget.open_files",
        "invocation.budget.subprocesses",
        "invocation.budget.wall_ms",
    ):
        if marker not in process_source:
            raise ContractError(f"provider process resource marker is missing: {marker}")
    validator_source = (root / "src/sdk/provider_runtime.cpp").read_text(encoding="utf-8")
    for marker in (
        "logical_output_bytes",
        "batch_terminal->row_count > request.budget->rows - output_rows",
        "records->size() > request.budget->diagnostics - diagnostics",
        'return fail("provider.output-limit", request.task_id, "output_bytes")',
    ):
        if marker not in validator_source:
            raise ContractError(f"shared logical budget marker is missing: {marker}")
    budget_test = (root / "tests/unit/sdk/provider_runtime_test.cpp").read_text(
        encoding="utf-8"
    )
    for marker in (
        "logical output-byte limit diverged by execution surface",
        "row budget could be bypassed by column chunks or execution surface",
        "diagnostic record budget diverged by framing or execution surface",
        "run_worker did not enforce logical output bytes before success",
        "one execution budget dimension accepted zero",
        "exact process transport byte budget was rejected",
        "worker output limit was not distinguished",
    ):
        if marker not in budget_test:
            raise ContractError(f"provider budget acceptance marker is missing: {marker}")
    native = entries.get("public.native-provider-sdk")
    if native is None or native["status"] != "implemented" or native["owner_issue"] != "#153":
        raise ContractError("public.native-provider-sdk is not an implemented Issue #153 entry")

    namespaces = load(root / "schemas/cxxlens_ng_namespace_registry.yaml")
    if not any(
        entry["kind"] == "relation"
        and entry["prefix"] == "frontend.clang22."
        and entry["owner"] == "cxxlens.clang22.reference"
        for entry in namespaces["entries"]
    ):
        raise ContractError("frontend.clang22 relation namespace is not registered")

    support = load(root / "schemas/cxxlens_ng_provider_support_matrix.yaml")
    if not any(
        entry["provider_id"] == "cxxlens.clang22.reference"
        and entry["status"] == "conformance-only"
        and entry["blocker_issue"] is None
        for entry in support["entries"]
    ):
        raise ContractError("Clang 22 provider conformance tuple is not published")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["check"])
    parser.add_argument("--root", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    try:
        validate(arguments.root.resolve())
    except (ContractError, jsonschema.ValidationError, jsonschema.SchemaError) as error:
        print(f"provider runtime contract check failed: {error}", file=sys.stderr)
        return 1
    print("provider runtime contract check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
