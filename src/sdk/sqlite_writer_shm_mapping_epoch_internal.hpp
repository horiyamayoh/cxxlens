#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "sqlite_same_process_shm_mapping_lease_internal.hpp"

namespace cxxlens::sdk
{
	namespace detail
	{
		struct sqlite_writer_shm_mapping_epoch_liveness;
		struct sqlite_writer_shm_native_lifetime_control;
		class sqlite_writer_shm_mapping_epoch_state;
		class sqlite_writer_shm_generation_epoch_custody;
		class sqlite_shm_mapping_lease_state;
		class sqlite_shm_mapping_registry_state;
	} // namespace detail

	class sqlite_writer_shm_native_lifetime_production_factory;
	class sqlite_writer_shm_mapping_receipt_validator;
	class sqlite_writer_shm_mapping_epoch_receipt;
	class sqlite_shm_writer_member_authority;
	class sqlite_shm_reader_native_ok_projection_reservation;
	class sqlite_shm_writer_reader_borrow_mint_capability;
	class sqlite_writer_shm_generation_epoch_authority;

	struct sqlite_shm_writer_reader_borrow_tokens
	{
		std::uint64_t map_token{};
		std::uint64_t generation{};
		std::uint64_t holder_token{};
	};

	/**
	 * Closed role of one native lifetime retained across a writer mapping epoch.
	 *
	 * These roles are intentionally narrower than a generic filesystem handle. The production
	 * forwarding VFS binds MAIN/WAL lifetimes at xOpen. The epoch port still
	 * requires the separately retained parent directory and SHM native attachment before it can
	 * arm a writer mapping epoch.
	 */
	enum class sqlite_writer_shm_native_lifetime_role : std::uint8_t
	{
		retained_parent,
		main_database,
		write_ahead_log,
		shared_memory_attachment,
	};

	class sqlite_writer_shm_native_lifetime_pin;
	class sqlite_writer_shm_native_lifetime_source;

	/**
	 * Move-only native close/unmap authority paired with one exact lifetime source.
	 *
	 * One source may issue several non-reusable pins for concurrent or later map epochs. Revocation
	 * is monotonic across every pin from that source. It does not release memory owners retained by
	 * pins, so storage may remain safe to inspect while a closed OS handle never regains authority.
	 * The production minter makes this revoker inseparable from the exact native close path and
	 * defers physical storage release while pins retain the owner.
	 */
	class sqlite_writer_shm_native_lifetime_revoker
	{
	  public:
		~sqlite_writer_shm_native_lifetime_revoker() noexcept;
		sqlite_writer_shm_native_lifetime_revoker(
			sqlite_writer_shm_native_lifetime_revoker&&) noexcept;
		sqlite_writer_shm_native_lifetime_revoker&
		operator=(sqlite_writer_shm_native_lifetime_revoker&&) = delete;
		sqlite_writer_shm_native_lifetime_revoker(
			const sqlite_writer_shm_native_lifetime_revoker&) = delete;
		sqlite_writer_shm_native_lifetime_revoker&
		operator=(const sqlite_writer_shm_native_lifetime_revoker&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] bool revoke() noexcept;

	  private:
		friend class sqlite_writer_shm_native_lifetime_production_factory;

		explicit sqlite_writer_shm_native_lifetime_revoker(
			std::shared_ptr<detail::sqlite_writer_shm_native_lifetime_control> control) noexcept;

		std::shared_ptr<detail::sqlite_writer_shm_native_lifetime_control> control_;
	};

