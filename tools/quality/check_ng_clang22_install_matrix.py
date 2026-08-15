#!/usr/bin/env python3
"""Check installed Clang 22 request/report/receipt evidence without qualifying GR.

The installed acceptance produces one request/report/receipt triplet per backend.
This checker validates those externally observable bytes and computes the report-set
projection required by the release contract.  It deliberately distinguishes a
complete configuration (memory plus SQLite) from the exact static/shared matrix;
the release qualification checker remains the sole aggregate authority.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import pathlib
import subprocess
import sys
from typing import Any

import jsonschema

import check_ng_clang22_materialization as oracle


REQUEST_FILENAME = "cxxlens-clang22-materialization-request.json"
REPORT_FILENAME = "cxxlens-clang22-materialization-report.json"
EXECUTION_RECEIPT_FILENAME = "cxxlens-clang22-materialization-execution-receipt.json"
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
    if not (parents[REQUEST_FILENAME] == parents[REPORT_FILENAME] == parents[EXECUTION_RECEIPT_FILENAME]):
        raise InstallMatrixError(
            "request/report/execution-receipt artifacts are not co-located one-for-one"
        )
    return {
        parent: {
            REQUEST_FILENAME: parent / REQUEST_FILENAME,
            REPORT_FILENAME: parent / REPORT_FILENAME,
            EXECUTION_RECEIPT_FILENAME: parent / EXECUTION_RECEIPT_FILENAME,
        }
        for parent in parents[REQUEST_FILENAME]
    }


def validate_triplet(
    root: pathlib.Path,
    paths: dict[str, pathlib.Path],
    expected_source: dict[str, str],
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

    report, report_bytes = load_object(
        paths[REPORT_FILENAME], f"materialization report {paths[REPORT_FILENAME]}"
    )
    try:
        validate_installed_task_v3_source_binding(request, report)
        runtime_raw_occurrences = oracle.report_runtime_raw_occurrences(root, request, report)
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
) -> dict[str, Any]:
    expected_source = {
        "revision": git_value(root, "HEAD"),
        "tree": git_value(root, "HEAD^{tree}"),
    }
    entries: dict[tuple[str, str], dict[str, Any]] = {}
    for parent, paths in artifact_paths(evidence_dir).items():
        entry = validate_triplet(root, paths, expected_source)
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
