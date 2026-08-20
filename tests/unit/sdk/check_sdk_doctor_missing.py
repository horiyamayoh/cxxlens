#!/usr/bin/env python3
"""Exercise `cxxlens-sdk-doctor missing` against real present/absent relation scenarios.

This is a positive/negative functional check over the built `cxxlens-sdk-doctor` binary, not a
repository-consistency checker: it invokes the actual executable and asserts on its actual JSON
report and exit code for exact relation IDs known to be registered (or not) in
`known_relation_registry()` (tools/sdk/sdk_doctor_main.cpp).
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys

# The exact 18 relation descriptor IDs known_relation_registry() registers, mirroring
# include/cxxlens/relations/*.hpp. Kept in sync by hand; a drift here only weakens this test's own
# "present" assertions, it cannot make the tool itself falsely report presence.
KNOWN_RELATION_IDS = (
    "build.compile_unit.v1",
    "build.project.v1",
    "build.toolchain_context.v1",
    "build.variant.v1",
    "cc.call_direct_target.v1",
    "cc.call_site.v1",
    "cc.declaration.v1",
    "cc.entity.v1",
    "cc.type.v1",
    "cc.type_component.v1",
    "company.lock.acquire.v1",
    "core.claim_conflict.v1",
    "core.differential_disagreement.v1",
    "core.provider_execution.v1",
    "core.unresolved.v1",
    "source.file.v1",
    "source.origin.v1",
    "source.span.v1",
)


def run(executable: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [executable, "missing", *arguments],
        capture_output=True,
        text=True,
        check=False,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"check_sdk_doctor_missing: {message}")


def check_all_present(executable: str) -> None:
    completed = run(executable, *KNOWN_RELATION_IDS)
    require(completed.returncode == 0, f"all-present exit code was {completed.returncode}")
    report = json.loads(completed.stdout)
    require(report["schema"] == "cxxlens.sdk-doctor-missing.v1", "unexpected schema")
    require(report["mode"] == "missing", "unexpected mode")
    require(report["requested"] == len(KNOWN_RELATION_IDS), "unexpected requested count")
    require(report["missing"] == 0, "known relations reported missing")
    require(report["status"] == "complete", "unexpected status for an all-present request")
    require(len(report["components"]) == len(KNOWN_RELATION_IDS), "component count drift")
    for component in report["components"]:
        require(component["status"] == "present", f"expected present: {component}")
        require("reason_code" not in component, "present component must not carry a reason_code")


def check_unregistered_relation_is_missing(executable: str) -> None:
    completed = run(executable, "cc.call_site.v1", "cc.does_not_exist.v1")
    require(completed.returncode == 1, f"partial-miss exit code was {completed.returncode}")
    report = json.loads(completed.stdout)
    require(report["missing"] == 1, "expected exactly one missing component")
    require(report["status"] == "incomplete", "unexpected status for a partial-miss request")
    by_id = {component["id"]: component for component in report["components"]}
    require(by_id["cc.call_site.v1"]["status"] == "present", "known relation reported missing")
    absent = by_id["cc.does_not_exist.v1"]
    require(absent["status"] == "missing", "unregistered relation reported present")
    require(
        absent["reason_code"] == "sdk.relation-not-found",
        f"unexpected reason_code: {absent.get('reason_code')}",
    )


def check_major_version_mismatch_is_missing(executable: str) -> None:
    completed = run(executable, "cc.call_site.v2")
    require(completed.returncode == 1, f"version-mismatch exit code was {completed.returncode}")
    report = json.loads(completed.stdout)
    require(report["missing"] == 1, "expected the mismatched major version to be missing")
    component = report["components"][0]
    require(component["id"] == "cc.call_site.v2", "unexpected component id")
    require(component["status"] == "missing", "version-mismatched relation reported present")
    require(
        component["reason_code"] == "sdk.relation-major-mismatch",
        f"unexpected reason_code: {component.get('reason_code')}",
    )


def check_no_relations_requested_fails_closed(executable: str) -> None:
    completed = run(executable)
    require(completed.returncode == 2, f"empty-request exit code was {completed.returncode}")
    require(completed.stdout == "", "empty-request must not print a report to stdout")
    require("usage:" in completed.stderr, "empty-request must explain usage on stderr")


def check_malformed_relation_id_fails_closed(executable: str) -> None:
    for malformed in ("not-a-relation-id", "cc.call_site", "cc.call_site.vX", ".v1", "UPPER.v1"):
        with_context = f"malformed id {malformed!r}"
        completed = run(executable, malformed)
        require(completed.returncode == 2, f"{with_context}: exit code was {completed.returncode}")
        require(completed.stdout == "", f"{with_context}: must not print a report to stdout")
        require(
            "sdk.relation-id-malformed" in completed.stderr,
            f"{with_context}: missing stable reason code on stderr",
        )


def check_duplicate_request_is_reported_independently(executable: str) -> None:
    completed = run(executable, "cc.call_site.v1", "cc.call_site.v1")
    require(completed.returncode == 0, "duplicate present request should still exit 0")
    report = json.loads(completed.stdout)
    require(report["requested"] == 2, "duplicate requests must both be reported")
    require(len(report["components"]) == 2, "duplicate requests must both appear in components")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    args = parser.parse_args()
    check_all_present(args.executable)
    check_unregistered_relation_is_missing(args.executable)
    check_major_version_mismatch_is_missing(args.executable)
    check_no_relations_requested_fails_closed(args.executable)
    check_malformed_relation_id_fails_closed(args.executable)
    check_duplicate_request_is_reported_independently(args.executable)
    return 0


if __name__ == "__main__":
    sys.exit(main())
