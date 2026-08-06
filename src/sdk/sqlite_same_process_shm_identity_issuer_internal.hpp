#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "sqlite_same_process_shm_mapping_registry_internal.hpp"

namespace cxxlens::sdk
{
	namespace detail
	{
		struct sqlite_shm_process_identity_record_control;
		struct sqlite_shm_reader_lifecycle_identity_scope_control;
		class sqlite_shm_reader_lifecycle_owner_abandonment_control
		{
		  public:
			virtual ~sqlite_shm_reader_lifecycle_owner_abandonment_control() = default;
			virtual void abandon() noexcept = 0;
			[[nodiscard]] virtual bool terminal_completion_claimed() const noexcept
			{
				return false;
			}
		};

		class sqlite_shm_reader_identity_completion_control
		{
		  public:
			virtual ~sqlite_shm_reader_identity_completion_control() = default;
			virtual void complete() noexcept = 0;
		};
	}

	/** Closed reader callback roles already present in the accepted DF-0207/DF-0209 ledger. */
	enum class sqlite_shm_reader_callback_identity_role : std::uint8_t
	{
		map,
		unpublished_cleanup_unmap,
		attachment_unmap,
		close,
		logical_ack_unmap,
		late_outer_unmap,
	};

	/**
	 * Closed reader native/zero-effect roles already present in that ledger.
	 *
	 * Map admits exactly one of mapped/zero. Unpublished cleanup and attachment unmap may each
	 * admit native-unmap plus latch-reset. Logical acknowledgement and the DF-0209 late outer
	 * unwind admit no effect identity. U1's direct close scope admits native-close only; sequencing
	 * for a future live composite close remains deferred with the production owner gate.
	 */
	enum class sqlite_shm_reader_effect_identity_role : std::uint8_t
	{
		mapped_result,
		zero_attachment_result,
		native_unmap,
		latch_reset,
		native_close,
	};

	/** Closed terminal roles for an exact reader session owner. */
	enum class sqlite_shm_reader_session_terminal_identity_role : std::uint8_t
	{
		success,
		failure,
		cancelled_before_authority_read,
	};

	enum class sqlite_shm_reader_lifecycle_identity_domain : std::uint8_t
	{
		callback_invocation,
		native_or_zero_effect,
		session_terminal,
	};

	/** Lock-free phase of a production-qualified lifecycle owner. */
	enum class sqlite_shm_reader_lifecycle_owner_phase : std::uint8_t
	{
		admission,
		owned,
		inactive,
	};

	/** Deterministic concurrency cut points exposed only through the registry test peer. */
	enum class sqlite_shm_identity_issuer_pause_point_for_testing : std::uint8_t
	{
		none,
		reserve_after_scope_count,
		effect_after_callback_phase,
	};

	/** Closed lifecycle owner class asserted when the U1 issuer-core test scope is sealed. */
	enum class sqlite_shm_reader_lifecycle_owner_kind : std::uint8_t
	{
		map,
		unpublished_cleanup,
		attachment,
		close,
		logical_ack,
		late_outer_unwind,
		session,
	};

	/**
	 * Asserted coordinates for one reader lifecycle owner family.
	 *
	 * This aggregate is not authority. The U1 `_for_testing` seam remains an unqualified route: it
	 * authenticates the exact registry and live family pin, then records these values in a private
	 * scope control, but does not prove that the asserted owner/request exists or enforce one scope
	 * per owner. U2a1a's separate registry-private reader-map gate performs that qualification; copied
	 * coordinates do not convey it and cannot be presented to this issuer or validator by themselves.
	 */
	struct sqlite_shm_reader_lifecycle_owner_coordinates
	{
		std::uint64_t registry_open_token{};
		sqlite_shm_reader_lifecycle_owner_kind owner_kind{
			sqlite_shm_reader_lifecycle_owner_kind::map};
		std::uint64_t lifecycle_owner_token{};
		std::uint64_t writer_mapping_generation{};

		[[nodiscard]] bool
		operator==(const sqlite_shm_reader_lifecycle_owner_coordinates&) const = default;
	};

