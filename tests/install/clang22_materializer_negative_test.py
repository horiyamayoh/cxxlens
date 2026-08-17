#!/usr/bin/env python3
"""Qualify installed Clang 22 phase-authentic negative responses.

The cases are deliberately limited to failures that can be induced through the
public request boundary without corrupting an installed prefix or synthesizing
Store state.  The first case stops before request binding.  The second executes
the real provider path and asks for a non-genesis publication against a fresh
Store, so the SDK's current-not-found observation is authoritative.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import sqlite3
import subprocess
import sys
import tempfile
from typing import Any


BASELINE_POLICY_DIGEST = (
    "semantic-v2:sha256:"
    "b4e95d8c88cf660fff40c4d9e7e4ae07bcb078013b5370c6b1abb80b0d75d375"
)
# The request schema's canonical integer domain is signed int64.  This value is
# used only when a sanitizer process needs to reserve its shadow address range.
ASAN_ADDRESS_SPACE_BYTES = (1 << 63) - 1
# Keep the sanitizer-only worker process allowance explicit. Normal and release
# requests retain the production subprocess budget from the canonical fixture.
ASAN_SUBPROCESS_BUDGET = 1024
OCCURRENCE_RELATIVE_PATH = (
    "share/cxxlens/materialization/clang22/occurrence-v1.json"
)
MATERIALIZATION_DATABASE_FILENAME = "materialization.sqlite"
EXECUTION_RECEIPT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_clang22_materialization_execution_receipt.schema.yaml"
)
NEGATIVE_REPORT_FILENAME = "report.json"
NEGATIVE_RECEIPT_FILENAME = "execution-receipt.json"
NEGATIVE_MANIFEST_FILENAME = "evidence-manifest.json"
NEGATIVE_INPUT_FILENAME = "stdin.bin"
NEGATIVE_STDERR_FILENAME = "stderr.bin"
NEGATIVE_EVIDENCE_SCHEMA = "cxxlens.clang22-installed-negative-evidence.v1"


def fail(message: str) -> None:
    raise AssertionError(message)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--prefix", required=True, type=pathlib.Path)
    parser.add_argument(
        "--executable-suffix",
        default="",
        help=(
            "configured CMake executable suffix used to resolve the installed "
            "materializer; manifest paths remain canonical"
        ),
    )
    parser.add_argument(
        "--evidence-dir",
        type=pathlib.Path,
        help="optional directory for exact raw-input, stdout, stderr, and receipt evidence",
    )
    return parser.parse_args()


def installed_request(
    root: pathlib.Path,
    prefix: pathlib.Path,
    oracle: Any,
) -> dict[str, Any]:
    occurrence_path = prefix / OCCURRENCE_RELATIVE_PATH
    if not occurrence_path.is_file():
        fail(f"installed occurrence manifest is missing: {occurrence_path}")
    occurrence_bytes = occurrence_path.read_bytes()
    occurrence = json.loads(occurrence_bytes)
    try:
        oracle.validate_occurrence_manifest(root, occurrence)
    except oracle.MaterializationError as error:
        fail(f"installed occurrence manifest failed independent validation: {error}")
    files = occurrence["files"]
    tool = next(file for file in files if file["role"] == "materializer-executable")
    worker = next(file for file in files if file["role"] == "worker-executable")
    configuration = occurrence["package_configuration"]
    if configuration not in {"static", "shared"}:
        fail(f"installed occurrence configuration is not closed: {configuration!r}")

    request = oracle.sample_request(
        root,
        configuration=configuration,
        backend="sqlite",
        translation_unit_count=1,
    )
    request["tool"].update(
        source_revision=occurrence["source_revision"],
        source_tree=occurrence["source_tree"],
        installed_executable_digest=tool["digest"],
        occurrence_manifest_digest=oracle.content_digest(occurrence_bytes),
    )
    request["worker"].update(
        installed_binary_digest=worker["digest"],
        sandbox_policy_digest=BASELINE_POLICY_DIGEST,
    )
    if os.environ.get("CXXLENS_ASAN_INSTALLED_QUALIFICATION") == "1":
        # AddressSanitizer reserves a platform shadow range before the worker
        # reaches main(); bind the explicit sanitizer profile to a maximum
        # address-space value. Normal and release requests keep finite RLIMIT_AS.
        for task in request["tasks"]:
            task["budget"]["address_space_bytes"] = ASAN_ADDRESS_SPACE_BYTES
            task["budget"]["subprocesses"] = ASAN_SUBPROCESS_BUDGET
    for task in request["tasks"]:
        task["sandbox"]["policy_digest"] = BASELINE_POLICY_DIGEST
    oracle.bind_provider_task_identities(request)
    oracle.bind_task_execution_identities(request)
    oracle.bind_engine_policy_and_selector_identities(request)
    oracle.bind_request_identity(request)
    oracle.validate_request(root, request)
    return request


def run_materializer(
    materializer: pathlib.Path,
    payload: bytes,
    working_directory: pathlib.Path,
) -> subprocess.CompletedProcess[bytes]:
    environment = dict(os.environ)
    environment.pop("LD_LIBRARY_PATH", None)
    environment.pop("DYLD_LIBRARY_PATH", None)
    return subprocess.run(
        [str(materializer)],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=working_directory,
        env=environment,
        check=False,
    )


def assert_compact_response(
    root: pathlib.Path,
    oracle: Any,
    completed: subprocess.CompletedProcess[bytes],
    payload: bytes,
    request: dict[str, Any] | None,
    store_failure_authority: dict[str, Any] | None = None,
) -> dict[str, Any]:
    if completed.returncode != 1:
        fail(
            "installed negative path did not return schema-valid failure status: "
            f"{completed.returncode}, stdout={completed.stdout[:2000]!r}, "
            f"stderr={completed.stderr[:2000]!r}"
        )
    if completed.stderr:
        fail(f"installed negative path wrote stderr: {completed.stderr[:2000]!r}")
    try:
        report: dict[str, Any] = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        fail(f"installed negative path did not emit JSON: {error}")
    oracle.validate_report(
        root,
        request,
        report,
        request_bytes=payload,
        store_failure_authority=store_failure_authority,
    )
    if report["response_kind"] != "compact_failure" or report["result"] != "failed":
        fail("installed negative path did not return compact_failure/failed")
    if report["process_exit_status"] != 1:
        fail("installed negative report lost the process exit status")
    if completed.stdout != oracle.canonical_json(report) + b"\n":
        fail("installed negative response is not the canonical report artifact")
    return report


def make_execution_receipt(
    root: pathlib.Path,
    oracle: Any,
    completed: subprocess.CompletedProcess[bytes],
    label: str,
) -> bytes:
    """Bind the externally observed process result without using report prose."""

    parsed_response_count = 0
    if completed.stdout:
        try:
            oracle.load_strict_json_bytes(completed.stdout, f"{label} stdout")
        except oracle.MaterializationError:
            parsed_response_count = 0
        else:
            parsed_response_count = 1
    receipt = {
        "schema": "cxxlens.clang22-materialization-execution-receipt.v1",
        "actual_exit_status": completed.returncode,
        "exact_stdout_byte_count": len(completed.stdout),
        "stdout_sha256": "sha256:" + hashlib.sha256(completed.stdout).hexdigest(),
        "parsed_response_count": parsed_response_count,
        "stderr_sha256": "sha256:" + hashlib.sha256(completed.stderr).hexdigest(),
    }
    oracle.validate_schema(
        receipt,
        oracle.load(root / EXECUTION_RECEIPT_SCHEMA),
        f"{label} external execution receipt",
        error_code="materialization.report-invalid",
    )
    return oracle.canonical_json(receipt)


def negative_source_identity(request: dict[str, Any]) -> dict[str, Any]:
    """Project the installed identity without adding a public report field."""

    return {
        "revision": request["tool"]["source_revision"],
        "tree": request["tool"]["source_tree"],
        "package_configuration": request["tool"]["package_configuration"],
        "occurrence_manifest_digest": request["tool"][
            "occurrence_manifest_digest"
        ],
        "materializer_digest": request["tool"]["installed_executable_digest"],
        "worker_digest": request["worker"]["installed_binary_digest"],
    }


def evidence_artifact(
    oracle: Any,
    relative_path: str,
    value: bytes,
) -> dict[str, Any]:
    return {
        "path": relative_path,
        "byte_count": len(value),
        "sha256": oracle.content_digest(value),
    }


def evidence_binding_state(report: dict[str, Any]) -> str:
    """Classify report binding without manufacturing a missing compact binding."""

    response_kind = report.get("response_kind")
    if response_kind == "detailed":
        if "binding" in report:
            fail("detailed evidence report unexpectedly retained compact binding")
        return "detailed"
    if response_kind != "compact_failure":
        fail("negative evidence report kind is outside the closed report union")
    if "binding" not in report:
        return "unbound"
    binding = report["binding"]
    if not isinstance(binding, dict):
        fail("compact evidence report binding is not an object")
    state = binding.get("state")
    if state not in {"raw-input-only", "request-bound"}:
        fail("compact evidence report binding state is outside the closed set")
    return state


def detailed_source_identity(
    request: dict[str, Any],
    report: dict[str, Any],
) -> dict[str, Any]:
    """Cross-bind a detailed report's own source/install projection to its input."""

    expected = negative_source_identity(request)
    try:
        measured = report["installation"]["measured"]
        actual = {
            "revision": report["source"]["revision"],
            "tree": report["source"]["tree"],
            "package_configuration": measured["configuration"],
            "occurrence_manifest_digest": measured["manifest_file_digest"],
            "materializer_digest": measured["tool"]["digest"],
            "worker_digest": measured["worker"]["digest"],
        }
    except (KeyError, TypeError) as error:
        fail(f"detailed evidence report lacks exact source identity: {error}")
    if actual != expected:
        fail("detailed evidence report source identity differs from the request")
    expected_request = {
        "materialization_request_id": request["materialization_request_id"],
        "request_digest": request["request_digest"],
        "semantic_request_digest": request["semantic_request_digest"],
    }
    if report.get("request") != expected_request:
        fail("detailed evidence report request identity differs from the request")
    return expected


