#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "sqlite_same_process_shm_mapping_lease_internal.hpp"

namespace cxxlens::sdk
{
	namespace detail
	{
		struct sqlite_shm_registry_process_owner_seal;
		struct sqlite_shm_registry_runtime_owner_box;
		class sqlite_shm_mapping_registry_state;
		enum class sqlite_shm_registry_counter_for_testing : std::uint8_t
		{
			alias_token,
			family_epoch,
			family_pin_token,
			activity_token,
		};
	} // namespace detail

	class sqlite_same_process_shm_registry_test_peer;

	/**
	 * One non-replayable, exact-epoch ownership receipt for the process-global registry and
	 * generation source.
	 *
	 * There is intentionally no production minter in this checkpoint. The future qualified process
	 * port must mint exactly one owner for one non-reusable process instance.
	 */
	class sqlite_shm_registry_process_owner
	{
	  public:
		~sqlite_shm_registry_process_owner() noexcept;
		sqlite_shm_registry_process_owner(sqlite_shm_registry_process_owner&&) noexcept;
		sqlite_shm_registry_process_owner& operator=(sqlite_shm_registry_process_owner&&) = delete;
		sqlite_shm_registry_process_owner(const sqlite_shm_registry_process_owner&) = delete;
		sqlite_shm_registry_process_owner&
		operator=(const sqlite_shm_registry_process_owner&) = delete;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class sqlite_same_process_shm_mapping_registry;
		friend class sqlite_same_process_shm_registry_test_peer;

		explicit sqlite_shm_registry_process_owner(sqlite_backend_opaque_identity process_instance);
		sqlite_shm_registry_process_owner(
			sqlite_backend_opaque_identity process_instance,
			std::shared_ptr<detail::sqlite_shm_registry_process_owner_seal> seal,
			std::uint64_t process_epoch) noexcept;

		sqlite_backend_opaque_identity process_instance_;
		std::shared_ptr<detail::sqlite_shm_registry_process_owner_seal> seal_;
		std::uint64_t process_epoch_{};
	};

	/**
	 * Closed, alias-distinct, move-only runtime lifetime pin.
	 *
	 * The underlying owner is deliberately inaccessible. Its exact semantic and pin identities are
	 * bound into registration/unregistration receipts, while neither identity partitions the shared
	 * runtime/VFS family key. The owner is also bound to the process-owner seal and epoch before it
	 * can enter an alias binding. Destruction releases the owner only in that exact live process
	 * epoch; a stale child intentionally leaks the opaque owner box rather than running an
	 * inherited runtime, mutex, or native-resource destructor.
	 */
	class sqlite_shm_registry_runtime_lifetime_pin
	{
	  public:
		~sqlite_shm_registry_runtime_lifetime_pin() noexcept;
		sqlite_shm_registry_runtime_lifetime_pin(
			sqlite_shm_registry_runtime_lifetime_pin&&) noexcept;
		sqlite_shm_registry_runtime_lifetime_pin&
		operator=(sqlite_shm_registry_runtime_lifetime_pin&&) = delete;
		sqlite_shm_registry_runtime_lifetime_pin(const sqlite_shm_registry_runtime_lifetime_pin&) =
			delete;
		sqlite_shm_registry_runtime_lifetime_pin&
		operator=(const sqlite_shm_registry_runtime_lifetime_pin&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& identity() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& pin_identity() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_shm_registry_alias_binding;
		friend class sqlite_same_process_shm_mapping_registry;

		sqlite_shm_registry_runtime_lifetime_pin(
			sqlite_backend_opaque_identity process_instance,
			std::shared_ptr<detail::sqlite_shm_registry_process_owner_seal> process_seal,
			std::uint64_t process_epoch,
			sqlite_backend_opaque_identity identity,
			sqlite_backend_opaque_identity pin_identity,
			std::weak_ptr<void> owner_control,
			std::shared_ptr<detail::sqlite_shm_registry_runtime_owner_box> owner_box);

		sqlite_backend_opaque_identity process_instance_;
		sqlite_backend_opaque_identity identity_;
		sqlite_backend_opaque_identity pin_identity_;
		std::shared_ptr<detail::sqlite_shm_registry_process_owner_seal> process_seal_;
		std::uint64_t process_epoch_{};
		std::weak_ptr<void> owner_control_;
		std::shared_ptr<detail::sqlite_shm_registry_runtime_owner_box> owner_box_;
	};

