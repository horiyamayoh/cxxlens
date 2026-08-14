#!/usr/bin/env python3
"""Produce one exact installed-production G5 coordinator evidence document.

This producer deliberately keeps all mutable SQLite state outside the source
checkout and treats the installed materializer's successful report as the
authority for execution counts, publication, and semantic reopen.  It does
not derive a receipt from the planner or from a Python-side model.
"""

from __future__ import annotations

import argparse
from collections import Counter
import copy
import hashlib
import json
import os
import pathlib
import re
import sqlite3
import subprocess
import sys
import tempfile
from typing import Any
from urllib.parse import quote


OCCURRENCE_RELATIVE_PATH = (
    "share/cxxlens/materialization/clang22/occurrence-v1.json"
)
BASELINE_POLICY_DIGEST = (
    "semantic-v2:sha256:"
    "b4e95d8c88cf660fff40c4d9e7e4ae07bcb078013b5370c6b1abb80b0d75d375"
)
EVIDENCE_SCHEMA = "cxxlens.ng-g5-production-coordinator-evidence.v1"
EXECUTION_CENSUS_SCHEMA = "cxxlens.ng-g5-production-execution-census.v1"
MATERIALIZATION_ARTIFACT_PATTERN = re.compile(
    r"^materialization\.incremental-sealed-artifact:sha256:[0-9a-f]{64}$"
)
PARTITION_SET_PATTERN = re.compile(
    r"^materialization\.incremental-task-partition-set:sha256:[0-9a-f]{64}$"
)
STRONG_ID_PATTERN = re.compile(r"^[^\x00-\x1f\x7f]+$")
SHA256_PATTERN = re.compile(r"^sha256:[0-9a-f]{64}$")
REVISION_PATTERN = re.compile(r"^[0-9a-f]{40}$")
DEFAULT_TIMEOUT_SECONDS = 600
MAX_TIMEOUT_SECONDS = 900
MIN_TRANSLATION_UNITS = 2
MAX_TRANSLATION_UNITS = 8


class EvidenceProductionError(RuntimeError):
    """The required installed-production observation could not be established."""


def fail(message: str) -> None:
    raise EvidenceProductionError(message)


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def content_digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run the installed Clang 22 materializer through cold, warm-zero, "
            "affected-only, and independent SQLite parity paths."
        )
    )
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--prefix", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument(
        "--production-report",
        required=True,
        type=pathlib.Path,
        help="path for the exact affected-only production report bytes",
    )
    parser.add_argument(
        "--work-directory",
        type=pathlib.Path,
        help="empty directory outside the checkout in which SQLite state is retained",
    )
    parser.add_argument(
        "--translation-unit-count",
        type=int,
        default=MIN_TRANSLATION_UNITS,
        help=f"bounded fixture task count ({MIN_TRANSLATION_UNITS}-{MAX_TRANSLATION_UNITS})",
    )
    parser.add_argument(
        "--changed-task-index",
        type=int,
        default=0,
        help="zero-based task whose condition identity is changed for affected-only",
    )
    parser.add_argument(
        "--timeout-seconds",
        type=int,
        default=DEFAULT_TIMEOUT_SECONDS,
        help=f"per-process timeout ({DEFAULT_TIMEOUT_SECONDS}-{MAX_TIMEOUT_SECONDS})",
    )
    parser.add_argument(
        "--expected-revision",
        help="optional exact 40-hex revision that must match the local checkout",
    )
    return parser.parse_args()


def _run_git(root: pathlib.Path, *arguments: str) -> str:
    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    try:
        completed = subprocess.run(
            ["git", "-C", str(root), *arguments],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
        )
    except OSError as error:
        fail(f"cannot observe local Git state for {' '.join(arguments)}: {error}")
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        fail(
            f"git {' '.join(arguments)} failed with {completed.returncode}"
            + (f": {detail}" if detail else "")
        )
    try:
        return completed.stdout.decode("utf-8", errors="strict").strip()
    except UnicodeDecodeError as error:
        fail(f"git {' '.join(arguments)} returned non-UTF-8 output: {error}")


