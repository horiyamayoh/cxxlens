#!/usr/bin/env python3
"""Validate SQLite Store v3 qualification evidence independently and fail closed."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys
from collections.abc import Callable
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_sqlite_store_v3_qualification_report.schema.yaml"
)
SQLITE_CONTRACT = pathlib.Path("schemas/cxxlens_ng_sqlite_store_contract.yaml")
EXPECTED_SCHEMA_DIGEST = (
    "sha256:d59a5a10cb15bfa4cf1bcee0e758e17385132f45a71e801172071a7bc9984e8d"
)
CHUNK_MAXIMUM_BYTES = 8_388_608
UINT64_MAX = 18_446_744_073_709_551_615
COMMITTED_STATE = 3
PAYLOAD_SCHEMA_ORDER = tuple(
    f"cxxlens.ng-snapshot-payload.v{version}" for version in range(1, 6)
)
PAYLOAD_MAGICS = frozenset(PAYLOAD_SCHEMA_ORDER)
DIGEST_PATTERN = re.compile(r"^sha256:[0-9a-f]{64}$")
METHOD = {
    "id": "cxxlens.sqlite-store-v3-linux-parent-observation",
    "version": "1.0.0",
    "operating_system": "linux",
    "process_model": "parent-observed-child",
    "wall_elapsed_source": "clock-monotonic-nanoseconds",
    "peak_rss_source": "wait4-rusage-ru-maxrss-kibibytes",
    "disk_source": "held-file-family-fstat-byte-count",
    "chunk_census_source": "independent-read-only-sqlite-row-scan",
}
CONFIGURATIONS = ("static", "shared")
CASE_IDS = (
    "current-v3.0.0-cold-reopen",
    "v2.6.0-read-only-zero-mutation",
    "compact-v2.6.0-to-v3.0.0-same-semantic-digest",
    "limit-length-exceeding-valid-canonical-v5-reopened-parity",
)


class SQLiteStoreV3QualificationError(ValueError):
    """A SQLite Store v3 qualification invariant is not satisfied."""


def fail(message: str) -> None:
    raise SQLiteStoreV3QualificationError(message)


def require_uint64(value: Any, label: str) -> int:
    if type(value) is not int or value < 0 or value > UINT64_MAX:
        fail(f"{label} must be a uint64")
    return value


def require_digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or DIGEST_PATTERN.fullmatch(value) is None:
        fail(f"{label} must be a canonical SHA-256 digest")
    return value


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, yaml.YAMLError) as error:
        fail(f"cannot load YAML document {path}: {error}")
    if not isinstance(value, dict):
        fail(f"expected YAML object: {path}")
    return value


def _reject_duplicate_members(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            fail(f"duplicate JSON member: {key}")
        value[key] = item
    return value


def _reject_float(spelling: str) -> Any:
    fail(f"floating-point JSON value is forbidden: {spelling}")


def _reject_constant(spelling: str) -> Any:
    fail(f"non-finite JSON value is forbidden: {spelling}")


def load_report_bytes(raw: bytes, label: str = "qualification report") -> dict[str, Any]:
    """Strictly decode one already-held report byte occurrence."""

    if raw.startswith(b"\xef\xbb\xbf"):
        fail(f"{label} must not contain a UTF-8 BOM")
    try:
        text = raw.decode("utf-8", errors="strict")
        value = json.loads(
            text,
            object_pairs_hook=_reject_duplicate_members,
            parse_float=_reject_float,
            parse_constant=_reject_constant,
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        fail(f"{label} is not strict JSON: {error}")
    if not isinstance(value, dict):
        fail(f"{label} root must be an object")
    return value


def load_report(path: pathlib.Path) -> dict[str, Any]:
    try:
        raw = path.read_bytes()
    except OSError as error:
        fail(f"cannot read qualification report {path}: {error}")
    return load_report_bytes(raw, f"qualification report {path}")


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")


def document_digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value)).hexdigest()


def validate_schema(value: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(value)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"{label} schema validation failed: {error.message}")


def validate_documents(root: pathlib.Path) -> dict[str, Any]:
    schema = load_yaml(root / REPORT_SCHEMA)
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
    except jsonschema.SchemaError as error:
        fail(f"SQLite qualification report schema is invalid: {error.message}")
    actual = document_digest(schema)
    if actual != EXPECTED_SCHEMA_DIGEST:
        fail(
            "SQLite qualification report schema digest drifted: "
            f"expected={EXPECTED_SCHEMA_DIGEST}, actual={actual}"
        )
    return schema


def checked_sum(values: list[int], label: str) -> int:
    total = sum(values)
    if total > UINT64_MAX:
        fail(f"{label} exceeds the uint64 evidence domain")
    return total


def _require_canonical_order(
    values: list[Any], key: Callable[[Any], Any], label: str
) -> None:
    if values != sorted(values, key=key):
        fail(f"{label} is not in canonical order")


def chunk_census(inventory: dict[str, Any], label: str) -> dict[str, Any]:
    profile = inventory["profile"]
    publications = inventory["publications"]
    chunks = inventory["chunks"]
    if profile == "cxxlens.sqlite-v2-single-blob-not-applicable":
        if publications or chunks:
            fail(f"{label} v2 non-chunked inventory is not empty")
        return {
            "profile": profile,
            "chunked_publication_count": 0,
            "zero_payload_publication_count": 0,
            "logical_payload_bytes": 0,
            "declared_chunk_count": 0,
            "observed_chunk_row_count": 0,
            "observed_chunk_bytes": 0,
            "maximum_chunk_bytes_observed": 0,
            "nonfinal_chunk_row_count": 0,
            "final_chunk_row_count": 0,
        }
    if profile != "cxxlens.sqlite-payload-chunks.v1":
        fail(f"{label} has an unknown chunk profile: {profile}")

    publication_key = lambda row: (row["publication_id"], row["generation"])
    chunk_key = lambda row: (
        row["publication_id"],
        row["generation"],
        row["chunk_ordinal"],
    )
    _require_canonical_order(publications, publication_key, f"{label} publications")
    _require_canonical_order(chunks, chunk_key, f"{label} chunks")

    publications_by_key: dict[tuple[str, int], dict[str, Any]] = {}
    for publication in publications:
        key = publication_key(publication)
        if key in publications_by_key:
            fail(f"{label} has a duplicate publication key: {key}")
        publications_by_key[key] = publication

    chunks_by_publication: dict[tuple[str, int], list[dict[str, Any]]] = {
        key: [] for key in publications_by_key
    }
    for chunk in chunks:
        key = (chunk["publication_id"], chunk["generation"])
        if key not in chunks_by_publication:
            fail(f"{label} has an orphan chunk row: {key}")
        chunks_by_publication[key].append(chunk)

    zero_payload_count = 0
    nonfinal_count = 0
    final_count = 0
    for key, publication in publications_by_key.items():
        payload_bytes = publication["payload_byte_count"]
        expected_count = (
            0
            if payload_bytes == 0
            else 1 + (payload_bytes - 1) // CHUNK_MAXIMUM_BYTES
        )
        if publication["payload_chunk_count"] != expected_count:
            fail(f"{label} publication {key} has a noncanonical declared chunk count")
        rows = chunks_by_publication[key]
        if len(rows) != expected_count:
            fail(f"{label} publication {key} chunk-row census differs")
        if payload_bytes == 0:
            zero_payload_count += 1
            continue
        offset = 0
        for ordinal, row in enumerate(rows):
            if row["chunk_ordinal"] != ordinal:
                fail(f"{label} publication {key} has a chunk ordinal gap")
            if row["byte_offset"] != offset:
                fail(f"{label} publication {key} has a chunk offset gap")
            final = ordinal + 1 == expected_count
            expected_size = (
                payload_bytes - offset if final else CHUNK_MAXIMUM_BYTES
            )
            if row["byte_count"] != expected_size:
                fail(f"{label} publication {key} has a noncanonical chunk boundary")
            offset += row["byte_count"]
            if final:
                final_count += 1
            else:
                nonfinal_count += 1
        if offset != payload_bytes:
            fail(f"{label} publication {key} chunk bytes do not cover its payload")

    logical_payload_bytes = checked_sum(
        [row["payload_byte_count"] for row in publications],
        f"{label} logical payload census",
    )
    declared_chunk_count = checked_sum(
        [row["payload_chunk_count"] for row in publications],
        f"{label} declared chunk census",
    )
    observed_chunk_bytes = checked_sum(
        [row["byte_count"] for row in chunks], f"{label} observed chunk bytes"
    )
    return {
        "profile": profile,
        "chunked_publication_count": len(publications),
        "zero_payload_publication_count": zero_payload_count,
        "logical_payload_bytes": logical_payload_bytes,
        "declared_chunk_count": declared_chunk_count,
        "observed_chunk_row_count": len(chunks),
        "observed_chunk_bytes": observed_chunk_bytes,
        "maximum_chunk_bytes_observed": max(
            (row["byte_count"] for row in chunks), default=0
        ),
        "nonfinal_chunk_row_count": nonfinal_count,
        "final_chunk_row_count": final_count,
    }


def operational_measurement(raw: dict[str, Any], label: str) -> dict[str, Any]:
    if raw["method"] != METHOD:
        fail(f"{label} measurement method/version differs from the qualified method")
    process = raw["process"]
    expected_rss_bytes = process["peak_rss_kib"] * 1024
    if expected_rss_bytes > UINT64_MAX:
        fail(f"{label} peak RSS conversion exceeds uint64")
    if process["os_peak_rss_bytes"] != expected_rss_bytes:
        fail(f"{label} OS peak RSS bytes do not match wait4 ru_maxrss")
    disk = raw["disk"]
    expected_disk_total = checked_sum(
        [
            disk["main_bytes"],
            disk["wal_bytes"],
            disk["shm_bytes"],
            disk["journal_bytes"],
        ],
        f"{label} disk census",
    )
    if disk["total_bytes"] != expected_disk_total:
        fail(f"{label} total disk bytes do not equal the exact file-family sum")
    census = chunk_census(raw["chunk_inventory"], label)
    if (
        census["profile"] == "cxxlens.sqlite-payload-chunks.v1"
        and disk["total_bytes"] < census["observed_chunk_bytes"]
    ):
        fail(f"{label} disk census is smaller than the observed SQLite chunk bytes")
    return {
        "method": raw["method"],
        "os_peak_rss_bytes": process["os_peak_rss_bytes"],
        "wall_elapsed_ns": process["wall_elapsed_ns"],
        "disk_bytes": disk,
        "chunk_census": census,
    }


def _v3_current_measurement(
    raw: dict[str, Any], operational: dict[str, Any], label: str
) -> dict[str, Any]:
    observations = raw["observations"]
    initial = observations["initial_open"]
    reopened = observations["cold_reopen"]
    if initial != reopened:
        fail(f"{label} cold reopen projection differs from the initial v3 projection")
    return {
        "operational": operational,
        "physical_format": reopened["physical_format"],
        "readable_format": reopened["readable_format"],
        "metadata_digest": reopened["metadata_digest"],
        "canonical_ddl_digest": reopened["canonical_ddl_digest"],
        "chunk_profile": operational["chunk_census"]["profile"],
        "cold_reopen_validated": True,
        "semantic_projection_digest": reopened["semantic_projection_digest"],
    }


def _file_presence_and_size(entry: dict[str, Any]) -> tuple[str, int]:
    return entry["state"], entry["byte_count"]


def _migration_generation_evidence(
    source: list[dict[str, Any]],
    target: list[dict[str, Any]],
    cold: list[dict[str, Any]],
    label: str,
) -> dict[str, Any]:
    """Re-derive the whole-row deterministic generation replacement from raw evidence."""

    def uint64(value: Any, field: str) -> int:
        if type(value) is not int or value < 0 or value > UINT64_MAX:
            fail(f"{label} {field} is not uint64")
        return value

    def indexed(rows: list[dict[str, Any]], phase: str) -> dict[str, dict[str, Any]]:
        if rows != sorted(rows, key=lambda row: row["publication_id"]):
            fail(f"{label} {phase} generation census is not in canonical order")
        output: dict[str, dict[str, Any]] = {}
        for index, row in enumerate(rows):
            if set(row) != {"publication_id", "sequence", "state", "generation"}:
                fail(f"{label} {phase} generation row {index} fields differ")
            publication_id = row["publication_id"]
            if not isinstance(publication_id, str) or not publication_id:
                fail(f"{label} {phase} generation row {index} id differs")
            uint64(row["sequence"], f"{phase} sequence")
            state = uint64(row["state"], f"{phase} state")
            uint64(row["generation"], f"{phase} generation")
            if state > 5 or publication_id in output:
                fail(f"{label} {phase} generation census is invalid")
            output[publication_id] = row
        return output

    source_by_id = indexed(source, "source")
    target_by_id = indexed(target, "target")
    indexed(cold, "cold")
    if source_by_id.keys() != target_by_id.keys() or target != cold:
        fail(f"{label} migration generation key/cold census differs")
    committed = [row for row in source if row["state"] == 3]
    if not committed:
        fail(f"{label} migration qualification has no committed replacement rows")
    committed.sort(
        key=lambda row: (
            row["sequence"],
            row["generation"],
            row["publication_id"],
        )
    )
    maximum = max(row["generation"] for row in committed)
    if maximum > UINT64_MAX - len(committed):
        fail(f"{label} committed generation replacement overflows uint64")
    mapping: list[dict[str, Any]] = []
    for rank, source_row in enumerate(committed, start=1):
        target_row = target_by_id[source_row["publication_id"]]
        if (
            target_row["sequence"] != source_row["sequence"]
            or target_row["state"] != source_row["state"]
            or target_row["generation"] != maximum + rank
        ):
            fail(
                f"{label} committed generations are not the deterministic contiguous replacement"
            )
        mapping.append(
            {
                "publication_id": source_row["publication_id"],
                "sequence": source_row["sequence"],
                "state": source_row["state"],
                "source_generation": source_row["generation"],
                "target_generation": target_row["generation"],
            }
        )
    for publication_id, source_row in source_by_id.items():
        target_row = target_by_id[publication_id]
        if (
            target_row["sequence"] != source_row["sequence"]
            or target_row["state"] != source_row["state"]
            or (
                source_row["state"] != 3
                and target_row["generation"] != source_row["generation"]
            )
        ):
            fail(f"{label} noncommitted generation or row class changed")
    return {
        "committed_replacement_count": len(committed),
        "generation_mapping_digest": document_digest(mapping),
        "contiguous_committed_generation_replacement": True,
        "noncommitted_generation_preserved": True,
    }


def _migration_payload_evidence(
    source: dict[str, Any],
    target: dict[str, Any],
    cold: dict[str, Any],
    label: str,
) -> dict[str, Any]:
    """Independently re-derive legacy-schema and diagnostic preservation evidence."""

    def committed(value: dict[str, Any], phase: str) -> list[dict[str, str]]:
        rows = value["committed_payload_schema_census"]
        if rows != sorted(rows, key=lambda row: row["publication_id"]):
            fail(f"{label} {phase} committed payload schema census order differs")
        seen: set[str] = set()
        for index, row in enumerate(rows):
            if set(row) != {"publication_id", "payload_schema"}:
                fail(f"{label} {phase} committed payload schema row {index} differs")
            publication_id = row["publication_id"]
            if (
                not isinstance(publication_id, str)
                or not publication_id
                or publication_id in seen
                or row["payload_schema"] not in PAYLOAD_MAGICS
            ):
                fail(f"{label} {phase} committed payload schema census is invalid")
            seen.add(publication_id)
        if sorted(row["payload_schema"] for row in rows) != sorted(
            PAYLOAD_SCHEMA_ORDER
        ):
            fail(f"{label} {phase} committed payload schemas are not exact v1-v5")
        return rows

    source_committed = committed(source, "source")
    target_committed = committed(target, "target")
    cold_committed = committed(cold, "cold")
    if source_committed != target_committed or target_committed != cold_committed:
        fail(f"{label} migration rewrote a committed payload schema version")

    expected_diagnostic_fields = {
        "publication_id",
        "series_id",
        "snapshot_id",
        "sequence",
        "generation",
        "parent",
        "state",
        "payload_checksum",
        "payload_byte_count",
        "raw_payload_digest",
        "payload_schema",
        "diagnostic_classification",
    }

    def diagnostics(value: dict[str, Any], phase: str) -> list[dict[str, Any]]:
        rows = value["noncommitted_census"]
        if rows != sorted(rows, key=lambda row: row["publication_id"]):
            fail(f"{label} {phase} noncommitted census order differs")
        seen: set[str] = set()
        for index, row in enumerate(rows):
            if set(row) != expected_diagnostic_fields:
                fail(f"{label} {phase} noncommitted row {index} fields differ")
            publication_id = row["publication_id"]
            classification = row["diagnostic_classification"]
            if (
                not isinstance(publication_id, str)
                or not publication_id
                or publication_id in seen
                or type(row["state"]) is not int
                or row["state"] < 0
                or row["state"] > 5
                or row["state"] == COMMITTED_STATE
                or classification
                not in (
                    "stored-full-checksum-mismatch",
                    "payload-decode-failure",
                    "valid-noncommitted",
                )
            ):
                fail(f"{label} {phase} noncommitted census is invalid")
            seen.add(publication_id)
            for field in ("series_id", "snapshot_id"):
                if not isinstance(row[field], str) or not row[field]:
                    fail(f"{label} {phase} diagnostic {field} is invalid")
            if row["parent"] is not None and (
                not isinstance(row["parent"], str) or not row["parent"]
            ):
                fail(f"{label} {phase} diagnostic parent is invalid")
            for field in ("sequence", "generation", "state", "payload_byte_count"):
                require_uint64(row[field], f"{label} {phase} diagnostic {field}")
            require_digest(
                row["payload_checksum"], f"{label} {phase} diagnostic checksum"
            )
            require_digest(
                row["raw_payload_digest"],
                f"{label} {phase} diagnostic raw payload",
            )
            if row["payload_schema"] is not None and row["payload_schema"] not in (
                PAYLOAD_MAGICS
            ):
                fail(f"{label} {phase} diagnostic payload schema is invalid")
            checksum_matches = (
                row["payload_checksum"] == row["raw_payload_digest"]
            )
            if (classification == "stored-full-checksum-mismatch") == (
                checksum_matches
            ):
                fail(f"{label} {phase} diagnostic checksum verdict is inconsistent")
            if classification == "payload-decode-failure" and row[
                "payload_schema"
            ] is not None:
                fail(f"{label} {phase} decode failure retained a payload schema")
            if classification == "valid-noncommitted" and row[
                "payload_schema"
            ] not in PAYLOAD_MAGICS:
                fail(f"{label} {phase} valid diagnostic lacks a payload schema")
        if value["noncommitted_census_digest"] != document_digest(rows):
            fail(f"{label} {phase} noncommitted census digest differs")
        return rows

    source_diagnostics = diagnostics(source, "source")
    target_diagnostics = diagnostics(target, "target")
    cold_diagnostics = diagnostics(cold, "cold")
    if source_diagnostics != target_diagnostics or target_diagnostics != cold_diagnostics:
        fail(f"{label} migration changed a noncommitted raw row or typed verdict")
    if not any(
        row["diagnostic_classification"]
        in ("stored-full-checksum-mismatch", "payload-decode-failure")
        for row in source_diagnostics
    ):
        fail(f"{label} migration qualification has no corrupt noncommitted diagnostic")
    return {
        "committed_payload_schema_census_digest": document_digest(source_committed),
        "noncommitted_census_digest": document_digest(source_diagnostics),
        "diagnostic_noncommitted_count": len(source_diagnostics),
        "committed_payload_schemas_preserved": True,
        "noncommitted_rows_preserved": True,
    }


def _v2_read_only_measurement(
    raw: dict[str, Any], operational: dict[str, Any], label: str
) -> dict[str, Any]:
    observations = raw["observations"]
    before = observations["before"]
    after = observations["after"]
    _require_canonical_order(
        before["directory_entries"], lambda value: value, f"{label} before directory"
    )
    _require_canonical_order(
        after["directory_entries"], lambda value: value, f"{label} after directory"
    )
    main_wal_journal_unchanged = all(
        before[role] == after[role] for role in ("main", "wal", "journal")
    )
    shm_unchanged = _file_presence_and_size(before["shm"]) == (
        _file_presence_and_size(after["shm"])
    )
    sidecars_stable = all(
        _file_presence_and_size(before[role])
        == _file_presence_and_size(after[role])
        for role in ("wal", "shm", "journal")
    )
    directory_unchanged = before["directory_entries"] == after["directory_entries"]
    if not (
        main_wal_journal_unchanged
        and shm_unchanged
        and sidecars_stable
        and directory_unchanged
    ):
        fail(f"{label} v2 read-only route changed its file-family evidence")
    expected_disk = {
        "main_bytes": after["main"]["byte_count"],
        "wal_bytes": after["wal"]["byte_count"],
        "shm_bytes": after["shm"]["byte_count"],
        "journal_bytes": after["journal"]["byte_count"],
        "total_bytes": sum(
            after[role]["byte_count"] for role in ("main", "wal", "shm", "journal")
        ),
    }
    if raw["disk"] != expected_disk:
        fail(f"{label} v2 disk census differs from the post-read file family")
    return {
        "operational": operational,
        "physical_format": observations["physical_format"],
        "reported_readable_format": observations["reported_readable_format"],
        "direct_open": observations["direct_open"],
        "migration_required": observations["migration_required"],
        "semantic_projection_digest": observations["semantic_projection_digest"],
        "main_wal_journal_bytes_unchanged": main_wal_journal_unchanged,
        "shm_presence_and_size_unchanged": shm_unchanged,
        "sidecar_create_delete_or_resize": not sidecars_stable,
        "directory_entry_set_unchanged": directory_unchanged,
        "begin_result": observations["begin_result"],
    }


def _migration_measurement(
    raw: dict[str, Any], operational: dict[str, Any], label: str
) -> dict[str, Any]:
    observations = raw["observations"]
    source = observations["source"]
    target = observations["target"]
    reopened = observations["cold_reopen"]
    if source["physical_format"] != "cxxlens.sqlite-semantic-store.v2":
        fail(f"{label} migration source is not exact v2")
    if target["physical_format"] != "cxxlens.sqlite-semantic-store.v3" or reopened[
        "physical_format"
    ] != "cxxlens.sqlite-semantic-store.v3":
        fail(f"{label} migration target/cold reopen is not exact v3")
    semantic_digests = {
        source["semantic_projection_digest"],
        target["semantic_projection_digest"],
        reopened["semantic_projection_digest"],
    }
    if len(semantic_digests) != 1:
        fail(f"{label} migration semantic projection changed")
    non_generation_digests = {
        source["non_generation_payload_projection_digest"],
        target["non_generation_payload_projection_digest"],
        reopened["non_generation_payload_projection_digest"],
    }
    if len(non_generation_digests) != 1:
        fail(f"{label} migration changed a non-generation payload projection")
    generation_evidence = _migration_generation_evidence(
        source["generation_census"],
        target["generation_census"],
        reopened["generation_census"],
        label,
    )
    payload_evidence = _migration_payload_evidence(source, target, reopened, label)
    for key, value in generation_evidence.items():
        if observations.get(key) != value:
            fail(f"{label} migration generation evidence differs: {key}")
    for key, value in payload_evidence.items():
        if observations.get(key) != value:
            fail(f"{label} migration payload evidence differs: {key}")
    return {
        "operational": operational,
        "source_format": source["physical_format"],
        "target_format": target["physical_format"],
        "trigger": observations["trigger"],
        "one_begin_immediate": observations["begin_immediate_count"] == 1,
        "pre_semantic_projection_digest": source["semantic_projection_digest"],
        "post_semantic_projection_digest": target["semantic_projection_digest"],
        "semantic_projection_equal": len(semantic_digests) == 1,
        "physical_generation_only_payload_transition": (
            len(non_generation_digests) == 1
            and generation_evidence["contiguous_committed_generation_replacement"]
        ),
        **generation_evidence,
        **payload_evidence,
        "canonical_ddl_digest": observations["canonical_ddl_digest"],
        "cold_reopen_validated": (
            target["semantic_projection_digest"]
            == reopened["semantic_projection_digest"]
        ),
    }


def _limit_parity_measurement(
    raw: dict[str, Any], operational: dict[str, Any], label: str
) -> dict[str, Any]:
    observations = raw["observations"]
    limit = observations["sqlite_limit_length_bytes"]
    payload = observations["logical_payload_bytes"]
    if payload <= limit:
        fail(f"{label} canonical-v5 payload does not exceed the actual SQLite limit")
    census = operational["chunk_census"]
    if census["profile"] != "cxxlens.sqlite-payload-chunks.v1":
        fail(f"{label} limit-parity evidence is not a v3 chunk inventory")
    if census["logical_payload_bytes"] != payload:
        fail(f"{label} chunk census does not cover the exact logical payload")
    memory = observations["memory"]
    sqlite = observations["sqlite"]
    reopened = observations["cold_reopen_sqlite"]
    semantic_equal = len(
        {
            memory["semantic_projection_digest"],
            sqlite["semantic_projection_digest"],
            reopened["semantic_projection_digest"],
        }
    ) == 1
    export_equal = len(
        {
            memory["canonical_export_digest"],
            sqlite["canonical_export_digest"],
            reopened["canonical_export_digest"],
        }
    ) == 1
    query_equal = len(
        {
            memory["query_projection_digest"],
            sqlite["query_projection_digest"],
            reopened["query_projection_digest"],
        }
    ) == 1
    if not (semantic_equal and export_equal and query_equal):
        fail(f"{label} memory/SQLite/cold-reopen parity projection differs")
    resident = observations["maximum_resident_payload_buffer_bytes"]
    if operational["os_peak_rss_bytes"] < resident:
        fail(f"{label} OS peak RSS is smaller than the resident payload buffer")
    return {
        "operational": operational,
        "sqlite_limit_length_bytes": limit,
        "logical_payload_bytes": payload,
        "payload_exceeds_sqlite_limit_length": payload > limit,
        "canonical_v5_payload_digest": observations["canonical_v5_payload_digest"],
        "maximum_chunk_bytes": CHUNK_MAXIMUM_BYTES,
        "maximum_resident_payload_buffer_bytes": resident,
        "bounded_streaming_verified": resident <= CHUNK_MAXIMUM_BYTES,
        "memory_semantic_projection_digest": memory["semantic_projection_digest"],
        "sqlite_semantic_projection_digest": sqlite["semantic_projection_digest"],
        "semantic_projection_equal": semantic_equal,
        "canonical_export_equal": export_equal,
        "query_projection_equal": query_equal,
        "cold_reopen_validated": (
            sqlite["semantic_projection_digest"]
            == reopened["semantic_projection_digest"]
        ),
    }


def case_evidence_projection(configuration: str, case: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema": "cxxlens.sqlite-store-v3-case-evidence.v1",
        "configuration": configuration,
        "case_id": case["id"],
        "backend_scope": case["backend_scope"],
        "result": case["result"],
        "raw_evidence_digest": case["raw_evidence_digest"],
        "measurement": case["measurement"],
    }


def validate_case(configuration: str, case: dict[str, Any]) -> None:
    label = f"{configuration}/{case['id']}"
    raw = case["raw_evidence"]
    if (
        raw["configuration"] != configuration
        or raw["case_id"] != case["id"]
        or raw["backend_scope"] != case["backend_scope"]
    ):
        fail(f"{label} raw evidence identity differs from its report position")
    raw_digest = document_digest(raw)
    if case["raw_evidence_digest"] != raw_digest:
        fail(f"{label} raw evidence digest differs from the canonical raw evidence")
    operational = operational_measurement(raw, label)
    validators: dict[
        str, Callable[[dict[str, Any], dict[str, Any], str], dict[str, Any]]
    ] = {
        CASE_IDS[0]: _v3_current_measurement,
        CASE_IDS[1]: _v2_read_only_measurement,
        CASE_IDS[2]: _migration_measurement,
        CASE_IDS[3]: _limit_parity_measurement,
    }
    expected_measurement = validators[case["id"]](raw, operational, label)
    if case["measurement"] != expected_measurement:
        fail(f"{label} measurement differs from the independently derived projection")
    expected_evidence_digest = document_digest(
        case_evidence_projection(configuration, case)
    )
    if case["evidence_digest"] != expected_evidence_digest:
        fail(f"{label} evidence digest differs from the validated case projection")


def report_set_projection(report: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema": "cxxlens.sqlite-store-v3-qualification-report-set.v1",
        "report_schema_digest": report["report_schema_digest"],
        "revision": report["revision"],
        "source_tree": report["source_tree"],
        "sqlite_contract_digest": report["sqlite_contract_digest"],
        "configurations": [
            {
                "configuration": configuration["configuration"],
                "backends": configuration["backends"],
                "cases": [
                    {
                        "id": case["id"],
                        "backend_scope": case["backend_scope"],
                        "evidence_digest": case["evidence_digest"],
                    }
                    for case in configuration["cases"]
                ],
            }
            for configuration in report["configurations"]
        ],
    }


def _cross_configuration_projection(case: dict[str, Any]) -> dict[str, Any]:
    measurement = case["measurement"]
    if case["id"] == CASE_IDS[0]:
        return {
            key: measurement[key]
            for key in (
                "physical_format",
                "readable_format",
                "metadata_digest",
                "canonical_ddl_digest",
                "chunk_profile",
                "semantic_projection_digest",
            )
        }
    if case["id"] == CASE_IDS[1]:
        return {
            key: measurement[key]
            for key in (
                "physical_format",
                "reported_readable_format",
                "direct_open",
                "migration_required",
                "semantic_projection_digest",
                "begin_result",
            )
        }
    if case["id"] == CASE_IDS[2]:
        return {
            key: measurement[key]
            for key in (
                "source_format",
                "target_format",
                "trigger",
                "pre_semantic_projection_digest",
                "post_semantic_projection_digest",
                "committed_replacement_count",
                "generation_mapping_digest",
                "committed_payload_schema_census_digest",
                "noncommitted_census_digest",
                "diagnostic_noncommitted_count",
                "canonical_ddl_digest",
            )
        }
    observations = case["raw_evidence"]["observations"]
    return {
        "sqlite_limit_length_bytes": measurement["sqlite_limit_length_bytes"],
        "logical_payload_bytes": measurement["logical_payload_bytes"],
        "canonical_v5_payload_digest": measurement["canonical_v5_payload_digest"],
        "memory": observations["memory"],
        "sqlite": observations["sqlite"],
        "cold_reopen_sqlite": observations["cold_reopen_sqlite"],
    }


def validate_cross_configuration_parity(report: dict[str, Any]) -> None:
    static_cases = report["configurations"][0]["cases"]
    shared_cases = report["configurations"][1]["cases"]
    for static_case, shared_case in zip(static_cases, shared_cases, strict=True):
        if static_case["id"] != shared_case["id"]:
            fail("static/shared case order differs")
        if _cross_configuration_projection(static_case) != _cross_configuration_projection(
            shared_case
        ):
            fail(f"static/shared semantic projection differs for {static_case['id']}")


def validate_report(
    root: pathlib.Path,
    report: dict[str, Any],
    *,
    expected_revision: str,
    expected_source_tree: str,
) -> None:
    schema = validate_documents(root)
    validate_schema(report, schema, "SQLite Store v3 qualification report")
    if report["report_schema_digest"] != EXPECTED_SCHEMA_DIGEST:
        fail("qualification report is not bound to the exact report schema digest")
    contract = load_yaml(root / SQLITE_CONTRACT)
    contract_digest = document_digest(contract)
    if report["sqlite_contract_digest"] != contract_digest:
        fail("qualification report SQLite contract digest differs from the source tree")
    try:
        ddl_authority = contract["schema_profiles"]["current_v3"]["canonical_ddl"]
        ddl_statements = ddl_authority["statements"]
        expected_ddl_digest = ddl_authority["digest"]
    except (KeyError, TypeError):
        fail("SQLite contract lacks the current-v3 canonical DDL authority")
    if (
        not isinstance(ddl_statements, list)
        or len(ddl_statements) != 6
        or document_digest(ddl_statements) != expected_ddl_digest
    ):
        fail("SQLite contract canonical DDL statements/digest differ")
    if report["revision"] != expected_revision:
        fail("qualification report revision differs from the expected exact revision")
    if report["source_tree"] != expected_source_tree:
        fail("qualification report source tree differs from the expected exact tree")
    for expected_configuration, configuration in zip(
        CONFIGURATIONS, report["configurations"], strict=True
    ):
        if configuration["configuration"] != expected_configuration:
            fail("qualification configuration order differs from static/shared authority")
        for expected_case, case in zip(CASE_IDS, configuration["cases"], strict=True):
            if case["id"] != expected_case:
                fail(f"{expected_configuration} case order differs from authority")
            validate_case(expected_configuration, case)
            if expected_case in (CASE_IDS[0], CASE_IDS[2]) and case["measurement"][
                "canonical_ddl_digest"
            ] != expected_ddl_digest:
                fail(
                    f"{expected_configuration}/{expected_case} canonical DDL digest "
                    "differs from the exact six contract statements"
                )
    validate_cross_configuration_parity(report)
    expected_report_set_digest = document_digest(report_set_projection(report))
    if report["report_set_digest"] != expected_report_set_digest:
        fail("qualification report-set digest differs from the exact 2x4 evidence set")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("contract", "check"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("--expected-revision")
    parser.add_argument("--expected-source-tree")
    arguments = parser.parse_args()
    root = arguments.root.resolve()
    try:
        if arguments.command == "contract":
            validate_documents(root)
            print(
                "verified SQLite Store v3 qualification report schema: "
                f"{EXPECTED_SCHEMA_DIGEST}"
            )
            return 0
        if (
            arguments.report is None
            or arguments.expected_revision is None
            or arguments.expected_source_tree is None
        ):
            fail(
                "check requires --report, --expected-revision, and "
                "--expected-source-tree"
            )
        report = load_report(arguments.report)
        validate_report(
            root,
            report,
            expected_revision=arguments.expected_revision,
            expected_source_tree=arguments.expected_source_tree,
        )
    except SQLiteStoreV3QualificationError as error:
        print(f"SQLite Store v3 qualification check failed: {error}", file=sys.stderr)
        return 1
    print(
        "verified SQLite Store v3 qualification report: "
        f"{arguments.expected_revision}; {report['report_set_digest']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