	/**
	 * One alias-local binding into a shared SQLite runtime/VFS cohort.
	 *
	 * `runtime_lifetime_identity` and `alias_lifetime` are intentionally not part of a family
	 * lookup key. Distinct owned forwarding aliases retain distinct move-only pins while sharing
	 * the exact process/runtime/VFS/file-family coordinator selected by
	 * `sqlite_shm_lease_family_binding`.
	 */
	class sqlite_shm_registry_alias_binding
	{
	  public:
		~sqlite_shm_registry_alias_binding() noexcept;
		sqlite_shm_registry_alias_binding(sqlite_shm_registry_alias_binding&&) noexcept;
		sqlite_shm_registry_alias_binding& operator=(sqlite_shm_registry_alias_binding&&) = delete;
		sqlite_shm_registry_alias_binding(const sqlite_shm_registry_alias_binding&) = delete;
		sqlite_shm_registry_alias_binding&
		operator=(const sqlite_shm_registry_alias_binding&) = delete;

		[[nodiscard]] const sqlite_backend_opaque_identity& process_instance() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		shared_runtime_vfs_cohort() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& alias_lifetime() const noexcept;
		[[nodiscard]] const sqlite_shm_registry_runtime_lifetime_pin&
		runtime_lifetime() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_same_process_shm_registry_test_peer;

		sqlite_shm_registry_alias_binding(
			sqlite_backend_opaque_identity process_instance,
			sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
			sqlite_backend_opaque_identity alias_lifetime,
			sqlite_shm_registry_runtime_lifetime_pin runtime_lifetime);

		sqlite_backend_opaque_identity process_instance_;
		sqlite_backend_opaque_identity shared_runtime_vfs_cohort_;
		sqlite_backend_opaque_identity alias_lifetime_;
		sqlite_shm_registry_runtime_lifetime_pin runtime_lifetime_;
	};

	/**
	 * Closed proof that one exact alias registration completed and remained discoverable.
	 *
	 * This production-inert checkpoint has no producer. Only its focused test peer can mint the
	 * value; the future VFS validator must be added explicitly before any production binding.
	 */
	class sqlite_shm_verified_alias_registration_receipt
	{
	  public:
		[[nodiscard]] const sqlite_backend_opaque_identity& process_instance() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		shared_runtime_vfs_cohort() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& alias_lifetime() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		runtime_lifetime_identity() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		runtime_lifetime_pin_identity() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& registration_epoch() const noexcept;

	  private:
		friend class sqlite_same_process_shm_registry_test_peer;

		sqlite_shm_verified_alias_registration_receipt(
			sqlite_backend_opaque_identity process_instance,
			sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
			sqlite_backend_opaque_identity alias_lifetime,
			sqlite_backend_opaque_identity runtime_lifetime_identity,
			sqlite_backend_opaque_identity runtime_lifetime_pin_identity,
			sqlite_backend_opaque_identity registration_epoch);

		sqlite_backend_opaque_identity process_instance_;
		sqlite_backend_opaque_identity shared_runtime_vfs_cohort_;
		sqlite_backend_opaque_identity alias_lifetime_;
		sqlite_backend_opaque_identity runtime_lifetime_identity_;
		sqlite_backend_opaque_identity runtime_lifetime_pin_identity_;
		sqlite_backend_opaque_identity registration_epoch_;
	};

