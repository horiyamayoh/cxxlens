#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

RANGES: dict[str, list[tuple[int, int]]] = {
    "src/sdk/sqlite_same_process_shm_identity_issuer_internal.hpp": [
        (230, 360),
        (360, 455),
    ],
    "src/sdk/sqlite_same_process_shm_identity_issuer_internal.cpp": [
        (1000, 1105),
        (1570, 1610),
        (1710, 1770),
    ],
    "src/sdk/sqlite_same_process_shm_mapping_lease_internal.hpp": [
        (1, 125),
        (520, 630),
        (1385, 1495),
    ],
    "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp": [
        (7900, 8200),
        (8800, 9025),
    ],
    "src/sdk/sqlite_same_process_shm_mapping_registry_internal.cpp": [
        (1900, 2055),
        (2070, 2185),
    ],
    "tests/unit/sdk/sqlite_same_process_shm_mapping_registry_test.cpp": [
        (360, 520),
        (2320, 2760),
    ],
}

NEEDLES: dict[str, list[str]] = {
    "src/sdk/sqlite_same_process_shm_mapping_lease_internal.hpp": [
        "class sqlite_same_process_shm_reader_receipt_validator",
        "sqlite_shm_reader_zero_effect_validation_phase",
        "sqlite_shm_reader_map_identity_owner_control",
    ],
    "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp": [
        "sqlite_shm_reader_zero_effect_validation_phase",
        "sqlite_shm_reader_map_identity_owner_control",
        "sqlite_shm_verified_reader_attachment_post_map_receipt::",
        "sqlite_same_process_shm_reader_receipt_validator::",
        "commit_reader(",
        "valid_reader_attachment_post",
        "valid_reader_post",
    ],
    "src/sdk/sqlite_same_process_shm_mapping_registry_internal.cpp": [
        "validate_zero_effect_identity_for_registry",
        "commit_registry_reader_map",
    ],
    "tests/unit/sdk/sqlite_same_process_shm_mapping_registry_test.cpp": [
        "qualified_reader_map_owner",
        "reader_zero_effect",
        "validate_reader_zero_attachment",
        "int main(",
    ],
}


def read_lines(rel: str) -> list[str]:
    path = ROOT / rel
    if not path.is_file():
        raise SystemExit(f"missing: {rel}")
    return path.read_text(encoding="utf-8").splitlines()


def show_range(rel: str, start: int, end: int) -> None:
    lines = read_lines(rel)
    lo = max(1, start)
    hi = min(len(lines), end)
    print(f"\n===== RANGE {rel}:{lo}-{hi} =====")
    for index in range(lo - 1, hi):
        print(f"{index + 1:6}: {lines[index]}")


def show_needles(rel: str, needles: list[str]) -> None:
    lines = read_lines(rel)
    print(f"\n===== INDEX {rel} =====")
    for needle in needles:
        hits = [i + 1 for i, line in enumerate(lines) if needle in line]
        print(f"{needle!r}: {hits}")
        for line_no in hits[:3]:
            lo = max(1, line_no - 18)
            hi = min(len(lines), line_no + 42)
            print(f"--- {needle!r} context {lo}-{hi} ---")
            for index in range(lo - 1, hi):
                print(f"{index + 1:6}: {lines[index]}")


def main() -> None:
    for rel, ranges in RANGES.items():
        for start, end in ranges:
            show_range(rel, start, end)
    for rel, needles in NEEDLES.items():
        show_needles(rel, needles)


if __name__ == "__main__":
    main()
