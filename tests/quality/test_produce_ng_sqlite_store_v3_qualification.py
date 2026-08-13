#!/usr/bin/env python3
"""Fail-closed tests for the SQLite Store v3 qualification producer."""

from __future__ import annotations

import os
import pathlib
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from types import SimpleNamespace
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

import produce_ng_sqlite_store_v3_qualification as producer  # noqa: E402


REVISION = "1" * 40
SOURCE_TREE = "2" * 40


def _unsigned(value: int) -> bytes:
    return value.to_bytes(8, byteorder="big", signed=False)


def _string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return _unsigned(len(encoded)) + encoded


def _payload(
    version: int,
    generation: int,
    *,
    publication_id: str = "publication",
    suffix: bytes = b"",
) -> bytes:
    return b"".join(
        (
            _string(f"cxxlens.ng-snapshot-payload.v{version}"),
            _string("cxxlens.ng-snapshot.v1"),
            _string("snapshot"),
            _unsigned(1),
            _unsigned(0),
            _unsigned(0),
            _string("sha256:" + "1" * 64),
            _string("universe"),
            _string("sha256:" + "2" * 64),
            _string("sha256:" + "3" * 64),
            _unsigned(0),
            _unsigned(0),
            _string(publication_id),
            _string("series"),
            _string("snapshot"),
            _unsigned(1),
            _unsigned(generation),
            suffix,
        )
    )


def _contract_statements(profile: str) -> list[str]:
    return producer.load_yaml(ROOT / producer.SQLITE_CONTRACT)["schema_profiles"][
        profile
    ]["canonical_ddl"]["statements"]


def _create_v2_database(
    path: pathlib.Path, publications: list[dict[str, object]] | None = None
) -> None:
    database = sqlite3.connect(path)
    try:
        for statement in _contract_statements("predecessor_v2"):
            database.execute(statement)
        database.execute(
            "INSERT INTO cxxlens_ng_metadata(key,value) VALUES(?,?)",
            ("physical_format", "cxxlens.sqlite-semantic-store.v2"),
        )
        for row in publications or []:
            database.execute(
                "INSERT INTO cxxlens_ng_publication("
                "publication_id,series_id,snapshot_id,sequence,generation,parent,"
                "state,checksum,payload) VALUES(?,?,?,?,?,?,?,?,?)",
                (
                    row["publication_id"],
                    row.get("series_id", "series"),
                    row.get("snapshot_id", "snapshot"),
                    row.get("sequence", 1),
                    row["generation"],
                    row.get("parent"),
                    row["state"],
                    row["checksum"],
                    sqlite3.Binary(row["payload"]),
                ),
            )
        database.commit()
    finally:
        database.close()


def _create_v3_database(
    path: pathlib.Path, publications: list[dict[str, object]]
) -> None:
    database = sqlite3.connect(path)
    try:
        for statement in _contract_statements("current_v3"):
            database.execute(statement)
        database.executemany(
            "INSERT INTO cxxlens_ng_metadata(key,value) VALUES(?,?)",
            (
                ("payload_chunk_maximum_bytes", "8388608"),
                ("payload_chunk_profile", "cxxlens.sqlite-payload-chunks.v1"),
                ("physical_format", "cxxlens.sqlite-semantic-store.v3"),
                ("physical_format_version", "3.0.0"),
            ),
        )
        for row in publications:
            payload = row["payload"]
            assert isinstance(payload, bytes)
            chunk_count = (
                0
                if not payload
                else 1 + (len(payload) - 1) // producer.CHUNK_MAXIMUM_BYTES
            )
            database.execute(
                "INSERT INTO cxxlens_ng_publication("
                "publication_id,series_id,snapshot_id,sequence,generation,parent,"
                "state,payload_checksum,payload_byte_count,payload_chunk_count) "
                "VALUES(?,?,?,?,?,?,?,?,?,?)",
                (
                    row["publication_id"],
                    row.get("series_id", "series"),
                    row.get("snapshot_id", "snapshot"),
                    row.get("sequence", 1),
                    row["generation"],
                    row.get("parent"),
                    row["state"],
                    row["checksum"],
                    len(payload),
                    chunk_count,
                ),
            )
            offset = 0
            for ordinal in range(chunk_count):
                chunk = payload[
                    offset : offset + producer.CHUNK_MAXIMUM_BYTES
                ]
                database.execute(
                    "INSERT INTO cxxlens_ng_payload_chunk("
                    "publication_id,generation,chunk_ordinal,byte_offset,byte_count,"
                    "checksum,payload) VALUES(?,?,?,?,?,?,?)",
                    (
                        row["publication_id"],
                        row["generation"],
                        ordinal,
                        offset,
                        len(chunk),
                        producer.bytes_digest(chunk),
                        sqlite3.Binary(chunk),
                    ),
                )
                offset += len(chunk)
        database.commit()
    finally:
        database.close()


