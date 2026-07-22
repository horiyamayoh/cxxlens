#!/usr/bin/env python3
"""Fail-closed tests for SQLite Store v3 qualification evidence."""

from __future__ import annotations

import copy
import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import check_ng_sqlite_store_v3_qualification as qualification  # noqa: E402


REVISION = "1" * 40
SOURCE_TREE = "2" * 40
DDL_DIGEST = qualification.load_yaml(ROOT / qualification.SQLITE_CONTRACT)[
    "schema_profiles"
]["current_v3"]["canonical_ddl"]["digest"]


def digest(digit: str) -> str:
    return "sha256:" + digit * 64


def chunk_inventory(publication_id: str, payload_bytes: int) -> dict:
    chunk_count = (
        0
        if payload_bytes == 0
        else 1 + (payload_bytes - 1) // qualification.CHUNK_MAXIMUM_BYTES
    )
    chunks = []
    offset = 0
    for ordinal in range(chunk_count):
        byte_count = min(
            qualification.CHUNK_MAXIMUM_BYTES, payload_bytes - offset
        )
        chunks.append(
            {
                "publication_id": publication_id,
                "generation": 7,
                "chunk_ordinal": ordinal,
                "byte_offset": offset,
                "byte_count": byte_count,
                "chunk_checksum": digest("b"),
            }
        )
        offset += byte_count
    return {
        "profile": "cxxlens.sqlite-payload-chunks.v1",
        "publications": [
            {
                "publication_id": publication_id,
                "generation": 7,
                "payload_byte_count": payload_bytes,
                "payload_chunk_count": chunk_count,
                "payload_checksum": digest("a"),
            }
        ],
        "chunks": chunks,
    }


def file_entry(byte_count: int, digit: str) -> dict:
    if byte_count == 0:
        return {"state": "absent", "byte_count": 0, "sha256": None}
    return {
        "state": "present",
        "byte_count": byte_count,
        "sha256": digest(digit),
    }


def file_family() -> dict:
    return {
        "main": file_entry(4096, "4"),
        "wal": file_entry(0, "5"),
        "shm": file_entry(0, "6"),
        "journal": file_entry(0, "7"),
        "directory_entries": ["database.sqlite"],
    }


def common_raw(
    configuration: str,
    case_id: str,
    backend_scope: str,
    inventory: dict,
    observations: dict,
) -> dict:
    chunk_bytes = sum(row["byte_count"] for row in inventory["chunks"])
    main_bytes = max(4096, chunk_bytes + 8192)
    return {
        "schema": "cxxlens.sqlite-store-v3-raw-evidence.v1",
        "configuration": configuration,
        "case_id": case_id,
        "backend_scope": backend_scope,
        "method": copy.deepcopy(qualification.METHOD),
        "process": {
            "exit_status": 0,
            "wall_elapsed_ns": 123456,
            "peak_rss_kib": 32768,
            "os_peak_rss_bytes": 32768 * 1024,
        },
        "disk": {
            "main_bytes": main_bytes,
            "wal_bytes": 0,
            "shm_bytes": 0,
            "journal_bytes": 0,
            "total_bytes": main_bytes,
        },
        "chunk_inventory": inventory,
        "observations": observations,
    }


def current_raw(configuration: str) -> dict:
    observation = {
        "physical_format": "cxxlens.sqlite-semantic-store.v3",
        "readable_format": "3.0.0",
        "metadata_digest": digest("3"),
        "canonical_ddl_digest": DDL_DIGEST,
        "semantic_projection_digest": digest("5"),
    }
    return common_raw(
        configuration,
        qualification.CASE_IDS[0],
        "sqlite",
        chunk_inventory("publication:current", qualification.CHUNK_MAXIMUM_BYTES + 3),
        {"initial_open": copy.deepcopy(observation), "cold_reopen": observation},
    )