	/**
	 * Move-only, close-revocable pin for one exact already-owned native lifetime.
	 *
	 * `native_lifetime_identity` identifies one non-reusable close epoch,
	 * `semantic_receipt` identifies the retained native object/parent/attachment, and
	 * `pin_identity` distinguishes this individual subpin. MAIN/WAL pins additionally carry the
	 * exact native xOpen receipt. Pointer equality and retained memory alone are never authority.
	 */
	class sqlite_writer_shm_native_lifetime_pin
	{
	  public:
		~sqlite_writer_shm_native_lifetime_pin() noexcept;
		sqlite_writer_shm_native_lifetime_pin(sqlite_writer_shm_native_lifetime_pin&&) noexcept;
		sqlite_writer_shm_native_lifetime_pin&
		operator=(sqlite_writer_shm_native_lifetime_pin&&) = delete;
		sqlite_writer_shm_native_lifetime_pin(const sqlite_writer_shm_native_lifetime_pin&) =
			delete;
		sqlite_writer_shm_native_lifetime_pin&
		operator=(const sqlite_writer_shm_native_lifetime_pin&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] sqlite_writer_shm_native_lifetime_role role() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		native_lifetime_identity() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& semantic_receipt() const noexcept;
		[[nodiscard]] const std::optional<sqlite_backend_opaque_identity>&
		native_xopen_receipt() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& pin_identity() const noexcept;

	  private:
		friend class detail::sqlite_writer_shm_mapping_epoch_state;
		friend class sqlite_writer_shm_native_lifetime_source;
		friend class sqlite_writer_shm_native_lifetime_production_factory;
		friend class sqlite_writer_shm_mapping_epoch_port;

		sqlite_writer_shm_native_lifetime_pin(
			std::shared_ptr<detail::sqlite_writer_shm_native_lifetime_control> control,
			std::shared_ptr<void> retained_owner,
			sqlite_backend_opaque_identity pin_identity) noexcept;
		[[nodiscard]] bool bind_epoch_liveness(
			const std::shared_ptr<detail::sqlite_writer_shm_mapping_epoch_liveness>& liveness);

		std::shared_ptr<detail::sqlite_writer_shm_native_lifetime_control> control_;
		std::shared_ptr<void> retained_owner_;
		sqlite_backend_opaque_identity pin_identity_;
	};

	/**
	 * Move-only mint authority for subpins of one exact native close epoch.
	 *
	 * The source enforces non-reuse of pin identities. Minting after close revocation fails closed.
	 * Production binding is restricted to the source-private production factory below.
	 */
	class sqlite_writer_shm_native_lifetime_source
	{
	  public:
		~sqlite_writer_shm_native_lifetime_source() noexcept;
		sqlite_writer_shm_native_lifetime_source(
			sqlite_writer_shm_native_lifetime_source&&) noexcept;
		sqlite_writer_shm_native_lifetime_source&
		operator=(sqlite_writer_shm_native_lifetime_source&&) = delete;
		sqlite_writer_shm_native_lifetime_source(const sqlite_writer_shm_native_lifetime_source&) =
			delete;
		sqlite_writer_shm_native_lifetime_source&
		operator=(const sqlite_writer_shm_native_lifetime_source&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_native_lifetime_pin> mint_pin();

	  private:
		friend class sqlite_writer_shm_native_lifetime_production_factory;

		sqlite_writer_shm_native_lifetime_source(
			std::shared_ptr<detail::sqlite_writer_shm_native_lifetime_control> control,
			const std::shared_ptr<void>& retained_owner) noexcept;

		std::shared_ptr<detail::sqlite_writer_shm_native_lifetime_control> control_;
		std::weak_ptr<void> retained_owner_;
	};

	/**
	 * Source-private production minter for a real native xOpen lifetime.
	 *
	 * The forwarding VFS is the only production caller. It supplies the complete sealed
	 * close-epoch tuple after native xOpen has returned and retains the native node as the
	 * owner. Malformed tuples are rejected before a source is minted.
	 */
	class sqlite_writer_shm_native_lifetime_production_factory final
	{
	  public:
		sqlite_writer_shm_native_lifetime_production_factory() = delete;

		[[nodiscard]] static sqlite_shm_lease_result<
			std::pair<sqlite_writer_shm_native_lifetime_revoker,
					  sqlite_writer_shm_native_lifetime_source>>
		create_source(sqlite_writer_shm_native_lifetime_role role,
					  sqlite_backend_opaque_identity native_lifetime_identity,
					  sqlite_backend_opaque_identity semantic_receipt,
					  std::optional<sqlite_backend_opaque_identity> native_xopen_receipt,
					  const std::shared_ptr<void>& retained_owner) noexcept;
	};

