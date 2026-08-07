#!/usr/bin/env python3
"""Apply the bounded U2a1c owner-qualified mapped-result validator patch.

The transformation is intentionally exact and idempotent.  It fails before writing when the
expected U2a1b source shape is not present, so the workbench cannot silently patch a moved base.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def write(rel: str, text: str) -> None:
    (ROOT / rel).write_text(text, encoding="utf-8")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one exact marker, found {count}")
    return text.replace(old, new, 1)


def insert_before_once(text: str, marker: str, insertion: str, label: str) -> str:
    if insertion in text:
        return text
    count = text.count(marker)
    if count != 1:
        raise RuntimeError(f"{label}: expected one marker, found {count}")
    return text.replace(marker, insertion + marker, 1)


def edit_class(text: str, class_name: str, transform) -> str:
    marker = f"\tclass {class_name}"
    start = text.find(marker)
    if start < 0:
        raise RuntimeError(f"class not found: {class_name}")
    end = text.find("\n\t};", start)
    if end < 0:
        raise RuntimeError(f"class terminator not found: {class_name}")
    end += len("\n\t};")
    segment = text[start:end]
    changed = transform(segment)
    if changed == segment:
        return text
    return text[:start] + changed + text[end:]


def edit_function_region(text: str, start_marker: str, end_marker: str, transform, label: str) -> str:
    start = text.find(start_marker)
    if start < 0:
        raise RuntimeError(f"{label}: start marker not found")
    end = text.find(end_marker, start)
    if end < 0:
        raise RuntimeError(f"{label}: end marker not found")
    segment = text[start:end]
    changed = transform(segment)
    return text[:start] + changed + text[end:]


# ---------------------------------------------------------------------------
# Identity issuer: an independent mapped-effect capability mirrors U2a1b's
# zero-effect capability while retaining its exact role and private record.
# ---------------------------------------------------------------------------

rel = "src/sdk/sqlite_same_process_shm_identity_issuer_internal.hpp"
text = read(rel)
mapped_capability_decl = r'''
	/**
	 * Move-only, non-projectable issuer bridge for one still-live mapped-result proof.
	 *
	 * The private issuer record and exact qualified owner remain live through validation.  The
	 * terminal coordinator consumes the proof exactly once; dropping either the original presenter
	 * or the validator receipt remains fail-closed.
	 */
	class sqlite_shm_reader_mapped_effect_identity_validation_capability final
	{
	  public:
		sqlite_shm_reader_mapped_effect_identity_validation_capability(
			sqlite_shm_reader_mapped_effect_identity_validation_capability&&) noexcept = default;
		sqlite_shm_reader_mapped_effect_identity_validation_capability& operator=(
			sqlite_shm_reader_mapped_effect_identity_validation_capability&&) = delete;
		sqlite_shm_reader_mapped_effect_identity_validation_capability(
			const sqlite_shm_reader_mapped_effect_identity_validation_capability&) = delete;
		sqlite_shm_reader_mapped_effect_identity_validation_capability& operator=(
			const sqlite_shm_reader_mapped_effect_identity_validation_capability&) = delete;

	  private:
		friend class detail::sqlite_shm_process_identity_issuer_state;
		friend class detail::sqlite_shm_mapping_lease_state;

		sqlite_shm_reader_mapped_effect_identity_validation_capability(
			std::shared_ptr<detail::sqlite_shm_process_identity_record_control> effect,
			std::weak_ptr<detail::sqlite_shm_reader_lifecycle_owner_abandonment_control>
				owner) noexcept;
		[[nodiscard]] bool matches_live_owner(
			const std::shared_ptr<detail::sqlite_shm_reader_lifecycle_owner_abandonment_control>&
				owner) const noexcept;
		[[nodiscard]] bool
		matches_effect_identity(const sqlite_backend_opaque_identity& identity) const noexcept;
		[[nodiscard]] sqlite_backend_opaque_identity copy_effect_identity() const;

		std::shared_ptr<detail::sqlite_shm_process_identity_record_control> effect_;
		std::weak_ptr<detail::sqlite_shm_reader_lifecycle_owner_abandonment_control> owner_;
	};

'''
text = insert_before_once(
    text,
    "\t/**\n\t * Move-only, non-projectable issuer bridge for one still-live map zero-effect proof.",
    mapped_capability_decl,
    "identity header mapped capability",
)
write(rel, text)

rel = "src/sdk/sqlite_same_process_shm_identity_issuer_internal.cpp"
text = read(rel)
state_mapped_validator = r'''
			[[nodiscard]] sqlite_shm_lease_result<
				sqlite_shm_reader_mapped_effect_identity_validation_capability>
			validate_mapped_effect_identity_for_registry(
				const sqlite_shm_reader_lifecycle_identity_scope& scope,
				const sqlite_shm_issued_reader_callback_identity& callback,
				const sqlite_shm_issued_reader_effect_identity& effect,
				const std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
					expected_owner) const noexcept
			{
				if (!current_before_owner_lock())
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				const auto expected = expected_owner.lock();
				const auto actual = scope.control_ ? scope.control_->owner_abandonment.lock() : nullptr;
				if (!expected || !actual || actual.get() != expected.get())
					return reject(sqlite_shm_lease_rejection_reason::receipt_mismatch);
				const auto callback_validated = validate_callback(
					scope, callback, sqlite_shm_reader_callback_identity_role::map);
				if (!callback_validated)
					return callback_validated.error();
				const auto effect_validated = validate_effect(scope,
					callback,
					effect,
					sqlite_shm_reader_effect_identity_role::mapped_result);
				if (!effect_validated)
					return effect_validated.error();
				return sqlite_shm_reader_mapped_effect_identity_validation_capability{
					effect.control_, actual};
			}

'''
zero_state_marker = "\t\t\t[[nodiscard]] sqlite_shm_lease_result<\n\t\t\t\tsqlite_shm_reader_zero_effect_identity_validation_capability>\n\t\t\tvalidate_zero_effect_identity_for_registry("
text = insert_before_once(text, zero_state_marker, state_mapped_validator, "issuer mapped validator")

state_mapped_current = r'''
			[[nodiscard]] bool mapped_effect_capability_is_current(
				const std::shared_ptr<sqlite_shm_process_identity_record_control>& effect,
				const std::shared_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
					expected_owner) const noexcept
			{
				if (!effect || !expected_owner || !scope_matches_control(effect->scope) ||
					effect->issuer.lock().get() != this ||
					effect->process_epoch.get() != process_epoch_.get() ||
					effect->expected_process_epoch != expected_process_epoch_ ||
					effect->domain !=
						sqlite_shm_reader_lifecycle_identity_domain::native_or_zero_effect ||
					effect->role != static_cast<std::uint8_t>(
						sqlite_shm_reader_effect_identity_role::mapped_result) ||
					effect->phase.load(std::memory_order_acquire) !=
						sqlite_shm_process_identity_record_phase::sealed ||
					!effect->scope->enforce_owner_phase || !effect->scope->owner_phase ||
					effect->scope->owner_phase->load(std::memory_order_acquire) !=
						sqlite_shm_reader_lifecycle_owner_phase::owned)
					return false;
				const auto actual_owner = effect->scope->owner_abandonment.lock();
				const auto parent = effect->parent_callback.lock();
				return actual_owner && actual_owner.get() == expected_owner.get() && parent &&
					parent->issuer.lock().get() == this && parent->scope.get() == effect->scope.get() &&
					parent->domain ==
						sqlite_shm_reader_lifecycle_identity_domain::callback_invocation &&
					parent->role == static_cast<std::uint8_t>(
						sqlite_shm_reader_callback_identity_role::map) &&
					parent->phase.load(std::memory_order_acquire) ==
						sqlite_shm_process_identity_record_phase::sealed;
			}

'''
text = insert_before_once(
    text,
    "\t\t\t[[nodiscard]] bool zero_effect_capability_is_current(",
    state_mapped_current,
    "issuer mapped currentness",
)
text = replace_once(
    text,
    "\t\t\tfriend class ::cxxlens::sdk::\n\t\t\t\tsqlite_shm_reader_zero_effect_identity_validation_capability;",
    "\t\t\tfriend class ::cxxlens::sdk::\n\t\t\t\tsqlite_shm_reader_mapped_effect_identity_validation_capability;\n\t\t\tfriend class ::cxxlens::sdk::\n\t\t\t\tsqlite_shm_reader_zero_effect_identity_validation_capability;",
    "issuer mapped friend",
)

bridge_mapped = r'''
		sqlite_shm_lease_result<sqlite_shm_reader_mapped_effect_identity_validation_capability>
		validate_mapped_effect_identity_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_issued_reader_callback_identity& callback,
			const sqlite_shm_issued_reader_effect_identity& effect,
			const std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
				expected_owner) noexcept
		{
			return state
				? state->validate_mapped_effect_identity_for_registry(
					  scope, callback, effect, expected_owner)
				: sqlite_shm_lease_result<
					  sqlite_shm_reader_mapped_effect_identity_validation_capability>{
					  reject(sqlite_shm_lease_rejection_reason::stale_token)};
		}

'''
zero_bridge_marker = "\t\tsqlite_shm_lease_result<sqlite_shm_reader_zero_effect_identity_validation_capability>\n\t\tvalidate_zero_effect_identity_for_registry("
text = insert_before_once(text, zero_bridge_marker, bridge_mapped, "issuer mapped bridge")

mapped_capability_impl = r'''
	sqlite_shm_reader_mapped_effect_identity_validation_capability::
		sqlite_shm_reader_mapped_effect_identity_validation_capability(
			std::shared_ptr<detail::sqlite_shm_process_identity_record_control> effect,
			std::weak_ptr<detail::sqlite_shm_reader_lifecycle_owner_abandonment_control>
				owner) noexcept
		: effect_{std::move(effect)}, owner_{std::move(owner)}
	{
	}

	bool sqlite_shm_reader_mapped_effect_identity_validation_capability::matches_live_owner(
		const std::shared_ptr<detail::sqlite_shm_reader_lifecycle_owner_abandonment_control>&
			owner) const noexcept
	{
		if (!effect_ || !effect_->process_epoch ||
			effect_->process_epoch->load(std::memory_order_acquire) !=
				effect_->expected_process_epoch)
			return false;
		const auto expected = owner_.lock();
		const auto issuer = effect_->issuer.lock();
		return expected && owner && expected.get() == owner.get() && issuer &&
			issuer->mapped_effect_capability_is_current(effect_, owner);
	}

	bool sqlite_shm_reader_mapped_effect_identity_validation_capability::matches_effect_identity(
		const sqlite_backend_opaque_identity& identity) const noexcept
	{
		return effect_ && effect_->projection == identity;
	}

	sqlite_backend_opaque_identity
	sqlite_shm_reader_mapped_effect_identity_validation_capability::copy_effect_identity() const
	{
		return effect_ ? effect_->projection : sqlite_backend_opaque_identity{};
	}

'''
text = insert_before_once(
    text,
    "\tsqlite_shm_reader_zero_effect_identity_validation_capability::\n\t\tsqlite_shm_reader_zero_effect_identity_validation_capability(",
    mapped_capability_impl,
    "mapped capability implementation",
)
write(rel, text)

# ---------------------------------------------------------------------------
# Lease types and closed validator surface.
# ---------------------------------------------------------------------------

rel = "src/sdk/sqlite_same_process_shm_mapping_lease_internal.hpp"
text = read(rel)
text = replace_once(
    text,
    "\t\tstruct sqlite_shm_reader_zero_effect_receipt_control;",
    "\t\tstruct sqlite_shm_reader_mapped_effect_receipt_control;\n\t\tstruct sqlite_shm_reader_zero_effect_receipt_control;",
    "lease mapped control forward",
)
text = replace_once(
    text,
    "\tclass sqlite_shm_reader_zero_effect_identity_validation_capability;",
    "\tclass sqlite_shm_reader_mapped_effect_identity_validation_capability;\n\tclass sqlite_shm_reader_zero_effect_identity_validation_capability;",
    "lease mapped capability forward",
)


def native_attachment_friends(segment: str) -> str:
    return replace_once(
        segment,
        "\t  private:\n\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;",
        "\t  private:\n\t\tfriend class detail::sqlite_shm_mapping_lease_state;\n\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;",
        "native attachment lease friend",
    )

text = edit_class(text, "sqlite_shm_reader_native_attachment_identity", native_attachment_friends)


def mapped_receipt_edit(segment: str) -> str:
    segment = replace_once(
        segment,
        "\t  private:\n\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;",
        "\t  private:\n\t\tfriend class detail::sqlite_shm_mapping_lease_state;\n\t\tfriend class detail::sqlite_shm_mapping_registry_state;\n\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;",
        "mapped receipt friends",
    )
    ctor = r'''
		sqlite_shm_verified_reader_attachment_post_map_receipt(
			const sqlite_shm_reader_attachment_map_inflight& inflight,
			sqlite_shm_reader_attachment_map_request request,
			std::uint64_t generation,
			sqlite_shm_mapping_tuple mapping,
			sqlite_shm_reader_native_attachment_identity observed_attachment,
			sqlite_backend_opaque_identity zero_resize_effect_receipt,
			std::shared_ptr<detail::sqlite_shm_reader_mapped_effect_receipt_control>
				qualified_control);
'''
    marker = "\t\tsqlite_shm_reader_attachment_map_request request_;"
    segment = insert_before_once(segment, marker, ctor + "\n\t\tstd::weak_ptr<detail::sqlite_shm_mapping_lease_state> state_;\n\t\tstd::uint64_t token_{};\n", "mapped receipt qualified ctor")
    segment = replace_once(
        segment,
        "\t\tsqlite_backend_opaque_identity zero_resize_effect_receipt_;",
        "\t\tsqlite_backend_opaque_identity zero_resize_effect_receipt_;\n\t\tstd::shared_ptr<detail::sqlite_shm_reader_mapped_effect_receipt_control>\n\t\t\tqualified_control_;",
        "mapped receipt control field",
    )
    return segment

text = edit_class(text, "sqlite_shm_verified_reader_attachment_post_map_receipt", mapped_receipt_edit)

mapped_validator_decl = r'''
	/**
	 * Closed validator for one exact owner-qualified mapped reader xShmMap result.
	 *
	 * Only exact native SQLITE_OK/non-null with delegated extend zero can be sealed.  The request,
	 * generation and mapping tuple are derived from the already-bound owner.  Direct SHM object,
	 * entry, device and mount observations are retained exactly in private one-shot provenance.
	 */
	class sqlite_same_process_shm_reader_receipt_validator final
	{
	  public:
		sqlite_same_process_shm_reader_receipt_validator() = delete;

		[[nodiscard]] static
		sqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
		validate(sqlite_same_process_shm_mapping_registry& registry,
			 sqlite_shm_registry_family_pin& family,
			 const sqlite_shm_reader_attachment_map_inflight& inflight,
			 const sqlite_shm_reader_lifecycle_identity_scope& scope,
			 const sqlite_shm_issued_reader_callback_identity& callback,
			 const sqlite_shm_issued_reader_effect_identity& effect,
			 int native_status,
			 const volatile void* native_mapping,
			 int delegated_extend,
			 sqlite_backend_opaque_identity observed_shm_object_receipt,
			 sqlite_backend_opaque_identity observed_shm_entry_receipt,
			 sqlite_backend_opaque_identity observed_device_receipt,
			 sqlite_backend_opaque_identity observed_mount_receipt) noexcept;
	};

'''
text = insert_before_once(
    text,
    "\t/**\n\t * Closed validator for one exact qualified reader-map zero-attachment native result.",
    mapped_validator_decl,
    "mapped validator declaration",
)


def inflight_friend(segment: str) -> str:
    return replace_once(
        segment,
        "\t\tfriend class sqlite_shm_verified_reader_attachment_zero_effect_receipt;",
        "\t\tfriend class sqlite_shm_verified_reader_attachment_post_map_receipt;\n\t\tfriend class sqlite_shm_verified_reader_attachment_zero_effect_receipt;",
        "inflight mapped receipt friend",
    )

text = edit_class(text, "sqlite_shm_reader_attachment_map_inflight", inflight_friend)


def lease_state_decl(segment: str) -> str:
    mapped = r'''
		[[nodiscard]]
		sqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
		validate_registry_reader_mapped_attachment_effect(
			const sqlite_shm_registry_family_pin& family,
			const sqlite_shm_reader_attachment_map_inflight& inflight,
			sqlite_shm_reader_mapped_effect_identity_validation_capability capability,
			int native_status,
			const volatile void* native_mapping,
			int delegated_extend,
			sqlite_backend_opaque_identity observed_shm_object_receipt,
			sqlite_backend_opaque_identity observed_shm_entry_receipt,
			sqlite_backend_opaque_identity observed_device_receipt,
			sqlite_backend_opaque_identity observed_mount_receipt) noexcept;
'''
    return insert_before_once(
        segment,
        "\t\t[[nodiscard]]\n\t\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_zero_effect_receipt>\n\t\tvalidate_registry_reader_zero_attachment_effect(",
        mapped + "\n",
        "lease state mapped validator declaration",
    )

text = edit_class(text, "sqlite_shm_mapping_lease_state", lease_state_decl)
write(rel, text)

# ---------------------------------------------------------------------------
# Lease implementation: exact mapped observation control, validation and
# terminal consumption.
# ---------------------------------------------------------------------------

rel = "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp"
text = read(rel)

mapped_receipt_ctor = r'''
	sqlite_shm_verified_reader_attachment_post_map_receipt::
		sqlite_shm_verified_reader_attachment_post_map_receipt(
			const sqlite_shm_reader_attachment_map_inflight& inflight,
			sqlite_shm_reader_attachment_map_request request,
			const std::uint64_t generation,
			sqlite_shm_mapping_tuple mapping,
			sqlite_shm_reader_native_attachment_identity observed_attachment,
			sqlite_backend_opaque_identity zero_resize_effect_receipt,
			std::shared_ptr<detail::sqlite_shm_reader_mapped_effect_receipt_control>
				qualified_control)
		: state_{inflight.state_}, token_{inflight.token_}, request_{std::move(request)},
		  generation_{generation}, mapping_{mapping},
		  observed_attachment_{std::move(observed_attachment)},
		  zero_resize_effect_receipt_{std::move(zero_resize_effect_receipt)},
		  qualified_control_{std::move(qualified_control)}
	{
	}

'''
text = insert_before_once(
    text,
    "\tconst sqlite_shm_reader_attachment_map_request&\n\tsqlite_shm_verified_reader_attachment_post_map_receipt::request() const noexcept",
    mapped_receipt_ctor,
    "mapped receipt qualified constructor implementation",
)

mapped_validator_wrapper = r'''
	sqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
	sqlite_same_process_shm_reader_receipt_validator::validate(
		sqlite_same_process_shm_mapping_registry& registry,
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_reader_lifecycle_identity_scope& scope,
		const sqlite_shm_issued_reader_callback_identity& callback,
		const sqlite_shm_issued_reader_effect_identity& effect,
		const int native_status,
		const volatile void* native_mapping,
		const int delegated_extend,
		sqlite_backend_opaque_identity observed_shm_object_receipt,
		sqlite_backend_opaque_identity observed_shm_entry_receipt,
		sqlite_backend_opaque_identity observed_device_receipt,
		sqlite_backend_opaque_identity observed_mount_receipt) noexcept
	{
		return registry.validate_reader_mapped_attachment_effect(family,
			inflight,
			scope,
			callback,
			effect,
			native_status,
			native_mapping,
			delegated_extend,
			std::move(observed_shm_object_receipt),
			std::move(observed_shm_entry_receipt),
			std::move(observed_device_receipt),
			std::move(observed_mount_receipt));
	}

'''
text = insert_before_once(
    text,
    "\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_zero_effect_receipt>\n\tsqlite_same_process_shm_reader_zero_effect_receipt_validator::validate(",
    mapped_validator_wrapper,
    "mapped validator wrapper",
)

mapped_phase = r'''
		enum class sqlite_shm_reader_mapped_effect_validation_phase : std::uint8_t
		{
			unclaimed,
			validating,
			sealed,
			terminal_consumed,
			poisoned,
		};

		static_assert(
			std::atomic<sqlite_shm_reader_mapped_effect_validation_phase>::is_always_lock_free);

'''
text = insert_before_once(
    text,
    "\t\tenum class sqlite_shm_reader_zero_effect_validation_phase : std::uint8_t",
    mapped_phase,
    "mapped validation phase",
)
text = replace_once(
    text,
    "\t\t\tstd::atomic_bool identity_scope_claimed{false};\n\t\t\tstd::atomic<sqlite_shm_reader_zero_effect_validation_phase>",
    "\t\t\tstd::atomic_bool identity_scope_claimed{false};\n\t\t\tstd::atomic<sqlite_shm_reader_mapped_effect_validation_phase>\n\t\t\t\tmapped_effect_validation_phase{\n\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::unclaimed};\n\t\t\tstd::atomic<sqlite_shm_reader_zero_effect_validation_phase>",
    "mapped phase owner field",
)

mapped_control = r'''
		struct sqlite_shm_reader_mapped_effect_receipt_control final
		{
			sqlite_shm_reader_mapped_effect_receipt_control(
				sqlite_shm_reader_mapped_effect_identity_validation_capability capability_value,
				const std::shared_ptr<sqlite_shm_reader_map_identity_owner_control>& owner_value,
				sqlite_shm_mapping_tuple mapping_value,
				sqlite_shm_reader_native_attachment_identity observed_attachment_value)
				: capability{std::move(capability_value)}, owner{owner_value},
				  process_epoch{owner_value ? owner_value->process_epoch : nullptr},
				  expected_process_epoch{owner_value ? owner_value->expected_process_epoch : 0U},
				  mapping{mapping_value},
				  observed_attachment{std::move(observed_attachment_value)}
			{
			}
			~sqlite_shm_reader_mapped_effect_receipt_control() noexcept;

			sqlite_shm_reader_mapped_effect_identity_validation_capability capability;
			std::weak_ptr<sqlite_shm_reader_map_identity_owner_control> owner;
			std::shared_ptr<std::atomic<std::uint64_t>> process_epoch;
			std::uint64_t expected_process_epoch{};
			sqlite_shm_mapping_tuple mapping;
			sqlite_shm_reader_native_attachment_identity observed_attachment;
		};

'''
text = insert_before_once(
    text,
    "\t\tstruct sqlite_shm_reader_zero_effect_receipt_control final",
    mapped_control,
    "mapped receipt control",
)

mapped_state_validator = r'''
			[[nodiscard]]
			sqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
			validate_reader_mapped_attachment_effect(
				const sqlite_shm_registry_family_pin& registry_family,
				const sqlite_shm_reader_attachment_map_inflight& inflight,
				sqlite_shm_reader_mapped_effect_identity_validation_capability capability,
				const int native_status,
				const volatile void* native_mapping,
				const int delegated_extend,
				sqlite_backend_opaque_identity observed_shm_object_receipt,
				sqlite_backend_opaque_identity observed_shm_entry_receipt,
				sqlite_backend_opaque_identity observed_device_receipt,
				sqlite_backend_opaque_identity observed_mount_receipt) noexcept
			{
				std::shared_ptr<sqlite_shm_reader_map_identity_owner_control> exact_owner;
				bool observation_burned{};
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(inflight.state_, inflight.token_))
						return sqlite_shm_unexpected(stale_token(
							sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto map = find_by_token(reader_attachment_maps_, inflight.token_);
					if (map == reader_attachment_maps_.end() ||
						map->phase != reader_phase::inflight || !map->registry_bound ||
						!map->qualified_identity_bound || !map->identity_owner_control ||
						map->identity_owner_control.get() != inflight.qualified_owner_control_.get() ||
						map->qualified_owner_phase.get() != inflight.qualified_owner_phase_.get() ||
						map->generation != inflight.generation_ ||
						(map->registry_predelegate_authority &&
						 !map->registry_predelegate_authority->retains_exact_owned_terminal_lifetimes(
							 registry_family, map->request)))
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::receipt_mismatch,
							sqlite_shm_lease_recovery_action::quarantine_no_retry));

					exact_owner = map->identity_owner_control;
					const std::shared_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>
						exact_owner_base = exact_owner;
					if (!capability.matches_live_owner(exact_owner_base) ||
						exact_owner->disposition.load(std::memory_order_acquire) !=
							sqlite_shm_reader_map_identity_disposition::live)
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::receipt_mismatch,
							sqlite_shm_lease_recovery_action::quarantine_no_retry));

					auto expected_phase =
						sqlite_shm_reader_mapped_effect_validation_phase::unclaimed;
					if (!exact_owner->mapped_effect_validation_phase.compare_exchange_strong(
							expected_phase,
							sqlite_shm_reader_mapped_effect_validation_phase::validating,
							std::memory_order_acq_rel,
							std::memory_order_acquire))
					{
						if (expected_phase ==
							sqlite_shm_reader_mapped_effect_validation_phase::terminal_consumed)
							return sqlite_shm_unexpected(stale_token(
								sqlite_shm_lease_recovery_action::quarantine_no_retry));
						exact_owner->mapped_effect_validation_phase.store(
							sqlite_shm_reader_mapped_effect_validation_phase::poisoned,
							std::memory_order_release);
						map->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid;
						quarantine_reader_map_terminal_commit_locked(map->token, map->session_token);
						return sqlite_shm_unexpected(ambiguous());
					}
					observation_burned = true;

					const auto exact_native_shape =
						native_status == static_cast<int>(sqlite_native_map_status::ok) &&
						native_mapping != nullptr && delegated_extend == 0 &&
						native_mapping == map->expected_mapping.native_mapping &&
						valid_identity(observed_shm_object_receipt) &&
						valid_identity(observed_shm_entry_receipt) &&
						valid_identity(observed_device_receipt) &&
						valid_identity(observed_mount_receipt);
					if (!exact_native_shape)
					{
						exact_owner->mapped_effect_validation_phase.store(
							sqlite_shm_reader_mapped_effect_validation_phase::poisoned,
							std::memory_order_release);
						map->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid;
						quarantine_reader_map_terminal_commit_locked(map->token, map->session_token);
						return sqlite_shm_unexpected(ambiguous());
					}

					auto request = map->request;
					auto mapping = map->expected_mapping;
					auto observed = sqlite_shm_reader_native_attachment_identity{
						request.expected_attachment,
						std::move(observed_shm_object_receipt),
						std::move(observed_shm_entry_receipt),
						std::move(observed_device_receipt),
						std::move(observed_mount_receipt)};
					auto effect_identity = capability.copy_effect_identity();
					if (!capability.matches_live_owner(exact_owner_base) ||
						exact_owner->disposition.load(std::memory_order_acquire) !=
							sqlite_shm_reader_map_identity_disposition::live)
					{
						exact_owner->mapped_effect_validation_phase.store(
							sqlite_shm_reader_mapped_effect_validation_phase::poisoned,
							std::memory_order_release);
						map->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::owner_abandoned;
						quarantine_reader_map_terminal_commit_locked(map->token, map->session_token);
						return sqlite_shm_unexpected(ambiguous());
					}
					auto qualified_control =
						std::make_shared<sqlite_shm_reader_mapped_effect_receipt_control>(
							std::move(capability), exact_owner, mapping, observed);
					auto receipt = sqlite_shm_verified_reader_attachment_post_map_receipt{
						inflight,
						std::move(request),
						map->generation,
						mapping,
						std::move(observed),
						std::move(effect_identity),
						std::move(qualified_control)};
					static_assert(std::is_nothrow_move_constructible_v<
						sqlite_shm_verified_reader_attachment_post_map_receipt>);
					auto output = sqlite_shm_lease_result<
						sqlite_shm_verified_reader_attachment_post_map_receipt>{
						std::move(receipt)};
					static_assert(std::is_nothrow_move_constructible_v<decltype(output)>);
					if (!output->qualified_control_ ||
						!output->qualified_control_->capability.matches_live_owner(exact_owner_base) ||
						exact_owner->disposition.load(std::memory_order_acquire) !=
							sqlite_shm_reader_map_identity_disposition::live)
					{
						exact_owner->mapped_effect_validation_phase.store(
							sqlite_shm_reader_mapped_effect_validation_phase::poisoned,
							std::memory_order_release);
						map->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::owner_abandoned;
						quarantine_reader_map_terminal_commit_locked(map->token, map->session_token);
						return sqlite_shm_unexpected(ambiguous());
					}
					exact_owner->mapped_effect_validation_phase.store(
						sqlite_shm_reader_mapped_effect_validation_phase::sealed,
						std::memory_order_release);
					return output;
				}
				catch (...)
				{
					if (observation_burned && exact_owner)
					{
						exact_owner->mapped_effect_validation_phase.store(
							sqlite_shm_reader_mapped_effect_validation_phase::poisoned,
							std::memory_order_release);
						exact_owner->abandon();
					}
					return sqlite_shm_unexpected(ambiguous());
				}
			}

'''
text = insert_before_once(
    text,
    "\t\t\t[[nodiscard]]\n\t\t\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_zero_effect_receipt>\n\t\t\tvalidate_reader_zero_attachment_effect(",
    mapped_state_validator,
    "lease mapped state validator",
)


def patch_commit_reader(segment: str) -> str:
    validation = r'''
					const auto mapped_receipt_state = prepared_terminal_receipt.state_.lock();
					auto qualified_control = prepared_terminal_receipt.qualified_control_;
					const auto exact_qualified_control = qualified_owned_terminal &&
						mapped_receipt_state.get() == this &&
						prepared_terminal_receipt.token_ == map_attempt->token &&
						map_attempt->identity_owner_control && qualified_control &&
						qualified_control->owner.lock().get() ==
							map_attempt->identity_owner_control.get() &&
						qualified_control->mapping == prepared_terminal_receipt.mapping() &&
						qualified_control->observed_attachment ==
							prepared_terminal_receipt.observed_attachment() &&
						qualified_control->capability.matches_effect_identity(
							prepared_terminal_receipt.zero_resize_effect_receipt()) &&
						map_attempt->identity_owner_control->mapped_effect_validation_phase.load(
							std::memory_order_acquire) ==
							sqlite_shm_reader_mapped_effect_validation_phase::sealed;
					if ((qualified_owned_terminal && !exact_qualified_control) ||
						(!qualified_owned_terminal &&
						 (qualified_control || prepared_terminal_receipt.token_ != 0U ||
						  mapped_receipt_state)))
					{
						if (map_attempt->identity_owner_control)
							map_attempt->identity_owner_control->mapped_effect_validation_phase.store(
								sqlite_shm_reader_mapped_effect_validation_phase::poisoned,
								std::memory_order_release);
						return quarantine_terminal(
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
					}
					if (qualified_owned_terminal)
					{
						const std::shared_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>
							exact_owner_base = map_attempt->identity_owner_control;
						if (!qualified_control->capability.matches_live_owner(exact_owner_base))
						{
							map_attempt->identity_owner_control->mapped_effect_validation_phase.store(
								sqlite_shm_reader_mapped_effect_validation_phase::poisoned,
								std::memory_order_release);
							return quarantine_terminal(
								sqlite_shm_reader_terminal_quarantine_reason::owner_abandoned);
						}
					}
'''
    segment = replace_once(
        segment,
        "\t\t\t\t\t};\n\t\t\t\t\tconst auto map_effect_identity_reused =",
        "\t\t\t\t\t};\n" + validation + "\t\t\t\t\tconst auto map_effect_identity_reused =",
        "commit mapped private validation",
    )
    consume = r'''
					if (qualified_owned_terminal)
					{
						const std::shared_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>
							exact_owner_base = map_attempt->identity_owner_control;
						auto expected_validation_phase =
							sqlite_shm_reader_mapped_effect_validation_phase::sealed;
						if (!qualified_control ||
							qualified_control->owner.lock().get() !=
								map_attempt->identity_owner_control.get() ||
							qualified_control->mapping != prepared_terminal_receipt.mapping() ||
							qualified_control->observed_attachment !=
								prepared_terminal_receipt.observed_attachment() ||
							!qualified_control->capability.matches_effect_identity(
								prepared_terminal_receipt.zero_resize_effect_receipt()) ||
							!qualified_control->capability.matches_live_owner(exact_owner_base) ||
							!map_attempt->identity_owner_control->mapped_effect_validation_phase
								 .compare_exchange_strong(
									expected_validation_phase,
									sqlite_shm_reader_mapped_effect_validation_phase::terminal_consumed,
									std::memory_order_acq_rel,
									std::memory_order_acquire))
						{
							map_attempt->identity_owner_control->mapped_effect_validation_phase.store(
								sqlite_shm_reader_mapped_effect_validation_phase::poisoned,
								std::memory_order_release);
							return quarantine_terminal(
								sqlite_shm_reader_terminal_quarantine_reason::owner_abandoned);
						}
					}
'''
    segment = replace_once(
        segment,
        "\t\t\t\t\tauto committed_state = shared_from_this();\n\t\t\t\t\tif (!claim_reader_map_identity_terminalization_locked(*map_attempt))",
        "\t\t\t\t\tauto committed_state = shared_from_this();\n" + consume + "\t\t\t\t\tif (!claim_reader_map_identity_terminalization_locked(*map_attempt))",
        "commit mapped terminal consume",
    )
    return segment

text = edit_function_region(
    text,
    "\t\t\t[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_map_commit> commit_reader(",
    "\t\t\t[[nodiscard]]\n\t\t\tsqlite_shm_lease_result<sqlite_shm_reader_attachment_zero_effect_result>\n\t\t\tcomplete_reader_zero_attachment(",
    patch_commit_reader,
    "commit_reader",
)

mapped_control_destructor = r'''
		sqlite_shm_reader_mapped_effect_receipt_control::
			~sqlite_shm_reader_mapped_effect_receipt_control() noexcept
		{
			if (!process_epoch || expected_process_epoch == 0U ||
				process_epoch->load(std::memory_order_acquire) != expected_process_epoch)
				return;
			const auto exact_owner = owner.lock();
			if (exact_owner &&
				exact_owner->mapped_effect_validation_phase.load(std::memory_order_acquire) ==
					sqlite_shm_reader_mapped_effect_validation_phase::sealed)
				exact_owner->abandon();
		}

'''
text = insert_before_once(
    text,
    "\t\tsqlite_shm_reader_zero_effect_receipt_control::\n\t\t\t~sqlite_shm_reader_zero_effect_receipt_control() noexcept",
    mapped_control_destructor,
    "mapped control destructor",
)
write(rel, text)

# ---------------------------------------------------------------------------
# Registry bridge: authenticate the exact registry/family/owner under the
# registry lock, then hand the private capability to the lease coordinator.
# ---------------------------------------------------------------------------

rel = "src/sdk/sqlite_same_process_shm_mapping_registry_internal.hpp"
text = read(rel)


def registry_class_edit(segment: str) -> str:
    segment = replace_once(
        segment,
        "\t\tfriend class sqlite_same_process_shm_registry_test_peer;\n\t\tfriend class sqlite_same_process_shm_reader_zero_effect_receipt_validator;",
        "\t\tfriend class sqlite_same_process_shm_registry_test_peer;\n\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;\n\t\tfriend class sqlite_same_process_shm_reader_zero_effect_receipt_validator;",
        "registry mapped validator friend",
    )
    method = r'''
		[[nodiscard]]
		sqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
		validate_reader_mapped_attachment_effect(
			sqlite_shm_registry_family_pin& family,
			const sqlite_shm_reader_attachment_map_inflight& inflight,
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_issued_reader_callback_identity& callback,
			const sqlite_shm_issued_reader_effect_identity& effect,
			int native_status,
			const volatile void* native_mapping,
			int delegated_extend,
			sqlite_backend_opaque_identity observed_shm_object_receipt,
			sqlite_backend_opaque_identity observed_shm_entry_receipt,
			sqlite_backend_opaque_identity observed_device_receipt,
			sqlite_backend_opaque_identity observed_mount_receipt) noexcept;
'''
    segment = insert_before_once(
        segment,
        "\t\t[[nodiscard]]\n\t\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_zero_effect_receipt>\n\t\tvalidate_reader_zero_attachment_effect(",
        method + "\n",
        "registry mapped private method",
    )
    return segment

text = edit_class(text, "sqlite_same_process_shm_mapping_registry", registry_class_edit)
write(rel, text)

rel = "src/sdk/sqlite_same_process_shm_mapping_registry_internal.cpp"
text = read(rel)
bridge_decl = r'''
		[[nodiscard]]
		sqlite_shm_lease_result<sqlite_shm_reader_mapped_effect_identity_validation_capability>
		validate_mapped_effect_identity_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_issued_reader_callback_identity& callback,
			const sqlite_shm_issued_reader_effect_identity& effect,
			const std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
				expected_owner) noexcept;
'''
text = insert_before_once(
    text,
    "\t\t[[nodiscard]]\n\t\tsqlite_shm_lease_result<sqlite_shm_reader_zero_effect_identity_validation_capability>\n\t\tvalidate_zero_effect_identity_for_registry(",
    bridge_decl + "\n",
    "registry mapped issuer declaration",
)

registry_state_method = r'''
			[[nodiscard]]
			sqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
			validate_reader_mapped_attachment_effect(
				sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_attachment_map_inflight& inflight,
				const sqlite_shm_reader_lifecycle_identity_scope& scope,
				const sqlite_shm_issued_reader_callback_identity& callback,
				const sqlite_shm_issued_reader_effect_identity& effect,
				const int native_status,
				const volatile void* native_mapping,
				const int delegated_extend,
				sqlite_backend_opaque_identity observed_shm_object_receipt,
				sqlite_backend_opaque_identity observed_shm_entry_receipt,
				sqlite_backend_opaque_identity observed_device_receipt,
				sqlite_backend_opaque_identity observed_mount_receipt,
				const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& issuer) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
						sqlite_shm_lease_recovery_action::quarantine_no_retry);
				if (inflight.terminal_presentation_stale_for_registry())
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
						sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::scoped_lock lock{mutex_};
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
							sqlite_shm_lease_recovery_action::quarantine_no_retry);
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					const auto exact_owner = inflight.qualified_identity_owned_for_registry(
						pin.family_epoch_, pin.pin_token_, pin.alias_token_, seal_->process_epoch,
						activity_emergency_latch_);
					if (!inflight.has_qualified_identity_for_registry())
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
							sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!exact_owner)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
							sqlite_shm_lease_recovery_action::quarantine_no_retry);
					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr ||
						!family->coordinator || admission_quarantined_locked())
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
							sqlite_shm_lease_recovery_action::quarantine_no_retry);
					auto capability = detail::validate_mapped_effect_identity_for_registry(issuer,
						scope,
						callback,
						effect,
						inflight.qualified_identity_owner_abandonment_for_registry());
					if (!capability)
						return capability.error();
					return family->coordinator->validate_registry_reader_mapped_attachment_effect(
						pin,
						inflight,
						std::move(*capability),
						native_status,
						native_mapping,
						delegated_extend,
						std::move(observed_shm_object_receipt),
						std::move(observed_shm_entry_receipt),
						std::move(observed_device_receipt),
						std::move(observed_mount_receipt));
				}
				catch (...)
				{
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
						sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

'''
text = insert_before_once(
    text,
    "\t\t\t[[nodiscard]]\n\t\t\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_zero_effect_receipt>\n\t\t\tvalidate_reader_zero_attachment_effect(",
    registry_state_method,
    "registry state mapped validator",
)

public_wrapper = r'''
	sqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
	sqlite_same_process_shm_mapping_registry::validate_reader_mapped_attachment_effect(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_reader_lifecycle_identity_scope& scope,
		const sqlite_shm_issued_reader_callback_identity& callback,
		const sqlite_shm_issued_reader_effect_identity& effect,
		const int native_status,
		const volatile void* native_mapping,
		const int delegated_extend,
		sqlite_backend_opaque_identity observed_shm_object_receipt,
		sqlite_backend_opaque_identity observed_shm_entry_receipt,
		sqlite_backend_opaque_identity observed_device_receipt,
		sqlite_backend_opaque_identity observed_mount_receipt) noexcept
	{
		return state_->validate_reader_mapped_attachment_effect(family,
			inflight,
			scope,
			callback,
			effect,
			native_status,
			native_mapping,
			delegated_extend,
			std::move(observed_shm_object_receipt),
			std::move(observed_shm_entry_receipt),
			std::move(observed_device_receipt),
			std::move(observed_mount_receipt),
			identity_issuer_state_);
	}

'''
text = insert_before_once(
    text,
    "\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_zero_effect_receipt>\n\tsqlite_same_process_shm_mapping_registry::validate_reader_zero_attachment_effect(",
    public_wrapper,
    "registry public mapped wrapper",
)
write(rel, text)

# ---------------------------------------------------------------------------
# Focused integration test: the existing ordinary-quarantine mapped branch is
# converted from test-peer construction to the closed production-inert validator.
# ---------------------------------------------------------------------------

rel = "tests/unit/sdk/sqlite_same_process_shm_mapping_registry_test.cpp"
text = read(rel)
pattern = re.compile(
    r'''\t\t\tif \(mapped\)\n\t\t\t\{\n\t\t\t\tauto committed = setup\.fixture\.registry->commit_reader_map\(\n\t\t\t\t\t\*setup\.fixture\.family_pin,\n\t\t\t\t\t\*bound,\n\t\t\t\t\tsqlite_same_process_shm_lease_test_peer::reader_attachment_map\(\n\t\t\t\t\t\trequest,\n\t\t\t\t\t\tsetup\.holder\.generation\(\),\n\t\t\t\t\t\tmapping\(setup\.writer_attempt\.native_page\.get\(\)\),\n\t\t\t\t\t\teffect->identity\(\)\),\n\t\t\t\t\tsetup\.session\);\n\t\t\t\trequire\(committed && committed->formed_group\(\) && !bound->valid\(\),\n\t\t\t\t\t"owned mapped terminal survives ordinary family quarantine"\);\n\t\t\t\}'''
)
replacement = r'''			if (mapped)
			{
				const auto exact_mapping = mapping(setup.writer_attempt.native_page.get());
				auto receipt = sqlite_same_process_shm_reader_receipt_validator::validate(
					*setup.fixture.registry,
					*setup.fixture.family_pin,
					*bound,
					owner.scope,
					owner.callback_identity,
					*effect,
					0,
					exact_mapping.native_mapping,
					0,
					identity("test.registry.qualified-mapped-shm-object", 78U),
					identity("test.registry.qualified-mapped-shm-entry", 78U),
					identity("test.registry.qualified-mapped-device", 78U),
					identity("test.registry.qualified-mapped-mount", 78U));
				require(receipt && effect->valid(),
					"owned mapped result validates after ordinary family quarantine");
				auto committed = setup.fixture.registry->commit_reader_map(
					*setup.fixture.family_pin, *bound, *receipt, setup.session);
				require(committed && committed->formed_group() && !bound->valid(),
					"owned mapped terminal survives ordinary family quarantine");
			}'''
if "owned mapped result validates after ordinary family quarantine" not in text:
    text, count = pattern.subn(replacement, text, count=1)
    if count != 1:
        raise RuntimeError(f"focused mapped test branch: expected one replacement, found {count}")
write(rel, text)

print("U2a1c owner-qualified mapped-result patch applied")
