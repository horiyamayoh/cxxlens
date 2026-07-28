#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

#include "sqlite_backend_observation_internal.hpp"

namespace cxxlens::sdk
{
	namespace detail
	{
		class sqlite_shm_mapping_lease_state;
		class sqlite_shm_mapping_registry_state;

		struct sqlite_shm_lease_token_identity
		{
			std::uint64_t value{};
		};

		struct sqlite_shm_mapping_generation_identity
		{
			std::uint64_t value{};
		};
	} // namespace detail

	class sqlite_shm_writer_member_authority;

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
	 * One callback invocation identity retained across a native callback boundary.
	 *
	 * The future process-global registry must seal `invocation_token` as non-reusable for the
	 * complete process/runtime/VFS cohort. This pure coordinator rejects duplicates among active
	 * callbacks and binds every multi-phase transition to exact receipt equality; it does not keep
	 * an unbounded history of completed invocations.
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

	class sqlite_shm_writer_map_inflight;
	class sqlite_shm_verified_writer_native_map_receipt;
	class sqlite_writer_shm_native_map_receipt_validator;
	class sqlite_writer_shm_mapping_receipt_validator;
	class sqlite_same_process_shm_writer_gate_receipt_validator;
	class sqlite_same_process_shm_reader_receipt_validator;
	class sqlite_same_process_shm_lease_test_peer;

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
		friend class sqlite_same_process_shm_lease_test_peer;

		sqlite_shm_verified_writer_post_map_receipt(
			sqlite_shm_writer_map_request request,
			sqlite_backend_opaque_identity open_epoch,
			sqlite_shm_mapping_tuple mapping,
			sqlite_shm_writer_extend_pair extend_pair,
			sqlite_backend_opaque_identity holder_specific_effect_receipt);

		sqlite_shm_writer_map_request request_;
		sqlite_backend_opaque_identity open_epoch_;
		sqlite_shm_mapping_tuple mapping_;
		sqlite_shm_writer_extend_pair extend_pair_{sqlite_shm_writer_extend_pair::zero_zero};
		sqlite_backend_opaque_identity holder_specific_effect_receipt_;
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
		bool reader_admission_visible{};
		bool quarantined{};
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
		explicit sqlite_shm_reader_handoff(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			detail::sqlite_shm_lease_token_identity token,
			detail::sqlite_shm_mapping_generation_identity generation) noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
		std::uint64_t generation_{};
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
		explicit sqlite_shm_reader_unmap_obligation(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			detail::sqlite_shm_lease_token_identity token,
			detail::sqlite_shm_mapping_generation_identity generation) noexcept;
		void disarm() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
		std::uint64_t token_{};
		std::uint64_t generation_{};
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
		[[nodiscard]] sqlite_shm_lease_result<void>
		complete_reader_unmap(sqlite_shm_reader_unmap_obligation& unmap,
							  const sqlite_shm_callback_execution_receipt& callback,
							  sqlite_shm_native_cleanup_outcome outcome) noexcept;

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
		void inject_writer_native_transition_failure_for_testing() noexcept;
		void inject_writer_attachment_seal_failure_for_testing() noexcept;
		void inject_writer_completion_transition_failure_for_testing() noexcept;
		void inject_registry_writer_incoming_liveness_loss_for_testing() noexcept;
		void inject_registry_writer_existing_liveness_loss_for_testing() noexcept;

		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state_;
	};
} // namespace cxxlens::sdk