	/** Only the closed zero/one/multiple-or-overflow census is representable. */
	enum class sqlite_writer_shm_bounded_count : std::uint8_t
	{
		zero,
		one,
		multiple_or_overflow,
	};

	/** SHM entry shapes which can participate in the accepted writer-map transition matrix. */
	enum class sqlite_writer_shm_entry_state : std::uint8_t
	{
		absent,
		direct_regular,
	};

	/**
	 * One duplicate-target-open-free, parent-relative SHM stat observation.
	 *
	 * The filesystem and mount receipts remain explicit even for an absent leaf. Direct regular
	 * state requires both object and directory-entry receipts; absence requires neither.
	 */
	struct sqlite_writer_shm_stat_census
	{
		sqlite_writer_shm_entry_state state{sqlite_writer_shm_entry_state::absent};
		sqlite_backend_opaque_identity parent_namespace_identity;
		sqlite_backend_opaque_identity filesystem_profile;
		sqlite_backend_opaque_identity mount_identity;
		std::optional<sqlite_backend_opaque_identity> object_identity;
		std::optional<sqlite_backend_opaque_identity> directory_entry_identity;
		std::uint64_t byte_count{};

		[[nodiscard]] bool operator==(const sqlite_writer_shm_stat_census&) const = default;
	};

	/**
	 * Bounded namespace transcript from watch arm through the post-map stat observation.
	 *
	 * Endpoint equality cannot erase `replacement_or_aba`, watch loss, queue overflow, or any
	 * other relevant event.
	 */
	struct sqlite_writer_shm_namespace_event_census
	{
		sqlite_backend_opaque_identity watch_epoch;
		sqlite_backend_opaque_identity expected_shm_leaf;
		sqlite_writer_shm_bounded_count expected_leaf_create{sqlite_writer_shm_bounded_count::zero};
		sqlite_writer_shm_bounded_count other_create{sqlite_writer_shm_bounded_count::zero};
		sqlite_writer_shm_bounded_count delete_event{sqlite_writer_shm_bounded_count::zero};
		sqlite_writer_shm_bounded_count move_event{sqlite_writer_shm_bounded_count::zero};
		sqlite_writer_shm_bounded_count other_relevant_event{sqlite_writer_shm_bounded_count::zero};
		bool trusted_stat_watch_profile{};
		bool watch_lost{};
		bool queue_overflow{};
		bool census_overflow{};
		bool replacement_or_aba{};

		[[nodiscard]] bool
		operator==(const sqlite_writer_shm_namespace_event_census&) const = default;
	};

	/**
	 * Exact wrapper-observable effect transcript for one writer-map callback.
	 *
	 * No field claims unobservable syscall provenance. A later closed validator relates this
	 * census to the exact extend pair and pre/post stat transition.
	 */
	struct sqlite_writer_shm_effect_census
	{
		sqlite_backend_opaque_identity sqlite_source_id;
		sqlite_backend_opaque_identity callback_transcript;
		sqlite_backend_opaque_identity wal_write_lock_receipt;
		sqlite_backend_opaque_identity effect_gate_receipt;
		sqlite_backend_opaque_identity effect_receipt;
		sqlite_writer_shm_bounded_count create_count{sqlite_writer_shm_bounded_count::zero};
		sqlite_writer_shm_bounded_count initialize_count{sqlite_writer_shm_bounded_count::zero};
		sqlite_writer_shm_bounded_count truncate_count{sqlite_writer_shm_bounded_count::zero};
		sqlite_writer_shm_bounded_count extend_count{sqlite_writer_shm_bounded_count::zero};
		sqlite_writer_shm_bounded_count delete_count{sqlite_writer_shm_bounded_count::zero};
		sqlite_writer_shm_bounded_count resize_count{sqlite_writer_shm_bounded_count::zero};
		std::optional<std::uint64_t> size_before;
		std::optional<std::uint64_t> size_after;
		std::optional<std::uint64_t> requested_range_end;
		bool complete{};
		bool result_confirmed_success{};
		bool outcome_unknown{};
		bool census_overflow{};