def validate_detailed_report_occurrence(
    root: pathlib.Path,
    oracle: Any,
    request: dict[str, Any],
    report: dict[str, Any],
) -> None:
    """Validate the report's measured installed occurrence against its request."""

    try:
        measured = report["installation"]["measured"]
    except (KeyError, TypeError) as error:
        fail(f"detailed evidence report lacks measured occurrence: {error}")
    oracle.validate_measured_occurrence(root, request, measured)


def report_store_projection(report: dict[str, Any]) -> dict[str, Any]:
    """Project compact effects or detailed publication Store evidence uniformly."""

    if "effects" in report:
        try:
            return {
                "head_observation": report["effects"]["head_observation"],
                "store_failure_cause": report["effects"]["store_failure_cause"],
            }
        except (KeyError, TypeError) as error:
            fail(f"compact evidence report lacks exact Store projection: {error}")
    try:
        publication = report["publication"]
        store_failure = publication["store_failure"]
        return {
            "head_observation": publication["head_observation"],
            "store_failure_cause": (
                None if store_failure is None else store_failure["cause"]
            ),
        }
    except (KeyError, TypeError) as error:
        fail(f"detailed evidence report lacks exact Store projection: {error}")


def build_negative_evidence_manifest(
    oracle: Any,
    request: dict[str, Any] | None,
    report: dict[str, Any],
    payload: bytes,
    completed: subprocess.CompletedProcess[bytes],
    receipt_bytes: bytes,
) -> bytes:
    """Build a test-only envelope joining report/receipt bytes to installed identity."""

    try:
        receipt: dict[str, Any] = json.loads(receipt_bytes)
    except json.JSONDecodeError as error:
        fail(f"negative execution receipt is not JSON: {error}")
    binding_state = evidence_binding_state(report)
    if binding_state == "detailed":
        if request is None:
            fail("detailed evidence requires its exact input request")
        source = detailed_source_identity(request, report)
        request_binding = {
            "materialization_request_id": request["materialization_request_id"],
            "request_digest": request["request_digest"],
            "semantic_request_digest": request["semantic_request_digest"],
        }
    elif binding_state == "request-bound":
        if request is None:
            fail("request-bound negative evidence lacks its input request")
        expected_binding = {
            "materialization_request_id": request["materialization_request_id"],
            "request_digest": request["request_digest"],
            "semantic_request_digest": request["semantic_request_digest"],
        }
        if report["binding"].get("request") != expected_binding:
            fail("negative report request binding differs from the input request")
        source = negative_source_identity(request)
        request_binding = expected_binding
    elif binding_state == "raw-input-only":
        if request is not None:
            fail("raw negative evidence was supplied with a request")
        source = None
        request_binding = None
    else:
        # A compact response without binding is an unauthenticated observation.
        # Keep its raw report/receipt, but never attribute it to the request or
        # installed source identity.
        source = None
        request_binding = None

    store_projection = report_store_projection(report)

    if receipt["actual_exit_status"] != completed.returncode:
        fail("negative evidence receipt exit status differs from the process")
    if receipt["exact_stdout_byte_count"] != len(completed.stdout):
        fail("negative evidence receipt stdout byte count differs")
    if receipt["stdout_sha256"] != oracle.content_digest(completed.stdout):
        fail("negative evidence receipt stdout digest differs")
    if receipt["stderr_sha256"] != oracle.content_digest(completed.stderr):
        fail("negative evidence receipt stderr digest differs")
    if receipt["parsed_response_count"] != 1:
        fail("negative evidence receipt must contain one parsed response")

    manifest = {
        "schema": NEGATIVE_EVIDENCE_SCHEMA,
        "binding_state": binding_state,
        "source": source,
        "request": request_binding,
        "report": {
            "path": NEGATIVE_REPORT_FILENAME,
            "sha256": oracle.content_digest(completed.stdout),
            "response_kind": report["response_kind"],
            "result": report["result"],
            "error": report["error"],
            "head_observation": store_projection["head_observation"],
            "store_failure_cause": store_projection["store_failure_cause"],
        },
        "execution_receipt": {
            "path": NEGATIVE_RECEIPT_FILENAME,
            "sha256": oracle.content_digest(receipt_bytes),
            "receipt": receipt,
        },
        "artifacts": {
            "input": evidence_artifact(oracle, NEGATIVE_INPUT_FILENAME, payload),
            "stderr": evidence_artifact(
                oracle, NEGATIVE_STDERR_FILENAME, completed.stderr
            ),
        },
        "qualification": {
            "negative_evidence_only": True,
            "native_positive_qualification": False,
            "release_qualification": False,
        },
    }
    return oracle.canonical_json(manifest)


