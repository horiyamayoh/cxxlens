#!/usr/bin/env python3
"""Validate the proposed dedicated source-closure transport authority."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import unicodedata
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONTRACT = pathlib.Path("schemas/cxxlens_ng_source_closure_transport.yaml")
SCHEMA = pathlib.Path("schemas/cxxlens_ng_source_closure_transport.schema.yaml")
PROTOCOL = pathlib.Path("schemas/cxxlens_ng_provider_protocol.yaml")
PROTOCOL_SCHEMA = pathlib.Path("schemas/cxxlens_ng_provider_protocol.schema.yaml")
REQUEST = pathlib.Path("schemas/cxxlens_ng_clang22_materialization_request_v2_2.schema.yaml")
TASK = pathlib.Path("schemas/cxxlens_ng_provider_task_v4.schema.yaml")
MANIFEST_SCHEMA = pathlib.Path("schemas/cxxlens_ng_source_closure_manifest_v1.schema.yaml")
ADR = pathlib.Path("docs/design/adr/0102-dedicated-source-closure-transport.md")
LEGACY_BINDINGS = {
    "request_schema_sha256": pathlib.Path(
        "schemas/cxxlens_ng_clang22_materialization_request.schema.yaml"
    ),
    "task_v3_header_sha256": pathlib.Path("src/llvm/clang22/provider_task_v3.hpp"),
    "task_v3_implementation_sha256": pathlib.Path(
        "src/llvm/clang22/provider_task_v3.cpp"
    ),
    "request_v2_1_implementation_sha256": pathlib.Path(
        "src/llvm/clang22/materialization_request_v2_1.cpp"
    ),
}
REVIEW_REF = re.compile(
    r"^https://github\.com/horiyamayoh/cxxlens/issues/261#issuecomment-[1-9][0-9]*$"
)


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
    return semantic_digest("cxxlens.source-closure-manifest.v1", manifest)


def closure_digest(members: list[dict[str, Any]], blobs: list[dict[str, Any]]) -> str:
    """Reproduce ADR 0101 / source_closure.cpp byte-for-byte."""
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
        _canonical_string("cxxlens.source-closure.v1"),
        _canonical_string("unicode-default-casefold-then-nfc"),
        _canonical_tuple(encoded_members),
        _canonical_tuple(encoded_blobs),
    ])
    return semantic_digest_bytes("cxxlens.source-closure.v1", projection)


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
        value = control.get(field)
        if not isinstance(value, str) or not value or len(value.encode("utf-8")) > 4096:
            raise SourceClosureTransportError(
                f"reject control {field} is not a bounded typed ID"
            )
    phase = control.get("failure_phase")
    row = contract["failure_phase_matrix"].get(phase)
    if row is None or control.get("reason_code") not in row["allowed"]:
        raise SourceClosureTransportError("reject reason is unavailable in failure phase")
    counters = control.get("observed_counters")
    if not isinstance(counters, dict) or set(counters) != set(row["counters"]):
        raise SourceClosureTransportError("reject counters are not phase-authentic")
    if not all(type(value) is int and 0 <= value <= (1 << 64) - 1 for value in counters.values()):
        raise SourceClosureTransportError("reject counters are not uint64 integers")
    if phase == "before-manifest" and any("byte" in key for key in counters):
        raise SourceClosureTransportError("before-manifest reject fabricates received bytes")


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
            if not isinstance(value, str) or not value or len(value.encode("utf-8")) > 4096:
                raise SourceClosureTransportError(f"wire control {field} is not a bounded typed ID")
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
        expected_count = 0 if control["total_bytes"] == 0 else (control["total_bytes"] + control["chunk_bytes"] - 1) // control["chunk_bytes"]
        if control["chunk_count"] != expected_count or control["chunk_count"] > maximum_count:
            raise SourceClosureTransportError("wire descriptor chunk count invalid")


def blob_receipts_digest(receipts: list[dict[str, Any]]) -> str:
    return semantic_digest("cxxlens.source-closure-blob-receipts.v1", receipts)


def transfer_digest(projection: dict[str, Any]) -> str:
    expected = [
        "session_id", "task_id", "task_v4_digest", "manifest_digest",
        "blob_receipts_digest", "blob_count", "total_bytes", "closure_digest",
    ]
    if list(projection) != expected:
        raise SourceClosureTransportError("transfer digest projection is not exact-order closed")
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
    if not isinstance(value, str) or not value.startswith("project://"):
        raise SourceClosureTransportError("task v4 logical path is not project-scoped")
    if len(value.encode("utf-8")) > 4096 or unicodedata.normalize("NFC", value) != value:
        raise SourceClosureTransportError("task v4 logical path violates UTF-8/NFC bounds")
    relative = value.removeprefix("project://")
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
        raise SourceClosureTransportError("task v4 logical path violates ADR 0101 segments")


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
        raise SourceClosureTransportError("task v4/base v2.1 census mismatch")
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
        if extension.get("base_task_v3_digest") != content_projection_digest(base):
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
            member["logical_path"]
            for member in manifest_by_id[reference["id"]]["members"]
            if member["role"] == "main"
        ]
        if manifest_main != [extension.get("main_logical_path")]:
            raise SourceClosureTransportError("task v4 manifest main path binding mismatch")
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
    """Build one complete v2.2 request from the repository's executable v2.1 witness."""

    sys.path.insert(0, str(root / "tools" / "quality"))
    import check_ng_clang22_materialization as materialization  # noqa: PLC0415

    fixture_root = (
        root if (root / "schemas/cxxlens_ng_relation_registry.yaml").exists() else ROOT
    )
    request = materialization.sample_request(fixture_root)
    base = request["tasks"][0]
    source = base["source"]
    source["read_only"] = True
    source.pop("content_base64")
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
        "schema": "cxxlens.source-closure-manifest.v1",
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

    required_features = ["task-input-chunks-v1", "task-source-closure-v1"]
    request["schema"] = "cxxlens.clang22-materialization-request.v2_2"
    request["request_version"] = "2.2.0"
    request["required_features"] = required_features
    request["worker"]["protocol_minor"] = 2
    request["worker"]["required_features"] = required_features
    request["trust_policy"]["protocol_minor"] = 2
    request["trust_policy"]["required_features"] = required_features
    request["trust_policy"]["task_sandbox_requirements"] = [base["sandbox"]]
    request["trust_policy"]["trust_policy_digest"] = trust_policy_digest(
        request["trust_policy"]
    )
    extension = {
        "schema": "cxxlens.clang22.task.v4",
        "base_task_index": 0,
        "base_provider_task_id": base["provider_task_id"],
        "base_task_v3_digest": content_projection_digest(base),
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
    legacy_request_schema = load(root / LEGACY_BINDINGS["request_schema_sha256"])
    schema_store = {
        request["$id"]: request,
        legacy_request_schema["$id"]: legacy_request_schema,
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
    adr = (root / ADR).read_text(encoding="utf-8")

    legacy_ids = [entry["id"] for entry in protocol["message_types"]["registry"]]
    proposed = contract["message_registry"]["proposed"]
    proposed_ids = [entry["id"] for entry in proposed]
    if len(legacy_ids) != len(set(legacy_ids)) or len(proposed_ids) != len(set(proposed_ids)):
        raise SourceClosureTransportError("duplicate message ID")
    if proposed_ids != list(range(24, 30)):
        raise SourceClosureTransportError("source-closure message IDs must be contiguous 24 through 29")
    if contract["message_registry"]["preserved"] != {"heartbeat": 23}:
        raise SourceClosureTransportError("heartbeat 23 is not preserved")

    versions = contract["versions"]
    if versions != {
        "provider_protocol": {"legacy": 1.1, "proposed": 1.2, "downgrade": "reject"},
        "request": {"legacy": 2.1, "proposed": 2.2},
        "task": {
            "legacy": "cxxlens.clang22.task.v3",
            "proposed": "cxxlens.clang22.task.v4",
        },
    }:
        raise SourceClosureTransportError("version or downgrade contract drift")
    if request["properties"]["required_features"].get("const") != [
        "task-input-chunks-v1",
        "task-source-closure-v1",
    ]:
        raise SourceClosureTransportError("request 2.2 omits exact source-closure capability")
    trust = request["properties"]["trust_policy"]["properties"]
    if trust["protocol_minor"].get("const") != 2 or trust["required_features"].get("const") != [
        "task-input-chunks-v1", "task-source-closure-v1"
    ]:
        raise SourceClosureTransportError("request 2.2 trust policy retains protocol 1.1 authority")
    request_text = (root / REQUEST).read_text(encoding="utf-8")
    task_text = (root / TASK).read_text(encoding="utf-8")
    if "content_base64" in request_text or "content_base64" in task_text:
        raise SourceClosureTransportError("request/task embeds closure blob bytes")
    request_required = set(request["required"])
    if not {"tasks", "source_closures", "task_extensions"}.issubset(
        request_required
    ):
        raise SourceClosureTransportError("request 2.2 projects away v2.1 authority")
    if "base_request_v2_1" in request.get("properties", {}) or request["properties"]["tasks"].get("items", {}).get("$ref") != "#/$defs/base_task_without_source_bytes":
        raise SourceClosureTransportError("request 2.2 nests an executable v2.1 request")
    source_properties = request["$defs"]["base_task_without_source_bytes"]["properties"]["source"]["properties"]
    if "content_base64" in source_properties or request["properties"]["worker"]["properties"]["protocol_minor"].get("const") != 2:
        raise SourceClosureTransportError("request 2.2 source bytes or protocol authority drift")
    source_id_pattern = request["$defs"]["source_closure_id"].get("pattern")
    if source_id_pattern != r"^source-closure:semantic-v2:sha256:[0-9a-f]{64}$":
        raise SourceClosureTransportError("source closure ID grammar differs from ADR 0101")
    task_required = set(task["required"])
    if not {"base_task_v3_digest", "open_task", "source_closure"}.issubset(
        task_required
    ):
        raise SourceClosureTransportError("task v4 omits inherited task/open-task authority")
    path_contract = task["$defs"]["logical_path"]
    if path_contract.get("x-cxxlens-max-utf8-bytes") != 4096 or not any(
        entry.get("not", {}).get("pattern") == r"(^|/)\.{1,2}(/|$)"
        for entry in path_contract.get("allOf", [])
    ):
        raise SourceClosureTransportError("task v4 path does not bind ADR 0101 byte/segment rules")

    bindings = contract["compatibility"]["legacy_bindings"]
    observed_bindings = {
        name: hashlib.sha256((root / path).read_bytes()).hexdigest()
        for name, path in LEGACY_BINDINGS.items()
    }
    if bindings != observed_bindings:
        raise SourceClosureTransportError("legacy 2.1/v3 byte authority drift")

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
    if identity.get("manifest", {}).get("exact_fields") != [
        "schema", "closure_id", "closure_digest", "members", "blobs"
    ] or identity.get("blob_receipts", {}).get("digest") != (
        "semantic-digest-of-canonical-complete-receipt-array-streamed"
    ):
        raise SourceClosureTransportError("manifest or bounded seal projection drift")
    failures = set(contract["failures"])
    matrix = contract["failure_phase_matrix"]
    matrix_failures = {reason for phase in matrix.values() for reason in phase["allowed"]}
    if matrix_failures != failures or any(
        set(phase) != {"allowed", "counters"} for phase in matrix.values()
    ):
        raise SourceClosureTransportError("failure phase/field matrix is incomplete")
    content = "sha256:" + "2" * 64
    witness_members = [{"file_id": "file:sha256:" + "3" * 64, "logical_path": "project://src/main.cpp", "role": "main", "encoding": "utf8", "size_bytes": 1, "content_digest": content, "read_only": True}]
    witness_blobs = [{"content_digest": content, "size_bytes": 1}]
    semantic = closure_digest(witness_members, witness_blobs)
    validate_manifest({
        "schema": "cxxlens.source-closure-manifest.v1",
        "closure_id": "source-closure:" + semantic,
        "closure_digest": semantic,
        "members": witness_members,
        "blobs": witness_blobs,
    }, manifest_schema)
    semantic_witness = "semantic-v2:sha256:" + "1" * 64
    content_witness = "sha256:" + "2" * 64
    controls_witness = [
        ("source_closure_manifest", {"kind": "descriptor", "session_id": "session:witness", "task_id": "task:witness", "task_v4_digest": semantic_witness, "closure_id": "source-closure:" + semantic_witness, "closure_digest": semantic_witness, "manifest_digest": semantic_witness, "total_bytes": 1, "chunk_bytes": 1, "chunk_count": 1}, b"", "task-v4-sealed"),
        ("source_closure_manifest", {"kind": "chunk", "session_id": "session:witness", "task_id": "task:witness", "manifest_digest": semantic_witness, "chunk_index": 0, "offset": 0, "byte_count": 1}, b"x", "manifest-open"),
        ("source_closure_blob", {"session_id": "session:witness", "task_id": "task:witness", "closure_digest": semantic_witness, "blob_ordinal": 0, "blob_digest": content_witness, "total_bytes": 1, "chunk_bytes": 1, "chunk_count": 1}, b"", "manifest-validated"),
        ("source_closure_chunk", {"session_id": "session:witness", "task_id": "task:witness", "blob_ordinal": 0, "blob_digest": content_witness, "chunk_index": 0, "offset": 0, "byte_count": 1}, b"x", "blob-open"),
        ("source_closure_seal", {"session_id": "session:witness", "task_id": "task:witness", "task_v4_digest": semantic_witness, "manifest_digest": semantic_witness, "blob_receipts_digest": semantic_witness, "blob_count": 1, "total_bytes": 1, "closure_digest": semantic_witness, "transfer_digest": semantic_witness}, b"", "blob-sealed"),
        ("source_closure_ack", {"session_id": "session:witness", "task_id": "task:witness", "closure_digest": semantic_witness, "transfer_digest": semantic_witness, "spool_receipt": "spool:witness", "cleanup_owner": "cleanup:witness"}, b"", "closure-sealed"),
    ]
    for control_name, control_value, control_payload, control_state in controls_witness:
        validate_wire_control(control_name, control_value, control_payload, control_state, contract)
    for phase, row in matrix.items():
        validate_reject_control({
            "session_id": "session:witness", "task_id": "task:witness",
            "failure_phase": phase, "reason_code": row["allowed"][0],
            "observed_counters": {name: 0 for name in row["counters"]},
            "cleanup_receipt": "cleanup:witness",
        }, contract)

    maturity = contract["maturity"]
    review = contract["authority"]["review"]
    review_findings = contract["review_findings"]
    adr_status = "Accepted" if "- Status: Accepted" in adr else "Proposed"
    if maturity == "proposed":
        if set(legacy_ids).intersection(proposed_ids):
            raise SourceClosureTransportError(
                "proposed message ID collides with accepted protocol"
            )
        if protocol["compatibility"].get("current") != "1.1.0":
            raise SourceClosureTransportError("proposed authority unexpectedly activated protocol")
        if adr_status != "Proposed" or review != {
            "status": "required",
            "reviewer": None,
            "ref": None,
            "exact_main_commit": None,
        }:
            raise SourceClosureTransportError("proposed authority has premature acceptance")
        if review_findings["status"] != "blocking" or review_findings["receipt_id"] is not None:
            raise SourceClosureTransportError("proposed authority lost blocking review history")
    else:
        if adr_status != "Accepted" or review["status"] != "complete":
            raise SourceClosureTransportError("accepted authority lacks completed review")
        if not isinstance(review["reviewer"], str) or not review["reviewer"]:
            raise SourceClosureTransportError("accepted authority lacks reviewer")
        if not isinstance(review["ref"], str) or not REVIEW_REF.fullmatch(review["ref"]):
            raise SourceClosureTransportError("accepted authority lacks canonical review reference")
        if not isinstance(review["exact_main_commit"], str) or not re.fullmatch(
            r"[0-9a-f]{40}", review["exact_main_commit"]
        ):
            raise SourceClosureTransportError("accepted authority lacks exact main commit")
        if review_findings != {
            "status": "resolved",
            "exact_main_commit": review["exact_main_commit"],
            "ref": review["ref"],
            "reviewer": review["reviewer"],
            "receipt_id": review_findings["receipt_id"],
            "required_resolutions": review_findings["required_resolutions"],
        } or not isinstance(review_findings["receipt_id"], str):
            raise SourceClosureTransportError("accepted authority retains blocking review findings")
        import check_ng_development_decisions as decisions  # noqa: PLC0415
        register = decisions.validate(root)
        decision = next(
            (entry for entry in register["decisions"] if entry["id"] == "decision.source-closure.dedicated-transport"),
            None,
        )
        if (
            decision is None
            or decision["authority_status"] != "accepted"
            or decision["review"]["outcome"] != "accepted"
            or review_findings["receipt_id"] not in decision["review"]["receipt_ids"]
        ):
            raise SourceClosureTransportError("accepted source closure bypasses authenticated decision receipt")
        try:
            reviewed_contract = subprocess.run(
                ["git", "show", f"{review['exact_main_commit']}:{CONTRACT.as_posix()}"],
                cwd=root,
                check=True,
                capture_output=True,
                text=True,
            ).stdout
            ancestor = subprocess.run(
                ["git", "merge-base", "--is-ancestor", review["exact_main_commit"], "HEAD"],
                cwd=root,
                check=False,
                capture_output=True,
            )
        except (OSError, subprocess.CalledProcessError) as error:
            raise SourceClosureTransportError("reviewed exact main commit is not available") from error
        reviewed = yaml.safe_load(reviewed_contract)
        if ancestor.returncode != 0 or reviewed.get("maturity") != "proposed":
            raise SourceClosureTransportError("reviewed commit is not an ancestor Proposed authority")
        reviewed_comparable = dict(reviewed)
        current_comparable = dict(contract)
        reviewed_comparable.pop("authority")
        current_comparable.pop("authority")
        reviewed_comparable.pop("maturity")
        current_comparable.pop("maturity")
        if reviewed_comparable != current_comparable:
            raise SourceClosureTransportError("accepted semantics differ from reviewed Proposed commit")
        registry = {
            (entry["id"], entry["name"], entry["direction"])
            for entry in protocol["message_types"]["registry"]
        }
        required_registry = {
            (entry["id"], entry["name"], entry["direction"]) for entry in proposed
        }
        minor = protocol["host_to_provider_state_machine"]["minor_profiles"].get("1.2")
        activation = contract["protocol_activation"]
        if (
            protocol["compatibility"].get("current") != activation["current"]
            or registry != required_registry.union(
                {
                    (entry["id"], entry["name"], entry["direction"])
                    for entry in reviewed_protocol_registry(root, review["exact_main_commit"])
                }
            )
            or minor != activation["minor_profile"]
        ):
            raise SourceClosureTransportError(
                "accepted authority is not atomically active in protocol 1.2"
            )
    return contract


def reviewed_protocol_registry(root: pathlib.Path, commit: str) -> list[dict[str, Any]]:
    try:
        raw = subprocess.run(
            ["git", "show", f"{commit}:{PROTOCOL.as_posix()}"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        raise SourceClosureTransportError("reviewed protocol registry is unavailable") from error
    value = yaml.safe_load(raw)
    return value["message_types"]["registry"]


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