	/**
	 * Move-only presenter for one exact live registry/family control plus an asserted owner/request.
	 *
	 * It grants no callback or effect authority by itself. Only the process-global issuer can
	 * consume it, and every validation checks exact private issuer/scope control provenance.
	 * The U1 `_for_testing` registry seam remains unqualified and does not authenticate owner
	 * existence or exact-one scope minting. U2a1a's registry-private reader-map gate is separate and
	 * is not conveyed by this presenter alone.
	 * The presenter object itself must not be concurrently moved or destroyed while an issuance
	 * call uses it. `retire_scope` is an atomic control transition and may race issuance; one side
	 * wins the documented total order without replacing the presenter's shared control. Independent
	 * scopes may issue concurrently. Any role claim that crosses the allocation cut remains burned
	 * on a later stale/exhausted failure and is never reconstructed.
	 */
	class sqlite_shm_reader_lifecycle_identity_scope
	{
	  public:
		~sqlite_shm_reader_lifecycle_identity_scope() noexcept;
		sqlite_shm_reader_lifecycle_identity_scope(
			const sqlite_shm_reader_lifecycle_identity_scope&) = delete;
		sqlite_shm_reader_lifecycle_identity_scope&
		operator=(const sqlite_shm_reader_lifecycle_identity_scope&) = delete;
		sqlite_shm_reader_lifecycle_identity_scope(
			sqlite_shm_reader_lifecycle_identity_scope&&) noexcept = default;
		sqlite_shm_reader_lifecycle_identity_scope&
		operator=(sqlite_shm_reader_lifecycle_identity_scope&&) = delete;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class detail::sqlite_shm_process_identity_issuer_state;
		friend class sqlite_same_process_shm_mapping_registry;
		friend class sqlite_same_process_shm_registry_test_peer;
		friend class sqlite_shm_process_global_identity_issuer;

		explicit sqlite_shm_reader_lifecycle_identity_scope(
			std::shared_ptr<detail::sqlite_shm_reader_lifecycle_identity_scope_control>
				control) noexcept;

		std::shared_ptr<detail::sqlite_shm_reader_lifecycle_identity_scope_control> control_;
	};

	/** Move-only pre-native callback permit. It contains no projectable callback identity. */
	class sqlite_shm_reader_callback_identity_permit
	{
	  public:
		~sqlite_shm_reader_callback_identity_permit() noexcept;
		sqlite_shm_reader_callback_identity_permit(
			sqlite_shm_reader_callback_identity_permit&&) noexcept;
		sqlite_shm_reader_callback_identity_permit&
		operator=(sqlite_shm_reader_callback_identity_permit&&) = delete;
		sqlite_shm_reader_callback_identity_permit(
			const sqlite_shm_reader_callback_identity_permit&) = delete;
		sqlite_shm_reader_callback_identity_permit&
		operator=(const sqlite_shm_reader_callback_identity_permit&) = delete;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class detail::sqlite_shm_process_identity_issuer_state;
		friend class sqlite_shm_process_global_identity_issuer;
		explicit sqlite_shm_reader_callback_identity_permit(
			std::shared_ptr<detail::sqlite_shm_process_identity_record_control> control) noexcept;

		std::shared_ptr<detail::sqlite_shm_process_identity_record_control> control_;
	};

	/** Move-only issuer proof whose projection is one existing callback receipt. */
	class sqlite_shm_issued_reader_callback_identity
	{
	  public:
		~sqlite_shm_issued_reader_callback_identity() noexcept;
		sqlite_shm_issued_reader_callback_identity(
			sqlite_shm_issued_reader_callback_identity&&) noexcept;
		sqlite_shm_issued_reader_callback_identity&
		operator=(sqlite_shm_issued_reader_callback_identity&&) = delete;
		sqlite_shm_issued_reader_callback_identity(
			const sqlite_shm_issued_reader_callback_identity&) = delete;
		sqlite_shm_issued_reader_callback_identity&
		operator=(const sqlite_shm_issued_reader_callback_identity&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] const sqlite_shm_callback_execution_receipt& receipt() const noexcept;