		[[nodiscard]] bool operator==(const sqlite_writer_shm_effect_census&) const = default;
	};

	/** Port classification which must independently agree with every typed census. */
	enum class sqlite_writer_shm_observed_transition : std::uint8_t
	{
		preexisting_unchanged,
		preexisting_preallocated,
		preexisting_grown,
		absent_created,
		unclassified,
	};

	struct sqlite_writer_shm_mapping_epoch_post_observation
	{
		sqlite_writer_shm_stat_census stat;
		sqlite_writer_shm_namespace_event_census namespace_events;
		sqlite_writer_shm_effect_census effects;
		sqlite_writer_shm_observed_transition transition{
			sqlite_writer_shm_observed_transition::unclassified};

		[[nodiscard]] bool
		operator==(const sqlite_writer_shm_mapping_epoch_post_observation&) const = default;
	};

	/**
	 * Immutable semantic binding captured before the namespace watch and pre-stat.
	 *
	 * Native lifetime pin identities are derived by the closed port and are not caller-supplied
	 * fields in this value.
	 */
	struct sqlite_writer_shm_mapping_epoch_binding
	{
		sqlite_shm_writer_map_request map_request;
		int delegated_extend{};
		sqlite_backend_opaque_identity expected_shm_leaf;
		sqlite_backend_opaque_identity retained_parent_receipt;
		sqlite_backend_opaque_identity wal_native_file_receipt;
		sqlite_backend_opaque_identity wal_xopen_receipt;
		sqlite_backend_opaque_identity shm_native_attachment_receipt;
		/** Exact target namespace epoch retained by the production writer route. */
		std::optional<sqlite_backend_opaque_identity> target_namespace_epoch_identity;

		[[nodiscard]] bool
		operator==(const sqlite_writer_shm_mapping_epoch_binding&) const = default;
	};

	/** Move-only input whose native lifetime pins become the epoch arm's sole strong authority. */
	struct sqlite_writer_shm_mapping_epoch_request
	{
		sqlite_writer_shm_mapping_epoch_binding binding;
		sqlite_writer_shm_native_lifetime_pin retained_parent;
		sqlite_writer_shm_native_lifetime_pin main_native_file;
		sqlite_writer_shm_native_lifetime_pin wal_native_file;
		sqlite_writer_shm_native_lifetime_pin shm_native_attachment;
	};

	/**
	 * Stateful observation half created by a platform-specific epoch port.
	 *
	 * The implementation must already have armed its retained-parent ancestry watch before
	 * returning the pre-stat preparation. This interface performs no native map or cleanup.
	 */
	class sqlite_writer_shm_mapping_epoch_observation_port
	{
	  public:
		virtual ~sqlite_writer_shm_mapping_epoch_observation_port() = default;

		[[nodiscard]] virtual sqlite_shm_lease_result<
			sqlite_writer_shm_mapping_epoch_post_observation>
		observe_after_native_map(const sqlite_writer_shm_mapping_epoch_binding& binding,
								 const sqlite_writer_shm_stat_census& pre_stat,
								 const volatile void* native_mapping) = 0;
	};

	struct sqlite_writer_shm_mapping_epoch_preparation
	{
		sqlite_backend_opaque_identity epoch_identity;
		sqlite_backend_opaque_identity watch_arm_receipt;
		sqlite_writer_shm_stat_census pre_stat;
		std::shared_ptr<sqlite_writer_shm_mapping_epoch_observation_port> observer;
		// Source-private custody only. It does not mint until a lease reservation binds the
		// exact holder/generation/map/callback/page coordinates in stage two.
		std::optional<sqlite_source_shm_target_namespace_epoch_borrow_minter> borrow_minter;
	};

