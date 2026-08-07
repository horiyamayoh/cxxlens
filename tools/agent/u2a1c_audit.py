#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QUERIES: dict[str, list[str]] = {
    "src/sdk/sqlite_same_process_shm_identity_issuer_internal.hpp": [
        "sqlite_shm_reader_zero_effect_identity_validation_capability",
        "sqlite_shm_reader_effect_identity_role",
        "validate_zero_effect_identity_for_registry",
    ],
    "src/sdk/sqlite_same_process_shm_identity_issuer_internal.cpp": [
        "validate_zero_effect_identity_for_registry",
        "zero_effect_capability_is_current",
        "sqlite_shm_reader_zero_effect_identity_validation_capability::",
        "validate_effect(",
    ],
    "src/sdk/sqlite_same_process_shm_mapping_lease_internal.hpp": [
        "sqlite_shm_verified_reader_attachment_post_map_receipt",
        "sqlite_shm_verified_reader_attachment_zero_effect_receipt",
        "sqlite_same_process_shm_reader_receipt_validator",
        "sqlite_shm_reader_zero_effect_receipt_control",
        "sqlite_shm_reader_attachment_map_inflight",
    ],
    "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp": [
        "classify_reader_zero_attachment_native_result",
        "reader_zero_attachment_fingerprint",
        "valid_reader_post_map_receipt",
        "validate_reader_zero_attachment_effect",
        "complete_reader_zero_attachment_map",
        "commit_reader_map(",
    ],
    "src/sdk/sqlite_same_process_shm_mapping_registry_internal.hpp": [
        "sqlite_same_process_shm_reader_zero_effect_receipt_validator",
        "validate_reader_zero_attachment_effect",
        "commit_reader_map(",
    ],
    "src/sdk/sqlite_same_process_shm_mapping_registry_internal.cpp": [
        "sqlite_same_process_shm_reader_zero_effect_receipt_validator",
        "validate_reader_zero_attachment_effect",
        "prepare_reader_map_identity(",
        "bind_reader_map_identity(",
        "commit_reader_map(",
    ],
    "tests/unit/sdk/sqlite_same_process_shm_mapping_registry_test.cpp": [
        "zero_attachment",
        "zero-effect",
        "zero_effect",
        "mapped_result",
    ],
    "tests/unit/sdk/sqlite_same_process_shm_identity_issuer_test.cpp": [
        "zero_effect",
        "mapped_result",
    ],
}


def print_context(path: Path, needle: str, radius: int = 24, max_hits: int = 4) -> None:
    lines = path.read_text(encoding="utf-8").splitlines()
    hits = [i for i, line in enumerate(lines) if needle in line]
    print(f"\n===== {path.relative_to(ROOT)} :: {needle!r} :: hits={len(hits)} =====")
    for ordinal, index in enumerate(hits[:max_hits], start=1):
        start = max(0, index - radius)
        end = min(len(lines), index + radius + 1)
        print(f"--- hit {ordinal}/{len(hits)} lines {start + 1}-{end} ---")
        for line_no in range(start, end):
            print(f"{line_no + 1:6}: {lines[line_no]}")


def main() -> None:
    for rel, needles in QUERIES.items():
        path = ROOT / rel
        if not path.is_file():
            raise SystemExit(f"missing audit file: {rel}")
        for needle in needles:
            print_context(path, needle)


if __name__ == "__main__":
    main()