	  private:
		friend class detail::sqlite_shm_process_identity_issuer_state;
		friend class sqlite_shm_process_global_identity_issuer;
		sqlite_shm_issued_reader_callback_identity(
			std::shared_ptr<detail::sqlite_shm_process_identity_record_control> control,
			sqlite_shm_callback_execution_receipt receipt) noexcept;

		std::shared_ptr<detail::sqlite_shm_process_identity_record_control> control_;
		sqlite_shm_callback_execution_receipt receipt_;
	};

	/** Move-only issuer proof whose projection is one existing opaque effect identity. */
	class sqlite_shm_issued_reader_effect_identity
	{
	  public:
		~sqlite_shm_issued_reader_effect_identity() noexcept;
		sqlite_shm_issued_reader_effect_identity(sqlite_shm_issued_reader_effect_identity&&) noexcept;
		sqlite_shm_issued_reader_effect_identity&
		operator=(sqlite_shm_issued_reader_effect_identity&&) = delete;
		sqlite_shm_issued_reader_effect_identity(
			const sqlite_shm_issued_reader_effect_identity&) = delete;
		sqlite_shm_issued_reader_effect_identity&
		operator=(const sqlite_shm_issued_reader_effect_identity&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& identity() const noexcept;

	  private:
		friend class detail::sqlite_shm_process_identity_issuer_state;
		friend class sqlite_shm_process_global_identity_issuer;
		sqlite_shm_issued_reader_effect_identity(
			std::shared_ptr<detail::sqlite_shm_process_identity_record_control> control,
			sqlite_backend_opaque_identity identity) noexcept;

		std::shared_ptr<detail::sqlite_shm_process_identity_record_control> control_;
		sqlite_backend_opaque_identity identity_;
	};

	/** Move-only issuer proof for one exact closed reader-session terminal role. */
	class sqlite_shm_issued_reader_session_terminal_identity
	{
	  public:
		~sqlite_shm_issued_reader_session_terminal_identity() noexcept;
		sqlite_shm_issued_reader_session_terminal_identity(
			sqlite_shm_issued_reader_session_terminal_identity&&) noexcept;
		sqlite_shm_issued_reader_session_terminal_identity&
		operator=(sqlite_shm_issued_reader_session_terminal_identity&&) = delete;
		sqlite_shm_issued_reader_session_terminal_identity(
			const sqlite_shm_issued_reader_session_terminal_identity&) = delete;
		sqlite_shm_issued_reader_session_terminal_identity&
		operator=(const sqlite_shm_issued_reader_session_terminal_identity&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& identity() const noexcept;

	  private:
		friend class detail::sqlite_shm_process_identity_issuer_state;
		friend class sqlite_shm_process_global_identity_issuer;
		sqlite_shm_issued_reader_session_terminal_identity(
			std::shared_ptr<detail::sqlite_shm_process_identity_record_control> control,
			sqlite_backend_opaque_identity identity) noexcept;

		std::shared_ptr<detail::sqlite_shm_process_identity_record_control> control_;
		sqlite_backend_opaque_identity identity_;
	};