def git_state(root: pathlib.Path) -> dict[str, Any]:
    top_level = pathlib.Path(_run_git(root, "rev-parse", "--show-toplevel")).resolve()
    if top_level != root:
        fail(f"--root is not the Git checkout top level: {root} != {top_level}")
    revision = _run_git(root, "rev-parse", "--verify", "HEAD")
    tree = _run_git(root, "rev-parse", "--verify", "HEAD^{tree}")
    branch = _run_git(root, "branch", "--show-current")
    status = _run_git(
        root,
        "status",
        "--porcelain=v1",
        "--untracked-files=all",
    )
    if REVISION_PATTERN.fullmatch(revision) is None:
        fail(f"local HEAD is not an exact lowercase revision: {revision!r}")
    if REVISION_PATTERN.fullmatch(tree) is None:
        fail(f"local HEAD tree is not an exact lowercase tree: {tree!r}")
    state = {
        "revision": revision,
        "tree": tree,
        "branch": branch,
        "clean": status == "",
    }
    if state["branch"] != "main":
        fail(f"G5 production evidence requires clean main, observed {state}")
    if not state["clean"]:
        fail(f"G5 production evidence requires an exact clean checkout: {state}")
    return state


def _outside(path: pathlib.Path, root: pathlib.Path, label: str) -> None:
    try:
        path.relative_to(root)
    except ValueError:
        return
    fail(f"{label} must be outside the source checkout: {path}")


def _load_oracle(root: pathlib.Path) -> Any:
    quality_directory = root / "tools" / "quality"
    if str(quality_directory) not in sys.path:
        sys.path.insert(0, str(quality_directory))
    try:
        import check_ng_clang22_materialization as oracle  # type: ignore
        import check_ng_g5_qualification as g5  # type: ignore
    except ImportError as error:
        fail(f"qualification oracle import failed: {error}")
    return oracle, g5


def _load_strict_json(oracle: Any, raw: bytes, label: str) -> dict[str, Any]:
    try:
        return oracle.load_strict_json_bytes(
            raw,
            label,
            error_code="materialization.report-invalid",
        )
    except Exception as error:
        fail(f"{label} is not strict report JSON: {error}")


def _read_occurrence(root: pathlib.Path, prefix: pathlib.Path, oracle: Any) -> tuple[dict[str, Any], bytes]:
    path = prefix / OCCURRENCE_RELATIVE_PATH
    if not path.is_file() or path.is_symlink():
        fail(f"installed occurrence manifest is not a regular file: {path}")
    try:
        raw = path.read_bytes()
    except OSError as error:
        fail(f"cannot read installed occurrence manifest {path}: {error}")
    occurrence = _load_strict_json(oracle, raw, "installed occurrence manifest")
    try:
        oracle.validate_occurrence_manifest(root, occurrence)
    except Exception as error:
        fail(f"installed occurrence manifest failed authority validation: {error}")
    if occurrence.get("package_configuration") not in {"static", "shared"}:
        fail("installed occurrence package configuration is not closed")
    if occurrence.get("source_revision") is None or occurrence.get("source_tree") is None:
        fail("installed occurrence lacks exact source provenance")
    files = occurrence.get("files")
    if not isinstance(files, list) or len(files) < 2:
        fail("installed occurrence does not inventory both executables")
    roles = [row.get("role") for row in files if isinstance(row, dict)]
    if roles.count("materializer-executable") != 1 or roles.count("worker-executable") != 1:
        fail("installed occurrence executable role census is not unique")
    return occurrence, raw


def _inventory_file(
    prefix: pathlib.Path,
    occurrence: dict[str, Any],
    role: str,
) -> tuple[pathlib.Path, str]:
    rows = [row for row in occurrence["files"] if row["role"] == role]
    if len(rows) != 1:
        fail(f"installed occurrence role is not unique: {role}")
    row = rows[0]
    relative = pathlib.PurePosixPath(row["path"])
    if relative.is_absolute() or ".." in relative.parts:
        fail(f"installed occurrence path is not prefix-relative: {row['path']!r}")
    path = prefix.joinpath(*relative.parts)
    if path.is_symlink() or not path.is_file():
        fail(f"installed occurrence role is not a regular file: {path}")
    actual_digest = content_digest(path.read_bytes())
    if actual_digest != row["digest"]:
        fail(
            f"installed {role} digest differs from occurrence manifest: "
            f"{actual_digest} != {row['digest']}"
        )
    return path, actual_digest