def _write_cross_operation_runner(
    path: pathlib.Path, mutation: str
) -> None:
    statements = repr(_contract_statements("predecessor_v2"))
    script = textwrap.dedent(
        f"""\
        #!/usr/bin/env python3
        import argparse
        import os
        import pathlib
        import shutil
        import sqlite3

        parser = argparse.ArgumentParser()
        parser.add_argument("--configuration")
        parser.add_argument("--case")
        parser.add_argument("--work-directory", type=pathlib.Path)
        parser.add_argument("--output")
        parser.add_argument("--event-fd", type=int)
        parser.add_argument("--control-fd", type=int)
        options = parser.parse_args()

        database_directory = options.work_directory / "database"
        database_directory.mkdir()
        database_path = database_directory / "database.sqlite"
        database = sqlite3.connect(database_path)
        for statement in {statements}:
            database.execute(statement)
        database.execute(
            "INSERT INTO cxxlens_ng_metadata(key,value) VALUES(?,?)",
            ("physical_format", "cxxlens.sqlite-semantic-store.v2"),
        )
        database.commit()
        database.close()

        def checkpoint(name):
            os.write(options.event_fd, (name + "\\n").encode("ascii"))
            if os.read(options.control_fd, 1) != b"c":
                raise RuntimeError("checkpoint acknowledgement failed")

        checkpoint("v2-before")
        {textwrap.indent(mutation, '        ').lstrip()}
        checkpoint("v2-after")
        raise RuntimeError("parent accepted a transient database mutation")
        """
    )
    path.write_text(script, encoding="utf-8")
    os.chmod(path, 0o755)