	/**
	 * Generation-owned source custody, distinct from a writer member's local census arm.
	 * It is intentionally opaque: only the lease may bind a reader mint to exact
	 * generation/map/holder coordinates. G1 introduces the custody split; G2 wires
	 * installation and joining at the writer lifecycle boundaries.
	 */
	class sqlite_writer_shm_generation_epoch_authority final
	{
	  public:
		~sqlite_writer_shm_generation_epoch_authority() noexcept;
		sqlite_writer_shm_generation_epoch_authority(
			const sqlite_writer_shm_generation_epoch_authority&) noexcept = default;
		sqlite_writer_shm_generation_epoch_authority(
			sqlite_writer_shm_generation_epoch_authority&&) noexcept;
		sqlite_writer_shm_generation_epoch_authority&
		operator=(const sqlite_writer_shm_generation_epoch_authority&) noexcept = default;
		sqlite_writer_shm_generation_epoch_authority&
		operator=(sqlite_writer_shm_generation_epoch_authority&&) noexcept = default;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_shm_writer_member_authority;
		friend class sqlite_writer_shm_mapping_epoch_arm;
		explicit sqlite_writer_shm_generation_epoch_authority(
			std::shared_ptr<detail::sqlite_writer_shm_mapping_epoch_state> state,
			std::shared_ptr<detail::sqlite_writer_shm_generation_epoch_custody> custody) noexcept;
		[[nodiscard]] bool target_identity_matches(
			const sqlite_shm_reader_attachment_target_identity& target,
			const sqlite_shm_verified_writer_post_map_receipt& receipt) const noexcept;
		[[nodiscard]] bool matches_canonical_target(
			const sqlite_shm_reader_attachment_target_identity& target) const noexcept;
		[[nodiscard]] bool canonical_target_bound() const noexcept;
		[[nodiscard]] bool reader_borrow_capable() const noexcept;
		[[nodiscard]] bool
		same_custody_as(const sqlite_writer_shm_generation_epoch_authority& other) const noexcept;
		[[nodiscard]] bool
		bind_canonical_target(const sqlite_shm_verified_writer_post_map_receipt& receipt) noexcept;
		[[nodiscard]] bool bind_generic_custody() noexcept;
		[[nodiscard]] result<sqlite_shm_writer_reader_borrow_mint_capability>
		reserve_reader_borrow_mint(std::uint64_t map_token,
								   std::uint64_t generation,
								   std::uint64_t holder_token) const;

		std::shared_ptr<detail::sqlite_writer_shm_mapping_epoch_state> state_;
		std::shared_ptr<detail::sqlite_writer_shm_generation_epoch_custody> custody_;
	};

	class sqlite_writer_shm_mapping_epoch_arm
	{
	  public:
		~sqlite_writer_shm_mapping_epoch_arm() noexcept;
		sqlite_writer_shm_mapping_epoch_arm(sqlite_writer_shm_mapping_epoch_arm&&) noexcept;
		sqlite_writer_shm_mapping_epoch_arm&
		operator=(sqlite_writer_shm_mapping_epoch_arm&&) = delete;
		sqlite_writer_shm_mapping_epoch_arm(const sqlite_writer_shm_mapping_epoch_arm&) = delete;
		sqlite_writer_shm_mapping_epoch_arm&
		operator=(const sqlite_writer_shm_mapping_epoch_arm&) = delete;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class sqlite_shm_writer_member_authority;
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_writer_shm_mapping_epoch_port;
		friend class sqlite_writer_shm_mapping_receipt_validator;