def _installed_request(
    root: pathlib.Path,
    prefix: pathlib.Path,
    occurrence: dict[str, Any],
    occurrence_bytes: bytes,
    oracle: Any,
    translation_unit_count: int,
) -> dict[str, Any]:
    _tool_path, tool_digest = _inventory_file(
        prefix, occurrence, "materializer-executable"
    )
    _worker_path, worker_digest = _inventory_file(prefix, occurrence, "worker-executable")
    request = oracle.sample_request(
        root,
        configuration=occurrence["package_configuration"],
        backend="sqlite",
        translation_unit_count=translation_unit_count,
    )
    request["tool"].update(
        source_revision=occurrence["source_revision"],
        source_tree=occurrence["source_tree"],
        installed_executable_digest=tool_digest,
        occurrence_manifest_digest=content_digest(occurrence_bytes),
    )
    request["worker"].update(
        installed_binary_digest=worker_digest,
        sandbox_policy_digest=BASELINE_POLICY_DIGEST,
    )
    for task in request["tasks"]:
        task["sandbox"]["policy_digest"] = BASELINE_POLICY_DIGEST
    oracle.bind_provider_task_identities(request)
    oracle.bind_task_execution_identities(request)
    oracle.bind_engine_policy_and_selector_identities(request)
    oracle.bind_request_identity(request)
    try:
        oracle.validate_request(root, request)
    except Exception as error:
        fail(f"installed production request fixture failed validation: {error}")
    return request


def _rebind_request(
    root: pathlib.Path,
    oracle: Any,
    request: dict[str, Any],
    *,
    genesis: bool,
    parent_publication: str | None,
) -> None:
    request["publication"]["genesis"] = genesis
    request["publication"]["expected_parent_publication"] = parent_publication
    oracle.bind_request_identity(request)
    try:
        oracle.validate_request(root, request)
    except Exception as error:
        fail(f"derived production request failed validation: {error}")


