#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "sqlite_backend_observation_internal.hpp"
#include "sqlite_same_process_shm_reader_lifecycle_internal.hpp"

namespace cxxlens::sdk
{
	namespace detail
	{
		class sqlite_shm_mapping_lease_state;
		class sqlite_shm_mapping_registry_state;
		class sqlite_writer_shm_mapping_epoch_state;

		struct sqlite_shm_lease_token_identity
		{
			std::uint64_t value{};
		};

		struct sqlite_shm_mapping_generation_identity
		{
			std::uint64_t value{};
		};

		/** Callback-free no-lock seal for one registry-issued reader open lineage. */
		struct sqlite_shm_reader_open_lineage_seal
		{
			std::atomic_bool authority_valid{true};
		};

		/**
		 * Registry-owned admission cut shared with no-lock peer-abandonment invalidation.
		 *
		 * A guarded xOpen registration linearizes only while all three latches retain authority.
		 * The lease coordinator never stores these latches as post-admission cleanup gates.
		 */
		struct sqlite_shm_reader_open_admission_guard
		{
			std::shared_ptr<std::atomic_bool> emergency_latch;
			std::shared_ptr<std::atomic_bool> alias_authority_latch;
			std::shared_ptr<std::atomic_bool> family_authority_latch;

			[[nodiscard]] bool valid() const noexcept
			{
				return emergency_latch && alias_authority_latch && family_authority_latch;
			}

			[[nodiscard]] bool admission_visible_now() const noexcept
			{
				return valid() && !emergency_latch->load(std::memory_order_acquire) &&
					alias_authority_latch->load(std::memory_order_acquire) &&
					family_authority_latch->load(std::memory_order_acquire);
			}
		};
	} // namespace detail

	class sqlite_shm_writer_member_authority;
	class sqlite_shm_reader_attachment_authority;
	class sqlite_shm_reader_candidate_authority_minter;
	class sqlite_shm_reader_map_predelegate_authority;
	class sqlite_shm_reader_map_predelegate_minter;
	class sqlite_shm_registry_family_pin;
	class sqlite_shm_reader_session_admission;
	struct sqlite_shm_reader_pre_sqlite_session_request;
	class sqlite_writer_shm_mapping_epoch_arm;

	/**
	 * Exact process/runtime/file-family binding consumed by the pure lease coordinator.
	 *
	 * A producer must seal each opaque identity from the full DF-0205 receipt. In particular,
	 * `shared_runtime_vfs_cohort` excludes the per-alias runtime lifetime identity: distinct
	 * owned forwarding aliases intentionally share one underlying mapping cohort.
	 */
	struct sqlite_shm_lease_family_binding
	{
		sqlite_backend_opaque_identity process_instance;
		sqlite_backend_opaque_identity shared_runtime_vfs_cohort;
		sqlite_backend_opaque_identity exact_file_family;

		[[nodiscard]] bool operator==(const sqlite_shm_lease_family_binding&) const = default;
	};

	/**
	 * Exact source-private binding of one authenticated reader xOpen epoch.
	 *
	 * This value deliberately predates any writer generation, reader attachment reservation, or
	 * map member. It is the immutable binding for the one orthogonal connection-close owner minted
	 * at xOpen registration. The registry must construct it from its locked reader-open record;
	 * copied connection, path, pointer, or open-epoch equality is not authority.
	 */
	struct sqlite_shm_reader_open_epoch_binding
	{
		sqlite_shm_lease_family_binding family;
		sqlite_backend_opaque_identity runtime_lifetime_pin;
		sqlite_backend_opaque_identity alias_lifetime;
		sqlite_backend_opaque_identity connection_token;
		sqlite_backend_opaque_identity main_native_file_receipt;
		sqlite_backend_opaque_identity main_xopen_receipt;
		sqlite_backend_opaque_identity open_epoch;
		sqlite_backend_opaque_identity callback_cohort;

		[[nodiscard]] bool operator==(const sqlite_shm_reader_open_epoch_binding&) const = default;
	};

	/**
	 * Source-private checked identity of one native SQLite SHM attachment.
	 *
	 * The attachment epoch is sealed as non-reusable within one process/runtime/VFS/file-family
	 * coordinator. Pointer, connection, path, or open epoch equality alone is not attachment
	 * identity. Construction fails unless every authority component is present. The value grants
	 * neither writer nor reader authority by itself; writer-only transitions consume it in this
	 * slice, and DF-0207 separately governs future reader grouping. This internal value is not an
	 * installed public SDK surface.
	 */
	class sqlite_shm_native_attachment_identity
	{
	  public:
		[[nodiscard]] static std::optional<sqlite_shm_native_attachment_identity>
		bind(sqlite_shm_lease_family_binding family,
			 sqlite_backend_opaque_identity alias_lifetime,
			 sqlite_backend_opaque_identity connection_token,
			 sqlite_backend_opaque_identity main_native_file_receipt,
			 sqlite_backend_opaque_identity main_xopen_receipt,
			 sqlite_backend_opaque_identity open_epoch,
			 sqlite_backend_opaque_identity callback_cohort,
			 sqlite_backend_opaque_identity attachment_epoch);

		[[nodiscard]] const sqlite_shm_lease_family_binding& family() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& alias_lifetime() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& connection_token() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		main_native_file_receipt() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& main_xopen_receipt() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& open_epoch() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& callback_cohort() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& attachment_epoch() const noexcept;

		[[nodiscard]] bool operator==(const sqlite_shm_native_attachment_identity&) const = default;

	  private:
		sqlite_shm_native_attachment_identity(
			sqlite_shm_lease_family_binding family,
			sqlite_backend_opaque_identity alias_lifetime,
			sqlite_backend_opaque_identity connection_token,
			sqlite_backend_opaque_identity main_native_file_receipt,
			sqlite_backend_opaque_identity main_xopen_receipt,
			sqlite_backend_opaque_identity open_epoch,
			sqlite_backend_opaque_identity callback_cohort,
			sqlite_backend_opaque_identity attachment_epoch);

		sqlite_shm_lease_family_binding family_;
		sqlite_backend_opaque_identity alias_lifetime_;
		sqlite_backend_opaque_identity connection_token_;
		sqlite_backend_opaque_identity main_native_file_receipt_;
		sqlite_backend_opaque_identity main_xopen_receipt_;
		sqlite_backend_opaque_identity open_epoch_;
		sqlite_backend_opaque_identity callback_cohort_;
		sqlite_backend_opaque_identity attachment_epoch_;
	};

	/**
	 * Source-private expected identity reserved before a first reader native map.
	 *
	 * This value is intentionally distinct from the accepted DF-0206 writer identity. It binds
	 * the exact reader alias/open epoch, writer mapping generation, and one non-reusable expected
	 * attachment epoch, but deliberately contains no post-map object observation. It grants no
	 * map, pointer, session, unmap, or close authority by itself. The current binder is
	 * production-inert; a future registry/VFS validator must become its only production minter.
	 */
	class sqlite_shm_reader_attachment_reservation_identity
	{
	  public:
		[[nodiscard]] const sqlite_shm_lease_family_binding& family() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& runtime_lifetime_pin() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& alias_lifetime() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& connection_token() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		main_native_file_receipt() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& main_xopen_receipt() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& open_epoch() const noexcept;
		[[nodiscard]] std::uint64_t writer_mapping_generation() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& callback_cohort() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& attachment_epoch() const noexcept;
		/** Exact issuer-owned registry reader-open token; zero denotes the legacy-only route. */
		[[nodiscard]] std::uint64_t registry_open_token() const noexcept;

		[[nodiscard]] bool
		operator==(const sqlite_shm_reader_attachment_reservation_identity&) const = default;

	  private:
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_same_process_shm_lease_test_peer;

		[[nodiscard]] static std::optional<sqlite_shm_reader_attachment_reservation_identity>
		bind(sqlite_shm_lease_family_binding family,
			 sqlite_backend_opaque_identity runtime_lifetime_pin,
			 sqlite_backend_opaque_identity alias_lifetime,
			 sqlite_backend_opaque_identity connection_token,
			 sqlite_backend_opaque_identity main_native_file_receipt,
			 sqlite_backend_opaque_identity main_xopen_receipt,
			 sqlite_backend_opaque_identity open_epoch,
			 std::uint64_t writer_mapping_generation,
			 sqlite_backend_opaque_identity callback_cohort,
			 sqlite_backend_opaque_identity attachment_epoch,
			 std::uint64_t registry_open_token = 0U);

		sqlite_shm_reader_attachment_reservation_identity(
			sqlite_shm_lease_family_binding family,
			sqlite_backend_opaque_identity runtime_lifetime_pin,
			sqlite_backend_opaque_identity alias_lifetime,
			sqlite_backend_opaque_identity connection_token,
			sqlite_backend_opaque_identity main_native_file_receipt,
			sqlite_backend_opaque_identity main_xopen_receipt,
			sqlite_backend_opaque_identity open_epoch,
			std::uint64_t writer_mapping_generation,
			sqlite_backend_opaque_identity callback_cohort,
			sqlite_backend_opaque_identity attachment_epoch,
			std::uint64_t registry_open_token);

		sqlite_shm_lease_family_binding family_;
		sqlite_backend_opaque_identity runtime_lifetime_pin_;
		sqlite_backend_opaque_identity alias_lifetime_;
		sqlite_backend_opaque_identity connection_token_;
		sqlite_backend_opaque_identity main_native_file_receipt_;
		sqlite_backend_opaque_identity main_xopen_receipt_;
		sqlite_backend_opaque_identity open_epoch_;
		std::uint64_t writer_mapping_generation_{};
		sqlite_backend_opaque_identity callback_cohort_;
		sqlite_backend_opaque_identity attachment_epoch_;
		std::uint64_t registry_open_token_{};
	};

	namespace detail
	{
		enum class sqlite_shm_registry_reader_pre_mint_route : std::uint8_t
		{
			active_group,
			local_proposal_candidate,
			ordinary_predecessor,
		};

		struct sqlite_shm_registry_reader_pre_mint_classification
		{
			sqlite_shm_registry_reader_pre_mint_route route{
				sqlite_shm_registry_reader_pre_mint_route::ordinary_predecessor};
			std::optional<sqlite_shm_reader_attachment_reservation_identity> active_attachment;
			std::uint64_t local_writer_generation{};
		};
	} // namespace detail

	/**
	 * Issuer-sealed post-map observed reader attachment identity.
	 *
	 * This can be formed only after a determinate mapped result. It cross-binds the expected
	 * reservation to checked direct SHM object/entry/device/mount evidence and is the identity
	 * retained by a live attachment group.
	 */
	class sqlite_shm_reader_native_attachment_identity
	{
	  public:
		[[nodiscard]] const sqlite_shm_reader_attachment_reservation_identity&
		expected() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		observed_shm_object_receipt() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		observed_shm_entry_receipt() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		observed_device_receipt() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& observed_mount_receipt() const noexcept;

