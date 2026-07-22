#!/usr/bin/env python3
"""Validate the C++ compact response against the independent machine oracle."""

from __future__ import annotations

import argparse
import copy
import json
import pathlib
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--driver", required=True, type=pathlib.Path)
    args = parser.parse_args()

    completed = subprocess.run(
        [str(args.driver), "--emit-raw"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    assert completed.returncode == 0, (completed.stdout, completed.stderr)
    assert completed.stderr == b""
    assert completed.stdout.endswith(b"\n")
    report = json.loads(completed.stdout)

    sys.path.insert(0, str(args.root / "tools" / "quality"))
    import check_ng_clang22_materialization as oracle  # pylint: disable=import-error

    oracle.validate_report(
        args.root,
        None,
        report,
        request_bytes=b"{invalid-request}",
    )

    report_schema = oracle.load(args.root / oracle.REPORT_SCHEMA)
    for phase in ("json-decode", "request-schema", "request-binding"):
        spool_report = copy.deepcopy(report)
        spool_report["error"].update(
            {"code": "materialization.spool-failure", "phase": phase}
        )
        oracle.validate_schema(
            spool_report,
            report_schema,
            f"{phase} spool report",
            error_code="materialization.report-invalid",
        )

    illegal_phase_codes = (
        ("materialization.spool-failure", "request-version"),
        ("materialization.request-invalid", "request-binding"),
        ("materialization.identity-mismatch", "request-schema"),
        ("materialization.version-unsupported", "json-decode"),
        ("no-response", "json-decode"),
    )
    for code, phase in illegal_phase_codes:
        invalid_report = copy.deepcopy(report)
        invalid_report["error"].update({"code": code, "phase": phase})
        try:
            oracle.validate_schema(
                invalid_report,
                report_schema,
                f"illegal {code}/{phase} report",
                error_code="materialization.report-invalid",
            )
        except oracle.MaterializationError as error:
            assert error.code == "materialization.report-invalid"
        else:
            raise AssertionError(f"schema accepted illegal {code}/{phase} report")

    head_completed = subprocess.run(
        [str(args.driver), "--emit-head-error"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    assert head_completed.returncode == 0, (
        head_completed.stdout,
        head_completed.stderr,
    )
    assert head_completed.stderr == b""
    assert head_completed.stdout.endswith(b"\n")
    head_report = json.loads(head_completed.stdout)
    oracle.validate_schema(
        head_report,
        oracle.load(args.root / oracle.REPORT_SCHEMA),
        "materialization report",
        error_code="materialization.report-invalid",
    )
    effects = head_report["effects"]
    cause = effects["store_failure_cause"]
    detail = cause["detail"]
    assert effects["store_draft_state"] == "discarded"
    assert effects["head_observation"] == "sdk-error"
    assert effects["observed_head_publication"] is None
    assert cause["operation"] == "head_current"
    assert cause["access_path"] == "current-selector"
    assert cause["code"] != "store.current-not-found"
    assert detail["kind"] == "opaque"
    diagnostic_bytes = detail["diagnostic"].encode("utf-8")
    assert detail["byte_count"] == len(diagnostic_bytes)
    assert detail["digest"] == oracle.content_digest(diagnostic_bytes)
    oracle.validate_compact_store_failure_cause(
        head_report["error"],
        effects,
        cause,
    )

    store_completed = subprocess.run(
        [str(args.driver), "--emit-store-open-error"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    assert store_completed.returncode == 0, (
        store_completed.stdout,
        store_completed.stderr,
    )
    assert store_completed.stderr == b""
    assert store_completed.stdout.endswith(b"\n")
    store_report = json.loads(store_completed.stdout)
    oracle.validate_schema(
        store_report,
        oracle.load(args.root / oracle.REPORT_SCHEMA),
        "materialization report",
        error_code="materialization.report-invalid",
    )
    store_effects = store_report["effects"]
    store_cause = store_effects["store_failure_cause"]
    store_detail = store_cause["detail"]
    assert store_effects["store_draft_state"] == "not-created"
    assert store_effects["head_observation"] == "not-observed"
    assert store_cause["operation"] == "store_open"
    assert store_cause["access_path"] is None
    assert store_detail["kind"] == "opaque"
    assert store_detail["diagnostic"]
    store_diagnostic_bytes = store_detail["diagnostic"].encode("utf-8")
    assert store_detail["byte_count"] == len(store_diagnostic_bytes)
    assert store_detail["digest"] == oracle.content_digest(store_diagnostic_bytes)
    oracle.validate_compact_store_failure_cause(
        store_report["error"],
        store_effects,
        store_cause,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
