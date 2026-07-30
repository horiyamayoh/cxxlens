#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "sqlite_same_process_shm_mapping_lease_internal.hpp"

namespace cxxlens::sdk
{
	namespace detail
	{
		struct sqlite_shm_registry_process_owner_seal;
		struct sqlite_shm_registry_runtime_owner_box;
		struct sqlite_shm_registry_activity_control;
		struct sqlite_shm_reader_open_control;
		class sqlite_shm_mapping_registry_state;
		struct sqlite_shm_reader_attachment_authority_state;
		struct sqlite_shm_reader_map_predelegate_authority_state;
		struct sqlite_shm_writer_member_authority_state;
		enum class sqlite_shm_writer_pending_authority_status : std::uint8_t
		{
			exact,
			determinate_mismatch,
			lifecycle_ambiguous,
		};
		enum class sqlite_shm_registry_counter_for_testing : std::uint8_t
		{
			alias_token,
			family_epoch,
			family_pin_token,
			activity_token,
			reader_open_token,
			reader_attachment_epoch,
		};
	} // namespace detail

	class sqlite_same_process_shm_registry_test_peer;
	class sqlite_shm_registry_activity_pin;
	class sqlite_shm_reader_open_authority;
	class sqlite_shm_reader_attachment_authority;
	class sqlite_shm_reader_map_predelegate_authority;
	class sqlite_writer_shm_mapping_epoch_arm;

	/**
	 * Copyable, weak, audit-only view of one exact registry activity.
	 *
	 * The seal never retains the activity owner or registry state and grants no release,
	 * coordinator, mapping, cleanup, or pending authority. It remains valid only while the exact
	 * move-only activity owner is active in the same live process and registry epoch and every
	 * bound alias/family authority remains unquarantined. `valid()` and this value alone must never
	 * be accepted as an authority input. Any later validator must re-enter the exact locked
	 * registry boundary, synchronize transitive quarantine, and recheck the live record/control.
	 */
	class sqlite_shm_registry_activity_seal
	{
	  public:
		~sqlite_shm_registry_activity_seal() noexcept = default;
		sqlite_shm_registry_activity_seal(const sqlite_shm_registry_activity_seal&) noexcept =
			default;
		sqlite_shm_registry_activity_seal&
		operator=(const sqlite_shm_registry_activity_seal&) noexcept = default;
		sqlite_shm_registry_activity_seal(sqlite_shm_registry_activity_seal&&) noexcept = default;
		sqlite_shm_registry_activity_seal&
		operator=(sqlite_shm_registry_activity_seal&&) noexcept = default;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_same_process_shm_registry_test_peer;
		friend class sqlite_shm_registry_activity_pin;
		friend class sqlite_shm_writer_member_authority;
		friend class sqlite_shm_reader_attachment_authority;
		friend class sqlite_shm_reader_map_predelegate_authority;

		explicit sqlite_shm_registry_activity_seal(
			std::weak_ptr<detail::sqlite_shm_registry_activity_control> control) noexcept;