		[[nodiscard]] bool
		operator==(const sqlite_shm_reader_native_attachment_identity&) const = default;

	  private:
		friend class sqlite_same_process_shm_reader_receipt_validator;
		friend class sqlite_same_process_shm_lease_test_peer;

		sqlite_shm_reader_native_attachment_identity(
			sqlite_shm_reader_attachment_reservation_identity expected,
			sqlite_backend_opaque_identity observed_shm_object_receipt,
			sqlite_backend_opaque_identity observed_shm_entry_receipt,
			sqlite_backend_opaque_identity observed_device_receipt,
			sqlite_backend_opaque_identity observed_mount_receipt);

		sqlite_shm_reader_attachment_reservation_identity expected_;
		sqlite_backend_opaque_identity observed_shm_object_receipt_;
		sqlite_backend_opaque_identity observed_shm_entry_receipt_;
		sqlite_backend_opaque_identity observed_device_receipt_;
		sqlite_backend_opaque_identity observed_mount_receipt_;
	};

	/**
	 * One callback invocation identity retained across a native callback boundary.
	 *
	 * The future process-global registry must seal `invocation_token` as non-reusable for the
	 * complete process/runtime/VFS cohort. This family coordinator rejects duplicates among active
	 * callbacks, binds every multi-phase transition to exact receipt equality, and retains compact
	 * same-family completed identities across coordinator replacement. Cross-family process-global
	 * nonreuse remains the production issuer's responsibility.
	 */
	struct sqlite_shm_callback_execution_receipt
	{
		sqlite_backend_opaque_identity thread_identity;
		std::uint64_t reentrancy_depth{};
		sqlite_backend_opaque_identity invocation_token;

		[[nodiscard]] bool operator==(const sqlite_shm_callback_execution_receipt&) const = default;
	};

	/** Exact page tuple. The pointer is non-owning and is used only as opaque identity. */
	struct sqlite_shm_mapping_tuple
	{
		int page_number{};
		int page_size{};
		std::uint64_t byte_offset{};
		std::uint64_t byte_count{};
		const volatile void* native_mapping{};
		std::uint64_t sealed_shm_size{};

		[[nodiscard]] bool operator==(const sqlite_shm_mapping_tuple&) const = default;
	};

	enum class sqlite_shm_writer_extend_pair : std::uint8_t
	{
		one_one,
		zero_zero,
	};

	/**
	 * Classify the only two extend pairs which a writer-map evidence validator may seal.
	 * `{1,0}`, `{0,1}`, and values outside SQLite's closed zero/one domain are rejected.
	 */
	[[nodiscard]] std::optional<sqlite_shm_writer_extend_pair>
	classify_sqlite_shm_writer_extend_pair(int caller_extend, int delegated_extend) noexcept;

	/** Request captured before a writer native xShmMap call. */
	struct sqlite_shm_writer_map_request
	{
		sqlite_shm_lease_family_binding family;
		sqlite_backend_opaque_identity alias_lifetime;
		sqlite_backend_opaque_identity connection_token;
		sqlite_shm_native_attachment_identity attachment;
		sqlite_shm_callback_execution_receipt callback;
		int page_number{};
		int page_size{};
		int caller_extend{};

		[[nodiscard]] bool operator==(const sqlite_shm_writer_map_request&) const = default;
	};

	/** Request captured before a qualified reader native xShmMap call. */
	struct sqlite_shm_reader_map_request
	{
		sqlite_shm_lease_family_binding family;
		sqlite_backend_opaque_identity alias_lifetime;
		sqlite_backend_opaque_identity connection_token;
		sqlite_shm_callback_execution_receipt callback;
		int page_number{};
		int page_size{};
		int caller_extend{};

		[[nodiscard]] bool operator==(const sqlite_shm_reader_map_request&) const = default;
	};

	/** Proposal-group reader map request, distinct from the byte-semantic legacy route. */
	struct sqlite_shm_reader_attachment_map_request
	{
		sqlite_shm_lease_family_binding family;
		sqlite_backend_opaque_identity alias_lifetime;
		sqlite_backend_opaque_identity connection_token;
		sqlite_shm_reader_attachment_reservation_identity expected_attachment;
		sqlite_shm_callback_execution_receipt callback;
		int page_number{};
		int page_size{};
		int caller_extend{};

		[[nodiscard]] bool
		operator==(const sqlite_shm_reader_attachment_map_request&) const = default;
	};

	/** Exact pre-SQLite read/decode session identity. */
	struct sqlite_shm_reader_session_request
	{
		sqlite_shm_reader_attachment_reservation_identity attachment;
		sqlite_backend_opaque_identity read_transaction_epoch;
		sqlite_backend_opaque_identity decode_attempt;
		sqlite_backend_opaque_identity authority_read_receipt;

		[[nodiscard]] bool operator==(const sqlite_shm_reader_session_request&) const = default;
	};

	enum class sqlite_shm_reader_session_terminal_kind : std::uint8_t
	{
		success,
		failure,
		cancelled_before_authority_read,
	};

	class sqlite_shm_writer_map_inflight;
	class sqlite_shm_writer_holder;
	class sqlite_shm_reader_session;
	class sqlite_shm_reader_attachment_map_inflight;
	class sqlite_shm_reader_unmap_obligation;
	class sqlite_shm_reader_close_obligation;
	class sqlite_shm_verified_reader_attachment_zero_effect_receipt;
	class sqlite_shm_verified_reader_unmap_terminal_receipt;
	class sqlite_shm_verified_reader_close_terminal_receipt;
	class sqlite_shm_verified_writer_native_map_receipt;
	class sqlite_writer_shm_native_map_receipt_validator;
	class sqlite_writer_shm_mapping_receipt_validator;
	class sqlite_same_process_shm_writer_gate_receipt_validator;
	class sqlite_same_process_shm_reader_receipt_validator;
	class sqlite_same_process_shm_reader_zero_effect_receipt_validator;
	class sqlite_same_process_shm_reader_session_terminal_receipt_validator;
	class sqlite_same_process_shm_lease_test_peer;

	/**
	 * Issuer-sealed one-session terminal receipt.
	 *
	 * The production issuer must bind the full process/family/session owner and request context and
	 * make the terminal identity nonreusable for process lifetime across families and receipt
	 * kinds. Coordinator-local replay census is defense-in-depth, not a substitute for that issuer
	 * proof.
	 */
	class sqlite_shm_reader_session_terminal_receipt
	{
	  public:
		[[nodiscard]] const sqlite_shm_reader_session_request& request() const noexcept;
		[[nodiscard]] sqlite_shm_reader_session_terminal_kind kind() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& terminal_receipt() const noexcept;
		[[nodiscard]] bool authority_read_closed() const noexcept;
		[[nodiscard]] bool no_live_shm_lock() const noexcept;

		[[nodiscard]] bool
		operator==(const sqlite_shm_reader_session_terminal_receipt&) const = default;

	  private:
		friend class sqlite_same_process_shm_reader_session_terminal_receipt_validator;
		friend class sqlite_same_process_shm_lease_test_peer;

		sqlite_shm_reader_session_terminal_receipt(sqlite_shm_reader_session_request request,
												   sqlite_shm_reader_session_terminal_kind kind,
												   sqlite_backend_opaque_identity terminal_receipt,
												   bool authority_read_closed,
												   bool no_live_shm_lock);

		sqlite_shm_reader_session_request request_;
		sqlite_shm_reader_session_terminal_kind kind_{
			sqlite_shm_reader_session_terminal_kind::failure};
		sqlite_backend_opaque_identity terminal_receipt_;
		bool authority_read_closed_{};
		bool no_live_shm_lock_{};
	};

	enum class sqlite_shm_reader_attachment_zero_effect_kind : std::uint8_t
	{
		exact_no_attachment_change,
		exact_protocol_invalid_no_attachment,
	};

	/**
	 * Issuer-sealed determinate reader-map result with complete zero-attachment-effect proof.
	 *
	 * This receipt is bound to one exact proposal attachment-map attempt. It can describe only
	 * the two zero-attachment Step 5a routes; mapped, predecessor, incomplete-effect, and
	 * ambiguous results cannot be represented by this type. The production validator remains
	 * deliberately absent until the source-private VFS issuer is implemented and reviewed.
	 *
	 * That issuer must bind the full process/family/attempt/request/callback/admission/effect role
	 * and make the effect identity nonreusable for process lifetime across families and mapped/zero
	 * kinds. Coordinator-local replay census is defense-in-depth, not an issuer replacement.
	 */
	class sqlite_shm_verified_reader_attachment_zero_effect_receipt
	{
	  public:
		[[nodiscard]] sqlite_shm_reader_attachment_zero_effect_kind kind() const noexcept;
		[[nodiscard]] const sqlite_shm_reader_attachment_map_request& request() const noexcept;
		[[nodiscard]] int native_status() const noexcept;
		[[nodiscard]] const volatile void* native_mapping() const noexcept;
		[[nodiscard]] int delegated_extend() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		zero_attachment_effect_receipt() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class sqlite_same_process_shm_reader_zero_effect_receipt_validator;
		friend class sqlite_same_process_shm_lease_test_peer;

		sqlite_shm_verified_reader_attachment_zero_effect_receipt(
			const sqlite_shm_reader_attachment_map_inflight& inflight,
			sqlite_shm_reader_attachment_zero_effect_kind kind,
			sqlite_shm_reader_attachment_map_request request,
			int native_status,
			const volatile void* native_mapping,
			int delegated_extend,
			sqlite_backend_opaque_identity zero_attachment_effect_receipt);

		std::weak_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
		sqlite_shm_reader_attachment_zero_effect_kind kind_{
			sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change};
		sqlite_shm_reader_attachment_map_request request_;
		int native_status_{};
		const volatile void* native_mapping_{};
		int delegated_extend_{};
		sqlite_backend_opaque_identity zero_attachment_effect_receipt_;
	};

	/**
	 * Closed outward projection after one zero-attachment terminal commit.
	 *
	 * `native_mapping()` is always null. Exact no-change preserves its determinate native status;
	 * protocol-invalid no-attachment projects the stable SQLite IOERR status.
	 */
	class sqlite_shm_reader_attachment_zero_effect_result
	{
	  public:
		[[nodiscard]] sqlite_shm_reader_attachment_zero_effect_kind kind() const noexcept;
		[[nodiscard]] int native_status() const noexcept;
		[[nodiscard]] const volatile void* native_mapping() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;

		sqlite_shm_reader_attachment_zero_effect_result(
			sqlite_shm_reader_attachment_zero_effect_kind kind, int native_status) noexcept;

		sqlite_shm_reader_attachment_zero_effect_kind kind_{
			sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change};
		int native_status_{};
	};

