#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TARGETS = {
    "src/sdk/sqlite_same_process_shm_identity_issuer_internal.hpp": [
        "sqlite_shm_reader_zero_effect_identity_validation_capability",
        "class sqlite_shm_process_global_identity_issuer",
    ],
    "src/sdk/sqlite_same_process_shm_identity_issuer_internal.cpp": [
        "validate_zero_effect_identity_for_registry(",
        "zero_effect_capability_is_current(",
        "sqlite_shm_reader_zero_effect_identity_validation_capability::",
    ],
    "src/sdk/sqlite_same_process_shm_mapping_lease_internal.hpp": [
        "struct sqlite_shm_reader_map_identity_owner_control",
        "class sqlite_shm_verified_reader_attachment_post_map_receipt",
        "class sqlite_same_process_shm_reader_receipt_validator",
        "class sqlite_same_process_shm_reader_zero_effect_receipt_validator",
        "class sqlite_shm_reader_attachment_map_inflight",
        "commit_reader_map(",
    ],
    "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp": [
        "struct sqlite_shm_reader_map_identity_owner_control",
        "sqlite_shm_reader_zero_effect_receipt_control::~",
        "sqlite_shm_verified_reader_attachment_post_map_receipt::",
        "sqlite_same_process_shm_reader_receipt_validator::",
        "sqlite_same_process_shm_reader_zero_effect_receipt_validator::",
        "reader_zero_attachment_fingerprint(",
        "valid_reader_attachment_receipt(",
        "validate_reader_zero_attachment_effect(",
        "complete_reader_zero_attachment(",
        "commit_registry_reader_map(",
        "commit_reader(",
        "commit_reader_map(",
    ],
    "src/sdk/sqlite_same_process_shm_mapping_registry_internal.hpp": [
        "sqlite_same_process_shm_reader_zero_effect_receipt_validator",
        "validate_reader_zero_attachment_effect(",
        "commit_reader_map(",
    ],
    "src/sdk/sqlite_same_process_shm_mapping_registry_internal.cpp": [
        "validate_reader_zero_attachment_effect(",
        "commit_reader_map(",
    ],
    "tests/unit/sdk/sqlite_same_process_shm_mapping_registry_test.cpp": [
        "zero_attachment",
        "zero_effect",
        "commit_reader_map(",
        "int main(",
    ],
}

for rel, needles in TARGETS.items():
    lines = (ROOT / rel).read_text(encoding="utf-8").splitlines()
    print(f"[{rel}] lines={len(lines)}")
    for needle in needles:
        hits = [str(i + 1) for i, line in enumerate(lines) if needle in line]
        print(f"  {needle}: {','.join(hits) if hits else '-'}")
