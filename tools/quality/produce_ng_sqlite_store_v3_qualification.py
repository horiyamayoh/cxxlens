#!/usr/bin/env python3
"""Produce the Linux parent-observed SQLite Store v3 2x4 qualification report."""

from __future__ import annotations

import argparse
import ctypes
import errno
import hashlib
import json
import os
import pathlib
import re
import select
import signal
import sqlite3
import stat
import subprocess
import sys
import tempfile
import time
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
OBSERVATION_COPY_BLOCK_BYTES = 256 * 1024
# The qualification observer is intentionally narrower than the Store's public
# semantic domain.  These bounds come from the accepted payload/chunk decoder
# profile and keep a corrupt fixture from turning the independent validator into
# an unbounded allocation or row walk.
OBSERVATION_MAXIMUM_TEXT_BYTES = CHUNK_MAXIMUM_BYTES
OBSERVATION_MAXIMUM_ROWS = 1_000_000
OBSERVATION_MAXIMUM_CHUNK_ROWS = 1_000_000
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
BACKEND_SCOPES = ("sqlite", "sqlite", "sqlite", "memory-sqlite-parity")
CHECKPOINTS = {
    CASE_IDS[0]: ("current-initial", "current-cold"),
    CASE_IDS[1]: ("v2-before", "v2-after"),
    CASE_IDS[2]: ("migration-source", "migration-target", "migration-cold"),
    CASE_IDS[3]: ("limit-sqlite", "limit-cold"),
}
ROLE_SUFFIXES = {
    "main": "",
    "wal": "-wal",
    "shm": "-shm",
    "journal": "-journal",
}
# A held descriptor proves the final object identity, while an inotify mutation
# watch closes the otherwise-unobservable A->B->A namespace gap.  Read/open/
# close-read events are deliberately excluded because the observer itself
# performs those operations.
INOTIFY_MUTATION_MASK = (
    0x00000002  # IN_MODIFY
    | 0x00000004  # IN_ATTRIB
    | 0x00000008  # IN_CLOSE_WRITE
    | 0x00000040  # IN_MOVED_FROM
    | 0x00000080  # IN_MOVED_TO
    | 0x00000100  # IN_CREATE
    | 0x00000200  # IN_DELETE
    | 0x00000400  # IN_DELETE_SELF
    | 0x00000800  # IN_MOVE_SELF
    | 0x00002000  # IN_UNMOUNT
    | 0x01000000  # IN_ONLYDIR
)
DIGEST_PATTERN = re.compile(r"^sha256:[0-9a-f]{64}$")
REVISION_PATTERN = re.compile(r"^[0-9a-f]{40}$")
CASE_TIMEOUT_SECONDS = 600
TERMINATION_GRACE_SECONDS = 2
PAYLOAD_MAGICS = frozenset(
    f"cxxlens.ng-snapshot-payload.v{version}" for version in range(1, 6)
)
PAYLOAD_SCHEMA_ORDER = tuple(
    f"cxxlens.ng-snapshot-payload.v{version}" for version in range(1, 6)
)
NORMALIZED_PAYLOAD_DOMAIN = b"cxxlens.sqlite-qualification.normalized-payload.v1"


class QualificationError(ValueError):
    """Qualification production could not establish an exact invariant."""


class ProductionReceiptUnavailable(QualificationError):
    """A required observation is not bound to the actual production operation."""


def fail(message: str) -> None:
    raise QualificationError(message)


def _observation_deadline(deadline_ns: int | None, label: str) -> int:
    if deadline_ns is None:
        deadline_ns = time.monotonic_ns() + CASE_TIMEOUT_SECONDS * 1_000_000_000
    if time.monotonic_ns() >= deadline_ns:
        fail(f"{label} independent observation exceeded its bounded deadline")
    return deadline_ns


def _check_observation_deadline(deadline_ns: int, label: str) -> None:
    if time.monotonic_ns() >= deadline_ns:
        fail(f"{label} independent observation exceeded its bounded deadline")


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")


def document_digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value)).hexdigest()


def bytes_digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _git_output(root: pathlib.Path, arguments: tuple[str, ...], label: str) -> bytes:
    try:
        completed = subprocess.run(
            ["git", "-C", str(root), *arguments],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "LC_ALL": "C"},
        )
    except OSError as error:
        fail(f"cannot observe source checkout {label}: {error}")
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        fail(
            f"cannot observe source checkout {label}: git exited "
            f"{completed.returncode}{': ' + detail if detail else ''}"
        )
    return completed.stdout


def _single_git_revision(output: bytes, label: str) -> str:
    lines = output.splitlines()
    if len(lines) != 1:
        fail(f"source checkout {label} did not produce one exact revision")
    try:
        value = lines[0].decode("ascii", errors="strict")
    except UnicodeDecodeError:
        fail(f"source checkout {label} is not ASCII")
    if REVISION_PATTERN.fullmatch(value) is None:
        fail(f"source checkout {label} is not an exact lowercase Git object ID")
    return value


def validate_exact_clean_checkout(
    root: pathlib.Path, revision: str, source_tree: str
) -> None:
    """Bind full report production to one exact clean Git checkout."""

    top_level_output = _git_output(root, ("rev-parse", "--show-toplevel"), "root")
    try:
        top_level = pathlib.Path(
            top_level_output.decode("utf-8", errors="strict").strip()
        ).resolve(strict=True)
    except (OSError, UnicodeDecodeError):
        fail("source checkout root is not an exact accessible path")
    if top_level != root:
        fail("--root is not the source checkout top level")

    observed_revision = _single_git_revision(
        _git_output(root, ("rev-parse", "--verify", "HEAD"), "HEAD"), "HEAD"
    )
    observed_tree = _single_git_revision(
        _git_output(root, ("rev-parse", "--verify", "HEAD^{tree}"), "HEAD tree"),
        "HEAD tree",
    )
    if observed_revision != revision:
        fail(
            "source checkout HEAD differs from --revision: "
            f"expected={revision}, actual={observed_revision}"
        )
    if observed_tree != source_tree:
        fail(
            "source checkout HEAD tree differs from --source-tree: "
            f"expected={source_tree}, actual={observed_tree}"
        )
    status = _git_output(
        root,
        ("status", "--porcelain=v1", "--untracked-files=all"),
        "worktree status",
    )
    if status:
        fail("source checkout is not clean, including untracked files")


def load_yaml(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, yaml.YAMLError) as error:
        fail(f"cannot load YAML document {path}: {error}")
    if not isinstance(value, dict):
        fail(f"expected YAML object: {path}")
    return value


def current_v3_ddl_authority(root: pathlib.Path = ROOT) -> tuple[list[dict[str, str]], str]:
    """Derive the exact sqlite_schema projection from the accepted Store contract."""

    contract = load_yaml(root / SQLITE_CONTRACT)
    try:
        canonical = contract["schema_profiles"]["current_v3"]["canonical_ddl"]
        statements = canonical["statements"]
        declared_digest = canonical["digest"]
    except (KeyError, TypeError):
        fail("SQLite contract lacks the current-v3 canonical DDL authority")
    if (
        not isinstance(statements, list)
        or len(statements) != 6
        or any(not isinstance(statement, str) for statement in statements)
        or document_digest(statements) != declared_digest
    ):
        fail("SQLite contract canonical DDL statements/digest differ")

    expected: list[dict[str, str]] = []
    for statement in statements:
        table = re.fullmatch(r"CREATE TABLE ([A-Za-z0-9_]+).*", statement)
        index = re.fullmatch(
            r"CREATE INDEX ([A-Za-z0-9_]+) ON ([A-Za-z0-9_]+)\(.*", statement
        )
        if table is not None:
            name = table.group(1)
            expected.append(
                {"type": "table", "name": name, "table": name, "sql": statement}
            )
        elif index is not None:
            expected.append(
                {
                    "type": "index",
                    "name": index.group(1),
                    "table": index.group(2),
                    "sql": statement,
                }
            )
        else:
            fail("SQLite contract canonical DDL statement is not an exact table/index")
    expected.sort(key=lambda row: (row["type"], row["name"]))
    return expected, declared_digest


def validate_current_v3_ddl(ddl: list[dict[str, str]], label: str) -> str:
    expected, declared_digest = current_v3_ddl_authority()
    if ddl != expected:
        fail(f"{label} current-v3 DDL differs from the exact six contract statements")
    return declared_digest


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