	/**
	 * Closed proof that the exact registered alias was unregistered and is no longer discoverable.
	 */
	class sqlite_shm_verified_alias_unregistration_receipt
	{
	  public:
		[[nodiscard]] const sqlite_backend_opaque_identity& process_instance() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		shared_runtime_vfs_cohort() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& alias_lifetime() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		runtime_lifetime_identity() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		runtime_lifetime_pin_identity() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& registration_epoch() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& unregistration_epoch() const noexcept;

	  private:
		friend class sqlite_same_process_shm_registry_test_peer;

		sqlite_shm_verified_alias_unregistration_receipt(
			sqlite_backend_opaque_identity process_instance,
			sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
			sqlite_backend_opaque_identity alias_lifetime,
			sqlite_backend_opaque_identity runtime_lifetime_identity,
			sqlite_backend_opaque_identity runtime_lifetime_pin_identity,
			sqlite_backend_opaque_identity registration_epoch,
			sqlite_backend_opaque_identity unregistration_epoch);

		sqlite_backend_opaque_identity process_instance_;
		sqlite_backend_opaque_identity shared_runtime_vfs_cohort_;
		sqlite_backend_opaque_identity alias_lifetime_;
		sqlite_backend_opaque_identity runtime_lifetime_identity_;
		sqlite_backend_opaque_identity runtime_lifetime_pin_identity_;
		sqlite_backend_opaque_identity registration_epoch_;
		sqlite_backend_opaque_identity unregistration_epoch_;
	};

	enum class sqlite_shm_registry_alias_phase : std::uint8_t
	{
		reserved,
		registering,
		registered,
		unregistering,
		detached,
		quarantined,
	};

	enum class sqlite_shm_registry_family_phase : std::uint8_t
	{
		active,
		retired,
		quarantined,
	};

	struct sqlite_shm_mapping_registry_snapshot
	{
		std::uint64_t process_epoch{};
		std::size_t cohort_count{};
		std::size_t alias_record_count{};
		std::size_t reserved_alias_count{};
		std::size_t registering_alias_count{};
		std::size_t registered_alias_count{};
		std::size_t unregistering_alias_count{};
		std::size_t detached_alias_tombstone_count{};
		std::size_t quarantined_alias_count{};
		std::size_t family_record_count{};
		std::size_t active_family_count{};
		std::size_t retired_family_tombstone_count{};
		std::size_t quarantined_family_count{};
		std::size_t active_family_pin_count{};
		std::size_t active_activity_pin_count{};
		std::size_t duplicate_rejection_count{};
		std::size_t cross_binding_rejection_count{};
		std::size_t ambiguous_lookup_count{};
		std::size_t generation_source_count{};
		bool process_live{};
		bool registry_quarantined{};
	};

	struct sqlite_shm_mapping_registry_family_snapshot
	{
		std::size_t exact_active_match_count{};
		std::size_t exact_retired_match_count{};
		std::size_t exact_quarantined_match_count{};
		/**
		 * Singleton detail is populated only when `exact_active_match_count == 1`.
		 *
		 * Zero or multiple active matches, and any active/quarantined mixture, leave every field
		 * below at its default and keep the coordinator invisible. Ambiguous mixtures quarantine
		 * the registry. No match is selected.
		 */
		std::uint64_t entry_epoch{};
		std::size_t alias_pin_count{};
		std::size_t activity_pin_count{};
		sqlite_shm_registry_family_phase phase{sqlite_shm_registry_family_phase::retired};
		sqlite_shm_mapping_lease_snapshot coordinator;
		bool coordinator_present{};
		bool lookup_visible{};
	};

	class sqlite_shm_registry_alias_pin
	{
	  public:
		~sqlite_shm_registry_alias_pin() noexcept;
		sqlite_shm_registry_alias_pin(sqlite_shm_registry_alias_pin&&) noexcept;
		sqlite_shm_registry_alias_pin& operator=(sqlite_shm_registry_alias_pin&&) = delete;
		sqlite_shm_registry_alias_pin(const sqlite_shm_registry_alias_pin&) = delete;
		sqlite_shm_registry_alias_pin& operator=(const sqlite_shm_registry_alias_pin&) = delete;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_same_process_shm_mapping_registry;