	/**
	 * Issuer-sealed exact terminal receipt for one proposal-group xShmUnmap(0) effect.
	 *
	 * The receipt is bound to the move-only unmap owner, exact callback invocation, and generation.
	 * Both delete flags must be zero. A confirmed SQLite OK result additionally requires one exact
	 * latch-reset receipt; every non-OK status must omit it. The coordinator performs no native
	 * call and consumes this receipt exactly once.
	 */
	enum class sqlite_shm_reader_unmap_evidence_kind : std::uint8_t
	{
		exact_native_result,
		throw_or_unknown,
	};

	struct sqlite_shm_reader_unmap_request
	{
		sqlite_shm_callback_execution_receipt callback;
		int caller_delete_flag{};
		int delegated_delete_flag{};
	};

	/**
	 * Issuer-sealed exact reader-unmap terminal evidence.
	 *
	 * The production issuer must bind the full process/family/group owner, callback, request, cut,
	 * and evidence roles; atomically issue distinct effect and latch-reset identities; and make
	 * both nonreusable for process lifetime across families and kinds. Coordinator-local replay
	 * census is defense-in-depth and does not replace that issuer proof.
	 */
	class sqlite_shm_verified_reader_unmap_terminal_receipt
	{
	  public:
		[[nodiscard]] const sqlite_shm_callback_execution_receipt& callback() const noexcept;
		[[nodiscard]] sqlite_shm_reader_unmap_evidence_kind evidence_kind() const noexcept;
		[[nodiscard]] std::optional<int> native_status() const noexcept;
		[[nodiscard]] int caller_delete_flag() const noexcept;
		[[nodiscard]] int delegated_delete_flag() const noexcept;
		[[nodiscard]] const std::optional<sqlite_backend_opaque_identity>&
		native_effect_receipt() const noexcept;
		[[nodiscard]] const std::optional<sqlite_backend_opaque_identity>&
		latch_reset_receipt() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class sqlite_same_process_shm_lease_test_peer;

		sqlite_shm_verified_reader_unmap_terminal_receipt(
			const sqlite_shm_reader_unmap_obligation& unmap,
			sqlite_shm_callback_execution_receipt callback,
			sqlite_shm_reader_unmap_evidence_kind evidence_kind,
			std::optional<int> native_status,
			int caller_delete_flag,
			int delegated_delete_flag,
			std::optional<sqlite_backend_opaque_identity> native_effect_receipt,
			std::optional<sqlite_backend_opaque_identity> latch_reset_receipt);

		std::weak_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
		std::uint64_t generation_{};
		sqlite_shm_callback_execution_receipt callback_;
		sqlite_shm_reader_unmap_evidence_kind evidence_kind_{
			sqlite_shm_reader_unmap_evidence_kind::throw_or_unknown};
		std::optional<int> native_status_;
		int caller_delete_flag_{};
		int delegated_delete_flag_{};
		std::optional<sqlite_backend_opaque_identity> native_effect_receipt_;
		std::optional<sqlite_backend_opaque_identity> latch_reset_receipt_;
	};

	enum class sqlite_shm_reader_unmap_terminal_kind : std::uint8_t
	{
		retired_confirmed,
		terminal_quarantined,
	};

	/** Exact allocation-free projection of a committed proposal-group unmap terminal. */
	class sqlite_shm_reader_unmap_terminal_result
	{
	  public:
		[[nodiscard]] sqlite_shm_reader_unmap_terminal_kind kind() const noexcept;
		[[nodiscard]] sqlite_shm_reader_unmap_evidence_kind evidence_kind() const noexcept;
		[[nodiscard]] std::optional<int> native_status() const noexcept;
		[[nodiscard]] int outward_status() const noexcept;
		[[nodiscard]] const std::optional<sqlite_backend_opaque_identity>&
		native_effect_receipt() const noexcept;
		[[nodiscard]] const std::optional<sqlite_backend_opaque_identity>&
		latch_reset_receipt() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;

		sqlite_shm_reader_unmap_terminal_result(
			sqlite_shm_reader_unmap_terminal_kind kind,
			sqlite_shm_reader_unmap_evidence_kind evidence_kind,
			std::optional<int> native_status,
			std::optional<sqlite_backend_opaque_identity> native_effect_receipt,
			std::optional<sqlite_backend_opaque_identity> latch_reset_receipt) noexcept;

		sqlite_shm_reader_unmap_terminal_kind kind_{
			sqlite_shm_reader_unmap_terminal_kind::terminal_quarantined};
		sqlite_shm_reader_unmap_evidence_kind evidence_kind_{
			sqlite_shm_reader_unmap_evidence_kind::throw_or_unknown};
		std::optional<int> native_status_;
		int outward_status_{};
		std::optional<sqlite_backend_opaque_identity> native_effect_receipt_;
		std::optional<sqlite_backend_opaque_identity> latch_reset_receipt_;
	};

	/** Phase-1 close rows which delegate xClose directly and never invent xShmUnmap authority. */
	enum class sqlite_shm_reader_close_route : std::uint8_t
	{
		close_without_group,
		close_after_confirmed_unmap,
	};

	enum class sqlite_shm_reader_close_evidence_kind : std::uint8_t
	{
		exact_native_result,
		throw_or_unknown,
	};

	struct sqlite_shm_reader_close_request
	{
		sqlite_shm_callback_execution_receipt callback;

		[[nodiscard]] bool operator==(const sqlite_shm_reader_close_request&) const = default;
	};

	/**
	 * Issuer-sealed exact xClose terminal evidence for one open-epoch close obligation.
	 *
	 * Phase 1 accepts either a determinate native result with one exact effect identity, or an
	 * explicit throw/unknown row with neither status nor effect identity. The production issuer is
	 * intentionally absent until the reader VFS integration is separately reviewed.
	 */
	class sqlite_shm_verified_reader_close_terminal_receipt
	{
	  public:
		[[nodiscard]] const sqlite_shm_callback_execution_receipt& callback() const noexcept;
		[[nodiscard]] sqlite_shm_reader_close_evidence_kind evidence_kind() const noexcept;
		[[nodiscard]] std::optional<int> native_status() const noexcept;
		[[nodiscard]] const std::optional<sqlite_backend_opaque_identity>&
		native_effect_receipt() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class sqlite_same_process_shm_lease_test_peer;

		sqlite_shm_verified_reader_close_terminal_receipt(
			const sqlite_shm_reader_close_obligation& close,
			sqlite_shm_callback_execution_receipt callback,
			sqlite_shm_reader_close_evidence_kind evidence_kind,
			std::optional<int> native_status,
			std::optional<sqlite_backend_opaque_identity> native_effect_receipt);

		std::weak_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t owner_token_{};
		std::uint64_t registry_open_token_{};
		sqlite_shm_callback_execution_receipt callback_;
		sqlite_shm_reader_close_evidence_kind evidence_kind_{
			sqlite_shm_reader_close_evidence_kind::throw_or_unknown};
		std::optional<int> native_status_;
		std::optional<sqlite_backend_opaque_identity> native_effect_receipt_;
	};

	enum class sqlite_shm_reader_close_terminal_kind : std::uint8_t
	{
		closed,
		terminal_quarantined,
	};

	/** Closed projection of one committed Phase-1 reader xClose terminal. */
	class sqlite_shm_reader_close_terminal_result
	{
	  public:
		[[nodiscard]] sqlite_shm_reader_close_terminal_kind kind() const noexcept;
		[[nodiscard]] sqlite_shm_reader_close_route route() const noexcept;
		[[nodiscard]] sqlite_shm_reader_close_evidence_kind evidence_kind() const noexcept;
		[[nodiscard]] std::optional<int> native_status() const noexcept;
		[[nodiscard]] int outward_status() const noexcept;
		[[nodiscard]] const std::optional<sqlite_backend_opaque_identity>&
		native_effect_receipt() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;

		sqlite_shm_reader_close_terminal_result(
			sqlite_shm_reader_close_terminal_kind kind,
			sqlite_shm_reader_close_route route,
			sqlite_shm_reader_close_evidence_kind evidence_kind,
			std::optional<int> native_status,
			std::optional<sqlite_backend_opaque_identity> native_effect_receipt) noexcept;

		sqlite_shm_reader_close_terminal_kind kind_{
			sqlite_shm_reader_close_terminal_kind::terminal_quarantined};
		sqlite_shm_reader_close_route route_{sqlite_shm_reader_close_route::close_without_group};
		sqlite_shm_reader_close_evidence_kind evidence_kind_{
			sqlite_shm_reader_close_evidence_kind::throw_or_unknown};
		std::optional<int> native_status_;
		int outward_status_{};
		std::optional<sqlite_backend_opaque_identity> native_effect_receipt_;
	};

	/**
	 * Process-lifetime one-shot identities retained by a compact reader lifecycle tombstone.
	 *
	 * Callback invocation identities and native-effect identities remain separate semantic
	 * domains, but every identity is unique inside its domain and survives family coordinator
	 * replacement.
	 */
	struct sqlite_shm_reader_replay_identity_tombstone
	{
		std::vector<sqlite_backend_opaque_identity> callback_invocation_tokens;
		std::vector<sqlite_backend_opaque_identity> effect_receipts;
		bool callback_free_terminal{};

		[[nodiscard]] bool
		operator==(const sqlite_shm_reader_replay_identity_tombstone&) const = default;
	};

	/**
	 * Registry-retained stale-reservation evidence after a family coordinator retires.
	 *
	 * Group payload, sessions, and native observations are intentionally excluded. Exact
	 * callback/effect one-shot identities remain as replay-only evidence.
	 */
	struct sqlite_shm_reader_lifecycle_compact_tombstone
	{
		sqlite_shm_reader_attachment_reservation_identity attachment;
		detail::sqlite_shm_reader_attachment_reservation_phase phase{
			detail::sqlite_shm_reader_attachment_reservation_phase::revoked_no_map};
		std::uint64_t origin_sequence{};
		std::uint64_t destination_sequence{};
		sqlite_shm_reader_replay_identity_tombstone replay_identities;

		[[nodiscard]] bool
		operator==(const sqlite_shm_reader_lifecycle_compact_tombstone&) const = default;
	};

	/**
	 * Process-lifetime stale-token tombstone for one successfully closed reader open epoch.
	 *
	 * Unlike an attachment tombstone, this remains representable when the handle never mapped and
	 * therefore has no writer generation or attachment epoch.
	 */
	struct sqlite_shm_reader_open_epoch_close_tombstone
	{
		std::uint64_t registry_open_token{};
		std::uint64_t close_owner_token{};
		sqlite_shm_reader_open_epoch_binding binding;
		sqlite_shm_reader_replay_identity_tombstone replay_identities;
		std::uint64_t origin_sequence{};
		std::uint64_t close_cut_sequence{};
		std::uint64_t terminal_sequence{};

		[[nodiscard]] bool
		operator==(const sqlite_shm_reader_open_epoch_close_tombstone&) const = default;
	};

