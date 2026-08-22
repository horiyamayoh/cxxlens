#!/usr/bin/env python3
"""Exercise the installed materializer's request-bound installation phase union."""

from __future__ import annotations

import argparse
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
    parser.add_argument(
        "--executable-suffix",
        default="",
        help=(
            "configured CMake executable suffix used to resolve the installed "
            "materializer; manifest paths remain canonical"
        ),
    )
    return parser.parse_args()


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


def load_installed_request(
    root: pathlib.Path,
    prefix: pathlib.Path,
    oracle: Any,
    mismatched_role: str,
) -> tuple[dict[str, Any], bytes]:
    occurrence_path = prefix / OCCURRENCE_RELATIVE_PATH
    if not occurrence_path.is_file():
        fail(f"installed occurrence manifest is missing: {occurrence_path}")
    occurrence_bytes = occurrence_path.read_bytes()
    occurrence = oracle.load_strict_json_bytes(
        occurrence_bytes, "installed occurrence manifest"
    )
    oracle.validate_occurrence_manifest(root, occurrence)
    files = occurrence["files"]
    try:
        tool = next(file for file in files if file["role"] == "materializer-executable")
        worker = next(file for file in files if file["role"] == "worker-executable")
    except StopIteration as error:
        raise AssertionError("installed occurrence lacks tool or worker") from error

    request = oracle.sample_request(
        root,
        configuration=occurrence["package_configuration"],
        backend="memory",
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

    if mismatched_role == "materializer-executable":
        request["tool"]["installed_executable_digest"] = oracle.content_digest(
            b"not-the-installed-materializer"
        )
    elif mismatched_role == "worker-executable":
        request["worker"]["installed_binary_digest"] = oracle.content_digest(
            b"not-the-installed-worker"
        )
    else:
        raise AssertionError(f"unknown mismatch role: {mismatched_role}")

    oracle.bind_provider_task_identities(request)
    oracle.bind_task_execution_identities(request)
    oracle.bind_engine_policy_and_selector_identities(request)
    oracle.bind_request_identity(request)
    oracle.validate_request(root, request)
    return request, oracle.canonical_json(request)


def assert_installation_binding_failure(
    root: pathlib.Path,
    oracle: Any,
    materializer: pathlib.Path,
    request: dict[str, Any],
    payload: bytes,
    mismatched_role: str,
) -> None:
    label = f"installation-{mismatched_role}"
    with tempfile.TemporaryDirectory(prefix="clang22-materializer-binding-") as directory:
        completed = run_materializer(materializer, payload, pathlib.Path(directory))

    if completed.returncode != 1 or completed.stderr:
        fail(
            f"{label} did not return one schema-valid failure: "
            f"status={completed.returncode}, stdout={completed.stdout[:2000]!r}, "
            f"stderr={completed.stderr[:2000]!r}"
        )
    report = oracle.load_strict_json_bytes(completed.stdout, f"{label} stdout")
    oracle.validate_report(root, request, report, request_bytes=payload)
    if completed.stdout != oracle.canonical_json(report) + b"\n":
        fail(f"{label} response is not canonical JSON")

    expected_field = (
        "materializer-executable"
        if mismatched_role == "materializer-executable"
        else "worker-executable"
    )
    expected_detail = (
        "request-or-self-mismatch"
        if mismatched_role == "materializer-executable"
        else "request-mismatch"
    )
    expected = {
        "code": "materialization.identity-mismatch",
        "diagnostic": (
            "source-code=materialization.identity-mismatch;"
            f"source-field={expected_field};source-detail={expected_detail}"
        ),
        "phase": "installation-binding",
        "subject": request["materialization_request_id"],
    }
    if report["response_kind"] != "compact_failure" or report["result"] != "failed":
        fail(f"{label} did not return compact_failure/failed")
    if report["error"] != expected:
        fail(f"{label} changed the phase-authentic error: {report['error']!r}")
    if report["binding"] != {
        "request": {
            "materialization_request_id": request["materialization_request_id"],
            "request_digest": request["request_digest"],
            "semantic_request_digest": request["semantic_request_digest"],
        },
        "state": "request-bound",
    }:
        fail(f"{label} lost the authenticated request binding")
    if report["effects"] != {
        "committed_transaction_count": 0,
        "head_observation": "not-observed",
        "observed_head_publication": None,
        "prior_history_retained": True,
        "publication_attempted": False,
        "store_draft_state": "not-created",
        "store_failure_cause": None,
        "task_attempt_count": 0,
        "task_success_count": 0,
        "worker_launch_attempt_count": 0,
        "worker_launch_success_count": 0,
    }:
        fail(f"{label} crossed an effect boundary: {report['effects']!r}")
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

    for role in ("materializer-executable", "worker-executable"):
        request, payload = load_installed_request(root, prefix, oracle, role)
        assert_installation_binding_failure(root, oracle, materializer, request, payload, role)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
