#!/usr/bin/env python3
from u2a1c_transform_common import insert_after, insert_before, read, write

rel = "src/sdk/sqlite_same_process_shm_mapping_registry_internal.hpp"
t = read(rel)
if "class sqlite_same_process_shm_reader_receipt_validator;" not in t:
    t = insert_before(
        t,
        "\tclass sqlite_same_process_shm_reader_zero_effect_receipt_validator;",
        "\tclass sqlite_same_process_shm_reader_receipt_validator;\n",
        "mapped validator forward",
    )
if "\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;" not in t:
    t = insert_before(
        t,
        "\t\tfriend class sqlite_same_process_shm_reader_zero_effect_receipt_validator;",
        "\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;\n",
        "mapped validator friend",
    )
if "validate_reader_mapped_attachment_effect(" not in t:
    zero = '''\t\tvalidate_reader_zero_attachment_effect(
\t\t\tsqlite_shm_registry_family_pin& family,
\t\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\tconst sqlite_shm_reader_lifecycle_identity_scope& scope,
\t\t\tconst sqlite_shm_issued_reader_callback_identity& callback,
\t\t\tconst sqlite_shm_issued_reader_effect_identity& effect,
\t\t\tint native_status,
\t\t\tconst volatile void* native_mapping,
\t\t\tint delegated_extend) noexcept;'''
    mapped = '''
\t\t[[nodiscard]]
\t\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
\t\tvalidate_reader_mapped_attachment_effect(
\t\t\tsqlite_shm_registry_family_pin& family,
\t\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\tconst sqlite_shm_reader_lifecycle_identity_scope& scope,
\t\t\tconst sqlite_shm_issued_reader_callback_identity& callback,
\t\t\tconst sqlite_shm_issued_reader_effect_identity& effect,
\t\t\tint native_status,
\t\t\tconst volatile void* native_mapping,
\t\t\tint delegated_extend,
\t\t\tsqlite_backend_opaque_identity observed_shm_object_receipt,
\t\t\tsqlite_backend_opaque_identity observed_shm_entry_receipt,
\t\t\tsqlite_backend_opaque_identity observed_device_receipt,
\t\t\tsqlite_backend_opaque_identity observed_mount_receipt) noexcept;
'''
    t = insert_after(t, zero, mapped, "mapped registry private method")
write(rel, t)

rel = "src/sdk/sqlite_same_process_shm_mapping_registry_internal.cpp"
t = read(rel)
if "validate_mapped_effect_identity_for_registry(" not in t:
    zero = '''\t\tsqlite_shm_lease_result<sqlite_shm_reader_zero_effect_identity_validation_capability>
\t\tvalidate_zero_effect_identity_for_registry(
\t\t\tconst std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
\t\t\tconst sqlite_shm_reader_lifecycle_identity_scope& scope,
\t\t\tconst sqlite_shm_issued_reader_callback_identity& callback,
\t\t\tconst sqlite_shm_issued_reader_effect_identity& effect,
\t\t\tconst std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
\t\t\t\texpected_owner) noexcept;'''
    mapped = '''
\t\t[[nodiscard]]
\t\tsqlite_shm_lease_result<sqlite_shm_reader_mapped_effect_identity_validation_capability>
\t\tvalidate_mapped_effect_identity_for_registry(
\t\t\tconst std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
\t\t\tconst sqlite_shm_reader_lifecycle_identity_scope& scope,
\t\t\tconst sqlite_shm_issued_reader_callback_identity& callback,
\t\t\tconst sqlite_shm_issued_reader_effect_identity& effect,
\t\t\tconst std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
\t\t\t\texpected_owner) noexcept;'''
    t = insert_after(t, zero, mapped, "mapped issuer detail declaration")