def v2_raw(configuration: str) -> dict:
    before = file_family()
    raw = common_raw(
        configuration,
        qualification.CASE_IDS[1],
        "sqlite",
        {
            "profile": "cxxlens.sqlite-v2-single-blob-not-applicable",
            "publications": [],
            "chunks": [],
        },
        {
            "physical_format": "cxxlens.sqlite-semantic-store.v2",
            "reported_readable_format": "2.6.0",
            "direct_open": True,
            "migration_required": True,
            "semantic_projection_digest": digest("5"),
            "begin_result": {
                "code": "store.migration-required",
                "field": "sqlite-physical-format",
                "detail": "cxxlens.sqlite-semantic-store.v2-to-v3",
            },
            "before": before,
            "after": copy.deepcopy(before),
        },
    )
    raw["disk"] = {
        "main_bytes": 4096,
        "wal_bytes": 0,
        "shm_bytes": 0,
        "journal_bytes": 0,
        "total_bytes": 4096,
    }
    return raw


def migration_raw(configuration: str) -> dict:
    source_census = [
        {
            "publication_id": "publication:diag",
            "sequence": 0,
            "state": 4,
            "generation": 20,
        },
        {
            "publication_id": "publication:v1",
            "sequence": 1,
            "state": 3,
            "generation": 2,
        },
        {
            "publication_id": "publication:v2",
            "sequence": 2,
            "state": 3,
            "generation": 4,
        },
        {
            "publication_id": "publication:v3",
            "sequence": 3,
            "state": 3,
            "generation": 7,
        },
        {
            "publication_id": "publication:v4",
            "sequence": 4,
            "state": 3,
            "generation": 11,
        },
        {
            "publication_id": "publication:v5",
            "sequence": 5,
            "state": 3,
            "generation": 13,
        },
    ]
    target_census = [
        {
            "publication_id": "publication:diag",
            "sequence": 0,
            "state": 4,
            "generation": 20,
        },
        {
            "publication_id": "publication:v1",
            "sequence": 1,
            "state": 3,
            "generation": 14,
        },
        {
            "publication_id": "publication:v2",
            "sequence": 2,
            "state": 3,
            "generation": 15,
        },
        {
            "publication_id": "publication:v3",
            "sequence": 3,
            "state": 3,
            "generation": 16,
        },
        {
            "publication_id": "publication:v4",
            "sequence": 4,
            "state": 3,
            "generation": 17,
        },
        {
            "publication_id": "publication:v5",
            "sequence": 5,
            "state": 3,
            "generation": 18,
        },
    ]
    committed_payload_schema_census = [
        {
            "publication_id": f"publication:v{version}",
            "payload_schema": f"cxxlens.ng-snapshot-payload.v{version}",
        }
        for version in range(1, 6)
    ]
    noncommitted_census = [
        {
            "publication_id": "publication:diag",
            "series_id": "series:diag",
            "snapshot_id": "snapshot:diag",
            "sequence": 0,
            "generation": 20,
            "parent": None,
            "state": 4,
            "payload_checksum": digest("c"),
            "payload_byte_count": 17,
            "raw_payload_digest": digest("d"),
            "payload_schema": "cxxlens.ng-snapshot-payload.v1",
            "diagnostic_classification": "stored-full-checksum-mismatch",
        }
    ]
    source = {
        "physical_format": "cxxlens.sqlite-semantic-store.v2",
        "semantic_projection_digest": digest("6"),
        "non_generation_payload_projection_digest": digest("7"),
        "generation_census": source_census,
        "committed_payload_schema_census": copy.deepcopy(
            committed_payload_schema_census
        ),
        "noncommitted_census": copy.deepcopy(noncommitted_census),
        "noncommitted_census_digest": qualification.document_digest(
            noncommitted_census
        ),
    }
    target = {
        "physical_format": "cxxlens.sqlite-semantic-store.v3",
        "semantic_projection_digest": digest("6"),
        "non_generation_payload_projection_digest": digest("7"),
        "generation_census": target_census,
        "committed_payload_schema_census": copy.deepcopy(
            committed_payload_schema_census
        ),
        "noncommitted_census": copy.deepcopy(noncommitted_census),
        "noncommitted_census_digest": qualification.document_digest(
            noncommitted_census
        ),
    }
    generation_evidence = qualification._migration_generation_evidence(
        source_census, target_census, target_census, "fixture"
    )
    payload_evidence = qualification._migration_payload_evidence(
        source, target, target, "fixture"
    )
    return common_raw(
        configuration,
        qualification.CASE_IDS[2],
        "sqlite",
        chunk_inventory("publication:migrated", 17),
        {
            "trigger": "snapshot-store-compact",
            "begin_immediate_count": 1,
            "source": source,
            "target": copy.deepcopy(target),
            "cold_reopen": target,
            **generation_evidence,
            **payload_evidence,
            "canonical_ddl_digest": DDL_DIGEST,
        },
    )


