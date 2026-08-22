#!/usr/bin/env python3
"""Validate installed Clang 22 runtime identity and task input binding.

The installed materializer report is a product runtime response.  This module
only contains the small assertions shared by the installed tests; it does not
collect, persist, or aggregate test reports, receipts, or evidence files.
"""

from __future__ import annotations

import base64
import pathlib
from typing import Any

import check_ng_clang22_materialization as oracle


class InstallMatrixError(ValueError):
    """An installed materialization identity or task binding is invalid."""


def resolve_installed_executable(
    prefix: pathlib.Path,
    canonical_relative_path: str,
    executable_suffix: str = "",
) -> pathlib.Path:
    """Resolve a canonical manifest path to the platform file name."""

    if not isinstance(canonical_relative_path, str) or not canonical_relative_path:
        raise InstallMatrixError("installed executable path is not a non-empty string")
    relative = pathlib.PurePosixPath(canonical_relative_path)
    if (
        relative.is_absolute()
        or "\\" in canonical_relative_path
        or "\x00" in canonical_relative_path
        or relative.as_posix() != canonical_relative_path
        or any(part in {"", ".", ".."} for part in relative.parts)
    ):
        raise InstallMatrixError(
            f"installed executable path is not canonical: {canonical_relative_path!r}"
        )
    if not isinstance(executable_suffix, str) or any(
        character in executable_suffix for character in ("\x00", "/", "\\")
    ):
        raise InstallMatrixError(
            f"configured executable suffix is not a filename suffix: {executable_suffix!r}"
        )
    candidate = prefix.joinpath(*relative.parts)
    return candidate.with_name(candidate.name + executable_suffix)


def validate_installed_task_v3_source_binding(
    request: dict[str, Any], report: dict[str, Any]
) -> None:
    """Assert canonical source spelling, task identity, and runtime receipts."""

    tasks = {oracle.task_execution_key(task): task for task in request["tasks"]}
    results = {
        oracle.task_execution_key(result): result
        for result in report.get("task_results", [])
    }
    if set(tasks) != set(results):
        raise InstallMatrixError(
            "installed task.v3 source binding has a different task execution census"
        )

    for key, task in tasks.items():
        result = results[key]
        spelling = task["source"]["content_base64"]
        try:
            source = oracle.decode_canonical_base64(spelling)
        except oracle.MaterializationError as error:
            raise InstallMatrixError(
                f"installed task.v3 source Base64 is not canonical for {key}: {error}"
            ) from error
        if base64.b64encode(source).decode("ascii") != spelling:
            raise InstallMatrixError(
                f"installed task.v3 source Base64 spelling is not unique for {key}"
            )

        expected_task_input = oracle.expected_task_input_digest(request, task)
        if task["task_input_digest"] != expected_task_input:
            raise InstallMatrixError(
                f"installed task.v3 digest is not bound to canonical source bytes for {key}"
            )
        if result["task_input_digest"] != expected_task_input:
            raise InstallMatrixError(
                f"installed report task.v3 digest differs for {key}"
            )

        expected_execution = oracle.expected_provider_execution_id(request, task)
        if task["provider_execution_id"] != expected_execution:
            raise InstallMatrixError(
                f"installed provider execution identity is not source-bound for {key}"
            )
        if result["provider_execution_id"] != expected_execution:
            raise InstallMatrixError(
                f"installed report provider execution identity differs for {key}"
            )
        if result["input_transfer"] != oracle.expected_input_transfer_receipt(
            request, task
        ):
            raise InstallMatrixError(
                f"installed task.v3 input transfer receipt differs for {key}"
            )
        sealed_digest = result.get("runtime_receipt", {}).get(
            "sealed_transcript_digest"
        )
        if not isinstance(sealed_digest, str) or not sealed_digest:
            raise InstallMatrixError(
                f"installed task.v3 result lacks a sealed provider transcript for {key}"
            )
