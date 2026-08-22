#!/usr/bin/env python3
"""Run the installed Clang 22 materializer through an actual-source E2E path."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Any


# This is the digest of the built-in Linux baseline policy in src/sdk/provider.cpp.
# The request is an external authority input, so the test binds it explicitly rather
# than allowing the materializer to substitute a first-match or ambient policy.
BASELINE_POLICY_DIGEST = (
    "semantic-v2:sha256:"
    "b4e95d8c88cf660fff40c4d9e7e4ae07bcb078013b5370c6b1abb80b0d75d375"
)
# The request schema's canonical integer domain is signed int64.  This value is
# used only when a sanitizer process needs to reserve its shadow address range.
ASAN_ADDRESS_SPACE_BYTES = (1 << 63) - 1
# LeakSanitizer creates runtime threads before the provider reaches main(). The
# production request remains at its normal subprocess budget; this explicit
# sanitizer-only profile matches the finite allowance used by sanitizer unit
# tests and keeps the process-limit distinction visible in the request digest.
ASAN_SUBPROCESS_BUDGET = 1024
OCCURRENCE_RELATIVE_PATH = (
    "share/cxxlens/materialization/clang22/occurrence-v1.json"
)
CANONICAL_BASE64_VECTOR_SOURCES = (
    b"int main() { return 0; }\n",  # RFC 4648 two-padding spelling.
    b"int unit_1() { return 1; }\n/*x*/",  # RFC 4648 one-padding spelling.
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--prefix", required=True, type=pathlib.Path)
    parser.add_argument(
        "--executable-suffix",
        default="",
        help=(
            "configured CMake executable suffix used to resolve the installed "
            "materializer and worker; manifest paths remain canonical"
        ),
    )
    parser.add_argument(
        "--backend", choices=("memory", "sqlite"), default="memory"
    )
    parser.add_argument("--translation-unit-count", type=int, default=2)
    parser.add_argument(
        "--canonical-base64-vectors",
        action="store_true",
        help="run actual installed provider tasks for one- and two-padding source spellings",
    )
    return parser.parse_args()


def fail(message: str) -> None:
    raise AssertionError(message)


def main() -> int:
    args = parse_args()
    args.root = args.root.resolve()
    args.prefix = args.prefix.resolve()
    sys.path.insert(0, str(args.root / "tools" / "quality"))
    import check_ng_clang22_materialization as oracle  # pylint: disable=import-error
    import check_ng_clang22_install_matrix as install_matrix  # pylint: disable=import-error

    occurrence_path = args.prefix / OCCURRENCE_RELATIVE_PATH
    if not occurrence_path.is_file():
        fail(f"installed occurrence manifest is missing: {occurrence_path}")
    occurrence_bytes = occurrence_path.read_bytes()
    try:
        occurrence = oracle.load_strict_json_bytes(
            occurrence_bytes, "installed occurrence manifest"
        )
        oracle.validate_occurrence_manifest(args.root, occurrence)
    except (json.JSONDecodeError, oracle.MaterializationError) as error:
        fail(f"installed occurrence manifest is invalid: {error}")
    if occurrence["package_configuration"] not in {"static", "shared"}:
        fail("installed occurrence package configuration is not closed")
    files = occurrence["files"]
    if len(files) < 2:
        fail("installed occurrence does not inventory both executables")

    source_factory = None
    if args.canonical_base64_vectors:
        if args.translation_unit_count != len(CANONICAL_BASE64_VECTOR_SOURCES):
            fail(
                "canonical Base64 vector acceptance requires exactly two translation units"
            )

        def source_factory(index: int) -> bytes:
            return CANONICAL_BASE64_VECTOR_SOURCES[index]

    request = oracle.sample_request(
        args.root,
        configuration=occurrence["package_configuration"],
        backend=args.backend,
        translation_unit_count=args.translation_unit_count,
        source_factory=source_factory,
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
    if os.environ.get("CXXLENS_ASAN_INSTALLED_TEST_PROFILE") == "1":
        # AddressSanitizer reserves a platform shadow range far beyond the
        # normal finite RLIMIT_AS budget before the worker reaches main().
        # This explicit CTest profile keeps the request binding visible while
        # reserving the sanitizer shadow range. Normal and release requests
        # continue to exercise their finite address-space budget.
        for task in request["tasks"]:
            task["budget"]["address_space_bytes"] = ASAN_ADDRESS_SPACE_BYTES
            task["budget"]["subprocesses"] = ASAN_SUBPROCESS_BUDGET
    for task in request["tasks"]:
        task["sandbox"]["policy_digest"] = BASELINE_POLICY_DIGEST
    oracle.bind_provider_task_identities(request)
    oracle.bind_task_execution_identities(request)
    oracle.bind_engine_policy_and_selector_identities(request)
    oracle.bind_request_identity(request)
    oracle.validate_request(args.root, request)
    if args.canonical_base64_vectors:
        expected_padding = ("==", "=")
        for task, padding in zip(request["tasks"], expected_padding, strict=True):
            spelling = task["source"]["content_base64"]
            if padding == "==":
                valid = spelling.endswith("==")
            else:
                valid = spelling.endswith("=") and not spelling.endswith("==")
            if not valid:
                fail(
                    "installed request did not preserve the expected canonical "
                    f"Base64 padding spelling: {spelling!r}"
                )
    request_bytes = oracle.canonical_json(request)

    environment = dict(os.environ)
    environment.pop("LD_LIBRARY_PATH", None)
    environment.pop("DYLD_LIBRARY_PATH", None)
    materializer = install_matrix.resolve_installed_executable(
        args.prefix,
        "bin/cxxlens-clang22-materialize",
        args.executable_suffix,
    )
    if not materializer.is_file() or materializer.is_symlink():
        fail(f"installed materializer is missing or not regular: {materializer}")
    # Keep mutable SQLite files outside the immutable install prefix. The
    # installed occurrence manifest is verified by sibling CTest jobs, so a
    # journal/WAL sidecar created under the prefix would race that exact file
    # census and make the package appear to change while it is being checked.
    with tempfile.TemporaryDirectory(
        prefix="clang22-materializer-e2e-"
    ) as working_directory:
        completed = subprocess.run(
            [str(materializer)],
            input=request_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=working_directory if args.backend == "sqlite" else args.prefix,
            env=environment,
            check=False,
        )
        sqlite_path = pathlib.Path(working_directory) / "materialization.sqlite"
        if args.backend == "sqlite" and not sqlite_path.is_file():
            fail("installed SQLite materializer did not leave a file-backed Store")
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
    install_matrix.validate_installed_task_v3_source_binding(request, report)

    incremental_execution = report["incremental_execution"]
    if (
        incremental_execution["schema"]
        != "cxxlens.ng-g5-production-execution-census.v1"
        or incremental_execution["planned_provider_executions"]
        != len(request["tasks"])
        or incremental_execution["planned_provider_task_executions"]
        != len(request["tasks"])
        or incremental_execution["actual_provider_executions"]
        != len(request["tasks"])
        or incremental_execution["warm_zero"]
    ):
        fail("installed success report incremental execution census differs")
    if (
        incremental_execution["actual_recomputed_partition_count"]
        != len(incremental_execution["executed_partition_ids"])
        or len(incremental_execution["executed_provider_task_ids"])
        != len(request["tasks"])
        or len(incremental_execution["executed_provider_execution_ids"])
        != len(request["tasks"])
        or len(
            set(incremental_execution["executed_provider_execution_ids"])
        )
        != len(request["tasks"])
        or len(incremental_execution["executed_artifact_digests"])
        != len(request["tasks"])
        or len(incremental_execution["executed_task_partition_set_digests"])
        != len(request["tasks"])
    ):
        fail("installed success report incremental execution receipt census differs")

    publication = report["publication"]
    if (
        publication["backend"] != args.backend
        or publication["selector"] != request["publication"]["selector"]
        or publication["outcome"] != "committed_verified"
        or publication["invocation_commit_state"] != "committed"
        or publication["committed_transaction_count"] != 1
        or publication["publication_attempted"] is not True
        or (args.backend == "memory" and publication["sqlite_effect_root_receipt"] is not None)
        or (args.backend == "sqlite" and publication["sqlite_effect_root_receipt"] is None)
    ):
        fail("installed success report publication is not committed and verified")
    authority_registry_digest = request["registry"]["authority_registry_digest"]
    engine_registry_digest = request["engine"]["engine_registry_digest"]
    store_selector_fields = report["store"]["selector"]["fields"]
    if authority_registry_digest == engine_registry_digest:
        fail("installed success request aliased authority and engine registry digests")
    if (
        publication["selector"]["relation_registry_digest"] != engine_registry_digest
        or publication["selector"]["relation_registry_digest"]
        == authority_registry_digest
        or store_selector_fields["relation_registry_digest"] != engine_registry_digest
        or report["store"]["snapshot_manifest"]["relation_registry_digest"]
        != engine_registry_digest
    ):
        fail("installed Store publication did not bind the exact admitted engine digest")
    if report["semantic_verification"]["status"] != "passed":
        fail("installed success report lacks reopened semantic verification")
    if not report["store"].get("snapshot_manifest") or not report["store"].get(
        "claim_batch_validation"
    ):
        fail("installed success report lacks verified Store observations")
    expected_projection, _ = oracle._reopened_handle_projection(
        request,
        report["store"],
        publication["invocation_committed_record"],
    )
    reopened_store = report["semantic_verification"]["reopened_store"]
    if reopened_store["canonical_export_digest"] != expected_projection[
        "canonical_export_digest"
    ]:
        fail("installed report canonical export digest differs from exact SDK export mirror")
    expected_selector = request["publication"]["selector"]
    expected_store_selector = report["store"]["selector"]
    expected_record = publication["invocation_committed_record"]
    expected_descriptors = sorted(
        request["engine"]["admitted_descriptors"],
        key=lambda row: row["descriptor_id"],
    )
    if (
        reopened_store["selector"] != expected_store_selector
        or reopened_store["publication_record"] != expected_record
        or reopened_store["descriptors"] != expected_descriptors
        or reopened_store["snapshot_manifest"]["relation_registry_digest"]
        != engine_registry_digest
    ):
        fail("installed reopened Store lost exact selector, record, or descriptor inventory")
    cursor_projection = reopened_store["cursor_projection"]
    cursor_relations = cursor_projection["relations"]
    if [row["relation_descriptor_id"] for row in cursor_relations] != [
        row["descriptor_id"] for row in expected_descriptors
    ]:
        fail("installed reopened query cursor lost or reordered an admitted descriptor")
    expected_descriptor_ids = {row["descriptor_id"] for row in expected_descriptors}
    if any(
        row["relation_descriptor_id"] not in expected_descriptor_ids
        or "row_canonical_forms" not in row
        or "claim_annotations" not in row
        or "coverage" not in row
        for row in cursor_relations
    ):
        fail("installed reopened query cursor contains an incomplete relation projection")
    expected_lookups = (
        {"selector": expected_store_selector},
        {"publication_id": expected_record["publication_id"]},
        {"snapshot_id": expected_record["snapshot_id"]},
    )
    for receipt, expected_lookup in zip(reopened_store["handle_receipts"], expected_lookups):
        projection = receipt["projection"]
        if (
            receipt["lookup"] != expected_lookup
            or projection["publication_record"] != expected_record
            or projection["descriptors"] != expected_descriptors
            or projection["snapshot_manifest"]["relation_registry_digest"]
            != engine_registry_digest
            or projection["cursor_projection_digest"] != cursor_projection["digest"]
            or projection["canonical_export_digest"]
            != reopened_store["canonical_export_digest"]
        ):
            fail("installed reopen receipt did not preserve exact lookup/query projection")
    if any(
        receipt["projection"]["canonical_export_digest"]
        != expected_projection["canonical_export_digest"]
        for receipt in reopened_store["handle_receipts"]
    ):
        fail("installed reopened handle receipt lost the exact SDK export digest")
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

    if args.translation_unit_count > 1 and not args.canonical_base64_vectors:
        direct_target_rows = [
            row
            for result in task_results
            for batch in result["batches"]
            if batch["descriptor_id"] == "cc.call_direct_target.v1"
            for row in batch["row_bindings"]
        ]
        if not direct_target_rows:
            fail("multi-TU installed source did not produce a direct-target row")
        target_compile_units = {
            row["final_relation_compile_unit_id"] for row in direct_target_rows
        }
        if len(target_compile_units) != 1:
            fail("direct-target rows did not retain one authoritative caller TU")
        entity_rows = [
            row
            for result in task_results
            for batch in result["batches"]
            if batch["descriptor_id"] == "cc.entity.v1"
            for row in batch["row_bindings"]
        ]
        entity_occurrences: dict[str, set[str]] = {}
        for row in entity_rows:
            canonical = json.loads(row["row_canonical_form"])
            entity_id = canonical["cells"]["cc.entity.v1.entity"]["value"]
            entity_occurrences.setdefault(entity_id, set()).add(
                row["final_relation_compile_unit_id"]
            )
        target_ids = {
            json.loads(row["row_canonical_form"])["cells"][
                "cc.call_direct_target.v1.target"
            ]["value"]
            for row in direct_target_rows
        }
        target_entity_units = set().union(
            *(entity_occurrences.get(target_id, set()) for target_id in target_ids)
        )
        if len(target_entity_units) < 2:
            fail(
                "multi-TU direct target did not resolve to the same entity across "
                "caller and separately defined target units"
            )

        if len({row["final_relation_compile_unit_id"] for row in entity_rows}) < 2:
            fail("multi-TU installed source did not retain both entity TUs")

    if completed.stdout != oracle.canonical_json(report):
        fail(
            "installed materializer stdout is not the canonical report artifact; "
            "an external receipt cannot bind a reformatted response"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
