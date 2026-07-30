#pragma once

#include <array>
#include <cstdint>

namespace cxxlens::sdk::detail
{
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