def limit_raw(configuration: str) -> dict:
    projection = {
        "semantic_projection_digest": digest("8"),
        "canonical_export_digest": digest("9"),
        "query_projection_digest": digest("a"),
    }
    payload_bytes = 16_777_217
    return common_raw(
        configuration,
        qualification.CASE_IDS[3],
        "memory-sqlite-parity",
        chunk_inventory("publication:limit", payload_bytes),
        {
            "sqlite_limit_length_bytes": 16_777_216,
            "logical_payload_bytes": payload_bytes,
            "canonical_v5_payload_digest": digest("b"),
            "maximum_resident_payload_buffer_bytes": (
                qualification.CHUNK_MAXIMUM_BYTES
            ),
            "memory": copy.deepcopy(projection),
            "sqlite": copy.deepcopy(projection),
            "cold_reopen_sqlite": projection,
        },
    )


def seal_case(configuration: str, raw: dict) -> dict:
    operational = qualification.operational_measurement(
        raw, f"fixture/{configuration}/{raw['case_id']}"
    )
    builders = {
        qualification.CASE_IDS[0]: qualification._v3_current_measurement,
        qualification.CASE_IDS[1]: qualification._v2_read_only_measurement,
        qualification.CASE_IDS[2]: qualification._migration_measurement,
        qualification.CASE_IDS[3]: qualification._limit_parity_measurement,
    }
    case = {
        "id": raw["case_id"],
        "backend_scope": raw["backend_scope"],
        "result": "passed",
        "raw_evidence": raw,
        "raw_evidence_digest": qualification.document_digest(raw),
        "evidence_digest": digest("0"),
        "measurement": builders[raw["case_id"]](
            raw, operational, f"fixture/{configuration}/{raw['case_id']}"
        ),
    }
    case["evidence_digest"] = qualification.document_digest(
        qualification.case_evidence_projection(configuration, case)
    )
    return case


def valid_report() -> dict:
    configurations = []
    for configuration in qualification.CONFIGURATIONS:
        configurations.append(
            {
                "configuration": configuration,
                "backends": ["memory", "sqlite"],
                "cases": [
                    seal_case(configuration, current_raw(configuration)),
                    seal_case(configuration, v2_raw(configuration)),
                    seal_case(configuration, migration_raw(configuration)),
                    seal_case(configuration, limit_raw(configuration)),
                ],
            }
        )
    report = {
        "schema": "cxxlens.ng-sqlite-store-v3-qualification-report.v1",
        "report_schema_digest": qualification.EXPECTED_SCHEMA_DIGEST,
        "revision": REVISION,
        "source_tree": SOURCE_TREE,
        "sqlite_contract_digest": qualification.document_digest(
            qualification.load_yaml(ROOT / qualification.SQLITE_CONTRACT)
        ),
        "configurations": configurations,
        "report_set_digest": digest("0"),
        "status": "green",
    }
    report["report_set_digest"] = qualification.document_digest(
        qualification.report_set_projection(report)
    )
    return report