def _run_materializer(
    materializer: pathlib.Path,
    request: dict[str, Any],
    work_directory: pathlib.Path,
    oracle: Any,
    root: pathlib.Path,
    timeout_seconds: int,
    label: str,
    binary_digest: str,
    occurrence: dict[str, Any],
) -> tuple[dict[str, Any], bytes]:
    request_bytes = oracle.canonical_json(request)
    environment = dict(os.environ)
    environment.pop("LD_LIBRARY_PATH", None)
    environment.pop("DYLD_LIBRARY_PATH", None)
    try:
        completed = subprocess.run(
            [str(materializer)],
            input=request_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=work_directory,
            env=environment,
            check=False,
            timeout=timeout_seconds,
        )
    except subprocess.TimeoutExpired as error:
        fail(f"{label} installed materializer exceeded {timeout_seconds}s: {error}")
    except OSError as error:
        fail(f"{label} installed materializer could not start: {error}")
    if completed.returncode != 0:
        fail(
            f"{label} installed materializer did not publish success: "
            f"returncode={completed.returncode}, stdout={completed.stdout[:2000]!r}, "
            f"stderr={completed.stderr[:2000]!r}"
        )
    if completed.stderr:
        fail(f"{label} installed materializer wrote stderr: {completed.stderr[:2000]!r}")
    report = _load_strict_json(oracle, completed.stdout, f"{label} installed report")
    try:
        oracle.validate_schema(
            report,
            oracle.load(root / oracle.REPORT_SCHEMA),
            f"{label} installed report",
            error_code="materialization.report-invalid",
        )
    except Exception as error:
        fail(f"{label} installed report failed schema validation: {error}")
    if completed.stdout != oracle.canonical_json(report):
        fail(f"{label} report stdout is not the canonical report artifact")
    try:
        runtime_raw_occurrences = oracle.report_runtime_raw_occurrences(
            root,
            request,
            report,
        )
        oracle.validate_report(
            root,
            request,
            report,
            request_bytes=request_bytes,
            runtime_raw_occurrences=runtime_raw_occurrences,
        )
    except Exception as error:
        fail(f"{label} report failed the independent materialization validator: {error}")
    if (
        report.get("response_kind") != "detailed"
        or report.get("result") != "passed"
        or report.get("process_exit_status") != 0
        or report.get("error") is not None
    ):
        fail(f"{label} report is not a detailed passed response")
    if report.get("request") != oracle._request_binding(request):
        fail(f"{label} report request binding differs from the submitted request")
    if report.get("raw_input_observation") != oracle.raw_input_observation(request_bytes):
        fail(f"{label} report lost the exact request byte observation")
    if report.get("source") != {
        "revision": request["tool"]["source_revision"],
        "tree": request["tool"]["source_tree"],
    }:
        fail(f"{label} report source provenance differs from the installed request")
    measured = report.get("installation", {}).get("measured", {})
    if (
        measured.get("source_revision") != occurrence["source_revision"]
        or measured.get("source_tree") != occurrence["source_tree"]
        or measured.get("configuration") != occurrence["package_configuration"]
        or measured.get("files") != occurrence["files"]
        or measured.get("tool") != {
            "path": "bin/cxxlens-clang22-materialize",
            "digest": binary_digest,
        }
        or report["installation"]["requested"]
        != {"occurrence_manifest_digest": request["tool"]["occurrence_manifest_digest"]}
    ):
        fail(f"{label} report lost the exact installed occurrence binding")
    publication = report.get("publication", {})
    if (
        publication.get("backend") != "sqlite"
        or publication.get("genesis") != request["publication"]["genesis"]
        or publication.get("expected_parent_publication")
        != request["publication"]["expected_parent_publication"]
        or publication.get("publication_attempted") is not True
        or publication.get("outcome") != "committed_verified"
        or publication.get("invocation_commit_state") != "committed"
        or publication.get("committed_transaction_count") != 1
        or publication.get("invocation_committed_record") is None
        or publication.get("candidate_visibility") != "present_by_invocation"
        or publication.get("prior_history_retained") is not True
        or publication.get("prior_artifact_persistence", {}).get("state") != "committed"
        or publication.get("prior_artifact_persistence", {}).get("error") is not None
        or publication.get("sqlite_reopen_status") != "opened"
    ):
        fail(f"{label} report does not prove one committed SQLite publication")
    verification = report.get("semantic_verification", {})
    reopened = verification.get("reopened_store")
    if verification.get("status") != "passed" or not isinstance(reopened, dict):
        fail(f"{label} report does not prove semantic Store reopen")
    canonical_export_digest = reopened.get("canonical_export_digest")
    if not isinstance(canonical_export_digest, str) or not canonical_export_digest.startswith(
        "sha256:"
    ):
        fail(f"{label} reopened Store lacks a canonical export digest")
    receipts = reopened.get("handle_receipts")
    if not isinstance(receipts, list) or not receipts:
        fail(f"{label} reopened Store lacks handle receipts")
    if any(
        receipt.get("projection", {}).get("canonical_export_digest")
        != canonical_export_digest
        for receipt in receipts
    ):
        fail(f"{label} reopened handle receipts disagree with the canonical export digest")
    census = report.get("incremental_execution")
    if not isinstance(census, dict) or census.get("schema") != EXECUTION_CENSUS_SCHEMA:
        fail(f"{label} report lacks the production execution census")
    return report, completed.stdout


