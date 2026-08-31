#!/usr/bin/env python3
"""Assert the bounded Clang 22 request ingress at the contract scale points."""

from __future__ import annotations

import argparse
import copy
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
from typing import Any, Callable


RAW_INPUT_LIMIT_BYTES = 1 << 30
MAXIMUM_TASK_COUNT = 4096
MAXIMUM_AGGREGATE_SOURCE_BYTES = 48 << 20
SOURCE_CHUNK_BYTES = 16 << 20
SCENARIO_IDS = (
    "one-task",
    "four-thousand-ninety-six-tasks",
    "sixteen-mib-source",
    "forty-eight-mib-aggregate-source",
    "one-gib-raw-request",
    "raw-request-limit-plus-one",
    "arbitrary-short-reads",
)
DEFAULT_INSTALLED_SCENARIOS = (
    "one-task",
    "sixteen-mib-source",
    "arbitrary-short-reads",
)
class ScaleTestError(RuntimeError):
    """The scale fixture or its observed process boundary is invalid."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--driver", required=True, type=pathlib.Path)
    parser.add_argument("--prefix", type=pathlib.Path)
    parser.add_argument("--work-directory", type=pathlib.Path)
    parser.add_argument(
        "--installed-scenarios",
        default=",".join(DEFAULT_INSTALLED_SCENARIOS),
        help="comma-separated positive scenarios to run through the installed tool",
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


def write_canonical_json(path: pathlib.Path, value: Any) -> None:
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


def write_padded_json(path: pathlib.Path, value: Any, byte_count: int) -> None:
    """Write one valid canonical JSON value followed only by JSON whitespace."""

    payload = canonical_json(value)
    if len(payload) > byte_count:
        raise ScaleTestError(
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


def padded_source(size: int, index: int = 0) -> bytes:
    prefix = f"int unit_{index}() {{ return {index}; }}\n".encode("ascii")
    if size < len(prefix):
        raise ScaleTestError(f"source size is smaller than its valid prefix: {size}")
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
        raise ScaleTestError(f"small source index exceeds fixed fixture width: {index}")
    return prefix + b" " * (target_bytes - len(prefix) - 1) + b"\n"


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
    elif scenario_id == "forty-eight-mib-aggregate-source":
        task_count = MAXIMUM_AGGREGATE_SOURCE_BYTES // SOURCE_CHUNK_BYTES
        source_bytes = SOURCE_CHUNK_BYTES
        factory = repeated_source(source_bytes)
    elif scenario_id == "arbitrary-short-reads":
        task_count = 1
        source_bytes = len(distinct_small_source(0))
        factory = distinct_small_source
    else:
        raise ScaleTestError(f"scenario does not contain a JSON request: {scenario_id}")

    # transport_bytes bounds combined stdout/stderr independently from the
    # Protocol 2 source-closure spool and frame-credit limits. Keep a
    # conservative response allowance proportional to the generated source,
    # plus fixed framing headroom, for these maximum-scale process scenarios.
    task_transport_bytes = max(2097152, 2 * source_bytes + (1 << 20))
    request = oracle.sample_request(
        root,
        configuration="static",
        backend="memory",
        translation_unit_count=task_count,
        source_factory=factory,
        task_transport_bytes=task_transport_bytes,
    )
    aggregate = task_count * source_bytes
    if aggregate > MAXIMUM_AGGREGATE_SOURCE_BYTES:
        raise ScaleTestError("generated source aggregate exceeds the authority")
    return request, task_count, aggregate


def run_process(
    argv: list[str],
    input_path: pathlib.Path,
    output_directory: pathlib.Path,
    *,
    fragmented: bool,
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
            raise ScaleTestError("wait4 returned a different child process")
        process.returncode = os.waitstatus_to_exitcode(status)
        if fragmented:
            writer.join()
        if source_handle is not None:
            source_handle.close()

    if writer_errors:
        raise ScaleTestError(f"fragmented input writer failed: {writer_errors[0]}")
    stdout_size = stdout_path.stat().st_size
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
    return {
        "actual_exit_status": process.returncode,
        "parsed_response_count": parsed_response_count,
        "peak_rss_bytes": int(usage.ru_maxrss) * 1024,
        "stdout_path": stdout_path,
        "stderr_path": stderr_path,
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
        raise ScaleTestError("installed occurrence manifest lacks tool and worker files")
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


def assert_installed_source_closure_channel_required(stdout_path: pathlib.Path) -> None:
    try:
        report = json.loads(stdout_path.read_text(encoding="utf-8"))
        error = report["error"]
    except (KeyError, TypeError, json.JSONDecodeError, UnicodeDecodeError) as failure:
        raise ScaleTestError("installed rejection is not a typed compact report") from failure
    expected = {
        "code": "materialization.request-invalid",
        "diagnostic": (
            "source-code=materialization.source-closure-invalid;"
            "source-detail=source-closure-channel-required;"
            "transport=protocol-v2-separate-channel"
        ),
        "phase": "request-schema",
        "subject": "request-v2_2",
    }
    if error != expected:
        raise ScaleTestError(f"installed rejection differs from channel contract: {error}")


def expected_process_status(
    process: dict[str, Any], expected: str, stdout_path: pathlib.Path
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


def scenario_input(
    root: pathlib.Path,
    oracle: Any,
    scenario_id: str,
    input_directory: pathlib.Path,
) -> pathlib.Path:
    if scenario_id in {"one-gib-raw-request", "raw-request-limit-plus-one"}:
        request, task_count, aggregate = build_request(root, oracle, "one-task")
        byte_count = RAW_INPUT_LIMIT_BYTES + (
            1 if scenario_id == "raw-request-limit-plus-one" else 0
        )
        request_path = input_directory / f"{scenario_id}.json"
        write_padded_json(request_path, request, byte_count)
    else:
        request, task_count, aggregate = build_request(root, oracle, scenario_id)
        request_path = input_directory / f"{scenario_id}.json"
        write_canonical_json(request_path, request)
    return request_path


def run() -> int:
    args = parse_args()
    root = args.root.resolve()
    driver = args.driver.resolve()
    prefix = args.prefix.resolve() if args.prefix else None
    if not driver.is_file():
        raise ScaleTestError(f"request driver is missing: {driver}")
    installed_scenarios = tuple(
        value.strip() for value in args.installed_scenarios.split(",") if value.strip()
    )
    unknown = set(installed_scenarios) - set(SCENARIO_IDS)
    if unknown:
        raise ScaleTestError(f"unknown installed scenarios: {sorted(unknown)}")
    if prefix is not None and not installed_scenarios:
        raise ScaleTestError("an installed prefix requires at least one scenario")
    if prefix is not None:
        materializer = prefix / "bin" / "cxxlens-clang22-materialize"
        occurrence_path = (
            prefix / "share/cxxlens/materialization/clang22/occurrence-v1.json"
        )
        if not materializer.is_file() or not occurrence_path.is_file():
            raise ScaleTestError("installed materializer or occurrence manifest is missing")
    else:
        materializer = None
        occurrence_path = None

    sys.path.insert(0, str(root / "tools" / "quality"))
    import check_ng_clang22_materialization as oracle  # pylint: disable=import-error

    work_directory = (
        args.work_directory.resolve()
        if args.work_directory
        else pathlib.Path(tempfile.mkdtemp(prefix="cxxlens-clang22-scale-"))
    )
    input_directory = work_directory / "inputs"
    run_directory = work_directory / "process"
    run_directory.mkdir(parents=True, exist_ok=True)
    for scenario_id in SCENARIO_IDS:
        expected = "reject" if scenario_id == "raw-request-limit-plus-one" else "pass"
        print(f"materialization scale scenario start: id={scenario_id}", flush=True)
        request_path = scenario_input(root, oracle, scenario_id, input_directory)
        driver_argv = [str(driver)]
        if scenario_id in {"one-gib-raw-request", "raw-request-limit-plus-one"}:
            driver_argv.append("--capture-only")
        admission = run_process(
            driver_argv,
            request_path,
            run_directory / scenario_id / "admission",
            fragmented=scenario_id == "arbitrary-short-reads",
        )
        admission_status = expected_process_status(
            admission,
            expected,
            admission["stdout_path"],
        )
        if admission_status == "failed":
            raise ScaleTestError(f"scale admission failed: {scenario_id}")
        print(
            f"materialization scale scenario progress: id={scenario_id} "
            f"phase=admission status={admission_status}",
            flush=True,
        )
        installed_status = "not-run"
        if materializer is not None and scenario_id in installed_scenarios:
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
            )
            installed_status = expected_process_status(
                installed,
                "reject",
                installed["stdout_path"],
            )
            if installed_status == "failed":
                raise ScaleTestError(f"installed scale scenario failed: {scenario_id}")
            assert_installed_source_closure_channel_required(installed["stdout_path"])
            print(
                f"materialization scale scenario progress: id={scenario_id} "
                f"phase=installed status={installed_status}",
                flush=True,
            )
        print(
            f"materialization scale scenario result: id={scenario_id} "
            f"admission={admission_status} installed={installed_status}",
            flush=True,
        )
    print("materialization scale tests passed", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except KeyboardInterrupt as error:
        print(f"materialization scale test interrupted: {error}", file=sys.stderr)
        raise SystemExit(130) from error
    except Exception as error:
        print(f"materialization scale test failed: {error}", file=sys.stderr)
        raise SystemExit(2) from error