def validate_negative_evidence_manifest(
    oracle: Any,
    request: dict[str, Any] | None,
    report: dict[str, Any],
    payload: bytes,
    completed: subprocess.CompletedProcess[bytes],
    receipt_bytes: bytes,
    manifest_bytes: bytes,
) -> None:
    """Reject any evidence envelope that can drift from its exact artifacts."""

    try:
        manifest = json.loads(manifest_bytes)
        receipt = json.loads(receipt_bytes)
    except json.JSONDecodeError as error:
        fail(f"negative evidence manifest is not JSON: {error}")
    if manifest["schema"] != NEGATIVE_EVIDENCE_SCHEMA:
        fail("negative evidence manifest schema differs")
    if manifest["report"]["path"] != NEGATIVE_REPORT_FILENAME:
        fail("negative evidence report path differs from canonical filename")
    if manifest["execution_receipt"]["path"] != NEGATIVE_RECEIPT_FILENAME:
        fail(
            "negative evidence execution receipt path differs from canonical filename"
        )
    binding_state = evidence_binding_state(report)
    if manifest["binding_state"] != binding_state:
        fail("negative evidence binding state differs from report")
    if binding_state == "detailed":
        if request is None:
            fail("detailed evidence requires its exact input request")
        expected_source = detailed_source_identity(request, report)
    elif binding_state == "request-bound":
        if request is None:
            fail("request-bound negative evidence lacks its input request")
        expected_source = negative_source_identity(request)
    else:
        expected_source = None
    if manifest["source"] != expected_source:
        fail("negative evidence source identity differs from the request")
    expected_request = (
        None
        if binding_state not in {"detailed", "request-bound"}
        else {
            # Detailed reports have no compact binding field, but their exact
            # top-level request projection is still independently validated.
            "materialization_request_id": request["materialization_request_id"],
            "request_digest": request["request_digest"],
            "semantic_request_digest": request["semantic_request_digest"],
        }
    )
    if manifest["request"] != expected_request:
        fail("negative evidence request binding differs")
    if manifest["report"]["sha256"] != oracle.content_digest(completed.stdout):
        fail("negative evidence report digest differs")
    if manifest["report"]["response_kind"] != report["response_kind"]:
        fail("negative evidence response kind differs")
    if manifest["report"]["result"] != report["result"]:
        fail("negative evidence result differs")
    if manifest["report"]["error"] != report["error"]:
        fail("negative evidence error projection differs")
    store_projection = report_store_projection(report)
    if (
        manifest["report"]["head_observation"]
        != store_projection["head_observation"]
        or manifest["report"]["store_failure_cause"]
        != store_projection["store_failure_cause"]
    ):
        fail("negative evidence Store outcome projection differs")
    if manifest["execution_receipt"]["sha256"] != oracle.content_digest(
        receipt_bytes
    ):
        fail("negative evidence execution receipt digest differs")
    if manifest["execution_receipt"]["receipt"] != receipt:
        fail("negative evidence execution receipt projection differs")
    if manifest["artifacts"]["input"] != evidence_artifact(
        oracle, NEGATIVE_INPUT_FILENAME, payload
    ):
        fail("negative evidence input artifact differs")
    if manifest["artifacts"]["stderr"] != evidence_artifact(
        oracle, NEGATIVE_STDERR_FILENAME, completed.stderr
    ):
        fail("negative evidence stderr artifact differs")
    if manifest["qualification"] != {
        "negative_evidence_only": True,
        "native_positive_qualification": False,
        "release_qualification": False,
    }:
        fail("negative evidence manifest must remain non-qualifying")


