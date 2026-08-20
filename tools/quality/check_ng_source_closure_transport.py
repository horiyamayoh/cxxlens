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
    return b"\x03" + _length(len(encoded)) + encoded


def _canonical_bytes(value: bytes) -> bytes:
    return b"\x04" + _length(len(value)) + value


def _canonical_tuple(values: list[bytes]) -> bytes:
    output = bytearray(b"\x05" + _length(len(values)))
    for value in values:
        output.extend(_length(len(value)))
        output.extend(value)
    return bytes(output)


def semantic_digest(domain: str, projection: Any) -> str:
    framed = _canonical_tuple(
        [
            _canonical_string("cxxlens-semantic-digest-v2"),
            _canonical_string(domain),
            _canonical_bytes(canonical_json(projection)),
        ]
    )
    return "semantic-v2:sha256:" + hashlib.sha256(framed).hexdigest()


def content_projection_digest(projection: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(projection)).hexdigest()


def manifest_digest(manifest: dict[str, Any]) -> str:
    return semantic_digest("cxxlens.source-closure-manifest.v1", manifest)


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
        "base_request_v2_1": request["base_request_v2_1"],
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


def validate_request_binding(request: dict[str, Any]) -> None:
    """Validate cross-document v2.2 relationships JSON Schema cannot express."""

    base_tasks = request.get("base_request_v2_1", {}).get("tasks")
    closures = request.get("source_closures")
    extensions = request.get("task_extensions")
    if not all(isinstance(value, list) for value in (base_tasks, closures, extensions)):
        raise SourceClosureTransportError("request 2.2 relationship inputs are missing")
    if len(base_tasks) != len(extensions):
        raise SourceClosureTransportError("task v4/base v2.1 census mismatch")

    closure_by_id: dict[str, dict[str, Any]] = {}
    for closure in closures:
        identifier = closure.get("source_closure_id")
        digest = closure.get("source_closure_digest")
        if identifier in closure_by_id:
            raise SourceClosureTransportError("duplicate source closure ID")
        if identifier != f"source-closure:{digest}":
            raise SourceClosureTransportError("source closure ID/digest mismatch")
        closure_by_id[identifier] = closure

    task_ids: set[str] = set()
    indices: set[int] = set()
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
        reference = extension.get("source_closure", {})
        closure = closure_by_id.get(reference.get("id"))
        if closure is None or reference.get("digest") != closure.get(
            "source_closure_digest"
        ) or reference.get("manifest_digest") != closure.get("manifest_digest"):
            raise SourceClosureTransportError("task v4 closure reference does not resolve exactly")
        _validate_logical_path(extension.get("main_logical_path"))
        _validate_logical_path(extension.get("logical_working_directory"))
        expected_task_digest = semantic_digest(
            "cxxlens.clang22.task.v4", task_v4_projection(extension)
        )
        if extension.get("task_v4_digest") != expected_task_digest or task_id != (
            "task:" + expected_task_digest
        ):
            raise SourceClosureTransportError("task v4 semantic identity mismatch")
        task_ids.add(task_id)
        indices.add(index)
    if indices != set(range(len(base_tasks))):
        raise SourceClosureTransportError("task v4/base task index set is incomplete")
    expected_request_digest = semantic_digest(
        "cxxlens.clang22.materialization-request.v2_2",
        request_v2_2_projection(request),
    )
    if request.get("request_digest") != expected_request_digest or request.get(
        "request_id"
    ) != ("materialization-request:" + expected_request_digest):
        raise SourceClosureTransportError("request v2.2 semantic identity mismatch")


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
    request_text = (root / REQUEST).read_text(encoding="utf-8")
    task_text = (root / TASK).read_text(encoding="utf-8")
    if "content_base64" in request_text or "content_base64" in task_text:
        raise SourceClosureTransportError("request/task embeds closure blob bytes")
    request_required = set(request["required"])
    if not {"base_request_v2_1", "source_closures", "task_extensions"}.issubset(
        request_required
    ):
        raise SourceClosureTransportError("request 2.2 projects away v2.1 authority")
    base_ref = request["properties"]["base_request_v2_1"].get("$ref")
    if base_ref != "cxxlens_ng_clang22_materialization_request.schema.yaml":
        raise SourceClosureTransportError("request 2.2 does not bind the exact v2.1 schema")
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

    maturity = contract["maturity"]
    review = contract["authority"]["review"]
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
