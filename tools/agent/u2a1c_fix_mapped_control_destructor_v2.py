#!/usr/bin/env python3
from u2a1c_transform_common import find_balanced_block, read, write

rel = "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp"
t = read(rel)
implementation = (
    "\t\tsqlite_shm_reader_mapped_effect_receipt_control::\n"
    "\t\t\t~sqlite_shm_reader_mapped_effect_receipt_control() noexcept"
)
if implementation not in t:
    declaration = "\t\t\t~sqlite_shm_reader_mapped_effect_receipt_control() noexcept;"
    if declaration not in t:
        raise RuntimeError("mapped receipt control declaration was not produced")
    _, end, _ = find_balanced_block(
        t,
        "\t\tsqlite_shm_reader_zero_effect_receipt_control::\n"
        "\t\t\t~sqlite_shm_reader_zero_effect_receipt_control() noexcept",
    )
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
write(rel, t)