def reseal_raw(report: dict, configuration_index: int, case_index: int) -> None:
    configuration = report["configurations"][configuration_index]
    case = configuration["cases"][case_index]
    case["raw_evidence_digest"] = qualification.document_digest(
        case["raw_evidence"]
    )
    case["evidence_digest"] = qualification.document_digest(
        qualification.case_evidence_projection(
            configuration["configuration"], case
        )
    )
    report["report_set_digest"] = qualification.document_digest(
        qualification.report_set_projection(report)
    )


def reseal_case(report: dict, configuration_index: int, case_index: int) -> None:
    configuration = report["configurations"][configuration_index]
    raw = configuration["cases"][case_index]["raw_evidence"]
    configuration["cases"][case_index] = seal_case(
        configuration["configuration"], raw
    )
    report["report_set_digest"] = qualification.document_digest(
        qualification.report_set_projection(report)
    )


class NgSQLiteStoreV3QualificationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.schema = qualification.validate_documents(ROOT)

    def setUp(self) -> None:
        self.report = valid_report()

    def validate(self) -> None:
        qualification.validate_report(
            ROOT,
            self.report,
            expected_revision=REVISION,
            expected_source_tree=SOURCE_TREE,
        )

    def test_exact_static_shared_four_case_report_passes(self) -> None:
        self.validate()

    def test_schema_digest_drift_is_rejected(self) -> None:
        self.report["report_schema_digest"] = digest("f")
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "exact report schema"
        ):
            self.validate()

    def test_contract_digest_drift_is_rejected(self) -> None:
        self.report["sqlite_contract_digest"] = digest("f")
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "contract digest differs"
        ):
            self.validate()

    def test_raw_evidence_digest_drift_is_rejected(self) -> None:
        self.report["configurations"][0]["cases"][0][
            "raw_evidence_digest"
        ] = digest("f")
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "raw evidence digest"
        ):
            self.validate()

    def test_case_evidence_digest_drift_is_rejected(self) -> None:
        case = self.report["configurations"][0]["cases"][0]
        case["evidence_digest"] = digest("f")
        self.report["report_set_digest"] = qualification.document_digest(
            qualification.report_set_projection(self.report)
        )
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "evidence digest"
        ):
            self.validate()

    def test_report_set_digest_drift_is_rejected(self) -> None:
        self.report["report_set_digest"] = digest("f")
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "report-set digest"
        ):
            self.validate()

    def test_raw_configuration_swap_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][0]["raw_evidence"]
        raw["configuration"] = "shared"
        reseal_raw(self.report, 0, 0)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "identity differs"
        ):
            self.validate()

    def test_chunk_ordinal_gap_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][0]["raw_evidence"]
        raw["chunk_inventory"]["chunks"][1]["chunk_ordinal"] = 2
        reseal_raw(self.report, 0, 0)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "chunk ordinal gap"
        ):
            self.validate()

    def test_chunk_offset_gap_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][0]["raw_evidence"]
        raw["chunk_inventory"]["chunks"][1]["byte_offset"] += 1
        reseal_raw(self.report, 0, 0)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "chunk offset gap"
        ):
            self.validate()

    def test_noncanonical_chunk_boundary_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][0]["raw_evidence"]
        raw["chunk_inventory"]["chunks"][0]["byte_count"] -= 1
        reseal_raw(self.report, 0, 0)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "noncanonical chunk boundary",
        ):
            self.validate()

    def test_duplicate_publication_key_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][0]["raw_evidence"]
        duplicate = copy.deepcopy(raw["chunk_inventory"]["publications"][0])
        duplicate["payload_checksum"] = digest("c")
        raw["chunk_inventory"]["publications"].append(duplicate)
        reseal_raw(self.report, 0, 0)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "duplicate publication key",
        ):
            self.validate()

    def test_derived_chunk_census_drift_is_rejected(self) -> None:
        case = self.report["configurations"][0]["cases"][0]
        case["measurement"]["operational"]["chunk_census"][
            "observed_chunk_row_count"
        ] += 1
        case["evidence_digest"] = qualification.document_digest(
            qualification.case_evidence_projection("static", case)
        )
        self.report["report_set_digest"] = qualification.document_digest(
            qualification.report_set_projection(self.report)
        )
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "independently derived projection",
        ):
            self.validate()

    def test_peak_rss_unit_conversion_drift_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][0]["raw_evidence"]
        raw["process"]["os_peak_rss_bytes"] += 1
        reseal_raw(self.report, 0, 0)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "OS peak RSS bytes"
        ):
            self.validate()

    def test_disk_family_total_drift_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][0]["raw_evidence"]
        raw["disk"]["total_bytes"] += 1
        reseal_raw(self.report, 0, 0)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "total disk bytes"
        ):
            self.validate()

    def test_disk_smaller_than_chunk_rows_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][0]["raw_evidence"]
        raw["disk"] = {
            "main_bytes": 1,
            "wal_bytes": 0,
            "shm_bytes": 0,
            "journal_bytes": 0,
            "total_bytes": 1,
        }
        reseal_raw(self.report, 0, 0)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "smaller than"
        ):
            self.validate()

    def test_unknown_measurement_method_version_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][0]["raw_evidence"]
        raw["method"]["version"] = "2.0.0"
        reseal_raw(self.report, 0, 0)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "schema validation failed"
        ):
            self.validate()

    def test_v2_read_only_file_mutation_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][1]["raw_evidence"]
        raw["observations"]["after"]["main"]["sha256"] = digest("e")
        reseal_raw(self.report, 0, 1)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "changed its file-family"
        ):
            self.validate()

    def test_v2_directory_order_is_not_a_set_authority(self) -> None:
        raw = self.report["configurations"][0]["cases"][1]["raw_evidence"]
        for phase in ("before", "after"):
            raw["observations"][phase]["directory_entries"] = ["z", "a"]
        reseal_raw(self.report, 0, 1)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "canonical order"
        ):
            self.validate()

    def test_migration_semantic_drift_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][2]["raw_evidence"]
        raw["observations"]["target"]["semantic_projection_digest"] = digest("e")
        reseal_raw(self.report, 0, 2)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "semantic projection changed",
        ):
            self.validate()

    def test_migration_non_generation_drift_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][2]["raw_evidence"]
        raw["observations"]["target"][
            "non_generation_payload_projection_digest"
        ] = digest("e")
        reseal_raw(self.report, 0, 2)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "non-generation"
        ):
            self.validate()

    def test_migration_partial_generation_replacement_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][2]["raw_evidence"]
        for phase in ("target", "cold_reopen"):
            raw["observations"][phase]["generation_census"][1]["generation"] = 2
        reseal_raw(self.report, 0, 2)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "deterministic contiguous replacement",
        ):
            self.validate()

    def test_migration_generation_replacement_order_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][2]["raw_evidence"]
        for phase in ("target", "cold_reopen"):
            census = raw["observations"][phase]["generation_census"]
            census[1]["generation"], census[2]["generation"] = (
                census[2]["generation"],
                census[1]["generation"],
            )
        reseal_raw(self.report, 0, 2)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "deterministic contiguous replacement",
        ):
            self.validate()

    def test_migration_noncommitted_generation_change_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][2]["raw_evidence"]
        for phase in ("target", "cold_reopen"):
            raw["observations"][phase]["generation_census"][0]["generation"] = 10
        reseal_raw(self.report, 0, 2)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "noncommitted generation",
        ):
            self.validate()

    def test_migration_missing_committed_payload_version_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][2]["raw_evidence"]
        for phase in ("source", "target", "cold_reopen"):
            raw["observations"][phase]["committed_payload_schema_census"][4][
                "payload_schema"
            ] = "cxxlens.ng-snapshot-payload.v4"
        reseal_raw(self.report, 0, 2)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "not exact v1-v5"
        ):
            self.validate()

    def test_migration_rewritten_committed_payload_version_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][2]["raw_evidence"]
        for phase in ("target", "cold_reopen"):
            census = raw["observations"][phase][
                "committed_payload_schema_census"
            ]
            census[1]["payload_schema"], census[2]["payload_schema"] = (
                census[2]["payload_schema"],
                census[1]["payload_schema"],
            )
        reseal_raw(self.report, 0, 2)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "rewrote a committed payload schema version",
        ):
            self.validate()

    def test_migration_noncommitted_raw_payload_drift_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][2]["raw_evidence"]
        for phase in ("target", "cold_reopen"):
            projection = raw["observations"][phase]
            projection["noncommitted_census"][0]["raw_payload_digest"] = digest(
                "e"
            )
            projection["noncommitted_census_digest"] = qualification.document_digest(
                projection["noncommitted_census"]
            )
        reseal_raw(self.report, 0, 2)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "changed a noncommitted raw row",
        ):
            self.validate()

    def test_migration_noncommitted_stored_checksum_drift_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][2]["raw_evidence"]
        for phase in ("target", "cold_reopen"):
            projection = raw["observations"][phase]
            projection["noncommitted_census"][0]["payload_checksum"] = digest("e")
            projection["noncommitted_census_digest"] = qualification.document_digest(
                projection["noncommitted_census"]
            )
        reseal_raw(self.report, 0, 2)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "changed a noncommitted raw row",
        ):
            self.validate()

    def test_migration_noncommitted_typed_verdict_drift_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][2]["raw_evidence"]
        for phase in ("target", "cold_reopen"):
            projection = raw["observations"][phase]
            projection["noncommitted_census"][0][
                "diagnostic_classification"
            ] = "payload-decode-failure"
            projection["noncommitted_census"][0][
                "payload_checksum"
            ] = projection["noncommitted_census"][0]["raw_payload_digest"]
            projection["noncommitted_census"][0]["payload_schema"] = None
            projection["noncommitted_census_digest"] = qualification.document_digest(
                projection["noncommitted_census"]
            )
        reseal_raw(self.report, 0, 2)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "changed a noncommitted raw row",
        ):
            self.validate()

    def test_migration_noncommitted_checksum_verdict_is_rederived(self) -> None:
        raw = self.report["configurations"][0]["cases"][2]["raw_evidence"]
        for phase in ("source", "target", "cold_reopen"):
            projection = raw["observations"][phase]
            projection["noncommitted_census"][0][
                "payload_checksum"
            ] = projection["noncommitted_census"][0]["raw_payload_digest"]
            projection["noncommitted_census_digest"] = qualification.document_digest(
                projection["noncommitted_census"]
            )
        reseal_raw(self.report, 0, 2)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "checksum verdict is inconsistent",
        ):
            self.validate()

    def test_migration_requires_a_corrupt_noncommitted_diagnostic(self) -> None:
        raw = self.report["configurations"][0]["cases"][2]["raw_evidence"]
        for phase in ("source", "target", "cold_reopen"):
            projection = raw["observations"][phase]
            projection["noncommitted_census"][0][
                "diagnostic_classification"
            ] = "valid-noncommitted"
            projection["noncommitted_census"][0][
                "payload_checksum"
            ] = projection["noncommitted_census"][0]["raw_payload_digest"]
            projection["noncommitted_census_digest"] = qualification.document_digest(
                projection["noncommitted_census"]
            )
        reseal_raw(self.report, 0, 2)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "no corrupt noncommitted diagnostic",
        ):
            self.validate()

    def test_static_shared_payload_schema_census_is_bound(self) -> None:
        raw = self.report["configurations"][1]["cases"][2]["raw_evidence"]
        for phase in ("source", "target", "cold_reopen"):
            census = raw["observations"][phase][
                "committed_payload_schema_census"
            ]
            census[1]["payload_schema"], census[2]["payload_schema"] = (
                census[2]["payload_schema"],
                census[1]["payload_schema"],
            )
        raw["observations"].update(
            qualification._migration_payload_evidence(
                raw["observations"]["source"],
                raw["observations"]["target"],
                raw["observations"]["cold_reopen"],
                "shared schema fixture",
            )
        )
        reseal_case(self.report, 1, 2)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "static/shared semantic projection differs",
        ):
            self.validate()

    def test_static_shared_noncommitted_census_is_bound(self) -> None:
        raw = self.report["configurations"][1]["cases"][2]["raw_evidence"]
        for phase in ("source", "target", "cold_reopen"):
            projection = raw["observations"][phase]
            projection["noncommitted_census"][0]["raw_payload_digest"] = digest(
                "e"
            )
            projection["noncommitted_census_digest"] = qualification.document_digest(
                projection["noncommitted_census"]
            )
        raw["observations"].update(
            qualification._migration_payload_evidence(
                raw["observations"]["source"],
                raw["observations"]["target"],
                raw["observations"]["cold_reopen"],
                "shared diagnostic fixture",
            )
        )
        reseal_case(self.report, 1, 2)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "static/shared semantic projection differs",
        ):
            self.validate()

    def test_schema_valid_stable_wrong_ddl_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][0]["raw_evidence"]
        for phase in ("initial_open", "cold_reopen"):
            raw["observations"][phase]["canonical_ddl_digest"] = digest("e")
        reseal_case(self.report, 0, 0)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "exact six contract statements",
        ):
            self.validate()

    def test_limit_payload_must_exceed_actual_runtime_limit(self) -> None:
        raw = self.report["configurations"][0]["cases"][3]["raw_evidence"]
        raw["observations"]["sqlite_limit_length_bytes"] = 20_000_000
        reseal_raw(self.report, 0, 3)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "does not exceed"
        ):
            self.validate()

    def test_limit_chunk_census_must_cover_logical_payload(self) -> None:
        raw = self.report["configurations"][0]["cases"][3]["raw_evidence"]
        raw["observations"]["logical_payload_bytes"] += 1
        reseal_raw(self.report, 0, 3)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "does not cover"
        ):
            self.validate()

    def test_limit_parity_digest_drift_is_rejected(self) -> None:
        raw = self.report["configurations"][0]["cases"][3]["raw_evidence"]
        raw["observations"]["sqlite"]["query_projection_digest"] = digest("e")
        reseal_raw(self.report, 0, 3)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "parity projection differs"
        ):
            self.validate()

    def test_static_shared_semantic_drift_is_rejected(self) -> None:
        raw = self.report["configurations"][1]["cases"][0]["raw_evidence"]
        for phase in ("initial_open", "cold_reopen"):
            raw["observations"][phase]["semantic_projection_digest"] = digest("e")
        reseal_case(self.report, 1, 0)
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError,
            "static/shared semantic projection differs",
        ):
            self.validate()

    def test_unknown_report_field_is_rejected_by_schema(self) -> None:
        self.report["self_reported_green"] = True
        with self.assertRaisesRegex(
            qualification.SQLiteStoreV3QualificationError, "schema validation failed"
        ):
            self.validate()

    def test_strict_json_loader_rejects_duplicate_members(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "report.json"
            path.write_text('{"schema":"one","schema":"two"}', encoding="utf-8")
            with self.assertRaisesRegex(
                qualification.SQLiteStoreV3QualificationError,
                "duplicate JSON member",
            ):
                qualification.load_report(path)

    def test_strict_json_loader_rejects_floating_measurements(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "report.json"
            path.write_text(json.dumps({"elapsed": 1.5}), encoding="utf-8")
            with self.assertRaisesRegex(
                qualification.SQLiteStoreV3QualificationError,
                "floating-point JSON value is forbidden",
            ):
                qualification.load_report(path)


if __name__ == "__main__":
    unittest.main()