		struct coordinates
		{
			std::uint64_t process_epoch{};
			std::uint64_t token{};
		};

		sqlite_shm_registry_alias_pin(
			std::shared_ptr<detail::sqlite_shm_mapping_registry_state> state,
			coordinates value) noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_registry_state> state_;
		std::uint64_t process_epoch_{};
		std::uint64_t token_{};
	};

	class sqlite_shm_registry_family_pin
	{
	  public:
		~sqlite_shm_registry_family_pin() noexcept;
		sqlite_shm_registry_family_pin(sqlite_shm_registry_family_pin&&) noexcept;
		sqlite_shm_registry_family_pin& operator=(sqlite_shm_registry_family_pin&&) = delete;
		sqlite_shm_registry_family_pin(const sqlite_shm_registry_family_pin&) = delete;
		sqlite_shm_registry_family_pin& operator=(const sqlite_shm_registry_family_pin&) = delete;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_same_process_shm_mapping_registry;

		struct coordinates
		{
			std::uint64_t process_epoch{};
			std::uint64_t alias_token{};
			std::uint64_t family_epoch{};
			std::uint64_t pin_token{};
		};

		sqlite_shm_registry_family_pin(
			std::shared_ptr<detail::sqlite_shm_mapping_registry_state> state,
			coordinates value) noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_registry_state> state_;
		std::uint64_t process_epoch_{};
		std::uint64_t alias_token_{};
		std::uint64_t family_epoch_{};
		std::uint64_t pin_token_{};
	};

	class sqlite_shm_registry_activity_pin
	{
	  public:
		~sqlite_shm_registry_activity_pin() noexcept;
		sqlite_shm_registry_activity_pin(sqlite_shm_registry_activity_pin&&) noexcept;
		sqlite_shm_registry_activity_pin& operator=(sqlite_shm_registry_activity_pin&&) = delete;
		sqlite_shm_registry_activity_pin(const sqlite_shm_registry_activity_pin&) = delete;
		sqlite_shm_registry_activity_pin&
		operator=(const sqlite_shm_registry_activity_pin&) = delete;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_same_process_shm_mapping_registry;

		struct coordinates
		{
			std::uint64_t process_epoch{};
			std::uint64_t alias_token{};
			std::uint64_t family_epoch{};
			std::uint64_t family_pin_token{};
			std::uint64_t activity_token{};
		};

		sqlite_shm_registry_activity_pin(
			std::shared_ptr<detail::sqlite_shm_mapping_registry_state> state,
			coordinates value) noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_registry_state> state_;
		std::uint64_t process_epoch_{};
		std::uint64_t alias_token_{};
		std::uint64_t family_epoch_{};
		std::uint64_t family_pin_token_{};
		std::uint64_t activity_token_{};
	};

	/**
	 * Callback-free process registry for exact same-process SHM family coordinators.
	 *
	 * The registry never invokes SQLite, a VFS callback, native registration, native cleanup, or
	 * Store validation. Its only effects are process-local lifecycle state transitions. All live
	 * family coordinators share the one registry-owned, non-reused generation source.
	 */
	class sqlite_same_process_shm_mapping_registry
	{
	  public:
		[[nodiscard]] static sqlite_shm_lease_result<
			std::unique_ptr<sqlite_same_process_shm_mapping_registry>>
		create(sqlite_shm_registry_process_owner owner);
		~sqlite_same_process_shm_mapping_registry() noexcept;