	/**
	 * Seal produced only after native writer map returned exact SQLITE_OK/non-null and the
	 * independent writer stat/watch/effect validator completed its post-map receipt.
	 */
	class sqlite_shm_verified_writer_post_map_receipt
	{
	  public:
		[[nodiscard]] const sqlite_shm_writer_map_request& request() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& open_epoch() const noexcept;
		[[nodiscard]] const sqlite_shm_mapping_tuple& mapping() const noexcept;
		[[nodiscard]] sqlite_shm_writer_extend_pair extend_pair() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		holder_specific_effect_receipt() const noexcept;

	  private:
		friend class sqlite_writer_shm_mapping_receipt_validator;
		friend class sqlite_writer_shm_mapping_epoch_arm;
		friend class sqlite_same_process_shm_lease_test_peer;

		sqlite_shm_verified_writer_post_map_receipt(
			sqlite_shm_writer_map_request request,
			sqlite_backend_opaque_identity open_epoch,
			sqlite_shm_mapping_tuple mapping,
			sqlite_shm_writer_extend_pair extend_pair,
			sqlite_backend_opaque_identity holder_specific_effect_receipt);
		sqlite_shm_verified_writer_post_map_receipt(
			sqlite_shm_writer_map_request request,
			sqlite_backend_opaque_identity open_epoch,
			sqlite_shm_mapping_tuple mapping,
			sqlite_shm_writer_extend_pair extend_pair,
			sqlite_backend_opaque_identity holder_specific_effect_receipt,
			std::weak_ptr<detail::sqlite_writer_shm_mapping_epoch_state> epoch_state,
			std::uint64_t epoch_seal_sequence);

		sqlite_shm_writer_map_request request_;
		sqlite_backend_opaque_identity open_epoch_;
		sqlite_shm_mapping_tuple mapping_;
		sqlite_shm_writer_extend_pair extend_pair_{sqlite_shm_writer_extend_pair::zero_zero};
		sqlite_backend_opaque_identity holder_specific_effect_receipt_;
		std::weak_ptr<detail::sqlite_writer_shm_mapping_epoch_state> epoch_state_;
		std::uint64_t epoch_seal_sequence_{};
	};

	/**
	 * Immutable current-v3 Store eligibility. It is neither mapping nor reader authority alone.
	 */
	class sqlite_shm_verified_writer_eligibility_receipt
	{
	  public:
		[[nodiscard]] const sqlite_shm_lease_family_binding& family() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& connection_token() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& open_epoch() const noexcept;
		[[nodiscard]] const sqlite_backend_effect_arm_receipt& effect_gate() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		complete_current_v3_gate() const noexcept;

	  private:
		friend class sqlite_same_process_shm_writer_gate_receipt_validator;
		friend class sqlite_same_process_shm_lease_test_peer;

		sqlite_shm_verified_writer_eligibility_receipt(
			sqlite_shm_lease_family_binding family,
			sqlite_backend_opaque_identity connection_token,
			sqlite_backend_opaque_identity open_epoch,
			sqlite_backend_effect_arm_receipt effect_gate,
			sqlite_backend_opaque_identity complete_current_v3_gate);

		sqlite_shm_lease_family_binding family_;
		sqlite_backend_opaque_identity connection_token_;
		sqlite_backend_opaque_identity open_epoch_;
		sqlite_backend_effect_arm_receipt effect_gate_;
		sqlite_backend_opaque_identity complete_current_v3_gate_;
	};

	/** Exact native reader-map post receipt, sealed independently from the lease lookup. */
	/**
	 * Issuer-sealed reader post-map evidence.
	 *
	 * The production issuer must bind the full process/family/attempt/request/callback/admission
	 * and effect role and make its effect identity nonreusable for process lifetime across families
	 * and mapped/zero kinds. Coordinator-local replay census is defense-in-depth only.
	 */
	class sqlite_shm_verified_reader_post_map_receipt
	{
	  public:
		[[nodiscard]] const sqlite_shm_reader_map_request& request() const noexcept;
		[[nodiscard]] std::uint64_t generation() const noexcept;
		[[nodiscard]] const sqlite_shm_mapping_tuple& mapping() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		zero_resize_effect_receipt() const noexcept;

	  private:
		friend class sqlite_same_process_shm_reader_receipt_validator;
		friend class sqlite_same_process_shm_lease_test_peer;

		sqlite_shm_verified_reader_post_map_receipt(
			sqlite_shm_reader_map_request request,
			std::uint64_t generation,
			sqlite_shm_mapping_tuple mapping,
			sqlite_backend_opaque_identity zero_resize_effect_receipt);

		sqlite_shm_reader_map_request request_;
		std::uint64_t generation_{};
		sqlite_shm_mapping_tuple mapping_;
		sqlite_backend_opaque_identity zero_resize_effect_receipt_;
	};

	/**
	 * Issuer-sealed proposal-group map receipt with post-map observed attachment evidence.
	 *
	 * The production issuer must bind the full process/family/attempt/request/callback/admission,
	 * observed attachment, and effect role and make the effect identity nonreusable for process
	 * lifetime across families and mapped/zero kinds. Coordinator-local replay census is
	 * defense-in-depth, not an issuer replacement.
	 */
	class sqlite_shm_verified_reader_attachment_post_map_receipt
	{
	  public:
		[[nodiscard]] const sqlite_shm_reader_attachment_map_request& request() const noexcept;
		[[nodiscard]] std::uint64_t generation() const noexcept;
		[[nodiscard]] const sqlite_shm_mapping_tuple& mapping() const noexcept;
		[[nodiscard]] const sqlite_shm_reader_native_attachment_identity&
		observed_attachment() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		zero_resize_effect_receipt() const noexcept;

	  private:
		friend class sqlite_same_process_shm_reader_receipt_validator;
		friend class sqlite_same_process_shm_lease_test_peer;

		sqlite_shm_verified_reader_attachment_post_map_receipt(
			sqlite_shm_reader_attachment_map_request request,
			std::uint64_t generation,
			sqlite_shm_mapping_tuple mapping,
			sqlite_shm_reader_native_attachment_identity observed_attachment,
			sqlite_backend_opaque_identity zero_resize_effect_receipt);

		sqlite_shm_reader_attachment_map_request request_;
		std::uint64_t generation_{};
		sqlite_shm_mapping_tuple mapping_;
		sqlite_shm_reader_native_attachment_identity observed_attachment_;
		sqlite_backend_opaque_identity zero_resize_effect_receipt_;
	};

	enum class sqlite_shm_lease_rejection_reason : std::uint8_t
	{
		invalid_identity,
		invalid_request,
		invalid_extend_pair,
		receipt_mismatch,
		stale_token,
		no_live_generation,
		pending_or_eligibility_only,
		retiring,
		successor_handoff_live,
		mapping_mismatch,
		stale_generation,
		generation_exhausted,
		lifecycle_ambiguous,
		quarantined,
	};

	enum class sqlite_shm_lease_recovery_action : std::uint8_t
	{
		deny_before_native_map,
		await_complete_attachment_gate_boundary,
		remove_pending_then_confirm_native_cleanup,
		attempt_nonremoving_unmap_then_outer_ioerr,
		resubmit_via_bound_route,
		outer_ioerr_no_retry,
		quarantine_no_retry,
	};

	struct sqlite_shm_lease_rejection
	{
		sqlite_shm_lease_rejection_reason reason{
			sqlite_shm_lease_rejection_reason::invalid_request};
		sqlite_shm_lease_recovery_action action{
			sqlite_shm_lease_recovery_action::deny_before_native_map};
	};

	template <class Value>
	class sqlite_shm_lease_result
	{
	  public:
		sqlite_shm_lease_result(Value value) : storage_{std::move(value)} {}
		sqlite_shm_lease_result(sqlite_shm_lease_rejection failure) : storage_{failure} {}

		[[nodiscard]] bool has_value() const noexcept
		{
			return std::holds_alternative<Value>(storage_);
		}
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return has_value();
		}
		[[nodiscard]] Value& value()
		{
			return std::get<Value>(storage_);
		}
		[[nodiscard]] const Value& value() const
		{
			return std::get<Value>(storage_);
		}
		[[nodiscard]] Value& operator*()
		{
			return value();
		}
		[[nodiscard]] const Value& operator*() const
		{
			return value();
		}
		[[nodiscard]] Value* operator->()
		{
			return &value();
		}
		[[nodiscard]] const Value* operator->() const
		{
			return &value();
		}
		[[nodiscard]] sqlite_shm_lease_rejection& error()
		{
			return std::get<sqlite_shm_lease_rejection>(storage_);
		}
		[[nodiscard]] const sqlite_shm_lease_rejection& error() const
		{
			return std::get<sqlite_shm_lease_rejection>(storage_);
		}