		explicit sqlite_writer_shm_mapping_epoch_arm(
			std::shared_ptr<detail::sqlite_writer_shm_mapping_epoch_state> state) noexcept;
		[[nodiscard]] bool
		valid_for_predelegation(const sqlite_shm_writer_map_request& request) const noexcept;
		[[nodiscard]] bool
		retains_exact_lifetimes(const sqlite_shm_writer_map_request& request) const noexcept;
		[[nodiscard]] bool target_identity_matches(
			const sqlite_shm_reader_attachment_target_identity& target,
			const sqlite_shm_verified_writer_post_map_receipt& receipt) const noexcept;
		[[nodiscard]] bool matches_validated_receipt(
			const sqlite_shm_verified_writer_post_map_receipt& receipt) const noexcept;
		[[nodiscard]] bool retains_exact_validated_receipt(
			const sqlite_shm_verified_writer_post_map_receipt& receipt) const noexcept;
		[[nodiscard]] bool attachment_cohort_compatible_with(
			const sqlite_writer_shm_mapping_epoch_arm& other) const noexcept;
		[[nodiscard]] result<sqlite_shm_writer_reader_borrow_mint_capability>
		reserve_reader_borrow_mint(std::uint64_t map_token,
								   std::uint64_t generation,
								   std::uint64_t holder_token) const;
		[[nodiscard]] sqlite_writer_shm_generation_epoch_authority
		make_generation_authority() const noexcept;

		std::shared_ptr<detail::sqlite_writer_shm_mapping_epoch_state> state_;
	};

	/** Opaque one-shot bridge from a selected writer holder to a lease reservation. */
	class sqlite_shm_writer_reader_borrow_mint_capability final
	{
	  public:
		~sqlite_shm_writer_reader_borrow_mint_capability() noexcept;
		sqlite_shm_writer_reader_borrow_mint_capability(
			sqlite_shm_writer_reader_borrow_mint_capability&&) noexcept;
		sqlite_shm_writer_reader_borrow_mint_capability&
		operator=(sqlite_shm_writer_reader_borrow_mint_capability&&) = delete;
		sqlite_shm_writer_reader_borrow_mint_capability(
			const sqlite_shm_writer_reader_borrow_mint_capability&) = delete;
		sqlite_shm_writer_reader_borrow_mint_capability&
		operator=(const sqlite_shm_writer_reader_borrow_mint_capability&) = delete;

	  private:
		friend class sqlite_shm_writer_member_authority;
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_shm_reader_native_ok_projection_reservation;
		friend class sqlite_same_process_shm_mapping_lease_coordinator;
		friend class sqlite_writer_shm_mapping_epoch_arm;
		friend class sqlite_writer_shm_generation_epoch_authority;
		explicit sqlite_shm_writer_reader_borrow_mint_capability(
			std::shared_ptr<detail::sqlite_writer_shm_generation_epoch_custody> custody,
			sqlite_shm_writer_reader_borrow_tokens tokens) noexcept;
		[[nodiscard]] result<sqlite_source_shm_target_namespace_epoch_reader_borrow>
		mint(const sqlite_shm_reader_native_ok_projection_reservation& reservation);
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_writer_shm_generation_epoch_custody> custody_;
		std::uint64_t map_token_{};
		std::uint64_t generation_{};
		std::uint64_t holder_token_{};
	};

	class sqlite_writer_shm_mapping_epoch_observer
	{
	  public:
		~sqlite_writer_shm_mapping_epoch_observer() noexcept;
		sqlite_writer_shm_mapping_epoch_observer(
			sqlite_writer_shm_mapping_epoch_observer&&) noexcept;
		sqlite_writer_shm_mapping_epoch_observer&
		operator=(sqlite_writer_shm_mapping_epoch_observer&&) = delete;
		sqlite_writer_shm_mapping_epoch_observer(const sqlite_writer_shm_mapping_epoch_observer&) =
			delete;
		sqlite_writer_shm_mapping_epoch_observer&
		operator=(const sqlite_writer_shm_mapping_epoch_observer&) = delete;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class sqlite_writer_shm_mapping_epoch_port;
		friend class sqlite_writer_shm_mapping_receipt_validator;
		friend sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_receipt>
		seal_sqlite_writer_shm_mapping_epoch(sqlite_writer_shm_mapping_epoch_observer& observer,
											 const volatile void* native_mapping) noexcept;

		explicit sqlite_writer_shm_mapping_epoch_observer(
			std::weak_ptr<detail::sqlite_writer_shm_mapping_epoch_state> state) noexcept;

