#!/usr/bin/env python3
from u2a1c_transform_common import find_balanced_block, insert_before, read, write

rel = "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp"
t = read(rel)

if "\t\t\tvalidate_reader_mapped_attachment_effect(" not in t:
    mapped = '''\t\t\t[[nodiscard]]
\t\t\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
\t\t\tvalidate_reader_mapped_attachment_effect(
\t\t\t\tconst sqlite_shm_registry_family_pin& registry_family,
\t\t\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\t\tsqlite_shm_reader_mapped_effect_identity_validation_capability capability,
\t\t\t\tconst int native_status,
\t\t\t\tconst volatile void* native_mapping,
\t\t\t\tconst int delegated_extend,
\t\t\t\tsqlite_backend_opaque_identity observed_shm_object_receipt,
\t\t\t\tsqlite_backend_opaque_identity observed_shm_entry_receipt,
\t\t\t\tsqlite_backend_opaque_identity observed_device_receipt,
\t\t\t\tsqlite_backend_opaque_identity observed_mount_receipt) noexcept
\t\t\t{
\t\t\t\tstd::shared_ptr<sqlite_shm_reader_map_identity_owner_control> exact_owner;
\t\t\t\tbool observation_burned{};
\t\t\t\ttry
\t\t\t\t{
\t\t\t\t\tstd::scoped_lock lock{mutex_};
\t\t\t\t\tif (!owns(inflight.state_, inflight.token_))
\t\t\t\t\t\treturn sqlite_shm_unexpected(stale_token(
\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry));
\t\t\t\t\tconst auto map = find_by_token(reader_attachment_maps_, inflight.token_);
\t\t\t\t\tif (map == reader_attachment_maps_.end() ||
\t\t\t\t\t\tmap->phase != reader_phase::inflight || !map->registry_bound ||
\t\t\t\t\t\t!map->qualified_identity_bound || !map->identity_owner_control ||
\t\t\t\t\t\tmap->identity_owner_control.get() != inflight.qualified_owner_control_.get() ||
\t\t\t\t\t\tmap->qualified_owner_phase.get() != inflight.qualified_owner_phase_.get() ||
\t\t\t\t\t\tmap->generation != inflight.generation_ ||
\t\t\t\t\t\t(map->registry_predelegate_authority &&
\t\t\t\t\t\t !map->registry_predelegate_authority->retains_exact_owned_terminal_lifetimes(
\t\t\t\t\t\t\t registry_family, map->request)))
\t\t\t\t\t\treturn sqlite_shm_unexpected(rejection(
\t\t\t\t\t\t\tsqlite_shm_lease_rejection_reason::receipt_mismatch,
\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry));

\t\t\t\t\texact_owner = map->identity_owner_control;
\t\t\t\t\tconst std::shared_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>
\t\t\t\t\t\texact_owner_base = exact_owner;
\t\t\t\t\tif (!capability.matches_live_owner(exact_owner_base) ||
\t\t\t\t\t\texact_owner->disposition.load(std::memory_order_acquire) !=
\t\t\t\t\t\t\tsqlite_shm_reader_map_identity_disposition::live)
\t\t\t\t\t\treturn sqlite_shm_unexpected(rejection(
\t\t\t\t\t\t\tsqlite_shm_lease_rejection_reason::receipt_mismatch,
\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry));

\t\t\t\t\tauto expected_phase =
\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::unclaimed;
\t\t\t\t\tif (!exact_owner->mapped_effect_validation_phase.compare_exchange_strong(
\t\t\t\t\t\t\texpected_phase,
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::validating,
\t\t\t\t\t\t\tstd::memory_order_acq_rel,
\t\t\t\t\t\t\tstd::memory_order_acquire))
\t\t\t\t\t{
\t\t\t\t\t\tif (expected_phase ==
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::terminal_consumed)
\t\t\t\t\t\t\treturn sqlite_shm_unexpected(stale_token(
\t\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry));
\t\t\t\t\t\texact_owner->mapped_effect_validation_phase.store(
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::poisoned,
\t\t\t\t\t\t\tstd::memory_order_release);
\t\t\t\t\t\tmap->quarantine_reason =
\t\t\t\t\t\t\tsqlite_shm_reader_terminal_quarantine_reason::presented_invalid;
\t\t\t\t\t\tquarantine_reader_map_terminal_commit_locked(map->token, map->session_token);
\t\t\t\t\t\treturn sqlite_shm_unexpected(ambiguous());
\t\t\t\t\t}
\t\t\t\t\tobservation_burned = true;

\t\t\t\t\tconst auto exact_native_result =
\t\t\t\t\t\tnative_status == static_cast<int>(sqlite_native_map_status::ok) &&
\t\t\t\t\t\tnative_mapping != nullptr && delegated_extend == 0 &&
\t\t\t\t\t\tmap->expected_mapping.native_mapping == native_mapping &&
\t\t\t\t\t\tvalid_identity(observed_shm_object_receipt) &&
\t\t\t\t\t\tvalid_identity(observed_shm_entry_receipt) &&
\t\t\t\t\t\tvalid_identity(observed_device_receipt) &&
\t\t\t\t\t\tvalid_identity(observed_mount_receipt);
\t\t\t\t\tif (!exact_native_result)
\t\t\t\t\t{
\t\t\t\t\t\texact_owner->mapped_effect_validation_phase.store(
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::poisoned,
\t\t\t\t\t\t\tstd::memory_order_release);
\t\t\t\t\t\tmap->quarantine_reason =
\t\t\t\t\t\t\tsqlite_shm_reader_terminal_quarantine_reason::presented_invalid;
\t\t\t\t\t\tquarantine_reader_map_terminal_commit_locked(map->token, map->session_token);
\t\t\t\t\t\treturn sqlite_shm_unexpected(ambiguous());
\t\t\t\t\t}

\t\t\t\t\tauto request = map->request;
\t\t\t\t\tauto mapping = map->expected_mapping;
\t\t\t\t\tauto observed_attachment = sqlite_shm_reader_native_attachment_identity{
\t\t\t\t\t\trequest.expected_attachment,
\t\t\t\t\t\tstd::move(observed_shm_object_receipt),
\t\t\t\t\t\tstd::move(observed_shm_entry_receipt),
\t\t\t\t\t\tstd::move(observed_device_receipt),
\t\t\t\t\t\tstd::move(observed_mount_receipt)};
\t\t\t\t\tauto effect_identity = capability.copy_effect_identity();
\t\t\t\t\tif (!capability.matches_live_owner(exact_owner_base) ||
\t\t\t\t\t\texact_owner->disposition.load(std::memory_order_acquire) !=
\t\t\t\t\t\t\tsqlite_shm_reader_map_identity_disposition::live)
\t\t\t\t\t{
\t\t\t\t\t\texact_owner->mapped_effect_validation_phase.store(
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::poisoned,
\t\t\t\t\t\t\tstd::memory_order_release);
\t\t\t\t\t\tmap->quarantine_reason =
\t\t\t\t\t\t\tsqlite_shm_reader_terminal_quarantine_reason::owner_abandoned;
\t\t\t\t\t\tquarantine_reader_map_terminal_commit_locked(map->token, map->session_token);
\t\t\t\t\t\treturn sqlite_shm_unexpected(ambiguous());
\t\t\t\t\t}

\t\t\t\t\tauto qualified_control =
\t\t\t\t\t\tstd::make_shared<sqlite_shm_reader_mapped_effect_receipt_control>(
\t\t\t\t\t\t\tstd::move(capability),
\t\t\t\t\t\t\texact_owner,
\t\t\t\t\t\t\tnative_status,
\t\t\t\t\t\t\tdelegated_extend,
\t\t\t\t\t\t\tmapping,
\t\t\t\t\t\t\tobserved_attachment);
\t\t\t\t\tauto receipt = sqlite_shm_verified_reader_attachment_post_map_receipt{
\t\t\t\t\t\tstd::move(request),
\t\t\t\t\t\tmap->generation,
\t\t\t\t\t\tmapping,
\t\t\t\t\t\tstd::move(observed_attachment),
\t\t\t\t\t\tstd::move(effect_identity),
\t\t\t\t\t\tstd::move(qualified_control)};
\t\t\t\t\tstatic_assert(std::is_nothrow_move_constructible_v<
\t\t\t\t\t\tsqlite_shm_verified_reader_attachment_post_map_receipt>);
\t\t\t\t\tauto output = sqlite_shm_lease_result<
\t\t\t\t\t\tsqlite_shm_verified_reader_attachment_post_map_receipt>{
\t\t\t\t\t\tstd::move(receipt)};
\t\t\t\t\tstatic_assert(std::is_nothrow_move_constructible_v<decltype(output)>);
\t\t\t\t\tif (!output->qualified_control_ ||
\t\t\t\t\t\t!output->qualified_control_->capability.matches_live_owner(exact_owner_base) ||
\t\t\t\t\t\texact_owner->disposition.load(std::memory_order_acquire) !=
\t\t\t\t\t\t\tsqlite_shm_reader_map_identity_disposition::live)
\t\t\t\t\t{
\t\t\t\t\t\texact_owner->mapped_effect_validation_phase.store(
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::poisoned,
\t\t\t\t\t\t\tstd::memory_order_release);
\t\t\t\t\t\tmap->quarantine_reason =
\t\t\t\t\t\t\tsqlite_shm_reader_terminal_quarantine_reason::owner_abandoned;
\t\t\t\t\t\tquarantine_reader_map_terminal_commit_locked(map->token, map->session_token);
\t\t\t\t\t\treturn sqlite_shm_unexpected(ambiguous());
\t\t\t\t\t}
\t\t\t\t\texact_owner->mapped_effect_validation_phase.store(
\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::sealed,
\t\t\t\t\t\tstd::memory_order_release);
\t\t\t\t\treturn output;
\t\t\t\t}
\t\t\t\tcatch (...)
\t\t\t\t{
\t\t\t\t\tif (observation_burned && exact_owner)
\t\t\t\t\t{
\t\t\t\t\t\texact_owner->mapped_effect_validation_phase.store(
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::poisoned,
\t\t\t\t\t\t\tstd::memory_order_release);
\t\t\t\t\t\texact_owner->abandon();
\t\t\t\t\t}
\t\t\t\t\treturn sqlite_shm_unexpected(ambiguous());
\t\t\t\t}
\t\t\t}

'''
    insert_at = (
        "\t\t\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_zero_effect_receipt>\n"
        "\t\t\tvalidate_reader_zero_attachment_effect("
    )
    t = insert_before(t, insert_at, mapped, "lease mapped validation method")