def write_negative_evidence(
    oracle: Any,
    evidence_dir: pathlib.Path,
    case_name: str,
    payload: bytes,
    completed: subprocess.CompletedProcess[bytes],
    receipt_bytes: bytes,
    request: dict[str, Any] | None,
    report: dict[str, Any],
) -> None:
    destination = (evidence_dir / case_name).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    files = {
        NEGATIVE_INPUT_FILENAME: payload,
        NEGATIVE_REPORT_FILENAME: completed.stdout,
        NEGATIVE_STDERR_FILENAME: completed.stderr,
        NEGATIVE_RECEIPT_FILENAME: receipt_bytes,
    }
    for filename, value in files.items():
        path = destination / filename
        path.write_bytes(value)
        if path.read_bytes() != value:
            fail(f"negative evidence artifact was not written exactly: {filename}")
    manifest_bytes = build_negative_evidence_manifest(
        oracle,
        request,
        report,
        payload,
        completed,
        receipt_bytes,
    )
    validate_negative_evidence_manifest(
        oracle,
        request,
        report,
        payload,
        completed,
        receipt_bytes,
        manifest_bytes,
    )
    manifest_path = destination / NEGATIVE_MANIFEST_FILENAME
    manifest_path.write_bytes(manifest_bytes)
    stored_manifest_bytes = manifest_path.read_bytes()
    validate_negative_evidence_manifest(
        oracle,
        request,
        report,
        payload,
        completed,
        receipt_bytes,
        stored_manifest_bytes,
    )


