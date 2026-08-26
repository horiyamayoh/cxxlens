#!/usr/bin/env python3
"""Test-only Protocol 2 wire fixtures.

The Protocol 2 wire and canonical-CBOR authority is the compiled C++ codec
(``tests/protocol_v2`` and the SDK adapter tests).  This module exists only so
the Python runtime/source-closure contract tests can construct bounded,
synthetic transcript bytes.  It is deliberately not imported by the
Protocol 2 contract checker.
"""

from __future__ import annotations

import hashlib
import struct
from typing import Any, NoReturn


FRAME = struct.Struct(">4sHHHHQQIQ32s32s")
MAX_CONTROL = 65536
MAX_PAYLOAD = 16777216
PROTOCOL_MAJOR = 2
PROTOCOL_MINOR = 0


class ProviderWireTestError(ValueError):
    """Typed failure raised by synthetic wire fixtures."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code


def fail(code: str, message: str) -> NoReturn:
    raise ProviderWireTestError(code, message)


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
    return {
        "required_extension": 1,
        "optional_extension": 2,
        "compressed_payload": 4,
        "end_of_stream": 8,
    }[name]


def _known_message_ids(contract: dict[str, Any]) -> set[int]:
    return {row["id"] for row in contract["message_types"]["registry"]}


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


__all__ = [
    "FRAME",
    "ProviderWireTestError",
    "cbor_decode",
    "cbor_encode",
    "decode_frame",
    "encode_frame",
]
