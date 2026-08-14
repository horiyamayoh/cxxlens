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
import json
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Any


BASELINE_POLICY_DIGEST = (
    "semantic-v2:sha256:"
    "b4e95d8c88cf660fff40c4d9e7e4ae07bcb078013b5370c6b1abb80b0d75d375"
)
OCCURRENCE_RELATIVE_PATH = (
    "share/cxxlens/materialization/clang22/occurrence-v1.json"
)


def fail(message: str) -> None:
    raise AssertionError(message)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--prefix", required=True, type=pathlib.Path)
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


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    prefix = args.prefix.resolve()
    materializer = prefix / "bin/cxxlens-clang22-materialize"
    if not materializer.is_file():
        fail(f"installed materializer is missing: {materializer}")
    sys.path.insert(0, str(root / "tools" / "quality"))
    import check_ng_clang22_materialization as oracle  # pylint: disable=import-error

    with tempfile.TemporaryDirectory(prefix="clang22-materializer-negative-") as directory:
        work = pathlib.Path(directory)
        raw_payload = b'{"schema":"cxxlens.clang22-materialization-request.v2","request_version":"2.1.0"}'
        raw_completed = run_materializer(materializer, raw_payload, work)
        raw_report = assert_compact_response(
            root, oracle, raw_completed, raw_payload, None
        )
        if (
            raw_report["binding"]["state"] != "raw-input-only"
            or raw_report["error"]["phase"] != "request-schema"
            or raw_report["error"]["code"] != "materialization.request-invalid"
            or raw_report["effects"]["publication_attempted"]
            or raw_report["effects"]["committed_transaction_count"] != 0
        ):
            fail("raw request-schema failure crossed the binding or publication boundary")

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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