	/**
	 * Production-inert facade over the one issuer owned by one process registry.
	 *
	 * No API here invokes SQLite or a native callback, and no identity operation acquires the
	 * process registry mutex. A future caller therefore cannot carry this bridge's lock across native
	 * execution.
	 * Public aggregate equality is never validation: every operation checks the private control
	 * provenance, process epoch, registry owner-equivalence, scope, domain, and role. The issuer
	 * retains no ever-issued ledger; nonreuse comes from its single checked no-wrap sequence.
	 * U2a1a mints and validates callback-invocation, native/zero-effect, and session-terminal identity
	 * domains from this one process source. Full session-transaction terminal validation, including
	 * the exact session owner and no-live-lock condition, remains deferred; this facade provides the
	 * identity primitive rather than that semantic validator.
	 */
	class sqlite_shm_process_global_identity_issuer
	{
	  public:
		[[nodiscard]] bool valid() const noexcept;

		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_callback_identity_permit>
		reserve_callback(const sqlite_shm_reader_lifecycle_identity_scope& scope,
					 sqlite_shm_reader_callback_identity_role role,
					 sqlite_backend_opaque_identity thread_identity,
					 std::uint64_t reentrancy_depth);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_issued_reader_callback_identity>
		seal_callback(sqlite_shm_reader_callback_identity_permit& permit,
				  const sqlite_shm_reader_lifecycle_identity_scope& scope,
				  sqlite_shm_reader_callback_identity_role role) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_issued_reader_effect_identity>
		issue_effect(const sqlite_shm_reader_lifecycle_identity_scope& scope,
				 const sqlite_shm_issued_reader_callback_identity& callback,
				 sqlite_shm_reader_effect_identity_role role);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_issued_reader_session_terminal_identity>
		issue_session_terminal(const sqlite_shm_reader_lifecycle_identity_scope& scope,
						 sqlite_shm_reader_session_terminal_identity_role role);

		[[nodiscard]] sqlite_shm_lease_result<void> validate_callback(
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_issued_reader_callback_identity& callback,
			sqlite_shm_reader_callback_identity_role role) const noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void> validate_effect(
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_issued_reader_callback_identity& callback,
			const sqlite_shm_issued_reader_effect_identity& effect,
			sqlite_shm_reader_effect_identity_role role) const noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void> validate_session_terminal(
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_issued_reader_session_terminal_identity& terminal,
			sqlite_shm_reader_session_terminal_identity_role role) const noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void> retire_callback(
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			sqlite_shm_issued_reader_callback_identity& callback,
			sqlite_shm_reader_callback_identity_role role) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void> retire_effect(
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_issued_reader_callback_identity& callback,
			sqlite_shm_issued_reader_effect_identity& effect,
			sqlite_shm_reader_effect_identity_role role) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void> retire_session_terminal(
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			sqlite_shm_issued_reader_session_terminal_identity& terminal,
			sqlite_shm_reader_session_terminal_identity_role role) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void>
		retire_scope(sqlite_shm_reader_lifecycle_identity_scope& scope) noexcept;

	  private:
		friend class sqlite_same_process_shm_mapping_registry;
		friend class sqlite_same_process_shm_registry_test_peer;
		explicit sqlite_shm_process_global_identity_issuer(
			std::weak_ptr<detail::sqlite_shm_process_identity_issuer_state> state,
			std::shared_ptr<std::atomic<std::uint64_t>> process_epoch,
			std::shared_ptr<std::atomic_bool> registry_issuer_owner_latch,
			std::uint64_t expected_process_epoch) noexcept;
		[[nodiscard]] bool current_before_state_lock() const noexcept;
		void set_scope_live_records_for_testing(
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			std::size_t value) noexcept;
		[[nodiscard]] std::size_t scope_live_records_for_testing(
			const sqlite_shm_reader_lifecycle_identity_scope& scope) const noexcept;
		void set_callback_live_children_for_testing(
			const sqlite_shm_issued_reader_callback_identity& callback,
			std::size_t value) noexcept;
		[[nodiscard]] std::size_t callback_live_children_for_testing(
			const sqlite_shm_issued_reader_callback_identity& callback) const noexcept;
		void arm_pause_for_testing(
			sqlite_shm_identity_issuer_pause_point_for_testing point) noexcept;
		[[nodiscard]] bool pause_entered_for_testing(
			sqlite_shm_identity_issuer_pause_point_for_testing point) const noexcept;
		void release_pause_for_testing() noexcept;

		std::weak_ptr<detail::sqlite_shm_process_identity_issuer_state> state_;
		std::shared_ptr<std::atomic<std::uint64_t>> process_epoch_;
		std::shared_ptr<std::atomic_bool> registry_issuer_owner_latch_;
		std::uint64_t expected_process_epoch_{};
	};

} // namespace cxxlens::sdk
