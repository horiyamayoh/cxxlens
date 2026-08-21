#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cxxlens::sdk::detail
{
	/**
	 * The bounded #201 U201-R0 active-read connection product.
	 *
	 * This is deliberately a smaller machine than the outer read/receipt machine below.  It
	 * authenticates the source family and the read-only connection before the first SHM map.  It
	 * carries no logical-read, mapping-lease, normalization, or zero-effect receipt authority.
	 * Failures are returned by the source preflight validator as typed fail-closed errors rather
	 * than being represented as a successful phase.
	 */
	enum class sqlite_active_read_connection_phase : std::uint8_t
	{
		unopened,
		source_family_sealed,
		readonly_profile_selected,
		outer_custody_open,
		active_read_connection,
	};

	inline constexpr std::array sqlite_active_read_connection_phases{
		sqlite_active_read_connection_phase::unopened,
		sqlite_active_read_connection_phase::source_family_sealed,
		sqlite_active_read_connection_phase::readonly_profile_selected,
		sqlite_active_read_connection_phase::outer_custody_open,
		sqlite_active_read_connection_phase::active_read_connection,
	};

	[[nodiscard]] constexpr bool is_sqlite_active_read_connection_transition(
		const sqlite_active_read_connection_phase origin,
		const sqlite_active_read_connection_phase destination) noexcept
	{
		using phase = sqlite_active_read_connection_phase;
		switch (origin)
		{
			case phase::unopened:
				return destination == phase::source_family_sealed;
			case phase::source_family_sealed:
				return destination == phase::readonly_profile_selected;
			case phase::readonly_profile_selected:
				return destination == phase::outer_custody_open;
			case phase::outer_custody_open:
				return destination == phase::active_read_connection;
			case phase::active_read_connection:
				return false;
		}
		return false;
	}

	/**
	 * The #201 outer zero-effect read success phases in the proposed, production-inactive
	 * state-only graph.
	 *
	 * This vocabulary mirrors the current Proposed ADR 0104 / DF-0201 state-and-test scope.  The
	 * #205 mapping subprotocol or private heap WAL-index route is nested between the held WAL
	 * prefix and eager decode.  The outer success graph deliberately has no generic quarantine
	 * edge: failure/ambiguity is owned by the typed nested/teardown terminal routes and cannot be
	 * converted into an outer receipt.  No phase or transition grants SQLite, VFS, mapping,
	 * normalization, public Store, or production-activation authority.
	 */
	enum class sqlite_shm_reader_outer_read_phase : std::uint8_t
	{
		unresolved,
		runtime_vfs_filesystem_sealed,
		retained_parent_held,
		no_effect_boundary_armed,
		typed_family_census,
		active_read_connection_open,
		wal_lock_and_prefix_held,
		mapping_subprotocol_or_private_index,
		eager_decode,
		decoded_read_candidate_sealed,
		connection_revoking,
		outer_custody_join_pending,
		outer_custody_join_sealed,
		connection_closed,
		zero_effect_callback_receipt_sealed,
		logical_read_receipt,
	};

	inline constexpr std::array sqlite_shm_reader_outer_read_phases{
		sqlite_shm_reader_outer_read_phase::unresolved,
		sqlite_shm_reader_outer_read_phase::runtime_vfs_filesystem_sealed,
		sqlite_shm_reader_outer_read_phase::retained_parent_held,
		sqlite_shm_reader_outer_read_phase::no_effect_boundary_armed,
		sqlite_shm_reader_outer_read_phase::typed_family_census,
		sqlite_shm_reader_outer_read_phase::active_read_connection_open,
		sqlite_shm_reader_outer_read_phase::wal_lock_and_prefix_held,
		sqlite_shm_reader_outer_read_phase::mapping_subprotocol_or_private_index,
		sqlite_shm_reader_outer_read_phase::eager_decode,
		sqlite_shm_reader_outer_read_phase::decoded_read_candidate_sealed,
		sqlite_shm_reader_outer_read_phase::connection_revoking,
		sqlite_shm_reader_outer_read_phase::outer_custody_join_pending,
		sqlite_shm_reader_outer_read_phase::outer_custody_join_sealed,
		sqlite_shm_reader_outer_read_phase::connection_closed,
		sqlite_shm_reader_outer_read_phase::zero_effect_callback_receipt_sealed,
		sqlite_shm_reader_outer_read_phase::logical_read_receipt,
	};

	[[nodiscard]] constexpr bool is_sqlite_shm_reader_outer_read_transition(
		const sqlite_shm_reader_outer_read_phase origin,
		const sqlite_shm_reader_outer_read_phase destination) noexcept
	{
		using phase = sqlite_shm_reader_outer_read_phase;
		switch (origin)
		{
			case phase::unresolved:
				return destination == phase::runtime_vfs_filesystem_sealed;
			case phase::runtime_vfs_filesystem_sealed:
				return destination == phase::retained_parent_held;
			case phase::retained_parent_held:
				return destination == phase::no_effect_boundary_armed;
			case phase::no_effect_boundary_armed:
				return destination == phase::typed_family_census;
			case phase::typed_family_census:
				return destination == phase::active_read_connection_open;
			case phase::active_read_connection_open:
				return destination == phase::wal_lock_and_prefix_held;
			case phase::wal_lock_and_prefix_held:
				return destination == phase::mapping_subprotocol_or_private_index;
			case phase::mapping_subprotocol_or_private_index:
				return destination == phase::eager_decode;
			case phase::eager_decode:
				return destination == phase::decoded_read_candidate_sealed;
			case phase::decoded_read_candidate_sealed:
				return destination == phase::connection_revoking;
			case phase::connection_revoking:
				return destination == phase::outer_custody_join_pending;
			case phase::outer_custody_join_pending:
				return destination == phase::outer_custody_join_sealed;
			case phase::outer_custody_join_sealed:
				return destination == phase::connection_closed;
			case phase::connection_closed:
				return destination == phase::zero_effect_callback_receipt_sealed;
			case phase::zero_effect_callback_receipt_sealed:
				return destination == phase::logical_read_receipt;
			case phase::logical_read_receipt:
				return false;
		}
		return false;
	}

	/**
	 * Validate the complete #201 outer success path in the proposed, production-inactive state-only
	 * graph.  The path has no generic failure state: a typed failure or quarantine terminal is not
	 * an outer logical-read receipt candidate.
	 */
	[[nodiscard]] constexpr bool validate_sqlite_shm_reader_outer_read_path(
		const std::span<const sqlite_shm_reader_outer_read_phase> path) noexcept
	{
		if (path.empty() || path.front() != sqlite_shm_reader_outer_read_phase::unresolved)
			return false;
		for (std::size_t index = 1U; index < path.size(); ++index)
		{
			if (!is_sqlite_shm_reader_outer_read_transition(path[index - 1U], path[index]))
				return false;
		}
		return path.back() == sqlite_shm_reader_outer_read_phase::logical_read_receipt;
	}

	/** The #202 exact-empty normalization cutover graph (production-inactive). */
	enum class sqlite_shm_reader_normalization_phase : std::uint8_t
	{
		no_authenticated_receipt,
		logical_read_receipt_authenticated,
		exclusive_source_revalidated,
		pre_effect_receipt_sealed,
		effect_armed,
		/** Every allowed callback effect is present exactly once and in contract order. */
		effect_transcript_sealed,
		/** Coordination-WAL delete, journal creation, and terminal delete have parent-sync
		 * receipts. */
		durability_barrier_sealed,
		effect_confirmed,
		connection_closed,
		post_effect_projection_validated,
		terminal_quarantined,
	};

	inline constexpr std::array sqlite_shm_reader_normalization_phases{
		sqlite_shm_reader_normalization_phase::no_authenticated_receipt,
		sqlite_shm_reader_normalization_phase::logical_read_receipt_authenticated,
		sqlite_shm_reader_normalization_phase::exclusive_source_revalidated,
		sqlite_shm_reader_normalization_phase::pre_effect_receipt_sealed,
		sqlite_shm_reader_normalization_phase::effect_armed,
		sqlite_shm_reader_normalization_phase::effect_transcript_sealed,
		sqlite_shm_reader_normalization_phase::durability_barrier_sealed,
		sqlite_shm_reader_normalization_phase::effect_confirmed,
		sqlite_shm_reader_normalization_phase::connection_closed,
		sqlite_shm_reader_normalization_phase::post_effect_projection_validated,
		sqlite_shm_reader_normalization_phase::terminal_quarantined,
	};

	[[nodiscard]] constexpr bool is_sqlite_shm_reader_normalization_transition(
		const sqlite_shm_reader_normalization_phase origin,
		const sqlite_shm_reader_normalization_phase destination) noexcept
	{
		using phase = sqlite_shm_reader_normalization_phase;
		if (destination == phase::terminal_quarantined &&
			origin != phase::post_effect_projection_validated &&
			origin != phase::terminal_quarantined)
			return true;
		switch (origin)
		{
			case phase::no_authenticated_receipt:
				return destination == phase::logical_read_receipt_authenticated;
			case phase::logical_read_receipt_authenticated:
				return destination == phase::exclusive_source_revalidated;
			case phase::exclusive_source_revalidated:
				return destination == phase::pre_effect_receipt_sealed;
			case phase::pre_effect_receipt_sealed:
				return destination == phase::effect_armed;
			case phase::effect_armed:
				return destination == phase::effect_transcript_sealed;
			case phase::effect_transcript_sealed:
				return destination == phase::durability_barrier_sealed;
			case phase::durability_barrier_sealed:
				return destination == phase::effect_confirmed;
			case phase::effect_confirmed:
				return destination == phase::connection_closed;
			case phase::connection_closed:
				return destination == phase::post_effect_projection_validated;
			case phase::post_effect_projection_validated:
			case phase::terminal_quarantined:
				return false;
		}
		return false;
	}

	[[nodiscard]] constexpr bool validate_sqlite_shm_reader_normalization_path(
		const std::span<const sqlite_shm_reader_normalization_phase> path) noexcept
	{
		using phase = sqlite_shm_reader_normalization_phase;
		if (path.empty() || path.front() != phase::no_authenticated_receipt)
			return false;
		for (std::size_t index = 1U; index < path.size(); ++index)
			if (!is_sqlite_shm_reader_normalization_transition(path[index - 1U], path[index]))
				return false;
		return path.back() == phase::post_effect_projection_validated;
	}

	/**
	 * Canonical DF-0207 reader attachment reservation phases.
	 *
	 * This vocabulary is deliberately state-only. The process-registry-owned sequence issuer and
	 * durable lifecycle ledger activate these phases together in the subsequent cutover; no phase
	 * in this header grants native, pointer, cleanup, acknowledgement, close, or public authority.
	 */
	enum class sqlite_shm_reader_attachment_reservation_phase : std::uint8_t
	{
		reserved,
		predecessor_route_active,
		predecessor_route_retired_confirmed,
		observed_present,
		retired_confirmed,
		revoked_no_map,
		unpublished_cleanup_admitted,
		unpublished_cleanup_confirmed,
		terminal_quarantined,
	};

	inline constexpr std::array sqlite_shm_reader_attachment_reservation_phases{
		sqlite_shm_reader_attachment_reservation_phase::reserved,
		sqlite_shm_reader_attachment_reservation_phase::predecessor_route_active,
		sqlite_shm_reader_attachment_reservation_phase::predecessor_route_retired_confirmed,
		sqlite_shm_reader_attachment_reservation_phase::observed_present,
		sqlite_shm_reader_attachment_reservation_phase::retired_confirmed,
		sqlite_shm_reader_attachment_reservation_phase::revoked_no_map,
		sqlite_shm_reader_attachment_reservation_phase::unpublished_cleanup_admitted,
		sqlite_shm_reader_attachment_reservation_phase::unpublished_cleanup_confirmed,
		sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined,
	};

	[[nodiscard]] constexpr bool is_sqlite_shm_reader_attachment_reservation_transition(
		const sqlite_shm_reader_attachment_reservation_phase origin,
		const sqlite_shm_reader_attachment_reservation_phase destination) noexcept
	{
		using phase = sqlite_shm_reader_attachment_reservation_phase;
		switch (origin)
		{
			case phase::reserved:
				return destination == phase::predecessor_route_active ||
					destination == phase::observed_present ||
					destination == phase::revoked_no_map ||
					destination == phase::unpublished_cleanup_admitted ||
					destination == phase::terminal_quarantined;
			case phase::predecessor_route_active:
				return destination == phase::predecessor_route_retired_confirmed ||
					destination == phase::terminal_quarantined;
			case phase::observed_present:
				return destination == phase::retired_confirmed ||
					destination == phase::terminal_quarantined;
			case phase::unpublished_cleanup_admitted:
				return destination == phase::unpublished_cleanup_confirmed ||
					destination == phase::terminal_quarantined;
			case phase::predecessor_route_retired_confirmed:
			case phase::retired_confirmed:
			case phase::revoked_no_map:
			case phase::unpublished_cleanup_confirmed:
			case phase::terminal_quarantined:
				return false;
		}
		return false;
	}

	enum class sqlite_shm_reader_session_reservation_phase : std::uint8_t
	{
		reserved_before_sqlite,
		promoted_to_group_owner,
		transferred_to_existing_predecessor,
		consumed_no_pointer,
		terminal_quarantined,
	};

	inline constexpr std::array sqlite_shm_reader_session_reservation_phases{
		sqlite_shm_reader_session_reservation_phase::reserved_before_sqlite,
		sqlite_shm_reader_session_reservation_phase::promoted_to_group_owner,
		sqlite_shm_reader_session_reservation_phase::transferred_to_existing_predecessor,
		sqlite_shm_reader_session_reservation_phase::consumed_no_pointer,
		sqlite_shm_reader_session_reservation_phase::terminal_quarantined,
	};

	[[nodiscard]] constexpr bool is_sqlite_shm_reader_session_reservation_transition(
		const sqlite_shm_reader_session_reservation_phase origin,
		const sqlite_shm_reader_session_reservation_phase destination) noexcept
	{
		using phase = sqlite_shm_reader_session_reservation_phase;
		if (origin != phase::reserved_before_sqlite)
			return false;
		return destination == phase::promoted_to_group_owner ||
			destination == phase::transferred_to_existing_predecessor ||
			destination == phase::consumed_no_pointer || destination == phase::terminal_quarantined;
	}

	/** Closed ownership vocabulary; additions require a new accepted contract revision. */
	enum class sqlite_shm_reader_custody_kind : std::uint8_t
	{
		map_attempt,
		use_session_reservation,
		attachment_group_handoff,
		generation_group_count,
		use_session,
		exact_present_attachment,
		normal_or_deferred_unmap,
		unpublished_cleanup,
		logical_ack,
		connection_close,
		unmap_cut,
		close_cut_or_composite,
		bounded_waiter_or_continuation,
		terminal_reporter,
		late_close_original_callback_drain,
		late_close_outer_unwind_validation_seal,
		opaque_attachment_uncertainty,
		runtime_vfs_namespace_generation_native_mapping_lifetime_pin,
	};

	inline constexpr std::array sqlite_shm_reader_custody_kinds{
		sqlite_shm_reader_custody_kind::map_attempt,
		sqlite_shm_reader_custody_kind::use_session_reservation,
		sqlite_shm_reader_custody_kind::attachment_group_handoff,
		sqlite_shm_reader_custody_kind::generation_group_count,
		sqlite_shm_reader_custody_kind::use_session,
		sqlite_shm_reader_custody_kind::exact_present_attachment,
		sqlite_shm_reader_custody_kind::normal_or_deferred_unmap,
		sqlite_shm_reader_custody_kind::unpublished_cleanup,
		sqlite_shm_reader_custody_kind::logical_ack,
		sqlite_shm_reader_custody_kind::connection_close,
		sqlite_shm_reader_custody_kind::unmap_cut,
		sqlite_shm_reader_custody_kind::close_cut_or_composite,
		sqlite_shm_reader_custody_kind::bounded_waiter_or_continuation,
		sqlite_shm_reader_custody_kind::terminal_reporter,
		sqlite_shm_reader_custody_kind::late_close_original_callback_drain,
		sqlite_shm_reader_custody_kind::late_close_outer_unwind_validation_seal,
		sqlite_shm_reader_custody_kind::opaque_attachment_uncertainty,
		sqlite_shm_reader_custody_kind::
			runtime_vfs_namespace_generation_native_mapping_lifetime_pin,
	};

	enum class sqlite_shm_reader_custody_state : std::uint8_t
	{
		live,
		consumed_with_exact_terminal_receipt,
		transferred_to_exact_successor,
		transferred_to_durable_tombstone,
	};

	inline constexpr std::array sqlite_shm_reader_custody_states{
		sqlite_shm_reader_custody_state::live,
		sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
		sqlite_shm_reader_custody_state::transferred_to_exact_successor,
		sqlite_shm_reader_custody_state::transferred_to_durable_tombstone,
	};

	enum class sqlite_shm_reader_terminal_quarantine_reason : std::uint8_t
	{
		none,
		owner_abandoned,
		presented_invalid,
		peer_quarantine,
		native_non_ok_or_unknown,
		injected_commit_failure,
		internal_failure,
	};

	[[nodiscard]] constexpr bool is_sqlite_shm_reader_custody_transition(
		const sqlite_shm_reader_custody_kind kind,
		const sqlite_shm_reader_custody_state origin,
		const sqlite_shm_reader_custody_state destination) noexcept
	{
		using kind_type = sqlite_shm_reader_custody_kind;
		using state = sqlite_shm_reader_custody_state;
		if (origin != state::live || destination == state::live)
			return false;
		switch (kind)
		{
			case kind_type::bounded_waiter_or_continuation:
			case kind_type::terminal_reporter:
				return destination == state::consumed_with_exact_terminal_receipt ||
					destination == state::transferred_to_durable_tombstone;
			case kind_type::opaque_attachment_uncertainty:
			case kind_type::runtime_vfs_namespace_generation_native_mapping_lifetime_pin:
				return destination == state::transferred_to_durable_tombstone;
			case kind_type::map_attempt:
			case kind_type::use_session_reservation:
			case kind_type::attachment_group_handoff:
			case kind_type::generation_group_count:
			case kind_type::use_session:
			case kind_type::exact_present_attachment:
			case kind_type::normal_or_deferred_unmap:
			case kind_type::unpublished_cleanup:
			case kind_type::logical_ack:
			case kind_type::connection_close:
			case kind_type::unmap_cut:
			case kind_type::close_cut_or_composite:
			case kind_type::late_close_original_callback_drain:
			case kind_type::late_close_outer_unwind_validation_seal:
				return true;
		}
		return false;
	}

	enum class sqlite_shm_reader_attachment_group_phase : std::uint8_t
	{
		active,
		unmap_cut_sealing,
		native_unmap_admitted,
		native_unmap_confirmed,
		terminal_quarantined,
	};

	inline constexpr std::array sqlite_shm_reader_attachment_group_phases{
		sqlite_shm_reader_attachment_group_phase::active,
		sqlite_shm_reader_attachment_group_phase::unmap_cut_sealing,
		sqlite_shm_reader_attachment_group_phase::native_unmap_admitted,
		sqlite_shm_reader_attachment_group_phase::native_unmap_confirmed,
		sqlite_shm_reader_attachment_group_phase::terminal_quarantined,
	};

	[[nodiscard]] constexpr bool is_sqlite_shm_reader_attachment_group_transition(
		const sqlite_shm_reader_attachment_group_phase origin,
		const sqlite_shm_reader_attachment_group_phase destination) noexcept
	{
		using phase = sqlite_shm_reader_attachment_group_phase;
		switch (origin)
		{
			case phase::active:
				return destination == phase::unmap_cut_sealing ||
					destination == phase::terminal_quarantined;
			case phase::unmap_cut_sealing:
				return destination == phase::native_unmap_admitted ||
					destination == phase::terminal_quarantined;
			case phase::native_unmap_admitted:
				return destination == phase::native_unmap_confirmed ||
					destination == phase::terminal_quarantined;
			case phase::native_unmap_confirmed:
			case phase::terminal_quarantined:
				return false;
		}
		return false;
	}

	enum class sqlite_shm_reader_logical_ack_phase : std::uint8_t
	{
		not_applicable,
		awaiting_sqlite_ack,
		consumed_by_exact_unmap,
		consumed_by_close,
	};

	inline constexpr std::array sqlite_shm_reader_logical_ack_phases{
		sqlite_shm_reader_logical_ack_phase::not_applicable,
		sqlite_shm_reader_logical_ack_phase::awaiting_sqlite_ack,
		sqlite_shm_reader_logical_ack_phase::consumed_by_exact_unmap,
		sqlite_shm_reader_logical_ack_phase::consumed_by_close,
	};

	[[nodiscard]] constexpr bool is_sqlite_shm_reader_logical_ack_transition(
		const sqlite_shm_reader_logical_ack_phase origin,
		const sqlite_shm_reader_logical_ack_phase destination) noexcept
	{
		using phase = sqlite_shm_reader_logical_ack_phase;
		if (origin == phase::not_applicable)
			return destination == phase::awaiting_sqlite_ack;
		if (origin == phase::awaiting_sqlite_ack)
			return destination == phase::consumed_by_exact_unmap ||
				destination == phase::consumed_by_close;
		return false;
	}

	/** Closed DF-0209 subledger. It never reactivates an ordinary reader lifecycle phase. */
	enum class sqlite_shm_reader_late_close_drain_phase : std::uint8_t
	{
		not_applicable,
		retained_original_callback_drain,
		cleanup_admitted,
		cleanup_confirmed_awaiting_sqlite_ack,
		terminal_quarantined,
		consumed_by_exact_outer_unmap,
	};

	inline constexpr std::array sqlite_shm_reader_late_close_drain_phases{
		sqlite_shm_reader_late_close_drain_phase::not_applicable,
		sqlite_shm_reader_late_close_drain_phase::retained_original_callback_drain,
		sqlite_shm_reader_late_close_drain_phase::cleanup_admitted,
		sqlite_shm_reader_late_close_drain_phase::cleanup_confirmed_awaiting_sqlite_ack,
		sqlite_shm_reader_late_close_drain_phase::terminal_quarantined,
		sqlite_shm_reader_late_close_drain_phase::consumed_by_exact_outer_unmap,
	};

	[[nodiscard]] constexpr bool is_sqlite_shm_reader_late_close_drain_transition(
		const sqlite_shm_reader_late_close_drain_phase origin,
		const sqlite_shm_reader_late_close_drain_phase destination) noexcept
	{
		using phase = sqlite_shm_reader_late_close_drain_phase;
		switch (origin)
		{
			case phase::not_applicable:
				return destination == phase::retained_original_callback_drain;
			case phase::retained_original_callback_drain:
				return destination == phase::cleanup_admitted ||
					destination == phase::terminal_quarantined;
			case phase::cleanup_admitted:
				return destination == phase::cleanup_confirmed_awaiting_sqlite_ack ||
					destination == phase::terminal_quarantined;
			case phase::cleanup_confirmed_awaiting_sqlite_ack:
				return destination == phase::consumed_by_exact_outer_unmap ||
					destination == phase::terminal_quarantined;
			case phase::terminal_quarantined:
			case phase::consumed_by_exact_outer_unmap:
				return false;
		}
		return false;
	}

	enum class sqlite_shm_reader_connection_close_phase : std::uint8_t
	{
		open,
		close_admitted,
		closed,
		terminal_quarantined,
	};

	inline constexpr std::array sqlite_shm_reader_connection_close_phases{
		sqlite_shm_reader_connection_close_phase::open,
		sqlite_shm_reader_connection_close_phase::close_admitted,
		sqlite_shm_reader_connection_close_phase::closed,
		sqlite_shm_reader_connection_close_phase::terminal_quarantined,
	};

	[[nodiscard]] constexpr bool is_sqlite_shm_reader_connection_close_transition(
		const sqlite_shm_reader_connection_close_phase origin,
		const sqlite_shm_reader_connection_close_phase destination) noexcept
	{
		using phase = sqlite_shm_reader_connection_close_phase;
		if (origin == phase::open)
			return destination == phase::close_admitted ||
				destination == phase::terminal_quarantined;
		if (origin == phase::close_admitted)
			return destination == phase::closed || destination == phase::terminal_quarantined;
		return false;
	}

	/**
	 * Events which share the one process-registry-epoch checked sequence domain.
	 *
	 * `session_start_admission` includes publication of its proposal session reservation or active
	 * group use owner in the same indivisible event. One indivisible transition may carry multiple
	 * roles: for example, a first-map `map_terminal` also performs
	 * `use_session_owner_promotion_or_admission` at the same sequence. Enum values classify roles;
	 * they do not each require a distinct sequence allocation.
	 */
	enum class sqlite_shm_reader_lifecycle_event_kind : std::uint8_t
	{
		session_start_admission,
		map_admission,
		use_session_owner_promotion_or_admission,
		map_terminal,
		use_session_terminal,
		unmap_cut,
		close_cut,
		cut_terminal_commit,
	};

	inline constexpr std::array sqlite_shm_reader_lifecycle_event_kinds{
		sqlite_shm_reader_lifecycle_event_kind::session_start_admission,
		sqlite_shm_reader_lifecycle_event_kind::map_admission,
		sqlite_shm_reader_lifecycle_event_kind::use_session_owner_promotion_or_admission,
		sqlite_shm_reader_lifecycle_event_kind::map_terminal,
		sqlite_shm_reader_lifecycle_event_kind::use_session_terminal,
		sqlite_shm_reader_lifecycle_event_kind::unmap_cut,
		sqlite_shm_reader_lifecycle_event_kind::close_cut,
		sqlite_shm_reader_lifecycle_event_kind::cut_terminal_commit,
	};

	/**
	 * Derived implementation partition for the accepted unmap/close rows.
	 *
	 * Unlike the authority-mirrored phase vocabularies above, these cut details remain internal
	 * and may be refined when the registry-owned cut ledger is activated.
	 */
	enum class sqlite_shm_reader_cut_kind : std::uint8_t
	{
		normal_or_deferred_unmap,
		close_without_group,
		close_after_confirmed_unmap,
		composite_unmap_then_close,
	};

	inline constexpr std::array sqlite_shm_reader_cut_kinds{
		sqlite_shm_reader_cut_kind::normal_or_deferred_unmap,
		sqlite_shm_reader_cut_kind::close_without_group,
		sqlite_shm_reader_cut_kind::close_after_confirmed_unmap,
		sqlite_shm_reader_cut_kind::composite_unmap_then_close,
	};

	enum class sqlite_shm_reader_cut_phase : std::uint8_t
	{
		sealed_waiting,
		ready,
		native_effect_admitted,
		terminal_confirmed,
		terminal_quarantined,
	};

	inline constexpr std::array sqlite_shm_reader_cut_phases{
		sqlite_shm_reader_cut_phase::sealed_waiting,
		sqlite_shm_reader_cut_phase::ready,
		sqlite_shm_reader_cut_phase::native_effect_admitted,
		sqlite_shm_reader_cut_phase::terminal_confirmed,
		sqlite_shm_reader_cut_phase::terminal_quarantined,
	};

	[[nodiscard]] constexpr bool
	is_sqlite_shm_reader_cut_transition(const sqlite_shm_reader_cut_phase origin,
										const sqlite_shm_reader_cut_phase destination) noexcept
	{
		using phase = sqlite_shm_reader_cut_phase;
		switch (origin)
		{
			case phase::sealed_waiting:
				return destination == phase::ready || destination == phase::terminal_quarantined;
			case phase::ready:
				return destination == phase::native_effect_admitted ||
					destination == phase::terminal_quarantined;
			case phase::native_effect_admitted:
				return destination == phase::terminal_confirmed ||
					destination == phase::terminal_quarantined;
			case phase::terminal_confirmed:
			case phase::terminal_quarantined:
				return false;
		}
		return false;
	}
} // namespace cxxlens::sdk::detail