if "\t\t\tvalidate_reader_mapped_attachment_effect(" not in t:
    mapped = '''\t\t\t[[nodiscard]]
\t\t\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
\t\t\tvalidate_reader_mapped_attachment_effect(
\t\t\t\tsqlite_shm_registry_family_pin& pin,
\t\t\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\t\tconst sqlite_shm_reader_lifecycle_identity_scope& scope,
\t\t\t\tconst sqlite_shm_issued_reader_callback_identity& callback,
\t\t\t\tconst sqlite_shm_issued_reader_effect_identity& effect,
\t\t\t\tconst int native_status,
\t\t\t\tconst volatile void* native_mapping,
\t\t\t\tconst int delegated_extend,
\t\t\t\tsqlite_backend_opaque_identity observed_shm_object_receipt,
\t\t\t\tsqlite_backend_opaque_identity observed_shm_entry_receipt,
\t\t\t\tsqlite_backend_opaque_identity observed_device_receipt,
\t\t\t\tsqlite_backend_opaque_identity observed_mount_receipt,
\t\t\t\tconst std::shared_ptr<sqlite_shm_process_identity_issuer_state>& issuer) noexcept
\t\t\t{
\t\t\t\tif (!current(pin.process_epoch_))
\t\t\t\t\treturn rejection(sqlite_shm_lease_rejection_reason::stale_token,
\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry);
\t\t\t\tif (inflight.terminal_presentation_stale_for_registry())
\t\t\t\t\treturn rejection(sqlite_shm_lease_rejection_reason::stale_token,
\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry);
\t\t\t\ttry
\t\t\t\t{
\t\t\t\t\tstd::scoped_lock lock{mutex_};
\t\t\t\t\tif (pin.state_.get() != this)
\t\t\t\t\t\treturn rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry);
\t\t\t\t\tsynchronize_activity_controls_locked();
\t\t\t\t\tsynchronize_reader_open_controls_locked();
\t\t\t\t\tsynchronize_coordinator_quarantines_locked();
\t\t\t\t\tconst auto exact_owner = inflight.qualified_identity_owned_for_registry(
\t\t\t\t\t\tpin.family_epoch_, pin.pin_token_, pin.alias_token_, seal_->process_epoch,
\t\t\t\t\t\tactivity_emergency_latch_);
\t\t\t\t\tif (!inflight.has_qualified_identity_for_registry())
\t\t\t\t\t\treturn rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry);
\t\t\t\t\tif (!exact_owner)
\t\t\t\t\t\treturn rejection(sqlite_shm_lease_rejection_reason::stale_token,
\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry);
\t\t\t\t\tauto* family_pin = current_family_pin_locked(pin);
\t\t\t\t\tauto* alias = find_alias_locked(pin.alias_token_);
\t\t\t\t\tauto* family = find_family_epoch_locked(pin.family_epoch_);
\t\t\t\t\tif (family_pin == nullptr || alias == nullptr || family == nullptr ||
\t\t\t\t\t\t!family->coordinator || admission_quarantined_locked())
\t\t\t\t\t\treturn rejection(sqlite_shm_lease_rejection_reason::stale_token,
\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry);
\t\t\t\t\tauto capability = detail::validate_mapped_effect_identity_for_registry(issuer,
\t\t\t\t\t\tscope,
\t\t\t\t\t\tcallback,
\t\t\t\t\t\teffect,
\t\t\t\t\t\tinflight.qualified_identity_owner_abandonment_for_registry());
\t\t\t\t\tif (!capability)
\t\t\t\t\t\treturn capability.error();
\t\t\t\t\treturn family->coordinator->validate_registry_reader_mapped_attachment_effect(
\t\t\t\t\t\tpin,
\t\t\t\t\t\tinflight,
\t\t\t\t\t\tstd::move(*capability),
\t\t\t\t\t\tnative_status,
\t\t\t\t\t\tnative_mapping,
\t\t\t\t\t\tdelegated_extend,
\t\t\t\t\t\tstd::move(observed_shm_object_receipt),
\t\t\t\t\t\tstd::move(observed_shm_entry_receipt),
\t\t\t\t\t\tstd::move(observed_device_receipt),
\t\t\t\t\t\tstd::move(observed_mount_receipt));
\t\t\t\t}
\t\t\t\tcatch (...)
\t\t\t\t{
\t\t\t\t\treturn rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry);
\t\t\t\t}
\t\t\t}

'''
    marker = (
        "\t\t\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_zero_effect_receipt>\n"
        "\t\t\tvalidate_reader_zero_attachment_effect("
    )
    t = insert_before(t, marker, mapped, "mapped registry state method")

if "sqlite_same_process_shm_mapping_registry::validate_reader_mapped_attachment_effect(" not in t:
    wrapper = '''\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
\tsqlite_same_process_shm_mapping_registry::validate_reader_mapped_attachment_effect(
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
\t\treturn state_->validate_reader_mapped_attachment_effect(family,
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
\t\t\tstd::move(observed_mount_receipt),
\t\t\tidentity_issuer_state_);
\t}

'''
    marker = (
        "\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_zero_effect_receipt>\n"
        "\tsqlite_same_process_shm_mapping_registry::validate_reader_zero_attachment_effect("
    )
    t = insert_before(t, marker, wrapper, "mapped registry wrapper")
write(rel, t)
