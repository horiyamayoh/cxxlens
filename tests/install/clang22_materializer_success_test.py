#!/usr/bin/env python3
"""Run the installed Clang 22 materializer through its positive memory path."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import sys
from typing import Any


# This is the digest of the built-in Linux baseline policy in src/sdk/provider.cpp.
# The request is an external authority input, so the test binds it explicitly rather
# than allowing the materializer to substitute a first-match or ambient policy.
BASELINE_POLICY_DIGEST = (
    "semantic-v2:sha256:"
    "b4e95d8c88cf660fff40c4d9e7e4ae07bcb078013b5370c6b1abb80b0d75d375"
)
OCCURRENCE_RELATIVE_PATH = (
    "share/cxxlens/materialization/clang22/occurrence-v1.json"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--prefix", required=True, type=pathlib.Path)
    return parser.parse_args()


def fail(message: str) -> None:
    raise AssertionError(message)


def main() -> int:
    args = parse_args()
    sys.path.insert(0, str(args.root / "tools" / "quality"))
    import check_ng_clang22_materialization as oracle  # pylint: disable=import-error

    occurrence_path = args.prefix / OCCURRENCE_RELATIVE_PATH
    if not occurrence_path.is_file():
        fail(f"installed occurrence manifest is missing: {occurrence_path}")
    occurrence_bytes = occurrence_path.read_bytes()
    occurrence = json.loads(occurrence_bytes)
    if occurrence["package_configuration"] not in {"static", "shared"}:
        fail("installed occurrence package configuration is not closed")
    files = occurrence["files"]
    if len(files) < 2:
        fail("installed occurrence does not inventory both executables")

    request = oracle.sample_request(
        args.root,
        configuration=occurrence["package_configuration"],
        backend="memory",
    )
    request["tool"].update(
        source_revision=occurrence["source_revision"],
        source_tree=occurrence["source_tree"],
        installed_executable_digest=files[0]["digest"],
        # The C++ runtime authenticates the exact manifest file bytes, including
        # its install-generated framing.  The payload digest remains canonical JSON.
        occurrence_manifest_digest=oracle.content_digest(occurrence_bytes),
    )
    request["worker"].update(
        installed_binary_digest=files[1]["digest"],
        sandbox_policy_digest=BASELINE_POLICY_DIGEST,
    )
    for task in request["tasks"]:
        task["sandbox"]["policy_digest"] = BASELINE_POLICY_DIGEST
    oracle.bind_provider_task_identities(request)
    oracle.bind_task_execution_identities(request)
    oracle.bind_engine_policy_and_selector_identities(request)
    oracle.bind_request_identity(request)
    oracle.validate_request(args.root, request)
    request_bytes = oracle.canonical_json(request)

    environment = dict(os.environ)
    environment.pop("LD_LIBRARY_PATH", None)
    environment.pop("DYLD_LIBRARY_PATH", None)
    materializer = args.prefix / "bin" / "cxxlens-clang22-materialize"
    completed = subprocess.run(
        [str(materializer)],
        input=request_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=args.prefix,
        env=environment,
        check=False,
    )
    if completed.returncode != 0:
        fail(
            "installed materializer did not publish success: "
            f"returncode={completed.returncode}, stdout={completed.stdout[:2000]!r}, "
            f"stderr={completed.stderr[:2000]!r}"
        )
    if completed.stderr:
        fail(f"installed materializer wrote stderr: {completed.stderr[:2000]!r}")
    try:
        report: dict[str, Any] = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        fail(f"installed materializer did not emit JSON: {error}")

    oracle.validate_schema(
        report,
        oracle.load(args.root / oracle.REPORT_SCHEMA),
        "installed materializer positive report",
        error_code="materialization.report-invalid",
    )
    if report["response_kind"] != "detailed" or report["result"] != "passed":
        fail("installed materializer positive path did not return detailed passed")
    if report["process_exit_status"] != 0 or report["error"] is not None:
        fail("installed materializer success report retained a failure")
    if report["raw_input_observation"] != oracle.raw_input_observation(request_bytes):
        fail("installed success report lost exact request byte observation")
    if report["request"] != oracle._request_binding(request):
        fail("installed success report request binding differs")
    if report["source"] != {
        "revision": request["tool"]["source_revision"],
        "tree": request["tool"]["source_tree"],
    }:
        fail("installed success report source provenance differs")
    if report["authority_digests"] != oracle.authority_bindings(args.root):
        fail("installed success report authority digest census differs")
    if report["installation"]["requested"] != {
        "occurrence_manifest_digest": request["tool"]["occurrence_manifest_digest"]
    }:
        fail("installed success report occurrence request binding differs")
    measured = report["installation"]["measured"]
    if (
        measured["manifest_path"] != OCCURRENCE_RELATIVE_PATH
        or measured["manifest_file_digest"]
        != request["tool"]["occurrence_manifest_digest"]
        or measured["source_revision"] != occurrence["source_revision"]
        or measured["source_tree"] != occurrence["source_tree"]
        or measured["configuration"] != occurrence["package_configuration"]
        or measured["files"] != files
        or measured["tool"]
        != {"path": files[0]["path"], "digest": files[0]["digest"]}
        or measured["worker"]
        != {"path": files[1]["path"], "digest": files[1]["digest"]}
    ):
        fail("installed success report measured occurrence differs")
    if report["provider"] != {
        "tool_executable": request["tool"]["executable"],
        "tool_interface_version": request["tool"]["interface_version"],
        "worker_executable": request["worker"]["executable"],
        "provider_id": request["worker"]["provider_id"],
        "provider_version": request["worker"]["provider_version"],
        "semantic_contract_digest": request["worker"]["semantic_contract_digest"],
        "protocol_major": request["worker"]["protocol_major"],
        "protocol_minor": request["worker"]["protocol_minor"],
        "required_features": request["worker"]["required_features"],
        "sandbox_policy_digest": BASELINE_POLICY_DIGEST,
    }:
        fail("installed success report provider binding differs")

    publication = report["publication"]
    if (
        publication["backend"] != "memory"
        or publication["selector"] != request["publication"]["selector"]
        or publication["outcome"] != "committed_verified"
        or publication["invocation_commit_state"] != "committed"
        or publication["committed_transaction_count"] != 1
        or publication["publication_attempted"] is not True
        or publication["sqlite_effect_root_receipt"] is not None
    ):
        fail("installed success report publication is not committed and verified")
    if report["semantic_verification"]["status"] != "passed":
        fail("installed success report lacks reopened semantic verification")
    if not report["store"].get("snapshot_manifest") or not report["store"].get(
        "claim_batch_validation"
    ):
        fail("installed success report lacks verified Store observations")
    if report["adoption"]["state"] != "sealed":
        fail("installed success report adopted an unsealed result")
    if (
        report["adoption"]["all_tasks_mandatory"] is not True
        or report["adoption"]["all_groups_mandatory"] is not True
        or report["adoption"]["all_batches_mandatory"] is not True
        or report["adoption"]["raw_frames"]["retained"] is not False
    ):
        fail("installed success report weakened mandatory sealed adoption")

    task_results = report["task_results"]
    if len(task_results) != len(request["tasks"]):
        fail("installed success report task census differs")
    for result in task_results:
        if result["terminal"] != "provider.success":
            fail("installed success report contains a non-success task")
        if len(result["batches"]) != len(oracle.DESCRIPTOR_IDS):
            fail("installed success report does not materialize all descriptors")
        if {batch["descriptor_id"] for batch in result["batches"]} != set(
            oracle.DESCRIPTOR_IDS
        ):
            fail("installed success report descriptor census differs")
        if not all(batch["sealed"] for batch in result["batches"]):
            fail("installed success report contains an unsealed batch")
        if not result["coverage"]["transport_records"] or not result["coverage"][
            "semantic_records"
        ]:
            fail("installed success report dropped transport or semantic coverage")
        if result["runtime_receipt"]["frame_count"] <= 0:
            fail("installed success report has no validated provider frames")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