		std::weak_ptr<detail::sqlite_writer_shm_mapping_epoch_state> state_;
	};

	/**
	 * Copyable audit-only epoch receipt.
	 *
	 * Its weak state binding allows the closed mapping validator to prove that the corresponding
	 * strong arm is still retained. Copying this value never extends native lifetime or grants
	 * mapping/pending/holder authority.
	 */
	class sqlite_writer_shm_mapping_epoch_receipt
	{
	  public:
		[[nodiscard]] const sqlite_backend_opaque_identity& epoch_identity() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& watch_arm_receipt() const noexcept;
		[[nodiscard]] const sqlite_writer_shm_mapping_epoch_binding& binding() const noexcept;
		[[nodiscard]] const sqlite_writer_shm_stat_census& pre_stat() const noexcept;
		[[nodiscard]] const sqlite_writer_shm_mapping_epoch_post_observation&
		post_observation() const noexcept;
		[[nodiscard]] const volatile void* native_mapping() const noexcept;

	  private:
		friend class detail::sqlite_writer_shm_mapping_epoch_state;
		friend class sqlite_writer_shm_mapping_receipt_validator;
		friend class sqlite_writer_shm_mapping_epoch_arm;

		sqlite_writer_shm_mapping_epoch_receipt(
			std::weak_ptr<detail::sqlite_writer_shm_mapping_epoch_state> state,
			std::uint64_t seal_sequence,
			sqlite_backend_opaque_identity epoch_identity,
			sqlite_backend_opaque_identity watch_arm_receipt,
			sqlite_writer_shm_mapping_epoch_binding binding,
			sqlite_writer_shm_stat_census pre_stat,
			sqlite_writer_shm_mapping_epoch_post_observation post_observation,
			const volatile void* native_mapping);
		[[nodiscard]] sqlite_shm_lease_result<
			std::shared_ptr<detail::sqlite_writer_shm_mapping_epoch_state>>
		begin_authoritative_validation() const noexcept;
		[[nodiscard]] bool authoritative_validation_still_live(
			const std::shared_ptr<detail::sqlite_writer_shm_mapping_epoch_state>& state)
			const noexcept;

		std::weak_ptr<detail::sqlite_writer_shm_mapping_epoch_state> state_;
		std::uint64_t seal_sequence_{};
		sqlite_backend_opaque_identity epoch_identity_;
		sqlite_backend_opaque_identity watch_arm_receipt_;
		sqlite_writer_shm_mapping_epoch_binding binding_;
		sqlite_writer_shm_stat_census pre_stat_;
		sqlite_writer_shm_mapping_epoch_post_observation post_observation_;
		const volatile void* native_mapping_{};
	};

	struct sqlite_writer_shm_mapping_epoch_activation
	{
	  public:
		sqlite_writer_shm_mapping_epoch_activation(
			sqlite_writer_shm_mapping_epoch_activation&&) noexcept;
		sqlite_writer_shm_mapping_epoch_activation&
		operator=(sqlite_writer_shm_mapping_epoch_activation&&) = delete;
		sqlite_writer_shm_mapping_epoch_activation(
			const sqlite_writer_shm_mapping_epoch_activation&) = delete;
		sqlite_writer_shm_mapping_epoch_activation&
		operator=(const sqlite_writer_shm_mapping_epoch_activation&) = delete;

		[[nodiscard]] sqlite_writer_shm_mapping_epoch_arm take_arm() noexcept;
		[[nodiscard]] sqlite_writer_shm_mapping_epoch_observer take_observer() noexcept;

	  private:
		friend class sqlite_writer_shm_mapping_epoch_port;

		sqlite_writer_shm_mapping_epoch_activation(
			sqlite_writer_shm_mapping_epoch_arm arm,
			sqlite_writer_shm_mapping_epoch_observer observer) noexcept;

		sqlite_writer_shm_mapping_epoch_arm arm_;
		sqlite_writer_shm_mapping_epoch_observer observer_;
	};

