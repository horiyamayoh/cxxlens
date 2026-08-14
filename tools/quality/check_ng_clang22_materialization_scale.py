#!/usr/bin/env python3
"""Independently check Clang 22 materialization scale evidence.

This checker is intentionally not the release authority.  It validates the
scenario census, exact contract constants, process receipts, and selected
installed-positive observations while preserving the distinction between
bounded request-ingress evidence and full semantic/release qualification.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_clang22_materialization_scale_evidence.schema.yaml"
)
REQUIRED_SCENARIOS = {
    "one-task",
    "four-thousand-ninety-six-tasks",
    "sixteen-mib-source",
    "five-hundred-twelve-mib-aggregate-source",
    "one-gib-raw-request",
    "raw-request-limit-plus-one",
    "arbitrary-short-reads",
}
RAW_INPUT_LIMIT_BYTES = 1 << 30
MAXIMUM_TASK_INPUT_BYTES = 64 << 20
MAXIMUM_TASK_COUNT = 4096
MAXIMUM_AGGREGATE_SOURCE_BYTES = 512 << 20
SOURCE_CHUNK_BYTES = 16 << 20
REQUIRED_NEGATIVE_VECTORS = {
    "missing-chunk",
    "duplicate-chunk",
    "reordered-chunk",
    "extra-chunk",
    "length-drift",
    "digest-drift",
    "task-cross-splice",
    "raw-request-limit-plus-one",
    "fragmented-short-reads",
}
RETAINED_MEMORY_FORMULA = (
    "one-shared-catalog-plus-fixed-buffers-plus-one-task-window-plus-one-source-"
    "plus-one-output-window"
)
FORBIDDEN_RESIDENCY = [
    "raw-request",
    "aggregate-source",
    "all-task-payloads",
    "task-count-times-catalog-count",
]


class ScaleEvidenceError(ValueError):
    """The scale evidence is not independently qualified."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("check", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--report", required=True, type=pathlib.Path)
    parser.add_argument(
        "--require-installed-scenarios",
        default="",
        help="comma-separated installed-positive scenario IDs required by this check",
    )
    return parser.parse_args()


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def digest_file(path: pathlib.Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
            size += len(chunk)
    return size, "sha256:" + digest.hexdigest()


def git_value(root: pathlib.Path, expression: str) -> str:
    return subprocess.check_output(
        ["git", "rev-parse", expression], cwd=root, text=True
    ).strip()


def load_report(root: pathlib.Path, report_path: pathlib.Path) -> dict[str, Any]:
    raw = report_path.read_bytes()
    try:
        report = json.loads(raw)
    except json.JSONDecodeError as error:
        raise ScaleEvidenceError(f"scale evidence is not JSON: {error}") from error
    if raw != canonical_json(report):
        raise ScaleEvidenceError("scale evidence is not canonical JSON")
    schema = yaml.safe_load((root / SCHEMA).read_text(encoding="utf-8"))
    try:
        jsonschema.Draft202012Validator(schema).validate(report)
    except jsonschema.ValidationError as error:
        raise ScaleEvidenceError(f"scale evidence schema failure: {error.message}") from error
    return report


def check_authority(root: pathlib.Path, authority: dict[str, Any]) -> None:
    expected = {
        "source_revision": git_value(root, "HEAD"),
        "source_tree": git_value(root, "HEAD^{tree}"),
        "protocol_minor": 1,
        "required_feature": "task-input-chunks-v1",
        "raw_input_limit_bytes": RAW_INPUT_LIMIT_BYTES,
        "maximum_task_input_bytes": MAXIMUM_TASK_INPUT_BYTES,
        "maximum_task_input_chunks": 64,
        "maximum_aggregate_source_bytes": MAXIMUM_AGGREGATE_SOURCE_BYTES,
        "maximum_task_count": MAXIMUM_TASK_COUNT,
        "retained_memory_formula": RETAINED_MEMORY_FORMULA,
        "forbidden_residency": FORBIDDEN_RESIDENCY,
    }
    if authority != expected:
        raise ScaleEvidenceError(
            "scale evidence authority differs from exact repository authority"
        )


def check_artifact(report_path: pathlib.Path, artifact: dict[str, Any] | None) -> None:
    if artifact is None:
        return
    path = (report_path.parent / artifact["path"]).resolve()
    if report_path.parent.resolve() not in path.parents:
        raise ScaleEvidenceError(f"retained input artifact escapes evidence directory: {path}")
    if not path.is_file():
        raise ScaleEvidenceError(f"retained input artifact is missing: {path}")
    size, digest = digest_file(path)
    if size != artifact["byte_count"] or digest != artifact["sha256"]:
        raise ScaleEvidenceError(f"retained input artifact digest differs: {path}")


def artifact_bytes(report_path: pathlib.Path, artifact: dict[str, Any]) -> bytes:
    path = (report_path.parent / artifact["path"]).resolve()
    if report_path.parent.resolve() not in path.parents:
        raise ScaleEvidenceError(f"process artifact escapes evidence directory: {path}")
    if not path.is_file():
        raise ScaleEvidenceError(f"process artifact is missing: {path}")
    size, digest = digest_file(path)
    if size != artifact["byte_count"] or digest != artifact["sha256"]:
        raise ScaleEvidenceError(f"process artifact digest differs: {path}")
    return path.read_bytes()


def parse_exact_json(raw: bytes) -> Any | None:
    if not raw or raw == b"ok\n":
        return None
    try:
        text = raw.decode("utf-8")
        decoder = json.JSONDecoder()
        value, end = decoder.raw_decode(text)
        if text[end:].strip():
            raise ScaleEvidenceError("process stdout contains trailing JSON")
        return value
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ScaleEvidenceError(f"process stdout is not one complete JSON value: {error}") from error


def check_process(
    root: pathlib.Path,
    report_path: pathlib.Path,
    process: dict[str, Any],
    *,
    expected: str,
    installed: bool,
    scenario_id: str,
) -> None:
    if process["status"] == "failed":
        raise ScaleEvidenceError(f"{scenario_id} has a failed {('installed' if installed else 'admission')} run")
    if process["peak_rss_bytes"] <= 0:
        raise ScaleEvidenceError(f"{scenario_id} lacks wait4 peak RSS evidence")
    stdout_artifact = process.get("stdout_artifact")
    stderr_artifact = process.get("stderr_artifact")
    if stdout_artifact is None or stderr_artifact is None:
        raise ScaleEvidenceError(f"{scenario_id} lacks process stdout/stderr artifacts")
    stdout = artifact_bytes(report_path, stdout_artifact)
    stderr = artifact_bytes(report_path, stderr_artifact)
    stdout_size, stdout_digest = digest_file(
        report_path.parent / stdout_artifact["path"]
    )
    stderr_size, stderr_digest = digest_file(
        report_path.parent / stderr_artifact["path"]
    )
    if (
        process["stdout_byte_count"] != stdout_size
        or process["stdout_sha256"] != stdout_digest
        or process["stderr_byte_count"] != stderr_size
        or process["stderr_sha256"] != stderr_digest
    ):
        raise ScaleEvidenceError(f"{scenario_id} process receipt differs from retained artifacts")
    try:
        parsed = parse_exact_json(stdout)
    except ScaleEvidenceError:
        if installed:
            raise
        parsed = None
    parsed_count = int(parsed is not None)
    if process["parsed_response_count"] != parsed_count:
        raise ScaleEvidenceError(f"{scenario_id} parsed response count is not reproducible")
    if stdout == b"ok\n":
        observed = "driver-ok"
    elif parsed is None:
        observed = "empty" if not stdout else ("driver-error" if not installed else "installed-json-response")
    elif (
        isinstance(parsed, dict)
        and parsed.get("response_kind") == "detailed"
        and parsed.get("result") == "passed"
    ):
        observed = "installed-detailed-passed" if installed else "driver-error"
    else:
        observed = "installed-json-response" if installed else "driver-error"
    if process["observation"] != observed:
        raise ScaleEvidenceError(f"{scenario_id} process observation is not reproducible")
    if expected == "pass":
        if process["status"] != "passed" or process["actual_exit_status"] != 0:
            raise ScaleEvidenceError(f"{scenario_id} did not pass its expected process boundary")
        if installed:
            if process["observation"] != "installed-detailed-passed":
                raise ScaleEvidenceError(f"{scenario_id} lacks a detailed installed success")
            transfer = process.get("input_transfer")
            if transfer is None:
                raise ScaleEvidenceError(f"{scenario_id} lacks its authenticated input transfer receipt")
            expected_chunk_count = (
                0
                if transfer["logical_input_bytes"] == 0
                else (transfer["logical_input_bytes"] + (1 << 20) - 1) // (1 << 20)
            )
            if transfer["chunk_count"] != expected_chunk_count:
                raise ScaleEvidenceError(f"{scenario_id} has a noncanonical input chunk census")
        elif process["observation"] != "driver-ok":
            raise ScaleEvidenceError(f"{scenario_id} lacks a request-driver success")
    else:
        if installed:
            raise ScaleEvidenceError(f"negative scenario was sent to the installed tool: {scenario_id}")
        if process["status"] != "expected-rejection":
            raise ScaleEvidenceError(f"{scenario_id} did not preserve its expected rejection")
        if process["observation"] != "driver-error":
            raise ScaleEvidenceError(f"{scenario_id} lacks a request-driver rejection observation")
        if process["actual_exit_status"] != 1:
            raise ScaleEvidenceError(f"{scenario_id} has an unexpected rejection exit status")
        if scenario_id == "raw-request-limit-plus-one":
            expected_output = b"materialization.request-invalid|input-limit|maximum-bytes\n"
            stdout = artifact_bytes(report_path, process["stdout_artifact"])
            if stdout != expected_output:
                raise ScaleEvidenceError(
                    f"{scenario_id} did not expose the exact input-limit rejection"
                )
            if process["parsed_response_count"] != 0:
                raise ScaleEvidenceError(f"{scenario_id} unexpectedly emitted JSON")


def check_installed_positive(
    root: pathlib.Path,
    report_path: pathlib.Path,
    scenario: dict[str, Any],
    installed: dict[str, Any],
) -> None:
    input_artifact = installed.get("input_artifact")
    occurrence_artifact = installed.get("occurrence_artifact")
    if input_artifact is None or occurrence_artifact is None:
        raise ScaleEvidenceError(f"{scenario['id']} lacks installed authority artifacts")
    request_bytes = artifact_bytes(report_path, input_artifact)
    occurrence_bytes = artifact_bytes(report_path, occurrence_artifact)
    try:
        request = json.loads(request_bytes)
        occurrence = json.loads(occurrence_bytes)
    except json.JSONDecodeError as error:
        raise ScaleEvidenceError(f"{scenario['id']} authority artifact is not JSON") from error
    sys.path.insert(0, str(root / "tools" / "quality"))
    import check_ng_clang22_materialization as oracle  # pylint: disable=import-error

    try:
        oracle.validate_occurrence_manifest(root, occurrence)
        if oracle.content_digest(occurrence_bytes) != request["tool"]["occurrence_manifest_digest"]:
            raise ScaleEvidenceError(f"{scenario['id']} occurrence digest is not request-bound")
        report_raw = artifact_bytes(report_path, installed["stdout_artifact"])
        report = parse_exact_json(report_raw)
        if not isinstance(report, dict):
            raise ScaleEvidenceError(f"{scenario['id']} installed output is not an object")
        if report_raw != oracle.canonical_json(report):
            raise ScaleEvidenceError(f"{scenario['id']} installed output is not canonical JSON")
        oracle.validate_report(root, request, report, request_bytes=request_bytes)
    except (KeyError, TypeError, ValueError) as error:
        raise ScaleEvidenceError(
            f"{scenario['id']} installed report failed the independent materialization validator: {error}"
        ) from error


def check_scenario(root: pathlib.Path, report_path: pathlib.Path, scenario: dict[str, Any]) -> None:
    scenario_id = scenario["id"]
    expected = scenario["expected"]
    input_metadata = scenario["input"]
    task_count = input_metadata["task_count"]
    source_bytes = input_metadata["source_bytes_per_task"]
    aggregate = input_metadata["aggregate_source_bytes"]
    raw_bytes = input_metadata["raw_input_byte_count"]
    if raw_bytes > RAW_INPUT_LIMIT_BYTES + 1:
        raise ScaleEvidenceError(f"{scenario_id} exceeds the raw observation boundary")
    if task_count * source_bytes != aggregate:
        raise ScaleEvidenceError(f"{scenario_id} has an inconsistent source aggregate")
    if task_count > MAXIMUM_TASK_COUNT or source_bytes > MAXIMUM_TASK_INPUT_BYTES:
        raise ScaleEvidenceError(f"{scenario_id} exceeds a task bound")
    if aggregate > MAXIMUM_AGGREGATE_SOURCE_BYTES:
        raise ScaleEvidenceError(f"{scenario_id} exceeds the source aggregate bound")
    if scenario_id == "one-task" and task_count != 1:
        raise ScaleEvidenceError("one-task scenario does not contain one task")
    if scenario_id == "four-thousand-ninety-six-tasks" and task_count != MAXIMUM_TASK_COUNT:
        raise ScaleEvidenceError("4096-task scenario is not limit-adjacent")
    if scenario_id == "sixteen-mib-source" and source_bytes != SOURCE_CHUNK_BYTES:
        raise ScaleEvidenceError("16 MiB scenario is not exact")
    if scenario_id == "five-hundred-twelve-mib-aggregate-source" and aggregate != MAXIMUM_AGGREGATE_SOURCE_BYTES:
        raise ScaleEvidenceError("512 MiB aggregate scenario is not exact")
    if scenario_id == "one-gib-raw-request" and raw_bytes != RAW_INPUT_LIMIT_BYTES:
        raise ScaleEvidenceError("1 GiB raw scenario is not exact")
    if scenario_id == "raw-request-limit-plus-one" and raw_bytes != RAW_INPUT_LIMIT_BYTES + 1:
        raise ScaleEvidenceError("raw limit-plus-one scenario is not exact")
    if scenario_id == "arbitrary-short-reads" and input_metadata["read_fragmentation"] != "pipe-fragmented":
        raise ScaleEvidenceError("short-read scenario is not pipe-fragmented")
    check_artifact(report_path, input_metadata.get("input_artifact"))
    check_process(
        root,
        report_path,
        scenario["admission"],
        expected=expected,
        installed=False,
        scenario_id=scenario_id,
    )
    installed = scenario.get("installed")
    if installed is not None:
        check_process(
            root,
            report_path,
            installed,
            expected=expected,
            installed=True,
            scenario_id=scenario_id,
        )
        check_installed_positive(root, report_path, scenario, installed)
        if installed["input_transfer"]["logical_input_bytes"] < source_bytes:
            raise ScaleEvidenceError(
                f"{scenario_id} input transfer is smaller than its declared source"
            )


def check_report(
    root: pathlib.Path,
    report_path: pathlib.Path,
    required_installed: set[str],
) -> dict[str, Any]:
    report = load_report(root, report_path)
    if report["scope"] != {
        "kind": "ingress-scale-and-selected-installed-positive",
        "release_qualification": False,
        "semantic_status": "partial",
        "resource_qualification": False,
    }:
        raise ScaleEvidenceError("scale report silently claims release or semantic completion")
    check_authority(root, report["authority"])
    scenario_ids = [scenario["id"] for scenario in report["scenarios"]]
    if len(scenario_ids) != len(set(scenario_ids)) or set(scenario_ids) != REQUIRED_SCENARIOS:
        raise ScaleEvidenceError("scale scenario census is incomplete or duplicated")
    for scenario in report["scenarios"]:
        check_scenario(root, report_path, scenario)
    for scenario_id in required_installed:
        scenario = next(item for item in report["scenarios"] if item["id"] == scenario_id)
        if scenario["installed"] is None:
            raise ScaleEvidenceError(f"required installed scenario is absent: {scenario_id}")
    if report["planned_negative_vectors"] != sorted(REQUIRED_NEGATIVE_VECTORS):
        raise ScaleEvidenceError("negative vector census is incomplete or not deterministic")
    return report


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    report_path = args.report.resolve()
    required_installed = {
        value.strip()
        for value in args.require_installed_scenarios.split(",")
        if value.strip()
    }
    unknown = required_installed - REQUIRED_SCENARIOS
    if unknown:
        raise ScaleEvidenceError(f"unknown required installed scenario: {sorted(unknown)}")
    check_report(root, report_path, required_installed)
    print(f"verified {len(REQUIRED_SCENARIOS)} materialization scale scenarios")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ScaleEvidenceError, subprocess.SubprocessError) as error:
        print(f"materialization scale evidence check failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