def _require_uint(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        fail(f"{label} is not a non-negative integer")
    return value


def _require_strings(value: Any, label: str) -> list[str]:
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        fail(f"{label} is not a string array")
    return value


def _require_unique_strings(value: Any, label: str) -> list[str]:
    value = _require_strings(value, label)
    if len(value) != len(set(value)):
        fail(f"{label} contains duplicate values")
    return value


def _validate_census(
    report: dict[str, Any],
    request: dict[str, Any],
    label: str,
    *,
    expected_actual: int,
    expected_task_ids: list[str],
    expected_execution_ids: list[str],
) -> dict[str, Any]:
    census = report["incremental_execution"]
    planned = _require_uint(census.get("planned_provider_executions"), f"{label} planned executions")
    planned_tasks = _require_uint(
        census.get("planned_provider_task_executions"),
        f"{label} planned task executions",
    )
    actual = _require_uint(census.get("actual_provider_executions"), f"{label} actual executions")
    partitions_count = _require_uint(
        census.get("actual_recomputed_partition_count"),
        f"{label} recomputed partition count",
    )
    if (
        planned != len(request["tasks"])
        or planned_tasks != len(request["tasks"])
        or actual != expected_actual
        or actual > planned
    ):
        fail(f"{label} execution counters are not the expected bounded census")
    # These are execution-event arrays, not sets. The production contract
    # permits repeated provider-task IDs and flattened partition IDs.
    partition_ids = _require_strings(census.get("executed_partition_ids"), f"{label} partitions")
    task_ids = _require_strings(census.get("executed_provider_task_ids"), f"{label} provider tasks")
    execution_ids = _require_unique_strings(
        census.get("executed_provider_execution_ids"),
        f"{label} provider executions",
    )
    artifact_digests = _require_unique_strings(
        census.get("executed_artifact_digests"),
        f"{label} artifact digests",
    )
    partition_set_digests = _require_unique_strings(
        census.get("executed_task_partition_set_digests"),
        f"{label} partition-set digests",
    )
    if any(not STRONG_ID_PATTERN.fullmatch(value) for value in partition_ids + task_ids + execution_ids):
        fail(f"{label} contains an invalid strong ID")
    if any(not MATERIALIZATION_ARTIFACT_PATTERN.fullmatch(value) for value in artifact_digests):
        fail(f"{label} contains an invalid artifact digest domain")
    if any(not PARTITION_SET_PATTERN.fullmatch(value) for value in partition_set_digests):
        fail(f"{label} contains an invalid partition-set digest domain")
    if (
        len(task_ids) != actual
        or len(execution_ids) != actual
        or len(artifact_digests) != actual
        or len(partition_set_digests) != actual
        or len(partition_ids) != partitions_count
    ):
        fail(f"{label} array cardinalities do not match the actual census")
    if bool(census.get("warm_zero")) != (actual == 0):
        fail(f"{label} warm-zero flag disagrees with actual provider executions")
    if Counter(task_ids) != Counter(expected_task_ids):
        fail(f"{label} provider-task ID multiset differs from expected execution events")
    if Counter(execution_ids) != Counter(expected_execution_ids):
        fail(f"{label} provider execution ID multiset differs from expected execution events")
    request_task_ids = {task["provider_task_id"] for task in request["tasks"]}
    if any(task_id not in request_task_ids for task_id in task_ids):
        fail(f"{label} executed a provider task absent from its request")
    request_execution_ids = {
        task["provider_execution_id"] for task in request["tasks"]
    }
    if any(execution_id not in request_execution_ids for execution_id in execution_ids):
        fail(f"{label} executed a provider execution absent from its request")
    if actual == 0 and any(
        (task_ids, execution_ids, artifact_digests, partition_set_digests, partition_ids)
    ):
        fail(f"{label} warm-zero execution census is not empty")
    return census


def _publication_id(report: dict[str, Any], label: str) -> str:
    record = report["publication"]["invocation_committed_record"]
    publication_id = record.get("publication_id") if isinstance(record, dict) else None
    if not isinstance(publication_id, str) or not publication_id:
        fail(f"{label} report lacks a committed publication ID")
    return publication_id


def _readonly_sqlite_reopen(database: pathlib.Path, label: str) -> None:
    if database.is_symlink() or not database.is_file():
        fail(f"{label} SQLite database is not a regular file: {database}")
    before = sorted(path.name for path in database.parent.iterdir())
    uri = "file:" + quote(str(database), safe="/") + "?mode=ro"
    try:
        with sqlite3.connect(uri, uri=True, timeout=5.0) as connection:
            result = connection.execute("PRAGMA integrity_check").fetchone()
            if result != ("ok",):
                fail(f"{label} independent SQLite integrity check failed: {result!r}")
            connection.execute("PRAGMA schema_version").fetchone()
    except sqlite3.Error as error:
        fail(f"{label} independent SQLite reopen failed: {error}")
    after = sorted(path.name for path in database.parent.iterdir())
    if before != after:
        fail(f"{label} read-only SQLite reopen changed its directory namespace")


def _work_directory(root: pathlib.Path, requested: pathlib.Path | None) -> Any:
    if requested is None:
        return tempfile.TemporaryDirectory(prefix="cxxlens-g5-production-")
    path = requested.resolve()
    _outside(path, root, "--work-directory")
    if path.exists():
        if not path.is_dir() or any(path.iterdir()):
            fail(f"--work-directory must be a new or empty directory: {path}")
    else:
        path.mkdir(parents=True)
    class RetainedDirectory:
        def __enter__(self) -> str:
            return str(path)

        def __exit__(self, _type: Any, _value: Any, _traceback: Any) -> None:
            return None

    return RetainedDirectory()


def _write_verified_output(
    output: pathlib.Path,
    evidence: dict[str, Any],
    root: pathlib.Path,
    g5: Any,
    expected_revision: str,
    production_binary: pathlib.Path,
    production_report: pathlib.Path,
) -> None:
    if output.exists() or output.is_symlink():
        fail(f"refusing to overwrite an existing evidence output: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    if temporary.exists() or temporary.is_symlink():
        fail(f"temporary evidence output already exists: {temporary}")
    try:
        temporary.write_bytes(canonical_json(evidence) + b"\n")
        g5.validate_production_coordinator_evidence(
            root,
            temporary,
            expected_revision,
            production_binary,
            production_report,
        )
        os.replace(temporary, output)
    except Exception as error:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        fail(f"production evidence failed the owning G5 checker: {error}")


def produce(arguments: argparse.Namespace) -> None:
    root = arguments.root.resolve()
    prefix = arguments.prefix.resolve()
    output = arguments.output.resolve()
    production_report = arguments.production_report.resolve()
    if arguments.translation_unit_count < MIN_TRANSLATION_UNITS or arguments.translation_unit_count > MAX_TRANSLATION_UNITS:
        fail(
            "translation-unit-count must be bounded to "
            f"[{MIN_TRANSLATION_UNITS}, {MAX_TRANSLATION_UNITS}]"
        )
    if arguments.changed_task_index < 0 or arguments.changed_task_index >= arguments.translation_unit_count:
        fail("changed-task-index is outside the translation-unit range")
    if arguments.timeout_seconds < DEFAULT_TIMEOUT_SECONDS or arguments.timeout_seconds > MAX_TIMEOUT_SECONDS:
        fail(f"timeout-seconds must be bounded to [{DEFAULT_TIMEOUT_SECONDS}, {MAX_TIMEOUT_SECONDS}]")
    _outside(output, root, "--output")
    _outside(production_report, root, "--production-report")
    if output.exists() or output.is_symlink():
        fail(f"refusing to overwrite an existing evidence output: {output}")
    if production_report.exists() or production_report.is_symlink():
        fail(f"refusing to overwrite an existing production report: {production_report}")

    initial_git = git_state(root)
    if arguments.expected_revision is not None:
        if REVISION_PATTERN.fullmatch(arguments.expected_revision) is None:
            fail("expected-revision is not an exact lowercase 40-hex revision")
        if arguments.expected_revision != initial_git["revision"]:
            fail(
                "expected-revision differs from the exact local HEAD: "
                f"{arguments.expected_revision} != {initial_git['revision']}"
            )
    oracle, g5 = _load_oracle(root)
    try:
        g5.validate_documents(root)
    except Exception as error:
        fail(f"G5 authority/checker documents are not valid: {error}")

    occurrence, occurrence_bytes = _read_occurrence(root, prefix, oracle)
    if (
        occurrence["source_revision"] != initial_git["revision"]
        or occurrence["source_tree"] != initial_git["tree"]
    ):
        fail(
            "installed occurrence is not bound to the exact local Git state: "
            f"occurrence=({occurrence['source_revision']}, {occurrence['source_tree']}), "
            f"local=({initial_git['revision']}, {initial_git['tree']})"
        )
    materializer, binary_digest = _inventory_file(
        prefix, occurrence, "materializer-executable"
    )
    request = _installed_request(
        root,
        prefix,
        occurrence,
        occurrence_bytes,
        oracle,
        arguments.translation_unit_count,
    )

    with _work_directory(root, arguments.work_directory) as work_root_text:
        work_root = pathlib.Path(work_root_text).resolve()
        state_directory = work_root / "state"
        independent_directory = work_root / "independent"
        state_directory.mkdir()
        independent_directory.mkdir()

        baseline_request = copy.deepcopy(request)
        _rebind_request(root, oracle, baseline_request, genesis=True, parent_publication=None)
        baseline_report, _baseline_bytes = _run_materializer(
            materializer,
            baseline_request,
            state_directory,
            oracle,
            root,
            arguments.timeout_seconds,
            "cold baseline",
            binary_digest,
            occurrence,
        )
        _validate_census(
            baseline_report,
            baseline_request,
            "cold baseline",
            expected_actual=arguments.translation_unit_count,
            expected_task_ids=[
                task["provider_task_id"] for task in baseline_request["tasks"]
            ],
            expected_execution_ids=[
                task["provider_execution_id"] for task in baseline_request["tasks"]
            ],
        )
        baseline_publication = _publication_id(baseline_report, "cold baseline")

        warm_request = copy.deepcopy(baseline_request)
        _rebind_request(
            root,
            oracle,
            warm_request,
            genesis=False,
            parent_publication=baseline_publication,
        )
        if warm_request["tasks"] != baseline_request["tasks"]:
            fail("warm-zero request changed task inputs instead of publication ancestry")
        warm_report, _warm_bytes = _run_materializer(
            materializer,
            warm_request,
            state_directory,
            oracle,
            root,
            arguments.timeout_seconds,
            "warm-zero",
            binary_digest,
            occurrence,
        )
        warm_census = _validate_census(
            warm_report,
            warm_request,
            "warm-zero",
            expected_actual=0,
            expected_task_ids=[],
            expected_execution_ids=[],
        )
        if not warm_census["warm_zero"]:
            fail("warm-zero report did not assert warm_zero=true")
        warm_publication = _publication_id(warm_report, "warm-zero")
        if warm_publication == baseline_publication:
            fail("warm-zero publication did not advance the committed head")

        affected_request = copy.deepcopy(warm_request)
        changed_index = arguments.changed_task_index
        old_condition = affected_request["tasks"][changed_index]["condition_id"]
        affected_request["tasks"][changed_index]["condition_id"] = (
            old_condition + ":g5-affected-only"
        )
        oracle.bind_provider_task_identities(affected_request)
        oracle.bind_task_execution_identities(affected_request)
        _rebind_request(
            root,
            oracle,
            affected_request,
            genesis=False,
            parent_publication=warm_publication,
        )
        changed_indexes = [
            index
            for index, (old_task, new_task) in enumerate(
                zip(warm_request["tasks"], affected_request["tasks"])
            )
            if old_task != new_task
        ]
        if changed_indexes != [changed_index]:
            fail(f"affected-only request changed more than one task: {changed_indexes}")
        affected_report, affected_bytes = _run_materializer(
            materializer,
            affected_request,
            state_directory,
            oracle,
            root,
            arguments.timeout_seconds,
            "affected-only",
            binary_digest,
            occurrence,
        )
        affected_census = _validate_census(
            affected_report,
            affected_request,
            "affected-only",
            expected_actual=1,
            expected_task_ids=[
                affected_request["tasks"][changed_index]["provider_task_id"]
            ],
            expected_execution_ids=[
                affected_request["tasks"][changed_index]["provider_execution_id"]
            ],
        )
        if affected_census["warm_zero"] or not affected_census["executed_provider_task_ids"]:
            fail("affected-only report did not execute one changed provider task")
        if affected_census["executed_provider_task_ids"] != [
            affected_request["tasks"][changed_index]["provider_task_id"]
        ]:
            fail("affected-only report executed a task other than the changed task")
        if affected_census["executed_provider_execution_ids"] != [
            affected_request["tasks"][changed_index]["provider_execution_id"]
        ]:
            fail("affected-only report executed a provider execution other than the changed task")
        affected_publication = _publication_id(affected_report, "affected-only")
        if affected_publication == warm_publication:
            fail("affected-only publication did not advance the committed head")
        _readonly_sqlite_reopen(
            state_directory / "materialization.sqlite",
            "affected-only",
        )

        independent_request = copy.deepcopy(affected_request)
        _rebind_request(
            root,
            oracle,
            independent_request,
            genesis=True,
            parent_publication=None,
        )
        independent_report, _independent_bytes = _run_materializer(
            materializer,
            independent_request,
            independent_directory,
            oracle,
            root,
            arguments.timeout_seconds,
            "independent recompute",
            binary_digest,
            occurrence,
        )
        _validate_census(
            independent_report,
            independent_request,
            "independent recompute",
            expected_actual=arguments.translation_unit_count,
            expected_task_ids=[
                task["provider_task_id"] for task in independent_request["tasks"]
            ],
            expected_execution_ids=[
                task["provider_execution_id"] for task in independent_request["tasks"]
            ],
        )
        _readonly_sqlite_reopen(
            independent_directory / "materialization.sqlite",
            "independent recompute",
        )

    affected_reopened = affected_report["semantic_verification"]["reopened_store"]
    independent_reopened = independent_report["semantic_verification"]["reopened_store"]
    if (
        affected_reopened["canonical_export_digest"]
        != independent_reopened["canonical_export_digest"]
        or affected_report["store"]["snapshot_manifest"]
        != independent_report["store"]["snapshot_manifest"]
    ):
        fail("independent recomputation canonical Store parity did not match affected-only output")

    final_git = git_state(root)
    if final_git != initial_git:
        fail(f"local Git state changed during production evidence: {initial_git} -> {final_git}")
    evidence = {
        "schema": EVIDENCE_SCHEMA,
        "evidence_status": "observed",
        "producer": {
            "kind": "production-coordinator",
            "synthetic_planner_evidence": False,
            "interface": "run_materialization_incremental_coordinator_and_publish",
            "binary_digest": binary_digest,
            "report_digest": content_digest(affected_bytes),
        },
        "git": initial_git,
        "execution_census": {
            "schema": EXECUTION_CENSUS_SCHEMA,
            "total_planned_provider_executions": (
                warm_census["planned_provider_executions"]
                + affected_census["planned_provider_executions"]
            ),
            "total_actual_provider_executions": (
                warm_census["actual_provider_executions"]
                + affected_census["actual_provider_executions"]
            ),
            "total_actual_recomputed_partition_count": (
                warm_census["actual_recomputed_partition_count"]
                + affected_census["actual_recomputed_partition_count"]
            ),
            "warm_zero": {
                "planned_provider_executions": warm_census["planned_provider_executions"],
                "actual_provider_executions": warm_census["actual_provider_executions"],
                "actual_recomputed_partition_count": warm_census[
                    "actual_recomputed_partition_count"
                ],
                "warm_zero": warm_census["warm_zero"],
                "affected_only": False,
                "exact_inputs_unchanged": True,
                "executed_partition_ids": warm_census["executed_partition_ids"],
                "executed_provider_task_ids": warm_census["executed_provider_task_ids"],
                "executed_provider_execution_ids": warm_census[
                    "executed_provider_execution_ids"
                ],
                "executed_artifact_digests": warm_census["executed_artifact_digests"],
                "executed_task_partition_set_digests": warm_census[
                    "executed_task_partition_set_digests"
                ],
            },
            "affected_only": {
                "planned_provider_executions": affected_census[
                    "planned_provider_executions"
                ],
                "actual_provider_executions": affected_census["actual_provider_executions"],
                "actual_recomputed_partition_count": affected_census[
                    "actual_recomputed_partition_count"
                ],
                "warm_zero": affected_census["warm_zero"],
                "affected_only": True,
                "exact_inputs_unchanged": False,
                "executed_partition_ids": affected_census["executed_partition_ids"],
                "executed_provider_task_ids": affected_census[
                    "executed_provider_task_ids"
                ],
                "executed_provider_execution_ids": affected_census[
                    "executed_provider_execution_ids"
                ],
                "executed_artifact_digests": affected_census["executed_artifact_digests"],
                "executed_task_partition_set_digests": affected_census[
                    "executed_task_partition_set_digests"
                ],
            },
        },
        "publication": {
            "backend": "sqlite",
            "attempted": True,
            "outcome": "committed_verified",
            "verified": True,
            # The installed report exposes Store.publish()'s exact one-call
            # result as committed_transaction_count; require that value before
            # projecting the G5 evidence field with its contract name.
            "publish_call_count": affected_report["publication"][
                "committed_transaction_count"
            ],
            "committed_transaction_count": affected_report["publication"][
                "committed_transaction_count"
            ],
        },
        "reopen": {
            "attempted": True,
            "outcome": "opened",
            "verified": True,
        },
        "independent_recompute": {
            "status": "passed",
            "canonical_parity": "passed",
        },
    }
    if not SHA256_PATTERN.fullmatch(binary_digest) or not SHA256_PATTERN.fullmatch(
        evidence["producer"]["report_digest"]
    ):
        fail("producer digests are not canonical SHA-256 content digests")
    production_report.parent.mkdir(parents=True, exist_ok=True)
    production_report.write_bytes(affected_bytes)
    _write_verified_output(
        output,
        evidence,
        root,
        g5,
        initial_git["revision"],
        materializer,
        production_report,
    )


def main() -> int:
    arguments = parse_args()
    try:
        produce(arguments)
    except EvidenceProductionError as error:
        print(f"G5 production evidence failure: {error}", file=sys.stderr)
        return 1
    except (KeyboardInterrupt, SystemExit):
        raise
    except Exception as error:  # pragma: no cover - fail closed for tool/environment drift
        print(f"G5 production evidence internal failure: {error}", file=sys.stderr)
        return 1
    print("G5 production coordinator evidence produced")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
