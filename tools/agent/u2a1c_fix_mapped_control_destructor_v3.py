#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
path = ROOT / "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp"
t = path.read_text(encoding="utf-8")
implementation = (
    "\t\tsqlite_shm_reader_mapped_effect_receipt_control::\n"
    "\t\t\t~sqlite_shm_reader_mapped_effect_receipt_control() noexcept"
)
if implementation not in t:
    declaration = "\t\t\t~sqlite_shm_reader_mapped_effect_receipt_control() noexcept;"
    if declaration not in t:
        raise RuntimeError("mapped receipt control declaration was not produced")
    marker = (
        "\t\tsqlite_shm_reader_zero_effect_receipt_control::\n"
        "\t\t\t~sqlite_shm_reader_zero_effect_receipt_control() noexcept"
    )
    start = t.find(marker)
    if start < 0:
        raise RuntimeError("zero receipt control destructor was not found")
    brace = t.find("{", start)
    if brace < 0:
        raise RuntimeError("zero receipt control destructor body was not found")
    depth = 0
    end = None
    for index in range(brace, len(t)):
        if t[index] == "{":
            depth += 1
        elif t[index] == "}":
            depth -= 1
            if depth == 0:
                end = index + 1
                break
    if end is None:
        raise RuntimeError("zero receipt control destructor body is unbalanced")
    destructor = '''

\t\tsqlite_shm_reader_mapped_effect_receipt_control::
\t\t\t~sqlite_shm_reader_mapped_effect_receipt_control() noexcept
\t\t{
\t\t\tif (!process_epoch || expected_process_epoch == 0U ||
\t\t\t\tprocess_epoch->load(std::memory_order_acquire) != expected_process_epoch)
\t\t\t\treturn;
\t\t\tconst auto exact_owner = owner.lock();
\t\t\tif (exact_owner &&
\t\t\t\texact_owner->mapped_effect_validation_phase.load(std::memory_order_acquire) ==
\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::sealed)
\t\t\t\texact_owner->abandon();
\t\t}'''
    t = t[:end] + destructor + t[end:]
path.write_text(t, encoding="utf-8")
