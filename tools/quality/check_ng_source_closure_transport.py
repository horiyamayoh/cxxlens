#!/usr/bin/env python3
"""Validate the active Protocol 2 source-closure transport authority."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys
import unicodedata
from typing import Any

import jsonschema
import yaml

from check_ng_provider_protocol import cbor_encode


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONTRACT = pathlib.Path("schemas/cxxlens_ng_source_closure_transport.yaml")
SCHEMA = pathlib.Path("schemas/cxxlens_ng_source_closure_transport.schema.yaml")
PROTOCOL = pathlib.Path("schemas/cxxlens_ng_provider_protocol_v2.yaml")
PROTOCOL_SCHEMA = pathlib.Path("schemas/cxxlens_ng_provider_protocol_v2.schema.yaml")
REQUEST = pathlib.Path("schemas/cxxlens_ng_clang22_materialization_request_v2_2.schema.yaml")
TASK = pathlib.Path("schemas/cxxlens_ng_provider_task_v4.schema.yaml")
MANIFEST_SCHEMA = pathlib.Path("schemas/cxxlens_ng_source_closure_manifest_v1.schema.yaml")
ADR = pathlib.Path("docs/design/adr/0107-provider-protocol-2-cutover.md")
PROJECT_PATH_PREFIX = "project://"
MAXIMUM_LOGICAL_PATH_UTF8_BYTES = 4096
SOURCE_CLOSURE_DIGEST_DOMAIN = "cxxlens.clang22.source-closure.v1"
SOURCE_CLOSURE_MANIFEST_SCHEMA = "cxxlens.source-closure-manifest.v1"
SOURCE_CLOSURE_MANIFEST_DIGEST_DOMAIN = "cxxlens.source-closure-manifest.v1"


class SourceClosureTransportError(ValueError):
    """A fail-closed source-closure transport contract violation."""


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode(
        "utf-8"
    )


def _length(value: int) -> bytes:
    return value.to_bytes(8, "big")


def _canonical_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return b"\x04" + _length(len(encoded)) + encoded


def _canonical_bytes(value: bytes) -> bytes:
    return b"\x03" + _length(len(value)) + value


def _canonical_boolean(value: bool) -> bytes:
    return b"\x01" + (b"\x01" if value else b"\x00")


def _canonical_integer(value: int) -> bytes:
    if type(value) is not int or value < -(1 << 63) or value > (1 << 63) - 1:
        raise SourceClosureTransportError("canonical integer is outside signed 64-bit range")
    magnitude = abs(value)
    width = max(1, (magnitude.bit_length() + 7) // 8)
    return b"\x02" + (b"\x01" if value < 0 else b"\x00") + _length(width) + magnitude.to_bytes(width, "big")


def _canonical_tuple(values: list[bytes]) -> bytes:
    output = bytearray(b"\x05" + _length(len(values)))
    for value in values:
        output.extend(_length(len(value)))
        output.extend(value)
    return bytes(output)


def semantic_digest(domain: str, projection: Any) -> str:
    return semantic_digest_bytes(domain, canonical_json(projection))


def semantic_digest_bytes(domain: str, projection: bytes) -> str:
    framed = _canonical_tuple(
        [
            _canonical_string("cxxlens-semantic-digest-v2"),
            _canonical_string(domain),
            _canonical_bytes(projection),
        ]
    )
    return "semantic-v2:sha256:" + hashlib.sha256(framed).hexdigest()


def content_projection_digest(projection: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(projection)).hexdigest()


def manifest_digest(manifest: dict[str, Any]) -> str:
    return semantic_digest(SOURCE_CLOSURE_MANIFEST_DIGEST_DOMAIN, manifest)


def closure_digest(members: list[dict[str, Any]], blobs: list[dict[str, Any]]) -> str:
    """Reproduce the ADR 0101 canonical source-closure projection."""
    encoded_members = []
    for member in members:
        encoded_members.append(_canonical_tuple([
            _canonical_string(member["file_id"]),
            _canonical_string(member["logical_path"]),
            _canonical_string(member["role"]),
            _canonical_string(member["encoding"]),
            _canonical_integer(member["size_bytes"]),
            _canonical_string(member["content_digest"]),
            _canonical_boolean(member["read_only"]),
        ]))
    encoded_blobs = [
        _canonical_tuple([
            _canonical_string(blob["content_digest"]),
            _canonical_integer(blob["size_bytes"]),
        ])
        for blob in blobs
    ]
    projection = _canonical_tuple([
        _canonical_string(SOURCE_CLOSURE_DIGEST_DOMAIN),
        _canonical_string("unicode-default-casefold-then-nfc"),
        _canonical_tuple(encoded_members),
        _canonical_tuple(encoded_blobs),
    ])
    return semantic_digest_bytes(SOURCE_CLOSURE_DIGEST_DOMAIN, projection)


def _validated_logical_path(value: Any, *, subject: str = "logical path") -> str:
    """Return the ADR 0101 canonical relative-path projection."""

    if not isinstance(value, str) or not value.startswith(PROJECT_PATH_PREFIX):
        raise SourceClosureTransportError(f"{subject} is not project-scoped")
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError as error:
        raise SourceClosureTransportError(
            f"{subject} violates UTF-8/NFC bounds"
        ) from error
    if (
        len(encoded) > MAXIMUM_LOGICAL_PATH_UTF8_BYTES
        or unicodedata.normalize("NFC", value) != value
    ):
        raise SourceClosureTransportError(f"{subject} violates UTF-8/NFC bounds")
    relative = value.removeprefix(PROJECT_PATH_PREFIX)
    if (
        not relative
        or relative.startswith("/")
        or relative.endswith("/")
        or "\\" in relative
        or "?" in relative
        or "#" in relative
        or any(part in {"", ".", ".."} for part in relative.split("/"))
        or any(ord(character) < 32 or ord(character) == 127 for character in value)
    ):
        raise SourceClosureTransportError(f"{subject} violates ADR 0101 segments")
    return relative


def source_closure_file_id(logical_path: Any) -> str:
    """Derive the ADR 0101 file identity from a logical path."""

    relative = _validated_logical_path(logical_path)
    projection = _canonical_tuple(
        [
            _canonical_string("project"),
            _canonical_string(relative),
            _canonical_string("cxxlens.logical-path.v1"),
        ]
    )
    return "file:sha256:" + hashlib.sha256(
        b"cxxlens\x00file\x00v1\x00" + projection
    ).hexdigest()


def trust_policy_digest(policy: dict[str, Any]) -> str:
    projection = _canonical_tuple(
        [
            _canonical_string(policy["policy_id"]),
            _canonical_string(policy["execution_profile"]),
            _canonical_string(policy["provider_id"]),
            _canonical_string(policy["provider_version"]),
            _canonical_string(policy["semantic_contract_digest"]),
            _canonical_integer(policy["protocol_major"]),
            _canonical_integer(policy["protocol_minor"]),
            _canonical_tuple(
                [_canonical_string(value) for value in policy["required_features"]]
            ),
            _canonical_string(policy["required_qualification"]),
            _canonical_string(policy["worker_sandbox_policy_digest"]),
            _canonical_tuple(
                [
                    _canonical_tuple(
                        [
                            _canonical_string(value["minimum"]),
                            _canonical_string(value["policy_digest"]),
                        ]
                    )
                    for value in policy["task_sandbox_requirements"]
                ]
            ),
        ]
    )
    return semantic_digest_bytes("cxxlens.clang22-installed-native-worker-trust.v1", projection)


def validate_manifest(manifest: dict[str, Any], schema: dict[str, Any]) -> None:
    """Execute the semantic closure rules that JSON Schema cannot express."""
    try:
        jsonschema.Draft202012Validator(schema).validate(manifest)
    except jsonschema.ValidationError as error:
        raise SourceClosureTransportError(f"manifest schema invalid: {error.message}") from error
    if manifest["closure_id"] != "source-closure:" + manifest["closure_digest"]:
        raise SourceClosureTransportError("manifest closure ID/digest mismatch")
    members = manifest["members"]
    blobs = manifest["blobs"]
    for member in members:
        expected_file_id = source_closure_file_id(member["logical_path"])
        if member["file_id"] != expected_file_id:
            raise SourceClosureTransportError(
                "manifest member file_id is not derived from logical_path"
            )
    paths = [entry["logical_path"] for entry in members]
    if paths != sorted(paths, key=lambda value: value.encode("utf-8")) or len(paths) != len(set(paths)):
        raise SourceClosureTransportError("manifest member order or path uniqueness invalid")
    folded = [unicodedata.normalize("NFC", value).casefold() for value in paths]
    if any(unicodedata.normalize("NFC", value) != value for value in paths) or len(folded) != len(set(folded)):
        raise SourceClosureTransportError("manifest NFC/casefold collision")
    if sum(entry["role"] == "main" for entry in members) != 1:
        raise SourceClosureTransportError("manifest must contain exactly one main")
    blob_digests = [entry["content_digest"] for entry in blobs]
    if blob_digests != sorted(blob_digests) or len(blob_digests) != len(set(blob_digests)):
        raise SourceClosureTransportError("manifest blob order or uniqueness invalid")
    by_digest = {entry["content_digest"]: entry for entry in blobs}
    used: set[str] = set()
    for member in members:
        blob = by_digest.get(member["content_digest"])
        if blob is None or blob["size_bytes"] != member["size_bytes"]:
            raise SourceClosureTransportError("manifest member does not resolve to one equal-size blob")
        used.add(member["content_digest"])
    if used != set(by_digest):
        raise SourceClosureTransportError("manifest contains orphan blob")
    if sum(entry["size_bytes"] for entry in blobs) > 48 * 1024 * 1024:
        raise SourceClosureTransportError("manifest aggregate unique blob bound exceeded")
    if manifest["closure_digest"] != closure_digest(members, blobs):
        raise SourceClosureTransportError("manifest ADR 0101 closure digest mismatch")


def validate_reject_control(control: dict[str, Any], contract: dict[str, Any]) -> None:
    expected = set(contract["wire_controls"]["source_closure_reject"]["exact_fields"])
    if set(control) != expected:
        raise SourceClosureTransportError("reject control fields are not exact and closed")
    for field in ("session_id", "task_id", "cleanup_receipt"):
        validate_wire_id(field, control.get(field))
    phase = control.get("failure_phase")
    row = contract["failure_phase_matrix"].get(phase)
    if row is None or control.get("reason_code") not in row["allowed"]:
        raise SourceClosureTransportError("reject reason is unavailable in failure phase")
    if phase == "local-only":
        raise SourceClosureTransportError(
            "local-only failure cannot be serialized as source-closure_reject"
        )
    counters = control.get("observed_counters")
    if not isinstance(counters, dict) or set(counters) != set(row["counters"]):
        raise SourceClosureTransportError("reject counters are not phase-authentic")
    if not all(type(value) is int and 0 <= value <= (1 << 64) - 1 for value in counters.values()):
        raise SourceClosureTransportError("reject counters are not uint64 integers")
    if phase == "before-manifest" and any("byte" in key for key in counters):
        raise SourceClosureTransportError("before-manifest reject fabricates received bytes")


WIRE_ID_PATTERNS = {
    "session_id": re.compile(r"^provider-session:sha256:[0-9a-f]{64}$"),
    "task_id": re.compile(r"^task:semantic-v2:sha256:[0-9a-f]{64}$"),
    "spool_receipt": re.compile(r"^spool-receipt:semantic-v2:sha256:[0-9a-f]{64}$"),
    "cleanup_owner": re.compile(r"^cleanup-owner:semantic-v2:sha256:[0-9a-f]{64}$"),
    "cleanup_receipt": re.compile(r"^cleanup-receipt:semantic-v2:sha256:[0-9a-f]{64}$"),
}


def validate_wire_id(field: str, value: Any) -> None:
    pattern = WIRE_ID_PATTERNS[field]
    if not isinstance(value, str) or pattern.fullmatch(value) is None:
        raise SourceClosureTransportError(f"wire control {field} is not a typed ID")


def validate_wire_control(
    name: str,
    control: dict[str, Any],
    payload: bytes,
    state: str,
    contract: dict[str, Any],
) -> None:
    """Validate one non-reject control before allocation, I/O, or state mutation."""
    if type(payload) is not bytes:
        raise SourceClosureTransportError("wire payload is not an exact byte string")
    controls = contract["wire_controls"]
    if name == "source_closure_reject":
        if payload:
            raise SourceClosureTransportError("reject control payload must be empty")
        validate_reject_control(control, contract)
        return
    spec = controls.get(name)
    if name == "source_closure_manifest":
        if not isinstance(control, dict) or control.get("kind") not in {"descriptor", "chunk"}:
            raise SourceClosureTransportError("manifest control discriminant invalid")
        spec = spec[control["kind"]]
    if not isinstance(spec, dict) or not isinstance(control, dict) or set(control) != set(spec["exact_fields"]):
        raise SourceClosureTransportError("wire control fields are not exact and closed")

    legal_states = {
        ("source_closure_manifest", "descriptor"): {"task-v4-sealed"},
        ("source_closure_manifest", "chunk"): {"manifest-open", "manifest-streaming"},
        ("source_closure_blob", None): {"manifest-validated", "blob-sealed"},
        ("source_closure_chunk", None): {"blob-open", "blob-streaming"},
        ("source_closure_seal", None): {"blob-sealed"},
        ("source_closure_ack", None): {"closure-sealed"},
    }
    if state not in legal_states[(name, control.get("kind"))]:
        raise SourceClosureTransportError("wire control is illegal in current state")
    for field, value in control.items():
        if field in {"total_bytes", "chunk_bytes", "chunk_count", "chunk_index", "offset", "byte_count", "blob_ordinal", "blob_count"}:
            if type(value) is not int or not 0 <= value <= (1 << 64) - 1:
                raise SourceClosureTransportError(f"wire control {field} is not uint64")
        elif field.endswith("digest"):
            prefix = "sha256:" if field == "blob_digest" else "semantic-v2:sha256:"
            if not isinstance(value, str) or not re.fullmatch(re.escape(prefix) + r"[0-9a-f]{64}", value):
                raise SourceClosureTransportError(f"wire control {field} is not a typed digest")
        elif field == "closure_id":
            if not isinstance(value, str) or not re.fullmatch(r"source-closure:semantic-v2:sha256:[0-9a-f]{64}", value):
                raise SourceClosureTransportError("wire control closure_id is not a source closure ID")
        elif field in {"session_id", "task_id", "spool_receipt", "cleanup_owner"}:
            validate_wire_id(field, value)
    if spec["payload"] == "empty" and payload:
        raise SourceClosureTransportError("wire control payload must be empty")
    if spec["payload"] == "exact-byte-count-frame-digest":
        if len(payload) != control["byte_count"] or not payload:
            raise SourceClosureTransportError("wire chunk payload byte count mismatch")
        if control["byte_count"] > contract["limits"]["maximum_chunk_payload_bytes"]:
            raise SourceClosureTransportError("wire chunk payload exceeds bound")
    if "chunk_bytes" in control and not 0 < control["chunk_bytes"] <= contract["limits"]["maximum_chunk_payload_bytes"]:
        raise SourceClosureTransportError("wire descriptor chunk bound invalid")
    if "chunk_count" in control:
        maximum_count = contract["limits"]["maximum_manifest_chunks"] if name == "source_closure_manifest" else contract["limits"]["maximum_chunks_per_blob"]
        maximum_bytes = contract["limits"]["maximum_manifest_bytes"] if name == "source_closure_manifest" else contract["limits"]["maximum_blob_bytes"]
        if control["total_bytes"] > maximum_bytes:
            raise SourceClosureTransportError("wire descriptor total byte bound exceeded")
        if name == "source_closure_manifest" and control["total_bytes"] == 0:
            raise SourceClosureTransportError("wire manifest requires one or more chunks")
        expected_count = 0 if control["total_bytes"] == 0 else (control["total_bytes"] + control["chunk_bytes"] - 1) // control["chunk_bytes"]
        if control["chunk_count"] != expected_count or control["chunk_count"] > maximum_count:
            raise SourceClosureTransportError("wire descriptor chunk count invalid")


class TransferStateWitness:
    """Executable exact-binding witness for one manifest and its canonical blob stream."""

    @classmethod
    def for_task_extension(
        cls,
        *,
        session_id: str,
        task_extension: dict[str, Any],
        manifest_schema: dict[str, Any],
    ) -> "TransferStateWitness":
        """Bind a transfer to the already-sealed outer task-v4 extension."""

        if not isinstance(task_extension, dict):
            raise SourceClosureTransportError(
                "outer task-v4 extension transfer binding is missing"
            )
        task_id = task_extension.get("task_id")
        task_v4_digest = task_extension.get("task_v4_digest")
        source_closure = task_extension.get("source_closure")
        if not isinstance(source_closure, dict):
            raise SourceClosureTransportError(
                "outer task-v4 extension source closure binding is missing"
            )
        closure_id = source_closure.get("id")
        closure_digest = source_closure.get("digest")
        sealed_manifest_digest = source_closure.get("manifest_digest")
        validate_wire_id("task_id", task_id)
        if (
            not isinstance(task_v4_digest, str)
            or re.fullmatch(r"semantic-v2:sha256:[0-9a-f]{64}", task_v4_digest)
            is None
            or task_id != "task:" + task_v4_digest
            or not isinstance(closure_id, str)
            or re.fullmatch(
                r"source-closure:semantic-v2:sha256:[0-9a-f]{64}",
                closure_id,
            )
            is None
            or not isinstance(closure_digest, str)
            or re.fullmatch(
                r"semantic-v2:sha256:[0-9a-f]{64}", closure_digest
            )
            is None
            or closure_id != "source-closure:" + closure_digest
            or not isinstance(sealed_manifest_digest, str)
            or re.fullmatch(
                r"semantic-v2:sha256:[0-9a-f]{64}",
                sealed_manifest_digest,
            )
            is None
        ):
            raise SourceClosureTransportError(
                "outer task-v4 extension transfer identity is invalid"
            )
        return cls(
            session_id=session_id,
            task_id=task_id,
            task_v4_digest=task_v4_digest,
            closure_id=closure_id,
            closure_digest=closure_digest,
            manifest_digest=sealed_manifest_digest,
            manifest_schema=manifest_schema,
        )

    def __init__(self, *, session_id: str, task_id: str, task_v4_digest: str,
                 closure_id: str, closure_digest: str, manifest_digest: str,
                 manifest_schema: dict[str, Any]) -> None:
        self.expected = {
            "session_id": session_id,
            "task_id": task_id,
            "task_v4_digest": task_v4_digest,
            "closure_id": closure_id,
            "closure_digest": closure_digest,
            "manifest_digest": manifest_digest,
        }
        self.manifest_schema = manifest_schema
        self.state = "task-v4-sealed"
        self.next_index = 0
        self.next_offset = 0
        self.declared_bytes = 0
        self.declared_chunk_bytes = 0
        self.current_blob_ordinal = 0
        self.current_blob_digest: str | None = None
        self.declared_chunk_count = 0
        self.manifest_bytes = bytearray()
        self.blob_bytes = bytearray()
        self.manifest: dict[str, Any] | None = None
        self.blob_receipts: list[dict[str, Any]] = []
        self.completed_blobs = 0
        self.total_blob_bytes = 0
        self.transfer_digest: str | None = None

    def _bind(self, control: dict[str, Any], fields: tuple[str, ...]) -> None:
        if any(control.get(field) != self.expected[field] for field in fields):
            raise SourceClosureTransportError("wire state witness identity binding mismatch")

    def apply(self, name: str, control: dict[str, Any], payload: bytes,
              contract: dict[str, Any]) -> None:
        if self.state in {"closure-acknowledged", "rejected"}:
            raise SourceClosureTransportError("wire frame follows terminal state")
        validate_wire_control(name, control, payload, self.state, contract)
        self._bind(control, tuple(field for field in ("session_id", "task_id") if field in control))
        if name == "source_closure_reject":
            phase_by_state = {
                "task-v4-sealed": "before-manifest",
                "manifest-open": "manifest-streaming",
                "manifest-streaming": "manifest-streaming",
                "manifest-validated": "manifest-validated",
                "blob-open": "blob-streaming",
                "blob-streaming": "blob-streaming",
                "blob-sealed": "blob-streaming",
                "closure-sealed": "closure-sealed",
            }
            if control["failure_phase"] != phase_by_state.get(self.state):
                raise SourceClosureTransportError("reject phase is not current-state authentic")
            self.state = "rejected"
            return
        if name == "source_closure_manifest" and control["kind"] == "descriptor":
            self._bind(control, ("task_v4_digest", "closure_id", "closure_digest", "manifest_digest"))
            self.declared_bytes = control["total_bytes"]
            self.declared_chunk_bytes = control["chunk_bytes"]
            self.declared_chunk_count = control["chunk_count"]
            self.manifest_bytes.clear()
            self.next_index = self.next_offset = 0
            self.state = "manifest-open"
        elif name == "source_closure_manifest":
            self._bind(control, ("manifest_digest",))
            if control["chunk_index"] != self.next_index or control["offset"] != self.next_offset:
                raise SourceClosureTransportError("wire manifest chunk is not contiguous")
            expected_bytes = min(self.declared_chunk_bytes, self.declared_bytes - self.next_offset)
            if control["byte_count"] != expected_bytes:
                raise SourceClosureTransportError("wire manifest chunk size differs from descriptor")
            self.next_index += 1
            self.next_offset += control["byte_count"]
            self.manifest_bytes.extend(payload)
            if self.next_offset > self.declared_bytes:
                raise SourceClosureTransportError("wire manifest chunk exceeds declaration")
            if self.next_offset == self.declared_bytes:
                if self.next_index != self.declared_chunk_count:
                    raise SourceClosureTransportError("wire manifest chunk census mismatch")
                try:
                    parsed = json.loads(bytes(self.manifest_bytes))
                except (UnicodeDecodeError, json.JSONDecodeError) as error:
                    raise SourceClosureTransportError("wire manifest is not canonical JSON") from error
                if canonical_json(parsed) != bytes(self.manifest_bytes):
                    raise SourceClosureTransportError("wire manifest bytes are not canonical")
                if not isinstance(parsed, dict) or set(parsed) != {"schema", "closure_id", "closure_digest", "members", "blobs"}:
                    raise SourceClosureTransportError("wire manifest shape is not exact")
                validate_manifest(parsed, self.manifest_schema)
                if (manifest_digest(parsed) != self.expected["manifest_digest"] or
                        parsed["closure_id"] != self.expected["closure_id"] or
                        parsed["closure_digest"] != self.expected["closure_digest"] or
                        closure_digest(parsed["members"], parsed["blobs"]) != parsed["closure_digest"]):
                    raise SourceClosureTransportError("wire manifest semantic digest mismatch")
                self.manifest = parsed
                self.state = "manifest-validated"
            else:
                self.state = "manifest-streaming"
        elif name == "source_closure_blob":
            self._bind(control, ("closure_digest",))
            if self.manifest is None or control["blob_ordinal"] >= len(self.manifest["blobs"]):
                raise SourceClosureTransportError("wire blob is absent from manifest")
            expected_blob = self.manifest["blobs"][control["blob_ordinal"]]
            if control["blob_ordinal"] != self.completed_blobs:
                raise SourceClosureTransportError("wire blob order is not canonical")
            if (control["blob_digest"] != expected_blob["content_digest"] or
                    control["total_bytes"] != expected_blob["size_bytes"]):
                raise SourceClosureTransportError("wire blob descriptor differs from manifest")
            self.current_blob_ordinal = control["blob_ordinal"]
            self.current_blob_digest = control["blob_digest"]
            self.declared_bytes = control["total_bytes"]
            self.declared_chunk_bytes = control["chunk_bytes"]
            self.declared_chunk_count = control["chunk_count"]
            self.blob_bytes.clear()
            self.next_index = self.next_offset = 0
            self.state = "blob-sealed" if self.declared_bytes == 0 else "blob-open"
            if self.declared_bytes == 0:
                if control["blob_digest"] != "sha256:" + hashlib.sha256(b"").hexdigest():
                    raise SourceClosureTransportError("wire empty blob digest mismatch")
                self.blob_receipts.append({"blob_ordinal": control["blob_ordinal"], "blob_digest": control["blob_digest"], "size_bytes": 0})
                self.completed_blobs += 1
        elif name == "source_closure_chunk":
            if (control["blob_ordinal"] != self.current_blob_ordinal or
                    control["blob_digest"] != self.current_blob_digest or
                    control["chunk_index"] != self.next_index or
                    control["offset"] != self.next_offset):
                raise SourceClosureTransportError("wire blob chunk binding/order mismatch")
            expected_bytes = min(self.declared_chunk_bytes, self.declared_bytes - self.next_offset)
            if control["byte_count"] != expected_bytes:
                raise SourceClosureTransportError("wire blob chunk size differs from descriptor")
            self.next_index += 1
            self.next_offset += control["byte_count"]
            self.blob_bytes.extend(payload)
            if self.next_offset > self.declared_bytes:
                raise SourceClosureTransportError("wire blob chunk exceeds declaration")
            if self.next_offset == self.declared_bytes:
                if self.next_index != self.declared_chunk_count:
                    raise SourceClosureTransportError("wire blob chunk census mismatch")
                observed_digest = "sha256:" + hashlib.sha256(self.blob_bytes).hexdigest()
                if observed_digest != self.current_blob_digest:
                    raise SourceClosureTransportError("wire blob content digest mismatch")
                self.blob_receipts.append({"blob_ordinal": self.current_blob_ordinal, "blob_digest": observed_digest, "size_bytes": self.declared_bytes})
                self.completed_blobs += 1
                self.total_blob_bytes += self.declared_bytes
                self.state = "blob-sealed"
            else:
                self.state = "blob-streaming"
        elif name == "source_closure_seal":
            self._bind(control, ("task_v4_digest", "manifest_digest", "closure_digest"))
            if self.manifest is None or self.completed_blobs != len(self.manifest["blobs"]):
                raise SourceClosureTransportError("wire seal precedes complete manifest blob census")
            receipts_digest = blob_receipts_digest(self.blob_receipts)
            projection = {
                "session_id": self.expected["session_id"],
                "task_id": self.expected["task_id"],
                "task_v4_digest": self.expected["task_v4_digest"],
                "manifest_digest": self.expected["manifest_digest"],
                "blob_receipts_digest": receipts_digest,
                "blob_count": self.completed_blobs,
                "total_bytes": self.total_blob_bytes,
                "closure_digest": self.expected["closure_digest"],
            }
            computed_transfer = transfer_digest(projection)
            if control["blob_count"] != self.completed_blobs or control["total_bytes"] != self.total_blob_bytes:
                raise SourceClosureTransportError("wire seal census mismatch")
            if control["blob_receipts_digest"] != receipts_digest or control["transfer_digest"] != computed_transfer:
                raise SourceClosureTransportError("wire seal digest mismatch")
            self.transfer_digest = computed_transfer
            self.state = "closure-sealed"
        elif name == "source_closure_ack":
            self._bind(control, ("closure_digest",))
            if control["transfer_digest"] != self.transfer_digest:
                raise SourceClosureTransportError("wire ack transfer binding mismatch")
            self.state = "closure-acknowledged"


def blob_receipts_digest(receipts: list[dict[str, Any]]) -> str:
    return semantic_digest("cxxlens.source-closure-blob-receipts.v1", receipts)


def transfer_digest(projection: dict[str, Any]) -> str:
    expected = [
        "session_id", "task_id", "task_v4_digest", "manifest_digest",
        "blob_receipts_digest", "blob_count", "total_bytes", "closure_digest",
    ]
    if set(projection) != set(expected):
        raise SourceClosureTransportError("transfer digest projection is not exact closed")
    return semantic_digest("cxxlens.source-closure-transfer.v1", projection)


def task_v4_projection(extension: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in extension.items() if key not in {"task_id", "task_v4_digest"}}


def request_v2_2_projection(request: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema": request["schema"],
        "request_version": request["request_version"],
        "required_features": request["required_features"],
        "inherited_authority": {key: request[key] for key in ("materialization_request_id", "semantic_request_digest", "tool", "worker", "project", "registry", "engine", "interpretation_policy", "trust_policy", "group_topology", "tasks", "publication")},
        "source_closures": request["source_closures"],
        "task_extensions": [task_v4_projection(value) for value in request["task_extensions"]],
    }


def load(path: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise SourceClosureTransportError(f"expected mapping: {path}")
    return value


def _validate_logical_path(value: Any) -> None:
    _validated_logical_path(value, subject="task v4 logical path")


def validate_request_binding(
    request: dict[str, Any], manifests: list[dict[str, Any]]
) -> None:
    """Validate cross-document v2.2 relationships JSON Schema cannot express."""

    base_tasks = request.get("tasks")
    closures = request.get("source_closures")
    extensions = request.get("task_extensions")
    if not all(isinstance(value, list) for value in (base_tasks, closures, extensions)):
        raise SourceClosureTransportError("request 2.2 relationship inputs are missing")
    if len(base_tasks) != len(extensions):
        raise SourceClosureTransportError("task v4/base task index parity mismatch")
    worker = request.get("worker")
    trust = request.get("trust_policy")
    if not isinstance(worker, dict) or not isinstance(trust, dict):
        raise SourceClosureTransportError("request 2.2 worker/trust authority missing")
    parity = {
        "provider_id": "provider_id",
        "provider_version": "provider_version",
        "semantic_contract_digest": "semantic_contract_digest",
        "protocol_major": "protocol_major",
        "protocol_minor": "protocol_minor",
        "required_features": "required_features",
        "worker_sandbox_policy_digest": "sandbox_policy_digest",
    }
    if any(trust.get(left) != worker.get(right) for left, right in parity.items()):
        raise SourceClosureTransportError("request 2.2 worker/trust parity mismatch")
    sandbox_requirements = sorted(
        {
            (
                task.get("sandbox", {}).get("minimum"),
                task.get("sandbox", {}).get("policy_digest"),
            )
            for task in base_tasks
        }
    )
    expected_requirements = [
        {"minimum": minimum, "policy_digest": digest}
        for minimum, digest in sandbox_requirements
    ]
    if trust.get("task_sandbox_requirements") != expected_requirements or trust.get(
        "trust_policy_digest"
    ) != trust_policy_digest(trust):
        raise SourceClosureTransportError("request 2.2 trust policy semantic digest mismatch")

    closure_by_id: dict[str, dict[str, Any]] = {}
    for closure in closures:
        identifier = closure.get("source_closure_id")
        digest = closure.get("source_closure_digest")
        if identifier in closure_by_id:
            raise SourceClosureTransportError("duplicate source closure ID")
        if identifier != f"source-closure:{digest}":
            raise SourceClosureTransportError("source closure ID/digest mismatch")
        closure_by_id[identifier] = closure

    manifest_by_id: dict[str, dict[str, Any]] = {}
    for manifest in manifests:
        identifier = manifest.get("closure_id")
        if identifier in manifest_by_id:
            raise SourceClosureTransportError("duplicate source closure manifest ID")
        manifest_by_id[identifier] = manifest
    if set(manifest_by_id) != set(closure_by_id):
        raise SourceClosureTransportError("request source closure/manifest census mismatch")
    for identifier, closure in closure_by_id.items():
        manifest = manifest_by_id[identifier]
        members = manifest.get("members", [])
        blobs = manifest.get("blobs", [])
        if (
            manifest.get("closure_digest") != closure.get("source_closure_digest")
            or manifest_digest(manifest) != closure.get("manifest_digest")
            or closure.get("member_count") != len(members)
            or closure.get("blob_count") != len(blobs)
            or closure.get("unique_blob_bytes")
            != sum(blob.get("size_bytes", -1) for blob in blobs)
        ):
            raise SourceClosureTransportError(
                "request source closure census/manifest binding mismatch"
            )

    task_ids: set[str] = set()
    indices: set[int] = set()
    referenced_closures: set[str] = set()
    for extension in extensions:
        task_id = extension.get("task_id")
        index = extension.get("base_task_index")
        if task_id in task_ids or index in indices:
            raise SourceClosureTransportError("duplicate task v4 ID or base index")
        if not isinstance(index, int) or index < 0 or index >= len(base_tasks):
            raise SourceClosureTransportError("task v4 base index is invalid")
        base = base_tasks[index]
        if extension.get("base_provider_task_id") != base.get("provider_task_id"):
            raise SourceClosureTransportError("task v4/base provider task identity mismatch")
        if extension.get("base_task_digest") != content_projection_digest(base):
            raise SourceClosureTransportError("task v4/base task digest mismatch")
        expected_open = {
            field: base.get(field)
            for field in (
                "task_input_digest",
                "normalized_invocation_digest",
                "toolchain_digest",
                "environment_digest",
            )
        }
        if extension.get("open_task") != expected_open:
            raise SourceClosureTransportError("task v4 open-task authority mismatch")
        _validate_logical_path(extension.get("main_logical_path"))
        _validate_logical_path(extension.get("logical_working_directory"))
        if extension.get("main_logical_path") != base.get("source", {}).get(
            "logical_path"
        ) or extension.get("logical_working_directory") != base.get(
            "working_directory"
        ):
            raise SourceClosureTransportError("task v4 base path binding mismatch")
        reference = extension.get("source_closure", {})
        closure = closure_by_id.get(reference.get("id"))
        if closure is None or reference.get("digest") != closure.get(
            "source_closure_digest"
        ) or reference.get("manifest_digest") != closure.get("manifest_digest"):
            raise SourceClosureTransportError("task v4 closure reference does not resolve exactly")
        manifest_main = [
            member
            for member in manifest_by_id[reference["id"]]["members"]
            if member["role"] == "main"
        ]
        base_source = base.get("source", {})
        if len(manifest_main) != 1 or {
            field: manifest_main[0].get(field)
            for field in (
                "file_id", "logical_path", "content_digest", "size_bytes",
                "encoding", "read_only",
            )
        } != {
            field: base_source.get(field)
            for field in (
                "file_id", "logical_path", "content_digest", "size_bytes",
                "encoding", "read_only",
            )
        }:
            raise SourceClosureTransportError("task v4 manifest main source binding mismatch")
        expected_task_digest = semantic_digest(
            "cxxlens.clang22.task.v4", task_v4_projection(extension)
        )
        if extension.get("task_v4_digest") != expected_task_digest or task_id != (
            "task:" + expected_task_digest
        ):
            raise SourceClosureTransportError("task v4 semantic identity mismatch")
        task_ids.add(task_id)
        indices.add(index)
        referenced_closures.add(reference["id"])
    if indices != set(range(len(base_tasks))):
        raise SourceClosureTransportError("task v4/base task index set is incomplete")
    if referenced_closures != set(closure_by_id):
        raise SourceClosureTransportError("request contains an unreferenced source closure")
    expected_request_digest = semantic_digest(
        "cxxlens.clang22.materialization-request.v2_2",
        request_v2_2_projection(request),
    )
    if request.get("request_digest") != expected_request_digest or request.get(
        "request_id"
    ) != ("materialization-request:" + expected_request_digest):
        raise SourceClosureTransportError("request v2.2 semantic identity mismatch")


def complete_request_witness(root: pathlib.Path) -> tuple[dict[str, Any], dict[str, Any]]:
    """Build one complete current request from the materialization witness."""

    sys.path.insert(0, str(root / "tools" / "quality"))
    import check_ng_clang22_materialization as materialization  # noqa: PLC0415

    fixture_root = (
        root if (root / "schemas/cxxlens_ng_relation_registry.yaml").exists() else ROOT
    )
    request = materialization.sample_request(fixture_root)
    base = request["tasks"][0]
    source = base["source"]
    source["read_only"] = True
    source.pop("content_base64", None)
    member = {
        field: source[field]
        for field in (
            "file_id", "logical_path", "encoding", "size_bytes",
            "content_digest", "read_only",
        )
    }
    member["role"] = "main"
    blob = {field: source[field] for field in ("content_digest", "size_bytes")}
    digest = closure_digest([member], [blob])
    manifest = {
        "schema": SOURCE_CLOSURE_MANIFEST_SCHEMA,
        "closure_id": "source-closure:" + digest,
        "closure_digest": digest,
        "members": [member],
        "blobs": [blob],
    }
    sealed_manifest_digest = manifest_digest(manifest)
    closure = {
        "source_closure_id": manifest["closure_id"],
        "source_closure_digest": digest,
        "manifest_digest": sealed_manifest_digest,
        "member_count": 1,
        "blob_count": 1,
        "unique_blob_bytes": source["size_bytes"],
    }

    required_features = ["task-input-chunks-v2", "task-source-closure-v2"]
    request["schema"] = "cxxlens.clang22-materialization-request.v2_2"
    request["request_version"] = "2.2.0"
    request["required_features"] = required_features
    request["worker"]["protocol_minor"] = 0
    request["worker"]["required_features"] = required_features
    request["trust_policy"]["protocol_minor"] = 0
    request["trust_policy"]["required_features"] = required_features
    request["trust_policy"]["task_sandbox_requirements"] = [base["sandbox"]]
    request["trust_policy"]["trust_policy_digest"] = trust_policy_digest(
        request["trust_policy"]
    )
    extension = {
        "schema": "cxxlens.clang22.task.v4",
        "base_task_index": 0,
        "base_provider_task_id": base["provider_task_id"],
        "base_task_digest": content_projection_digest(base),
        "open_task": {
            field: base[field]
            for field in (
                "task_input_digest", "normalized_invocation_digest",
                "toolchain_digest", "environment_digest",
            )
        },
        "source_closure": {
            "id": closure["source_closure_id"],
            "digest": digest,
            "manifest_digest": sealed_manifest_digest,
        },
        "main_logical_path": source["logical_path"],
        "logical_working_directory": base["working_directory"],
    }
    extension["task_v4_digest"] = semantic_digest(
        "cxxlens.clang22.task.v4", task_v4_projection(extension)
    )
    extension["task_id"] = "task:" + extension["task_v4_digest"]
    request["source_closures"] = [closure]
    request["task_extensions"] = [extension]
    request["request_digest"] = semantic_digest(
        "cxxlens.clang22.materialization-request.v2_2",
        request_v2_2_projection(request),
    )
    request["request_id"] = "materialization-request:" + request["request_digest"]
    return request, manifest


def validate(root: pathlib.Path) -> dict[str, Any]:
    contract = load(root / CONTRACT)
    schema = load(root / SCHEMA)
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(contract)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise SourceClosureTransportError(
            f"transport schema validation failed: {error.message}"
        ) from error

    protocol = load(root / PROTOCOL)
    protocol_schema = load(root / PROTOCOL_SCHEMA)
    request = load(root / REQUEST)
    task = load(root / TASK)
    manifest_schema = load(root / MANIFEST_SCHEMA)
    if manifest_schema.get("x-cxxlens-closure-digest-domain") != SOURCE_CLOSURE_DIGEST_DOMAIN or manifest_schema.get(
        "x-cxxlens-manifest-digest-domain"
    ) != SOURCE_CLOSURE_MANIFEST_DIGEST_DOMAIN:
        raise SourceClosureTransportError(
            "source-closure manifest schema digest domains are not the active product domains"
        )
    try:
        jsonschema.Draft202012Validator.check_schema(request)
        jsonschema.Draft202012Validator.check_schema(task)
        jsonschema.Draft202012Validator.check_schema(protocol_schema)
        jsonschema.Draft202012Validator(protocol_schema).validate(protocol)
        jsonschema.Draft202012Validator.check_schema(manifest_schema)
    except jsonschema.SchemaError as error:
        raise SourceClosureTransportError(
            f"request/task schema is invalid: {error.message}"
        ) from error
    witness, witness_manifest = complete_request_witness(root)
    # v2.2 reuses the common typed definitions from the request schema. This
    # is a schema-reference dependency, not a compatibility or fallback path.
    request_base_schema = load(
        root / "schemas/cxxlens_ng_clang22_materialization_request.schema.yaml"
    )
    schema_store = {
        request["$id"]: request,
        request_base_schema["$id"]: request_base_schema,
        task["$id"]: task,
        "https://cxxlens.dev/schemas/cxxlens_ng_provider_task_v4.schema.yaml": task,
    }
    try:
        resolver = jsonschema.RefResolver.from_schema(request, store=schema_store)
        jsonschema.Draft202012Validator(request, resolver=resolver).validate(
            witness
        )
        validate_manifest(witness_manifest, manifest_schema)
        validate_request_binding(witness, [witness_manifest])
    except (jsonschema.ValidationError, SourceClosureTransportError) as error:
        message = (
            error.message
            if isinstance(error, jsonschema.ValidationError)
            else str(error)
        )
        raise SourceClosureTransportError(
            f"complete request 2.2 constructibility witness failed: {message}"
        ) from error

    outer_extension = witness["task_extensions"][0]
    outer_manifest_bytes = canonical_json(witness_manifest)
    outer_descriptor = {
        "kind": "descriptor",
        "session_id": "provider-session:sha256:" + "4" * 64,
        "task_id": outer_extension["task_id"],
        "task_v4_digest": outer_extension["task_v4_digest"],
        "closure_id": outer_extension["source_closure"]["id"],
        "closure_digest": outer_extension["source_closure"]["digest"],
        "manifest_digest": outer_extension["source_closure"]["manifest_digest"],
        "total_bytes": len(outer_manifest_bytes),
        "chunk_bytes": len(outer_manifest_bytes),
        "chunk_count": 1,
    }
    outer_transfer = TransferStateWitness.for_task_extension(
        session_id=outer_descriptor["session_id"],
        task_extension=outer_extension,
        manifest_schema=manifest_schema,
    )
    outer_transfer.apply(
        "source_closure_manifest", outer_descriptor, b"", contract
    )
    if outer_transfer.state != "manifest-open":
        raise SourceClosureTransportError(
            "outer task-v4 extension did not admit its exact transfer descriptor"
        )
    foreign_descriptor = dict(outer_descriptor)
    foreign_descriptor["task_id"] = (
        "task:semantic-v2:sha256:" + "f" * 64
    )
    foreign_transfer = TransferStateWitness.for_task_extension(
        session_id=outer_descriptor["session_id"],
        task_extension=outer_extension,
        manifest_schema=manifest_schema,
    )
    try:
        foreign_transfer.apply(
            "source_closure_manifest", foreign_descriptor, b"", contract
        )
    except SourceClosureTransportError as error:
        if "identity binding mismatch" not in str(error):
            raise
    else:
        raise SourceClosureTransportError(
            "wire transfer accepted cross-task rebinding"
        )
    protocol_ids = [entry["id"] for entry in protocol["message_types"]["registry"]]
    proposed = contract["message_registry"]["proposed"]
    proposed_ids = [entry["id"] for entry in proposed]
    if len(protocol_ids) != len(set(protocol_ids)) or len(proposed_ids) != len(set(proposed_ids)):
        raise SourceClosureTransportError("duplicate message ID")
    if proposed_ids != list(range(24, 30)):
        raise SourceClosureTransportError("source-closure message IDs must be contiguous 24 through 29")
    if contract["message_registry"]["preserved"] != {"heartbeat": 23}:
        raise SourceClosureTransportError("heartbeat 23 is not preserved")

    versions = contract["versions"]
    if versions != {
        "provider_protocol": {"major": 2, "minor": 0, "downgrade": "reject"},
        "request": {"current": 2.2},
        "task": {"current": "cxxlens.clang22.task.v4"},
    }:
        raise SourceClosureTransportError("version or downgrade contract drift")
    if request["properties"]["required_features"].get("const") != [
        "task-input-chunks-v2",
        "task-source-closure-v2",
    ]:
        raise SourceClosureTransportError("request 2.2 omits exact source-closure capability")
    trust = request["properties"]["trust_policy"]["properties"]
    if trust["protocol_minor"].get("const") != 0 or trust["required_features"].get("const") != [
        "task-input-chunks-v2", "task-source-closure-v2"
    ]:
        raise SourceClosureTransportError("request 2.2 trust policy is not Protocol 2.0")
    request_required = set(request["required"])
    if not {"tasks", "source_closures", "task_extensions"}.issubset(
        request_required
    ):
        raise SourceClosureTransportError("request 2.2 omits source-closure authority")
    if "base_request_v2_2" in request.get("properties", {}) or request["properties"]["tasks"].get("items", {}).get("$ref") != "#/$defs/base_task_without_source_bytes":
        raise SourceClosureTransportError("request 2.2 nests an invalid task projection")
    source_properties = request["$defs"]["base_task_without_source_bytes"]["properties"]["source"]["properties"]
    if "content_base64" in source_properties or request["properties"]["worker"]["properties"]["protocol_minor"].get("const") != 0:
        raise SourceClosureTransportError("request 2.2 source bytes or protocol authority drift")
    source_id_pattern = request["$defs"]["source_closure_id"].get("pattern")
    if source_id_pattern != r"^source-closure:semantic-v2:sha256:[0-9a-f]{64}$":
        raise SourceClosureTransportError("source closure ID grammar differs from ADR 0101")
    task_required = set(task["required"])
    if not {"base_task_digest", "open_task", "source_closure"}.issubset(
        task_required
    ):
        raise SourceClosureTransportError("task v4 omits inherited task/open-task authority")
    path_contract = task["$defs"]["logical_path"]
    if path_contract.get("x-cxxlens-max-utf8-bytes") != 4096 or not any(
        entry.get("not", {}).get("pattern") == r"(^|/)\.{1,2}(/|$)"
        for entry in path_contract.get("allOf", [])
    ) or not any(
        entry.get("not", {}).get("pattern") == r"^project://.*//.*$"
        for entry in path_contract.get("allOf", [])
    ):
        raise SourceClosureTransportError("task v4 path does not bind ADR 0101 byte/segment rules")

    controls = contract["wire_controls"]
    expected_fields = {
        "source_closure_blob": ["session_id", "task_id", "closure_digest", "blob_ordinal", "blob_digest", "total_bytes", "chunk_bytes", "chunk_count"],
        "source_closure_chunk": ["session_id", "task_id", "blob_ordinal", "blob_digest", "chunk_index", "offset", "byte_count"],
        "source_closure_seal": ["session_id", "task_id", "task_v4_digest", "manifest_digest", "blob_receipts_digest", "blob_count", "total_bytes", "closure_digest", "transfer_digest"],
        "source_closure_ack": ["session_id", "task_id", "closure_digest", "transfer_digest", "spool_receipt", "cleanup_owner"],
        "source_closure_reject": ["session_id", "task_id", "failure_phase", "reason_code", "observed_counters", "cleanup_receipt"],
    }
    for name, fields in expected_fields.items():
        if controls[name].get("exact_fields") != fields:
            raise SourceClosureTransportError(f"wire control field drift: {name}")
    manifest = controls["source_closure_manifest"]
    if manifest != {
        "descriptor": {
            "kind": "descriptor",
            "exact_fields": ["kind", "session_id", "task_id", "task_v4_digest", "closure_id", "closure_digest", "manifest_digest", "total_bytes", "chunk_bytes", "chunk_count"],
            "payload": "empty",
        },
        "chunk": {
            "kind": "chunk",
            "exact_fields": ["kind", "session_id", "task_id", "manifest_digest", "chunk_index", "offset", "byte_count"],
            "payload": "exact-byte-count-frame-digest",
        },
    }:
        raise SourceClosureTransportError("manifest descriptor/chunk discriminant is missing")
    common = controls["common"]
    if common.get("encoding") != "deterministic-cbor-closed-map" or common.get(
        "sequence"
    ) != "contiguous-shared-session-sequence":
        raise SourceClosureTransportError("wire canonical encoding or sequence drift")
    if common["control_bytes"] != protocol["wire"]["limits"]["control_bytes"]:
        raise SourceClosureTransportError(
            "source-closure and provider control-byte limits drift"
        )

    maximum_blob_count = contract["limits"]["maximum_unique_blobs"]
    maximum_total_blob_bytes = contract["limits"]["maximum_unique_blob_bytes"]
    if maximum_total_blob_bytes % maximum_blob_count:
        raise SourceClosureTransportError(
            "maximum receipt witness cannot evenly cover unique blob bytes"
        )
    maximum_receipt_size = maximum_total_blob_bytes // maximum_blob_count
    if maximum_receipt_size > contract["limits"]["maximum_blob_bytes"]:
        raise SourceClosureTransportError(
            "maximum receipt witness exceeds per-blob byte bound"
        )
    maximum_receipts = [
        {
            "blob_ordinal": index,
            "blob_digest": "sha256:" + f"{index:064x}",
            "size_bytes": maximum_receipt_size,
        }
        for index in range(maximum_blob_count)
    ]
    maximum_session_id = "provider-session:sha256:" + "f" * 64
    maximum_task_id = "task:semantic-v2:sha256:" + "e" * 64
    maximum_task_digest = "semantic-v2:sha256:" + "d" * 64
    maximum_manifest_digest = "semantic-v2:sha256:" + "c" * 64
    maximum_closure_digest = "semantic-v2:sha256:" + "b" * 64
    maximum_receipts_digest = blob_receipts_digest(maximum_receipts)
    maximum_seal = {
        "session_id": maximum_session_id,
        "task_id": maximum_task_id,
        "task_v4_digest": maximum_task_digest,
        "manifest_digest": maximum_manifest_digest,
        "blob_receipts_digest": maximum_receipts_digest,
        "blob_count": maximum_blob_count,
        "total_bytes": maximum_total_blob_bytes,
        "closure_digest": maximum_closure_digest,
    }
    maximum_seal["transfer_digest"] = transfer_digest(
        {
            "session_id": maximum_session_id,
            "task_id": maximum_task_id,
            "task_v4_digest": maximum_task_digest,
            "manifest_digest": maximum_manifest_digest,
            "blob_receipts_digest": maximum_receipts_digest,
            "blob_count": maximum_seal["blob_count"],
            "total_bytes": maximum_seal["total_bytes"],
            "closure_digest": maximum_closure_digest,
        }
    )
    encoded_seal_bytes = len(cbor_encode(maximum_seal))
    if encoded_seal_bytes > common["control_bytes"]:
        raise SourceClosureTransportError(
            "maximum message-27 control exceeds deterministic CBOR control bound"
        )
    if encoded_seal_bytes + protocol["wire"]["fixed_header_bytes"] > (
        protocol["wire"]["fixed_header_bytes"]
        + protocol["wire"]["limits"]["control_bytes"]
    ):
        raise SourceClosureTransportError(
            "maximum message-27 frame exceeds fixed-header frame bound"
        )

    success = contract["state_machine"]["success_path"]
    required_states = [
        "task-v4-sealed",
        "manifest-open",
        "manifest-streaming",
        "manifest-validated",
        "blob-open",
        "blob-streaming",
        "blob-sealed",
        "closure-sealed",
        "closure-acknowledged",
        "task-accepted",
    ]
    if success != required_states:
        raise SourceClosureTransportError("source-closure success state machine drift")
    if contract["cache"] != {
        "cross_task_v1": "forbidden",
        "transfer": "complete-per-task",
        "future_activation": "separate-accepted-capability-and-adr",
    }:
        raise SourceClosureTransportError("cross-task cache was activated")
    limits = contract["limits"]
    if limits["maximum_resident_transport_bytes"] > 1310720:
        raise SourceClosureTransportError("resident transport bound exceeds 1.25 MiB")
    if limits["maximum_chunks_per_blob"] < 16 or limits["maximum_blob_chunk_frames"] < 4144:
        raise SourceClosureTransportError("chunk bounds exclude valid ADR 0101 closures")
    liveness = contract["state_machine"]["pre_accept_liveness"]
    if liveness.get("clock") != "host-injected-monotonic" or not all(
        isinstance(liveness.get(field), int) and liveness[field] > 0
        for field in ("send_progress_timeout_ns", "seal_ack_timeout_ns")
    ):
        raise SourceClosureTransportError("pre-accept closure lifecycle is unbounded")
    identity = contract["identity"]
    if identity.get("projection_codec") != (
        "canonical-json-utf8-sorted-keys-no-whitespace"
    ) or identity.get("semantic_digest_codec") != "cxxlens-canonical-binary-v1" or identity.get(
        "semantic_digest_framing"
    ) != ["cxxlens-semantic-digest-v2", "domain", "canonical-projection-bytes"]:
        raise SourceClosureTransportError("semantic digest framing is not constructible")
    manifest_identity = identity.get("manifest", {})
    if manifest_identity.get("exact_fields") != [
        "schema", "closure_id", "closure_digest", "members", "blobs"
    ] or manifest_identity.get("closure_digest_domain") != SOURCE_CLOSURE_DIGEST_DOMAIN or manifest_identity.get(
        "domain"
    ) != SOURCE_CLOSURE_MANIFEST_DIGEST_DOMAIN or identity.get("blob_receipts", {}).get("digest") != (
        "semantic-digest-of-canonical-complete-receipt-array-streamed"
    ):
        raise SourceClosureTransportError("manifest or bounded seal projection drift")
    if contract["request_task_binding"].get("wire_transfer_identity") != (
        "exact-outer-task-extension-task-id-task-v4-digest-and-source-closure-reference-before-first-transfer-frame"
    ):
        raise SourceClosureTransportError(
            "outer task-v4 extension/wire transfer binding drift"
        )
    failures = set(contract["failures"])
    matrix = contract["failure_phase_matrix"]
    matrix_failures = {reason for phase in matrix.values() for reason in phase["allowed"]}
    if matrix_failures != failures or any(
        set(phase) != {"allowed", "counters"} for phase in matrix.values()
    ):
        raise SourceClosureTransportError("failure phase/field matrix is incomplete")
    blob_payload = b"x"
    content = "sha256:" + hashlib.sha256(blob_payload).hexdigest()
    witness_path = "project://src/main.cpp"
    witness_members = [{"file_id": source_closure_file_id(witness_path), "logical_path": witness_path, "role": "main", "encoding": "utf8", "size_bytes": 1, "content_digest": content, "read_only": True}]
    witness_blobs = [{"content_digest": content, "size_bytes": 1}]
    semantic = closure_digest(witness_members, witness_blobs)
    validate_manifest({
        "schema": SOURCE_CLOSURE_MANIFEST_SCHEMA,
        "closure_id": "source-closure:" + semantic,
        "closure_digest": semantic,
        "members": witness_members,
        "blobs": witness_blobs,
    }, manifest_schema)
    witness_manifest = {
        "schema": SOURCE_CLOSURE_MANIFEST_SCHEMA,
        "closure_id": "source-closure:" + semantic,
        "closure_digest": semantic,
        "members": witness_members,
        "blobs": witness_blobs,
    }
    manifest_payload = canonical_json(witness_manifest)
    semantic_witness = "semantic-v2:sha256:" + "1" * 64
    content_witness = content
    manifest_witness = manifest_digest(witness_manifest)
    session_witness = "provider-session:sha256:" + "3" * 64
    task_witness = "task:" + semantic_witness
    receipt_witness = blob_receipts_digest([
        {"blob_ordinal": 0, "blob_digest": content_witness, "size_bytes": 1}
    ])
    transfer_witness_digest = transfer_digest({
        "session_id": session_witness,
        "task_id": task_witness,
        "task_v4_digest": semantic_witness,
        "manifest_digest": manifest_witness,
        "blob_receipts_digest": receipt_witness,
        "blob_count": 1,
        "total_bytes": 1,
        "closure_digest": semantic,
    })
    controls_witness = [
        ("source_closure_manifest", {"kind": "descriptor", "session_id": session_witness, "task_id": task_witness, "task_v4_digest": semantic_witness, "closure_id": "source-closure:" + semantic, "closure_digest": semantic, "manifest_digest": manifest_witness, "total_bytes": len(manifest_payload), "chunk_bytes": len(manifest_payload), "chunk_count": 1}, b""),
        ("source_closure_manifest", {"kind": "chunk", "session_id": session_witness, "task_id": task_witness, "manifest_digest": manifest_witness, "chunk_index": 0, "offset": 0, "byte_count": len(manifest_payload)}, manifest_payload),
        ("source_closure_blob", {"session_id": session_witness, "task_id": task_witness, "closure_digest": semantic, "blob_ordinal": 0, "blob_digest": content_witness, "total_bytes": 1, "chunk_bytes": 1, "chunk_count": 1}, b""),
        ("source_closure_chunk", {"session_id": session_witness, "task_id": task_witness, "blob_ordinal": 0, "blob_digest": content_witness, "chunk_index": 0, "offset": 0, "byte_count": 1}, blob_payload),
        ("source_closure_seal", {"session_id": session_witness, "task_id": task_witness, "task_v4_digest": semantic_witness, "manifest_digest": manifest_witness, "blob_receipts_digest": receipt_witness, "blob_count": 1, "total_bytes": 1, "closure_digest": semantic, "transfer_digest": transfer_witness_digest}, b""),
        ("source_closure_ack", {"session_id": session_witness, "task_id": task_witness, "closure_digest": semantic, "transfer_digest": transfer_witness_digest, "spool_receipt": "spool-receipt:" + semantic_witness, "cleanup_owner": "cleanup-owner:" + semantic_witness}, b""),
    ]
    transfer_witness = TransferStateWitness.for_task_extension(
        session_id=session_witness,
        task_extension={
            "task_id": task_witness,
            "task_v4_digest": semantic_witness,
            "source_closure": {
                "id": "source-closure:" + semantic,
                "digest": semantic,
                "manifest_digest": manifest_witness,
            },
        },
        manifest_schema=manifest_schema,
    )
    for control_name, control_value, control_payload in controls_witness:
        transfer_witness.apply(control_name, control_value, control_payload, contract)
    if transfer_witness.state != "closure-acknowledged":
        raise SourceClosureTransportError("wire state witness did not reach exact terminal")
    for phase, row in matrix.items():
        if phase == "local-only":
            if row != {
                "allowed": ["source-closure.ambient-fallback-denied"],
                "counters": [],
            }:
                raise SourceClosureTransportError(
                    "local-only failure phase contract is not exact"
                )
            continue
        validate_reject_control({
            "session_id": session_witness, "task_id": task_witness,
            "failure_phase": phase, "reason_code": row["allowed"][0],
            "observed_counters": {name: 0 for name in row["counters"]},
            "cleanup_receipt": "cleanup-receipt:" + semantic_witness,
        }, contract)

    maturity = contract["maturity"]
    if maturity not in {"proposed", "accepted"}:
        raise SourceClosureTransportError("source-closure maturity is invalid")
    if not set(proposed_ids).issubset(set(protocol_ids)):
        raise SourceClosureTransportError(
            "source-closure message IDs are absent from Protocol 2 registry"
        )
    if (
        protocol["compatibility"].get("accepted_major") != 2
        or protocol["compatibility"].get("accepted_minor") != 0
    ):
        raise SourceClosureTransportError("source-closure authority unexpectedly activated protocol")
    return contract


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    arguments = parser.parse_args()
    try:
        validate(arguments.root.resolve())
    except (OSError, SourceClosureTransportError) as error:
        print(f"source-closure-transport: {error}", file=sys.stderr)
        return 1
    print("source-closure-transport: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
