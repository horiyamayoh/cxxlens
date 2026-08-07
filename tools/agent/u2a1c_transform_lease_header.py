#!/usr/bin/env python3
from u2a1c_transform_common import find_balanced_block, insert_after, insert_before, read, write

rel = "src/sdk/sqlite_same_process_shm_mapping_lease_internal.hpp"
t = read(rel)
if "sqlite_shm_reader_mapped_effect_receipt_control" not in t:
    t = insert_after(
        t,
        "\t\tstruct sqlite_shm_reader_zero_effect_receipt_control;",
        "\n\t\tstruct sqlite_shm_reader_mapped_effect_receipt_control;",
        "mapped receipt control forward",
    )
if "class sqlite_shm_reader_mapped_effect_identity_validation_capability;" not in t:
    t = insert_after(
        t,
        "\tclass sqlite_shm_reader_zero_effect_identity_validation_capability;",
        "\n\tclass sqlite_shm_reader_mapped_effect_identity_validation_capability;",
        "mapped capability forward",
    )

start, end, block = find_balanced_block(
    t, "\tclass sqlite_shm_reader_native_attachment_identity"
)
if "friend class detail::sqlite_shm_mapping_lease_state;" not in block:
    anchor = "\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;"
    if anchor not in block:
        raise RuntimeError("native attachment validator friend not found")
    block = block.replace(
        anchor,
        "\t\tfriend class detail::sqlite_shm_mapping_lease_state;\n" + anchor,
        1,
    )
    t = t[:start] + block + t[end:]

start, end, block = find_balanced_block(
    t, "\tclass sqlite_shm_verified_reader_attachment_post_map_receipt"
)
if "qualified_control_" not in block:
    block = block.replace(
        "\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;",
        "\t\tfriend class detail::sqlite_shm_mapping_lease_state;\n"
        "\t\tfriend class detail::sqlite_shm_mapping_registry_state;\n"
        "\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;",
        1,
    )
    block = block.replace(
        "\t\t\tsqlite_shm_reader_native_attachment_identity observed_attachment,\n"
        "\t\t\tsqlite_backend_opaque_identity zero_resize_effect_receipt);",
        "\t\t\tsqlite_shm_reader_native_attachment_identity observed_attachment,\n"
        "\t\t\tsqlite_backend_opaque_identity zero_resize_effect_receipt,\n"
        "\t\t\tstd::shared_ptr<detail::sqlite_shm_reader_mapped_effect_receipt_control>\n"
        "\t\t\t\tqualified_control = {});",
        1,
    )
    block = block.replace(
        "\t\tsqlite_backend_opaque_identity zero_resize_effect_receipt_;",
        "\t\tsqlite_backend_opaque_identity zero_resize_effect_receipt_;\n"
        "\t\tstd::shared_ptr<detail::sqlite_shm_reader_mapped_effect_receipt_control>\n"
        "\t\t\tqualified_control_;",
        1,
    )
    t = t[:start] + block + t[end:]

if "\tclass sqlite_same_process_shm_reader_receipt_validator final" not in t:
    mapped_validator = '''\t/**
\t * Closed validator for one exact qualified reader-map mapped native result.
\t *
\t * The request, generation, mapping tuple, and expected attachment are derived from the
\t * already-bound owner. The caller supplies only the closed native result and independently
\t * observed SHM object/entry/device/mount receipts. There is no raw request, mapping tuple,
\t * generation, or opaque-effect overload. Successful validation preserves the in-flight owner,
\t * scope, callback, and issued effect presenter until the terminal coordinator consumes the
\t * attached one-shot provenance.
\t */
\tclass sqlite_same_process_shm_reader_receipt_validator final
\t{
\t  public:
\t\tsqlite_same_process_shm_reader_receipt_validator() = delete;

\t\t[[nodiscard]] static
\t\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
\t\tvalidate(sqlite_same_process_shm_mapping_registry& registry,
\t\t\t sqlite_shm_registry_family_pin& family,
\t\t\t const sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\t const sqlite_shm_reader_lifecycle_identity_scope& scope,
\t\t\t const sqlite_shm_issued_reader_callback_identity& callback,
\t\t\t const sqlite_shm_issued_reader_effect_identity& effect,
\t\t\t int native_status,
\t\t\t const volatile void* native_mapping,
\t\t\t int delegated_extend,
\t\t\t sqlite_backend_opaque_identity observed_shm_object_receipt,
\t\t\t sqlite_backend_opaque_identity observed_shm_entry_receipt,
\t\t\t sqlite_backend_opaque_identity observed_device_receipt,
\t\t\t sqlite_backend_opaque_identity observed_mount_receipt) noexcept;
\t};

'''
    t = insert_before(
        t,
        "\tclass sqlite_same_process_shm_reader_zero_effect_receipt_validator final",
        mapped_validator,
        "mapped validator class",
    )

if "validate_registry_reader_mapped_attachment_effect(" not in t:
    zero = '''\t\tvalidate_registry_reader_zero_attachment_effect(
\t\t\tconst sqlite_shm_registry_family_pin& family,
\t\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\tsqlite_shm_reader_zero_effect_identity_validation_capability capability,
\t\t\tint native_status,
\t\t\tconst volatile void* native_mapping,
\t\t\tint delegated_extend) noexcept;'''
    mapped = '''
\t\t[[nodiscard]]
\t\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
\t\tvalidate_registry_reader_mapped_attachment_effect(
\t\t\tconst sqlite_shm_registry_family_pin& family,
\t\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\tsqlite_shm_reader_mapped_effect_identity_validation_capability capability,
\t\t\tint native_status,
\t\t\tconst volatile void* native_mapping,
\t\t\tint delegated_extend,
\t\t\tsqlite_backend_opaque_identity observed_shm_object_receipt,
\t\t\tsqlite_backend_opaque_identity observed_shm_entry_receipt,
\t\t\tsqlite_backend_opaque_identity observed_device_receipt,
\t\t\tsqlite_backend_opaque_identity observed_mount_receipt) noexcept;
'''
    t = insert_after(t, zero, mapped, "lease mapped validator declaration")
write(rel, t)
