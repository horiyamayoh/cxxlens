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


def expect_pass(driver: pathlib.Path, value: dict[str, Any]) -> None:
    result = run(driver, encoded(value))
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert result.stdout == b"ok\n"
    assert result.stderr == b""


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


def main() -> int:
    args = parse_args()
    sys.path.insert(0, str(args.root / "tools" / "quality"))
    import check_ng_clang22_materialization as oracle  # pylint: disable=import-error

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
        b"request.request_version",
    )

    unknown_schema_with_shape_error = copy.deepcopy(memory)
    unknown_schema_with_shape_error["schema"] = (
        "cxxlens.clang22-materialization-request.v3"
    )
    unknown_schema_with_shape_error["unexpected"] = True
    expect_failure(
        args.driver,
        encoded(unknown_schema_with_shape_error),
        b"materialization.version-unsupported",
        b"request.schema",
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
    stale_spelling_binding = copy.deepcopy(base64_alias)
    oracle.bind_request_identity(stale_spelling_binding)
    expect_failure(
        args.driver,
        encoded(stale_spelling_binding),
        b"materialization.identity-mismatch",
        b"task.task_input_digest",
    )
    oracle.bind_provider_task_identities(base64_alias)
    oracle.bind_task_execution_identities(base64_alias)
    oracle.bind_request_identity(base64_alias)
    oracle.validate_request(args.root, base64_alias)
    expect_pass(args.driver, base64_alias)

    one_pad_base64_alias = copy.deepcopy(memory)
    one_pad_base64_alias["project"]["catalog_compile_units"][0][
        "source_digest"
    ] = oracle.content_digest(b"ab")
    one_pad_base64_alias["tasks"][0]["source"]["content_base64"] = "YWJ="
    oracle.rebind_request_base_identities(args.root, one_pad_base64_alias)
    oracle.validate_request(args.root, one_pad_base64_alias)
    expect_pass(args.driver, one_pad_base64_alias)

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
        b"request.identity",
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
    expect_failure(args.driver, duplicate, b"materialization.json-invalid", b"input")
    expect_failure(
        args.driver,
        b"\xef\xbb\xbf" + valid,
        b"materialization.json-invalid",
        b"input",
    )
    expect_failure(args.driver, valid + b"{}", b"materialization.json-invalid", b"input")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