def assert_raw_failure(
    root: pathlib.Path,
    oracle: Any,
    completed: subprocess.CompletedProcess[bytes],
    payload: bytes,
    case_name: str,
    expected_phase: str,
    expected_code: str,
    evidence_dir: pathlib.Path | None,
) -> dict[str, Any]:
    report = assert_compact_response(root, oracle, completed, payload, None)
    binding_state = evidence_binding_state(report)
    if (
        binding_state != "raw-input-only"
        or report["error"]["phase"] != expected_phase
        or report["error"]["code"] != expected_code
        or report["effects"]["store_draft_state"] != "not-created"
        or report["effects"]["head_observation"] != "not-observed"
        or report["effects"]["publication_attempted"]
        or report["effects"]["committed_transaction_count"] != 0
        or report["effects"]["task_attempt_count"] != 0
        or report["effects"]["task_success_count"] != 0
        or report["effects"]["worker_launch_attempt_count"] != 0
        or report["effects"]["worker_launch_success_count"] != 0
    ):
        fail(f"{case_name} crossed the raw binding or publication boundary")
    receipt_bytes = make_execution_receipt(root, oracle, completed, case_name)
    if evidence_dir is not None:
        write_negative_evidence(
            oracle,
            evidence_dir,
            case_name,
            payload,
            completed,
            receipt_bytes,
            None,
            report,
        )
    return report