	  private:
		std::variant<Value, sqlite_shm_lease_rejection> storage_;
	};

	template <>
	class sqlite_shm_lease_result<void>
	{
	  public:
		sqlite_shm_lease_result() = default;
		sqlite_shm_lease_result(sqlite_shm_lease_rejection failure) : storage_{failure} {}

		[[nodiscard]] bool has_value() const noexcept
		{
			return std::holds_alternative<std::monostate>(storage_);
		}
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return has_value();
		}
		[[nodiscard]] sqlite_shm_lease_rejection& error()
		{
			return std::get<sqlite_shm_lease_rejection>(storage_);
		}
		[[nodiscard]] const sqlite_shm_lease_rejection& error() const
		{
			return std::get<sqlite_shm_lease_rejection>(storage_);
		}

	  private:
		std::variant<std::monostate, sqlite_shm_lease_rejection> storage_;
	};

	enum class sqlite_shm_native_cleanup_outcome : std::uint8_t
	{
		confirmed_success,
		non_ok,
		unknown,
	};

	enum class sqlite_shm_mapping_generation_phase : std::uint8_t
	{
		empty,
		live,
		retiring,
		retired,
		quarantined,
	};

	enum class sqlite_shm_writer_retirement_decision : std::uint8_t
	{
		not_last_attachment,
		ready,
		wait_for_inflight,
		quarantine_same_thread,
		quarantined,
	};

	enum class sqlite_shm_retirement_wait_failure : std::uint8_t
	{
		timeout,
		unknown,
	};

	enum class sqlite_shm_positive_writer_attachment_gate_progress : std::uint8_t
	{
		waiting,
		complete,
	};

	struct sqlite_shm_positive_writer_attachment_gate_result
	{
		sqlite_shm_positive_writer_attachment_gate_progress progress{
			sqlite_shm_positive_writer_attachment_gate_progress::waiting};
		std::vector<sqlite_shm_writer_holder> holders;
	};

	struct sqlite_shm_writer_retirement_result
	{
		sqlite_shm_writer_retirement_decision decision{
			sqlite_shm_writer_retirement_decision::not_last_attachment};
		std::uint64_t generation{};
	};

	struct sqlite_shm_mapping_lease_snapshot
	{
		sqlite_shm_mapping_generation_phase phase{sqlite_shm_mapping_generation_phase::empty};
		std::optional<std::uint64_t> generation;
		std::uint64_t sealed_shm_size{};
		std::size_t mapping_page_count{};
		std::size_t generation_authority_count{};
		std::size_t eligibility_count{};
		std::size_t writer_inflight_count{};
		std::size_t writer_cleanup_count{};
		std::size_t writer_holder_count{};
		/** Exact registry activity + weak audit seal + strong mapping-epoch arm bundles. */
		std::size_t writer_member_authority_count{};
		std::size_t writer_member_liveness_lost_count{};
		/** Retained attachment/member identities include non-reuse tombstones. */
		std::size_t writer_attachment_identity_count{};
		std::size_t writer_attachment_member_count{};
		/** Immutable per-map audit evidence retained by sealed attachment tombstones. */
		std::size_t writer_attachment_audit_member_count{};
		std::size_t writer_attachment_audit_native_mapping_count{};
		std::size_t writer_attachment_audit_post_map_count{};
		std::size_t writer_attachment_audit_promotion_count{};
		/**
		 * Unresolved-lifecycle counts include predelegate, pending, live, cleanup, and
		 * terminal-quarantined mandatory-drain tokens. They are census groundwork only and
		 * never imply live mapping or reader authority.
		 */
		std::size_t writer_attachment_unresolved_count{};
		std::size_t writer_attachment_unresolved_member_count{};
		std::size_t reader_inflight_count{};
		std::size_t reader_cleanup_count{};
		std::size_t reader_handoff_count{};
		std::size_t reader_attachment_group_count{};
		std::size_t reader_attachment_live_member_count{};
		std::size_t reader_attachment_audit_count{};
		std::size_t reader_session_reservation_count{};
		std::size_t reader_session_owner_count{};
		std::size_t reader_session_terminal_count{};
		std::size_t reader_attachment_zero_effect_terminal_count{};
		std::size_t reader_attachment_revoked_no_map_count{};
		std::size_t reader_registry_bound_group_count{};
		std::size_t reader_registry_bound_session_count{};
		std::size_t reader_registry_open_count{};
		std::size_t reader_open_close_owner_count{};
		std::size_t reader_close_admitted_count{};
		std::size_t reader_close_terminal_count{};
		std::size_t reader_open_close_tombstone_count{};
		std::size_t reader_registry_activity_authority_count{};
		std::size_t reader_registry_activity_liveness_lost_count{};
		bool reader_admission_visible{};
		bool quarantined{};
	};

	struct sqlite_shm_reader_open_epoch_test_view
	{
		std::uint64_t registry_open_token{};
		sqlite_shm_reader_open_epoch_binding binding;
		std::uint64_t close_owner_token{};
		detail::sqlite_shm_reader_connection_close_phase phase{
			detail::sqlite_shm_reader_connection_close_phase::open};
		std::uint64_t origin_sequence{};
		std::uint64_t close_cut_permit_slot{};
		std::uint64_t close_terminal_permit_slot{};
		std::uint64_t initial_close_cut_permit_slot{};
		std::uint64_t initial_close_terminal_permit_slot{};
		std::optional<sqlite_shm_reader_close_route> route;
		std::uint64_t close_cut_sequence{};
		std::uint64_t destination_sequence{};
	};

	struct sqlite_shm_reader_close_terminal_test_view
	{
		std::uint64_t registry_open_token{};
		sqlite_shm_reader_open_epoch_binding binding;
		std::uint64_t close_owner_token{};
		sqlite_shm_reader_close_route route{sqlite_shm_reader_close_route::close_without_group};
		sqlite_shm_reader_close_terminal_kind kind{
			sqlite_shm_reader_close_terminal_kind::terminal_quarantined};
		sqlite_shm_reader_close_evidence_kind evidence_kind{
			sqlite_shm_reader_close_evidence_kind::throw_or_unknown};
		std::optional<int> native_status;
		int outward_status{};
		std::optional<sqlite_backend_opaque_identity> native_effect_receipt;
		sqlite_shm_callback_execution_receipt callback;
		bool exact_terminal_receipt_retained{};
		detail::sqlite_shm_reader_terminal_quarantine_reason reason{
			detail::sqlite_shm_reader_terminal_quarantine_reason::none};
		std::uint64_t terminal_sequence{};
	};

	struct sqlite_shm_reader_attachment_reservation_test_view
	{
		sqlite_shm_reader_attachment_reservation_identity attachment;
		detail::sqlite_shm_reader_attachment_reservation_phase phase{
			detail::sqlite_shm_reader_attachment_reservation_phase::reserved};
		std::uint64_t origin_sequence{};
		std::uint64_t destination_sequence{};
		bool group_payload_present{};
	};

	struct sqlite_shm_reader_session_reservation_test_view
	{
		std::uint64_t session_token{};
		sqlite_shm_reader_attachment_reservation_identity attachment;
		detail::sqlite_shm_reader_session_reservation_phase phase{
			detail::sqlite_shm_reader_session_reservation_phase::reserved_before_sqlite};
		std::uint64_t origin_sequence{};
		std::uint64_t destination_sequence{};
		std::uint64_t terminal_permit_slot{};
	};

	struct sqlite_shm_reader_attachment_group_test_view
	{
		sqlite_shm_reader_attachment_reservation_identity attachment;
		detail::sqlite_shm_reader_attachment_group_phase phase{
			detail::sqlite_shm_reader_attachment_group_phase::active};
		std::uint64_t origin_sequence{};
		std::uint64_t destination_sequence{};
		std::uint64_t unmap_cut_permit_slot{};
		std::uint64_t unmap_terminal_permit_slot{};
	};

	struct sqlite_shm_reader_map_attempt_test_view
	{
		std::uint64_t map_token{};
		sqlite_shm_reader_attachment_reservation_identity attachment;
		std::uint64_t admission_sequence{};
		std::uint64_t terminal_permit_slot{};
		std::uint64_t potential_group_cut_permit_slot{};
		std::uint64_t potential_group_terminal_permit_slot{};
	};

	struct sqlite_shm_reader_lifecycle_event_test_view
	{
		std::uint64_t sequence{};
		detail::sqlite_shm_reader_lifecycle_event_kind kind{
			detail::sqlite_shm_reader_lifecycle_event_kind::session_start_admission};
		std::uint64_t owner_token{};
	};

	struct sqlite_shm_reader_terminal_quarantine_test_view
	{
		std::uint64_t owner_token{};
		sqlite_shm_reader_attachment_reservation_identity attachment;
		detail::sqlite_shm_reader_terminal_quarantine_reason reason{
			detail::sqlite_shm_reader_terminal_quarantine_reason::none};
		std::uint64_t terminal_sequence{};
		std::optional<sqlite_shm_callback_execution_receipt> callback;
		std::optional<sqlite_backend_opaque_identity> native_effect_receipt;
		bool exact_terminal_receipt_retained{};
		std::optional<sqlite_shm_reader_unmap_evidence_kind> unmap_evidence_kind;
		std::optional<int> native_status;
		std::optional<sqlite_backend_opaque_identity> latch_reset_receipt;
	};

	struct sqlite_shm_reader_zero_effect_terminal_test_view
	{
		std::uint64_t owner_token{};
		sqlite_shm_reader_attachment_reservation_identity attachment;
		std::uint64_t terminal_sequence{};
		sqlite_shm_reader_attachment_zero_effect_kind kind{
			sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change};
		int native_status{};
		std::optional<sqlite_shm_callback_execution_receipt> callback;
		std::optional<sqlite_backend_opaque_identity> zero_attachment_effect_receipt;
		bool exact_terminal_receipt_retained{};
		bool revoked_first_reservation{};
	};

	/** Test-only closed projection of the activated DF-0207 reader ledger. */
	struct sqlite_shm_reader_lifecycle_test_view
	{
		const void* sequence_source_identity{};
		std::uint64_t last_issued_sequence{};
		std::uint64_t last_committed_sequence{};
		std::size_t outstanding_terminal_permit_count{};
		std::vector<std::uint64_t> outstanding_terminal_permit_slots;
		bool sequence_source_exhausted{};
		std::array<std::size_t, detail::sqlite_shm_reader_attachment_reservation_phases.size()>
			attachment_reservation_phase_counts{};
		std::array<std::size_t, detail::sqlite_shm_reader_session_reservation_phases.size()>
			session_reservation_phase_counts{};
		std::array<std::size_t, detail::sqlite_shm_reader_custody_kinds.size()>
			live_custody_kind_counts{};
		std::array<std::size_t, detail::sqlite_shm_reader_custody_states.size()>
			custody_state_counts{};
		std::size_t compact_tombstone_count{};
		std::size_t open_epoch_close_compact_tombstone_count{};
		std::vector<sqlite_shm_reader_attachment_reservation_test_view> attachment_reservations;
		std::vector<sqlite_shm_reader_session_reservation_test_view> session_reservations;
		std::vector<sqlite_shm_reader_attachment_group_test_view> attachment_groups;
		std::vector<sqlite_shm_reader_map_attempt_test_view> map_attempts;
		std::vector<sqlite_shm_reader_open_epoch_test_view> open_epochs;
		std::vector<sqlite_shm_reader_close_terminal_test_view> close_terminals;
		std::vector<sqlite_shm_reader_terminal_quarantine_test_view> terminal_quarantines;
		std::vector<sqlite_shm_reader_zero_effect_terminal_test_view> zero_effect_terminals;
		std::vector<sqlite_shm_reader_lifecycle_event_test_view> events;
	};

	/**
	 * Process-instance-wide checked monotonic generation source shared by family coordinators.
	 *
	 * The future process-global registry owns exactly one source for a sealed process identity.
	 * Direct construction remains available only for this production-inert coordinator unit and
	 * its tests until that registry is introduced.
	 */
	class sqlite_shm_mapping_generation_source
	{
	  public:
		explicit sqlite_shm_mapping_generation_source(std::uint64_t first_generation = 1U);
		~sqlite_shm_mapping_generation_source();

		sqlite_shm_mapping_generation_source(const sqlite_shm_mapping_generation_source&) = delete;
		sqlite_shm_mapping_generation_source&
		operator=(const sqlite_shm_mapping_generation_source&) = delete;
		sqlite_shm_mapping_generation_source(sqlite_shm_mapping_generation_source&&) = delete;
		sqlite_shm_mapping_generation_source&
		operator=(sqlite_shm_mapping_generation_source&&) = delete;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		struct state;
		std::shared_ptr<state> state_;
	};

	/**
	 * One checked sequence domain shared by every reader family coordinator in one registry epoch.
	 *
	 * A batch is minted contiguously or not at all. Zero, wrap, reuse, and partial allocation are
	 * impossible; exhaustion permanently fails closed.
	 */
	class sqlite_shm_reader_lifecycle_sequence_source
	{
	  public:
		explicit sqlite_shm_reader_lifecycle_sequence_source(std::uint64_t first_sequence = 1U);
		~sqlite_shm_reader_lifecycle_sequence_source();

		sqlite_shm_reader_lifecycle_sequence_source(
			const sqlite_shm_reader_lifecycle_sequence_source&) = delete;
		sqlite_shm_reader_lifecycle_sequence_source&
		operator=(const sqlite_shm_reader_lifecycle_sequence_source&) = delete;
		sqlite_shm_reader_lifecycle_sequence_source(sqlite_shm_reader_lifecycle_sequence_source&&) =
			delete;
		sqlite_shm_reader_lifecycle_sequence_source&
		operator=(sqlite_shm_reader_lifecycle_sequence_source&&) = delete;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_same_process_shm_lease_test_peer;
		[[nodiscard]] std::uint64_t observed_last_issued() const noexcept;
		[[nodiscard]] const void* identity_for_testing() const noexcept;
		[[nodiscard]] std::uint64_t last_issued_for_testing() const noexcept;
		void exhaust_for_testing() noexcept;
		void make_unavailable_for_testing() noexcept;
		struct state;
		std::shared_ptr<state> state_;
	};

	class sqlite_shm_writer_eligibility
	{
	  public:
		~sqlite_shm_writer_eligibility() noexcept;
		sqlite_shm_writer_eligibility(sqlite_shm_writer_eligibility&&) noexcept;
		sqlite_shm_writer_eligibility& operator=(sqlite_shm_writer_eligibility&&) = delete;
		sqlite_shm_writer_eligibility(const sqlite_shm_writer_eligibility&) = delete;
		sqlite_shm_writer_eligibility& operator=(const sqlite_shm_writer_eligibility&) = delete;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		explicit sqlite_shm_writer_eligibility(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			std::uint64_t token) noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
	};

	class sqlite_shm_writer_map_inflight
	{
	  public:
		~sqlite_shm_writer_map_inflight() noexcept;
		sqlite_shm_writer_map_inflight(sqlite_shm_writer_map_inflight&&) noexcept;
		sqlite_shm_writer_map_inflight& operator=(sqlite_shm_writer_map_inflight&&) = delete;
		sqlite_shm_writer_map_inflight(const sqlite_shm_writer_map_inflight&) = delete;
		sqlite_shm_writer_map_inflight& operator=(const sqlite_shm_writer_map_inflight&) = delete;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class sqlite_shm_verified_writer_native_map_receipt;
		friend class sqlite_writer_shm_native_map_receipt_validator;
		explicit sqlite_shm_writer_map_inflight(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			std::uint64_t token) noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
		bool native_result_validated_{};
		bool native_result_observed_{};
		bool native_result_validation_ambiguous_{};
	};

	/**
	 * Cleanup-only seal produced immediately after exact native SQLITE_OK/non-null.
	 *
	 * This receipt binds the native pointer to one exact predelegate token. It grants no
	 * pending, generation, holder, or reader authority; a complete post-map receipt is still
	 * required before pending installation.
	 */
	class sqlite_shm_verified_writer_native_map_receipt
	{
	  public:
		[[nodiscard]] const volatile void* native_mapping() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class sqlite_writer_shm_native_map_receipt_validator;
		friend class sqlite_same_process_shm_lease_test_peer;

		sqlite_shm_verified_writer_native_map_receipt(
			const sqlite_shm_writer_map_inflight& inflight,
			const volatile void* native_mapping) noexcept;

		std::weak_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
		const volatile void* native_mapping_{};
	};

	/**
	 * Closed validator for one writer native-map callback result.
	 *
	 * Exact native SQLITE_OK/non-null is the only shape which can mint a cleanup-only receipt.
	 * Validation is one-shot for an exact in-flight token. A non-null input records that a native
	 * mapping was observed, but validation does not consume the token; the coordinator transition
	 * remains the sole consumer. A first null result leaves the separate no-map resolution
	 * available but cannot be replaced by a later validation. This internal validator grants no
	 * mapping, generation, holder, or reader authority by itself.
	 */
	class sqlite_writer_shm_native_map_receipt_validator final
	{
	  public:
		sqlite_writer_shm_native_map_receipt_validator() = delete;

		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_verified_writer_native_map_receipt>
		validate(sqlite_shm_writer_map_inflight& inflight,
				 int native_status,
				 const volatile void* native_mapping) noexcept;
	};

	class sqlite_shm_writer_post_native_mapping
	{
	  public:
		~sqlite_shm_writer_post_native_mapping() noexcept;
		sqlite_shm_writer_post_native_mapping(sqlite_shm_writer_post_native_mapping&&) noexcept;
		sqlite_shm_writer_post_native_mapping&
		operator=(sqlite_shm_writer_post_native_mapping&&) = delete;
		sqlite_shm_writer_post_native_mapping(const sqlite_shm_writer_post_native_mapping&) =
			delete;
		sqlite_shm_writer_post_native_mapping&
		operator=(const sqlite_shm_writer_post_native_mapping&) = delete;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		explicit sqlite_shm_writer_post_native_mapping(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			std::uint64_t token) noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
	};

	class sqlite_shm_pending_mapping
	{
	  public:
		~sqlite_shm_pending_mapping() noexcept;
		sqlite_shm_pending_mapping(sqlite_shm_pending_mapping&&) noexcept;
		sqlite_shm_pending_mapping& operator=(sqlite_shm_pending_mapping&&) = delete;
		sqlite_shm_pending_mapping(const sqlite_shm_pending_mapping&) = delete;
		sqlite_shm_pending_mapping& operator=(const sqlite_shm_pending_mapping&) = delete;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		explicit sqlite_shm_pending_mapping(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			std::uint64_t token) noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
	};

	/**
	 * The sole move-only native cleanup owner for one exact writer attachment.
	 *
	 * Although a map wrapper is used as the lookup anchor, successful admission seals the
	 * coordinator-derived complete attachment member prefix. No per-map wrapper owns cleanup.
	 */
	class sqlite_shm_writer_attachment_cleanup
	{
	  public:
		~sqlite_shm_writer_attachment_cleanup() noexcept;
		sqlite_shm_writer_attachment_cleanup(sqlite_shm_writer_attachment_cleanup&&) noexcept;
		sqlite_shm_writer_attachment_cleanup&
		operator=(sqlite_shm_writer_attachment_cleanup&&) = delete;
		sqlite_shm_writer_attachment_cleanup(const sqlite_shm_writer_attachment_cleanup&) = delete;
		sqlite_shm_writer_attachment_cleanup&
		operator=(const sqlite_shm_writer_attachment_cleanup&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] std::uint64_t generation() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		explicit sqlite_shm_writer_attachment_cleanup(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			detail::sqlite_shm_lease_token_identity token,
			detail::sqlite_shm_mapping_generation_identity generation) noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
		std::uint64_t generation_{};
	};

	class sqlite_shm_writer_holder
	{
	  public:
		~sqlite_shm_writer_holder() noexcept;
		sqlite_shm_writer_holder(sqlite_shm_writer_holder&&) noexcept;
		sqlite_shm_writer_holder& operator=(sqlite_shm_writer_holder&&) = delete;
		sqlite_shm_writer_holder(const sqlite_shm_writer_holder&) = delete;
		sqlite_shm_writer_holder& operator=(const sqlite_shm_writer_holder&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] std::uint64_t generation() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		explicit sqlite_shm_writer_holder(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			detail::sqlite_shm_lease_token_identity token,
			detail::sqlite_shm_mapping_generation_identity generation) noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
		std::uint64_t generation_{};
	};

	class sqlite_shm_reader_map_inflight
	{
	  public:
		~sqlite_shm_reader_map_inflight() noexcept;
		sqlite_shm_reader_map_inflight(sqlite_shm_reader_map_inflight&&) noexcept;
		sqlite_shm_reader_map_inflight& operator=(sqlite_shm_reader_map_inflight&&) = delete;
		sqlite_shm_reader_map_inflight(const sqlite_shm_reader_map_inflight&) = delete;
		sqlite_shm_reader_map_inflight& operator=(const sqlite_shm_reader_map_inflight&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] std::uint64_t generation() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		explicit sqlite_shm_reader_map_inflight(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			detail::sqlite_shm_lease_token_identity token,
			detail::sqlite_shm_mapping_generation_identity generation) noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
		std::uint64_t generation_{};
	};

	class sqlite_shm_reader_attachment_map_inflight
	{
	  public:
		~sqlite_shm_reader_attachment_map_inflight() noexcept;
		sqlite_shm_reader_attachment_map_inflight(
			sqlite_shm_reader_attachment_map_inflight&&) noexcept;
		sqlite_shm_reader_attachment_map_inflight&
		operator=(sqlite_shm_reader_attachment_map_inflight&&) = delete;
		sqlite_shm_reader_attachment_map_inflight(
			const sqlite_shm_reader_attachment_map_inflight&) = delete;
		sqlite_shm_reader_attachment_map_inflight&
		operator=(const sqlite_shm_reader_attachment_map_inflight&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] std::uint64_t generation() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class sqlite_shm_verified_reader_attachment_zero_effect_receipt;
		explicit sqlite_shm_reader_attachment_map_inflight(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			detail::sqlite_shm_lease_token_identity token,
			detail::sqlite_shm_mapping_generation_identity generation) noexcept;
		void disable_terminal_presentation() noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
		std::uint64_t generation_{};
		bool terminal_presentation_disabled_{};
	};

	enum class sqlite_shm_reader_session_phase : std::uint8_t
	{
		reserved_for_first_map,
		active_group_owner,
	};

	/**
	 * Move-only pre-SQLite owner for one exact read/decode session.
	 *
	 * Before a first map it owns one reservation. For an existing group, or after the first
	 * positive map commit, it owns one active group/session edge. Destruction is fail-closed and
	 * no field equality can recreate the owner.
	 */
	class sqlite_shm_reader_session
	{
	  public:
		~sqlite_shm_reader_session() noexcept;
		sqlite_shm_reader_session(sqlite_shm_reader_session&&) noexcept;
		sqlite_shm_reader_session& operator=(sqlite_shm_reader_session&&) = delete;
		sqlite_shm_reader_session(const sqlite_shm_reader_session&) = delete;
		sqlite_shm_reader_session& operator=(const sqlite_shm_reader_session&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] std::uint64_t generation() const noexcept;
		[[nodiscard]] sqlite_shm_reader_session_phase phase() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		explicit sqlite_shm_reader_session(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			detail::sqlite_shm_lease_token_identity token,
			detail::sqlite_shm_mapping_generation_identity generation,
			sqlite_shm_reader_session_phase phase) noexcept;
		void disable_terminal_presentation() noexcept;
		void promote_to_active() noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
		std::uint64_t generation_{};
		sqlite_shm_reader_session_phase phase_{
			sqlite_shm_reader_session_phase::reserved_for_first_map};
		bool terminal_presentation_disabled_{};
	};

	class sqlite_shm_reader_cleanup_obligation
	{
	  public:
		~sqlite_shm_reader_cleanup_obligation() noexcept;
		sqlite_shm_reader_cleanup_obligation(sqlite_shm_reader_cleanup_obligation&&) noexcept;
		sqlite_shm_reader_cleanup_obligation&
		operator=(sqlite_shm_reader_cleanup_obligation&&) = delete;
		sqlite_shm_reader_cleanup_obligation(const sqlite_shm_reader_cleanup_obligation&) = delete;
		sqlite_shm_reader_cleanup_obligation&
		operator=(const sqlite_shm_reader_cleanup_obligation&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] std::uint64_t generation() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		explicit sqlite_shm_reader_cleanup_obligation(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			detail::sqlite_shm_lease_token_identity token,
			detail::sqlite_shm_mapping_generation_identity generation) noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
		std::uint64_t generation_{};
	};

	class sqlite_shm_reader_handoff
	{
	  public:
		~sqlite_shm_reader_handoff() noexcept;
		sqlite_shm_reader_handoff(sqlite_shm_reader_handoff&&) noexcept;
		sqlite_shm_reader_handoff& operator=(sqlite_shm_reader_handoff&&) = delete;
		sqlite_shm_reader_handoff(const sqlite_shm_reader_handoff&) = delete;
		sqlite_shm_reader_handoff& operator=(const sqlite_shm_reader_handoff&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] std::uint64_t generation() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class sqlite_same_process_shm_lease_test_peer;
		explicit sqlite_shm_reader_handoff(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			detail::sqlite_shm_lease_token_identity token,
			detail::sqlite_shm_mapping_generation_identity generation) noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
		std::uint64_t generation_{};
	};

	enum class sqlite_shm_reader_map_commit_kind : std::uint8_t
	{
		first_member,
		new_member,
		existing_member_revalidation,
	};

	/**
	 * Positive atomic map commit.
	 *
	 * Only a first-member commit carries a newly minted group handoff. The session owner is the
	 * same exact move-only token supplied to the commit and has already transitioned to active.
	 */
	class sqlite_shm_reader_map_commit
	{
	  public:
		sqlite_shm_reader_map_commit(sqlite_shm_reader_map_commit&&) noexcept;
		sqlite_shm_reader_map_commit& operator=(sqlite_shm_reader_map_commit&&) = delete;
		sqlite_shm_reader_map_commit(const sqlite_shm_reader_map_commit&) = delete;
		sqlite_shm_reader_map_commit& operator=(const sqlite_shm_reader_map_commit&) = delete;

		[[nodiscard]] sqlite_shm_reader_map_commit_kind kind() const noexcept;
		[[nodiscard]] const sqlite_shm_mapping_tuple& mapping() const noexcept;
		[[nodiscard]] bool formed_group() const noexcept;
		[[nodiscard]] std::optional<sqlite_shm_reader_handoff> take_handoff() noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		sqlite_shm_reader_map_commit(sqlite_shm_reader_map_commit_kind kind,
									 sqlite_shm_mapping_tuple mapping,
									 std::optional<sqlite_shm_reader_handoff> handoff) noexcept;

		sqlite_shm_reader_map_commit_kind kind_{
			sqlite_shm_reader_map_commit_kind::existing_member_revalidation};
		sqlite_shm_mapping_tuple mapping_;
		std::optional<sqlite_shm_reader_handoff> handoff_;
	};

	class sqlite_shm_reader_unmap_obligation
	{
	  public:
		~sqlite_shm_reader_unmap_obligation() noexcept;
		sqlite_shm_reader_unmap_obligation(sqlite_shm_reader_unmap_obligation&&) noexcept;
		sqlite_shm_reader_unmap_obligation&
		operator=(sqlite_shm_reader_unmap_obligation&&) = delete;
		sqlite_shm_reader_unmap_obligation(const sqlite_shm_reader_unmap_obligation&) = delete;
		sqlite_shm_reader_unmap_obligation&
		operator=(const sqlite_shm_reader_unmap_obligation&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] std::uint64_t generation() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class sqlite_shm_verified_reader_unmap_terminal_receipt;
		explicit sqlite_shm_reader_unmap_obligation(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			detail::sqlite_shm_lease_token_identity token,
			detail::sqlite_shm_mapping_generation_identity generation) noexcept;
		void disable_terminal_presentation() noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
		std::uint64_t generation_{};
		bool terminal_presentation_disabled_{};
	};

	/**
	 * Move-only Phase-1 xClose authority for one exact authenticated reader open epoch.
	 *
	 * It is minted only by consuming the orthogonal xOpen close owner through one sequenced close
	 * cut. Active attachment, logical-ack, opaque, predecessor, and composite routes cannot be
	 * represented by this checkpoint type.
	 */
	class sqlite_shm_reader_close_obligation
	{
	  public:
		~sqlite_shm_reader_close_obligation() noexcept;
		sqlite_shm_reader_close_obligation(sqlite_shm_reader_close_obligation&&) noexcept;
		sqlite_shm_reader_close_obligation&
		operator=(sqlite_shm_reader_close_obligation&&) = delete;
		sqlite_shm_reader_close_obligation(const sqlite_shm_reader_close_obligation&) = delete;
		sqlite_shm_reader_close_obligation&
		operator=(const sqlite_shm_reader_close_obligation&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] sqlite_shm_reader_close_route route() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		friend class sqlite_shm_verified_reader_close_terminal_receipt;
		friend class sqlite_same_process_shm_lease_test_peer;

		sqlite_shm_reader_close_obligation(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			detail::sqlite_shm_lease_token_identity owner_token,
			std::uint64_t registry_open_token,
			sqlite_shm_reader_close_route route) noexcept;
		void disable_terminal_presentation() noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t owner_token_{};
		std::uint64_t registry_open_token_{};
		sqlite_shm_reader_close_route route_{sqlite_shm_reader_close_route::close_without_group};
		bool terminal_presentation_disabled_{};
	};

	class sqlite_shm_writer_release
	{
	  public:
		~sqlite_shm_writer_release() noexcept;
		sqlite_shm_writer_release(sqlite_shm_writer_release&&) noexcept;
		sqlite_shm_writer_release& operator=(sqlite_shm_writer_release&&) = delete;
		sqlite_shm_writer_release(const sqlite_shm_writer_release&) = delete;
		sqlite_shm_writer_release& operator=(const sqlite_shm_writer_release&) = delete;

		[[nodiscard]] sqlite_shm_writer_retirement_decision decision() const noexcept;
		[[nodiscard]] std::uint64_t generation() const noexcept;
		[[nodiscard]] sqlite_shm_writer_attachment_cleanup& cleanup() noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_lease_state;
		sqlite_shm_writer_release(sqlite_shm_writer_retirement_decision decision,
								  std::uint64_t generation,
								  sqlite_shm_writer_attachment_cleanup cleanup) noexcept;

		sqlite_shm_writer_retirement_decision decision_{
			sqlite_shm_writer_retirement_decision::not_last_attachment};
		std::uint64_t generation_{};
		sqlite_shm_writer_attachment_cleanup cleanup_;
	};

	/**
	 * Pure, callback-free lifecycle coordinator. It never performs native I/O or waits. A future
	 * process-global registry serializes native callback boundaries around these transitions.
	 * Coordinator state is safe across threads, but each move-only token object has one thread
	 * owner and must not be concurrently read or consumed through the same object.
	 *
	 * Quarantine rejects new mapping authority. A distinct pre-existing native effect or
	 * attachment still retains exactly one mandatory cleanup-admission attempt; that source token
	 * is consumed before receipt storage, and any admission or storage failure is terminal.
	 *
	 * This production-inert checkpoint deliberately fails closed before exposing cleanup when a
	 * nonlast attachment is sole support for any generation page, when its boundary still has an
	 * inflight member, or when post-native/pending failure state is mixed with a live holder.
	 * Reader-predelegate ordering and those quarantined drain paths remain later-slice work.
	 */
	class sqlite_same_process_shm_mapping_lease_coordinator
	{
	  public:
		sqlite_same_process_shm_mapping_lease_coordinator(
			sqlite_shm_lease_family_binding family,
			std::shared_ptr<sqlite_shm_mapping_generation_source> generations);
		sqlite_same_process_shm_mapping_lease_coordinator(
			sqlite_shm_lease_family_binding family,
			std::shared_ptr<sqlite_shm_mapping_generation_source> generations,
			std::shared_ptr<sqlite_shm_reader_lifecycle_sequence_source>
				reader_lifecycle_sequences);
		~sqlite_same_process_shm_mapping_lease_coordinator() noexcept;

		sqlite_same_process_shm_mapping_lease_coordinator(
			const sqlite_same_process_shm_mapping_lease_coordinator&) = delete;
		sqlite_same_process_shm_mapping_lease_coordinator&
		operator=(const sqlite_same_process_shm_mapping_lease_coordinator&) = delete;
		sqlite_same_process_shm_mapping_lease_coordinator(
			sqlite_same_process_shm_mapping_lease_coordinator&&) = delete;
		sqlite_same_process_shm_mapping_lease_coordinator&
		operator=(sqlite_same_process_shm_mapping_lease_coordinator&&) = delete;

		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_eligibility>
		install_writer_eligibility(const sqlite_shm_verified_writer_eligibility_receipt& receipt);
		[[nodiscard]] sqlite_shm_lease_result<void>
		revoke_writer_eligibility(sqlite_shm_writer_eligibility& eligibility) noexcept;

		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_map_inflight>
		begin_writer_map(const sqlite_shm_writer_map_request& request);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_post_native_mapping>
		record_writer_native_mapping(
			sqlite_shm_writer_map_inflight& inflight,
			const sqlite_shm_verified_writer_native_map_receipt& receipt) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_pending_mapping>
		install_pending(sqlite_shm_writer_post_native_mapping& post_native,
						const sqlite_shm_verified_writer_post_map_receipt& receipt);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_holder>
		promote_writer(sqlite_shm_pending_mapping& pending,
					   const sqlite_shm_writer_eligibility& eligibility);
		[[nodiscard]] sqlite_shm_lease_result<void>
		resolve_writer_map_failure(sqlite_shm_writer_map_inflight& inflight) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_attachment_cleanup>
		begin_writer_cleanup(sqlite_shm_writer_post_native_mapping& rejected_mapping,
							 const sqlite_shm_callback_execution_receipt& callback) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_attachment_cleanup>
		begin_writer_cleanup(sqlite_shm_pending_mapping& pending,
							 const sqlite_shm_callback_execution_receipt& callback) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void>
		complete_writer_cleanup(sqlite_shm_writer_attachment_cleanup& cleanup,
								const sqlite_shm_callback_execution_receipt& callback,
								sqlite_shm_native_cleanup_outcome outcome) noexcept;

		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_map_inflight>
		begin_reader_map(const sqlite_shm_reader_map_request& request);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_handoff>
		promote_reader(sqlite_shm_reader_map_inflight& inflight,
					   const sqlite_shm_verified_reader_post_map_receipt& receipt);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_session>
		begin_reader_session(const sqlite_shm_reader_session_request& request);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_attachment_map_inflight>
		begin_reader_map(sqlite_shm_reader_session& session,
						 const sqlite_shm_reader_attachment_map_request& request);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_map_commit>
		commit_reader_map(sqlite_shm_reader_attachment_map_inflight& inflight,
						  const sqlite_shm_verified_reader_attachment_post_map_receipt& receipt,
						  sqlite_shm_reader_session& session);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_attachment_zero_effect_result>
		complete_reader_zero_attachment_map(
			sqlite_shm_reader_attachment_map_inflight& inflight,
			const sqlite_shm_verified_reader_attachment_zero_effect_receipt& receipt,
			sqlite_shm_reader_session& session);
		[[nodiscard]] sqlite_shm_lease_result<void>
		complete_reader_session(sqlite_shm_reader_session& session,
								const sqlite_shm_reader_session_terminal_receipt& receipt) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void>
		resolve_reader_map_failure(sqlite_shm_reader_map_inflight& inflight) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_cleanup_obligation>
		begin_reader_cleanup(sqlite_shm_reader_map_inflight& rejected_mapping,
							 const sqlite_shm_callback_execution_receipt& callback) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void>
		complete_reader_cleanup(sqlite_shm_reader_cleanup_obligation& cleanup,
								const sqlite_shm_callback_execution_receipt& callback,
								sqlite_shm_native_cleanup_outcome outcome) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unmap_obligation>
		begin_reader_unmap(sqlite_shm_reader_handoff& handoff,
						   const sqlite_shm_callback_execution_receipt& callback) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unmap_obligation>
		begin_reader_unmap(sqlite_shm_reader_handoff& handoff,
						   const sqlite_shm_reader_unmap_request& request) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void>
		complete_reader_unmap(sqlite_shm_reader_unmap_obligation& unmap,
							  const sqlite_shm_callback_execution_receipt& callback,
							  sqlite_shm_native_cleanup_outcome outcome) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unmap_terminal_result>
		complete_reader_unmap(
			sqlite_shm_reader_unmap_obligation& unmap,
			const sqlite_shm_verified_reader_unmap_terminal_receipt& receipt) noexcept;

		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_release>
		release_writer_holder(sqlite_shm_writer_holder& holder,
							  const sqlite_shm_callback_execution_receipt& callback) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_retirement_result>
		poll_writer_retirement(const sqlite_shm_writer_attachment_cleanup& cleanup,
							   const sqlite_shm_callback_execution_receipt& callback) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void>
		fail_writer_retirement_wait(const sqlite_shm_writer_attachment_cleanup& cleanup,
									const sqlite_shm_callback_execution_receipt& callback,
									sqlite_shm_retirement_wait_failure failure) noexcept;

		[[nodiscard]] sqlite_shm_mapping_lease_snapshot snapshot() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_same_process_shm_lease_test_peer;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_map_inflight>
		begin_registry_writer_map(const sqlite_shm_writer_map_request& request,
								  sqlite_shm_writer_member_authority& authority);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_pending_mapping>
		install_registry_writer_pending(sqlite_shm_registry_family_pin& family,
										sqlite_shm_writer_post_native_mapping& post_native,
										sqlite_shm_verified_writer_post_map_receipt receipt);
		[[nodiscard]] sqlite_shm_lease_result<std::vector<sqlite_shm_writer_holder>>
		promote_registry_writer_attachment_group(sqlite_shm_registry_family_pin& family,
												 std::span<sqlite_shm_pending_mapping*> pending,
												 const sqlite_shm_writer_eligibility& eligibility);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_positive_writer_attachment_gate_result>
		advance_positive_registry_writer_attachment_gate(
			sqlite_shm_registry_family_pin& family,
			const sqlite_shm_native_attachment_identity& attachment,
			std::span<sqlite_shm_pending_mapping*> pending,
			const sqlite_shm_writer_eligibility& eligibility);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_holder>
		complete_gate_winning_registry_writer_map_before_callback_return(
			sqlite_shm_registry_family_pin& family,
			sqlite_shm_writer_post_native_mapping& post_native,
			const sqlite_shm_verified_writer_post_map_receipt& receipt);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_session_admission>
		admit_registry_reader_session(
			sqlite_shm_registry_family_pin& family,
			std::uint64_t registry_open_token,
			const sqlite_shm_reader_pre_sqlite_session_request& request,
			sqlite_shm_reader_candidate_authority_minter& candidate_minter);
		[[nodiscard]] sqlite_shm_lease_result<void> register_registry_reader_open(
			std::uint64_t registry_open_token,
			const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
			const sqlite_shm_reader_open_epoch_binding& binding);
		[[nodiscard]] sqlite_shm_lease_result<void> register_registry_reader_open(
			std::uint64_t registry_open_token,
			const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
			const sqlite_shm_reader_open_epoch_binding& binding,
			const detail::sqlite_shm_reader_open_admission_guard& admission_guard);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_close_obligation>
		begin_registry_reader_close(
			std::uint64_t registry_open_token,
			const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
			const sqlite_shm_reader_open_epoch_binding& binding,
			const sqlite_shm_reader_close_request& request) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_close_terminal_result>
		complete_registry_reader_close(
			std::uint64_t registry_open_token,
			const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
			const sqlite_shm_reader_open_epoch_binding& binding,
			sqlite_shm_reader_close_obligation& close,
			const sqlite_shm_verified_reader_close_terminal_receipt& receipt) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void> release_registry_reader_open(
			std::uint64_t registry_open_token,
			const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_attachment_map_inflight>
		begin_registry_reader_map(sqlite_shm_registry_family_pin& family,
								  sqlite_shm_reader_session& session,
								  const sqlite_shm_reader_attachment_map_request& request,
								  sqlite_shm_reader_map_predelegate_minter& predelegate_minter);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_map_commit>
		commit_registry_reader_map(
			sqlite_shm_registry_family_pin& family,
			sqlite_shm_reader_attachment_map_inflight& inflight,
			const sqlite_shm_verified_reader_attachment_post_map_receipt& receipt,
			sqlite_shm_reader_session& session,
			std::optional<sqlite_shm_reader_map_predelegate_authority>& completed_predelegate);
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_attachment_zero_effect_result>
		complete_registry_reader_zero_attachment_map(
			sqlite_shm_registry_family_pin& family,
			sqlite_shm_reader_attachment_map_inflight& inflight,
			const sqlite_shm_verified_reader_attachment_zero_effect_receipt& receipt,
			sqlite_shm_reader_session& session,
			std::optional<sqlite_shm_reader_map_predelegate_authority>& completed_predelegate,
			std::optional<sqlite_shm_reader_attachment_authority>& completed_candidate);
		[[nodiscard]] sqlite_shm_lease_result<void> complete_registry_reader_session(
			sqlite_shm_reader_session& session,
			const sqlite_shm_reader_session_terminal_receipt& receipt,
			std::optional<sqlite_shm_reader_attachment_authority>& completed_activity) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unmap_obligation>
		begin_registry_reader_unmap(sqlite_shm_reader_handoff& handoff,
									const sqlite_shm_callback_execution_receipt& callback) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unmap_terminal_result>
		complete_registry_reader_unmap(
			sqlite_shm_reader_unmap_obligation& unmap,
			const sqlite_shm_verified_reader_unmap_terminal_receipt& receipt,
			std::optional<sqlite_shm_reader_attachment_authority>& completed_activity) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<
			std::vector<sqlite_shm_reader_lifecycle_compact_tombstone>>
		export_registry_reader_lifecycle_tombstones() const;
		[[nodiscard]] sqlite_shm_lease_result<void> import_registry_reader_lifecycle_tombstones(
			std::span<const sqlite_shm_reader_lifecycle_compact_tombstone> tombstones);
		[[nodiscard]] sqlite_shm_lease_result<void> check_registry_reader_lifecycle_tombstone(
			const sqlite_shm_reader_attachment_reservation_identity& attachment) const noexcept;
		[[nodiscard]] sqlite_shm_lease_result<
			std::vector<sqlite_shm_reader_open_epoch_close_tombstone>>
		export_registry_reader_open_epoch_close_tombstones() const;
		[[nodiscard]] sqlite_shm_lease_result<void>
		import_registry_reader_open_epoch_close_tombstones(
			std::span<const sqlite_shm_reader_open_epoch_close_tombstone> tombstones);
		[[nodiscard]] sqlite_shm_lease_result<void>
		check_registry_reader_open_epoch_close_tombstone(
			std::uint64_t registry_open_token,
			const sqlite_shm_reader_open_epoch_binding& binding) const noexcept;
		[[nodiscard]] sqlite_shm_reader_lifecycle_test_view
		reader_lifecycle_view_for_testing() const;
		[[nodiscard]] std::optional<sqlite_shm_reader_open_epoch_test_view>
		reader_open_epoch_view_for_testing(
			std::uint64_t registry_open_token,
			const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal)
			const noexcept;
		void exhaust_reader_lifecycle_sequence_source_for_testing() noexcept;
		void make_reader_lifecycle_sequence_source_unavailable_for_testing() noexcept;
		void inject_reader_close_terminal_commit_failure_for_testing() noexcept;
		void inject_reader_close_post_receipt_state_failure_for_testing() noexcept;
		void inject_reader_close_begin_preparation_failure_for_testing() noexcept;
		void inject_reader_unmap_terminal_commit_failure_for_testing() noexcept;
		void inject_reader_unmap_post_receipt_state_failure_for_testing() noexcept;
		void inject_reader_unmap_begin_preparation_failure_for_testing() noexcept;
		void inject_reader_coarse_unmap_terminal_exception_for_testing() noexcept;
		void inject_reader_operation_mutex_acquire_failure_for_testing() noexcept;
		void inject_reader_recovery_mutex_reacquire_failure_for_testing() noexcept;
		void inject_writer_native_transition_failure_for_testing() noexcept;
		void inject_writer_attachment_seal_failure_for_testing() noexcept;
		void inject_writer_completion_transition_failure_for_testing() noexcept;
		void inject_reader_map_terminal_commit_failure_for_testing() noexcept;
		void inject_reader_session_terminal_commit_failure_for_testing() noexcept;
		void inject_registry_reader_attachment_liveness_loss_for_testing() noexcept;
		void inject_registry_reader_predelegate_liveness_loss_for_testing() noexcept;
		void inject_registry_writer_incoming_liveness_loss_for_testing() noexcept;
		void inject_registry_writer_existing_liveness_loss_for_testing() noexcept;
		void inject_registry_writer_pending_liveness_loss_for_testing() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
	};
} // namespace cxxlens::sdk