start, end, block = find_balanced_block(
    t,
    "\t\t\t[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_map_commit> commit_reader(",
)
if "mapped_effect_validation_phase" not in block:
    anchor = (
        "\t\t\t\t\tconst auto qualified_owned_terminal = map_attempt->qualified_identity_bound;\n"
        "\t\t\t\t\tauto prepared_terminal_receipt = receipt;"
    )
    replacement = '''\t\t\t\t\tconst auto qualified_owned_terminal = map_attempt->qualified_identity_bound;
\t\t\t\t\tauto prepared_terminal_receipt = receipt;
\t\t\t\t\tauto qualified_control = prepared_terminal_receipt.qualified_control_;
\t\t\t\t\tconst auto exact_qualified_control = qualified_owned_terminal &&
\t\t\t\t\t\tmap_attempt->identity_owner_control && qualified_control &&
\t\t\t\t\t\tqualified_control->owner.lock().get() ==
\t\t\t\t\t\t\tmap_attempt->identity_owner_control.get() &&
\t\t\t\t\t\tqualified_control->native_status ==
\t\t\t\t\t\t\tstatic_cast<int>(sqlite_native_map_status::ok) &&
\t\t\t\t\t\tqualified_control->delegated_extend == 0 &&
\t\t\t\t\t\tqualified_control->mapping == prepared_terminal_receipt.mapping() &&
\t\t\t\t\t\tqualified_control->observed_attachment ==
\t\t\t\t\t\t\tprepared_terminal_receipt.observed_attachment() &&
\t\t\t\t\t\tqualified_control->capability.matches_effect_identity(
\t\t\t\t\t\t\tprepared_terminal_receipt.zero_resize_effect_receipt()) &&
\t\t\t\t\t\tmap_attempt->identity_owner_control->mapped_effect_validation_phase.load(
\t\t\t\t\t\t\tstd::memory_order_acquire) ==
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::sealed;
\t\t\t\t\tif ((qualified_owned_terminal && !exact_qualified_control) ||
\t\t\t\t\t\t(!qualified_owned_terminal && qualified_control))
\t\t\t\t\t{
\t\t\t\t\t\tif (map_attempt->identity_owner_control)
\t\t\t\t\t\t\tmap_attempt->identity_owner_control->mapped_effect_validation_phase.store(
\t\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::poisoned,
\t\t\t\t\t\t\t\tstd::memory_order_release);
\t\t\t\t\t\tmap_attempt->quarantine_reason =
\t\t\t\t\t\t\tsqlite_shm_reader_terminal_quarantine_reason::presented_invalid;
\t\t\t\t\t\tquarantine_reader_map_terminal_commit_locked(
\t\t\t\t\t\t\tmap_attempt->token, owner->token);
\t\t\t\t\t\tinflight.disarm();
\t\t\t\t\t\tsession.disarm();
\t\t\t\t\t\treturn sqlite_shm_unexpected(ambiguous());
\t\t\t\t\t}
\t\t\t\t\tif (qualified_owned_terminal)
\t\t\t\t\t{
\t\t\t\t\t\tconst std::shared_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>
\t\t\t\t\t\t\texact_owner_base = map_attempt->identity_owner_control;
\t\t\t\t\t\tif (!qualified_control->capability.matches_live_owner(exact_owner_base))
\t\t\t\t\t\t{
\t\t\t\t\t\t\tmap_attempt->identity_owner_control->mapped_effect_validation_phase.store(
\t\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::poisoned,
\t\t\t\t\t\t\t\tstd::memory_order_release);
\t\t\t\t\t\t\tmap_attempt->quarantine_reason =
\t\t\t\t\t\t\t\tsqlite_shm_reader_terminal_quarantine_reason::owner_abandoned;
\t\t\t\t\t\t\tquarantine_reader_map_terminal_commit_locked(
\t\t\t\t\t\t\t\tmap_attempt->token, owner->token);
\t\t\t\t\t\t\tinflight.disarm();
\t\t\t\t\t\t\tsession.disarm();
\t\t\t\t\t\t\treturn sqlite_shm_unexpected(ambiguous());
\t\t\t\t\t\t}
\t\t\t\t\t}'''
    if anchor not in block:
        raise RuntimeError("commit qualified anchor missing")
    block = block.replace(anchor, replacement, 1)

    anchor = (
        "\t\t\t\t\tauto committed_state = shared_from_this();\n"
        "\t\t\t\t\tif (!claim_reader_map_identity_terminalization_locked(*map_attempt))"
    )
    replacement = '''\t\t\t\t\tauto committed_state = shared_from_this();
\t\t\t\t\tif (qualified_owned_terminal)
\t\t\t\t\t{
\t\t\t\t\t\tauto expected_validation_phase =
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::sealed;
\t\t\t\t\t\tif (!qualified_control ||
\t\t\t\t\t\t\t!map_attempt->identity_owner_control->mapped_effect_validation_phase
\t\t\t\t\t\t\t\t .compare_exchange_strong(
\t\t\t\t\t\t\t\t\texpected_validation_phase,
\t\t\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::terminal_consumed,
\t\t\t\t\t\t\t\t\tstd::memory_order_acq_rel,
\t\t\t\t\t\t\t\t\tstd::memory_order_acquire))
\t\t\t\t\t\t{
\t\t\t\t\t\t\tmap_attempt->identity_owner_control->mapped_effect_validation_phase.store(
\t\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::poisoned,
\t\t\t\t\t\t\t\tstd::memory_order_release);
\t\t\t\t\t\t\treturn quarantine_terminal(
\t\t\t\t\t\t\t\tsqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
\t\t\t\t\t\t}
\t\t\t\t\t}
\t\t\t\t\tif (!claim_reader_map_identity_terminalization_locked(*map_attempt))'''
    if anchor not in block:
        raise RuntimeError("commit terminalization anchor missing")
    block = block.replace(anchor, replacement, 1)
    t = t[:start] + block + t[end:]

write(rel, t)