def prepare_sqlite_baseline(
    root: pathlib.Path,
    materializer: pathlib.Path,
    oracle: Any,
    prefix: pathlib.Path,
    evidence_dir: pathlib.Path | None,
) -> tuple[pathlib.Path, dict[str, Any], dict[str, Any]]:
    """Create one real installed Store used as the corruption source.

    The negative matrix must exercise the installed materializer against an
    authenticated Store artifact.  It therefore starts with a genuine
    one-task genesis publication, then copies that database into disposable
    case directories before applying one bounded SQLite corruption vector.
    Nothing under the installed prefix is modified.
    """

    request = installed_request(root, prefix, oracle)
    request["publication"]["genesis"] = True
    request["publication"]["expected_parent_publication"] = None
    request["publication"]["sqlite_path"] = MATERIALIZATION_DATABASE_FILENAME
    oracle.bind_request_identity(request)
    oracle.validate_request(root, request)
    request_payload = oracle.canonical_json(request)

    work = pathlib.Path(
        tempfile.mkdtemp(prefix="clang22-materializer-negative-baseline-")
    )
    completed = run_materializer(materializer, request_payload, work)
    if completed.returncode != 0 or completed.stderr:
        fail(
            "installed baseline Store publication failed: "
            f"returncode={completed.returncode}, stdout={completed.stdout[:2000]!r}, "
            f"stderr={completed.stderr[:2000]!r}"
        )
    try:
        report: dict[str, Any] = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        fail(f"installed baseline Store publication did not emit JSON: {error}")
    oracle.validate_schema(
        report,
        oracle.load(root / oracle.REPORT_SCHEMA),
        "installed baseline Store publication report",
        error_code="materialization.report-invalid",
    )
    validate_detailed_report_occurrence(root, oracle, request, report)
    if (
        report["response_kind"] != "detailed"
        or report["result"] != "passed"
        or report["process_exit_status"] != 0
        or report["raw_input_observation"]
        != oracle.raw_input_observation(request_payload)
        or report["request"] != oracle._request_binding(request)
        or report["publication"]["outcome"] != "committed_verified"
        or report["publication"]["committed_transaction_count"] != 1
    ):
        fail(
            "installed baseline Store publication was not a committed verified "
            "genesis"
        )
    database = work / MATERIALIZATION_DATABASE_FILENAME
    if not database.is_file():
        fail(f"installed baseline Store did not create {database}")
    if evidence_dir is not None:
        receipt_bytes = make_execution_receipt(
            root, oracle, completed, "store-head-corruption-baseline"
        )
        write_negative_evidence(
            oracle,
            evidence_dir,
            "store-head-corruption-baseline",
            request_payload,
            completed,
            receipt_bytes,
            request,
            report,
        )
    return database, request, report


def corrupt_sqlite_payload(
    database: pathlib.Path,
    mode: str,
) -> str:
    """Apply one deterministic, bounded corruption to a disposable Store copy."""

    connection = sqlite3.connect(database)
    try:
        row = connection.execute(
            "SELECT publication_id, generation, chunk_ordinal, payload "
            "FROM cxxlens_ng_payload_chunk "
            "ORDER BY publication_id, generation, chunk_ordinal LIMIT 1"
        ).fetchone()
        if row is None:
            fail(f"{mode} fixture has no persisted payload chunk")
        publication_id, generation, chunk_ordinal, payload = row
        if not isinstance(publication_id, str) or not isinstance(payload, bytes):
            fail(f"{mode} fixture payload authority is not typed")
        if mode == "payload-byte-flip":
            if not payload:
                fail("payload-byte-flip fixture has an empty payload")
            corrupted = bytearray(payload)
            corrupted[0] ^= 1
            connection.execute(
                "UPDATE cxxlens_ng_payload_chunk SET payload=? "
                "WHERE publication_id=? AND generation=? AND chunk_ordinal=?",
                (
                    sqlite3.Binary(corrupted),
                    publication_id,
                    generation,
                    chunk_ordinal,
                ),
            )
        elif mode == "payload-checksum":
            connection.execute(
                "UPDATE cxxlens_ng_publication SET payload_checksum=? "
                "WHERE publication_id=?",
                ("sha256:" + "0" * 64, publication_id),
            )
        else:
            fail(f"unknown SQLite corruption mode: {mode}")
        connection.commit()

        remaining = connection.execute(
            "SELECT publication_id FROM cxxlens_ng_publication "
            "ORDER BY publication_id LIMIT 2"
        ).fetchall()
        if len(remaining) != 1 or remaining[0][0] != publication_id:
            fail(f"{mode} fixture changed the publication census")
    finally:
        connection.close()
    if any(
        database.with_name(database.name + suffix).exists()
        for suffix in ("-wal", "-shm")
    ):
        fail(f"{mode} fixture left active SQLite sidecars after corruption")
    return publication_id


