#!/usr/bin/env python3
"""Check installed Clang 22 request/report/receipt evidence without qualifying GR.

The installed acceptance produces one request/report/receipt set plus an
independent raw-provider evidence manifest per backend.  This checker validates
those externally observable bytes and computes the report-set projection required
by the release contract.  It deliberately distinguishes a complete configuration
(memory plus SQLite) from the exact static/shared matrix; the release
qualification checker remains the sole aggregate authority.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import pathlib
import subprocess
import sys
from typing import Any

import jsonschema

import check_ng_clang22_materialization as oracle
from check_ng_provider_protocol import encode_frame


REQUEST_FILENAME = "cxxlens-clang22-materialization-request.json"
REPORT_FILENAME = "cxxlens-clang22-materialization-report.json"
EXECUTION_RECEIPT_FILENAME = "cxxlens-clang22-materialization-execution-receipt.json"
OCCURRENCE_FILENAME = "occurrence-v1.json"
OCCURRENCE_RELATIVE_PATH = (
    "share/cxxlens/materialization/clang22/occurrence-v1.json"
)
RAW_PROVIDER_EVIDENCE_MANIFEST_FILENAME = (
    "cxxlens-clang22-materialization-raw-provider-evidence-v1.json"
)
RAW_PROVIDER_EVIDENCE_DIRECTORY = "raw-provider-transcripts"
RAW_PROVIDER_EVIDENCE_SCHEMA = (
    "cxxlens.clang22-materialization-raw-provider-evidence.v1"
)
REPORT_SET_FILENAME = "cxxlens-clang22-materialization-report-set.json"
RECEIPT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_clang22_materialization_execution_receipt.schema.yaml"
)
MATRIX = (
    ("static", "memory"),
    ("static", "sqlite"),
    ("shared", "memory"),
    ("shared", "sqlite"),
)
BACKENDS = ("memory", "sqlite")


class InstallMatrixError(ValueError):
    """The installed evidence is incomplete or does not bind exactly."""


def installed_worker_host_transcript(
    root: pathlib.Path, request: dict[str, Any], task: dict[str, Any]
) -> bytes:
    """Encode the exact request-bound host transcript for one installed worker.

    This is a qualification-side transport oracle only.  It uses the request's
    task.v3 projection and the same bounded Protocol 1.1 framing contract as the
    materializer, but it never consumes rows or receipt leaves from the public
    materialization report.
    """

    if (
        request["worker"]["protocol_major"] != 1
        or request["worker"]["protocol_minor"] != 1
        or request["worker"]["required_features"] != ["task-input-chunks-v1"]
    ):
        raise InstallMatrixError(
            "installed worker raw evidence requires Provider Protocol 1.1 "
            "task-input-chunks-v1"
        )
    logical_input = oracle.worker_task_v3_projection(request, task)
    if oracle.content_digest(logical_input) != task["task_input_digest"]:
        raise InstallMatrixError(
            "installed worker host transcript task.v3 digest differs from request"
        )
    chunk_size = 1024 * 1024
    chunks = [
        logical_input[offset : offset + chunk_size]
        for offset in range(0, len(logical_input), chunk_size)
    ]
    task_id = task["provider_task_id"]
    input_digest = task["task_input_digest"]
    sequence = 0

    def frame(message_type: int, control: Any, payload: bytes = b"") -> bytes:
        nonlocal sequence
        encoded = encode_frame(
            control,
            payload,
            message_type=message_type,
            stream_id=1,
            sequence=sequence,
            protocol_major=1,
            protocol_minor=1,
        )
        sequence += 1
        return encoded

    frames = [
        frame(
            2,
            oracle.expected_runtime_provider_manifest(root, request),
        ),
        frame(
            3,
            {
                "schema": "cxxlens.provider-control.schema-negotiate.v1",
                "protocol_schema": "cxxlens.provider-protocol.v1",
                "protocol_minor": 1,
            },
        ),
        frame(
            4,
            {
                "schema": "cxxlens.provider-control.open-task.v1",
                "task_id": task_id,
                "task_input_digest": input_digest,
                "normalized_invocation_digest": task[
                    "normalized_invocation_digest"
                ],
                "toolchain_digest": task["toolchain_digest"],
                "environment_digest": task["environment_digest"],
            },
        ),
        frame(
            6,
            {
                "schema": "cxxlens.provider-control.input-descriptor.v1",
                "task_id": task_id,
                "input_digest": input_digest,
                "total_bytes": len(logical_input),
                "chunk_bytes": chunk_size,
                "chunk_count": len(chunks),
            },
        ),
    ]
    for index, payload in enumerate(chunks):
        frames.append(
            frame(
                7,
                {
                    "schema": "cxxlens.provider-control.input-chunk.v1",
                    "task_id": task_id,
                    "input_digest": input_digest,
                    "chunk_index": index,
                    "offset": index * chunk_size,
                    "byte_count": len(payload),
                },
                payload,
            )
        )
    budget = task["budget"]
    frames.append(
        frame(
            8,
            {
                "schema": "cxxlens.provider-control.credit.v1",
                "bytes": max(budget["output_bytes"], 1),
                "frames": max(budget["diagnostics"] + budget["rows"] + 32, 1),
            },
        )
    )
    frames.append(
        frame(
            22,
            {"schema": "cxxlens.provider-control.close.v1", "task_id": task_id},
        )
    )
    return b"".join(frames)


def capture_installed_raw_provider_transcripts(
    root: pathlib.Path,
    prefix: pathlib.Path,
    request: dict[str, Any],
    occurrence: dict[str, Any],
) -> dict[tuple[str, str, str], bytes]:
    """Capture raw stdout from the installed worker, independently of report rows."""

    worker_record = next(
        (row for row in occurrence["files"] if row.get("role") == "worker-executable"),
        None,
    )
    if not isinstance(worker_record, dict) or not isinstance(
        worker_record.get("path"), str
    ):
        raise InstallMatrixError("installed occurrence lacks a worker executable")
    worker = prefix / worker_record["path"]
    if not worker.is_file() or worker.is_symlink():
        raise InstallMatrixError(f"installed worker is not a regular file: {worker}")
    if worker_record.get("digest") != request["worker"].get(
        "installed_binary_digest"
    ):
        raise InstallMatrixError(
            "installed worker occurrence digest differs from request binding"
        )
    if digest_bytes(worker.read_bytes()) != worker_record.get("digest"):
        raise InstallMatrixError(
            f"installed worker bytes differ from occurrence manifest: {worker}"
        )

    manifest = oracle.expected_runtime_provider_manifest(root, request)
    environment = dict(os.environ)
    environment.pop("LD_LIBRARY_PATH", None)
    environment.pop("DYLD_LIBRARY_PATH", None)
    raw_occurrences: dict[tuple[str, str, str], bytes] = {}
    for task in sorted(request["tasks"], key=oracle.task_execution_key):
        key = oracle.task_execution_key(task)
        task_environment = {
            "CXXLENS_PROVIDER_ID": request["worker"]["provider_id"],
            "CXXLENS_PROVIDER_MANIFEST": manifest,
            "CXXLENS_PROVIDER_BINARY_DIGEST": request["worker"][
                "installed_binary_digest"
            ],
            "CXXLENS_PROVIDER_SEMANTIC_CONTRACT_DIGEST": request["worker"][
                "semantic_contract_digest"
            ],
            "CXXLENS_PROVIDER_TASK_ID": task["provider_task_id"],
            "CXXLENS_PROVIDER_TASK_INPUT_DIGEST": task["task_input_digest"],
            "CXXLENS_PROVIDER_NORMALIZED_INVOCATION_DIGEST": task[
                "normalized_invocation_digest"
            ],
            "CXXLENS_PROVIDER_TOOLCHAIN_DIGEST": task["toolchain_digest"],
            "CXXLENS_PROVIDER_ENVIRONMENT_DIGEST": task["environment_digest"],
            "CXXLENS_PROVIDER_PROTOCOL_MAJOR": str(
                request["worker"]["protocol_major"]
            ),
            "CXXLENS_PROVIDER_PROTOCOL_MINOR": str(
                request["worker"]["protocol_minor"]
            ),
        }
        completed = subprocess.run(
            [str(worker)],
            input=installed_worker_host_transcript(root, request, task),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=prefix,
            env={**environment, **task_environment},
            check=False,
        )
        if completed.returncode != 0:
            raise InstallMatrixError(
                "installed worker raw transcript execution failed for "
                f"{key}: returncode={completed.returncode}, "
                f"stderr={completed.stderr[:1000]!r}"
            )
        if completed.stderr:
            raise InstallMatrixError(
                f"installed worker wrote stderr for {key}: {completed.stderr[:1000]!r}"
            )
        if not completed.stdout:
            raise InstallMatrixError(f"installed worker emitted no raw transcript for {key}")
        raw_occurrences[key] = completed.stdout
    return raw_occurrences


def validate_independent_raw_provider_transcripts(
    root: pathlib.Path,
    request: dict[str, Any],
    report: dict[str, Any],
    raw_occurrences: dict[tuple[str, str, str], bytes],
) -> None:
    """Bind report receipt leaves to actual worker bytes, never report-derived bytes."""

    tasks = {oracle.task_execution_key(task): task for task in request["tasks"]}
    results = {
        oracle.task_execution_key(result): result
        for result in report.get("task_results", [])
    }
    if set(raw_occurrences) != set(tasks) or set(results) != set(tasks):
        raise InstallMatrixError(
            "installed raw provider evidence task census differs from request/report"
        )
    for key, task in tasks.items():
        observation = oracle.derive_runtime_observation(
            root, request, task, raw_occurrences[key]
        )
        expected_receipt = oracle.materialization_runtime_receipt(
            observation["receipt"]
        )
        if results[key].get("runtime_receipt") != expected_receipt:
            raise InstallMatrixError(
                "installed report runtime receipt is not bound to actual worker raw "
                f"stdout for {key}"
            )


def validate_installed_task_v3_source_binding(
    request: dict[str, Any], report: dict[str, Any]
) -> None:
    """Validate the installed task.v3 source spelling and execution chain.

    The generic report validator checks the complete report contract.  This
    named check keeps the #199 closure boundary visible at the installed
    evidence layer: the exact canonical Base64 spelling must decode to the
    source bytes used by task.v3, whose digest and provider execution identity
    must be the same values adopted in the installed report.
    """

    tasks = {oracle.task_execution_key(task): task for task in request["tasks"]}
    results = {
        oracle.task_execution_key(result): result
        for result in report.get("task_results", [])
    }
    if set(tasks) != set(results):
        raise InstallMatrixError(
            "installed task.v3 source binding has a different task execution census"
        )

    for key, task in tasks.items():
        result = results[key]
        spelling = task["source"]["content_base64"]
        try:
            source = oracle.decode_canonical_base64(spelling)
        except oracle.MaterializationError as error:
            raise InstallMatrixError(
                f"installed task.v3 source Base64 is not canonical for {key}: {error}"
            ) from error
        if base64.b64encode(source).decode("ascii") != spelling:
            raise InstallMatrixError(
                f"installed task.v3 source Base64 spelling is not unique for {key}"
            )

        expected_task_input = oracle.expected_task_input_digest(request, task)
        if task["task_input_digest"] != expected_task_input:
            raise InstallMatrixError(
                f"installed task.v3 digest is not bound to canonical source bytes for {key}"
            )
        if result["task_input_digest"] != expected_task_input:
            raise InstallMatrixError(
                f"installed report task.v3 digest differs for {key}"
            )

        expected_execution = oracle.expected_provider_execution_id(request, task)
        if task["provider_execution_id"] != expected_execution:
            raise InstallMatrixError(
                f"installed provider execution identity is not source-bound for {key}"
            )
        if result["provider_execution_id"] != expected_execution:
            raise InstallMatrixError(
                f"installed report provider execution identity differs for {key}"
            )
        if result["input_transfer"] != oracle.expected_input_transfer_receipt(
            request, task
        ):
            raise InstallMatrixError(
                f"installed task.v3 input transfer receipt differs for {key}"
            )
        sealed_digest = result.get("runtime_receipt", {}).get(
            "sealed_transcript_digest"
        )
        if not isinstance(sealed_digest, str) or not sealed_digest:
            raise InstallMatrixError(
                f"installed task.v3 result lacks a sealed provider transcript for {key}"
            )


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def canonical_digest(value: Any) -> str:
    return digest_bytes(oracle.canonical_json(value))


def git_value(root: pathlib.Path, expression: str) -> str:
    completed = subprocess.run(
        ["git", "rev-parse", expression],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def load_object(path: pathlib.Path, label: str) -> tuple[dict[str, Any], bytes]:
    try:
        raw = path.read_bytes()
        value = oracle.load_strict_json_bytes(raw, label)
    except (OSError, oracle.MaterializationError) as error:
        raise InstallMatrixError(f"{label} cannot be loaded: {error}") from error
    if raw != oracle.canonical_json(value):
        raise InstallMatrixError(f"{label} is not canonical JSON: {path}")
    return value, raw


def load_receipt(
    root: pathlib.Path, path: pathlib.Path
) -> tuple[dict[str, Any], bytes]:
    value, raw = load_object(path, f"execution receipt {path}")
    try:
        jsonschema.Draft202012Validator(
            oracle.load(root / RECEIPT_SCHEMA),
            format_checker=jsonschema.Draft202012Validator.FORMAT_CHECKER,
        ).validate(value)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise InstallMatrixError(
            f"execution receipt schema validation failed: {path}: {error.message}"
        ) from error
    return value, raw


def load_occurrence(
    root: pathlib.Path, path: pathlib.Path
) -> tuple[dict[str, Any], bytes]:
    """Load and validate the raw installed occurrence authority."""

    try:
        raw = path.read_bytes()
        value = oracle.load_strict_json_bytes(raw, f"occurrence manifest {path}")
        oracle.validate_occurrence_manifest(root, value)
    except (OSError, oracle.MaterializationError) as error:
        raise InstallMatrixError(
            f"occurrence manifest cannot be independently validated: {path}: {error}"
        ) from error
    return value, raw


def validate_installed_prefix_occurrence(
    prefix: pathlib.Path,
    occurrence: dict[str, Any],
    occurrence_bytes: bytes,
) -> None:
    """Re-measure the exact installed occurrence behind an evidence triplet."""

    prefix = prefix.resolve()
    installed_manifest = prefix / OCCURRENCE_RELATIVE_PATH
    try:
        if installed_manifest.is_symlink() or not installed_manifest.is_file():
            raise InstallMatrixError(
                "installed occurrence manifest is not a regular file"
            )
        if installed_manifest.read_bytes() != occurrence_bytes:
            raise InstallMatrixError(
                "installed occurrence manifest differs from the retained evidence"
            )
        for entry in occurrence["files"]:
            candidate = prefix / entry["path"]
            resolved = candidate.resolve()
            if prefix not in resolved.parents:
                raise InstallMatrixError(
                    f"installed occurrence path escapes the prefix: {entry['path']}"
                )
            if candidate.is_symlink() or not candidate.is_file():
                raise InstallMatrixError(
                    f"installed occurrence entry is not a regular file: {entry['path']}"
                )
            observed_digest = digest_bytes(candidate.read_bytes())
            if observed_digest != entry["digest"]:
                raise InstallMatrixError(
                    "installed occurrence file digest differs for "
                    f"{entry['role']} ({entry['path']}): "
                    f"expected={entry['digest']} actual={observed_digest}"
                )
    except OSError as error:
        raise InstallMatrixError(
            f"installed occurrence cannot be measured under {prefix}: {error}"
        ) from error


def load_raw_provider_evidence(
    path: pathlib.Path,
    request: dict[str, Any],
) -> tuple[dict[tuple[str, str, str], bytes], bytes]:
    """Load exact raw worker stdout occurrences bound to request task identities."""

    manifest, manifest_bytes = load_object(
        path, f"raw provider evidence manifest {path}"
    )
    if (
        not isinstance(manifest, dict)
        or set(manifest) != {"schema", "entries"}
        or manifest["schema"] != RAW_PROVIDER_EVIDENCE_SCHEMA
    ):
        raise InstallMatrixError(
            f"raw provider evidence manifest has an invalid closed shape: {path}"
        )
    entries = manifest["entries"]
    if not isinstance(entries, list):
        raise InstallMatrixError(
            f"raw provider evidence entries are not an array: {path}"
        )
    tasks = sorted(request["tasks"], key=oracle.task_execution_key)
    expected_keys = {oracle.task_execution_key(task) for task in tasks}
    if len(entries) != len(expected_keys):
        raise InstallMatrixError(f"raw provider evidence entry census differs: {path}")

    raw_occurrences: dict[tuple[str, str, str], bytes] = {}
    paths: set[str] = set()
    for ordinal, entry in enumerate(entries):
        if not isinstance(entry, dict) or set(entry) != {
            "task_execution_key",
            "relative_path",
            "byte_count",
            "sha256",
        }:
            raise InstallMatrixError(
                f"raw provider evidence entry is not closed: {path}"
            )
        key_value = entry["task_execution_key"]
        if not isinstance(key_value, list) or len(key_value) != 3 or not all(
            isinstance(value, str) for value in key_value
        ):
            raise InstallMatrixError(f"raw provider evidence task key is invalid: {path}")
        if (
            isinstance(entry["byte_count"], bool)
            or not isinstance(entry["byte_count"], int)
            or not isinstance(entry["sha256"], str)
        ):
            raise InstallMatrixError(
                f"raw provider evidence byte metadata is invalid: {path}"
            )
        key = tuple(key_value)
        if key not in expected_keys or key in raw_occurrences:
            raise InstallMatrixError(
                f"raw provider evidence task key is duplicate or unauthorized: {path}"
            )
        relative_path = entry["relative_path"]
        if not isinstance(relative_path, str):
            raise InstallMatrixError(
                f"raw provider evidence path is not a string: {path}"
            )
        if key != oracle.task_execution_key(tasks[ordinal]):
            raise InstallMatrixError(
                f"raw provider evidence task order is not canonical: {path}"
            )
        expected_relative_path = (
            f"{RAW_PROVIDER_EVIDENCE_DIRECTORY}/task-{ordinal:04d}.bin"
        )
        if relative_path != expected_relative_path or relative_path in paths:
            raise InstallMatrixError(
                f"raw provider evidence path/order is not canonical: {path}"
            )
        paths.add(relative_path)
        relative = pathlib.PurePosixPath(relative_path)
        raw_path = path.parent / relative
        if (
            relative.is_absolute()
            or ".." in relative.parts
            or not raw_path.is_file()
            or raw_path.is_symlink()
        ):
            raise InstallMatrixError(
                f"raw provider evidence occurrence is not a regular file: {raw_path}"
            )
        raw = raw_path.read_bytes()
        task = tasks[ordinal]
        if (
            entry["byte_count"] != len(raw)
            or entry["sha256"] != digest_bytes(raw)
            or len(raw) > task["budget"]["transport_bytes"]
        ):
            raise InstallMatrixError(
                f"raw provider evidence bytes are not request-bound: {raw_path}"
            )
        raw_occurrences[key] = raw
    if set(raw_occurrences) != expected_keys:
        raise InstallMatrixError(f"raw provider evidence task set differs: {path}")
    return raw_occurrences, manifest_bytes


def artifact_paths(evidence_dir: pathlib.Path) -> dict[pathlib.Path, dict[str, pathlib.Path]]:
    if not evidence_dir.is_dir():
        raise InstallMatrixError(f"evidence directory does not exist: {evidence_dir}")
    paths = {
        name: sorted(
            path
            for path in evidence_dir.rglob(name)
            if path.is_file()
        )
        for name in (
            REQUEST_FILENAME,
            REPORT_FILENAME,
            EXECUTION_RECEIPT_FILENAME,
            OCCURRENCE_FILENAME,
            RAW_PROVIDER_EVIDENCE_MANIFEST_FILENAME,
        )
    }
    if not all(paths.values()):
        missing = [name for name, values in paths.items() if not values]
        raise InstallMatrixError(
            "installed materialization evidence is missing required artifacts: "
            + ", ".join(missing)
        )
    parents = {
        name: {path.parent for path in values}
        for name, values in paths.items()
    }
    if not (
        parents[REQUEST_FILENAME]
        == parents[REPORT_FILENAME]
        == parents[EXECUTION_RECEIPT_FILENAME]
        == parents[OCCURRENCE_FILENAME]
    ):
        raise InstallMatrixError(
            "request/report/receipt/occurrence artifacts are not co-located one-for-one"
        )
    if parents[RAW_PROVIDER_EVIDENCE_MANIFEST_FILENAME] != parents[REQUEST_FILENAME]:
        raise InstallMatrixError(
            "raw provider evidence manifest is not co-located with its request/report"
        )
    return {
        parent: {
            REQUEST_FILENAME: parent / REQUEST_FILENAME,
            REPORT_FILENAME: parent / REPORT_FILENAME,
            EXECUTION_RECEIPT_FILENAME: parent / EXECUTION_RECEIPT_FILENAME,
            OCCURRENCE_FILENAME: parent / OCCURRENCE_FILENAME,
            RAW_PROVIDER_EVIDENCE_MANIFEST_FILENAME: parent
            / RAW_PROVIDER_EVIDENCE_MANIFEST_FILENAME,
        }
        for parent in parents[REQUEST_FILENAME]
    }


def validate_triplet(
    root: pathlib.Path,
    paths: dict[str, pathlib.Path],
    expected_source: dict[str, str],
    prefix: pathlib.Path | None,
) -> dict[str, Any]:
    request, request_bytes = load_object(
        paths[REQUEST_FILENAME], f"materialization request {paths[REQUEST_FILENAME]}"
    )
    oracle.validate_schema(
        request,
        oracle.load(root / oracle.REQUEST_SCHEMA),
        f"materialization request {paths[REQUEST_FILENAME]}",
    )
    try:
        oracle.validate_request(root, request)
    except oracle.MaterializationError as error:
        raise InstallMatrixError(
            f"materialization request binding is invalid: {paths[REQUEST_FILENAME]}: {error}"
        ) from error

    occurrence, occurrence_bytes = load_occurrence(
        root, paths[OCCURRENCE_FILENAME]
    )
    if prefix is not None:
        validate_installed_prefix_occurrence(prefix, occurrence, occurrence_bytes)
    if (
        occurrence["source_revision"] != expected_source["revision"]
        or occurrence["source_tree"] != expected_source["tree"]
    ):
        raise InstallMatrixError(
            "installed occurrence manifest is not bound to the current source: "
            f"{paths[OCCURRENCE_FILENAME]}"
        )
    if (
        request["tool"]["source_revision"] != occurrence["source_revision"]
        or request["tool"]["source_tree"] != occurrence["source_tree"]
        or request["tool"]["package_configuration"]
        != occurrence["package_configuration"]
        or request["tool"]["occurrence_manifest_digest"]
        != digest_bytes(occurrence_bytes)
    ):
        raise InstallMatrixError(
            "request does not bind the retained installed occurrence manifest"
        )

    report, report_bytes = load_object(
        paths[REPORT_FILENAME], f"materialization report {paths[REPORT_FILENAME]}"
    )
    try:
        validate_installed_task_v3_source_binding(request, report)
        runtime_raw_occurrences, raw_manifest_bytes = load_raw_provider_evidence(
            paths[RAW_PROVIDER_EVIDENCE_MANIFEST_FILENAME], request
        )
        oracle.validate_report(
            root,
            request,
            report,
            request_bytes=request_bytes,
            runtime_raw_occurrences=runtime_raw_occurrences,
        )
    except (InstallMatrixError, oracle.MaterializationError) as error:
        raise InstallMatrixError(
            f"materialization report binding is invalid: {paths[REPORT_FILENAME]}: {error}"
        ) from error

    receipt, receipt_bytes = load_receipt(root, paths[EXECUTION_RECEIPT_FILENAME])
    report_digest = digest_bytes(report_bytes)
    if (
        receipt["actual_exit_status"] != 0
        or receipt["parsed_response_count"] != 1
        or receipt["exact_stdout_byte_count"] != len(report_bytes)
        or receipt["stdout_sha256"] != report_digest
    ):
        raise InstallMatrixError(
            "execution receipt does not bind exact successful report stdout: "
            f"{paths[EXECUTION_RECEIPT_FILENAME]}"
        )

    configuration = request["tool"].get("package_configuration")
    backend = report["publication"].get("backend")
    if request["publication"].get("backend") != backend:
        raise InstallMatrixError(
            f"request/report backend binding differs: {paths[REPORT_FILENAME]}"
        )
    if report["source"] != expected_source:
        raise InstallMatrixError(
            f"materialization report is not bound to the current source: {paths[REPORT_FILENAME]}"
        )
    if report["installation"]["measured"]["configuration"] != configuration:
        raise InstallMatrixError(
            f"materialization report/package configuration differs: {paths[REPORT_FILENAME]}"
        )
    if (configuration, backend) not in MATRIX:
        raise InstallMatrixError(
            f"unexpected installed materialization matrix entry: {(configuration, backend)}"
        )
    return {
        "configuration": configuration,
        "backend": backend,
        "request_path": paths[REQUEST_FILENAME].as_posix(),
        "request_digest": digest_bytes(request_bytes),
        "request_byte_count": len(request_bytes),
        "report_path": paths[REPORT_FILENAME].as_posix(),
        "report_digest": report_digest,
        "report_byte_count": len(report_bytes),
        "execution_receipt_path": paths[EXECUTION_RECEIPT_FILENAME].as_posix(),
        "execution_receipt_digest": digest_bytes(receipt_bytes),
        "occurrence_manifest_path": paths[OCCURRENCE_FILENAME].as_posix(),
        "occurrence_manifest_digest": digest_bytes(occurrence_bytes),
        "occurrence_manifest_byte_count": len(occurrence_bytes),
        "occurrence_file_count": len(occurrence["files"]),
        "raw_provider_evidence_manifest_path": paths[
            RAW_PROVIDER_EVIDENCE_MANIFEST_FILENAME
        ].as_posix(),
        "raw_provider_evidence_manifest_digest": digest_bytes(raw_manifest_bytes),
        "actual_exit_status": receipt["actual_exit_status"],
        "exact_stdout_byte_count": receipt["exact_stdout_byte_count"],
        "stdout_sha256": receipt["stdout_sha256"],
        "parsed_response_count": receipt["parsed_response_count"],
        "stderr_sha256": receipt["stderr_sha256"],
    }


def report_set_projection(
    entries: dict[tuple[str, str], dict[str, Any]], configuration: str
) -> dict[str, Any]:
    return {
        "configuration": configuration,
        "reports": [
            {
                "backend": backend,
                "report_digest": entries[(configuration, backend)]["report_digest"],
                "execution_receipt_digest": entries[(configuration, backend)][
                    "execution_receipt_digest"
                ],
            }
            for backend in BACKENDS
        ],
    }


def write_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(oracle.canonical_json(value) + b"\n")


def write_report_set_projection(
    evidence_dir: pathlib.Path,
    configuration: str,
    projection: dict[str, Any],
    report_set_digest: str,
) -> None:
    write_json(
        evidence_dir / configuration / REPORT_SET_FILENAME,
        {
            "projection": projection,
            "report_set_digest": report_set_digest,
        },
    )


def build_report(
    root: pathlib.Path,
    evidence_dir: pathlib.Path,
    require_exact_matrix: bool,
    prefix: pathlib.Path | None = None,
) -> dict[str, Any]:
    expected_source = {
        "revision": git_value(root, "HEAD"),
        "tree": git_value(root, "HEAD^{tree}"),
    }
    entries: dict[tuple[str, str], dict[str, Any]] = {}
    for parent, paths in artifact_paths(evidence_dir).items():
        entry = validate_triplet(root, paths, expected_source, prefix)
        key = (entry["configuration"], entry["backend"])
        if key in entries:
            raise InstallMatrixError(f"duplicate materialization matrix entry: {key}")
        entries[key] = entry

    configurations = sorted({configuration for configuration, _ in entries})
    expected_configuration_pairs = {
        (configuration, backend)
        for configuration in configurations
        for backend in BACKENDS
    }
    if set(entries) != expected_configuration_pairs:
        raise InstallMatrixError(
            "each observed package configuration must contain exactly memory and SQLite: "
            f"observed={sorted(entries)}, expected={sorted(expected_configuration_pairs)}"
        )
    exact_matrix = set(entries) == set(MATRIX)
    if require_exact_matrix and not exact_matrix:
        raise InstallMatrixError(
            "exact static/shared x memory/SQLite matrix is unavailable; "
            "one installed package configuration is not four-configuration qualification"
        )

    report_sets: list[dict[str, Any]] = []
    report_set_digests: dict[str, str] = {}
    for configuration in configurations:
        projection = report_set_projection(entries, configuration)
        digest = canonical_digest(projection)
        report_set_digests[configuration] = digest
        write_report_set_projection(evidence_dir, configuration, projection, digest)
        report_sets.append(
            {
                "configuration": configuration,
                "projection": projection,
                "report_set_digest": digest,
            }
        )

    blockers = []
    if not exact_matrix:
        blockers.append(
            "missing-installed-package-configuration: static and shared must both be collected"
        )
    blockers.append(
        "release-qualification-delegated: run check_ng_release_qualification.py with exact install manifests and GR evidence"
    )
    blockers.append(
        "external-publication-prerequisite: CI must upload materialization-evidence from both install-consumer jobs"
    )
    return {
        "schema": "cxxlens.test.clang22-install-matrix-evidence.v1",
        "source": expected_source,
        "evidence_root": evidence_dir.as_posix(),
        "status": "exact-matrix-observed" if exact_matrix else "configuration-complete",
        "release_qualification": "not-evaluated",
        "release_claim": "not-qualified-by-this-checker",
        "observed_matrix": [
            {"configuration": configuration, "backend": backend}
            for configuration, backend in sorted(entries)
        ],
        "report_sets": report_sets,
        "report_set_digests": report_set_digests,
        "reports": [entries[key] for key in sorted(entries)],
        "blockers": blockers,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).parents[2])
    parser.add_argument("--evidence-dir", required=True, type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument(
        "--prefix",
        type=pathlib.Path,
        help=(
            "optional installed prefix; when supplied, re-measure every occurrence "
            "manifest entry against the retained evidence"
        ),
    )
    parser.add_argument(
        "--require-exact-matrix",
        action="store_true",
        help="reject a single static or shared configuration instead of returning partial evidence",
    )
    args = parser.parse_args()
    try:
        report = build_report(
            args.root.resolve(),
            args.evidence_dir.resolve(),
            args.require_exact_matrix,
            args.prefix.resolve() if args.prefix is not None else None,
        )
        if args.output is not None:
            write_json(args.output.resolve(), report)
        print(
            f"installed Clang 22 materialization evidence {report['status']}; "
            f"entries={len(report['observed_matrix'])}; "
            f"release_qualification={report['release_qualification']}"
        )
        return 0
    except (
        InstallMatrixError,
        OSError,
        subprocess.CalledProcessError,
        json.JSONDecodeError,
        jsonschema.SchemaError,
    ) as error:
        print(f"installed materialization evidence check failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