		sqlite_same_process_shm_mapping_registry(const sqlite_same_process_shm_mapping_registry&) =
			delete;
		sqlite_same_process_shm_mapping_registry&
		operator=(const sqlite_same_process_shm_mapping_registry&) = delete;
		sqlite_same_process_shm_mapping_registry(sqlite_same_process_shm_mapping_registry&&) =
			delete;
		sqlite_same_process_shm_mapping_registry&
		operator=(sqlite_same_process_shm_mapping_registry&&) = delete;

		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_registry_alias_pin>
		reserve_alias(sqlite_shm_registry_alias_binding binding);
		/**
		 * Arms the one external native-registration attempt.
		 *
		 * A caller must cross this boundary immediately before the native call. Reserved aliases
		 * can be cancelled as proven pre-effect state; registering aliases can only be confirmed
		 * by an exact success receipt, and abandonment quarantines their retained runtime owner.
		 */
		[[nodiscard]] sqlite_shm_lease_result<void>
		begin_alias_register(sqlite_shm_registry_alias_pin& alias) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void> confirm_alias_registered(
			sqlite_shm_registry_alias_pin& alias,
			const sqlite_shm_verified_alias_registration_receipt& receipt) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void>
		cancel_unregistered_alias(sqlite_shm_registry_alias_pin& alias) noexcept;

		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_registry_family_pin>
		install_or_join_family(sqlite_shm_registry_alias_pin& alias,
							   const sqlite_shm_lease_family_binding& family);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_registry_family_pin>
		pin_existing_family(sqlite_shm_registry_alias_pin& alias,
							const sqlite_shm_lease_family_binding& family);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_registry_activity_pin>
		acquire_activity(sqlite_shm_registry_family_pin& family);
		[[nodiscard]] sqlite_shm_lease_result<void>
		release_activity(sqlite_shm_registry_activity_pin& activity) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void>
		release_family(sqlite_shm_registry_family_pin& family) noexcept;

		[[nodiscard]] sqlite_shm_lease_result<void>
		begin_alias_unregister(sqlite_shm_registry_alias_pin& alias) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void>
		poll_alias_unregister(sqlite_shm_registry_alias_pin& alias) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void> confirm_alias_unregistered(
			sqlite_shm_registry_alias_pin& alias,
			const sqlite_shm_verified_alias_unregistration_receipt& receipt) noexcept;

		[[nodiscard]] sqlite_shm_mapping_registry_snapshot snapshot() const noexcept;
		[[nodiscard]] sqlite_shm_mapping_registry_family_snapshot
		family_snapshot(const sqlite_shm_lease_family_binding& family) const noexcept;

	  private:
		friend class sqlite_same_process_shm_registry_test_peer;

		sqlite_same_process_shm_mapping_registry(
			std::shared_ptr<detail::sqlite_shm_mapping_registry_state> state) noexcept;
		[[nodiscard]] static sqlite_shm_lease_result<
			std::unique_ptr<sqlite_same_process_shm_mapping_registry>>
		create_with_generation_for_testing(sqlite_shm_registry_process_owner owner,
										   std::uint64_t first_mapping_generation);
		void invalidate_process_instance_for_testing() noexcept;
		void lock_registry_mutex_for_fork_testing();
		void unlock_registry_mutex_for_fork_testing() noexcept;
		[[nodiscard]] bool
		inject_duplicate_family_for_testing(const sqlite_shm_lease_family_binding& family) noexcept;
		void exhaust_registry_counters_for_testing() noexcept;
		void exhaust_registry_counter_for_testing(
			detail::sqlite_shm_registry_counter_for_testing counter) noexcept;
		[[nodiscard]] static std::uint64_t state_destruction_count_for_testing() noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_registry_runtime_lifetime_pin>
		adopt_runtime_lifetime_for_testing(sqlite_backend_opaque_identity identity,
										   sqlite_backend_opaque_identity pin_identity,
										   std::shared_ptr<void> owner);
		[[nodiscard]] sqlite_same_process_shm_mapping_lease_coordinator*
		coordinator_for_activity_for_testing(
			const sqlite_shm_registry_activity_pin& activity) const noexcept;
		[[nodiscard]] const void* generation_source_identity_for_testing() const noexcept;
		void lock_state_mutex_for_testing();
		void unlock_state_mutex_for_testing();

		std::shared_ptr<detail::sqlite_shm_mapping_registry_state> state_;
	};
} // namespace cxxlens::sdk
