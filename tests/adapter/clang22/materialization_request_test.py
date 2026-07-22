#!/usr/bin/env python3
"""Cross-check the private C++ request validator against the independent Python oracle."""

from __future__ import annotations

import argparse
import copy
import json
import pathlib
import subprocess
import sys
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--driver", required=True, type=pathlib.Path)
    return parser.parse_args()


def encoded(value: dict[str, Any]) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        allow_nan=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def run(driver: pathlib.Path, payload: bytes) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [str(driver)],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def run_option(
    driver: pathlib.Path, option: str, payload: bytes = b""
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [str(driver), option],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def expect_pass_payload(driver: pathlib.Path, payload: bytes) -> None:
    result = run(driver, payload)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert result.stdout == b"ok\n"
    assert result.stderr == b""


def expect_pass(driver: pathlib.Path, value: dict[str, Any]) -> None:
    expect_pass_payload(driver, encoded(value))


def expect_failure(
    driver: pathlib.Path,
    payload: bytes,
    expected_code: bytes,
    expected_field: bytes | None = None,
) -> None:
    result = run(driver, payload)
    assert result.returncode == 1, (result.stdout, result.stderr)
    assert result.stdout.startswith(expected_code + b"|"), result.stdout
    if expected_field is not None:
        assert result.stdout.split(b"|", 2)[1] == expected_field, result.stdout
    assert result.stderr == b""


def expect_no_response(driver: pathlib.Path, option: str) -> None:
    result = run_option(driver, option)
    assert result.returncode == 2, (result.stdout, result.stderr)
    assert result.stdout == b""
    assert result.stderr == b""


def main() -> int:
    args = parse_args()
    sys.path.insert(0, str(args.root / "tools" / "quality"))
    import check_ng_clang22_materialization as oracle  # pylint: disable=import-error

    expect_no_response(args.driver, "--test-no-response")
    unexpected = run_option(args.driver, "--unexpected")
    assert unexpected.returncode == 2
    assert unexpected.stdout == b""
    assert unexpected.stderr == b""
    over_limit = run_option(args.driver, "--test-input-limit", b"x" * 17)
    assert over_limit.returncode == 1
    assert (
        over_limit.stdout
        == b"materialization.request-invalid|input-limit|maximum-bytes\n"
    )
    assert over_limit.stderr == b""

    memory = oracle.sample_request(args.root, configuration="static", backend="memory")
    sqlite = oracle.sample_request(
        args.root,
        configuration="shared",
        backend="sqlite",
        translation_unit_count=2,
    )
    expect_pass(args.driver, memory)
    expect_pass(args.driver, sqlite)

    unknown_version_with_shape_error = copy.deepcopy(memory)
    unknown_version_with_shape_error["request_version"] = "3.0.0"
    unknown_version_with_shape_error.pop("tasks")
    expect_failure(
        args.driver,
        encoded(unknown_version_with_shape_error),
        b"materialization.version-unsupported",
        b"request-version",
    )

    unknown_schema_with_shape_error = copy.deepcopy(memory)
    unknown_schema_with_shape_error["schema"] = (
        "cxxlens.clang22-materialization-request.v3"
    )
    unknown_schema_with_shape_error["unexpected"] = True
    expect_failure(
        args.driver,
        encoded(unknown_schema_with_shape_error),
        b"materialization.request-invalid",
        b"request-envelope",
    )

    supported_version_with_shape_error = copy.deepcopy(memory)
    supported_version_with_shape_error.pop("tasks")
    expect_failure(
        args.driver,
        encoded(supported_version_with_shape_error),
        b"materialization.request-invalid",
        b"request",
    )

    base64_alias = copy.deepcopy(memory)
    canonical_base64 = base64_alias["tasks"][0]["source"]["content_base64"]
    assert canonical_base64.endswith("g=="), canonical_base64
    base64_alias["tasks"][0]["source"]["content_base64"] = (
        canonical_base64[:-3] + "h=="
    )
    try:
        oracle.validate_request(args.root, base64_alias)
    except oracle.MaterializationError as error:
        assert error.code == "materialization.request-invalid", error
    else:
        raise AssertionError("Python oracle accepted noncanonical two-pad Base64")
    expect_failure(
        args.driver,
        encoded(base64_alias),
        b"materialization.request-invalid",
        b"request-schema",
    )

    escaped_base64 = encoded(memory).replace(
        json.dumps(canonical_base64).encode("ascii"),
        ("\"" + canonical_base64.replace("=", "\\u003d") + "\"").encode("ascii"),
        1,
    )
    assert escaped_base64 != encoded(memory)
    decoded_escape = oracle.load_strict_json_bytes(escaped_base64, "request")
    assert decoded_escape == memory
    assert oracle.expected_task_input_digest(
        decoded_escape, decoded_escape["tasks"][0]
    ) == oracle.expected_task_input_digest(memory, memory["tasks"][0])
    expect_pass_payload(args.driver, escaped_base64)

    one_pad_base64 = copy.deepcopy(memory)
    one_pad_base64["project"]["catalog_compile_units"][0][
        "source_digest"
    ] = oracle.content_digest(b"ab")
    one_pad_base64["tasks"][0]["source"]["content_base64"] = "YWI="
    oracle.rebind_request_base_identities(args.root, one_pad_base64)
    oracle.validate_request(args.root, one_pad_base64)
    expect_pass(args.driver, one_pad_base64)

    one_pad_base64_alias = copy.deepcopy(one_pad_base64)
    one_pad_base64_alias["tasks"][0]["source"]["content_base64"] = "YWJ="
    try:
        oracle.validate_request(args.root, one_pad_base64_alias)
    except oracle.MaterializationError as error:
        assert error.code == "materialization.request-invalid", error
    else:
        raise AssertionError("Python oracle accepted noncanonical one-pad Base64")
    expect_failure(
        args.driver,
        encoded(one_pad_base64_alias),
        b"materialization.request-invalid",
        b"request-schema",
    )

    empty_source = copy.deepcopy(memory)
    empty_source["project"]["catalog_compile_units"][0]["source_digest"] = (
        oracle.content_digest(b"")
    )
    empty_source["tasks"][0]["source"]["content_base64"] = ""
    oracle.rebind_request_base_identities(args.root, empty_source)
    oracle.validate_request(args.root, empty_source)
    expect_pass(args.driver, empty_source)

    identity_drift = copy.deepcopy(memory)
    identity_drift["request_digest"] = "semantic-v2:sha256:" + "0" * 64
    expect_failure(
        args.driver,
        encoded(identity_drift),
        b"materialization.identity-mismatch",
        b"request.request_digest",
    )

    task_drift = copy.deepcopy(memory)
    task_drift["tasks"][0]["task_input_digest"] = "sha256:" + "0" * 64
    oracle.bind_request_identity(task_drift)
    expect_failure(
        args.driver,
        encoded(task_drift),
        b"materialization.identity-mismatch",
        b"task.task_input_digest",
    )

    selector_drift = copy.deepcopy(memory)
    selector_drift["publication"]["selector"]["channel_id"] = "channel:drift"
    oracle.bind_request_identity(selector_drift)
    expect_failure(
        args.driver,
        encoded(selector_drift),
        b"materialization.identity-mismatch",
        b"publication.selector",
    )

    non_nfc = copy.deepcopy(memory)
    non_nfc["tasks"][0]["source"]["logical_path"] = "project://cafe\u0301.cpp"
    oracle.bind_request_identity(non_nfc)
    expect_failure(
        args.driver,
        encoded(non_nfc),
        b"materialization.identity-mismatch",
        b"source.logical_path",
    )

    valid = encoded(memory)
    duplicate = valid.replace(
        b'{"engine":',
        b'{"engine":null,"engine":',
        1,
    )
    expect_failure(args.driver, duplicate, b"materialization.request-invalid", b"json-decode")
    expect_failure(
        args.driver,
        b"\xef\xbb\xbf" + valid,
        b"materialization.request-invalid",
        b"json-decode",
    )
    expect_failure(
        args.driver,
        valid + b"{}",
        b"materialization.request-invalid",
        b"json-decode",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