class SQLiteStoreV3QualificationProducerTests(unittest.TestCase):
    def test_stable_wrong_current_v3_ddl_is_rejected(self) -> None:
        expected, declared_digest = producer.current_v3_ddl_authority(ROOT)
        statements = producer.load_yaml(ROOT / producer.SQLITE_CONTRACT)[
            "schema_profiles"
        ]["current_v3"]["canonical_ddl"]["statements"]
        self.assertEqual(producer.document_digest(statements), declared_digest)
        wrong = [dict(row) for row in expected]
        wrong[0]["sql"] += " /* stable drift */"
        with self.assertRaisesRegex(
            producer.QualificationError, "exact six contract statements"
        ):
            producer.validate_current_v3_ddl(wrong, "fixture")

    def test_full_production_rejects_stale_or_dirty_source_checkout(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-sqlite-producer-checkout-"
        ) as raw:
            checkout = pathlib.Path(raw) / "checkout"
            checkout.mkdir()
            subprocess.run(
                ["git", "init", "--quiet", str(checkout)], check=True
            )
            tracked = checkout / "tracked.txt"
            tracked.write_text("authority\n", encoding="utf-8")
            subprocess.run(
                ["git", "-C", str(checkout), "add", "tracked.txt"], check=True
            )
            subprocess.run(
                [
                    "git",
                    "-C",
                    str(checkout),
                    "-c",
                    "user.name=cxxlens qualification",
                    "-c",
                    "user.email=qualification@example.invalid",
                    "commit",
                    "--quiet",
                    "-m",
                    "fixture",
                ],
                check=True,
            )
            revision = subprocess.run(
                ["git", "-C", str(checkout), "rev-parse", "HEAD"],
                check=True,
                capture_output=True,
                text=True,
            ).stdout.strip()
            source_tree = subprocess.run(
                ["git", "-C", str(checkout), "rev-parse", "HEAD^{tree}"],
                check=True,
                capture_output=True,
                text=True,
            ).stdout.strip()

            arguments = SimpleNamespace(
                root=checkout,
                revision="3" * 40,
                source_tree=source_tree,
                static_runner=checkout / "missing-static-runner",
                shared_runner=checkout / "missing-shared-runner",
                work_directory=pathlib.Path(raw) / "work-stale",
            )
            with self.assertRaisesRegex(
                producer.QualificationError, "HEAD differs from --revision"
            ):
                producer.produce(arguments)
            self.assertFalse(arguments.work_directory.exists())

            arguments.revision = revision
            arguments.work_directory = pathlib.Path(raw) / "work-dirty"
            (checkout / "untracked.txt").write_text("dirty\n", encoding="utf-8")
            with self.assertRaisesRegex(
                producer.QualificationError, "source checkout is not clean"
            ):
                producer.produce(arguments)
            self.assertFalse(arguments.work_directory.exists())

    def test_payload_normalization_omits_only_declared_generation(self) -> None:
        def unsigned(value: int) -> bytes:
            return value.to_bytes(8, byteorder="big", signed=False)

        def string(value: str) -> bytes:
            encoded = value.encode("utf-8")
            return unsigned(len(encoded)) + encoded

        def payload(magic: str, generation: int, suffix: bytes) -> bytes:
            return b"".join(
                (
                    string(magic),
                    string("cxxlens.ng-snapshot.v1"),
                    string("snapshot"),
                    unsigned(1),
                    unsigned(0),
                    unsigned(0),
                    string("sha256:" + "1" * 64),
                    string("universe"),
                    string("sha256:" + "2" * 64),
                    string("sha256:" + "3" * 64),
                    unsigned(0),
                    unsigned(0),
                    string("publication"),
                    string("series"),
                    string("snapshot"),
                    unsigned(1),
                    unsigned(generation),
                    suffix,
                )
            )

        for version in range(1, 6):
            with self.subTest(version=version):
                first = payload(
                    f"cxxlens.ng-snapshot-payload.v{version}", 7, b"suffix"
                )
                second = payload(
                    f"cxxlens.ng-snapshot-payload.v{version}", 11, b"suffix"
                )
                changed = payload(
                    f"cxxlens.ng-snapshot-payload.v{version}", 11, b"changed"
                )
                first_digest = producer.normalized_payload_digest(
                    first, 7, "first"
                )
                self.assertEqual(
                    first_digest,
                    producer.normalized_payload_digest(second, 11, "second"),
                )
                self.assertNotEqual(
                    first_digest,
                    producer.normalized_payload_digest(changed, 11, "changed"),
                )
                with self.assertRaisesRegex(
                    producer.QualificationError, "embedded physical generation differs"
                ):
                    producer.normalized_payload_digest(first, 8, "mismatch")

    def test_v2_payload_larger_than_one_chunk_is_streamed_and_classified(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-sqlite-v2-large-payload-"
        ) as raw:
            path = pathlib.Path(raw) / "database.sqlite"
            payload = _payload(
                5,
                7,
                suffix=b"x" * (producer.CHUNK_MAXIMUM_BYTES + 257),
            )
            _create_v2_database(
                path,
                [
                    {
                        "publication_id": "publication:large",
                        "generation": 7,
                        "state": producer.COMMITTED_STATE,
                        "checksum": producer.bytes_digest(payload),
                        "payload": payload,
                    }
                ],
            )

            with mock.patch.object(
                producer, "_sqlite_rows", wraps=producer._sqlite_rows
            ) as bounded_rows:
                authority = producer.scan_sqlite_authority(path, "large-v2")

            self.assertTrue(
                all(
                    "cxxlens_ng_publication" not in call.args[1]
                    for call in bounded_rows.call_args_list
                )
            )
            publication = authority["publications"][0]
            self.assertEqual(publication["raw_payload_digest"], producer.bytes_digest(payload))
            self.assertEqual(
                publication["diagnostic_classification"], "committed-valid"
            )
            self.assertEqual(
                publication["payload_schema"], "cxxlens.ng-snapshot-payload.v5"
            )

    def test_v2_publication_cardinality_is_bounded(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-sqlite-v2-cardinality-"
        ) as raw:
            path = pathlib.Path(raw) / "database.sqlite"
            payload = _payload(1, 1)
            publications = [
                {
                    "publication_id": f"publication:{index}",
                    "sequence": index,
                    "generation": 1,
                    "state": 4,
                    "checksum": producer.bytes_digest(payload),
                    "payload": payload,
                }
                for index in range(3)
            ]
            _create_v2_database(path, publications)
            with mock.patch.object(producer, "OBSERVATION_MAXIMUM_ROWS", 2):
                with self.assertRaisesRegex(
                    producer.QualificationError, "cardinality exceeds"
                ):
                    producer.scan_sqlite_authority(path, "many-v2")

    def test_v2_oversized_text_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-sqlite-v2-text-bound-"
        ) as raw:
            path = pathlib.Path(raw) / "database.sqlite"
            payload = _payload(1, 1)
            _create_v2_database(
                path,
                [
                    {
                        "publication_id": "p" * 1025,
                        "generation": 1,
                        "state": 4,
                        "checksum": producer.bytes_digest(payload),
                        "payload": payload,
                    }
                ],
            )
            with mock.patch.object(
                producer, "OBSERVATION_MAXIMUM_TEXT_BYTES", 1024
            ):
                with self.assertRaisesRegex(
                    producer.QualificationError, "TEXT exceeds"
                ):
                    producer.scan_sqlite_authority(path, "oversized-v2-text")

    def test_independent_scan_deadline_is_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-sqlite-v2-deadline-"
        ) as raw:
            path = pathlib.Path(raw) / "database.sqlite"
            _create_v2_database(path)
            with self.assertRaisesRegex(
                producer.QualificationError, "bounded deadline"
            ):
                producer.scan_sqlite_authority(
                    path,
                    "expired-v2",
                    deadline_ns=time.monotonic_ns() - 1,
                )

    def test_noncommitted_raw_checksum_verdict_survives_v2_v3_projection(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-sqlite-diagnostic-projection-"
        ) as raw:
            directory = pathlib.Path(raw)
            v2_path = directory / "source.sqlite"
            v3_path = directory / "target.sqlite"
            checksum_payload = _payload(
                1, 9, publication_id="publication:checksum"
            )
            decode_payload = b"not-a-canonical-snapshot-payload"
            rows = [
                {
                    "publication_id": "publication:checksum",
                    "series_id": "series:checksum",
                    "snapshot_id": "snapshot:checksum",
                    "sequence": 0,
                    "generation": 9,
                    "parent": None,
                    "state": 4,
                    "checksum": "sha256:" + "0" * 64,
                    "payload": checksum_payload,
                },
                {
                    "publication_id": "publication:decode",
                    "series_id": "series:decode",
                    "snapshot_id": "snapshot:decode",
                    "sequence": 1,
                    "generation": 10,
                    "parent": None,
                    "state": 5,
                    "checksum": producer.bytes_digest(decode_payload),
                    "payload": decode_payload,
                },
            ]
            _create_v2_database(v2_path, rows)
            _create_v3_database(v3_path, rows)

            source = producer.scan_sqlite_authority(v2_path, "diagnostic-v2")
            target = producer.scan_sqlite_authority(v3_path, "diagnostic-v3")
            self.assertEqual(
                source["noncommitted_census"], target["noncommitted_census"]
            )
            classifications = {
                row["publication_id"]: row["diagnostic_classification"]
                for row in source["noncommitted_census"]
            }
            self.assertEqual(
                classifications,
                {
                    "publication:checksum": "stored-full-checksum-mismatch",
                    "publication:decode": "payload-decode-failure",
                },
            )

    def test_same_byte_main_replacement_during_scan_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-sqlite-same-byte-replacement-"
        ) as raw:
            path = pathlib.Path(raw) / "database.sqlite"
            _create_v2_database(path)

            def replace(_phase: str, database_path: pathlib.Path) -> None:
                replacement = database_path.with_name("replacement.sqlite")
                shutil.copy2(database_path, replacement)
                os.replace(replacement, database_path)

            with self.assertRaisesRegex(
                producer.QualificationError,
                "replaced|namespace changed|changed while its held bytes were hashed",
            ):
                producer.observe_database(
                    path, "same-byte-replacement", observation_hook=replace
                )

    def test_main_identity_aba_during_scan_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-sqlite-aba-") as raw:
            directory = pathlib.Path(raw)
            path = directory / "database.sqlite"
            replacement = directory / "replacement.sqlite"
            _create_v2_database(path)
            shutil.copy2(path, replacement)

            def swap_aba(_phase: str, database_path: pathlib.Path) -> None:
                held_name = database_path.with_name("held.sqlite")
                os.replace(database_path, held_name)
                os.replace(replacement, database_path)
                os.replace(database_path, replacement)
                os.replace(held_name, database_path)

            with self.assertRaisesRegex(
                producer.QualificationError,
                "namespace changed|held object changed|changed while its held bytes were hashed",
            ):
                producer.observe_database(path, "identity-aba", observation_hook=swap_aba)

    def test_symlink_replacement_during_scan_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-sqlite-symlink-race-"
        ) as raw:
            path = pathlib.Path(raw) / "database.sqlite"
            _create_v2_database(path)

            def replace_with_symlink(
                _phase: str, database_path: pathlib.Path
            ) -> None:
                saved = database_path.with_name("saved.sqlite")
                os.replace(database_path, saved)
                os.symlink(saved.name, database_path)

            with self.assertRaisesRegex(
                producer.QualificationError,
                "replaced|namespace changed|changed while its held bytes were hashed",
            ):
                producer.observe_database(
                    path, "symlink-race", observation_hook=replace_with_symlink
                )

    def test_present_sidecar_prevents_main_only_private_scan(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-sqlite-sidecar-presence-"
        ) as raw:
            path = pathlib.Path(raw) / "database.sqlite"
            _create_v2_database(path)
            path.with_name(path.name + "-wal").write_bytes(b"not-quiescent")
            with self.assertRaisesRegex(
                producer.QualificationError, "not quiescent"
            ):
                producer.observe_database(path, "sidecar-present")

    def test_run_case_rejects_transient_wal_between_v2_checkpoints(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-sqlite-cross-operation-wal-"
        ) as raw:
            directory = pathlib.Path(raw)
            runner = directory / "transient-wal-runner.py"
            _write_cross_operation_runner(
                runner,
                textwrap.dedent(
                    """\
                    wal_path = database_path.with_name(database_path.name + "-wal")
                    wal_path.write_bytes(b"transient-wal")
                    wal_path.unlink()
                    """
                ),
            )
            with self.assertRaisesRegex(
                producer.QualificationError,
                "database namespace changed during held observation",
            ):
                producer.run_case(
                    runner,
                    "static",
                    producer.CASE_IDS[1],
                    directory / "case",
                    REVISION,
                    SOURCE_TREE,
                )

    def test_run_case_rejects_main_aba_between_v2_checkpoints(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="cxxlens-sqlite-cross-operation-aba-"
        ) as raw:
            directory = pathlib.Path(raw)
            runner = directory / "main-aba-runner.py"
            _write_cross_operation_runner(
                runner,
                textwrap.dedent(
                    """\
                    replacement = database_path.with_name("replacement.sqlite")
                    held = database_path.with_name("held.sqlite")
                    shutil.copy2(database_path, replacement)
                    os.replace(database_path, held)
                    os.replace(replacement, database_path)
                    os.replace(database_path, replacement)
                    os.replace(held, database_path)
                    replacement.unlink()
                    """
                ),
            )
            with self.assertRaisesRegex(
                producer.QualificationError,
                "database namespace changed|changed while its held bytes were hashed",
            ):
                producer.run_case(
                    runner,
                    "static",
                    producer.CASE_IDS[1],
                    directory / "case",
                    REVISION,
                    SOURCE_TREE,
                )

    def test_compiled_provenance_mismatch_is_rejected(self) -> None:
        child = {
            "compiled_provenance": {
                "configuration": "static",
                "revision": "3" * 40,
                "source_tree": SOURCE_TREE,
            }
        }
        with self.assertRaisesRegex(
            producer.QualificationError, "compiled provenance differs"
        ):
            producer.validate_compiled_provenance(
                child,
                "static",
                REVISION,
                SOURCE_TREE,
                "fixture/current",
            )

    def test_silent_child_is_terminated_at_bounded_deadline(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cxxlens-sqlite-producer-timeout-") as raw:
            directory = pathlib.Path(raw)
            runner = directory / "silent-runner.py"
            runner.write_text(
                "#!/usr/bin/env python3\nimport time\ntime.sleep(30)\n",
                encoding="utf-8",
            )
            os.chmod(runner, 0o755)
            started = time.monotonic()
            with mock.patch.object(producer, "CASE_TIMEOUT_SECONDS", 0.05):
                with self.assertRaisesRegex(
                    producer.QualificationError, "qualification deadline"
                ):
                    producer.run_case(
                        runner,
                        "static",
                        producer.CASE_IDS[0],
                        directory / "case",
                        REVISION,
                        SOURCE_TREE,
                    )
            self.assertLess(time.monotonic() - started, 5.0)


if __name__ == "__main__":
    unittest.main()