def load_strict_json(path: pathlib.Path, label: str) -> dict[str, Any]:
    try:
        raw = path.read_bytes()
    except OSError as error:
        fail(f"cannot read {label} {path}: {error}")
    if raw.startswith(b"\xef\xbb\xbf"):
        fail(f"{label} must not contain a UTF-8 BOM")
    try:
        value = json.loads(
            raw.decode("utf-8", errors="strict"),
            object_pairs_hook=_reject_duplicate_members,
            parse_float=_reject_float,
            parse_constant=_reject_constant,
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        fail(f"{label} is not strict JSON: {error}")
    if not isinstance(value, dict):
        fail(f"{label} root must be an object")
    return value


def require_object(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        fail(f"{label} must be an object")
    return value


def require_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        fail(f"{label} must be a nonempty string")
    return value


def require_bool(value: Any, label: str) -> bool:
    if type(value) is not bool:
        fail(f"{label} must be a boolean")
    return value


def require_uint64(value: Any, label: str) -> int:
    if type(value) is not int or value < 0 or value > UINT64_MAX:
        fail(f"{label} must be a uint64")
    return value


def require_digest(value: Any, label: str) -> str:
    text = require_string(value, label)
    if DIGEST_PATTERN.fullmatch(text) is None:
        fail(f"{label} must be a canonical SHA-256 digest")
    return text


def checked_sum(values: list[int], label: str) -> int:
    total = sum(values)
    if total > UINT64_MAX:
        fail(f"{label} exceeds the uint64 evidence domain")
    return total


def _stat_identity(value: os.stat_result) -> tuple[int, ...]:
    return (
        value.st_dev,
        value.st_ino,
        stat.S_IFMT(value.st_mode),
        value.st_nlink,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
    )


def _directory_entries(
    directory_fd: int, label: str, deadline_ns: int
) -> list[str]:
    entries: list[str] = []
    try:
        iterator = os.scandir(directory_fd)
    except OSError as error:
        fail(f"{label} directory census failed: {error}")
    try:
        with iterator:
            for entry in iterator:
                _check_observation_deadline(deadline_ns, label)
                if len(entries) >= OBSERVATION_MAXIMUM_ROWS:
                    fail(f"{label} directory entry cardinality exceeds its bound")
                name = entry.name
                if not isinstance(name, str):
                    fail(f"{label} contains a non-text directory entry")
                try:
                    name.encode("utf-8", errors="strict")
                except UnicodeError as error:
                    fail(f"{label} contains a non-UTF-8 filename: {error}")
                if not name or len(name) > 4096:
                    fail(f"{label} contains an invalid directory entry")
                entries.append(name)
    except OSError as error:
        fail(f"{label} directory census failed: {error}")
    return sorted(entries)


def _write_all(descriptor: int, value: bytes, label: str) -> None:
    offset = 0
    while offset < len(value):
        try:
            written = os.write(descriptor, value[offset:])
        except InterruptedError:
            continue
        except OSError as error:
            fail(f"{label} write failed: {error}")
        if written <= 0:
            fail(f"{label} write made no progress")
        offset += written


class _HeldFileFamily:
    """Pin one quiescent namespace across endpoint scans and guarded operations."""

    def __init__(self, database_path: pathlib.Path, label: str, deadline_ns: int):
        parent = database_path.parent
        base = database_path.name
        if not base or database_path != parent / base:
            fail(f"{label} database locator is not a direct child path")
        self.label = label
        self.database_path = database_path
        self.base = base
        self.deadline_ns = deadline_ns
        self.directory_fd = -1
        self.namespace_watch_fd = -1
        self.directory_stat: os.stat_result | None = None
        self.entries: list[str] = []
        self.held: dict[str, tuple[str, int, os.stat_result]] = {}
        self.absent: dict[str, str] = {}

        directory_flags = os.O_RDONLY | os.O_CLOEXEC
        directory_flags |= getattr(os, "O_DIRECTORY", 0)
        directory_flags |= getattr(os, "O_NOFOLLOW", 0)
        try:
            self.directory_fd = os.open(parent, directory_flags)
            self.directory_stat = os.fstat(self.directory_fd)
            if not stat.S_ISDIR(self.directory_stat.st_mode):
                fail(f"{label} database parent is not a directory")
            self._open_namespace_watch()
            self.entries = _directory_entries(
                self.directory_fd, label, self.deadline_ns
            )
            open_flags = os.O_RDONLY | os.O_CLOEXEC | getattr(os, "O_NONBLOCK", 0)
            open_flags |= getattr(os, "O_NOFOLLOW", 0)
            for role, suffix in ROLE_SUFFIXES.items():
                _check_observation_deadline(deadline_ns, label)
                name = base + suffix
                try:
                    descriptor = os.open(name, open_flags, dir_fd=self.directory_fd)
                except FileNotFoundError:
                    self.absent[role] = name
                    continue
                except OSError as error:
                    fail(f"{label} {role} held no-follow open failed: {error}")
                opened_stat = os.fstat(descriptor)
                if not stat.S_ISREG(opened_stat.st_mode):
                    os.close(descriptor)
                    fail(f"{label} {role} is not a regular file")
                self.held[role] = (name, descriptor, opened_stat)
            if "main" not in self.held:
                fail(f"{label} main database is absent")
            self._revalidate_namespace("initial hold")
        except BaseException:
            self.close()
            raise

    def close(self) -> None:
        for _name, descriptor, _opened_stat in self.held.values():
            os.close(descriptor)
        self.held.clear()
        if self.namespace_watch_fd >= 0:
            os.close(self.namespace_watch_fd)
            self.namespace_watch_fd = -1
        if self.directory_fd >= 0:
            os.close(self.directory_fd)
            self.directory_fd = -1

    def __enter__(self) -> _HeldFileFamily:
        return self

    def __exit__(self, _type: Any, _value: Any, _traceback: Any) -> None:
        self.close()

    def _open_namespace_watch(self) -> None:
        libc = ctypes.CDLL(None, use_errno=True)
        try:
            inotify_init1 = libc.inotify_init1
            inotify_add_watch = libc.inotify_add_watch
        except AttributeError:
            fail(f"{self.label} cannot establish the required Linux namespace watch")
        inotify_init1.argtypes = [ctypes.c_int]
        inotify_init1.restype = ctypes.c_int
        inotify_add_watch.argtypes = [
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_uint32,
        ]
        inotify_add_watch.restype = ctypes.c_int
        descriptor = inotify_init1(os.O_CLOEXEC | os.O_NONBLOCK)
        if descriptor < 0:
            error = OSError(ctypes.get_errno(), os.strerror(ctypes.get_errno()))
            fail(f"{self.label} namespace watch create failed: {error}")
        watch_path = f"/proc/self/fd/{self.directory_fd}".encode("ascii")
        watch_descriptor = inotify_add_watch(
            descriptor, watch_path, INOTIFY_MUTATION_MASK
        )
        if watch_descriptor < 0:
            error_number = ctypes.get_errno()
            os.close(descriptor)
            error = OSError(error_number, os.strerror(error_number))
            fail(f"{self.label} namespace watch bind failed: {error}")
        self.namespace_watch_fd = descriptor

    def _reject_namespace_mutation_events(self, phase: str) -> None:
        if self.namespace_watch_fd < 0:
            fail(f"{self.label} namespace watch is unavailable during {phase}")
        while True:
            try:
                events = os.read(self.namespace_watch_fd, 64 * 1024)
            except BlockingIOError:
                return
            except InterruptedError:
                continue
            except OSError as error:
                fail(f"{self.label} namespace watch read failed during {phase}: {error}")
            if not events:
                fail(f"{self.label} namespace watch closed during {phase}")
            fail(f"{self.label} database namespace changed during {phase}")

    def _revalidate_namespace(self, phase: str) -> None:
        _check_observation_deadline(self.deadline_ns, self.label)
        self._reject_namespace_mutation_events(phase)
        assert self.directory_stat is not None
        for role, (name, descriptor, opened_stat) in self.held.items():
            current_stat = os.fstat(descriptor)
            if _stat_identity(opened_stat) != _stat_identity(current_stat):
                fail(f"{self.label} {role} held object changed during {phase}")
            try:
                named_stat = os.stat(
                    name, dir_fd=self.directory_fd, follow_symlinks=False
                )
            except OSError as error:
                fail(
                    f"{self.label} {role} directory entry disappeared during "
                    f"{phase}: {error}"
                )
            if _stat_identity(named_stat) != _stat_identity(current_stat):
                fail(
                    f"{self.label} {role} directory entry was replaced during {phase}"
                )
        for role, name in self.absent.items():
            try:
                os.stat(name, dir_fd=self.directory_fd, follow_symlinks=False)
            except FileNotFoundError:
                continue
            except OSError as error:
                fail(f"{self.label} absent {role} recheck failed during {phase}: {error}")
            fail(f"{self.label} absent {role} appeared during {phase}")
        if self.entries != _directory_entries(
            self.directory_fd, self.label, self.deadline_ns
        ) or (
            _stat_identity(self.directory_stat)
            != _stat_identity(os.fstat(self.directory_fd))
        ):
            fail(f"{self.label} database namespace changed during {phase}")

    def _hash_descriptor(
        self, role: str, copy_descriptor: int | None = None
    ) -> dict[str, Any]:
        _name, descriptor, opened_stat = self.held[role]
        digest = hashlib.sha256()
        offset = 0
        while offset < opened_stat.st_size:
            _check_observation_deadline(self.deadline_ns, self.label)
            count = min(OBSERVATION_COPY_BLOCK_BYTES, opened_stat.st_size - offset)
            try:
                block = os.pread(descriptor, count, offset)
            except InterruptedError:
                continue
            except OSError as error:
                fail(f"{self.label} {role} held read failed: {error}")
            if not block:
                fail(f"{self.label} {role} held read was truncated")
            digest.update(block)
            if copy_descriptor is not None:
                _write_all(copy_descriptor, block, f"{self.label} private main copy")
            offset += len(block)
        current_stat = os.fstat(descriptor)
        if offset != opened_stat.st_size or _stat_identity(opened_stat) != _stat_identity(
            current_stat
        ):
            fail(f"{self.label} {role} changed while its held bytes were hashed")
        return {
            "state": "present",
            "byte_count": current_stat.st_size,
            "sha256": "sha256:" + digest.hexdigest(),
        }

    def observe(self, *, copy_main_to: pathlib.Path | None = None) -> dict[str, Any]:
        copy_descriptor = -1
        if copy_main_to is not None:
            try:
                copy_descriptor = os.open(
                    copy_main_to,
                    os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC,
                    0o600,
                )
            except OSError as error:
                fail(f"{self.label} private main copy create failed: {error}")
        try:
            output: dict[str, Any] = {}
            for role in ROLE_SUFFIXES:
                if role not in self.held:
                    output[role] = {
                        "state": "absent",
                        "byte_count": 0,
                        "sha256": None,
                    }
                    continue
                output[role] = self._hash_descriptor(
                    role,
                    copy_descriptor
                    if role == "main" and copy_descriptor >= 0
                    else None,
                )
            if copy_descriptor >= 0:
                try:
                    os.fsync(copy_descriptor)
                except OSError as error:
                    fail(f"{self.label} private main copy sync failed: {error}")
                _check_observation_deadline(self.deadline_ns, self.label)
                copied_stat = os.fstat(copy_descriptor)
                if (
                    not stat.S_ISREG(copied_stat.st_mode)
                    or copied_stat.st_size != output["main"]["byte_count"]
                ):
                    fail(f"{self.label} private main copy size/type differs")
            self._revalidate_namespace("held observation")
            output["directory_entries"] = list(self.entries)
            return output
        finally:
            if copy_descriptor >= 0:
                os.close(copy_descriptor)


def observe_file_family(database_path: pathlib.Path, label: str) -> dict[str, Any]:
    """Hash one held regular file family without releasing identity mid-read."""

    deadline_ns = _observation_deadline(None, label)
    with _HeldFileFamily(database_path, label, deadline_ns) as held:
        return held.observe()


def _sqlite_rows(
    database: sqlite3.Connection,
    sql: str,
    width: int,
    label: str,
    *,
    maximum_rows: int,
    deadline_ns: int,
) -> list[tuple[Any, ...]]:
    cursor = _sqlite_cursor(database, sql, label)
    rows: list[tuple[Any, ...]] = []
    while True:
        row = _sqlite_next_row(cursor, width, label, deadline_ns=deadline_ns)
        if row is None:
            break
        if len(rows) >= maximum_rows:
            fail(f"{label} independent SQLite row cardinality exceeds its bound")
        rows.append(row)
    return rows


def _sqlite_cursor(
    database: sqlite3.Connection, sql: str, label: str
) -> sqlite3.Cursor:
    try:
        return database.execute(sql)
    except sqlite3.Error as error:
        fail(f"{label} independent SQLite query failed: {error}")


def _sqlite_next_row(
    cursor: sqlite3.Cursor,
    width: int,
    label: str,
    *,
    deadline_ns: int | None = None,
) -> tuple[Any, ...] | None:
    if deadline_ns is not None:
        _check_observation_deadline(deadline_ns, label)
    try:
        row = cursor.fetchone()
    except sqlite3.Error as error:
        fail(f"{label} independent SQLite row read failed: {error}")
    if row is None:
        return None
    if not isinstance(row, tuple) or len(row) != width:
        fail(f"{label} independent SQLite row width drifted")
    return row


def _sqlite_text(value: Any, label: str) -> str:
    if not isinstance(value, str):
        fail(f"{label} SQLite value is not TEXT")
    encoded = value.encode("utf-8", errors="strict")
    if len(encoded) > OBSERVATION_MAXIMUM_TEXT_BYTES:
        fail(f"{label} SQLite TEXT exceeds the bounded observation size")
    return value


def _sqlite_integer(value: Any, label: str) -> int:
    if type(value) is not int or value < 0 or value > (1 << 63) - 1:
        fail(f"{label} SQLite value is not a nonnegative INTEGER")
    return value


def _sqlite_nullable_text(value: Any, label: str) -> str | None:
    if value is None:
        return None
    return _sqlite_text(value, label)


def _sqlite_blob(value: Any, label: str) -> bytes:
    if not isinstance(value, bytes):
        fail(f"{label} SQLite value is not BLOB")
    return value


def _validate_checksum(checksum: str, payload: bytes, label: str) -> None:
    require_digest(checksum, f"{label} checksum")
    if bytes_digest(payload) != checksum:
        fail(f"{label} payload checksum differs from independently hashed bytes")


def _framed_digest_segment(digest: Any, name: bytes, value: bytes) -> None:
    digest.update(len(name).to_bytes(8, byteorder="big", signed=False))
    digest.update(name)
    digest.update(len(value).to_bytes(8, byteorder="big", signed=False))
    digest.update(value)


class _PayloadChunkReader:
    def __init__(
        self,
        chunks: Any,
        expected_size: int,
        label: str,
        deadline_ns: int | None = None,
    ) -> None:
        self._chunks = iter(chunks)
        self._expected_size = expected_size
        self._label = label
        self._buffer = memoryview(b"")
        self._buffer_offset = 0
        self._offset = 0
        self._phase = "prefix"
        self._prefix = hashlib.sha256()
        self._suffix = hashlib.sha256()
        self._prefix_byte_count = 0
        self._suffix_byte_count = 0
        self._deadline_ns = deadline_ns

    def _next_buffer(self) -> None:
        while self._buffer_offset == len(self._buffer):
            if self._deadline_ns is not None:
                _check_observation_deadline(self._deadline_ns, self._label)
            try:
                chunk = next(self._chunks)
            except StopIteration:
                fail(f"{self._label} is truncated at byte {self._offset}")
            if not isinstance(chunk, bytes) or not chunk:
                fail(f"{self._label} yielded an invalid payload chunk")
            self._buffer = memoryview(chunk)
            self._buffer_offset = 0

    def _consume(self, size: int, label: str, capture: bool) -> bytes:
        if size > self._expected_size - self._offset:
            fail(f"{label} exceeds the declared payload byte count")
        output = bytearray() if capture else None
        remaining = size
        while remaining:
            if self._deadline_ns is not None:
                _check_observation_deadline(self._deadline_ns, self._label)
            self._next_buffer()
            count = min(remaining, len(self._buffer) - self._buffer_offset)
            block = self._buffer[
                self._buffer_offset : self._buffer_offset + count
            ]
            if self._phase == "prefix":
                self._prefix.update(block)
                self._prefix_byte_count += count
            elif self._phase == "suffix":
                self._suffix.update(block)
                self._suffix_byte_count += count
            if output is not None:
                output.extend(block)
            self._buffer_offset += count
            self._offset += count
            remaining -= count
        return bytes(output) if output is not None else b""

    def prefix_u64(self, label: str) -> int:
        if self._phase != "prefix":
            fail(f"{label} was read outside the normalized payload prefix")
        return int.from_bytes(
            self._consume(8, label, True), byteorder="big", signed=False
        )

    def prefix_string(self, label: str, *, capture: bool = False) -> bytes:
        size = self.prefix_u64(f"{label}/byte-count")
        if size > OBSERVATION_MAXIMUM_TEXT_BYTES:
            fail(f"{label} exceeds the bounded payload string size")
        return self._consume(size, label, capture)

    def prefix_bool(self, label: str) -> None:
        value = self._consume(1, label, True)
        if value not in (b"\x00", b"\x01"):
            fail(f"{label} has an invalid canonical boolean")

    def generation(self, label: str) -> int:
        if self._phase != "prefix":
            fail(f"{label} physical generation was parsed more than once")
        self._phase = "generation"
        value = int.from_bytes(
            self._consume(8, label, True), byteorder="big", signed=False
        )
        self._phase = "suffix"
        return value

    def finish(self) -> str:
        self._consume(
            self._expected_size - self._offset,
            f"{self._label}/suffix",
            False,
        )
        try:
            extra = next(self._chunks)
        except StopIteration:
            extra = None
        if extra is not None:
            fail(f"{self._label} yielded bytes beyond its declared payload byte count")
        if self._prefix_byte_count + 8 + self._suffix_byte_count != (
            self._expected_size
        ):
            fail(f"{self._label} normalized payload census is not exact")
        normalized = hashlib.sha256()
        _framed_digest_segment(normalized, b"domain", NORMALIZED_PAYLOAD_DOMAIN)
        _framed_digest_segment(
            normalized,
            b"prefix-byte-count",
            self._prefix_byte_count.to_bytes(8, byteorder="big", signed=False),
        )
        _framed_digest_segment(normalized, b"prefix-sha256", self._prefix.digest())
        _framed_digest_segment(
            normalized,
            b"suffix-byte-count",
            self._suffix_byte_count.to_bytes(8, byteorder="big", signed=False),
        )
        _framed_digest_segment(normalized, b"suffix-sha256", self._suffix.digest())
        return "sha256:" + normalized.hexdigest()


def inspected_normalized_payload_chunks(
    chunks: Any,
    payload_byte_count: int,
    expected_generation: int,
    label: str,
    *,
    deadline_ns: int | None = None,
) -> tuple[str, str]:
    """Return declared payload schema and digest omitting only its generation field."""

    reader = _PayloadChunkReader(
        chunks, payload_byte_count, label, deadline_ns=deadline_ns
    )
    magic_bytes = reader.prefix_string(f"{label}/magic", capture=True)
    try:
        magic = magic_bytes.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        fail(f"{label} payload magic is not UTF-8")
    if magic not in PAYLOAD_MAGICS:
        fail(f"{label} payload magic is not a declared v1-v5 schema")

    for field in (
        "schema",
        "snapshot-id",
    ):
        reader.prefix_string(f"{label}/{field}")
    for field in ("version-major", "version-minor", "version-patch"):
        reader.prefix_u64(f"{label}/{field}")
    for field in (
        "catalog-digest",
        "condition-universe",
        "relation-registry",
        "interpretation-policy",
    ):
        reader.prefix_string(f"{label}/{field}")

    partition_count = reader.prefix_u64(f"{label}/partition-count")
    if partition_count > 1_000_000:
        fail(f"{label} payload partition count exceeds the declared bound")
    for partition_index in range(partition_count):
        partition_label = f"{label}/partition-{partition_index}"
        for field in (
            "id",
            "descriptor",
            "basis",
            "claims",
            "coverage",
            "content",
        ):
            reader.prefix_string(f"{partition_label}/{field}")
        reader.prefix_u64(f"{partition_label}/claim-count")
        reader.prefix_bool(f"{partition_label}/complete")

    closure_count = reader.prefix_u64(f"{label}/closure-count")
    if closure_count > 1_000_000:
        fail(f"{label} payload closure count exceeds the declared bound")
    for closure_index in range(closure_count):
        reader.prefix_string(f"{label}/closure-{closure_index}")

    for field in ("publication-id", "series-id", "publication-snapshot-id"):
        reader.prefix_string(f"{label}/{field}")
    reader.prefix_u64(f"{label}/publication-sequence")
    stored_generation = reader.generation(f"{label}/physical-generation")
    if stored_generation != expected_generation:
        fail(
            f"{label} embedded physical generation differs from its authority row: "
            f"row={expected_generation}, payload={stored_generation}"
        )
    return magic, reader.finish()


def normalized_payload_chunks_digest(
    chunks: Any, payload_byte_count: int, expected_generation: int, label: str
) -> str:
    """Stream a declared v1-v5 payload and omit only its parsed generation field."""

    return inspected_normalized_payload_chunks(
        chunks, payload_byte_count, expected_generation, label
    )[1]


def normalized_payload_digest(
    payload: bytes, expected_generation: int, label: str
) -> str:
    return normalized_payload_chunks_digest(
        (payload,), len(payload), expected_generation, label
    )


def _inspect_publication_payload(
    chunks: Any,
    payload_byte_count: int,
    expected_generation: int,
    state: int,
    stored_checksum: str,
    label: str,
    deadline_ns: int,
) -> dict[str, Any]:
    """Stream one payload and classify committed versus diagnostic authority."""

    aggregate = hashlib.sha256()
    observed_bytes = 0

    def observed_chunks() -> Any:
        nonlocal observed_bytes
        for chunk in chunks:
            _check_observation_deadline(deadline_ns, label)
            if not isinstance(chunk, bytes) or not chunk:
                fail(f"{label} yielded an invalid payload chunk")
            if len(chunk) > payload_byte_count - observed_bytes:
                fail(f"{label} yielded bytes beyond its declared payload byte count")
            aggregate.update(chunk)
            observed_bytes += len(chunk)
            yield chunk

    stream = observed_chunks()
    payload_schema: str | None = None
    normalized_digest: str | None = None
    decode_failed = False
    try:
        payload_schema, normalized_digest = inspected_normalized_payload_chunks(
            stream,
            payload_byte_count,
            expected_generation,
            label,
            deadline_ns=deadline_ns,
        )
    except QualificationError:
        if state == COMMITTED_STATE:
            raise
        decode_failed = True
        # A diagnostic parser may fail before consuming the final chunk.  Drain
        # the same bounded stream so the raw digest and following row cursor are
        # still exact; per-chunk integrity failures remain terminal.
        for _chunk in stream:
            pass
    if observed_bytes != payload_byte_count:
        fail(f"{label} raw payload length differs from its declared byte count")
    raw_digest = "sha256:" + aggregate.hexdigest()
    checksum_matches = raw_digest == stored_checksum
    if state == COMMITTED_STATE:
        if not checksum_matches:
            fail(f"{label} committed payload checksum differs from independently hashed bytes")
        diagnostic_classification = "committed-valid"
    elif not checksum_matches:
        diagnostic_classification = "stored-full-checksum-mismatch"
    elif decode_failed:
        diagnostic_classification = "payload-decode-failure"
    else:
        diagnostic_classification = "valid-noncommitted"
    return {
        "raw_payload_digest": raw_digest,
        "payload_schema": payload_schema,
        "normalized_payload_digest": normalized_digest,
        "diagnostic_classification": diagnostic_classification,
    }


def scan_sqlite_authority(
    database_path: pathlib.Path,
    label: str,
    *,
    deadline_ns: int | None = None,
) -> dict[str, Any]:
    """Read exact authority with an immutable read-only connection owned by the parent."""

    deadline_ns = _observation_deadline(deadline_ns, label)
    try:
        uri = database_path.resolve(strict=True).as_uri() + "?mode=ro&immutable=1"
        database = sqlite3.connect(uri, uri=True, isolation_level=None, timeout=0.0)
    except (OSError, sqlite3.Error) as error:
        fail(f"{label} independent read-only SQLite open failed: {error}")
    try:
        database.set_progress_handler(
            lambda: int(time.monotonic_ns() >= deadline_ns), 1000
        )
        metadata_rows = _sqlite_rows(
            database,
            "SELECT key,value FROM cxxlens_ng_metadata ORDER BY key",
            2,
            label,
            maximum_rows=4,
            deadline_ns=deadline_ns,
        )
        metadata = [
            {
                "key": _sqlite_text(row[0], f"{label} metadata key"),
                "value": _sqlite_text(row[1], f"{label} metadata value"),
            }
            for row in metadata_rows
        ]
        if metadata != sorted(metadata, key=lambda row: row["key"]):
            fail(f"{label} metadata is not in canonical order")
        metadata_map = {row["key"]: row["value"] for row in metadata}
        if len(metadata_map) != len(metadata):
            fail(f"{label} contains duplicate metadata keys")
        physical_format = metadata_map.get("physical_format")
        if physical_format not in (
            "cxxlens.sqlite-semantic-store.v2",
            "cxxlens.sqlite-semantic-store.v3",
        ):
            fail(f"{label} has no exact v2/v3 physical-format marker")

        schema_rows = _sqlite_rows(
            database,
            "SELECT type,name,tbl_name,sql FROM sqlite_schema "
            "WHERE sql IS NOT NULL AND name NOT LIKE 'sqlite_%' ORDER BY type,name",
            4,
            label,
            maximum_rows=6,
            deadline_ns=deadline_ns,
        )
        ddl = [
            {
                "type": _sqlite_text(row[0], f"{label} DDL type"),
                "name": _sqlite_text(row[1], f"{label} DDL name"),
                "table": _sqlite_text(row[2], f"{label} DDL table"),
                "sql": _sqlite_text(row[3], f"{label} DDL SQL"),
            }
            for row in schema_rows
        ]
        canonical_ddl_digest = document_digest(ddl)

        heads = []
        head_cursor = _sqlite_cursor(
            database,
            "SELECT series_id,current_publication,sequence "
            "FROM cxxlens_ng_series_head ORDER BY series_id",
            label,
        )
        while True:
            row = _sqlite_next_row(
                head_cursor, 3, label, deadline_ns=deadline_ns
            )
            if row is None:
                break
            if len(heads) >= OBSERVATION_MAXIMUM_ROWS:
                fail(f"{label} head row cardinality exceeds its observation bound")
            heads.append(
                {
                    "series_id": _sqlite_text(row[0], f"{label} head series"),
                    "publication_id": _sqlite_text(
                        row[1], f"{label} head publication"
                    ),
                    "sequence": _sqlite_integer(row[2], f"{label} head sequence"),
                }
            )

        publications: list[dict[str, Any]] = []
        inventory_publications: list[dict[str, Any]] = []
        inventory_chunks: list[dict[str, Any]] = []
        if physical_format == "cxxlens.sqlite-semantic-store.v2":
            if metadata != [
                {
                    "key": "physical_format",
                    "value": "cxxlens.sqlite-semantic-store.v2",
                }
            ]:
                fail(f"{label} exact-v2 metadata profile drifted")
            publication_cursor = _sqlite_cursor(
                database,
                "SELECT publication_id,series_id,snapshot_id,sequence,generation,parent,"
                "state,checksum,length(payload),typeof(payload) "
                "FROM cxxlens_ng_publication ORDER BY publication_id",
                label,
            )
            index = 0
            while True:
                row = _sqlite_next_row(
                    publication_cursor, 10, label, deadline_ns=deadline_ns
                )
                if row is None:
                    break
                if index >= OBSERVATION_MAXIMUM_ROWS:
                    fail(
                        f"{label} v2 publication cardinality exceeds its observation bound"
                    )
                publication_label = f"{label} v2 publication {index}"
                publication_id = _sqlite_text(
                    row[0], f"{publication_label} id"
                )
                generation = _sqlite_integer(
                    row[4], f"{publication_label} generation"
                )
                state = _sqlite_integer(row[6], f"{publication_label} state")
                if state > 5:
                    fail(f"{publication_label} state is outside the closed row class")
                checksum = _sqlite_text(row[7], f"{publication_label} checksum")
                require_digest(checksum, f"{publication_label} checksum")
                payload_byte_count = _sqlite_integer(
                    row[8], f"{publication_label} payload byte count"
                )
                if payload_byte_count > (
                    CHUNK_MAXIMUM_BYTES * OBSERVATION_MAXIMUM_CHUNK_ROWS
                ):
                    fail(f"{publication_label} payload chunk cardinality exceeds its bound")
                if row[9] != "blob":
                    fail(f"{publication_label} payload storage class is not BLOB")

                def payload_chunks(
                    publication_key: str = publication_id,
                    byte_count: int = payload_byte_count,
                    chunk_label: str = publication_label,
                ) -> Any:
                    offset = 0
                    while offset < byte_count:
                        _check_observation_deadline(deadline_ns, chunk_label)
                        count = min(CHUNK_MAXIMUM_BYTES, byte_count - offset)
                        try:
                            cursor = database.execute(
                                "SELECT substr(payload,?,?) "
                                "FROM cxxlens_ng_publication WHERE publication_id=?",
                                (offset + 1, count, publication_key),
                            )
                        except sqlite3.Error as error:
                            fail(
                                f"{chunk_label} bounded payload query failed: {error}"
                            )
                        chunk_row = _sqlite_next_row(
                            cursor, 1, chunk_label, deadline_ns=deadline_ns
                        )
                        if chunk_row is None or _sqlite_next_row(
                            cursor, 1, chunk_label, deadline_ns=deadline_ns
                        ) is not None:
                            fail(f"{chunk_label} keyed payload row is not exact")
                        chunk = _sqlite_blob(
                            chunk_row[0], f"{chunk_label} bounded payload chunk"
                        )
                        if len(chunk) != count or len(chunk) > CHUNK_MAXIMUM_BYTES:
                            fail(f"{chunk_label} bounded payload chunk size differs")
                        offset += len(chunk)
                        yield chunk

                inspection = _inspect_publication_payload(
                    payload_chunks(),
                    payload_byte_count,
                    generation,
                    state,
                    checksum,
                    publication_label,
                    deadline_ns,
                )
                publications.append(
                    {
                        "publication_id": publication_id,
                        "series_id": _sqlite_text(
                            row[1], f"{publication_label} series id"
                        ),
                        "snapshot_id": _sqlite_text(
                            row[2], f"{publication_label} snapshot id"
                        ),
                        "sequence": _sqlite_integer(
                            row[3], f"{publication_label} sequence"
                        ),
                        "generation": generation,
                        "parent": _sqlite_nullable_text(
                            row[5], f"{publication_label} parent"
                        ),
                        "state": state,
                        "payload_checksum": checksum,
                        "payload_byte_count": payload_byte_count,
                        **inspection,
                    }
                )
                index += 1
            inventory = {
                "profile": "cxxlens.sqlite-v2-single-blob-not-applicable",
                "publications": [],
                "chunks": [],
            }
        else:
            canonical_ddl_digest = validate_current_v3_ddl(ddl, label)
            expected_metadata = [
                {"key": "payload_chunk_maximum_bytes", "value": "8388608"},
                {
                    "key": "payload_chunk_profile",
                    "value": "cxxlens.sqlite-payload-chunks.v1",
                },
                {
                    "key": "physical_format",
                    "value": "cxxlens.sqlite-semantic-store.v3",
                },
                {"key": "physical_format_version", "value": "3.0.0"},
            ]
            if metadata != expected_metadata:
                fail(f"{label} exact-v3 metadata profile drifted")
            publication_cursor = _sqlite_cursor(
                database,
                "SELECT publication_id,series_id,snapshot_id,sequence,generation,parent,"
                "state,payload_checksum,payload_byte_count,payload_chunk_count "
                "FROM cxxlens_ng_publication ORDER BY publication_id,generation",
                label,
            )
            chunk_cursor = _sqlite_cursor(
                database,
                "SELECT publication_id,generation,chunk_ordinal,byte_offset,byte_count,"
                "checksum,length(payload),typeof(payload),"
                f"substr(payload,1,{CHUNK_MAXIMUM_BYTES + 1}) "
                "FROM cxxlens_ng_payload_chunk "
                "ORDER BY publication_id,generation,chunk_ordinal",
                label,
            )
            index = 0
            while True:
                row = _sqlite_next_row(
                    publication_cursor, 10, label, deadline_ns=deadline_ns
                )
                if row is None:
                    break
                if index >= OBSERVATION_MAXIMUM_ROWS:
                    fail(
                        f"{label} v3 publication cardinality exceeds its observation bound"
                    )
                publication_label = f"{label} publication {index}"
                publication_id = _sqlite_text(row[0], f"{publication_label} id")
                generation = _sqlite_integer(row[4], f"{publication_label} generation")
                state = _sqlite_integer(row[6], f"{publication_label} state")
                if state > 5:
                    fail(f"{publication_label} state is outside the closed row class")
                payload_checksum = _sqlite_text(
                    row[7], f"{publication_label} checksum"
                )
                require_digest(payload_checksum, f"{publication_label} checksum")
                payload_byte_count = _sqlite_integer(
                    row[8], f"{publication_label} byte count"
                )
                payload_chunk_count = _sqlite_integer(
                    row[9], f"{publication_label} chunk count"
                )
                expected_count = (
                    0
                    if payload_byte_count == 0
                    else 1 + (payload_byte_count - 1) // CHUNK_MAXIMUM_BYTES
                )
                if payload_chunk_count != expected_count:
                    fail(f"{publication_label} declared noncanonical chunk count")
                offset = 0

                def payload_chunks() -> Any:
                    nonlocal offset
                    for ordinal in range(expected_count):
                        if len(inventory_chunks) >= OBSERVATION_MAXIMUM_CHUNK_ROWS:
                            fail(f"{label} chunk row cardinality exceeds its bound")
                        chunk = _sqlite_next_row(
                            chunk_cursor,
                            9,
                            f"{publication_label} chunk {ordinal}",
                            deadline_ns=deadline_ns,
                        )
                        if chunk is None:
                            fail(f"{publication_label} chunk census differs")
                        chunk_publication_id = _sqlite_text(
                            chunk[0], f"{label} chunk publication id"
                        )
                        chunk_generation = _sqlite_integer(
                            chunk[1], f"{label} chunk generation"
                        )
                        chunk_ordinal = _sqlite_integer(
                            chunk[2], f"{label} chunk ordinal"
                        )
                        byte_offset = _sqlite_integer(
                            chunk[3], f"{label} chunk offset"
                        )
                        byte_count = _sqlite_integer(
                            chunk[4], f"{label} chunk byte count"
                        )
                        checksum = _sqlite_text(
                            chunk[5], f"{label} chunk checksum"
                        )
                        stored_byte_count = _sqlite_integer(
                            chunk[6], f"{label} stored chunk byte count"
                        )
                        if chunk[7] != "blob":
                            fail(f"{publication_label} chunk storage class is not BLOB")
                        payload = _sqlite_blob(chunk[8], f"{label} chunk payload")
                        expected_size = min(
                            CHUNK_MAXIMUM_BYTES, payload_byte_count - offset
                        )
                        if (
                            chunk_publication_id != publication_id
                            or chunk_generation != generation
                            or chunk_ordinal != ordinal
                            or byte_offset != offset
                            or byte_count != expected_size
                            or stored_byte_count != byte_count
                            or len(payload) != byte_count
                        ):
                            fail(
                                f"{publication_label} has a noncanonical chunk"
                            )
                        _validate_checksum(
                            checksum,
                            payload,
                            f"{publication_label} chunk {ordinal}",
                        )
                        inventory_chunks.append(
                            {
                                "publication_id": publication_id,
                                "generation": generation,
                                "chunk_ordinal": chunk_ordinal,
                                "byte_offset": byte_offset,
                                "byte_count": byte_count,
                                "chunk_checksum": checksum,
                            }
                        )
                        offset += byte_count
                        yield payload

                inspection = _inspect_publication_payload(
                    payload_chunks(),
                    payload_byte_count,
                    generation,
                    state,
                    payload_checksum,
                    publication_label,
                    deadline_ns,
                )
                publication = {
                    "publication_id": publication_id,
                    "series_id": _sqlite_text(row[1], f"{label} series id"),
                    "snapshot_id": _sqlite_text(row[2], f"{label} snapshot id"),
                    "sequence": _sqlite_integer(row[3], f"{label} sequence"),
                    "generation": generation,
                    "parent": _sqlite_nullable_text(row[5], f"{label} parent"),
                    "state": state,
                    "payload_checksum": payload_checksum,
                    "payload_byte_count": payload_byte_count,
                    **inspection,
                }
                publications.append(publication)
                inventory_publications.append(
                    {
                        "publication_id": publication_id,
                        "generation": generation,
                        "payload_byte_count": payload_byte_count,
                        "payload_chunk_count": payload_chunk_count,
                        "payload_checksum": payload_checksum,
                    }
                )
                index += 1
            if _sqlite_next_row(
                chunk_cursor, 9, f"{label} trailing chunk", deadline_ns=deadline_ns
            ) is not None:
                fail(f"{label} contains orphan payload chunks")
            inventory = {
                "profile": "cxxlens.sqlite-payload-chunks.v1",
                "publications": inventory_publications,
                "chunks": inventory_chunks,
            }

        logical_publications: list[dict[str, Any]] = []
        committed_payload_schema_census: list[dict[str, str]] = []
        noncommitted_census: list[dict[str, Any]] = []
        for publication in publications:
            if publication["state"] == COMMITTED_STATE:
                if (
                    publication["normalized_payload_digest"] is None
                    or publication["payload_schema"] not in PAYLOAD_MAGICS
                    or publication["diagnostic_classification"] != "committed-valid"
                ):
                    fail(f"{label} committed payload validation census is incomplete")
                logical_publications.append(
                    {
                        key: value
                        for key, value in publication.items()
                        if key
                        not in (
                            "generation",
                            "payload_checksum",
                            "raw_payload_digest",
                            "diagnostic_classification",
                        )
                    }
                )
                committed_payload_schema_census.append(
                    {
                        "publication_id": publication["publication_id"],
                        "payload_schema": publication["payload_schema"],
                    }
                )
            else:
                diagnostic = {
                    key: value
                    for key, value in publication.items()
                    if key != "normalized_payload_digest"
                }
                noncommitted_census.append(diagnostic)
                logical_publications.append(diagnostic)
        authority_projection = {
            "publications": logical_publications,
            "heads": heads,
        }
        return {
            "physical_format": physical_format,
            "metadata": metadata,
            "ddl": ddl,
            "canonical_ddl_digest": canonical_ddl_digest,
            "publications": publications,
            "heads": heads,
            "chunk_inventory": inventory,
            "committed_payload_schema_census": committed_payload_schema_census,
            "noncommitted_census": noncommitted_census,
            "noncommitted_census_digest": document_digest(noncommitted_census),
            "non_generation_payload_projection_digest": document_digest(
                authority_projection
            ),
        }
    finally:
        database.close()


def _observe_held_database(
    held: _HeldFileFamily,
    label: str,
    *,
    deadline_ns: int,
    observation_hook: Callable[[str, pathlib.Path], None] | None = None,
) -> dict[str, Any]:
    """Scan one endpoint without releasing the caller-owned source identities."""

    with tempfile.TemporaryDirectory(prefix="cxxlens-sqlite-held-scan-") as raw:
        private_path = pathlib.Path(raw) / "database.sqlite"
        before = held.observe(copy_main_to=private_path)
        if any(before[role]["state"] != "absent" for role in ("wal", "shm", "journal")):
            fail(
                f"{label} qualification checkpoint is not quiescent; "
                "a main-only private scan cannot represent sidecar authority"
            )
        if observation_hook is not None:
            observation_hook("private-copy-complete", held.database_path)
        authority = scan_sqlite_authority(
            private_path, label, deadline_ns=deadline_ns
        )
        if observation_hook is not None:
            observation_hook("private-scan-complete", held.database_path)
        after = held.observe()
        if before != after:
            fail(f"{label} held source changed across its private SQLite scan")
        return {"file_family": after, "authority": authority}


def observe_database(
    database_path: pathlib.Path,
    label: str,
    *,
    deadline_ns: int | None = None,
    observation_hook: Callable[[str, pathlib.Path], None] | None = None,
) -> dict[str, Any]:
    """Scan only a bounded copy of one continuously held quiescent main file."""

    deadline_ns = _observation_deadline(deadline_ns, label)
    with _HeldFileFamily(database_path, label, deadline_ns) as held:
        return _observe_held_database(
            held,
            label,
            deadline_ns=deadline_ns,
            observation_hook=observation_hook,
        )


def _write_pipe_byte(descriptor: int, value: bytes, label: str) -> None:
    while True:
        try:
            count = os.write(descriptor, value)
        except InterruptedError:
            continue
        except OSError as error:
            fail(f"{label} control write failed: {error}")
        if count != len(value):
            fail(f"{label} control write was short")
        return


def _runner_failure_detail(stderr_path: pathlib.Path) -> str:
    try:
        raw = stderr_path.read_bytes()
    except OSError:
        return "stderr unavailable"
    return raw[-4096:].decode("utf-8", errors="replace").strip()


def _terminate_and_reap(pid: int) -> tuple[int, Any]:
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    grace_deadline = time.monotonic_ns() + TERMINATION_GRACE_SECONDS * 1_000_000_000
    while time.monotonic_ns() < grace_deadline:
        try:
            waited_pid, status, usage = os.wait4(pid, os.WNOHANG)
        except InterruptedError:
            continue
        if waited_pid == pid:
            return status, usage
        time.sleep(0.01)
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    while True:
        try:
            _waited_pid, status, usage = os.wait4(pid, 0)
            return status, usage
        except InterruptedError:
            continue


def validate_compiled_provenance(
    child: dict[str, Any],
    configuration: str,
    revision: str,
    source_tree: str,
    label: str,
) -> None:
    compiled_provenance = require_object(
        child.get("compiled_provenance"), f"{label} compiled provenance"
    )
    if compiled_provenance != {
        "configuration": configuration,
        "revision": revision,
        "source_tree": source_tree,
    }:
        fail(
            f"{label} compiled provenance differs from the requested exact "
            "revision/tree/configuration"
        )


def run_case(
    runner: pathlib.Path,
    configuration: str,
    case_id: str,
    case_directory: pathlib.Path,
    revision: str,
    source_tree: str,
) -> dict[str, Any]:
    case_directory.mkdir()
    child_result_path = case_directory / "child-result.json"
    stdout_path = case_directory / "stdout.log"
    stderr_path = case_directory / "stderr.log"
    stdout_fd = os.open(stdout_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    stderr_fd = os.open(stderr_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    event_read, event_write = os.pipe2(os.O_CLOEXEC)
    control_read, control_write = os.pipe2(os.O_CLOEXEC)
    os.set_inheritable(event_write, True)
    os.set_inheritable(control_read, True)
    expected_checkpoints = CHECKPOINTS[case_id]
    observations: dict[str, dict[str, Any]] = {}
    start = time.monotonic_ns()
    try:
        pid = os.fork()
    except OSError as error:
        for descriptor in (
            stdout_fd,
            stderr_fd,
            event_read,
            event_write,
            control_read,
            control_write,
        ):
            os.close(descriptor)
        fail(f"{configuration}/{case_id} fork failed: {error}")
    if pid == 0:
        try:
            os.close(event_read)
            os.close(control_write)
            os.dup2(stdout_fd, 1)
            os.dup2(stderr_fd, 2)
            os.close(stdout_fd)
            os.close(stderr_fd)
            argv = [
                str(runner),
                "--configuration",
                configuration,
                "--case",
                case_id,
                "--work-directory",
                str(case_directory),
                "--output",
                str(child_result_path),
                "--event-fd",
                str(event_write),
                "--control-fd",
                str(control_read),
            ]
            os.execv(str(runner), argv)
        except BaseException as error:  # pragma: no cover - child-only terminal path
            try:
                os.write(2, f"qualification exec failed: {error}\n".encode())
            finally:
                os._exit(127)

    os.close(stdout_fd)
    os.close(stderr_fd)
    os.close(event_write)
    os.close(control_read)
    database_path = case_directory / "database" / "database.sqlite"
    status: int | None = None
    usage: Any = None
    primary_error: Exception | None = None
    v2_observation_session: _HeldFileFamily | None = None
    try:
        os.set_blocking(event_read, False)
        poller = select.poll()
        poller.register(event_read, select.POLLIN | select.POLLHUP | select.POLLERR)
        pending = b""
        checkpoint_index = 0
        event_eof = False
        deadline = start + CASE_TIMEOUT_SECONDS * 1_000_000_000

        def consume_events() -> None:
            nonlocal pending, checkpoint_index, event_eof, v2_observation_session
            while True:
                try:
                    block = os.read(event_read, 4096)
                except InterruptedError:
                    continue
                except BlockingIOError:
                    return
                if not block:
                    event_eof = True
                    try:
                        poller.unregister(event_read)
                    except KeyError:
                        pass
                    return
                pending += block
                while b"\n" in pending:
                    raw_name, pending = pending.split(b"\n", 1)
                    try:
                        name = raw_name.decode("ascii", errors="strict")
                    except UnicodeError as error:
                        fail(
                            f"{configuration}/{case_id} checkpoint is not ASCII: "
                            f"{error}"
                        )
                    if checkpoint_index >= len(expected_checkpoints) or name != (
                        expected_checkpoints[checkpoint_index]
                    ):
                        fail(
                            f"{configuration}/{case_id} checkpoint order drifted: "
                            f"{name}"
                        )
                    endpoint_label = f"{configuration}/{case_id}/{name}"
                    if case_id == CASE_IDS[1]:
                        # The zero-mutation claim is about the whole API operation,
                        # not merely two equal endpoint snapshots.  Keep the first
                        # checkpoint's objects and mutation watch owned until the
                        # second endpoint has been copied, scanned, and revalidated.
                        if name == expected_checkpoints[0]:
                            if v2_observation_session is not None:
                                fail(f"{endpoint_label} held session was duplicated")
                            v2_observation_session = _HeldFileFamily(
                                database_path,
                                f"{configuration}/{case_id}/cross-operation",
                                deadline,
                            )
                        elif v2_observation_session is None:
                            fail(f"{endpoint_label} held session is unavailable")
                        assert v2_observation_session is not None
                        observations[name] = _observe_held_database(
                            v2_observation_session,
                            endpoint_label,
                            deadline_ns=deadline,
                        )
                        if name == expected_checkpoints[-1]:
                            v2_observation_session.close()
                            v2_observation_session = None
                    else:
                        observations[name] = observe_database(
                            database_path,
                            endpoint_label,
                            deadline_ns=deadline,
                        )
                    if time.monotonic_ns() >= deadline:
                        fail(
                            f"{configuration}/{case_id} exceeded the fixed "
                            f"{CASE_TIMEOUT_SECONDS}s qualification deadline"
                        )
                    checkpoint_index += 1
                    _write_pipe_byte(
                        control_write, b"c", f"{configuration}/{case_id}/{name}"
                    )

        while status is None:
            remaining = deadline - time.monotonic_ns()
            if remaining <= 0:
                fail(
                    f"{configuration}/{case_id} exceeded the fixed "
                    f"{CASE_TIMEOUT_SECONDS}s qualification deadline"
                )
            if event_eof:
                time.sleep(min(0.05, remaining / 1_000_000_000))
            else:
                timeout_ms = max(1, min(100, (remaining + 999_999) // 1_000_000))
                events = poller.poll(timeout_ms)
                if any(
                    descriptor == event_read
                    and flags & (select.POLLIN | select.POLLHUP | select.POLLERR)
                    for descriptor, flags in events
                ):
                    consume_events()
            try:
                waited_pid, waited_status, waited_usage = os.wait4(pid, os.WNOHANG)
            except InterruptedError:
                continue
            if waited_pid == pid:
                status = waited_status
                usage = waited_usage
        consume_events()
        if pending:
            fail(f"{configuration}/{case_id} ended with a partial checkpoint")
        if checkpoint_index != len(expected_checkpoints):
            fail(f"{configuration}/{case_id} did not reach every checkpoint")
    except Exception as error:
        primary_error = error
        if status is None:
            try:
                status, usage = _terminate_and_reap(pid)
            except OSError as reap_error:
                primary_error = QualificationError(
                    f"{configuration}/{case_id} termination/reap failed after "
                    f"{error}: {reap_error}"
                )
    finally:
        if v2_observation_session is not None:
            v2_observation_session.close()
            v2_observation_session = None
        os.close(event_read)
        os.close(control_write)
    elapsed = time.monotonic_ns() - start
    if primary_error is not None:
        raise primary_error
    assert status is not None and usage is not None
    if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
        detail = _runner_failure_detail(stderr_path)
        fail(
            f"{configuration}/{case_id} child did not exit successfully: "
            f"status={status}; {detail}"
        )
    child = load_strict_json(child_result_path, f"{configuration}/{case_id} child result")
    if (
        child.get("schema") != "cxxlens.sqlite-store-v3-case-result.v1"
        or child.get("configuration") != configuration
        or child.get("case_id") != case_id
        or child.get("database_relative_path") != "database/database.sqlite"
    ):
        fail(f"{configuration}/{case_id} child result identity drifted")
    validate_compiled_provenance(
        child,
        configuration,
        revision,
        source_tree,
        f"{configuration}/{case_id}",
    )
    final_observation = observe_database(
        database_path,
        f"{configuration}/{case_id}/post-exit",
        deadline_ns=start + CASE_TIMEOUT_SECONDS * 1_000_000_000,
    )
    if final_observation != observations[expected_checkpoints[-1]]:
        fail(f"{configuration}/{case_id} database changed after its final checkpoint")
    peak_rss_kib = require_uint64(
        int(usage.ru_maxrss), f"{configuration}/{case_id} wait4 ru_maxrss"
    )
    if peak_rss_kib == 0:
        fail(f"{configuration}/{case_id} wait4 ru_maxrss was zero")
    os_peak_rss_bytes = peak_rss_kib * 1024
    if os_peak_rss_bytes > UINT64_MAX:
        fail(f"{configuration}/{case_id} RSS conversion exceeds uint64")
    return {
        "child": child,
        "checkpoints": observations,
        "process": {
            "exit_status": 0,
            "wall_elapsed_ns": require_uint64(elapsed, "monotonic elapsed time"),
            "peak_rss_kib": peak_rss_kib,
            "os_peak_rss_bytes": os_peak_rss_bytes,
        },
        "final": final_observation,
    }


def _disk_census(file_family: dict[str, Any], label: str) -> dict[str, int]:
    values: dict[str, int] = {}
    for role in ROLE_SUFFIXES:
        entry = require_object(file_family.get(role), f"{label} {role}")
        values[f"{role}_bytes"] = require_uint64(
            entry.get("byte_count"), f"{label} {role} byte count"
        )
    values["total_bytes"] = checked_sum(
        list(values.values()), f"{label} file-family byte census"
    )
    return values


def _child_open(child: dict[str, Any], key: str, label: str) -> dict[str, Any]:
    observations = require_object(child.get("observations"), f"{label} observations")
    opened = require_object(observations.get(key), f"{label} {key}")
    backend = require_string(opened.get("backend"), f"{label} {key} backend")
    readable = require_string(
        opened.get("readable_format"), f"{label} {key} readable format"
    )
    direct_open = require_bool(
        opened.get("direct_open"), f"{label} {key} direct-open"
    )
    migration_required = require_bool(
        opened.get("migration_required"), f"{label} {key} migration-required"
    )
    projection = require_object(opened.get("projection"), f"{label} {key} projection")
    normalized_projection = {
        "snapshot_id": require_string(
            projection.get("snapshot_id"), f"{label} {key} snapshot id"
        ),
        "publication_id": require_string(
            projection.get("publication_id"), f"{label} {key} publication id"
        ),
        "sequence": require_uint64(
            projection.get("sequence"), f"{label} {key} sequence"
        ),
        "physical_generation": require_uint64(
            projection.get("physical_generation"),
            f"{label} {key} physical generation",
        ),
        "canonical_export_digest": require_digest(
            projection.get("canonical_export_digest"),
            f"{label} {key} canonical export",
        ),
        "query_projection_digest": require_digest(
            projection.get("query_projection_digest"),
            f"{label} {key} query projection",
        ),
    }
    return {
        "backend": backend,
        "readable_format": readable,
        "direct_open": direct_open,
        "migration_required": migration_required,
        "projection": normalized_projection,
    }


def _validate_open(
    opened: dict[str, Any],
    authority: dict[str, Any] | None,
    *,
    backend: str,
    readable_format: str,
    migration_required: bool,
    label: str,
) -> None:
    if (
        opened["backend"] != backend
        or opened["readable_format"] != readable_format
        or not opened["direct_open"]
        or opened["migration_required"] != migration_required
    ):
        fail(f"{label} compatibility tuple differs from the exact expected tuple")
    if authority is None:
        return
    projection = opened["projection"]
    matching = [
        row
        for row in authority["publications"]
        if row["publication_id"] == projection["publication_id"]
    ]
    if len(matching) != 1:
        fail(f"{label} API publication is not exact in the SQLite authority")
    publication = matching[0]
    if (
        publication["snapshot_id"] != projection["snapshot_id"]
        or publication["sequence"] != projection["sequence"]
        or publication["generation"] != projection["physical_generation"]
    ):
        fail(f"{label} API projection differs from its SQLite publication row")
    matching_heads = [
        head
        for head in authority["heads"]
        if head["publication_id"] == projection["publication_id"]
    ]
    if len(matching_heads) != 1 or matching_heads[0]["sequence"] != projection["sequence"]:
        fail(f"{label} API current projection differs from the SQLite series head")


def _semantic_projection_digest(opened: dict[str, Any]) -> str:
    projection = opened["projection"]
    return document_digest(
        {
            "snapshot_id": projection["snapshot_id"],
            "publication_id": projection["publication_id"],
            "sequence": projection["sequence"],
            "canonical_export_digest": projection["canonical_export_digest"],
            "query_projection_digest": projection["query_projection_digest"],
        }
    )


def _parity_projection(opened: dict[str, Any]) -> dict[str, str]:
    return {
        "semantic_projection_digest": _semantic_projection_digest(opened),
        "canonical_export_digest": opened["projection"]["canonical_export_digest"],
        "query_projection_digest": opened["projection"]["query_projection_digest"],
    }


def _generation_census(authority: dict[str, Any]) -> list[dict[str, Any]]:
    rows = [
        {
            "publication_id": row["publication_id"],
            "sequence": row["sequence"],
            "state": row["state"],
            "generation": row["generation"],
        }
        for row in authority["publications"]
    ]
    rows.sort(key=lambda row: row["publication_id"])
    return rows


def migration_generation_evidence(
    source: list[dict[str, Any]],
    target: list[dict[str, Any]],
    cold: list[dict[str, Any]],
    label: str,
) -> dict[str, Any]:
    """Independently prove the deterministic whole committed-generation replacement."""

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
            require_uint64(row["sequence"], f"{label} {phase} sequence")
            state = require_uint64(row["state"], f"{label} {phase} state")
            require_uint64(row["generation"], f"{label} {phase} generation")
            if state > 5 or publication_id in output:
                fail(f"{label} {phase} generation census is invalid")
            output[publication_id] = row
        return output

    source_by_id = indexed(source, "source")
    target_by_id = indexed(target, "target")
    cold_by_id = indexed(cold, "cold")
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


def migration_payload_evidence(
    source: dict[str, Any],
    target: dict[str, Any],
    cold: dict[str, Any],
    label: str,
) -> dict[str, Any]:
    """Prove v1-v5 schema preservation and exact diagnostic-row transport."""

    def committed_census(value: dict[str, Any], phase: str) -> list[dict[str, str]]:
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

    source_committed = committed_census(source, "source")
    target_committed = committed_census(target, "target")
    cold_committed = committed_census(cold, "cold")
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

    def diagnostic_census(value: dict[str, Any], phase: str) -> list[dict[str, Any]]:
        rows = value["noncommitted_census"]
        if rows != sorted(rows, key=lambda row: row["publication_id"]):
            fail(f"{label} {phase} noncommitted census order differs")
        seen: set[str] = set()
        for index, row in enumerate(rows):
            if set(row) != expected_diagnostic_fields:
                fail(f"{label} {phase} noncommitted row {index} fields differ")
            publication_id = row["publication_id"]
            if (
                not isinstance(publication_id, str)
                or not publication_id
                or publication_id in seen
                or type(row["state"]) is not int
                or row["state"] < 0
                or row["state"] > 5
                or row["state"] == COMMITTED_STATE
                or row["diagnostic_classification"]
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
            if row["payload_schema"] is not None and row["payload_schema"] not in (
                PAYLOAD_MAGICS
            ):
                fail(f"{label} {phase} diagnostic payload schema is invalid")
            require_uint64(row["sequence"], f"{label} {phase} diagnostic sequence")
            require_uint64(row["generation"], f"{label} {phase} diagnostic generation")
            require_uint64(row["state"], f"{label} {phase} diagnostic state")
            require_uint64(
                row["payload_byte_count"],
                f"{label} {phase} diagnostic payload byte count",
            )
            require_digest(
                row["payload_checksum"], f"{label} {phase} diagnostic checksum"
            )
            require_digest(
                row["raw_payload_digest"],
                f"{label} {phase} diagnostic raw payload",
            )
            checksum_matches = (
                row["payload_checksum"] == row["raw_payload_digest"]
            )
            classification = row["diagnostic_classification"]
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

    source_diagnostics = diagnostic_census(source, "source")
    target_diagnostics = diagnostic_census(target, "target")
    cold_diagnostics = diagnostic_census(cold, "cold")
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


def _common_raw(
    configuration: str,
    case_id: str,
    backend_scope: str,
    execution: dict[str, Any],
    observations: dict[str, Any],
) -> dict[str, Any]:
    final = execution["final"]
    return {
        "schema": "cxxlens.sqlite-store-v3-raw-evidence.v1",
        "configuration": configuration,
        "case_id": case_id,
        "backend_scope": backend_scope,
        "method": METHOD,
        "process": execution["process"],
        "disk": _disk_census(final["file_family"], f"{configuration}/{case_id}"),
        "chunk_inventory": final["authority"]["chunk_inventory"],
        "observations": observations,
    }


def build_current_raw(configuration: str, execution: dict[str, Any]) -> dict[str, Any]:
    label = f"{configuration}/{CASE_IDS[0]}"
    initial = _child_open(execution["child"], "initial_open", label)
    cold = _child_open(execution["child"], "cold_reopen", label)
    initial_authority = execution["checkpoints"]["current-initial"]["authority"]
    cold_authority = execution["checkpoints"]["current-cold"]["authority"]
    _validate_open(
        initial,
        initial_authority,
        backend="sqlite",
        readable_format="3.0.0",
        migration_required=False,
        label=f"{label}/initial",
    )
    _validate_open(
        cold,
        cold_authority,
        backend="sqlite",
        readable_format="3.0.0",
        migration_required=False,
        label=f"{label}/cold",
    )

    def observation(opened: dict[str, Any], authority: dict[str, Any]) -> dict[str, Any]:
        return {
            "physical_format": authority["physical_format"],
            "readable_format": opened["readable_format"],
            "metadata_digest": document_digest(authority["metadata"]),
            "canonical_ddl_digest": authority["canonical_ddl_digest"],
            "semantic_projection_digest": _semantic_projection_digest(opened),
        }

    return _common_raw(
        configuration,
        CASE_IDS[0],
        BACKEND_SCOPES[0],
        execution,
        {
            "initial_open": observation(initial, initial_authority),
            "cold_reopen": observation(cold, cold_authority),
        },
    )


def build_v2_raw(configuration: str, execution: dict[str, Any]) -> dict[str, Any]:
    label = f"{configuration}/{CASE_IDS[1]}"
    opened = _child_open(execution["child"], "open", label)
    before = execution["checkpoints"]["v2-before"]
    after = execution["checkpoints"]["v2-after"]
    _validate_open(
        opened,
        after["authority"],
        backend="sqlite",
        readable_format="2.6.0",
        migration_required=True,
        label=label,
    )
    begin_result = require_object(
        require_object(execution["child"].get("observations"), f"{label} observations").get(
            "begin_result"
        ),
        f"{label} begin result",
    )
    exact_begin = {
        "code": "store.migration-required",
        "field": "sqlite-physical-format",
        "detail": "cxxlens.sqlite-semantic-store.v2-to-v3",
    }
    if begin_result != exact_begin:
        fail(f"{label} begin result differs from the exact migration-required tuple")
    if before["authority"] != after["authority"]:
        fail(f"{label} read-only API operation changed SQLite logical authority")
    return _common_raw(
        configuration,
        CASE_IDS[1],
        BACKEND_SCOPES[1],
        execution,
        {
            "physical_format": after["authority"]["physical_format"],
            "reported_readable_format": opened["readable_format"],
            "direct_open": opened["direct_open"],
            "migration_required": opened["migration_required"],
            "semantic_projection_digest": _semantic_projection_digest(opened),
            "begin_result": exact_begin,
            "before": before["file_family"],
            "after": after["file_family"],
        },
    )


def build_migration_raw(configuration: str, execution: dict[str, Any]) -> dict[str, Any]:
    label = f"{configuration}/{CASE_IDS[2]}"
    source = _child_open(execution["child"], "source", label)
    target = _child_open(execution["child"], "target", label)
    cold = _child_open(execution["child"], "cold_reopen", label)
    source_authority = execution["checkpoints"]["migration-source"]["authority"]
    target_authority = execution["checkpoints"]["migration-target"]["authority"]
    cold_authority = execution["checkpoints"]["migration-cold"]["authority"]
    _validate_open(
        source,
        source_authority,
        backend="sqlite",
        readable_format="2.6.0",
        migration_required=True,
        label=f"{label}/source",
    )
    for phase, opened, authority in (
        ("target", target, target_authority),
        ("cold", cold, cold_authority),
    ):
        _validate_open(
            opened,
            authority,
            backend="sqlite",
            readable_format="3.0.0",
            migration_required=False,
            label=f"{label}/{phase}",
        )
    receipts = require_object(
        execution["child"].get("production_receipts"), f"{label} receipts"
    )
    begin_count = receipts.get("migration_begin_immediate_count")
    if type(begin_count) is not int:
        raise ProductionReceiptUnavailable(
            f"{label}: production migration BEGIN IMMEDIATE receipt is unavailable; "
            "add a source-private Store compact observation scope that counts the actual "
            "writer connection's BEGIN IMMEDIATE executions"
        )
    if begin_count != 1:
        fail(f"{label} production migration receipt observed {begin_count} BEGINs")
    source_generations = _generation_census(source_authority)
    target_generations = _generation_census(target_authority)
    cold_generations = _generation_census(cold_authority)
    generation_evidence = migration_generation_evidence(
        source_generations,
        target_generations,
        cold_generations,
        label,
    )
    payload_evidence = migration_payload_evidence(
        source_authority, target_authority, cold_authority, label
    )

    def projection(opened: dict[str, Any], authority: dict[str, Any]) -> dict[str, Any]:
        return {
            "physical_format": authority["physical_format"],
            "semantic_projection_digest": _semantic_projection_digest(opened),
            "non_generation_payload_projection_digest": authority[
                "non_generation_payload_projection_digest"
            ],
            "generation_census": _generation_census(authority),
            "committed_payload_schema_census": authority[
                "committed_payload_schema_census"
            ],
            "noncommitted_census": authority["noncommitted_census"],
            "noncommitted_census_digest": authority["noncommitted_census_digest"],
        }

    return _common_raw(
        configuration,
        CASE_IDS[2],
        BACKEND_SCOPES[2],
        execution,
        {
            "trigger": "snapshot-store-compact",
            "begin_immediate_count": begin_count,
            "source": projection(source, source_authority),
            "target": projection(target, target_authority),
            "cold_reopen": projection(cold, cold_authority),
            **generation_evidence,
            **payload_evidence,
            "canonical_ddl_digest": target_authority["canonical_ddl_digest"],
        },
    )


def build_limit_raw(configuration: str, execution: dict[str, Any]) -> dict[str, Any]:
    label = f"{configuration}/{CASE_IDS[3]}"
    memory = _child_open(execution["child"], "memory", label)
    sqlite = _child_open(execution["child"], "sqlite", label)
    cold = _child_open(execution["child"], "cold_reopen_sqlite", label)
    sqlite_authority = execution["checkpoints"]["limit-sqlite"]["authority"]
    cold_authority = execution["checkpoints"]["limit-cold"]["authority"]
    _validate_open(
        memory,
        None,
        backend="memory",
        readable_format="2.6.0",
        migration_required=False,
        label=f"{label}/memory",
    )
    for phase, opened, authority in (
        ("sqlite", sqlite, sqlite_authority),
        ("cold", cold, cold_authority),
    ):
        _validate_open(
            opened,
            authority,
            backend="sqlite",
            readable_format="3.0.0",
            migration_required=False,
            label=f"{label}/{phase}",
        )
    receipts = require_object(
        execution["child"].get("production_receipts"), f"{label} receipts"
    )
    limit_receipt = require_object(
        receipts.get("sqlite_limit_length"), f"{label} SQLite limit receipt"
    )
    actual_limit = require_uint64(
        limit_receipt.get("minimum_actual_limit"), f"{label} actual SQLite limit"
    )
    if (
        limit_receipt.get("requested_limit_length") != 16_777_216
        or limit_receipt.get("maximum_actual_limit") != actual_limit
        or require_uint64(
            limit_receipt.get("observed_connection_count"),
            f"{label} observed connection count",
        )
        == 0
        or limit_receipt.get("all_actual_limits_exact") is not True
        or limit_receipt.get("observation_count_overflow") is not False
        or actual_limit != 16_777_216
    ):
        fail(f"{label} production SQLite limit receipt is not exact")
    resident_receipt = require_object(
        receipts.get("payload_resident"), f"{label} payload resident receipt"
    )
    resident = resident_receipt.get("maximum_resident_payload_buffer_bytes")
    if type(resident) is not int:
        raise ProductionReceiptUnavailable(
            f"{label}: production maximum resident payload-buffer receipt is unavailable; "
            "add a source-private operation scope that aggregates high-water marks from the "
            "actual sqlite_payload_chunk_framer and sqlite_validating_payload_source instances"
        )
    if (
        resident < 1
        or resident > CHUNK_MAXIMUM_BYTES
        or require_uint64(
            resident_receipt.get("observation_count"),
            f"{label} payload resident observation count",
        )
        == 0
        or require_uint64(
            resident_receipt.get("chunk_framer_instance_count"),
            f"{label} payload framer instance count",
        )
        == 0
        or require_uint64(
            resident_receipt.get("validating_source_instance_count"),
            f"{label} validating source instance count",
        )
        == 0
        or resident_receipt.get("observation_count_overflow") is not False
    ):
        fail(f"{label} production resident payload receipt exceeds the closed bound")
    inventory = cold_authority["chunk_inventory"]
    publications = inventory["publications"]
    logical_payload_bytes = checked_sum(
        [row["payload_byte_count"] for row in publications],
        f"{label} logical payload bytes",
    )
    if len(publications) != 1:
        fail(f"{label} bounded-large scenario did not create exactly one publication")
    if sqlite_authority != cold_authority:
        fail(f"{label} cold reopen changed independently observed SQLite authority")
    return _common_raw(
        configuration,
        CASE_IDS[3],
        BACKEND_SCOPES[3],
        execution,
        {
            "sqlite_limit_length_bytes": actual_limit,
            "logical_payload_bytes": logical_payload_bytes,
            "canonical_v5_payload_digest": publications[0]["payload_checksum"],
            "maximum_resident_payload_buffer_bytes": resident,
            "memory": _parity_projection(memory),
            "sqlite": _parity_projection(sqlite),
            "cold_reopen_sqlite": _parity_projection(cold),
        },
    )


RAW_BUILDERS: dict[str, Callable[[str, dict[str, Any]], dict[str, Any]]] = {
    CASE_IDS[0]: build_current_raw,
    CASE_IDS[1]: build_v2_raw,
    CASE_IDS[2]: build_migration_raw,
    CASE_IDS[3]: build_limit_raw,
}


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
        fail(f"{label} has an unknown chunk profile")
    by_key = {
        (row["publication_id"], row["generation"]): [] for row in publications
    }
    if len(by_key) != len(publications):
        fail(f"{label} has duplicate publication inventory keys")
    for chunk in chunks:
        key = (chunk["publication_id"], chunk["generation"])
        if key not in by_key:
            fail(f"{label} has an orphan inventory chunk")
        by_key[key].append(chunk)
    zero_payload_count = 0
    nonfinal_count = 0
    final_count = 0
    for publication in publications:
        key = (publication["publication_id"], publication["generation"])
        payload_bytes = publication["payload_byte_count"]
        expected_count = (
            0
            if payload_bytes == 0
            else 1 + (payload_bytes - 1) // CHUNK_MAXIMUM_BYTES
        )
        rows = by_key[key]
        if publication["payload_chunk_count"] != expected_count or len(rows) != expected_count:
            fail(f"{label} chunk census differs from its publication declaration")
        if payload_bytes == 0:
            zero_payload_count += 1
        else:
            nonfinal_count += max(0, len(rows) - 1)
            final_count += 1
    return {
        "profile": profile,
        "chunked_publication_count": len(publications),
        "zero_payload_publication_count": zero_payload_count,
        "logical_payload_bytes": checked_sum(
            [row["payload_byte_count"] for row in publications],
            f"{label} logical payload census",
        ),
        "declared_chunk_count": checked_sum(
            [row["payload_chunk_count"] for row in publications],
            f"{label} declared chunk census",
        ),
        "observed_chunk_row_count": len(chunks),
        "observed_chunk_bytes": checked_sum(
            [row["byte_count"] for row in chunks], f"{label} observed chunk bytes"
        ),
        "maximum_chunk_bytes_observed": max(
            (row["byte_count"] for row in chunks), default=0
        ),
        "nonfinal_chunk_row_count": nonfinal_count,
        "final_chunk_row_count": final_count,
    }


def operational_measurement(raw: dict[str, Any], label: str) -> dict[str, Any]:
    process = raw["process"]
    disk = raw["disk"]
    if process["os_peak_rss_bytes"] != process["peak_rss_kib"] * 1024:
        fail(f"{label} peak RSS conversion differs")
    if disk["total_bytes"] != sum(
        disk[f"{role}_bytes"] for role in ROLE_SUFFIXES
    ):
        fail(f"{label} disk total differs from its file-family census")
    census = chunk_census(raw["chunk_inventory"], label)
    if (
        census["profile"] == "cxxlens.sqlite-payload-chunks.v1"
        and disk["total_bytes"] < census["observed_chunk_bytes"]
    ):
        fail(f"{label} disk bytes are smaller than observed chunk bytes")
    return {
        "method": METHOD,
        "os_peak_rss_bytes": process["os_peak_rss_bytes"],
        "wall_elapsed_ns": process["wall_elapsed_ns"],
        "disk_bytes": disk,
        "chunk_census": census,
    }


def build_measurement(raw: dict[str, Any], label: str) -> dict[str, Any]:
    operational = operational_measurement(raw, label)
    observations = raw["observations"]
    if raw["case_id"] == CASE_IDS[0]:
        if observations["initial_open"] != observations["cold_reopen"]:
            fail(f"{label} current-v3 cold-reopen projection differs")
        value = observations["cold_reopen"]
        return {
            "operational": operational,
            "physical_format": value["physical_format"],
            "readable_format": value["readable_format"],
            "metadata_digest": value["metadata_digest"],
            "canonical_ddl_digest": value["canonical_ddl_digest"],
            "chunk_profile": operational["chunk_census"]["profile"],
            "cold_reopen_validated": True,
            "semantic_projection_digest": value["semantic_projection_digest"],
        }
    if raw["case_id"] == CASE_IDS[1]:
        before = observations["before"]
        after = observations["after"]
        main_wal_journal_unchanged = all(
            before[role] == after[role] for role in ("main", "wal", "journal")
        )
        shm_unchanged = (before["shm"]["state"], before["shm"]["byte_count"]) == (
            after["shm"]["state"],
            after["shm"]["byte_count"],
        )
        sidecars_stable = all(
            (before[role]["state"], before[role]["byte_count"])
            == (after[role]["state"], after[role]["byte_count"])
            for role in ("wal", "shm", "journal")
        )
        directory_unchanged = before["directory_entries"] == after["directory_entries"]
        if not (
            main_wal_journal_unchanged
            and shm_unchanged
            and sidecars_stable
            and directory_unchanged
        ):
            fail(f"{label} exact-v2 read-only route changed its file family")
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
    if raw["case_id"] == CASE_IDS[2]:
        source = observations["source"]
        target = observations["target"]
        cold = observations["cold_reopen"]
        semantic_equal = len(
            {
                source["semantic_projection_digest"],
                target["semantic_projection_digest"],
                cold["semantic_projection_digest"],
            }
        ) == 1
        non_generation_equal = len(
            {
                source["non_generation_payload_projection_digest"],
                target["non_generation_payload_projection_digest"],
                cold["non_generation_payload_projection_digest"],
            }
        ) == 1
        if not semantic_equal or not non_generation_equal:
            fail(f"{label} migration changed a semantic/non-generation projection")
        generation_evidence = migration_generation_evidence(
            source["generation_census"],
            target["generation_census"],
            cold["generation_census"],
            label,
        )
        payload_evidence = migration_payload_evidence(source, target, cold, label)
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
            "semantic_projection_equal": semantic_equal,
            "physical_generation_only_payload_transition": (
                non_generation_equal
                and generation_evidence[
                    "contiguous_committed_generation_replacement"
                ]
            ),
            **generation_evidence,
            **payload_evidence,
            "canonical_ddl_digest": observations["canonical_ddl_digest"],
            "cold_reopen_validated": (
                target["semantic_projection_digest"]
                == cold["semantic_projection_digest"]
            ),
        }
    memory = observations["memory"]
    sqlite = observations["sqlite"]
    cold = observations["cold_reopen_sqlite"]
    semantic_equal = len(
        {
            memory["semantic_projection_digest"],
            sqlite["semantic_projection_digest"],
            cold["semantic_projection_digest"],
        }
    ) == 1
    export_equal = len(
        {
            memory["canonical_export_digest"],
            sqlite["canonical_export_digest"],
            cold["canonical_export_digest"],
        }
    ) == 1
    query_equal = len(
        {
            memory["query_projection_digest"],
            sqlite["query_projection_digest"],
            cold["query_projection_digest"],
        }
    ) == 1
    if not semantic_equal or not export_equal or not query_equal:
        fail(f"{label} memory/SQLite/cold-reopen projection differs")
    resident = observations["maximum_resident_payload_buffer_bytes"]
    return {
        "operational": operational,
        "sqlite_limit_length_bytes": observations["sqlite_limit_length_bytes"],
        "logical_payload_bytes": observations["logical_payload_bytes"],
        "payload_exceeds_sqlite_limit_length": (
            observations["logical_payload_bytes"]
            > observations["sqlite_limit_length_bytes"]
        ),
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
            sqlite["semantic_projection_digest"] == cold["semantic_projection_digest"]
        ),
    }


def seal_case(configuration: str, raw: dict[str, Any]) -> dict[str, Any]:
    case = {
        "id": raw["case_id"],
        "backend_scope": raw["backend_scope"],
        "result": "passed",
        "raw_evidence": raw,
        "raw_evidence_digest": document_digest(raw),
        "evidence_digest": "",
        "measurement": build_measurement(
            raw, f"{configuration}/{raw['case_id']}"
        ),
    }
    evidence_projection = {
        "schema": "cxxlens.sqlite-store-v3-case-evidence.v1",
        "configuration": configuration,
        "case_id": case["id"],
        "backend_scope": case["backend_scope"],
        "result": case["result"],
        "raw_evidence_digest": case["raw_evidence_digest"],
        "measurement": case["measurement"],
    }
    case["evidence_digest"] = document_digest(evidence_projection)
    return case


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
        keys = (
            "physical_format",
            "readable_format",
            "metadata_digest",
            "canonical_ddl_digest",
            "chunk_profile",
            "semantic_projection_digest",
        )
        return {key: measurement[key] for key in keys}
    if case["id"] == CASE_IDS[1]:
        keys = (
            "physical_format",
            "reported_readable_format",
            "direct_open",
            "migration_required",
            "semantic_projection_digest",
            "begin_result",
        )
        return {key: measurement[key] for key in keys}
    if case["id"] == CASE_IDS[2]:
        keys = (
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
        return {key: measurement[key] for key in keys}
    observations = case["raw_evidence"]["observations"]
    return {
        "sqlite_limit_length_bytes": measurement["sqlite_limit_length_bytes"],
        "logical_payload_bytes": measurement["logical_payload_bytes"],
        "canonical_v5_payload_digest": measurement["canonical_v5_payload_digest"],
        "memory": observations["memory"],
        "sqlite": observations["sqlite"],
        "cold_reopen_sqlite": observations["cold_reopen_sqlite"],
    }


def _validate_runner(path: pathlib.Path, label: str) -> tuple[pathlib.Path, tuple[int, int]]:
    try:
        resolved = path.resolve(strict=True)
        value = resolved.stat()
    except OSError as error:
        fail(f"{label} runner is unavailable: {error}")
    if not stat.S_ISREG(value.st_mode) or not os.access(resolved, os.X_OK):
        fail(f"{label} runner is not an executable regular file")
    return resolved, (value.st_dev, value.st_ino)


def write_new_atomic(path: pathlib.Path, value: dict[str, Any]) -> None:
    if path.exists():
        fail(f"qualification output already exists: {path}")
    if not path.parent.is_dir():
        fail(f"qualification output parent does not exist: {path.parent}")
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = pathlib.Path(temporary_name)
    try:
        payload = canonical_json(value) + b"\n"
        offset = 0
        while offset < len(payload):
            offset += os.write(descriptor, payload[offset:])
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = -1
        try:
            os.link(temporary, path)
        except FileExistsError:
            fail(f"qualification output appeared concurrently: {path}")
        temporary.unlink()
        directory_fd = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def produce(arguments: argparse.Namespace) -> dict[str, Any]:
    if sys.platform != "linux" or not hasattr(os, "wait4"):
        fail("SQLite Store v3 qualification production requires Linux wait4")
    root = arguments.root.resolve(strict=True)
    if REVISION_PATTERN.fullmatch(arguments.revision) is None:
        fail("--revision must be exactly 40 lowercase hexadecimal digits")
    if REVISION_PATTERN.fullmatch(arguments.source_tree) is None:
        fail("--source-tree must be exactly 40 lowercase hexadecimal digits")
    validate_exact_clean_checkout(root, arguments.revision, arguments.source_tree)
    static_runner, static_identity = _validate_runner(
        arguments.static_runner, "static"
    )
    shared_runner, shared_identity = _validate_runner(
        arguments.shared_runner, "shared"
    )
    if static_identity == shared_identity:
        fail("static and shared qualification runners designate the same file instance")
    work_directory = arguments.work_directory
    if work_directory.exists():
        fail(f"qualification work directory already exists: {work_directory}")
    if not work_directory.parent.is_dir():
        fail(f"qualification work-directory parent does not exist: {work_directory.parent}")
    work_directory.mkdir()

    schema = load_yaml(root / REPORT_SCHEMA)
    schema_digest = document_digest(schema)
    if schema_digest != EXPECTED_SCHEMA_DIGEST:
        fail(
            "SQLite qualification report schema digest drifted: "
            f"expected={EXPECTED_SCHEMA_DIGEST}, actual={schema_digest}"
        )
    sqlite_contract_digest = document_digest(load_yaml(root / SQLITE_CONTRACT))
    runners = {"static": static_runner, "shared": shared_runner}
    executions: dict[tuple[str, str], dict[str, Any]] = {}
    for configuration in CONFIGURATIONS:
        configuration_directory = work_directory / configuration
        configuration_directory.mkdir()
        for index, case_id in enumerate(CASE_IDS):
            executions[(configuration, case_id)] = run_case(
                runners[configuration],
                configuration,
                case_id,
                configuration_directory / f"{index + 1}-{case_id}",
                arguments.revision,
                arguments.source_tree,
            )

    configurations: list[dict[str, Any]] = []
    unavailable: list[str] = []
    for configuration in CONFIGURATIONS:
        cases: list[dict[str, Any]] = []
        for case_id in CASE_IDS:
            try:
                raw = RAW_BUILDERS[case_id](
                    configuration, executions[(configuration, case_id)]
                )
                cases.append(seal_case(configuration, raw))
            except ProductionReceiptUnavailable as error:
                unavailable.append(str(error))
        configurations.append(
            {
                "configuration": configuration,
                "backends": ["memory", "sqlite"],
                "cases": cases,
            }
        )
    if unavailable:
        raise ProductionReceiptUnavailable(
            "required production-bound receipts are unavailable; no green report was written:\n- "
            + "\n- ".join(unavailable)
        )

    for index, static_case in enumerate(configurations[0]["cases"]):
        shared_case = configurations[1]["cases"][index]
        if _cross_configuration_projection(static_case) != (
            _cross_configuration_projection(shared_case)
        ):
            fail(f"static/shared semantic projection differs for {static_case['id']}")
    report = {
        "schema": "cxxlens.ng-sqlite-store-v3-qualification-report.v1",
        "report_schema_digest": schema_digest,
        "revision": arguments.revision,
        "source_tree": arguments.source_tree,
        "sqlite_contract_digest": sqlite_contract_digest,
        "configurations": configurations,
        "report_set_digest": "",
        "status": "green",
    }
    report["report_set_digest"] = document_digest(report_set_projection(report))
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(report)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"produced qualification report schema validation failed: {error.message}")
    validate_exact_clean_checkout(root, arguments.revision, arguments.source_tree)
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("produce",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--static-runner", type=pathlib.Path, required=True)
    parser.add_argument("--shared-runner", type=pathlib.Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--source-tree", required=True)
    parser.add_argument("--work-directory", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    try:
        report = produce(arguments)
        write_new_atomic(arguments.output, report)
    except QualificationError as error:
        print(f"SQLite Store v3 qualification production failed: {error}", file=sys.stderr)
        return 1
    print(
        "produced SQLite Store v3 qualification report: "
        f"{arguments.revision}; {report['report_set_digest']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
