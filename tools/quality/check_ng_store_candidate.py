#!/usr/bin/env python3
"""Check the bounded Store candidate/reference projection implementation binding.

The autonomy model proves the #200 state machine symbolically.  This checker keeps that
authority from becoming detached from the source-private reference port and its focused test.
It deliberately checks only the bounded implementation slice; it does not claim production Store
or release qualification.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = pathlib.Path("src/llvm/clang22/materialization_store_candidate.hpp")
SOURCE = pathlib.Path("src/llvm/clang22/materialization_store_candidate.cpp")
TEST = pathlib.Path("tests/adapter/clang22/materialization_store_candidate_test.cpp")
BUILD = pathlib.Path("CMakeLists.txt")
TEST_BUILD = pathlib.Path("tests/CMakeLists.txt")
PRODUCTION_SOURCES = (
    pathlib.Path("tools/clang22/materialize_main.cpp"),
    pathlib.Path("src/llvm/clang22/materialization_store.cpp"),
    pathlib.Path("src/llvm/clang22/materialization_incremental_coordinator.cpp"),
    pathlib.Path("src/llvm/clang22/materialization_public_report.cpp"),
)

# ADR 0103 is still proposed. Until its exact candidate endpoint is independently accepted,
# the source-private reference port must not become an accidental production activation. This
# narrower guard does not claim that the current bulk Store path is residency-qualified.
FORBIDDEN_PRODUCTION_ACTIVATION = (
    '#include "llvm/clang22/materialization_store_candidate.hpp"',
    "begin_bounded_store_candidate(",
    "bounded_store_candidate::",
)


class StoreCandidateError(ValueError):
    """The source-private #200 candidate witness is incomplete or weakened."""


def read(root: pathlib.Path, relative: pathlib.Path) -> str:
    path = root / relative
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise StoreCandidateError(f"cannot read {relative}: {error}") from error


def require_tokens(text: str, tokens: tuple[str, ...], label: str) -> None:
    missing = [token for token in tokens if token not in text]
    if missing:
        raise StoreCandidateError(f"{label} missing required token(s): {', '.join(missing)}")


def reject_tokens(text: str, tokens: tuple[str, ...], label: str) -> None:
    present = [token for token in tokens if token in text]
    if present:
        raise StoreCandidateError(f"{label} contains forbidden token(s): {', '.join(present)}")


def validate(root: pathlib.Path) -> None:
    header = read(root, HEADER)
    source = read(root, SOURCE)
    test = read(root, TEST)
    build = read(root, BUILD)
    test_build = read(root, TEST_BUILD)
    production = {relative: read(root, relative) for relative in PRODUCTION_SOURCES}

    required_constants = {
        "bounded_store_max_tasks": "4096U",
        "bounded_store_max_aggregate_bytes": "512U * 1024U * 1024U",
        "bounded_store_resident_window_bytes": "77'729'792U",
        "bounded_store_report_tail_bytes": "28'321'546U",
        "bounded_store_max_report_bytes": "1024U * 1024U * 1024U",
        "bounded_store_sort_arena_bytes": "8U * 1024U * 1024U",
        "bounded_store_comparator_cursor_bytes": "32U * 1024U",
        "bounded_store_merge_file_descriptors": "18U",
        "bounded_store_sqlite_chunk_bytes": "8U * 1024U * 1024U",
    }
    for name, value in required_constants.items():
        if not re.search(rf"\b{name}\s*=\s*{re.escape(value)}", header):
            raise StoreCandidateError(f"bounded constant drift: {name}")

    require_tokens(
        header,
        (
            "bounded_store_record_cursor",
            "bounded_store_record_spool",
            "bounded_store_publication_port",
            "bounded_store_report_writer",
            "bounded_store_candidate_phase",
            "publication_outcome_unknown",
            "build_expected_projection",
            "build_actual_projection",
            "compare_projections",
            "reserve_report_tail",
            "finish_without_publication",
            "publish_once",
            "finalize_report",
        ),
        "candidate header",
    )
    require_tokens(
        source,
        (
            "encode_bounded_store_record",
            "decode_bounded_store_record",
            "record_cursor_impl",
            "validate_actual_order",
            "**expected_record != **actual_record",
            "*expected_bytes != *actual_bytes",
            "full-byte-mismatch",
            "if (auto reserved = report.reserve(); !reserved)",
            "state_->report_reserved = true",
            "state_->phase = bounded_store_candidate_phase::report_tail_reserved",
            "state_->terminal = bounded_store_publication_terminal::publication_outcome_unknown",
            "catch (...)",
            "state_->phase = bounded_store_candidate_phase::aborted",
            "publication-outcome-invalid",
            "publication_outcome_unknown",
            "report_transport_failed",
            "semantic_digest(",
        ),
        "candidate source",
    )
    require_tokens(
        test,
        (
            "positive_memory_and_sqlite_compatible_port_contract",
            "full_byte_projection_tamper_and_order_are_rejected",
            "unknown_terminal_is_fail_closed_and_not_retried",
            "report_reservation_and_resource_bounds_are_enforced",
            "not_attempted_terminal_is_fail_closed_and_not_retried",
            "abort_is_terminal_and_does_not_retry",
            "backend.calls == 1U",
            "backend.calls == 0U",
            "full-byte-mismatch",
        ),
        "candidate test",
    )
    require_tokens(
        build,
        ("src/llvm/clang22/materialization_store_candidate.cpp",),
        "materializer target",
    )
    require_tokens(
        test_build,
        (
            "adapter/clang22/materialization_store_candidate_test.cpp",
            "adapter.clang22-materialization-store-candidate",
        ),
        "candidate test registration",
    )
    for relative, text in production.items():
        reject_tokens(text, FORBIDDEN_PRODUCTION_ACTIVATION, f"production boundary {relative}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    args = parser.parse_args()
    try:
        validate(args.root.resolve())
    except StoreCandidateError as error:
        print(f"ng-store-candidate: {error}", file=sys.stderr)
        return 1
    print("ng-store-candidate: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