		std::weak_ptr<detail::sqlite_shm_registry_activity_control> control_;
	};

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
		std::size_t active_reader_open_count{};
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
		std::size_t reader_open_count{};
		sqlite_shm_registry_family_phase phase{sqlite_shm_registry_family_phase::retired};
		sqlite_shm_mapping_lease_snapshot coordinator;
		bool coordinator_present{};
		bool lookup_visible{};
	};

	/**
	 * Registry-owned input captured before any SQLite API can start or reuse WAL access.
	 *
	 * The request deliberately has no writer generation, runtime pin, or attachment epoch.
	 * Those values may only be selected by the exact locked registry/lease partition.
	 */
	struct sqlite_shm_reader_pre_sqlite_session_request
	{
		sqlite_shm_lease_family_binding family;
		sqlite_backend_opaque_identity alias_lifetime;
		sqlite_backend_opaque_identity connection_token;
		sqlite_backend_opaque_identity main_native_file_receipt;
		sqlite_backend_opaque_identity main_xopen_receipt;
		sqlite_backend_opaque_identity open_epoch;
		sqlite_backend_opaque_identity callback_cohort;
		sqlite_backend_opaque_identity read_transaction_epoch;
		sqlite_backend_opaque_identity decode_attempt;
		sqlite_backend_opaque_identity authority_read_receipt;

		[[nodiscard]] bool
		operator==(const sqlite_shm_reader_pre_sqlite_session_request&) const = default;
	};

	/** Registry-owned forwarding-file/open binding captured before SQLite entry. */
	struct sqlite_shm_reader_open_binding
	{
		sqlite_shm_lease_family_binding family;
		sqlite_backend_opaque_identity alias_lifetime;
		sqlite_backend_opaque_identity connection_token;
		sqlite_backend_opaque_identity main_native_file_receipt;
		sqlite_backend_opaque_identity main_xopen_receipt;
		sqlite_backend_opaque_identity open_epoch;
		sqlite_backend_opaque_identity callback_cohort;

		[[nodiscard]] bool operator==(const sqlite_shm_reader_open_binding&) const = default;
	};

	/**
	 * Move-only issuer authority for one exact forwarding file/open.
	 *
	 * Plain copied connection/open fields cannot replace this owner. The registry retains the
	 * exact source record and every proposal reservation/group stores its non-reusable token.
	 */
	class sqlite_shm_reader_open_authority
	{
	  public:
		~sqlite_shm_reader_open_authority() noexcept;
		sqlite_shm_reader_open_authority(sqlite_shm_reader_open_authority&&) noexcept;
		sqlite_shm_reader_open_authority& operator=(sqlite_shm_reader_open_authority&&) = delete;
		sqlite_shm_reader_open_authority(const sqlite_shm_reader_open_authority&) = delete;
		sqlite_shm_reader_open_authority&
		operator=(const sqlite_shm_reader_open_authority&) = delete;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_same_process_shm_mapping_registry;
		friend class sqlite_same_process_shm_registry_test_peer;

		explicit sqlite_shm_reader_open_authority(
			std::weak_ptr<detail::sqlite_shm_mapping_registry_state> state,
			std::shared_ptr<detail::sqlite_shm_reader_open_control> control) noexcept;
		void publish_abandonment_lineage_for_testing() noexcept;
		void disarm() noexcept;

		std::weak_ptr<detail::sqlite_shm_mapping_registry_state> state_;
		std::shared_ptr<detail::sqlite_shm_reader_open_control> control_;
	};

	enum class sqlite_shm_reader_session_admission_kind : std::uint8_t
	{
		active_group_owner_admitted,
		reserved_for_local_proposal_candidate,
		existing_or_ordinary_predecessor_zero_proposal_custody,
		rejected_before_sqlite,
	};

	/**
	 * Closed four-way pre-SQLite reader admission result.
	 *
	 * Only the first two kinds carry a proposal request and move-only session owner. The ordinary
	 * predecessor route carries zero proposal identity, epoch, reservation, map attempt, or owner.
	 * A rejected result carries one typed rejection and likewise has zero proposal custody.
	 */
	class sqlite_shm_reader_session_admission
	{
	  public:
		sqlite_shm_reader_session_admission(sqlite_shm_reader_session_admission&&) noexcept;
		sqlite_shm_reader_session_admission&
		operator=(sqlite_shm_reader_session_admission&&) = delete;
		sqlite_shm_reader_session_admission(const sqlite_shm_reader_session_admission&) = delete;
		sqlite_shm_reader_session_admission&
		operator=(const sqlite_shm_reader_session_admission&) = delete;

		[[nodiscard]] sqlite_shm_reader_session_admission_kind kind() const noexcept;
		[[nodiscard]] bool has_proposal_custody() const noexcept;
		[[nodiscard]] const std::optional<sqlite_shm_reader_session_request>&
		proposal_request() const noexcept;
		[[nodiscard]] const std::optional<sqlite_shm_lease_rejection>& rejection() const noexcept;
		[[nodiscard]] std::optional<sqlite_shm_reader_session> take_session() noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class detail::sqlite_shm_mapping_registry_state;

		sqlite_shm_reader_session_admission(
			sqlite_shm_reader_session_admission_kind kind,
			std::optional<sqlite_shm_reader_session_request> proposal_request,
			std::optional<sqlite_shm_reader_session> session,
			std::optional<sqlite_shm_lease_rejection> rejection) noexcept;

		sqlite_shm_reader_session_admission_kind kind_{
			sqlite_shm_reader_session_admission_kind::rejected_before_sqlite};
		std::optional<sqlite_shm_reader_session_request> proposal_request_;
		std::optional<sqlite_shm_reader_session> session_;
		std::optional<sqlite_shm_lease_rejection> rejection_;
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
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_same_process_shm_mapping_registry;
		friend class sqlite_shm_writer_member_authority;
		friend class sqlite_shm_reader_attachment_authority;
		friend class sqlite_shm_reader_map_predelegate_authority;

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

	/**
	 * Move-only sole owner of one exact registry activity.
	 *
	 * Destruction is atomic-only and never acquires the registry or lease mutex. Abandonment first
	 * invalidates the alias latch, then uses the family-latch store as its no-lock scope
	 * linearization point; the next locked registry entry drains the exact count once and applies
	 * local quarantine. Only an exact explicit registry release performs clean count drain. The
	 * owner retains standalone control, not registry state, and therefore cannot create a
	 * registry/coordinator/lease lifetime cycle.
	 */
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
		/**
		 * Mints the sole weak audit seal for this exact active owner.
		 *
		 * A second call fails closed. The returned seal does not retain this owner or the registry.
		 */
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_registry_activity_seal>
		seal_for_audit() noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_same_process_shm_mapping_registry;
		friend class sqlite_same_process_shm_registry_test_peer;
		friend class sqlite_shm_writer_member_authority;
		friend class sqlite_shm_reader_attachment_authority;
		friend class sqlite_shm_reader_map_predelegate_authority;

		sqlite_shm_registry_activity_pin(
			std::weak_ptr<detail::sqlite_shm_mapping_registry_state> state,
			std::shared_ptr<detail::sqlite_shm_registry_activity_control> control) noexcept;
		void disarm() noexcept;

		std::weak_ptr<detail::sqlite_shm_mapping_registry_state> state_;
		std::shared_ptr<detail::sqlite_shm_registry_activity_control> control_;
	};

	/**
	 * Source-private exact-member authority retained by the lease coordinator.
	 *
	 * The bundle is useful only as the inseparable conjunction of its move-only registry activity
	 * owner, one-shot weak audit seal, and strong exact-request writer mapping-epoch arm. Neither a
	 * copied seal nor any semantic receipt can recreate it. Clean release is performed only after
	 * the lease mutex is left; ambiguous destruction uses the activity owner's atomic quarantine
	 * path and never re-enters either mutex.
	 */
	class sqlite_shm_writer_member_authority
	{
	  public:
		~sqlite_shm_writer_member_authority() noexcept;
		sqlite_shm_writer_member_authority(sqlite_shm_writer_member_authority&&) noexcept;
		sqlite_shm_writer_member_authority&
		operator=(sqlite_shm_writer_member_authority&&) = delete;
		sqlite_shm_writer_member_authority(const sqlite_shm_writer_member_authority&) = delete;
		sqlite_shm_writer_member_authority&
		operator=(const sqlite_shm_writer_member_authority&) = delete;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_same_process_shm_registry_test_peer;

		explicit sqlite_shm_writer_member_authority(
			std::unique_ptr<detail::sqlite_shm_writer_member_authority_state> state) noexcept;
		[[nodiscard]] bool
		valid_for_predelegation(const sqlite_shm_writer_map_request& request) const noexcept;
		[[nodiscard]] bool
		retains_exact_lifetimes(const sqlite_shm_writer_map_request& request) const noexcept;
		[[nodiscard]] bool attachment_cohort_compatible_with(
			const sqlite_shm_writer_member_authority& other) const noexcept;
		[[nodiscard]] detail::sqlite_shm_writer_pending_authority_status validate_pending_authority(
			const sqlite_shm_registry_family_pin& family,
			const sqlite_shm_writer_map_request& request,
			const sqlite_shm_verified_writer_post_map_receipt& receipt) const noexcept;
		void invalidate_epoch_for_testing() noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void> release_activity() noexcept;

		std::unique_ptr<detail::sqlite_shm_writer_member_authority_state> state_;
	};

	/**
	 * Move-only registry activity retained by one reader candidate reservation and then its group.
	 *
	 * A successful first-member publication transfers this exact owner from the session
	 * reservation into the live attachment group. Clean release is permitted only for a
	 * determinate no-pointer reservation terminal or after confirmed group unmap. Ambiguous
	 * destruction invalidates the registry activity and preserves quarantine.
	 */
	class sqlite_shm_reader_attachment_authority
	{
	  public:
		~sqlite_shm_reader_attachment_authority() noexcept;
		sqlite_shm_reader_attachment_authority(sqlite_shm_reader_attachment_authority&&) noexcept;
		sqlite_shm_reader_attachment_authority&
		operator=(sqlite_shm_reader_attachment_authority&&) = delete;
		sqlite_shm_reader_attachment_authority(const sqlite_shm_reader_attachment_authority&) =
			delete;
		sqlite_shm_reader_attachment_authority&
		operator=(const sqlite_shm_reader_attachment_authority&) = delete;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_same_process_shm_registry_test_peer;

		explicit sqlite_shm_reader_attachment_authority(
			std::unique_ptr<detail::sqlite_shm_reader_attachment_authority_state> state) noexcept;
		[[nodiscard]] bool
		valid_for_predelegation(const sqlite_shm_reader_session_request& request) const noexcept;
		[[nodiscard]] bool retains_exact_lifetimes(
			const sqlite_shm_reader_attachment_reservation_identity& attachment) const noexcept;
		[[nodiscard]] bool validate_active_authority(
			const sqlite_shm_registry_family_pin& family,
			const sqlite_shm_reader_attachment_reservation_identity& attachment) const noexcept;
		void invalidate_activity_for_testing() noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void> release_activity() noexcept;

		std::unique_ptr<detail::sqlite_shm_reader_attachment_authority_state> state_;
	};

	class sqlite_shm_reader_candidate_authority_minter
	{
	  public:
		struct candidate
		{
			sqlite_shm_reader_session_request request;
			sqlite_shm_reader_attachment_authority authority;
		};

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class detail::sqlite_shm_mapping_registry_state;

		sqlite_shm_reader_candidate_authority_minter(
			detail::sqlite_shm_mapping_registry_state& registry,
			const sqlite_shm_registry_family_pin& family,
			const sqlite_shm_reader_open_authority& open,
			const sqlite_shm_reader_pre_sqlite_session_request& request) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<candidate> mint(std::uint64_t writer_generation);
		void cancel(sqlite_shm_reader_attachment_authority& authority) noexcept;

		detail::sqlite_shm_mapping_registry_state* registry_{};
		const sqlite_shm_registry_family_pin* family_{};
		const sqlite_shm_reader_open_authority* open_{};
		const sqlite_shm_reader_pre_sqlite_session_request* request_{};
	};

	class sqlite_shm_reader_map_predelegate_authority
	{
	  public:
		~sqlite_shm_reader_map_predelegate_authority() noexcept;
		sqlite_shm_reader_map_predelegate_authority(
			sqlite_shm_reader_map_predelegate_authority&&) noexcept;
		sqlite_shm_reader_map_predelegate_authority&
		operator=(sqlite_shm_reader_map_predelegate_authority&&) = delete;
		sqlite_shm_reader_map_predelegate_authority(
			const sqlite_shm_reader_map_predelegate_authority&) = delete;
		sqlite_shm_reader_map_predelegate_authority&
		operator=(const sqlite_shm_reader_map_predelegate_authority&) = delete;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class detail::sqlite_shm_mapping_registry_state;

		explicit sqlite_shm_reader_map_predelegate_authority(
			std::unique_ptr<detail::sqlite_shm_reader_map_predelegate_authority_state>
				state) noexcept;
		[[nodiscard]] bool valid_for_predelegation(
			const sqlite_shm_reader_attachment_map_request& request) const noexcept;
		[[nodiscard]] bool validate_active_authority(
			const sqlite_shm_registry_family_pin& family,
			const sqlite_shm_reader_attachment_map_request& request) const noexcept;
		void invalidate_activity_for_testing() noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void> release_activity() noexcept;

		std::unique_ptr<detail::sqlite_shm_reader_map_predelegate_authority_state> state_;
	};

	class sqlite_shm_reader_map_predelegate_minter
	{
	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class detail::sqlite_shm_mapping_registry_state;

		sqlite_shm_reader_map_predelegate_minter(
			detail::sqlite_shm_mapping_registry_state& registry,
			const sqlite_shm_registry_family_pin& family) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_map_predelegate_authority>
		mint(const sqlite_shm_reader_attachment_map_request& request);
		void cancel(sqlite_shm_reader_map_predelegate_authority& authority) noexcept;

		detail::sqlite_shm_mapping_registry_state* registry_{};
		const sqlite_shm_registry_family_pin* family_{};
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
		/**
		 * Atomically predelegates one exact registry member into its family coordinator.
		 *
		 * This source-private checkpoint has no production caller. The strong mapping-epoch arm
		 * is consumed on every outcome, and the registry lock remains held through the lease begin
		 * visibility point in registry-to-lease lock order.
		 */
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_map_inflight>
		begin_writer_map(sqlite_shm_registry_family_pin& family,
						 sqlite_writer_shm_mapping_epoch_arm arm,
						 const sqlite_shm_writer_map_request& request);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_session_admission>
		admit_reader_session_before_sqlite(
			sqlite_shm_registry_family_pin& family,
			const sqlite_shm_reader_open_authority& open,
			const sqlite_shm_reader_pre_sqlite_session_request& request);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_attachment_map_inflight>
		begin_reader_map(sqlite_shm_registry_family_pin& family,
						 sqlite_shm_reader_session& session,
						 const sqlite_shm_reader_attachment_map_request& request);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_map_commit>
		commit_reader_map(sqlite_shm_registry_family_pin& family,
						  sqlite_shm_reader_attachment_map_inflight& inflight,
						  const sqlite_shm_verified_reader_attachment_post_map_receipt& receipt,
						  sqlite_shm_reader_session& session);
		/**
		 * Installs one validator-sealed registry-bound writer pending at the exact original
		 * family/activity boundary. The registry mutex remains held through the lease transition.
		 */
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_pending_mapping>
		install_writer_pending(sqlite_shm_registry_family_pin& family,
							   sqlite_shm_writer_post_native_mapping& post_native,
							   const sqlite_shm_verified_writer_post_map_receipt& receipt);
		/**
		 * Records one positive Store-gate cut for an exact registry-bound attachment.
		 *
		 * A visible pre-boundary native-map attempt produces `waiting`; once every such attempt is
		 * pending or has resolved without a native mapping, the same exact token completes the cut.
		 * An empty exact group activates the gate without minting mapping authority.
		 */
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_positive_writer_attachment_gate_result>
		advance_positive_writer_attachment_gate(
			sqlite_shm_registry_family_pin& family,
			const sqlite_shm_native_attachment_identity& attachment,
			std::span<sqlite_shm_pending_mapping*> pending,
			const sqlite_shm_writer_eligibility& eligibility);
		/**
		 * Completes one gate-before-map positive path without publishing a pending owner.
		 *
		 * The exact attachment-bound eligibility token and registry member lifetime are rechecked
		 * under the registry-to-lease lock order before the holder is committed.
		 */
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_holder>
		complete_gate_winning_writer_map_before_callback_return(
			sqlite_shm_registry_family_pin& family,
			sqlite_shm_writer_post_native_mapping& post_native,
			const sqlite_shm_verified_writer_post_map_receipt& receipt);
		[[nodiscard]] sqlite_shm_lease_result<void>
		release_activity(sqlite_shm_registry_activity_pin& activity) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void>
		release_reader_open(sqlite_shm_reader_open_authority& open) noexcept;
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
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_open_authority>
		acquire_reader_open_for_testing(sqlite_shm_registry_family_pin& family,
										const sqlite_shm_reader_open_binding& binding);
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
		[[nodiscard]] sqlite_same_process_shm_mapping_lease_coordinator*
		coordinator_for_family_for_testing(
			const sqlite_shm_lease_family_binding& family) const noexcept;
		[[nodiscard]] bool activity_seal_matches_for_testing(
			const sqlite_shm_registry_activity_seal& seal,
			const sqlite_backend_opaque_identity& process_instance,
			const sqlite_shm_lease_family_binding& family,
			const sqlite_backend_opaque_identity& alias_lifetime) noexcept;
		[[nodiscard]] const void* generation_source_identity_for_testing() const noexcept;
		void lock_state_mutex_for_testing();
		void unlock_state_mutex_for_testing();

		std::shared_ptr<detail::sqlite_shm_mapping_registry_state> state_;
	};
} // namespace cxxlens::sdk