def assert_corrupt_head_failure(
    root: pathlib.Path,
    oracle: Any,
    materializer: pathlib.Path,
    baseline_database: pathlib.Path,
    baseline_request: dict[str, Any],
    mode: str,
    evidence_dir: pathlib.Path | None,
) -> None:
    case_name = f"store-head-current-corrupt-{mode}"
    with tempfile.TemporaryDirectory(
        prefix=f"clang22-materializer-{case_name}-"
    ) as directory:
        work = pathlib.Path(directory)
        baseline_entries = sorted(baseline_database.parent.iterdir())
        if any(not entry.is_file() or entry.is_symlink() for entry in baseline_entries):
            fail(f"{case_name} baseline Store directory contains an unexpected entry")
        for entry in baseline_entries:
            shutil.copy2(entry, work / entry.name)
        database = work / MATERIALIZATION_DATABASE_FILENAME
        if not database.is_file():
            fail(f"{case_name} baseline Store copy lost the database")
        publication_id = corrupt_sqlite_payload(database, mode)

        request = json.loads(json.dumps(baseline_request))
        request["publication"]["genesis"] = False
        request["publication"]["expected_parent_publication"] = (
            "publication:missing-parent"
        )
        request["publication"]["sqlite_path"] = MATERIALIZATION_DATABASE_FILENAME
        oracle.bind_request_identity(request)
        oracle.validate_request(root, request)
        request_payload = oracle.canonical_json(request)
        completed = run_materializer(materializer, request_payload, work)

    expected_failure = {
        "kind": "sdk_error",
        "operation": "head_current",
        "access_path": "current-selector",
        "code": "store.current-corrupt",
        "field": publication_id,
        "detail": {
            "kind": "opaque",
            "byte_count": 0,
            "diagnostic": "",
            "digest": oracle.content_digest(b""),
        },
    }
    report = assert_compact_response(
        root,
        oracle,
        completed,
        request_payload,
        request,
        expected_failure,
    )
    cause = report["effects"]["store_failure_cause"]
    binding_state = evidence_binding_state(report)
    if (
        binding_state != "request-bound"
        or report["error"]["phase"] != "store-stage"
        or report["error"]["code"] != "materialization.store-failure"
        or report["effects"]["store_draft_state"] != "discarded"
        or report["effects"]["head_observation"] != "sdk-error"
        or report["effects"]["observed_head_publication"] is not None
        or cause != expected_failure
        or report["effects"]["publication_attempted"]
        or report["effects"]["committed_transaction_count"] != 0
        or report["effects"]["prior_history_retained"] is not True
        or report["effects"]["task_attempt_count"] != 1
        or report["effects"]["task_success_count"] != 1
        or report["effects"]["worker_launch_attempt_count"] != 1
        or report["effects"]["worker_launch_success_count"] != 1
    ):
        fail(f"{case_name} lost the authenticated non-not-found head failure")
    receipt_bytes = make_execution_receipt(root, oracle, completed, case_name)
    if evidence_dir is not None:
        write_negative_evidence(
            oracle,
            evidence_dir,
            case_name,
            request_payload,
            completed,
            receipt_bytes,
            request,
            report,
        )


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    prefix = args.prefix.resolve()
    sys.path.insert(0, str(root / "tools" / "quality"))
    import check_ng_clang22_materialization as oracle  # pylint: disable=import-error
    import check_ng_clang22_install_matrix as install_matrix  # pylint: disable=import-error

    materializer = install_matrix.resolve_installed_executable(
        prefix,
        "bin/cxxlens-clang22-materialize",
        args.executable_suffix,
    )
    if not materializer.is_file() or materializer.is_symlink():
        fail(f"installed materializer is missing or not regular: {materializer}")

    negative_evidence_dir = (
        args.evidence_dir.resolve() if args.evidence_dir is not None else None
    )
    raw_cases = (
        (
            "raw-request-schema",
            b'{"schema":"cxxlens.clang22-materialization-request.v2","request_version":"2.1.0"}',
            "request-schema",
            "materialization.request-invalid",
        ),
        (
            "raw-request-envelope",
            b"{}",
            "request-envelope",
            "materialization.request-invalid",
        ),
        (
            "raw-request-version",
            b'{"schema":"cxxlens.clang22-materialization-request.v2","request_version":"9.9.9"}',
            "request-version",
            "materialization.version-unsupported",
        ),
        (
            "raw-invalid-utf8",
            b'{"schema":\xff}',
            "json-decode",
            "materialization.request-invalid",
        ),
        (
            "raw-bom",
            b"\xef\xbb\xbf{}",
            "json-decode",
            "materialization.request-invalid",
        ),
        (
            "raw-duplicate-member",
            b'{"schema":"x","schema":"y"}',
            "json-decode",
            "materialization.request-invalid",
        ),
        (
            "raw-nested-duplicate-member",
            b'{"nested":{"member":1,"member":2}}',
            "json-decode",
            "materialization.request-invalid",
        ),
        (
            "raw-non-object",
            b"[]",
            "json-decode",
            "materialization.request-invalid",
        ),
        (
            "raw-trailing-value",
            b"{} {}",
            "json-decode",
            "materialization.request-invalid",
        ),
    )
    with tempfile.TemporaryDirectory(prefix="clang22-materializer-negative-") as directory:
        work = pathlib.Path(directory)
        for case_name, raw_payload, expected_phase, expected_code in raw_cases:
            raw_completed = run_materializer(materializer, raw_payload, work)
            assert_raw_failure(
                root,
                oracle,
                raw_completed,
                raw_payload,
                case_name,
                expected_phase,
                expected_code,
                negative_evidence_dir,
            )

    request = installed_request(root, prefix, oracle)
    request["publication"]["genesis"] = False
    request["publication"]["expected_parent_publication"] = "publication:missing-parent"
    oracle.bind_request_identity(request)
    oracle.validate_request(root, request)
    request_payload = oracle.canonical_json(request)
    expected_head_failure = {
        "kind": "sdk_error",
        "operation": "head_current",
        "access_path": "current-selector",
        "code": "store.current-not-found",
        "field": request["publication"]["series_id"],
        "detail": {
            "kind": "opaque",
            "byte_count": 0,
            "diagnostic": "",
            "digest": oracle.content_digest(b""),
        },
    }
    with tempfile.TemporaryDirectory(prefix="clang22-materializer-head-negative-") as directory:
        work = pathlib.Path(directory)
        completed = run_materializer(materializer, request_payload, work)
    report = assert_compact_response(
        root,
        oracle,
        completed,
        request_payload,
        request,
        expected_head_failure,
    )
    receipt_bytes = make_execution_receipt(root, oracle, completed, "store-head-absent")
    if negative_evidence_dir is not None:
        write_negative_evidence(
            oracle,
            negative_evidence_dir,
            "store-head-absent",
            request_payload,
            completed,
            receipt_bytes,
            request,
            report,
        )
    cause = report["effects"]["store_failure_cause"]
    if (
        report["error"]["phase"] != "store-stage"
        or report["error"]["code"] != "materialization.store-failure"
        or report["effects"]["store_draft_state"] != "discarded"
        or report["effects"]["head_observation"] != "absent"
        or report["effects"]["observed_head_publication"] is not None
        or cause is None
        or cause["operation"] != "head_current"
        or cause["access_path"] != "current-selector"
        or cause["code"] != "store.current-not-found"
        or report["effects"]["publication_attempted"]
        or report["effects"]["committed_transaction_count"] != 0
    ):
        fail("fresh non-genesis Store failure lost the exact absent-head authority")

    baseline_database, baseline_request, _baseline_report = prepare_sqlite_baseline(
        root,
        materializer,
        oracle,
        prefix,
        negative_evidence_dir,
    )
    try:
        # A public installed request has no fault-injection option.  These
        # cases therefore use only disposable copies of a real committed Store
        # and retain the exact SDK error observed by snapshot_store::current.
        for corruption_mode in ("payload-byte-flip", "payload-checksum"):
            assert_corrupt_head_failure(
                root,
                oracle,
                materializer,
                baseline_database,
                baseline_request,
                corruption_mode,
                negative_evidence_dir,
            )
    finally:
        shutil.rmtree(baseline_database.parent)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
