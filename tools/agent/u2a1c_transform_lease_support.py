#!/usr/bin/env python3
from u2a1c_transform_common import find_balanced_block, insert_after, read, write

rel = "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp"
t = read(rel)

if "sqlite_shm_reader_mapped_effect_validation_phase" not in t:
    zero = '''\t\tenum class sqlite_shm_reader_zero_effect_validation_phase : std::uint8_t
\t\t{
\t\t\tunclaimed,
\t\t\tvalidating,
\t\t\tsealed,
\t\t\tterminal_consumed,
\t\t\tpoisoned,
\t\t};

\t\tstatic_assert(
\t\t\tstd::atomic<sqlite_shm_reader_zero_effect_validation_phase>::is_always_lock_free);'''
    mapped = '''

\t\tenum class sqlite_shm_reader_mapped_effect_validation_phase : std::uint8_t
\t\t{
\t\t\tunclaimed,
\t\t\tvalidating,
\t\t\tsealed,
\t\t\tterminal_consumed,
\t\t\tpoisoned,
\t\t};

\t\tstatic_assert(
\t\t\tstd::atomic<sqlite_shm_reader_mapped_effect_validation_phase>::is_always_lock_free);'''
    t = insert_after(t, zero, mapped, "mapped phase enum")

start, end, block = find_balanced_block(
    t, "\t\tstruct sqlite_shm_reader_map_identity_owner_control final"
)
if "mapped_effect_validation_phase" not in block:
    anchor = "\t\t\tstd::atomic<std::uint64_t> zero_effect_validation_fingerprint{0U};"
    addition = '''
\t\t\tstd::atomic<sqlite_shm_reader_mapped_effect_validation_phase>
\t\t\t\tmapped_effect_validation_phase{
\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::unclaimed};'''
    if anchor not in block:
        raise RuntimeError("zero fingerprint anchor missing")
    block = block.replace(anchor, anchor + addition, 1)
    t = t[:start] + block + t[end:]

if "struct sqlite_shm_reader_mapped_effect_receipt_control final" not in t:
    _, end, _ = find_balanced_block(
        t, "\t\tstruct sqlite_shm_reader_zero_effect_receipt_control final"
    )
    mapped = '''

\t\tstruct sqlite_shm_reader_mapped_effect_receipt_control final
\t\t{
\t\t\tsqlite_shm_reader_mapped_effect_receipt_control(
\t\t\t\tsqlite_shm_reader_mapped_effect_identity_validation_capability capability_value,
\t\t\t\tconst std::shared_ptr<sqlite_shm_reader_map_identity_owner_control>& owner_value,
\t\t\t\tconst int native_status_value,
\t\t\t\tconst int delegated_extend_value,
\t\t\t\tsqlite_shm_mapping_tuple mapping_value,
\t\t\t\tsqlite_shm_reader_native_attachment_identity observed_attachment_value) noexcept
\t\t\t\t: capability{std::move(capability_value)}, owner{owner_value},
\t\t\t\t  process_epoch{owner_value ? owner_value->process_epoch : nullptr},
\t\t\t\t  expected_process_epoch{owner_value ? owner_value->expected_process_epoch : 0U},
\t\t\t\t  native_status{native_status_value}, delegated_extend{delegated_extend_value},
\t\t\t\t  mapping{mapping_value}, observed_attachment{std::move(observed_attachment_value)}
\t\t\t{
\t\t\t}
\t\t\t~sqlite_shm_reader_mapped_effect_receipt_control() noexcept;

\t\t\tsqlite_shm_reader_mapped_effect_identity_validation_capability capability;
\t\t\tstd::weak_ptr<sqlite_shm_reader_map_identity_owner_control> owner;
\t\t\tstd::shared_ptr<std::atomic<std::uint64_t>> process_epoch;
\t\t\tstd::uint64_t expected_process_epoch{};
\t\t\tint native_status{};
\t\t\tint delegated_extend{};
\t\t\tsqlite_shm_mapping_tuple mapping;
\t\t\tsqlite_shm_reader_native_attachment_identity observed_attachment;
\t\t};'''
    t = t[:end] + mapped + t[end:]

marker = (
    "\tsqlite_shm_verified_reader_attachment_post_map_receipt::\n"
    "\t\tsqlite_shm_verified_reader_attachment_post_map_receipt("
)
start, end, block = find_balanced_block(t, marker)
if "qualified_control" not in block:
    block = block.replace(
        "\t\tsqlite_shm_reader_native_attachment_identity observed_attachment,\n"
        "\t\tsqlite_backend_opaque_identity zero_resize_effect_receipt)",
        "\t\tsqlite_shm_reader_native_attachment_identity observed_attachment,\n"
        "\t\tsqlite_backend_opaque_identity zero_resize_effect_receipt,\n"
        "\t\tstd::shared_ptr<detail::sqlite_shm_reader_mapped_effect_receipt_control>\n"
        "\t\t\tqualified_control)",
        1,
    )
    block = block.replace(
        "\t\t  zero_resize_effect_receipt_{std::move(zero_resize_effect_receipt)}",
        "\t\t  zero_resize_effect_receipt_{std::move(zero_resize_effect_receipt)},\n"
        "\t\t  qualified_control_{std::move(qualified_control)}",
        1,
    )
    t = t[:start] + block + t[end:]

if "sqlite_same_process_shm_reader_receipt_validator::validate(" not in t:
    marker = (
        "\tconst sqlite_backend_opaque_identity&\n"
        "\tsqlite_shm_verified_reader_attachment_post_map_receipt::zero_resize_effect_receipt()"
    )
    _, end, _ = find_balanced_block(t, marker)
    implementation = '''

\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
\tsqlite_same_process_shm_reader_receipt_validator::validate(
\t\tsqlite_same_process_shm_mapping_registry& registry,
\t\tsqlite_shm_registry_family_pin& family,
\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\tconst sqlite_shm_reader_lifecycle_identity_scope& scope,
\t\tconst sqlite_shm_issued_reader_callback_identity& callback,
\t\tconst sqlite_shm_issued_reader_effect_identity& effect,
\t\tconst int native_status,
\t\tconst volatile void* native_mapping,
\t\tconst int delegated_extend,
\t\tsqlite_backend_opaque_identity observed_shm_object_receipt,
\t\tsqlite_backend_opaque_identity observed_shm_entry_receipt,
\t\tsqlite_backend_opaque_identity observed_device_receipt,
\t\tsqlite_backend_opaque_identity observed_mount_receipt) noexcept
\t{
\t\treturn registry.validate_reader_mapped_attachment_effect(family,
\t\t\tinflight,
\t\t\tscope,
\t\t\tcallback,
\t\t\teffect,
\t\t\tnative_status,
\t\t\tnative_mapping,
\t\t\tdelegated_extend,
\t\t\tstd::move(observed_shm_object_receipt),
\t\t\tstd::move(observed_shm_entry_receipt),
\t\t\tstd::move(observed_device_receipt),
\t\t\tstd::move(observed_mount_receipt));
\t}'''
    t = t[:end] + implementation + t[end:]

if "~sqlite_shm_reader_mapped_effect_receipt_control() noexcept" not in t:
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
