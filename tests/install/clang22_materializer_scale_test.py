#!/usr/bin/env python3
"""Exercise the bounded Clang 22 request ingress at the contract scale points.

The request-driver runs every scale point without launching a provider.  When an
installed prefix is supplied, selected positive cases are also sent through the
installed materializer.  The two observations are deliberately kept separate:
request-ingress scale evidence is not a claim that every large request completed
semantic materialization.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading
from typing import Any, Callable


RAW_INPUT_LIMIT_BYTES = 1 << 30
MAXIMUM_TASK_INPUT_BYTES = 64 << 20
MAXIMUM_TASK_COUNT = 4096
MAXIMUM_AGGREGATE_SOURCE_BYTES = 512 << 20
SOURCE_CHUNK_BYTES = 16 << 20
SCENARIO_IDS = (
    "one-task",
    "four-thousand-ninety-six-tasks",
    "sixteen-mib-source",
    "five-hundred-twelve-mib-aggregate-source",
    "one-gib-raw-request",
    "raw-request-limit-plus-one",
    "arbitrary-short-reads",
)
DEFAULT_INSTALLED_SCENARIOS = (
    "one-task",
    "sixteen-mib-source",
    "arbitrary-short-reads",
)
SCALE_SCHEMA = "cxxlens.clang22-materialization-scale-evidence.v1"
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
NEGATIVE_VECTORS = [
    "missing-chunk",
    "duplicate-chunk",
    "reordered-chunk",
    "extra-chunk",
    "length-drift",
    "digest-drift",
    "task-cross-splice",
    "raw-request-limit-plus-one",
    "fragmented-short-reads",
]
RUN_MARKER_SCHEMA = "cxxlens.clang22-materialization-scale-run.v1"

_ACTIVE_RUN_MARKER: tuple[pathlib.Path, dict[str, Any]] | None = None


class ScaleEvidenceError(RuntimeError):
    """The scale fixture or its observed process boundary is invalid."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--driver", required=True, type=pathlib.Path)
    parser.add_argument("--prefix", type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--work-directory", type=pathlib.Path)
    parser.add_argument(
        "--installed-scenarios",
        default=",".join(DEFAULT_INSTALLED_SCENARIOS),
        help="comma-separated positive scenarios to run through the installed tool",
    )
    parser.add_argument(
        "--preserve-inputs",
        action="store_true",
        help="retain generated request files next to the evidence report",
    )
    parser.add_argument(
        "--failure-marker",
        type=pathlib.Path,
        help="write a canonical run/scenario status marker for success and failure",
    )
    return parser.parse_args()


def digest_file(path: pathlib.Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
            size += len(chunk)
    return size, "sha256:" + digest.hexdigest()


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def write_run_marker(path: pathlib.Path, marker: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_bytes(canonical_json(marker))
    temporary.replace(path)


def safe_git_value(root: pathlib.Path, expression: str) -> str | None:
    try:
        return git_value(root, expression)
    except (OSError, subprocess.SubprocessError):
        return None


def new_run_marker(
    root: pathlib.Path, driver: pathlib.Path, output: pathlib.Path
) -> dict[str, Any]:
    return {
        "schema": RUN_MARKER_SCHEMA,
        "status": "running",
        "exit_status": None,
        "source_revision": safe_git_value(root, "HEAD"),
        "source_tree": safe_git_value(root, "HEAD^{tree}"),
        "driver": str(driver),
        "report": output.name,
        "phase": "startup",
        "current_scenario": None,
        "scenarios": [],
        "failure": None,
    }


def finish_aborted_run(error: BaseException, exit_status: int) -> None:
    if _ACTIVE_RUN_MARKER is None:
        return
    marker_path, marker = _ACTIVE_RUN_MARKER
    marker["status"] = "aborted"
    marker["exit_status"] = exit_status
    marker["failure"] = {
        "phase": marker.get("phase", "unknown"),
        "scenario_id": marker.get("current_scenario"),
        "error_type": type(error).__name__,
        "message": str(error),
    }
    for scenario in marker["scenarios"]:
        if scenario["status"] == "running":
            scenario["status"] = "aborted"
    marker["phase"] = "aborted"
    marker["current_scenario"] = None
    write_run_marker(marker_path, marker)


def write_canonical_json(path: pathlib.Path, value: Any) -> tuple[int, str]:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as destination:
        json.dump(
            value,
            destination,
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        )
    return digest_file(path)


def write_padded_json(path: pathlib.Path, value: Any, byte_count: int) -> tuple[int, str]:
    """Write one valid canonical JSON value followed only by JSON whitespace."""

    payload = canonical_json(value)
    if len(payload) > byte_count:
        raise ScaleEvidenceError(
            f"valid JSON fixture exceeds requested raw boundary: {len(payload)} > {byte_count}"
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    padding = b" " * (1024 * 1024)
    with path.open("wb") as destination:
        destination.write(payload)
        remaining = byte_count - len(payload)
        while remaining:
            chunk = padding if remaining >= len(padding) else padding[:remaining]
            destination.write(chunk)
            remaining -= len(chunk)
    return digest_file(path)


def padded_source(size: int, index: int = 0) -> bytes:
    prefix = f"int unit_{index}() {{ return {index}; }}\n".encode("ascii")
    if size < len(prefix):
        raise ScaleEvidenceError(f"source size is smaller than its valid prefix: {size}")
    if size == len(prefix):
        return prefix
    if size < len(prefix) + 4:
        return prefix + b" " * (size - len(prefix))
    return prefix + b"/*" + b"x" * (size - len(prefix) - 4) + b"*/"


def repeated_source(size: int) -> Callable[[int], bytes]:
    value = padded_source(size)
    return lambda _index: value


def distinct_small_source(index: int) -> bytes:
    prefix = f"int unit_{index}() {{ return {index}; }}".encode("ascii")
    target_bytes = len(b"int unit_4095() { return 4095; }\n")
    if len(prefix) + 1 > target_bytes:
        raise ScaleEvidenceError(f"small source index exceeds fixed fixture width: {index}")
    return prefix + b" " * (target_bytes - len(prefix) - 1) + b"\n"


def git_value(root: pathlib.Path, expression: str) -> str:
    return subprocess.check_output(
        ["git", "rev-parse", expression], cwd=root, text=True
    ).strip()


def build_request(
    root: pathlib.Path,
    oracle: Any,
    scenario_id: str,
) -> tuple[dict[str, Any], int, int]:
    if scenario_id == "one-task":
        task_count = 1
        source_bytes = len(distinct_small_source(0))
        factory = distinct_small_source
    elif scenario_id == "four-thousand-ninety-six-tasks":
        task_count = MAXIMUM_TASK_COUNT
        source_bytes = len(distinct_small_source(0))
        factory = distinct_small_source
    elif scenario_id == "sixteen-mib-source":
        task_count = 1
        source_bytes = SOURCE_CHUNK_BYTES
        factory = repeated_source(source_bytes)
    elif scenario_id == "five-hundred-twelve-mib-aggregate-source":
        task_count = MAXIMUM_AGGREGATE_SOURCE_BYTES // SOURCE_CHUNK_BYTES
        source_bytes = SOURCE_CHUNK_BYTES
        factory = repeated_source(source_bytes)
    elif scenario_id == "arbitrary-short-reads":
        task_count = 1
        source_bytes = len(distinct_small_source(0))
        factory = distinct_small_source
    else:
        raise ScaleEvidenceError(f"scenario does not contain a JSON request: {scenario_id}")

    request = oracle.sample_request(
        root,
        configuration="static",
        backend="memory",
        translation_unit_count=task_count,
        source_factory=factory,
    )
    aggregate = task_count * source_bytes
    if aggregate > MAXIMUM_AGGREGATE_SOURCE_BYTES:
        raise ScaleEvidenceError("generated source aggregate exceeds the authority")
    return request, task_count, aggregate


def copy_if_requested(
    source: pathlib.Path,
    output: pathlib.Path,
    preserve_inputs: bool,
) -> dict[str, Any] | None:
    if not preserve_inputs:
        return None
    destination = output.parent / "inputs" / source.name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    size, digest = digest_file(destination)
    return {
        "path": destination.relative_to(output.parent).as_posix(),
        "byte_count": size,
        "sha256": digest,
    }


def run_process(
    argv: list[str],
    input_path: pathlib.Path,
    output_directory: pathlib.Path,
    *,
    fragmented: bool,
    kind: str,
    artifact_root: pathlib.Path,
) -> dict[str, Any]:
    output_directory.mkdir(parents=True, exist_ok=True)
    stdout_path = output_directory / "stdout"
    stderr_path = output_directory / "stderr"
    writer_errors: list[BaseException] = []
    source_handle = None
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        if fragmented:
            process = subprocess.Popen(
                argv,
                stdin=subprocess.PIPE,
                stdout=stdout,
                stderr=stderr,
                close_fds=True,
            )

            def write_fragments() -> None:
                try:
                    assert process.stdin is not None
                    with input_path.open("rb") as source:
                        pattern = (1, 7, 4093, 65537, 1048573)
                        position = 0
                        while chunk := source.read(pattern[position % len(pattern)]):
                            view = memoryview(chunk)
                            while view:
                                written = os.write(process.stdin.fileno(), view)
                                view = view[written:]
                            position += 1
                    process.stdin.close()
                except BrokenPipeError:
                    try:
                        process.stdin.close()
                    except (BrokenPipeError, OSError):
                        pass
                except BaseException as error:  # pragma: no cover - defensive thread path
                    writer_errors.append(error)

            writer = threading.Thread(target=write_fragments, name="scale-input-writer")
            writer.start()
        else:
            source_handle = input_path.open("rb")
            process = subprocess.Popen(
                argv,
                stdin=source_handle,
                stdout=stdout,
                stderr=stderr,
                close_fds=True,
            )

        waited_pid, status, usage = os.wait4(process.pid, 0)
        if waited_pid != process.pid:
            raise ScaleEvidenceError("wait4 returned a different child process")
        process.returncode = os.waitstatus_to_exitcode(status)
        if fragmented:
            writer.join()
        if source_handle is not None:
            source_handle.close()

    if writer_errors:
        raise ScaleEvidenceError(f"fragmented input writer failed: {writer_errors[0]}")
    stdout_size, stdout_digest = digest_file(stdout_path)
    stderr_size, stderr_digest = digest_file(stderr_path)
    parsed_value = None
    if stdout_size == 0 or stdout_path.read_bytes() == b"ok\n":
        parsed_response_count = 0
    else:
        try:
            raw_response = stdout_path.read_bytes()
            text_response = raw_response.decode("utf-8")
            decoder = json.JSONDecoder()
            parsed_value, end = decoder.raw_decode(text_response)
            if text_response[end:].strip():
                raise json.JSONDecodeError("trailing JSON value", text_response, end)
            parsed_response_count = 1
        except (json.JSONDecodeError, UnicodeDecodeError):
            parsed_response_count = 0
    if stdout_size == 0:
        observation = "empty"
    elif kind == "admission" and stdout_path.read_bytes() == b"ok\n":
        observation = "driver-ok"
    elif kind == "admission":
        observation = "driver-error"
    elif (
        isinstance(parsed_value, dict)
        and parsed_value.get("response_kind") == "detailed"
        and parsed_value.get("result") == "passed"
    ):
        observation = "installed-detailed-passed"
    else:
        observation = "installed-json-response"
    return {
        "status": "failed",
        "observation": observation,
        "actual_exit_status": process.returncode,
        "stdout_byte_count": stdout_size,
        "stdout_sha256": stdout_digest,
        "stderr_byte_count": stderr_size,
        "stderr_sha256": stderr_digest,
        "parsed_response_count": parsed_response_count,
        "peak_rss_bytes": int(usage.ru_maxrss) * 1024,
        "command_argv": argv,
        "stdout_artifact": {
            "path": stdout_path.relative_to(artifact_root).as_posix(),
            "byte_count": stdout_size,
            "sha256": stdout_digest,
        },
        "stderr_artifact": {
            "path": stderr_path.relative_to(artifact_root).as_posix(),
            "byte_count": stderr_size,
            "sha256": stderr_digest,
        },
    }


def bind_installed_request(
    request: dict[str, Any],
    occurrence_path: pathlib.Path,
    oracle: Any,
) -> dict[str, Any]:
    occurrence_bytes = occurrence_path.read_bytes()
    occurrence = json.loads(occurrence_bytes)
    files = occurrence.get("files")
    if not isinstance(files, list) or len(files) < 2:
        raise ScaleEvidenceError("installed occurrence manifest lacks tool and worker files")
    bound = copy.deepcopy(request)
    bound["tool"].update(
        source_revision=occurrence["source_revision"],
        source_tree=occurrence["source_tree"],
        installed_executable_digest=files[0]["digest"],
        occurrence_manifest_digest=oracle.content_digest(occurrence_bytes),
    )
    bound["worker"].update(
        installed_binary_digest=files[1]["digest"],
        sandbox_policy_digest=(
            "semantic-v2:sha256:"
            "b4e95d8c88cf660fff40c4d9e7e4ae07bcb078013b5370c6b1abb80b0d75d375"
        ),
    )
    for task in bound["tasks"]:
        task["sandbox"]["policy_digest"] = bound["worker"]["sandbox_policy_digest"]
    oracle.bind_provider_task_identities(bound)
    oracle.bind_task_execution_identities(bound)
    oracle.bind_engine_policy_and_selector_identities(bound)
    oracle.bind_request_identity(bound)
    return bound


def expected_process_status(
    process: dict[str, Any], expected: str, oracle: Any, stdout_path: pathlib.Path
) -> str:
    if expected == "pass":
        if process["actual_exit_status"] != 0:
            return "failed"
        if stdout_path.read_bytes() == b"ok\n":
            return "passed"
        try:
            with stdout_path.open("rb") as response:
                parsed = json.load(response)
            if parsed.get("response_kind") == "detailed" and parsed.get("result") == "passed":
                return "passed"
        except (json.JSONDecodeError, UnicodeDecodeError, AttributeError):
            pass
        return "failed"
    if process["actual_exit_status"] in {1, 2}:
        return "expected-rejection"
    return "failed"


def installed_input_transfer_receipt(stdout_path: pathlib.Path) -> dict[str, Any]:
    try:
        with stdout_path.open("rb") as response:
            report = json.load(response)
        task_results = report["task_results"]
        if len(task_results) != 1:
            raise ScaleEvidenceError("scale installed positive must contain one task result")
        return task_results[0]["input_transfer"]
    except (KeyError, TypeError, IndexError, json.JSONDecodeError) as error:
        raise ScaleEvidenceError(
            f"installed scale positive lacks its authenticated input transfer receipt: {stdout_path}"
        ) from error


def attach_installed_input_transfer_receipt(
    process: dict[str, Any], stdout_path: pathlib.Path
) -> None:
    """Attach the receipt only after the installed process passed its boundary.

    A failed installed invocation is expected to emit a compact failure report,
    which intentionally has no task result or input-transfer receipt.  Reading
    that failure as though it were a success report obscures the owning worker
    failure with a false "receipt missing" error.
    """

    if process["status"] == "passed":
        process["input_transfer"] = installed_input_transfer_receipt(stdout_path)


def scenario_input(
    root: pathlib.Path,
    oracle: Any,
    scenario_id: str,
    input_directory: pathlib.Path,
    preserve_inputs: bool,
    output: pathlib.Path,
) -> tuple[pathlib.Path, dict[str, Any], dict[str, Any] | None]:
    if scenario_id in {"one-gib-raw-request", "raw-request-limit-plus-one"}:
        request, task_count, aggregate = build_request(root, oracle, "one-task")
        byte_count = RAW_INPUT_LIMIT_BYTES + (
            1 if scenario_id == "raw-request-limit-plus-one" else 0
        )
        request_path = input_directory / f"{scenario_id}.json"
        raw_count, raw_digest = write_padded_json(request_path, request, byte_count)
        source_bytes = aggregate // task_count
        input_metadata = {
            "raw_input_byte_count": raw_count,
            "raw_input_sha256": raw_digest,
            "task_count": task_count,
            "source_bytes_per_task": source_bytes,
            "aggregate_source_bytes": aggregate,
            "read_fragmentation": "file-backed",
        }
    else:
        request, task_count, aggregate = build_request(root, oracle, scenario_id)
        request_path = input_directory / f"{scenario_id}.json"
        raw_count, raw_digest = write_canonical_json(request_path, request)
        source_bytes = aggregate // task_count if task_count else 0
        input_metadata = {
            "raw_input_byte_count": raw_count,
            "raw_input_sha256": raw_digest,
            "task_count": task_count,
            "source_bytes_per_task": source_bytes,
            "aggregate_source_bytes": aggregate,
            "read_fragmentation": (
                "pipe-fragmented" if scenario_id == "arbitrary-short-reads" else "file-backed"
            ),
        }
    artifact = copy_if_requested(request_path, output, preserve_inputs)
    input_metadata["input_artifact"] = artifact
    return request_path, input_metadata, artifact


def run() -> int:
    global _ACTIVE_RUN_MARKER

    args = parse_args()
    root = args.root.resolve()
    driver = args.driver.resolve()
    output = args.output.resolve()
    failure_marker = (
        args.failure_marker.resolve()
        if args.failure_marker
        else output.parent / f"{output.stem}.failure.json"
    )
    marker = new_run_marker(root, driver, output)
    _ACTIVE_RUN_MARKER = (failure_marker, marker)
    write_run_marker(failure_marker, marker)

    prefix = args.prefix.resolve() if args.prefix else None
    marker["phase"] = "validate"
    write_run_marker(failure_marker, marker)
    if not driver.is_file():
        raise ScaleEvidenceError(f"request driver is missing: {driver}")
    installed_scenarios = tuple(
        value.strip() for value in args.installed_scenarios.split(",") if value.strip()
    )
    unknown = set(installed_scenarios) - set(SCENARIO_IDS)
    if unknown:
        raise ScaleEvidenceError(f"unknown installed scenarios: {sorted(unknown)}")
    if prefix is not None and not installed_scenarios:
        raise ScaleEvidenceError("an installed prefix requires at least one scenario")
    if prefix is not None:
        materializer = prefix / "bin" / "cxxlens-clang22-materialize"
        occurrence_path = (
            prefix / "share/cxxlens/materialization/clang22/occurrence-v1.json"
        )
        if not materializer.is_file() or not occurrence_path.is_file():
            raise ScaleEvidenceError("installed materializer or occurrence manifest is missing")
    else:
        materializer = None
        occurrence_path = None

    sys.path.insert(0, str(root / "tools" / "quality"))
    marker["phase"] = "load-oracle"
    write_run_marker(failure_marker, marker)
    import check_ng_clang22_materialization as oracle  # pylint: disable=import-error

    work_directory = (
        args.work_directory.resolve()
        if args.work_directory
        else pathlib.Path(tempfile.mkdtemp(prefix="cxxlens-clang22-scale-"))
    )
    input_directory = work_directory / "inputs"
    run_directory = output.parent / f"{output.stem}.process"
    run_directory.mkdir(parents=True, exist_ok=True)
    scenarios: list[dict[str, Any]] = []
    failed = False
    marker["phase"] = "scenario-loop"
    write_run_marker(failure_marker, marker)
    for scenario_id in SCENARIO_IDS:
        expected = "reject" if scenario_id == "raw-request-limit-plus-one" else "pass"
        scenario_marker = {
            "id": scenario_id,
            "expected": expected,
            "status": "running",
            "input": "pending",
            "admission": None,
            "installed": None,
        }
        marker["scenarios"].append(scenario_marker)
        marker["current_scenario"] = scenario_id
        marker["phase"] = "input"
        write_run_marker(failure_marker, marker)
        print(f"materialization scale scenario start: id={scenario_id}", flush=True)
        request_path, input_metadata, _artifact = scenario_input(
            root,
            oracle,
            scenario_id,
            input_directory,
            args.preserve_inputs,
            output,
        )
        scenario_marker["input"] = "generated"
        marker["phase"] = "admission"
        write_run_marker(failure_marker, marker)
        admission = run_process(
            [str(driver)],
            request_path,
            run_directory / scenario_id / "admission",
            fragmented=scenario_id == "arbitrary-short-reads",
            kind="admission",
            artifact_root=output.parent,
        )
        admission["status"] = expected_process_status(
            admission,
            expected,
            oracle,
            run_directory / scenario_id / "admission" / "stdout",
        )
        scenario_marker["admission"] = admission["status"]
        write_run_marker(failure_marker, marker)
        print(
            f"materialization scale scenario progress: id={scenario_id} "
            f"phase=admission status={admission['status']}",
            flush=True,
        )
        installed = None
        if materializer is not None and scenario_id in installed_scenarios:
            # Rebuild the request from the input only for selected positive cases.  The
            # request-driver fixture intentionally uses deterministic synthetic install
            # identities; the installed tool needs the exact relocated occurrence binding.
            if expected != "pass":
                raise ScaleEvidenceError("installed scenarios must be positive")
            marker["phase"] = "installed"
            write_run_marker(failure_marker, marker)
            with request_path.open("rb") as source:
                request_value = json.load(source)
            installed_request = bind_installed_request(request_value, occurrence_path, oracle)
            installed_path = input_directory / f"{scenario_id}.installed.json"
            write_canonical_json(installed_path, installed_request)
            installed = run_process(
                [str(materializer)],
                installed_path,
                run_directory / scenario_id / "installed",
                fragmented=scenario_id == "arbitrary-short-reads",
                kind="installed",
                artifact_root=output.parent,
            )
            installed["status"] = expected_process_status(
                installed,
                expected,
                oracle,
                run_directory / scenario_id / "installed" / "stdout",
            )
            scenario_marker["installed"] = installed["status"]
            write_run_marker(failure_marker, marker)
            print(
                f"materialization scale scenario progress: id={scenario_id} "
                f"phase=installed status={installed['status']}",
                flush=True,
            )
            attach_installed_input_transfer_receipt(
                installed, run_directory / scenario_id / "installed" / "stdout"
            )
            installed["input_artifact"] = copy_if_requested(
                installed_path, output, True
            )
            installed["occurrence_artifact"] = copy_if_requested(
                occurrence_path, output, True
            )
        if admission["status"] == "failed" or (
            installed is not None and installed["status"] == "failed"
        ):
            failed = True
            if marker["failure"] is None:
                marker["failure"] = {
                    "phase": "scenario-result",
                    "scenario_id": scenario_id,
                    "reason": "process-boundary-failed",
                }
        scenario_marker["status"] = (
            "failed"
            if admission["status"] == "failed"
            or (installed is not None and installed["status"] == "failed")
            else "passed"
        )
        marker["phase"] = "scenario-result"
        write_run_marker(failure_marker, marker)
        print(
            f"materialization scale scenario result: id={scenario_id} "
            f"status={scenario_marker['status']} "
            f"admission={admission['status']} "
            f"installed={scenario_marker['installed'] or 'not-run'}",
            flush=True,
        )
        scenarios.append(
            {
                "id": scenario_id,
                "expected": expected,
                "input": input_metadata,
                "admission": admission,
                "installed": installed,
            }
        )

    marker["phase"] = "report"
    marker["current_scenario"] = None
    write_run_marker(failure_marker, marker)
    report = {
        "schema": SCALE_SCHEMA,
        "scope": {
            "kind": "ingress-scale-and-selected-installed-positive",
            "release_qualification": False,
            "semantic_status": "partial",
            "resource_qualification": False,
        },
        "authority": {
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
        },
        "execution": {
            "platform": "linux",
            "runner": "wait4-peak-rss",
            "toolchain": {
                "compiler": os.environ.get("CXX", "unknown"),
                "python": sys.version.split()[0],
            },
            "command_argv": [str(driver)],
        },
        "scenarios": scenarios,
        "planned_negative_vectors": sorted(NEGATIVE_VECTORS),
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(canonical_json(report))
    exit_status = 1 if failed else 0
    marker["status"] = "failed" if failed else "passed"
    marker["exit_status"] = exit_status
    marker["phase"] = "complete"
    write_run_marker(failure_marker, marker)
    print(
        f"materialization scale run result: status={marker['status']} "
        f"exit_status={exit_status}",
        flush=True,
    )
    _ACTIVE_RUN_MARKER = None
    return exit_status


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except KeyboardInterrupt as error:
        finish_aborted_run(error, 130)
        print(f"materialization scale evidence interrupted: {error}", file=sys.stderr)
        raise SystemExit(130) from error
    except Exception as error:
        finish_aborted_run(error, 2)
        print(f"materialization scale evidence failed: {error}", file=sys.stderr)
        raise SystemExit(2) from error
