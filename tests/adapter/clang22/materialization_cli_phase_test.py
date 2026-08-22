#!/usr/bin/env python3
"""Exercise phase-authentic compact failures at the installed JSON boundary."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any


RAW_INPUT_LIMIT_BYTES = 1 << 30
REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_clang22_materialization_report.schema.yaml"
)


def load_oracle(root: pathlib.Path) -> Any:
    sys.path.insert(0, str(root / "tools" / "quality"))
    import check_ng_clang22_materialization as oracle  # pylint: disable=import-error

    return oracle


def run_materializer(
    driver: pathlib.Path, payload: bytes
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [str(driver)],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def run_input_limit(
    driver: pathlib.Path,
) -> tuple[subprocess.CompletedProcess[bytes], str]:
    """Send the exact limit-plus-one stream without retaining it in Python."""

    with tempfile.TemporaryFile() as payload:
        payload.truncate(RAW_INPUT_LIMIT_BYTES + 1)
        payload.seek(0)
        completed = subprocess.run(
            [str(driver)],
            stdin=payload,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=180,
        )
    digest = hashlib.sha256()
    zero_chunk = b"\0" * (1 << 20)
    remaining = RAW_INPUT_LIMIT_BYTES + 1
    while remaining:
        chunk_size = min(remaining, len(zero_chunk))
        digest.update(zero_chunk[:chunk_size])
        remaining -= chunk_size
    return completed, "sha256:" + digest.hexdigest()


def parse_one_json(
    oracle: Any, stdout: bytes, label: str
) -> dict[str, Any]:
    report = oracle.load_strict_json_bytes(stdout, f"{label} stdout")
    assert isinstance(report, dict), report
    assert stdout == oracle.canonical_json(report) + b"\n", label
    return report


def assert_raw_compact_failure(
    root: pathlib.Path,
    oracle: Any,
    completed: subprocess.CompletedProcess[bytes],
    payload: bytes,
    *,
    phase: str,
    code: str,
    subject: str,
    label: str,
) -> None:
    assert completed.returncode == 1, (label, completed.stdout, completed.stderr)
    assert completed.stderr == b"", (label, completed.stderr)
    report = parse_one_json(oracle, completed.stdout, label)
    oracle.validate_report(root, None, report, request_bytes=payload)
    assert report["response_kind"] == "compact_failure"
    assert report["result"] == "failed"
    assert report["process_exit_status"] == 1
    assert report["error"] == {
        "code": code,
        "diagnostic": {
            "request-envelope": "missing-or-non-string-envelope:byte=0",
            "request-version": "unsupported-version:byte=0",
        }.get(phase, "selected-contract"),
        "phase": phase,
        "subject": subject,
    }
    assert report["binding"] == {"request": None, "state": "raw-input-only"}
    effects = report["effects"]
    assert effects["publication_attempted"] is False
    assert effects["committed_transaction_count"] == 0
    assert effects["store_draft_state"] == "not-created"
    assert effects["head_observation"] == "not-observed"
    assert effects["task_attempt_count"] == 0
    assert effects["task_success_count"] == 0
    assert effects["worker_launch_attempt_count"] == 0
    assert effects["worker_launch_success_count"] == 0


def assert_input_limit_failure(
    root: pathlib.Path,
    oracle: Any,
    completed: subprocess.CompletedProcess[bytes],
    expected_prefix_digest: str,
) -> None:
    label = "input-limit"
    assert completed.returncode == 1, (completed.stdout, completed.stderr)
    assert completed.stderr == b""
    report = parse_one_json(oracle, completed.stdout, label)
    oracle.validate_schema(
        report,
        oracle.load(root / REPORT_SCHEMA),
        "input-limit materialization report",
        error_code="materialization.report-invalid",
    )
    assert report["response_kind"] == "compact_failure"
    assert report["result"] == "failed"
    assert report["process_exit_status"] == 1
    assert report["error"] == {
        "code": "materialization.request-invalid",
        "diagnostic": "maximum-bytes",
        "phase": "input-limit",
        "subject": "input-limit",
    }
    assert report["raw_input_observation"] == {
        "byte_limit": RAW_INPUT_LIMIT_BYTES,
        "observed_size_bytes": RAW_INPUT_LIMIT_BYTES + 1,
        "observed_prefix_digest": expected_prefix_digest,
        "complete": False,
    }
    assert report["binding"] == {"request": None, "state": "raw-input-only"}
    effects = report["effects"]
    assert effects["publication_attempted"] is False
    assert effects["committed_transaction_count"] == 0
    assert effects["store_draft_state"] == "not-created"
    assert effects["head_observation"] == "not-observed"
    assert effects["task_attempt_count"] == 0
    assert effects["task_success_count"] == 0
    assert effects["worker_launch_attempt_count"] == 0
    assert effects["worker_launch_success_count"] == 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", required=True, type=pathlib.Path)
    parser.add_argument("--root", type=pathlib.Path)
    parser.add_argument(
        "--input-limit",
        action="store_true",
        help="run the opt-in 1 GiB plus one byte transport boundary",
    )
    args = parser.parse_args()

    root = (args.root or pathlib.Path(__file__).resolve().parents[3]).resolve()
    oracle = load_oracle(root)
    if args.input_limit:
        completed, digest = run_input_limit(args.driver)
        assert_input_limit_failure(root, oracle, completed, digest)
        return 0

    assert_raw_compact_failure(
        root,
        oracle,
        run_materializer(args.driver, b"{}"),
        b"{}",
        phase="request-envelope",
        code="materialization.request-invalid",
        subject="request-envelope",
        label="request-envelope",
    )
    unsupported_version = json.dumps(
        {
            "schema": "cxxlens.clang22-materialization-request.v2",
            "request_version": "9.9.9",
        },
        separators=(",", ":"),
    ).encode("utf-8")
    assert_raw_compact_failure(
        root,
        oracle,
        run_materializer(args.driver, unsupported_version),
        unsupported_version,
        phase="request-version",
        code="materialization.version-unsupported",
        subject="request-version",
        label="request-version",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