	/**
	 * Closed platform boundary for watch-before-pre-stat writer mapping epochs.
	 *
	 * The generic boundary has no native-VFS activation or status projection. The
	 * source-private retained-namespace implementation below supplies exact pre-existing
	 * `{0, 0}` and same-process `{1, 1}` preallocated/growth/create observations. It never opens a
	 * duplicate target descriptor while SQLite locks may be live.
	 */
	class sqlite_writer_shm_mapping_epoch_port
	{
	  public:
		virtual ~sqlite_writer_shm_mapping_epoch_port() = default;

		[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_activation>
		arm(sqlite_writer_shm_mapping_epoch_request request) noexcept;

	  protected:
		[[nodiscard]] virtual sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_preparation>
		arm_watch_before_pre_stat(const sqlite_writer_shm_mapping_epoch_request& request) = 0;
	};

	/**
	 * Exact source-private inputs for the retained-namespace writer epoch port.
	 *
	 * This binding deliberately contains no pathname or native descriptor. The target epoch and
	 * its held SHM object are the only observation authority. The remaining receipts are supplied
	 * by their owning writer/callback layers; this port never infers them from a pointer or from a
	 * successful native SQLite return code.
	 */
	struct sqlite_writer_shm_mapping_epoch_platform_binding
	{
		std::shared_ptr<sqlite_source_shm_target_namespace_epoch> target_namespace_epoch;
		sqlite_backend_opaque_identity parent_namespace_identity;
		sqlite_backend_opaque_identity sqlite_source_id;
		sqlite_backend_opaque_identity wal_write_lock_receipt;
		sqlite_backend_opaque_identity effect_gate_receipt;
		sqlite_backend_opaque_identity effect_receipt;
		/** Filesystem/mount proof inherited from the retained main/parent for an absent SHM leaf.
		 */
		std::optional<sqlite_backend_opaque_identity> absent_filesystem_profile;
		std::optional<sqlite_backend_opaque_identity> absent_mount_identity;
	};

	/**
	 * Source-private Linux-compatible slice for a writer epoch over an already-held regular SHM
	 * entry or an absent leaf whose parent filesystem/mount proof is retained by the caller. It
	 * accepts only `{0,0}` and `{1,1}`; mixed extend pairs fail closed.
	 */
	class sqlite_retained_namespace_writer_shm_mapping_epoch_port final
		: public sqlite_writer_shm_mapping_epoch_port
	{
	  public:
		explicit sqlite_retained_namespace_writer_shm_mapping_epoch_port(
			sqlite_writer_shm_mapping_epoch_platform_binding binding) noexcept;
		~sqlite_retained_namespace_writer_shm_mapping_epoch_port() override = default;
		sqlite_retained_namespace_writer_shm_mapping_epoch_port(
			const sqlite_retained_namespace_writer_shm_mapping_epoch_port&) = delete;
		sqlite_retained_namespace_writer_shm_mapping_epoch_port&
		operator=(const sqlite_retained_namespace_writer_shm_mapping_epoch_port&) = delete;
		sqlite_retained_namespace_writer_shm_mapping_epoch_port(
			sqlite_retained_namespace_writer_shm_mapping_epoch_port&&) = delete;
		sqlite_retained_namespace_writer_shm_mapping_epoch_port&
		operator=(sqlite_retained_namespace_writer_shm_mapping_epoch_port&&) = delete;

	  protected:
		[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_preparation>
		arm_watch_before_pre_stat(const sqlite_writer_shm_mapping_epoch_request& request) override;

	  private:
		sqlite_writer_shm_mapping_epoch_platform_binding binding_;
		std::atomic<std::uint64_t> next_arm_sequence_{1U};
	};

	/**
	 * One-shot post-native observation. The arm must remain strongly owned until exact cleanup.
	 */
	[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_receipt>
	seal_sqlite_writer_shm_mapping_epoch(sqlite_writer_shm_mapping_epoch_observer& observer,
										 const volatile void* native_mapping) noexcept;
} // namespace cxxlens::sdk
