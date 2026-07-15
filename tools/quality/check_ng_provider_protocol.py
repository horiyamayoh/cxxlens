#!/usr/bin/env python3
"""Executable provider wire, streaming, atomicity, and planning contract."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import random
import struct
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONTRACT = pathlib.Path("schemas/cxxlens_ng_provider_protocol.yaml")
CONTRACT_SCHEMA = pathlib.Path("schemas/cxxlens_ng_provider_protocol.schema.yaml")
MANIFEST_SCHEMA = pathlib.Path("schemas/cxxlens_ng_provider_manifest.schema.yaml")
TASK_SCHEMA = pathlib.Path("schemas/cxxlens_ng_provider_task.schema.yaml")
VECTORS = pathlib.Path("schemas/cxxlens_ng_provider_conformance_vectors.yaml")
VECTORS_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_provider_conformance_vectors.schema.yaml"
)
REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_provider_conformance_report.schema.yaml"
)
FUZZ = pathlib.Path("schemas/cxxlens_ng_provider_fuzz_corpus.yaml")
FUZZ_SCHEMA = pathlib.Path("schemas/cxxlens_ng_provider_fuzz_corpus.schema.yaml")

FRAME = struct.Struct(">4sHHHHQQIQ32s32s")
MAX_CONTROL = 65536
MAX_PAYLOAD = 16777216
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


def encode_frame(
    control: Any,
    payload: bytes = b"",
    *,
    message_type: int = 1,
    flags: int = 0,
    stream_id: int = 0,
    sequence: int = 0,
    protocol_major: int = 1,
    protocol_minor: int = 0,
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


def decode_frame(contract: dict[str, Any], data: bytes) -> dict[str, Any]:
    if len(data) < FRAME.size:
        fail("provider.truncated-stream", "fixed header")
    fields = FRAME.unpack(data[: FRAME.size])
    magic, major, minor, message_type, flags, stream, sequence = fields[:7]
    control_length, payload_length, control_hash, payload_hash = fields[7:]
    if magic != b"CXXP":
        fail("provider.malformed-frame", "magic")
    expected_major = int(contract["compatibility"]["current"].split(".", 1)[0])
    if major != expected_major:
        fail("provider.protocol-major-mismatch", str(major))
    known_flags = 0
    for flag in contract["wire"]["flags"].values():
        known_flags |= flag
    if flags & ~known_flags:
        fail("provider.malformed-frame", "unknown frame flag")
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
    if message_type not in _known_message_ids(contract):
        required = flags & contract["wire"]["flags"]["required_extension"]
        optional = flags & contract["wire"]["flags"]["optional_extension"]
        if required and optional:
            fail("provider.malformed-frame", "conflicting extension flags")
        if required:
            fail("provider.required-feature-missing", str(message_type))
        if not optional:
            fail("provider.malformed-frame", "unclassified unknown message")
        return {"skipped_optional": True}
    return {
        "protocol_major": major,
        "protocol_minor": minor,
        "message_type": message_type,
        "flags": flags,
        "stream_id": stream,
        "sequence": sequence,
        "control": decoded,
        "payload_hex": payload.hex(),
    }


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
        b"CXXP", 1, 0, message_type, flags, 0, 0, len(control), 0,
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
    fail("provider.fuzz-mutation-unknown", mutation)


def negotiate(value: dict[str, Any]) -> dict[str, Any]:
    host, requested, offered = value["host"], value["requested"], value["offered"]
    if host["major"] != offered["protocol"]["major"]:
        fail("provider.protocol-major-mismatch", str(host["major"]))
    for field in ("provider_id", "provider_version"):
        if requested[field] != offered[field]:
            fail("provider.adjacent-fallback-forbidden", field)
    if requested["binary_digest"] != offered["binary_digest"]:
        fail("provider.binary-identity-mismatch", "binary digest")
    available = set(host["features"])
    required = set(offered["required_features"])
    if not required <= available:
        fail("provider.required-feature-missing", str(sorted(required - available)))
    minor = min(host["minor"], offered["protocol"]["maximum_minor"])
    if minor < offered["protocol"]["minimum_minor"]:
        fail("provider.protocol-minor-incompatible", str(minor))
    features = sorted(required | (available & set(offered["optional_features"])))
    return {"major": host["major"], "minor": minor, "features": features}


def flow(value: dict[str, Any]) -> dict[str, int]:
    remaining_bytes = value["credit"]["bytes"]
    remaining_frames = value["credit"]["frames"]
    if remaining_bytes < 0 or remaining_frames < 0:
        fail("provider.credit-invalid", "negative initial credit")
    expected_sequence = 0
    for frame in value["frames"]:
        if frame["sequence"] != expected_sequence:
            fail("provider.sequence-gap", str(frame["sequence"]))
        if frame["bytes"] > remaining_bytes or remaining_frames < 1:
            fail("provider.credit-exceeded", str(frame["sequence"]))
        remaining_bytes -= frame["bytes"]
        remaining_frames -= 1
        expected_sequence += 1
    ack = value["ack"]
    if ack["highest_contiguous_sequence"] != expected_sequence - 1:
        fail("provider.ack-invalid", str(ack))
    if ack["stream_id"] != value["stream_id"]:
        fail("provider.ack-invalid", "stream binding")
    if ack["staged_digest"] != value["staged_digest"]:
        fail("provider.ack-invalid", "staged digest binding")
    if ack["return_bytes"] < 0 or ack["return_frames"] < 0:
        fail("provider.credit-invalid", "negative returned credit")
    return {
        "highest_contiguous_sequence": ack["highest_contiguous_sequence"],
        "remaining_bytes": remaining_bytes + ack["return_bytes"],
        "remaining_frames": remaining_frames + ack["return_frames"],
    }


def resume(value: dict[str, Any]) -> int:
    if value["expected"] != value["offered"]:
        fail("provider.resume-token-stale", "token binding")
    return value["offered"]["highest_contiguous_acked_sequence"] + 1


def group(value: dict[str, Any]) -> tuple[dict[str, Any], str]:
    groups = value["groups"]
    identifiers = [row["id"] for row in groups]
    if not groups or len(identifiers) != len(set(identifiers)):
        fail("provider.dependency-group-invalid", "empty or duplicate group IDs")
    atomic_groups = [atomic for row in groups for atomic in row["atomic_groups"]]
    if len(atomic_groups) != len(set(atomic_groups)):
        fail("provider.atomic-output-group-duplicate", "atomic group ownership")
    owner = {atomic: row["id"] for row in groups for atomic in row["atomic_groups"]}
    for row in groups:
        for reference in row["hard_references"]:
            if reference["target"] in owner and owner[reference["target"]] != row["id"]:
                fail("provider.hard-reference-group-mismatch", reference["target"])
    fail_group = value.get("fail_group")
    if fail_group is None:
        for row in groups:
            if row["state"] != "sealed" or not row["batches_sealed"]:
                fail("provider.group-not-sealed", row["id"])
            if not all(
                row[field]
                for field in (
                    "digests_valid", "coverage_balanced", "unresolved_accounted", "closures_valid"
                )
            ):
                fail("provider.group-validation-failed", row["id"])
        return (
            {"adopted": [row["id"] for row in groups], "rolled_back": [], "prior_snapshot": "unchanged"},
            "provider.group-valid",
        )
    if fail_group not in identifiers:
        fail("provider.dependency-group-invalid", "failure group is not declared")
    fail_index = identifiers.index(fail_group)
    before_rows = groups[:fail_index]
    for row in before_rows:
        if row["state"] != "sealed" or not row["batches_sealed"] or not all(
            row[field]
            for field in (
                "digests_valid",
                "coverage_balanced",
                "unresolved_accounted",
                "closures_valid",
            )
        ):
            fail("provider.group-validation-failed", row["id"])
    before = [row["id"] for row in before_rows]
    rollback = identifiers[fail_index:]
    if value["partial_policy"] == "declared_dependency_groups":
        return (
            {"adopted": before, "rolled_back": rollback, "prior_snapshot": "unchanged"},
            "provider.group-partial-valid",
        )
    return (
        {"adopted": [], "rolled_back": identifiers, "prior_snapshot": "unchanged"},
        "provider.group-rollback-valid",
    )


def plan(value: dict[str, Any]) -> tuple[list[str], str]:
    stages = ["observation", "assertion", "canonical_claim", "derived_claim"]
    tasks = {row["id"]: row for row in value["tasks"]}
    if len(tasks) != len(value["tasks"]):
        fail("provider.task-duplicate", "task IDs")
    for row in tasks.values():
        if stages.index(row["output_stage"]) < stages.index(row["input_stage"]):
            fail("provider.stage-regression", row["id"])
        if any(dependency not in tasks for dependency in row["depends_on"]):
            fail("provider.task-dependency-missing", row["id"])

    indegree = {identifier: 0 for identifier in tasks}
    consumers = {identifier: [] for identifier in tasks}
    for identifier, row in tasks.items():
        for dependency in row["depends_on"]:
            indegree[identifier] += 1
            consumers[dependency].append(identifier)
    key = lambda identifier: (
        stages.index(tasks[identifier]["output_stage"]),
        tasks[identifier]["provider_id"],
        tasks[identifier]["provider_version"],
        tasks[identifier]["binary_digest"],
        identifier,
    )
    ready = sorted((identifier for identifier, degree in indegree.items() if degree == 0), key=key)
    result: list[str] = []
    while ready:
        identifier = ready.pop(0)
        result.append(identifier)
        for consumer in consumers[identifier]:
            indegree[consumer] -= 1
            if indegree[consumer] == 0:
                ready.append(consumer)
                ready.sort(key=key)
    if len(result) != len(tasks):
        if value["profile"] == "NG0":
            if "fixed_point_contract" in value:
                fail("provider.fixed-point-unsupported", "NG0")
            fail("provider.dependency-cycle", "task graph")
        contract = value.get("fixed_point_contract", {})
        if set(contract) != FIXED_POINT_FIELDS:
            fail("provider.fixed-point-contract-incomplete", "NG1")
        cyclic = sorted(identifier for identifier, degree in indegree.items() if degree > 0)
        return ["fixed-point:" + "+".join(cyclic)], "provider.plan-fixed-point-valid"
    return result, "provider.plan-valid"


def reuse(value: dict[str, Any]) -> str:
    stored, requested = value["stored"], value["requested"]
    if stored["semantic_contract_digest"] != requested["semantic_contract_digest"]:
        fail("provider.reuse-semantic-mismatch", "semantic contract")
    if stored["binary_digest"] != requested["binary_digest"]:
        fail("provider.reuse-binary-mismatch", "binary digest")
    if any(stored.get(field) != requested.get(field) for field in REUSE_FIELDS):
        fail("provider.reuse-key-mismatch", "exact reuse tuple")
    return "reusable"


def failure(value: dict[str, Any], contract: dict[str, Any]) -> dict[str, Any]:
    if value["reason"] not in contract["failures"]["terminal_reasons"]:
        fail("provider.failure-reason-unknown", value["reason"])
    if not value["coverage_accounted"] or not value["unresolved_accounted"]:
        fail("provider.failure-accounting-missing", value["reason"])
    retained = (
        value["adopted_groups"]
        if value["partial_policy"] == "declared_dependency_groups"
        else []
    )
    return {
        "terminal": value["reason"],
        "retained": retained,
        "rolled_back": [value["current_group"]],
        "prior_snapshot": "unchanged",
    }


def surface_parity(value: dict[str, Any]) -> tuple[dict[str, Any], int]:
    results = []
    for surface in ("in_process", "out_of_process"):
        for order in ("forward", "reverse", "seeded-shuffle"):
            rows = copy.deepcopy(value["rows"])
            if order == "reverse":
                rows.reverse()
            elif order == "seeded-shuffle":
                random.Random(64).shuffle(rows)
            if surface == "out_of_process":
                rows = [decode_frame(_CONTRACT_CACHE, encode_frame(row))["control"] for row in rows]
            results.append(
                {"rows": sorted(rows, key=canonical_json), "coverage": sorted(value["coverage"])}
            )
    if len({canonical_json(row) for row in results}) != 1:
        fail("provider.surface-parity-mismatch", "surface/order matrix")
    return results[0], len(results)


def run_fuzz(contract: dict[str, Any], corpus: dict[str, Any]) -> dict[str, int]:
    stable = 0
    for case in corpus["cases"]:
        try:
            decode_frame(contract, mutate_frame(case["mutation"]))
            fail("provider.fuzz-case-accepted", case["id"])
        except ProviderContractError as error:
            if error.code != case["expected_reason"]:
                fail("provider.fuzz-reason-mismatch", f"{case['id']}: {error.code}")
            stable += 1
    return {
        "cases": len(corpus["cases"]),
        "stable_rejections": stable,
        "crashes": 0,
        "hangs": 0,
        "unbounded_allocations": 0,
    }


_CONTRACT_CACHE: dict[str, Any] = {}


def execute(
    contract: dict[str, Any], vector: dict[str, Any], root: pathlib.Path
) -> tuple[dict[str, Any], int, int]:
    global _CONTRACT_CACHE
    _CONTRACT_CACHE = contract
    operation, value = vector["operation"], vector["input"]
    comparisons = fuzz_cases = 0
    try:
        reason = f"provider.{operation}-valid"
        if operation == "wire":
            if value["action"] == "round_trip":
                frame = encode_frame(
                    value["control"], bytes.fromhex(value["payload_hex"]),
                    message_type=value["message_type"], flags=value["flags"],
                    stream_id=value["stream_id"], sequence=value["sequence"],
                )
                output = decode_frame(contract, frame)
            elif value["action"] == "mutate":
                output = decode_frame(contract, mutate_frame(value["mutation"]))
            else:
                output = decode_frame(
                    contract,
                    encode_frame({}, message_type=value["message_type"], flags=value["flags"]),
                )
        elif operation == "negotiate":
            output = negotiate(value)
        elif operation == "flow":
            output = flow(value)
        elif operation == "resume":
            output = resume(value)
        elif operation == "group":
            output, reason = group(value)
        elif operation == "plan":
            output, reason = plan(value)
        elif operation == "failure":
            output = failure(value, contract)
        elif operation == "reuse":
            output = reuse(value)
        elif operation == "surface_parity":
            output, comparisons = surface_parity(value)
        elif operation == "fuzz":
            output = run_fuzz(contract, load_yaml(root / value["corpus"]))
            fuzz_cases = output["cases"]
        else:
            fail("provider.operation-unknown", operation)
        return {"decision": "accepted", "reason_code": reason, "value": output}, comparisons, fuzz_cases
    except ProviderContractError as error:
        return {"decision": "rejected", "reason_code": error.code}, comparisons, fuzz_cases


def sample_manifest() -> dict[str, Any]:
    return {
        "schema": "cxxlens.provider-manifest.v1",
        "provider_id": "provider.cc.clang22",
        "provider_version": "1.0.0",
        "provider_binary_digest": "sha256:" + "a" * 64,
        "provider_semantic_contract_digest": "sha256:" + "b" * 64,
        "publisher": "cxxlens.project",
        "license": "Apache-2.0 WITH LLVM-exception",
        "signature": None,
        "protocol_range": {"major": 1, "minimum_minor": 0, "maximum_minor": 0, "required_features": ["streaming"], "optional_features": ["resume"]},
        "platform_tuples": ["linux-x86_64"],
        "offered_relations": ["cc.entity.v1"],
        "required_relations": [],
        "interpretation_domains": ["cc.canonical-1"],
        "invalidation_contract": "sha256:" + "c" * 64,
        "determinism_contract": "sha256:" + "d" * 64,
        "resource_class": "frontend-medium",
        "sandbox_minimum": "process-isolated",
        "task_stage": {"input": "observation", "output": "assertion"},
    }


def sample_task() -> dict[str, Any]:
    positive = {key: 1 for key in ("wall_ms", "cpu_ms", "rss_bytes", "output_bytes", "rows", "diagnostics", "open_files", "created_files", "subprocesses", "minimum_progress_bytes_per_second")}
    return {
        "schema": "cxxlens.provider-task.v1",
        "task_id": "task-1",
        "provider": {"id": "provider.cc.clang22", "version": "1.0.0", "binary_digest": "sha256:" + "a" * 64, "semantic_contract_digest": "sha256:" + "b" * 64},
        "outputs": ["cc.entity.v1"],
        "input_partitions": [],
        "condition": "condition-1",
        "interpretation": "cc.canonical-1",
        "budget": positive,
        "dependency_groups": [{"id": "dependency-1", "depends_on": [], "atomic_output_groups": ["output-1"]}],
        "partial_policy": "forbid",
    }


def validate_contract_shape(contract: dict[str, Any]) -> None:
    if FRAME.size != 104 or contract["wire"]["fixed_header_bytes"] != FRAME.size:
        fail("provider.wire-header-size-invalid", str(FRAME.size))
    if sum(row["bytes"] for row in contract["wire"]["fixed_header_fields"]) != FRAME.size:
        fail("provider.wire-header-layout-invalid", "field bytes")
    rows = contract["message_types"]["registry"]
    if len(rows) != 23 or len({row["id"] for row in rows}) != 23:
        fail("provider.message-registry-invalid", "message IDs")
    if contract["atomicity"]["partial_adoption"]["boundary"] != "dependency-group-only":
        fail("provider.atomic-boundary-invalid", "partial adoption")
    if contract["planning"]["cycle"] != "reject":
        fail("provider.dependency-cycle", "contract")
    if contract["reuse_and_invalidation"]["binary_digest_change"] != "invalidate":
        fail("provider.reuse-binary-mismatch", "contract")


def validate_design(root: pathlib.Path) -> None:
    design = (root / "docs/design/cxxlens_next_generation_integrated_design_ja.md").read_text(encoding="utf-8")
    for marker in (
        "0.9.0-normative", "cxxlens_ng_provider_protocol.yaml", "atomic_output_group",
        "dependency_group", "deterministic CBOR", "Issue #64",
    ):
        if marker not in design:
            fail("provider.design-marker-missing", marker)
    for stale in ("std::vector<relation_batch> batches", "exact wire encoding は ADR で確定する"):
        if stale in design:
            fail("provider.design-stale-contract", stale)
    index = (root / "docs/design/catalogs/README.md").read_text(encoding="utf-8")
    if "Provider Protocol" not in index or "accepted exact contract" not in index or "#64" not in index:
        fail("provider.catalog-index-stale", "provider protocol")


def validate_all(root: pathlib.Path) -> tuple[dict[str, Any], list[dict[str, Any]], int, int]:
    contract = load_yaml(root / CONTRACT)
    schema_validate(contract, load_yaml(root / CONTRACT_SCHEMA), "provider protocol")
    schema_validate(sample_manifest(), load_yaml(root / MANIFEST_SCHEMA), "provider manifest")
    schema_validate(sample_task(), load_yaml(root / TASK_SCHEMA), "provider task")
    corpus = load_yaml(root / FUZZ)
    schema_validate(corpus, load_yaml(root / FUZZ_SCHEMA), "provider fuzz corpus")
    validate_contract_shape(contract)
    validate_design(root)
    vectors = load_yaml(root / VECTORS)
    schema_validate(vectors, load_yaml(root / VECTORS_SCHEMA), "provider vectors")
    ids = [row["id"] for row in vectors["vectors"]]
    if len(ids) != len(set(ids)) or len(ids) != 34:
        fail("provider.vector-set-invalid", f"{len(ids)} vectors")
    results = []
    comparisons = fuzz_cases = 0
    for vector in vectors["vectors"]:
        actual, compared, fuzzed = execute(contract, vector, root)
        expected = vector["expected"]
        comparisons += compared
        fuzz_cases += fuzzed
        matched = actual["decision"] == expected["decision"] and actual["reason_code"] == expected["reason_code"] and ("value" not in expected or actual.get("value") == expected["value"])
        if not matched:
            fail("provider.vector-mismatch", f"{vector['id']}: {actual} != {expected}")
        if (vector["class"] == "positive") != (actual["decision"] == "accepted"):
            fail("provider.vector-class-mismatch", vector["id"])
        results.append({"id": vector["id"], **actual, "matched": True})
    if comparisons != 6 or fuzz_cases != len(corpus["cases"]):
        fail("provider.matrix-incomplete", f"surface={comparisons}, fuzz={fuzz_cases}")
    report = make_report(contract, results, comparisons, fuzz_cases)
    schema_validate(report, load_yaml(root / REPORT_SCHEMA), "provider report")
    return contract, results, comparisons, fuzz_cases


def make_report(contract: dict[str, Any], results: list[dict[str, Any]], comparisons: int, fuzz_cases: int) -> dict[str, Any]:
    return {
        "schema": "cxxlens.provider-conformance-report.v1",
        "contract_digest": digest(contract),
        "vector_results": results,
        "fuzz": {"cases": fuzz_cases, "stable_rejections": fuzz_cases, "crashes": 0, "hangs": 0, "unbounded_allocations": 0},
        "surface_matrix": {"surfaces": ["in_process", "out_of_process"], "orders": ["forward", "reverse", "seeded-shuffle"], "comparisons": comparisons, "all_equal": True},
        "status": "green",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("check", "report"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    contract, results, comparisons, fuzz_cases = validate_all(args.root.resolve())
    report = make_report(contract, results, comparisons, fuzz_cases)
    if args.mode == "report":
        rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.output:
            args.output.write_text(rendered, encoding="utf-8")
        else:
            print(rendered, end="")
    print(f"verified provider protocol: {len(results)} vectors, {fuzz_cases} fuzz cases, {comparisons} surface comparisons, {digest(contract)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ProviderContractError) as error:
        print(f"provider protocol failure: {error}", file=sys.stderr)
        raise SystemExit(1) from error
