#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <barrier>

#include "sdk/sqlite_same_process_shm_mapping_registry_internal.hpp"
#include "sdk/sqlite_writer_shm_mapping_epoch_internal.hpp"
#include "sdk/sqlite_writer_shm_mapping_semantics_internal.hpp"

namespace cxxlens::sdk
{
	class sqlite_same_process_shm_registry_test_peer
	{
	  public:
		[[nodiscard]] static sqlite_shm_registry_process_owner
		process_owner(sqlite_backend_opaque_identity process_instance)
		{
			return sqlite_shm_registry_process_owner{std::move(process_instance)};
		}

		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_registry_runtime_lifetime_pin>
		adopt_runtime_lifetime(sqlite_same_process_shm_mapping_registry& registry,
							   sqlite_backend_opaque_identity identity,
							   sqlite_backend_opaque_identity pin_identity,
							   std::shared_ptr<void> owner)
		{
			return registry.adopt_runtime_lifetime_for_testing(
				std::move(identity), std::move(pin_identity), std::move(owner));
		}

		[[nodiscard]] static std::pair<sqlite_writer_shm_native_lifetime_revoker,
									   sqlite_writer_shm_native_lifetime_source>
		native_lifetime_source(const sqlite_writer_shm_native_lifetime_role role,
							   sqlite_backend_opaque_identity native_lifetime_identity,
							   sqlite_backend_opaque_identity semantic_receipt,
							   std::optional<sqlite_backend_opaque_identity> native_xopen_receipt,
							   const std::shared_ptr<void>& retained_owner)
		{
			return sqlite_writer_shm_native_lifetime_test_factory::create_source(
				role,
				std::move(native_lifetime_identity),
				std::move(semantic_receipt),
				std::move(native_xopen_receipt),
				retained_owner);
		}

		[[nodiscard]] static sqlite_shm_registry_alias_binding
		alias_binding(sqlite_backend_opaque_identity process_instance,
					  sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
					  sqlite_backend_opaque_identity alias_lifetime,
					  sqlite_shm_registry_runtime_lifetime_pin runtime_lifetime)
		{
			return {
				std::move(process_instance),
				std::move(shared_runtime_vfs_cohort),
				std::move(alias_lifetime),
				std::move(runtime_lifetime),
			};
		}

		[[nodiscard]] static sqlite_shm_verified_alias_registration_receipt
		registration_receipt(sqlite_backend_opaque_identity process_instance,
							 sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
							 sqlite_backend_opaque_identity alias_lifetime,
							 sqlite_backend_opaque_identity runtime_lifetime_identity,
							 sqlite_backend_opaque_identity runtime_lifetime_pin_identity,
							 sqlite_backend_opaque_identity registration_epoch)
		{
			return {
				std::move(process_instance),
				std::move(shared_runtime_vfs_cohort),
				std::move(alias_lifetime),
				std::move(runtime_lifetime_identity),
				std::move(runtime_lifetime_pin_identity),
				std::move(registration_epoch),
			};
		}

		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_reader_open_authority>
		reader_open(sqlite_same_process_shm_mapping_registry& registry,
					sqlite_shm_registry_family_pin& family,
					const sqlite_shm_reader_open_binding& binding)
		{
			return registry.acquire_reader_open_for_testing(family, binding);
		}

		[[nodiscard]] static sqlite_same_process_shm_mapping_lease_coordinator*
		coordinator(sqlite_same_process_shm_mapping_registry& registry,
					const sqlite_shm_lease_family_binding& family) noexcept
		{
			return registry.coordinator_for_family_for_testing(family);
		}

		[[nodiscard]] static sqlite_shm_verified_writer_route_proof
		writer_route_proof(const sqlite_writer_shm_mapping_semantic_route route,
						   sqlite_shm_writer_map_request request,
						   const int delegated_extend,
						   sqlite_backend_opaque_identity authenticated_route_seal,
						   sqlite_backend_opaque_identity main_native_file_receipt,
						   sqlite_backend_opaque_identity main_xopen_receipt,
						   sqlite_backend_opaque_identity sqlite_source_id,
						   sqlite_backend_opaque_identity callback_transcript,
						   sqlite_backend_opaque_identity wal_write_lock_receipt,
						   sqlite_backend_opaque_identity effect_gate_receipt,
						   sqlite_backend_opaque_identity validation_seal)
		{
			return {
				route,
				std::move(request),
				delegated_extend,
				std::move(authenticated_route_seal),
				std::move(main_native_file_receipt),
				std::move(main_xopen_receipt),
				std::move(sqlite_source_id),
				std::move(callback_transcript),
				std::move(wal_write_lock_receipt),
				std::move(effect_gate_receipt),
				std::move(validation_seal),
			};
		}
	};

	class sqlite_same_process_shm_lease_test_peer
	{
	  public:
		[[nodiscard]] static sqlite_shm_verified_writer_native_map_receipt
		unchecked_writer_native_map(const sqlite_shm_writer_map_inflight& inflight,
									const volatile void* native_mapping)
		{
			return {inflight, native_mapping};
		}

		static void fail_next_writer_native_transition(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_writer_native_transition_failure_for_testing();
		}

		static void fail_next_writer_completion_transition(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_writer_completion_transition_failure_for_testing();
		}

		static void fail_next_writer_attachment_seal_transition(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_writer_attachment_seal_failure_for_testing();
		}

		static void fail_next_reader_map_terminal_commit(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_reader_map_terminal_commit_failure_for_testing();
		}

		static void fail_next_reader_session_terminal_commit(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_reader_session_terminal_commit_failure_for_testing();
		}

		static void fail_next_reader_unpublished_cleanup_terminal_commit(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_reader_unpublished_cleanup_terminal_commit_failure_for_testing();
		}

		static void fail_next_reader_unmap_terminal_commit(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_reader_unmap_terminal_commit_failure_for_testing();
		}

		static void fail_next_reader_close_terminal_commit(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_reader_close_terminal_commit_failure_for_testing();
		}

		static void fail_reader_close_after_exact_receipt(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_reader_close_post_receipt_state_failure_for_testing();
		}

		static void fail_reader_close_begin_preparation(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_reader_close_begin_preparation_failure_for_testing();
		}

		static void fail_reader_unmap_after_exact_receipt(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_reader_unmap_post_receipt_state_failure_for_testing();
		}

		static void fail_reader_unmap_begin_preparation(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_reader_unmap_begin_preparation_failure_for_testing();
		}

		static void throw_reader_unmap_terminal_exception(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_reader_coarse_unmap_terminal_exception_for_testing();
		}

		static void fail_next_reader_recovery_mutex_reacquire(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_reader_recovery_mutex_reacquire_failure_for_testing();
		}

		static void fail_next_reader_operation_mutex_acquire(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_reader_operation_mutex_acquire_failure_for_testing();
		}

		static void exhaust_reader_lifecycle_sequences(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.exhaust_reader_lifecycle_sequence_source_for_testing();
		}

		static void make_reader_lifecycle_sequences_unavailable(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.make_reader_lifecycle_sequence_source_unavailable_for_testing();
		}

		[[nodiscard]] static sqlite_shm_reader_lifecycle_test_view reader_lifecycle_view(
			const sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			return coordinator.reader_lifecycle_view_for_testing();
		}

		[[nodiscard]] static std::uint64_t
		reader_handoff_token(const sqlite_shm_reader_handoff& handoff) noexcept
		{
			return handoff.token_;
		}

		[[nodiscard]] static sqlite_shm_verified_writer_post_map_receipt
		writer_map(sqlite_shm_writer_map_request request,
				   sqlite_backend_opaque_identity open_epoch,
				   const sqlite_shm_mapping_tuple mapping,
				   const sqlite_shm_writer_extend_pair pair,
				   sqlite_backend_opaque_identity effect)
		{
			return {std::move(request), std::move(open_epoch), mapping, pair, std::move(effect)};
		}

		[[nodiscard]] static sqlite_shm_verified_writer_eligibility_receipt
		eligibility(sqlite_shm_lease_family_binding family,
					sqlite_backend_opaque_identity connection,
					sqlite_backend_opaque_identity open_epoch,
					sqlite_backend_effect_arm_receipt effect,
					sqlite_backend_opaque_identity complete_gate)
		{
			return {std::move(family),
					std::move(connection),
					std::move(open_epoch),
					std::move(effect),
					std::move(complete_gate)};
		}

		[[nodiscard]] static sqlite_shm_verified_reader_post_map_receipt
		reader_map(sqlite_shm_reader_map_request request,
				   const std::uint64_t generation,
				   const sqlite_shm_mapping_tuple mapping,
				   sqlite_backend_opaque_identity zero_resize_effect)
		{
			return {std::move(request), generation, mapping, std::move(zero_resize_effect)};
		}

		[[nodiscard]] static sqlite_shm_verified_reader_attachment_post_map_receipt
		reader_attachment_map(sqlite_shm_reader_attachment_map_request request,
							  const std::uint64_t generation,
							  const sqlite_shm_mapping_tuple mapping,
							  sqlite_backend_opaque_identity zero_resize_effect)
		{
			auto observed = sqlite_shm_reader_native_attachment_identity{
				request.expected_attachment,
				{"test.reader-observed-shm-object", {std::byte{1}}},
				{"test.reader-observed-shm-entry", {std::byte{2}}},
				{"test.reader-observed-device", {std::byte{3}}},
				{"test.reader-observed-mount", {std::byte{4}}}};
			return {std::move(request),
					generation,
					mapping,
					std::move(observed),
					std::move(zero_resize_effect)};
		}

		[[nodiscard]] static sqlite_shm_verified_reader_attachment_zero_effect_receipt
		reader_attachment_zero_effect(const sqlite_shm_reader_attachment_map_inflight& inflight,
									  const sqlite_shm_reader_attachment_zero_effect_kind kind,
									  sqlite_shm_reader_attachment_map_request request,
									  const int native_status,
									  const volatile void* native_mapping,
									  sqlite_backend_opaque_identity zero_attachment_effect,
									  const int delegated_extend = 0)
		{
			return {inflight,
					kind,
					std::move(request),
					native_status,
					native_mapping,
					delegated_extend,
					std::move(zero_attachment_effect)};
		}

		[[nodiscard]] static sqlite_shm_verified_reader_predecessor_map_receipt
		reader_predecessor_map(const sqlite_shm_reader_attachment_map_inflight& inflight,
							   const sqlite_shm_reader_predecessor_map_kind kind,
							   sqlite_shm_reader_attachment_map_request request,
							   const int native_status,
							   const volatile void* native_mapping,
							   sqlite_backend_opaque_identity native_effect,
							   const int delegated_extend = 0)
		{
			std::optional<sqlite_shm_reader_native_attachment_identity> observed;
			if (kind == sqlite_shm_reader_predecessor_map_kind::exact_predecessor_mapped_route)
			{
				auto exact_observed = sqlite_shm_reader_native_attachment_identity{
					request.expected_attachment,
					sqlite_backend_opaque_identity{"test.reader-predecessor-shm-object",
												   {std::byte{1}}},
					sqlite_backend_opaque_identity{"test.reader-predecessor-shm-entry",
												   {std::byte{2}}},
					sqlite_backend_opaque_identity{"test.reader-predecessor-device",
												   {std::byte{3}}},
					sqlite_backend_opaque_identity{"test.reader-predecessor-mount",
												   {std::byte{4}}}};
				observed.emplace(std::move(exact_observed));
			}
			return {inflight,
					kind,
					std::move(request),
					native_status,
					native_mapping,
					delegated_extend,
					std::move(observed),
					std::move(native_effect)};
		}

		[[nodiscard]] static sqlite_shm_verified_reader_predecessor_map_receipt
		reader_existing_group_predecessor_mismatch(
			const sqlite_shm_reader_attachment_map_inflight& inflight,
			sqlite_shm_reader_attachment_map_request request,
			const int native_status,
			const volatile void* native_mapping,
			sqlite_backend_opaque_identity native_effect)
		{
			auto observed = sqlite_shm_reader_native_attachment_identity{
				request.expected_attachment,
				{"test.reader-observed-shm-object", {std::byte{1}}},
				{"test.reader-observed-shm-entry", {std::byte{2}}},
				{"test.reader-observed-device", {std::byte{3}}},
				{"test.reader-observed-mount", {std::byte{4}}}};
			return {inflight,
					sqlite_shm_reader_predecessor_map_kind::exact_predecessor_mapped_route,
					std::move(request),
					native_status,
					native_mapping,
					0,
					std::move(observed),
					std::move(native_effect)};
		}

		[[nodiscard]] static sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt
		reader_predecessor_unmap(const sqlite_shm_reader_predecessor_map_result& predecessor,
								 sqlite_shm_callback_execution_receipt callback,
								 const sqlite_shm_reader_unmap_evidence_kind evidence_kind,
								 std::optional<int> native_status,
								 std::optional<sqlite_backend_opaque_identity> native_effect,
								 const int caller_delete_flag = 0,
								 const int delegated_delete_flag = 0)
		{
			return {predecessor,
					std::move(callback),
					evidence_kind,
					native_status,
					caller_delete_flag,
					delegated_delete_flag,
					std::move(native_effect)};
		}

		[[nodiscard]] static sqlite_shm_verified_reader_unpublished_cleanup_receipt
		reader_unpublished_cleanup(
			const sqlite_shm_reader_attachment_map_inflight& inflight,
			const sqlite_shm_reader_unpublished_cleanup_entry_kind kind,
			sqlite_shm_reader_attachment_map_request request,
			sqlite_shm_reader_session_request session_request,
			const std::uint64_t generation,
			const int native_status,
			const volatile void* native_mapping,
			const int delegated_extend,
			sqlite_shm_reader_native_attachment_identity observed_attachment,
			sqlite_backend_opaque_identity mapped_effect_receipt,
			sqlite_backend_opaque_identity session_no_pointer_terminal_receipt)
		{
			return {
				inflight,
				kind,
				std::move(request),
				std::move(session_request),
				generation,
				native_status,
				native_mapping,
				delegated_extend,
				std::move(observed_attachment),
				std::move(mapped_effect_receipt),
				std::move(session_no_pointer_terminal_receipt),
			};
		}

		[[nodiscard]] static sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt
		reader_unpublished_cleanup_terminal(
			const sqlite_shm_reader_unpublished_cleanup_obligation& cleanup,
			sqlite_shm_callback_execution_receipt callback,
			const sqlite_shm_reader_unpublished_cleanup_evidence_kind evidence_kind,
			std::optional<int> native_status,
			const int caller_delete_flag,
			const int delegated_delete_flag,
			std::optional<sqlite_backend_opaque_identity> native_effect_receipt,
			std::optional<sqlite_backend_opaque_identity> latch_reset_receipt)
		{
			return {
				cleanup,
				std::move(callback),
				evidence_kind,
				native_status,
				caller_delete_flag,
				delegated_delete_flag,
				std::move(native_effect_receipt),
				std::move(latch_reset_receipt),
			};
		}

		[[nodiscard]] static std::optional<sqlite_shm_reader_attachment_reservation_identity>
		reader_attachment_reservation(sqlite_shm_lease_family_binding family,
									  sqlite_backend_opaque_identity runtime_lifetime_pin,
									  sqlite_backend_opaque_identity alias_lifetime,
									  sqlite_backend_opaque_identity connection_token,
									  sqlite_backend_opaque_identity main_native_file_receipt,
									  sqlite_backend_opaque_identity main_xopen_receipt,
									  sqlite_backend_opaque_identity open_epoch,
									  const std::uint64_t writer_mapping_generation,
									  sqlite_backend_opaque_identity callback_cohort,
									  sqlite_backend_opaque_identity attachment_epoch,
									  const std::uint64_t registry_open_token = 0U,
									  std::optional<sqlite_shm_reader_attachment_target_identity>
										  target_identity = std::nullopt)
		{
			return sqlite_shm_reader_attachment_reservation_identity::bind(
				std::move(family),
				std::move(runtime_lifetime_pin),
				std::move(alias_lifetime),
				std::move(connection_token),
				std::move(main_native_file_receipt),
				std::move(main_xopen_receipt),
				std::move(open_epoch),
				writer_mapping_generation,
				std::move(callback_cohort),
				std::move(attachment_epoch),
				registry_open_token,
				std::move(target_identity));
		}

		[[nodiscard]] static sqlite_shm_reader_session_terminal_receipt
		reader_session_terminal(sqlite_shm_reader_session_request request,
								const sqlite_shm_reader_session_terminal_kind kind,
								sqlite_backend_opaque_identity terminal_receipt,
								const bool authority_read_closed = true,
								const bool no_live_shm_lock = true)
		{
			return {std::move(request),
					kind,
					std::move(terminal_receipt),
					authority_read_closed,
					no_live_shm_lock};
		}

		[[nodiscard]] static sqlite_shm_verified_reader_unmap_terminal_receipt
		reader_unmap_terminal(const sqlite_shm_reader_unmap_obligation& unmap,
							  sqlite_shm_callback_execution_receipt callback,
							  const sqlite_shm_reader_unmap_evidence_kind evidence_kind,
							  std::optional<int> native_status,
							  const int caller_delete_flag,
							  const int delegated_delete_flag,
							  std::optional<sqlite_backend_opaque_identity> native_effect_receipt,
							  std::optional<sqlite_backend_opaque_identity> latch_reset_receipt)
		{
			return {unmap,
					std::move(callback),
					evidence_kind,
					native_status,
					caller_delete_flag,
					delegated_delete_flag,
					std::move(native_effect_receipt),
					std::move(latch_reset_receipt)};
		}

		[[nodiscard]] static sqlite_shm_verified_reader_close_terminal_receipt
		reader_close_terminal(const sqlite_shm_reader_close_obligation& close,
							  sqlite_shm_callback_execution_receipt callback,
							  const sqlite_shm_reader_close_evidence_kind evidence_kind,
							  std::optional<int> native_status,
							  std::optional<sqlite_backend_opaque_identity> native_effect_receipt)
		{
			return {close,
					std::move(callback),
					evidence_kind,
					native_status,
					std::move(native_effect_receipt)};
		}

		[[nodiscard]] static sqlite_shm_lease_result<void> register_reader_open(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
			const std::uint64_t registry_open_token,
			const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
			const sqlite_shm_reader_open_epoch_binding& binding)
		{
			return coordinator.register_registry_reader_open(registry_open_token, seal, binding);
		}

		[[nodiscard]] static sqlite_shm_lease_result<void> register_reader_open(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
			const std::uint64_t registry_open_token,
			const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
			const sqlite_shm_reader_open_epoch_binding& binding,
			const detail::sqlite_shm_reader_open_admission_guard& admission_guard)
		{
			return coordinator.register_registry_reader_open(
				registry_open_token, seal, binding, admission_guard);
		}

		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_reader_close_obligation>
		begin_reader_close(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
						   const std::uint64_t registry_open_token,
						   const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
						   const sqlite_shm_reader_open_epoch_binding& binding,
						   const sqlite_shm_reader_close_request& request) noexcept
		{
			return coordinator.begin_registry_reader_close(
				registry_open_token, seal, binding, request);
		}

		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_reader_close_terminal_result>
		complete_reader_close(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
			const std::uint64_t registry_open_token,
			const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
			const sqlite_shm_reader_open_epoch_binding& binding,
			sqlite_shm_reader_close_obligation& close,
			const sqlite_shm_verified_reader_close_terminal_receipt& receipt) noexcept
		{
			return coordinator.complete_registry_reader_close(
				registry_open_token, seal, binding, close, receipt);
		}

		[[nodiscard]] static sqlite_shm_lease_result<void> release_reader_open(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
			const std::uint64_t registry_open_token,
			const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal) noexcept
		{
			return coordinator.release_registry_reader_open(registry_open_token, seal);
		}

		[[nodiscard]] static sqlite_shm_lease_result<
			std::vector<sqlite_shm_reader_open_epoch_close_tombstone>>
		export_reader_open_close_tombstones(
			const sqlite_same_process_shm_mapping_lease_coordinator& coordinator)
		{
			return coordinator.export_registry_reader_open_epoch_close_tombstones();
		}

		[[nodiscard]] static sqlite_shm_lease_result<void> import_reader_open_close_tombstones(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
			const std::span<const sqlite_shm_reader_open_epoch_close_tombstone> tombstones)
		{
			return coordinator.import_registry_reader_open_epoch_close_tombstones(tombstones);
		}

		[[nodiscard]] static sqlite_shm_lease_result<
			std::vector<sqlite_shm_reader_lifecycle_compact_tombstone>>
		export_reader_lifecycle_tombstones(
			const sqlite_same_process_shm_mapping_lease_coordinator& coordinator)
		{
			return coordinator.export_registry_reader_lifecycle_tombstones();
		}

		[[nodiscard]] static sqlite_shm_lease_result<void> import_reader_lifecycle_tombstones(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
			const std::span<const sqlite_shm_reader_lifecycle_compact_tombstone> tombstones)
		{
			return coordinator.import_registry_reader_lifecycle_tombstones(tombstones);
		}

		[[nodiscard]] static sqlite_shm_lease_result<void> check_reader_lifecycle_tombstone(
			const sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
			const sqlite_shm_reader_attachment_reservation_identity& attachment) noexcept
		{
			return coordinator.check_registry_reader_lifecycle_tombstone(attachment);
		}
	};
} // namespace cxxlens::sdk

namespace
{
	using namespace cxxlens::sdk;

	/** Stable SQLite ABI result values used by the closed native-map validator fixture. */
	constexpr int sqlite_ok_status = 0;
	constexpr int sqlite_busy_status = 5;
	constexpr int sqlite_ioerr_status = 10;
	constexpr int sqlite_ioerr_read_status = sqlite_ioerr_status | (1 << 8);
	constexpr int sqlite_readonly_status = 8;
	constexpr int sqlite_readonly_cantinit_status = sqlite_readonly_status | (5 << 8);
	constexpr int sqlite_readonly_unsupported_extended_status = sqlite_readonly_status | (7 << 8);
	constexpr int sqlite_invalid_extended_ok_status = 1 << 8;
	constexpr int sqlite_undefined_primary_status = 29;

	static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_eligibility>);
	static_assert(!std::is_default_constructible_v<sqlite_shm_native_attachment_identity>);
	static_assert(std::is_copy_constructible_v<sqlite_shm_native_attachment_identity>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_map_inflight>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_post_native_mapping>);
	static_assert(!std::is_default_constructible_v<sqlite_shm_verified_writer_native_map_receipt>);
	static_assert(!std::is_default_constructible_v<sqlite_writer_shm_native_map_receipt_validator>);
	static_assert(noexcept(sqlite_writer_shm_native_map_receipt_validator::validate(
		std::declval<sqlite_shm_writer_map_inflight&>(), sqlite_ok_status, nullptr)));
	static_assert(!std::is_copy_constructible_v<sqlite_shm_pending_mapping>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_attachment_cleanup>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_holder>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_reader_map_inflight>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_reader_attachment_map_inflight>);
	static_assert(
		!std::is_default_constructible_v<sqlite_shm_reader_late_close_outer_unwind_authority>);
	static_assert(
		!std::is_copy_constructible_v<sqlite_shm_reader_late_close_outer_unwind_authority>);
	static_assert(!std::is_copy_assignable_v<sqlite_shm_reader_late_close_outer_unwind_authority>);
	static_assert(!std::is_move_assignable_v<sqlite_shm_reader_late_close_outer_unwind_authority>);
	static_assert(
		!std::is_trivially_copyable_v<sqlite_shm_reader_late_close_outer_unwind_authority>);
	static_assert(!std::is_default_constructible_v<
				  sqlite_shm_verified_reader_attachment_zero_effect_receipt>);
	static_assert(
		!std::is_default_constructible_v<sqlite_shm_verified_reader_unpublished_cleanup_receipt>);
	static_assert(!std::is_default_constructible_v<
				  sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt>);
	static_assert(
		!std::is_default_constructible_v<sqlite_shm_verified_reader_unmap_terminal_receipt>);
	static_assert(
		!std::is_default_constructible_v<sqlite_shm_verified_reader_close_terminal_receipt>);
	static_assert(
		!std::is_default_constructible_v<sqlite_shm_reader_attachment_reservation_identity>);
	static_assert(!std::is_default_constructible_v<sqlite_shm_reader_native_attachment_identity>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_reader_session>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_reader_map_commit>);
	static_assert(!std::is_move_assignable_v<sqlite_shm_reader_map_commit>);
	static_assert(!std::is_default_constructible_v<sqlite_shm_reader_session_terminal_receipt>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_reader_cleanup_obligation>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_reader_unpublished_cleanup_obligation>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_reader_handoff>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_reader_unmap_obligation>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_reader_close_obligation>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_release>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_writer_eligibility>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_writer_map_inflight>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_writer_post_native_mapping>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_pending_mapping>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_writer_attachment_cleanup>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_writer_holder>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_reader_map_inflight>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_reader_attachment_map_inflight>);
	static_assert(
		std::is_nothrow_move_constructible_v<sqlite_shm_reader_late_close_outer_unwind_authority>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_reader_session>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_reader_cleanup_obligation>);
	static_assert(
		std::is_nothrow_move_constructible_v<sqlite_shm_reader_unpublished_cleanup_obligation>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_reader_handoff>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_reader_unmap_obligation>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_reader_close_obligation>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_writer_release>);
	static_assert(std::is_nothrow_destructible_v<sqlite_shm_writer_holder>);
	static_assert(std::is_nothrow_destructible_v<sqlite_shm_reader_handoff>);
	static_assert(
		std::is_nothrow_destructible_v<sqlite_shm_reader_late_close_outer_unwind_authority>);

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error{std::string{message}};
	}

	template <class Enum>
	[[nodiscard]] constexpr std::size_t enum_index(const Enum value) noexcept
	{
		return static_cast<std::size_t>(value);
	}

	[[nodiscard]] const sqlite_shm_reader_attachment_reservation_test_view&
	only_attachment_reservation(const sqlite_shm_reader_lifecycle_test_view& view)
	{
		require(view.attachment_reservations.size() == 1U,
				"reader lifecycle view must contain exactly one attachment reservation");
		return view.attachment_reservations.front();
	}

	[[nodiscard]] const sqlite_shm_reader_session_reservation_test_view&
	only_session_reservation(const sqlite_shm_reader_lifecycle_test_view& view)
	{
		require(view.session_reservations.size() == 1U,
				"reader lifecycle view must contain exactly one session reservation");
		return view.session_reservations.front();
	}

	[[nodiscard]] const sqlite_shm_reader_map_attempt_test_view&
	only_reader_map_attempt(const sqlite_shm_reader_lifecycle_test_view& view)
	{
		require(view.map_attempts.size() == 1U,
				"reader lifecycle view must contain exactly one pending map attempt");
		return view.map_attempts.front();
	}

	[[nodiscard]] std::size_t
	count_reader_lifecycle_events(const sqlite_shm_reader_lifecycle_test_view& view,
								  const detail::sqlite_shm_reader_lifecycle_event_kind kind)
	{
		return static_cast<std::size_t>(std::ranges::count(
			view.events, kind, &sqlite_shm_reader_lifecycle_event_test_view::kind));
	}

	[[nodiscard]] std::uint64_t
	last_reader_lifecycle_event_sequence(const sqlite_shm_reader_lifecycle_test_view& view,
										 const detail::sqlite_shm_reader_lifecycle_event_kind kind)
	{
		for (auto iterator = view.events.rbegin(); iterator != view.events.rend(); ++iterator)
			if (iterator->kind == kind)
				return iterator->sequence;
		require(false, "reader lifecycle event kind must be present");
		return 0U;
	}

	[[nodiscard]] std::uint64_t
	last_reader_lifecycle_event_owner(const sqlite_shm_reader_lifecycle_test_view& view,
									  const detail::sqlite_shm_reader_lifecycle_event_kind kind)
	{
		for (auto iterator = view.events.rbegin(); iterator != view.events.rend(); ++iterator)
			if (iterator->kind == kind)
				return iterator->owner_token;
		require(false, "reader lifecycle event owner kind must be present");
		return 0U;
	}

	[[nodiscard]] bool
	all_reader_live_custody_released(const sqlite_shm_reader_lifecycle_test_view& view)
	{
		return std::ranges::all_of(view.live_custody_kind_counts,
								   [](const std::size_t count)
								   {
									   return count == 0U;
								   });
	}

	[[nodiscard]] std::vector<std::uint64_t>
	reader_record_terminal_permit_slots(const sqlite_shm_reader_lifecycle_test_view& view)
	{
		std::vector<std::uint64_t> slots;
		const auto append = [&slots](const std::uint64_t slot)
		{
			if (slot != 0U)
				slots.push_back(slot);
		};
		for (const auto& session : view.session_reservations)
			append(session.terminal_permit_slot);
		for (const auto& group : view.attachment_groups)
		{
			append(group.unmap_cut_permit_slot);
			append(group.unmap_terminal_permit_slot);
		}
		for (const auto& map : view.map_attempts)
		{
			append(map.terminal_permit_slot);
			append(map.potential_group_cut_permit_slot);
			append(map.potential_group_terminal_permit_slot);
		}
		for (const auto& open : view.open_epochs)
		{
			append(open.close_cut_permit_slot);
			append(open.close_terminal_permit_slot);
		}
		std::ranges::sort(slots);
		return slots;
	}

	[[nodiscard]] bool
	reader_terminal_permit_slots_are_exact(const sqlite_shm_reader_lifecycle_test_view& view)
	{
		const auto record_slots = reader_record_terminal_permit_slots(view);
		return view.outstanding_terminal_permit_count ==
			view.outstanding_terminal_permit_slots.size() &&
			view.outstanding_terminal_permit_slots == record_slots &&
			std::ranges::none_of(view.outstanding_terminal_permit_slots,
								 [](const std::uint64_t slot)
								 {
									 return slot == 0U;
								 }) &&
			std::ranges::adjacent_find(view.outstanding_terminal_permit_slots) ==
			view.outstanding_terminal_permit_slots.end();
	}

	[[nodiscard]] bool reader_shared_terminal_permit_slots_are_exact(
		const sqlite_shm_reader_lifecycle_test_view& source_view,
		const sqlite_shm_reader_lifecycle_test_view& first_family,
		const sqlite_shm_reader_lifecycle_test_view& second_family)
	{
		auto record_slots = reader_record_terminal_permit_slots(first_family);
		auto second_slots = reader_record_terminal_permit_slots(second_family);
		record_slots.insert(record_slots.end(), second_slots.begin(), second_slots.end());
		std::ranges::sort(record_slots);
		return source_view.outstanding_terminal_permit_count ==
			source_view.outstanding_terminal_permit_slots.size() &&
			source_view.outstanding_terminal_permit_slots == record_slots &&
			std::ranges::adjacent_find(source_view.outstanding_terminal_permit_slots) ==
			source_view.outstanding_terminal_permit_slots.end();
	}

	[[nodiscard]] const sqlite_shm_reader_terminal_quarantine_test_view*
	find_reader_terminal_quarantine(const sqlite_shm_reader_lifecycle_test_view& view,
									const std::uint64_t owner_token)
	{
		const auto found =
			std::ranges::find(view.terminal_quarantines,
							  owner_token,
							  &sqlite_shm_reader_terminal_quarantine_test_view::owner_token);
		return found == view.terminal_quarantines.end() ? nullptr : &*found;
	}

	[[nodiscard]] bool
	reader_event_sequences_are_dense(const sqlite_shm_reader_lifecycle_test_view& view)
	{
		std::vector<std::uint64_t> sequences;
		for (const auto& event : view.events)
			if (event.sequence != 0U)
				sequences.push_back(event.sequence);
		std::ranges::sort(sequences);
		sequences.erase(std::unique(sequences.begin(), sequences.end()), sequences.end());
		if (view.last_issued_sequence == 0U)
			return sequences.empty();
		if (sequences.empty() || sequences.front() != 1U ||
			sequences.back() != view.last_issued_sequence)
			return false;
		for (std::size_t index = 1U; index < sequences.size(); ++index)
			if (sequences[index] != sequences[index - 1U] + 1U)
				return false;
		return true;
	}

	template <class Enum, std::size_t ValueCount>
	void verify_dense_unique_enum_values(const std::array<Enum, ValueCount>& values,
										 const std::string_view message)
	{
		for (std::size_t index = 0; index < values.size(); ++index)
		{
			require(static_cast<std::size_t>(values[index]) == index, message);
			require(std::ranges::count(values, values[index]) == 1, message);
		}
	}

	template <class Enum, std::size_t ValueCount, std::size_t EdgeCount, class Predicate>
	void verify_closed_transition_graph(
		const std::array<Enum, ValueCount>& values,
		const std::array<std::pair<Enum, Enum>, EdgeCount>& expected_edges,
		Predicate&& predicate,
		const std::string_view message)
	{
		verify_dense_unique_enum_values(values, message);
		for (const auto origin : values)
			for (const auto destination : values)
			{
				const auto edge = std::pair{origin, destination};
				const auto expected =
					std::ranges::find(expected_edges, edge) != expected_edges.end();
				require(std::invoke(predicate, origin, destination) == expected, message);
			}
	}

	void verify_reader_lifecycle_vocabulary_is_closed()
	{
		using namespace cxxlens::sdk::detail;
		using reservation = sqlite_shm_reader_attachment_reservation_phase;
		using session = sqlite_shm_reader_session_reservation_phase;
		using custody_kind = sqlite_shm_reader_custody_kind;
		using custody_state = sqlite_shm_reader_custody_state;
		using group = sqlite_shm_reader_attachment_group_phase;
		using ack = sqlite_shm_reader_logical_ack_phase;
		using late_close = sqlite_shm_reader_late_close_drain_phase;
		using close = sqlite_shm_reader_connection_close_phase;
		using cut = sqlite_shm_reader_cut_phase;
		using provenance = sqlite_shm_reader_late_close_provenance_kind;

		static_assert(sqlite_shm_reader_attachment_reservation_phases.size() == 9U);
		static_assert(sqlite_shm_reader_session_reservation_phases.size() == 5U);
		static_assert(sqlite_shm_reader_custody_kinds.size() == 18U);
		static_assert(sqlite_shm_reader_custody_states.size() == 4U);
		static_assert(sqlite_shm_reader_attachment_group_phases.size() == 5U);
		static_assert(sqlite_shm_reader_logical_ack_phases.size() == 4U);
		static_assert(sqlite_shm_reader_late_close_drain_phases.size() == 6U);
		static_assert(sqlite_shm_reader_connection_close_phases.size() == 4U);
		static_assert(sqlite_shm_reader_lifecycle_event_kinds.size() == 8U);
		static_assert(sqlite_shm_reader_cut_kinds.size() == 4U);
		static_assert(sqlite_shm_reader_cut_phases.size() == 5U);

		verify_dense_unique_enum_values(sqlite_shm_reader_custody_states,
										"reader custody state vocabulary is dense and unique");
		verify_dense_unique_enum_values(sqlite_shm_reader_lifecycle_event_kinds,
										"reader lifecycle event vocabulary is dense and unique");
		verify_dense_unique_enum_values(sqlite_shm_reader_cut_kinds,
										"reader cut kind vocabulary is dense and unique");

		constexpr std::array reservation_edges{
			std::pair{reservation::reserved, reservation::predecessor_route_active},
			std::pair{reservation::reserved, reservation::observed_present},
			std::pair{reservation::reserved, reservation::revoked_no_map},
			std::pair{reservation::reserved, reservation::unpublished_cleanup_admitted},
			std::pair{reservation::reserved, reservation::terminal_quarantined},
			std::pair{reservation::predecessor_route_active,
					  reservation::predecessor_route_retired_confirmed},
			std::pair{reservation::predecessor_route_active, reservation::terminal_quarantined},
			std::pair{reservation::observed_present, reservation::retired_confirmed},
			std::pair{reservation::observed_present, reservation::terminal_quarantined},
			std::pair{reservation::unpublished_cleanup_admitted,
					  reservation::unpublished_cleanup_confirmed},
			std::pair{reservation::unpublished_cleanup_admitted, reservation::terminal_quarantined},
		};
		verify_closed_transition_graph(sqlite_shm_reader_attachment_reservation_phases,
									   reservation_edges,
									   is_sqlite_shm_reader_attachment_reservation_transition,
									   "reader attachment reservation graph is exact and closed");

		constexpr std::array session_edges{
			std::pair{session::reserved_before_sqlite, session::promoted_to_group_owner},
			std::pair{session::reserved_before_sqlite,
					  session::transferred_to_existing_predecessor},
			std::pair{session::reserved_before_sqlite, session::consumed_no_pointer},
			std::pair{session::reserved_before_sqlite, session::terminal_quarantined},
		};
		verify_closed_transition_graph(sqlite_shm_reader_session_reservation_phases,
									   session_edges,
									   is_sqlite_shm_reader_session_reservation_transition,
									   "reader session reservation graph is exact and closed");

		verify_dense_unique_enum_values(sqlite_shm_reader_custody_kinds,
										"reader custody kind vocabulary is dense and unique");
		for (const auto kind : sqlite_shm_reader_custody_kinds)
		{
			for (const auto origin : sqlite_shm_reader_custody_states)
				for (const auto destination : sqlite_shm_reader_custody_states)
				{
					auto expected =
						origin == custody_state::live && destination != custody_state::live;
					if (kind == custody_kind::bounded_waiter_or_continuation ||
						kind == custody_kind::terminal_reporter)
						expected = origin == custody_state::live &&
							(destination == custody_state::consumed_with_exact_terminal_receipt ||
							 destination == custody_state::transferred_to_durable_tombstone);
					if (kind == custody_kind::opaque_attachment_uncertainty ||
						kind ==
							custody_kind::
								runtime_vfs_namespace_generation_native_mapping_lifetime_pin)
						expected = origin == custody_state::live &&
							destination == custody_state::transferred_to_durable_tombstone;
					require(is_sqlite_shm_reader_custody_transition(kind, origin, destination) ==
								expected,
							"reader custody transition table is exact and closed");
				}
		}

		constexpr std::array group_edges{
			std::pair{group::active, group::unmap_cut_sealing},
			std::pair{group::active, group::terminal_quarantined},
			std::pair{group::unmap_cut_sealing, group::native_unmap_admitted},
			std::pair{group::unmap_cut_sealing, group::terminal_quarantined},
			std::pair{group::native_unmap_admitted, group::native_unmap_confirmed},
			std::pair{group::native_unmap_admitted, group::terminal_quarantined},
		};
		verify_closed_transition_graph(sqlite_shm_reader_attachment_group_phases,
									   group_edges,
									   is_sqlite_shm_reader_attachment_group_transition,
									   "reader attachment group graph is exact and closed");

		constexpr std::array ack_edges{
			std::pair{ack::not_applicable, ack::awaiting_sqlite_ack},
			std::pair{ack::awaiting_sqlite_ack, ack::consumed_by_exact_unmap},
			std::pair{ack::awaiting_sqlite_ack, ack::consumed_by_close},
		};
		verify_closed_transition_graph(sqlite_shm_reader_logical_ack_phases,
									   ack_edges,
									   is_sqlite_shm_reader_logical_ack_transition,
									   "reader logical acknowledgement graph is exact and closed");

		static_assert(sqlite_shm_reader_late_close_drain_phases ==
					  std::array{late_close::not_applicable,
								 late_close::retained_original_callback_drain,
								 late_close::cleanup_admitted,
								 late_close::cleanup_confirmed_awaiting_sqlite_ack,
								 late_close::terminal_quarantined,
								 late_close::consumed_by_exact_outer_unmap});
		constexpr std::array late_close_edges{
			std::pair{late_close::not_applicable, late_close::retained_original_callback_drain},
			std::pair{late_close::retained_original_callback_drain, late_close::cleanup_admitted},
			std::pair{late_close::retained_original_callback_drain,
					  late_close::terminal_quarantined},
			std::pair{late_close::cleanup_admitted,
					  late_close::cleanup_confirmed_awaiting_sqlite_ack},
			std::pair{late_close::cleanup_admitted, late_close::terminal_quarantined},
			std::pair{late_close::cleanup_confirmed_awaiting_sqlite_ack,
					  late_close::consumed_by_exact_outer_unmap},
			std::pair{late_close::cleanup_confirmed_awaiting_sqlite_ack,
					  late_close::terminal_quarantined},
		};
		verify_closed_transition_graph(sqlite_shm_reader_late_close_drain_phases,
									   late_close_edges,
									   is_sqlite_shm_reader_late_close_drain_transition,
									   "reader late-close drain graph is exact and closed");
		for (const auto destination : sqlite_shm_reader_late_close_drain_phases)
		{
			require(!is_sqlite_shm_reader_late_close_drain_transition(
						late_close::terminal_quarantined, destination),
					"terminal late-close quarantine has no successor");
			require(!is_sqlite_shm_reader_late_close_drain_transition(
						late_close::consumed_by_exact_outer_unmap, destination),
					"consumed late-close outer unwind has no successor");
		}

		constexpr std::array late_close_provenance_kinds{
			provenance::same_thread_or_reentrant_precleanup_quarantine,
			provenance::bounded_other_thread_timeout_precleanup_quarantine,
			provenance::bounded_other_thread_unknown_precleanup_quarantine,
		};
		static_assert(late_close_provenance_kinds.size() == 3U);
		verify_dense_unique_enum_values(
			late_close_provenance_kinds,
			"reader late-close provenance vocabulary is dense and unique");

		constexpr std::array close_edges{
			std::pair{close::open, close::close_admitted},
			std::pair{close::open, close::terminal_quarantined},
			std::pair{close::close_admitted, close::closed},
			std::pair{close::close_admitted, close::terminal_quarantined},
		};
		verify_closed_transition_graph(sqlite_shm_reader_connection_close_phases,
									   close_edges,
									   is_sqlite_shm_reader_connection_close_transition,
									   "reader connection close graph is exact and closed");

		constexpr std::array cut_edges{
			std::pair{cut::sealed_waiting, cut::ready},
			std::pair{cut::sealed_waiting, cut::terminal_quarantined},
			std::pair{cut::ready, cut::native_effect_admitted},
			std::pair{cut::ready, cut::terminal_quarantined},
			std::pair{cut::native_effect_admitted, cut::terminal_confirmed},
			std::pair{cut::native_effect_admitted, cut::terminal_quarantined},
		};
		verify_closed_transition_graph(sqlite_shm_reader_cut_phases,
									   cut_edges,
									   is_sqlite_shm_reader_cut_transition,
									   "reader cut graph is exact and closed");
	}

	[[nodiscard]] sqlite_backend_opaque_identity identity(const std::string_view profile,
														  const std::uint8_t marker)
	{
		return {std::string{profile}, {static_cast<std::byte>(marker)}};
	}

	[[nodiscard]] sqlite_shm_lease_family_binding family(const std::uint8_t marker)
	{
		return {identity("test.process-instance", marker),
				identity("test.runtime-vfs-cohort", marker),
				identity("test.file-family", marker)};
	}

	[[nodiscard]] sqlite_shm_callback_execution_receipt callback(const std::uint8_t thread,
																 const std::uint8_t invocation,
																 const std::uint64_t depth = 0U)
	{
		return {identity("test.thread", thread),
				depth,
				identity("test.callback-invocation", invocation)};
	}

	[[nodiscard]] sqlite_shm_callback_execution_receipt
	reader_session_execution(const std::uint8_t marker, const std::uint64_t depth = 0U)
	{
		return {identity("test.reader-session-thread", marker),
				depth,
				identity("test.reader-session-execution", marker)};
	}

	[[nodiscard]] sqlite_backend_effect_arm_receipt
	effect_gate(const sqlite_backend_opaque_identity& connection, const std::uint8_t marker)
	{
		return {"test.effect-gate",
				identity("test.capability", marker),
				connection,
				"/test/main.db",
				identity("test.prerequisite", marker),
				identity("test.validation", marker),
				sqlite_backend_effect_stage::fully_armed,
				marker,
				false};
	}

	[[nodiscard]] sqlite_shm_native_attachment_identity
	writer_attachment(const sqlite_shm_lease_family_binding& binding,
					  const sqlite_backend_opaque_identity& alias_lifetime,
					  const sqlite_backend_opaque_identity& connection,
					  const sqlite_backend_opaque_identity& open_epoch,
					  const std::uint8_t marker,
					  std::optional<sqlite_backend_opaque_identity> attachment_epoch = std::nullopt)
	{
		auto bound = sqlite_shm_native_attachment_identity::bind(
			binding,
			alias_lifetime,
			connection,
			identity("test.main-native-file-receipt", marker),
			identity("test.main-xopen-receipt", marker),
			open_epoch,
			identity("test.native-shm-callback-cohort", marker),
			attachment_epoch ? std::move(*attachment_epoch)
							 : identity("test.writer-attachment-epoch", marker));
		require(bound.has_value(), "bind checked writer native attachment");
		return std::move(*bound);
	}

	[[nodiscard]] sqlite_shm_writer_map_request
	writer_request(const sqlite_shm_lease_family_binding& binding,
				   const sqlite_backend_opaque_identity& connection,
				   const std::uint8_t alias,
				   const std::uint8_t thread,
				   const std::uint8_t invocation,
				   const int page,
				   const int extend)
	{
		auto alias_lifetime = identity("test.alias-lifetime", alias);
		auto open_epoch = identity("test.open-epoch", alias);
		return {binding,
				alias_lifetime,
				connection,
				writer_attachment(binding,
								  alias_lifetime,
								  connection,
								  open_epoch,
								  alias,
								  identity("test.writer-attachment-epoch", invocation)),
				callback(thread, invocation),
				page,
				4096,
				extend};
	}

	[[nodiscard]] sqlite_shm_reader_map_request
	reader_request(const sqlite_shm_lease_family_binding& binding,
				   const sqlite_backend_opaque_identity& connection,
				   const std::uint8_t alias,
				   const std::uint8_t thread,
				   const std::uint8_t invocation,
				   const int page = 0)
	{
		return {binding,
				identity("test.alias-lifetime", alias),
				connection,
				callback(thread, invocation),
				page,
				4096,
				0};
	}

	[[nodiscard]] sqlite_shm_reader_attachment_map_request reader_attachment_request(
		const sqlite_shm_lease_family_binding& binding,
		const sqlite_backend_opaque_identity& connection,
		const std::uint8_t alias,
		const std::uint8_t thread,
		const std::uint8_t invocation,
		const int page,
		const std::uint64_t writer_generation,
		std::optional<sqlite_backend_opaque_identity> attachment_epoch = std::nullopt,
		const std::uint64_t registry_open_token = 0U)
	{
		auto alias_lifetime = identity("test.alias-lifetime", alias);
		auto expected = sqlite_same_process_shm_lease_test_peer::reader_attachment_reservation(
			binding,
			identity("test.reader-runtime-lifetime-pin", alias),
			alias_lifetime,
			connection,
			identity("test.reader-main-native-file-receipt", alias),
			identity("test.reader-main-xopen-receipt", alias),
			identity("test.reader-open-epoch", alias),
			writer_generation,
			identity("test.reader-callback-cohort", alias),
			attachment_epoch ? std::move(*attachment_epoch)
							 : identity("test.reader-attachment-epoch", alias),
			registry_open_token);
		require(expected.has_value(), "bind checked reader attachment reservation");
		return {binding,
				std::move(alias_lifetime),
				connection,
				std::move(*expected),
				callback(thread, invocation),
				page,
				4096,
				0};
	}

	[[nodiscard]] sqlite_shm_reader_open_epoch_binding
	reader_open_epoch_binding(const sqlite_shm_reader_attachment_reservation_identity& attachment)
	{
		return {
			attachment.family(),
			attachment.runtime_lifetime_pin(),
			attachment.alias_lifetime(),
			attachment.connection_token(),
			attachment.main_native_file_receipt(),
			attachment.main_xopen_receipt(),
			attachment.open_epoch(),
			attachment.callback_cohort(),
			attachment.target_identity(),
		};
	}

	[[nodiscard]] sqlite_shm_reader_open_epoch_binding
	reader_open_epoch_binding(const sqlite_shm_lease_family_binding& binding,
							  const std::uint8_t marker)
	{
		return {
			binding,
			identity("test.reader-runtime-lifetime-pin", marker),
			identity("test.alias-lifetime", marker),
			identity("test.connection", marker),
			identity("test.reader-main-native-file-receipt", marker),
			identity("test.reader-main-xopen-receipt", marker),
			identity("test.reader-open-epoch", marker),
			identity("test.reader-callback-cohort", marker),
			std::nullopt,
		};
	}

	[[nodiscard]] sqlite_shm_reader_attachment_reservation_identity
	reader_attachment_for_open(const sqlite_shm_reader_open_epoch_binding& binding,
							   const std::uint64_t writer_generation,
							   const sqlite_backend_opaque_identity& attachment_epoch,
							   const std::uint64_t registry_open_token)
	{
		auto attachment = sqlite_same_process_shm_lease_test_peer::reader_attachment_reservation(
			binding.family,
			binding.runtime_lifetime_pin,
			binding.alias_lifetime,
			binding.connection_token,
			binding.main_native_file_receipt,
			binding.main_xopen_receipt,
			binding.open_epoch,
			writer_generation,
			binding.callback_cohort,
			attachment_epoch,
			registry_open_token,
			binding.target_identity);
		require(attachment.has_value(), "bind reader attachment to exact reader-open epoch");
		return std::move(*attachment);
	}

	[[nodiscard]] sqlite_shm_reader_session_request
	reader_session_request(const sqlite_shm_reader_attachment_map_request& request,
						   const std::uint8_t marker)
	{
		return {request.expected_attachment,
				reader_session_execution(marker),
				identity("test.reader-transaction-epoch", marker),
				identity("test.reader-decode-attempt", marker),
				identity("test.reader-authority-read-receipt", marker)};
	}

	[[nodiscard]] sqlite_shm_mapping_tuple
	mapping(const int page, const volatile void* pointer, const std::uint64_t sealed_size)
	{
		const auto offset = static_cast<std::uint64_t>(page) * 4096U;
		return {page, 4096, offset, 4096U, pointer, sealed_size};
	}

	[[nodiscard]] sqlite_shm_verified_writer_eligibility_receipt
	eligibility_receipt(const sqlite_shm_lease_family_binding& binding,
						const sqlite_backend_opaque_identity& connection,
						const sqlite_backend_opaque_identity& open_epoch,
						const std::uint8_t marker)
	{
		return sqlite_same_process_shm_lease_test_peer::eligibility(
			binding,
			connection,
			open_epoch,
			effect_gate(connection, marker),
			identity("test.complete-current-v3-gate", marker));
	}

	void verify_production_writer_eligibility_factory_is_exact()
	{
		const auto binding = family(1U);
		const auto connection = identity("test.production-eligibility-connection", 1U);
		const auto open_epoch = identity("test.production-eligibility-open-epoch", 1U);
		auto mismatched = sqlite_shm_writer_eligibility_receipt_production_factory::seal(
			binding,
			connection,
			open_epoch,
			effect_gate(identity("test.other-connection", 1U), 1U));
		require(!mismatched,
				"production writer eligibility factory accepted an effect for another connection");

		auto sealed = sqlite_shm_writer_eligibility_receipt_production_factory::seal(
			binding, connection, open_epoch, effect_gate(connection, 2U));
		require(sealed.has_value(),
				"production writer eligibility factory rejected a complete cut");
		require(sealed->family() == binding && sealed->connection_token() == connection &&
					sealed->open_epoch() == open_epoch &&
					sealed->complete_current_v3_gate() == sealed->effect_gate().validation_receipt,
				"production writer eligibility factory changed the authenticated cut");
	}

	[[nodiscard]] sqlite_shm_verified_writer_post_map_receipt
	writer_receipt(const sqlite_shm_writer_map_request& request,
				   const sqlite_backend_opaque_identity& open_epoch,
				   const sqlite_shm_mapping_tuple& mapped,
				   const sqlite_shm_writer_extend_pair pair,
				   const std::uint8_t marker)
	{
		return sqlite_same_process_shm_lease_test_peer::writer_map(
			request, open_epoch, mapped, pair, identity("test.holder-effect", marker));
	}

	[[nodiscard]] sqlite_shm_writer_eligibility
	install_eligibility(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
						const sqlite_shm_lease_family_binding& binding,
						const sqlite_backend_opaque_identity& connection,
						const sqlite_backend_opaque_identity& open_epoch,
						const std::uint8_t marker)
	{
		auto installed = coordinator.install_writer_eligibility(
			eligibility_receipt(binding, connection, open_epoch, marker));
		require(installed.has_value(), "install writer eligibility");
		return std::move(*installed);
	}

	[[nodiscard]] sqlite_shm_writer_post_native_mapping
	record_native_mapping(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
						  sqlite_shm_writer_map_inflight& inflight,
						  const volatile void* native_mapping)
	{
		auto receipt = sqlite_writer_shm_native_map_receipt_validator::validate(
			inflight, sqlite_ok_status, native_mapping);
		require(receipt.has_value(), "validate cleanup-only native writer mapping");
		auto recorded = coordinator.record_writer_native_mapping(inflight, *receipt);
		require(recorded.has_value() && !inflight.valid(),
				"record cleanup-only native writer mapping");
		return std::move(*recorded);
	}

	[[nodiscard]] sqlite_shm_pending_mapping
	install_pending(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
					const sqlite_shm_writer_map_request& request,
					const sqlite_backend_opaque_identity& open_epoch,
					const sqlite_shm_mapping_tuple& mapped,
					const sqlite_shm_writer_extend_pair pair,
					const std::uint8_t marker)
	{
		auto begun = coordinator.begin_writer_map(request);
		require(begun.has_value(), "begin writer map");
		auto inflight = std::move(*begun);
		auto post_native = record_native_mapping(coordinator, inflight, mapped.native_mapping);
		auto pending = coordinator.install_pending(
			post_native, writer_receipt(request, open_epoch, mapped, pair, marker));
		require(pending.has_value() && !post_native.valid(), "install post-native pending");
		return std::move(*pending);
	}

	[[nodiscard]] sqlite_shm_writer_holder
	promote(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
			sqlite_shm_pending_mapping& pending,
			const sqlite_shm_writer_eligibility& eligibility)
	{
		auto promoted = coordinator.promote_writer(pending, eligibility);
		require(promoted.has_value() && !pending.valid(), "promote writer holder");
		return std::move(*promoted);
	}

	struct live_writer_tokens
	{
		sqlite_shm_writer_eligibility eligibility;
		sqlite_shm_writer_holder holder;
	};

	struct live_reader_group_tokens
	{
		sqlite_shm_reader_session_request session_request;
		sqlite_shm_reader_session session;
		sqlite_shm_reader_handoff handoff;
		std::optional<sqlite_shm_reader_cached_member_identity> cached_member;
	};

	struct registered_reader_open_tokens
	{
		std::uint64_t registry_open_token{};
		std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal> seal;
		sqlite_shm_reader_open_epoch_binding binding;
	};

	[[nodiscard]] registered_reader_open_tokens
	register_reader_open(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
						 const std::uint64_t registry_open_token,
						 sqlite_shm_reader_open_epoch_binding binding)
	{
		auto seal = std::make_shared<detail::sqlite_shm_reader_open_lineage_seal>();
		require(sqlite_same_process_shm_lease_test_peer::register_reader_open(
					coordinator, registry_open_token, seal, binding)
					.has_value(),
				"register exact reader open epoch");
		return {
			registry_open_token,
			std::move(seal),
			std::move(binding),
		};
	}

	void close_and_release_registered_reader_open(
		sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
		registered_reader_open_tokens& open,
		const std::uint8_t marker)
	{
		const auto close_callback = callback(12U, marker);
		auto close = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
			coordinator,
			open.registry_open_token,
			open.seal,
			open.binding,
			sqlite_shm_reader_close_request{close_callback});
		require(close && close->valid() &&
					close->route() == sqlite_shm_reader_close_route::close_without_group,
				"admit exact registered reader close");
		const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
			*close,
			close_callback,
			sqlite_shm_reader_close_evidence_kind::exact_native_result,
			sqlite_ok_status,
			identity("test.reader-registered-close-effect", marker));
		auto completed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
			coordinator, open.registry_open_token, open.seal, open.binding, *close, receipt);
		require(completed && completed->kind() == sqlite_shm_reader_close_terminal_kind::closed &&
					!close->valid(),
				"complete exact registered reader close");
		require(sqlite_same_process_shm_lease_test_peer::release_reader_open(
					coordinator, open.registry_open_token, open.seal)
					.has_value(),
				"compact exact registered reader close");
	}

	[[nodiscard]] live_writer_tokens
	install_live_writer(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
						const sqlite_shm_lease_family_binding& binding,
						const sqlite_backend_opaque_identity& connection,
						const sqlite_backend_opaque_identity& open_epoch,
						const std::uint8_t marker,
						const volatile void* page)
	{
		auto eligibility =
			install_eligibility(coordinator, binding, connection, open_epoch, marker);
		auto pending = install_pending(coordinator,
									   writer_request(binding, connection, marker, 1, marker, 0, 1),
									   open_epoch,
									   mapping(0, page, 4096U),
									   sqlite_shm_writer_extend_pair::one_one,
									   marker);
		auto holder = promote(coordinator, pending, eligibility);
		return {std::move(eligibility), std::move(holder)};
	}

	[[nodiscard]] live_reader_group_tokens
	install_live_reader_group(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
							  const sqlite_shm_lease_family_binding& binding,
							  const sqlite_backend_opaque_identity& connection,
							  const std::uint8_t marker,
							  const std::uint64_t generation,
							  const volatile void* page)
	{
		const auto map_request =
			reader_attachment_request(binding, connection, marker, 2, marker, 0, generation);
		const auto session_request = reader_session_request(map_request, marker);
		auto session = coordinator.begin_reader_session(session_request);
		require(session.has_value(), "reserve exact reader-group fixture session");
		auto map = coordinator.begin_reader_map(*session, map_request);
		require(map.has_value(), "begin exact reader-group fixture map");
		auto committed = coordinator.commit_reader_map(
			*map,
			sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
				map_request,
				generation,
				mapping(0, page, 4096U),
				identity("test.reader-group-zero-resize", marker)),
			*session);
		require(committed && committed->formed_group(), "commit exact reader-group fixture map");
		auto handoff = committed->take_handoff();
		require(handoff.has_value(), "take exact reader-group fixture handoff");
		return {
			session_request,
			std::move(*session),
			std::move(*handoff),
			committed->cached_member(),
		};
	}

	void cleanup_writer(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
						sqlite_shm_pending_mapping& pending,
						const sqlite_shm_callback_execution_receipt& cleanup_callback)
	{
		auto begun = coordinator.begin_writer_cleanup(pending, cleanup_callback);
		require(begun.has_value() && !pending.valid(), "hide pending before writer cleanup");
		auto cleanup = std::move(*begun);
		require(
			coordinator
					.complete_writer_cleanup(cleanup,
											 cleanup_callback,
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value() &&
				!cleanup.valid(),
			"complete pending writer cleanup");
	}

	void cleanup_rejected_writer(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
								 sqlite_shm_writer_post_native_mapping& post_native,
								 const sqlite_shm_callback_execution_receipt& cleanup_callback)
	{
		auto begun = coordinator.begin_writer_cleanup(post_native, cleanup_callback);
		require(begun.has_value() && !post_native.valid(),
				"hide rejected writer mapping before cleanup");
		auto cleanup = std::move(*begun);
		require(
			coordinator
					.complete_writer_cleanup(cleanup,
											 cleanup_callback,
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value() &&
				!cleanup.valid(),
			"complete rejected writer cleanup");
	}

	void retire_last(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
					 sqlite_shm_writer_holder& holder,
					 const sqlite_shm_callback_execution_receipt& release_callback)
	{
		const auto generation = holder.generation();
		auto retirement = coordinator.release_writer_holder(holder, release_callback);
		require(retirement.has_value() &&
					retirement->decision() == sqlite_shm_writer_retirement_decision::ready &&
					retirement->generation() == generation && !holder.valid(),
				"last holder reaches ready retirement");
		require(
			coordinator
					.complete_writer_cleanup(retirement->cleanup(),
											 release_callback,
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value() &&
				!retirement->cleanup().valid(),
			"complete writer retirement");
	}

	class unpublished_cleanup_writer_observation_port final
		: public sqlite_writer_shm_mapping_epoch_observation_port
	{
	  public:
		explicit unpublished_cleanup_writer_observation_port(
			sqlite_writer_shm_mapping_epoch_post_observation observation)
			: observation_{std::move(observation)}
		{
		}

		[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_post_observation>
		observe_after_native_map(const sqlite_writer_shm_mapping_epoch_binding&,
								 const sqlite_writer_shm_stat_census&,
								 const volatile void*) override
		{
			return observation_;
		}

	  private:
		sqlite_writer_shm_mapping_epoch_post_observation observation_;
	};

	class unpublished_cleanup_writer_epoch_port final : public sqlite_writer_shm_mapping_epoch_port
	{
	  public:
		unpublished_cleanup_writer_epoch_port(
			const std::uint8_t marker,
			sqlite_writer_shm_stat_census pre_stat,
			sqlite_writer_shm_mapping_epoch_post_observation post_observation)
			: marker_{marker}, pre_stat_{std::move(pre_stat)},
			  observer_{std::make_shared<unpublished_cleanup_writer_observation_port>(
				  std::move(post_observation))}
		{
		}

	  protected:
		[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_preparation>
		arm_watch_before_pre_stat(const sqlite_writer_shm_mapping_epoch_request&) override
		{
			return sqlite_writer_shm_mapping_epoch_preparation{
				identity("test.phase2.writer-epoch", marker_),
				identity("test.phase2.writer-watch", marker_),
				pre_stat_,
				observer_,
			};
		}

	  private:
		std::uint8_t marker_{};
		sqlite_writer_shm_stat_census pre_stat_;
		std::shared_ptr<unpublished_cleanup_writer_observation_port> observer_;
	};

	struct unpublished_cleanup_writer_native_source
	{
		sqlite_writer_shm_native_lifetime_revoker revoker;
		sqlite_writer_shm_native_lifetime_source source;
		std::shared_ptr<int> owner;
	};

	struct unpublished_cleanup_writer_epoch_sources
	{
		sqlite_backend_opaque_identity retained_parent_receipt;
		sqlite_backend_opaque_identity wal_file_receipt;
		sqlite_backend_opaque_identity wal_xopen_receipt;
		sqlite_backend_opaque_identity shm_attachment_receipt;
		sqlite_backend_opaque_identity expected_shm_leaf;
		unpublished_cleanup_writer_native_source parent;
		unpublished_cleanup_writer_native_source main;
		unpublished_cleanup_writer_native_source wal;
		unpublished_cleanup_writer_native_source shm;
	};

	[[nodiscard]] unpublished_cleanup_writer_native_source
	make_unpublished_cleanup_writer_native_source(
		const sqlite_writer_shm_native_lifetime_role role,
		const sqlite_backend_opaque_identity& lifetime_identity,
		const sqlite_backend_opaque_identity& semantic_receipt,
		std::optional<sqlite_backend_opaque_identity> xopen_receipt,
		const int owner_marker)
	{
		auto owner = std::make_shared<int>(owner_marker);
		auto source = sqlite_same_process_shm_registry_test_peer::native_lifetime_source(
			role, lifetime_identity, semantic_receipt, std::move(xopen_receipt), owner);
		return {std::move(source.first), std::move(source.second), std::move(owner)};
	}

	[[nodiscard]] std::shared_ptr<unpublished_cleanup_writer_epoch_sources>
	make_unpublished_cleanup_writer_epoch_sources(const sqlite_shm_writer_map_request& request,
												  const std::uint8_t marker)
	{
		const auto retained_parent = identity("test.phase2.writer-retained-parent", marker);
		const auto wal_file = identity("test.phase2.writer-wal-file", marker);
		const auto wal_xopen = identity("test.phase2.writer-wal-xopen", marker);
		const auto shm_attachment = identity("test.phase2.writer-shm-attachment", marker);
		return std::make_shared<unpublished_cleanup_writer_epoch_sources>(
			unpublished_cleanup_writer_epoch_sources{
				retained_parent,
				wal_file,
				wal_xopen,
				shm_attachment,
				identity("test.phase2.writer-shm-leaf", marker),
				make_unpublished_cleanup_writer_native_source(
					sqlite_writer_shm_native_lifetime_role::retained_parent,
					identity("test.phase2.writer-parent-lifetime", marker),
					retained_parent,
					std::nullopt,
					1),
				make_unpublished_cleanup_writer_native_source(
					sqlite_writer_shm_native_lifetime_role::main_database,
					identity("test.phase2.writer-main-lifetime", marker),
					request.attachment.main_native_file_receipt(),
					request.attachment.main_xopen_receipt(),
					2),
				make_unpublished_cleanup_writer_native_source(
					sqlite_writer_shm_native_lifetime_role::write_ahead_log,
					identity("test.phase2.writer-wal-lifetime", marker),
					wal_file,
					wal_xopen,
					3),
				make_unpublished_cleanup_writer_native_source(
					sqlite_writer_shm_native_lifetime_role::shared_memory_attachment,
					identity("test.phase2.writer-shm-lifetime", marker),
					shm_attachment,
					std::nullopt,
					4),
			});
	}

	[[nodiscard]] sqlite_writer_shm_stat_census
	unpublished_cleanup_writer_stat(const std::uint8_t marker)
	{
		return {
			sqlite_writer_shm_entry_state::direct_regular,
			identity("test.phase2.writer-parent", marker),
			identity("test.phase2.writer-filesystem", marker),
			identity("test.phase2.writer-mount", marker),
			identity("test.phase2.writer-object", marker),
			identity("test.phase2.writer-entry", marker),
			4096U,
		};
	}

	[[nodiscard]] sqlite_writer_shm_mapping_epoch_post_observation
	unpublished_cleanup_writer_post_observation(
		const unpublished_cleanup_writer_epoch_sources& sources, const std::uint8_t marker)
	{
		sqlite_writer_shm_namespace_event_census events{
			identity("test.phase2.writer-watch", marker),
			sources.expected_shm_leaf,
		};
		events.trusted_stat_watch_profile = true;
		sqlite_writer_shm_effect_census effects;
		effects.sqlite_source_id = identity("test.phase2.writer-sqlite-source", marker);
		effects.callback_transcript = identity("test.phase2.writer-callback-transcript", marker);
		effects.wal_write_lock_receipt = identity("test.phase2.writer-wal-write-lock", marker);
		effects.effect_gate_receipt = identity("test.phase2.writer-map-effect-gate", marker);
		effects.effect_receipt = identity("test.phase2.writer-map-effect", marker);
		effects.size_before = 4096U;
		effects.size_after = 4096U;
		effects.requested_range_end = 4096U;
		effects.complete = true;
		effects.result_confirmed_success = true;
		return {
			unpublished_cleanup_writer_stat(marker),
			std::move(events),
			std::move(effects),
			sqlite_writer_shm_observed_transition::preexisting_preallocated,
		};
	}

	struct unpublished_cleanup_registry_bound_writer
	{
		live_writer_tokens tokens;
		std::shared_ptr<unpublished_cleanup_writer_epoch_sources> epoch_sources;
	};

	[[nodiscard]] unpublished_cleanup_registry_bound_writer
	install_unpublished_cleanup_registry_bound_writer(
		sqlite_same_process_shm_mapping_registry& registry,
		sqlite_shm_registry_family_pin& family_pin,
		sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
		const sqlite_shm_lease_family_binding& binding,
		const sqlite_backend_opaque_identity& connection,
		const sqlite_backend_opaque_identity& open_epoch,
		const std::uint8_t marker,
		const volatile void* native_page)
	{
		auto request = writer_request(binding, connection, marker, 1U, marker, 0, 1);
		auto eligibility =
			install_eligibility(coordinator, binding, connection, open_epoch, marker);
		auto gate = registry.advance_positive_writer_attachment_gate(
			family_pin, request.attachment, std::span<sqlite_shm_pending_mapping*>{}, eligibility);
		require(gate &&
					gate->progress == sqlite_shm_positive_writer_attachment_gate_progress::complete,
				"activate unpublished-cleanup registry writer gate");

		auto sources = make_unpublished_cleanup_writer_epoch_sources(request, marker);
		auto parent_pin = sources->parent.source.mint_pin();
		auto main_pin = sources->main.source.mint_pin();
		auto wal_pin = sources->wal.source.mint_pin();
		auto shm_pin = sources->shm.source.mint_pin();
		require(parent_pin && main_pin && wal_pin && shm_pin,
				"mint unpublished-cleanup writer native lifetime pins");
		unpublished_cleanup_writer_epoch_port port{
			marker,
			unpublished_cleanup_writer_stat(marker),
			unpublished_cleanup_writer_post_observation(*sources, marker),
		};
		auto activation = port.arm(sqlite_writer_shm_mapping_epoch_request{
			sqlite_writer_shm_mapping_epoch_binding{
				request,
				request.caller_extend,
				sources->expected_shm_leaf,
				sources->retained_parent_receipt,
				sources->wal_file_receipt,
				sources->wal_xopen_receipt,
				sources->shm_attachment_receipt,
				std::nullopt,
			},
			std::move(*parent_pin),
			std::move(*main_pin),
			std::move(*wal_pin),
			std::move(*shm_pin),
		});
		require(activation.has_value(), "arm unpublished-cleanup writer epoch");
		auto observer = activation->take_observer();
		auto arm = activation->take_arm();
		auto begun = registry.begin_writer_map(family_pin, std::move(arm), request);
		require(begun.has_value(), "begin unpublished-cleanup registry writer map");
		auto inflight = std::move(*begun);
		auto native = sqlite_writer_shm_native_map_receipt_validator::validate(
			inflight, sqlite_ok_status, native_page);
		require(native.has_value(), "validate unpublished-cleanup writer native result");
		auto epoch = seal_sqlite_writer_shm_mapping_epoch(observer, native_page);
		require(epoch.has_value(), "seal unpublished-cleanup writer epoch");
		auto post_native = coordinator.record_writer_native_mapping(inflight, *native);
		require(post_native.has_value(), "record unpublished-cleanup writer native result");
		const auto& effects = epoch->post_observation().effects;
		auto proof = sqlite_same_process_shm_registry_test_peer::writer_route_proof(
			sqlite_writer_shm_mapping_semantic_route::one_one_preexisting_preallocated,
			request,
			request.caller_extend,
			identity("test.phase2.writer-authenticated-route", marker),
			request.attachment.main_native_file_receipt(),
			request.attachment.main_xopen_receipt(),
			effects.sqlite_source_id,
			effects.callback_transcript,
			effects.wal_write_lock_receipt,
			effects.effect_gate_receipt,
			identity("test.phase2.writer-route-validation", marker));
		auto receipt = sqlite_writer_shm_mapping_receipt_validator::validate(*epoch, proof);
		require(receipt.has_value(), "validate unpublished-cleanup writer route");
		auto holder = registry.complete_gate_winning_writer_map_before_callback_return(
			family_pin, *post_native, *receipt);
		require(holder && holder->valid(), "publish unpublished-cleanup registry writer");
		return {
			{std::move(eligibility), std::move(*holder)},
			std::move(sources),
		};
	}

	struct unpublished_cleanup_registry_fixture
	{
		sqlite_shm_lease_family_binding family;
		sqlite_backend_opaque_identity alias_lifetime;
		std::shared_ptr<void> runtime_owner;
		std::unique_ptr<sqlite_same_process_shm_mapping_registry> registry;
		std::optional<sqlite_shm_registry_alias_pin> alias;
		std::optional<sqlite_shm_registry_family_pin> family_pin;
		sqlite_same_process_shm_mapping_lease_coordinator* coordinator{};
	};

	[[nodiscard]] unpublished_cleanup_registry_fixture
	make_unpublished_cleanup_registry_fixture(const std::uint8_t marker)
	{
		auto binding = family(marker);
		auto alias_lifetime = identity("test.alias-lifetime", marker);
		auto runtime_identity = identity("test.phase2.runtime-lifetime", marker);
		auto runtime_pin_identity = identity("test.phase2.runtime-pin", marker);
		auto runtime_owner = std::make_shared<int>(marker);
		auto owner =
			sqlite_same_process_shm_registry_test_peer::process_owner(binding.process_instance);
		auto created = sqlite_same_process_shm_mapping_registry::create(std::move(owner));
		require(created.has_value(), "create unpublished-cleanup registry fixture");
		auto registry = std::move(*created);
		auto adopted = sqlite_same_process_shm_registry_test_peer::adopt_runtime_lifetime(
			*registry, runtime_identity, runtime_pin_identity, runtime_owner);
		require(adopted.has_value(), "adopt unpublished-cleanup runtime lifetime");
		auto alias_binding = sqlite_same_process_shm_registry_test_peer::alias_binding(
			binding.process_instance,
			binding.shared_runtime_vfs_cohort,
			alias_lifetime,
			std::move(*adopted));
		auto reserved = registry->reserve_alias(std::move(alias_binding));
		require(reserved.has_value(), "reserve unpublished-cleanup alias");
		std::optional<sqlite_shm_registry_alias_pin> alias{std::move(*reserved)};
		require(registry->begin_alias_register(*alias).has_value(),
				"arm unpublished-cleanup alias registration");
		const auto registration = sqlite_same_process_shm_registry_test_peer::registration_receipt(
			binding.process_instance,
			binding.shared_runtime_vfs_cohort,
			alias_lifetime,
			runtime_identity,
			runtime_pin_identity,
			identity("test.phase2.registration-epoch", marker));
		require(registry->confirm_alias_registered(*alias, registration).has_value(),
				"confirm unpublished-cleanup alias registration");
		auto pinned = registry->install_or_join_family(*alias, binding);
		require(pinned.has_value(), "install unpublished-cleanup family");
		std::optional<sqlite_shm_registry_family_pin> family_pin{std::move(*pinned)};
		auto* coordinator =
			sqlite_same_process_shm_registry_test_peer::coordinator(*registry, binding);
		require(coordinator != nullptr, "resolve unpublished-cleanup coordinator");
		return {
			std::move(binding),
			std::move(alias_lifetime),
			std::move(runtime_owner),
			std::move(registry),
			std::move(alias),
			std::move(family_pin),
			coordinator,
		};
	}

	[[nodiscard]] sqlite_shm_reader_pre_sqlite_session_request
	unpublished_cleanup_pre_sqlite_request(const unpublished_cleanup_registry_fixture& fixture,
										   const std::uint8_t marker)
	{
		return {
			fixture.family,
			fixture.alias_lifetime,
			identity("test.phase2.reader-connection", marker),
			identity("test.phase2.reader-main-native", marker),
			identity("test.phase2.reader-main-xopen", marker),
			identity("test.phase2.reader-open-epoch", marker),
			identity("test.phase2.reader-callback-cohort", marker),
			reader_session_execution(marker),
			identity("test.phase2.reader-transaction", marker),
			identity("test.phase2.reader-decode", marker),
			identity("test.phase2.reader-authority-read", marker),
			std::nullopt,
		};
	}

	[[nodiscard]] sqlite_shm_reader_open_binding
	unpublished_cleanup_open_binding(const sqlite_shm_reader_pre_sqlite_session_request& request)
	{
		return {
			request.family,
			request.alias_lifetime,
			request.connection_token,
			request.main_native_file_receipt,
			request.main_xopen_receipt,
			request.open_epoch,
			request.callback_cohort,
			request.target_identity,
		};
	}

	struct unpublished_cleanup_candidate
	{
		unpublished_cleanup_registry_fixture fixture;
		std::shared_ptr<int> native_page;
		std::shared_ptr<unpublished_cleanup_writer_epoch_sources> writer_epoch_sources;
		live_writer_tokens writer;
		sqlite_shm_reader_pre_sqlite_session_request pre_sqlite;
		sqlite_shm_reader_open_authority open;
		sqlite_shm_reader_session_request session_request;
		sqlite_shm_reader_session session;
	};

	[[nodiscard]] unpublished_cleanup_candidate
	make_unpublished_cleanup_candidate(const std::uint8_t marker)
	{
		auto fixture = make_unpublished_cleanup_registry_fixture(marker);
		auto native_page = std::make_shared<int>();
		const auto writer_connection = identity("test.connection", marker);
		const auto writer_open_epoch = identity("test.open-epoch", marker);
		auto writer = install_unpublished_cleanup_registry_bound_writer(*fixture.registry,
																		*fixture.family_pin,
																		*fixture.coordinator,
																		fixture.family,
																		writer_connection,
																		writer_open_epoch,
																		marker,
																		native_page.get());
		auto pre_sqlite = unpublished_cleanup_pre_sqlite_request(fixture, marker);
		auto open = sqlite_same_process_shm_registry_test_peer::reader_open(
			*fixture.registry, *fixture.family_pin, unpublished_cleanup_open_binding(pre_sqlite));
		require(open.has_value(), "acquire unpublished-cleanup reader open");
		auto admitted = fixture.registry->admit_reader_session_before_sqlite(
			*fixture.family_pin, *open, pre_sqlite);
		require(admitted &&
					admitted->kind() ==
						sqlite_shm_reader_session_admission_kind::
							reserved_for_local_proposal_candidate &&
					admitted->proposal_request(),
				"admit unpublished-cleanup reader candidate");
		auto session_request = *admitted->proposal_request();
		auto session = admitted->take_session();
		require(session && session->valid(), "take unpublished-cleanup reader session");
		return {
			std::move(fixture),
			std::move(native_page),
			std::move(writer.epoch_sources),
			std::move(writer.tokens),
			std::move(pre_sqlite),
			std::move(*open),
			std::move(session_request),
			std::move(*session),
		};
	}

	[[nodiscard]] sqlite_shm_reader_attachment_map_request
	unpublished_cleanup_map_request(const sqlite_shm_reader_session_request& session,
									const std::uint8_t marker)
	{
		return {
			session.attachment.family(),
			session.attachment.alias_lifetime(),
			session.attachment.connection_token(),
			session.attachment,
			callback(30U, marker),
			0,
			4096,
			0,
		};
	}

	struct unpublished_cleanup_attempt
	{
		unpublished_cleanup_candidate candidate;
		sqlite_shm_reader_attachment_map_request map_request;
		sqlite_shm_verified_reader_attachment_post_map_receipt mapped_receipt;
		sqlite_backend_opaque_identity session_no_pointer_terminal_receipt;
		sqlite_shm_reader_attachment_map_inflight inflight;
	};

	[[nodiscard]] unpublished_cleanup_attempt
	prepare_mapped_validation_failure_cleanup(unpublished_cleanup_candidate candidate,
											  const std::uint8_t marker)
	{
		auto map_request = unpublished_cleanup_map_request(candidate.session_request,
														   static_cast<std::uint8_t>(marker + 1U));
		auto inflight = candidate.fixture.registry->begin_reader_map(
			*candidate.fixture.family_pin, candidate.session, map_request);
		require(inflight && inflight->valid(), "begin mapped-validation-failure first map");
		auto mapped_receipt = sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
			map_request,
			candidate.writer.holder.generation(),
			mapping(0, candidate.native_page.get(), 4096U),
			identity("test.phase2.mapped-effect", marker));
		sqlite_same_process_shm_lease_test_peer::fail_next_reader_map_terminal_commit(
			*candidate.fixture.coordinator);
		auto rejected = candidate.fixture.registry->commit_reader_map(
			*candidate.fixture.family_pin, *inflight, mapped_receipt, candidate.session);
		require(
			!rejected &&
				rejected.error().reason ==
					sqlite_shm_lease_rejection_reason::unpublished_cleanup_required &&
				rejected.error().action ==
					sqlite_shm_lease_recovery_action::attempt_nonremoving_unmap_then_outer_ioerr &&
				inflight->valid() && candidate.session.valid() &&
				!candidate.fixture.coordinator->snapshot().quarantined,
			"deterministic prepublication rejection must retain exact cleanup lineage");
		return {
			std::move(candidate),
			std::move(map_request),
			std::move(mapped_receipt),
			identity("test.phase2.session-no-pointer-terminal", marker),
			std::move(*inflight),
		};
	}

	[[nodiscard]] unpublished_cleanup_attempt
	prepare_mapped_validation_failure_cleanup(const std::uint8_t marker)
	{
		return prepare_mapped_validation_failure_cleanup(make_unpublished_cleanup_candidate(marker),
														 marker);
	}

	[[nodiscard]] sqlite_shm_verified_reader_unpublished_cleanup_receipt
	mapped_validation_failure_cleanup_receipt(const unpublished_cleanup_attempt& attempt)
	{
		return sqlite_same_process_shm_lease_test_peer::reader_unpublished_cleanup(
			attempt.inflight,
			sqlite_shm_reader_unpublished_cleanup_entry_kind::exact_mapped_validation_failure,
			attempt.map_request,
			attempt.candidate.session_request,
			attempt.mapped_receipt.generation(),
			sqlite_ok_status,
			attempt.mapped_receipt.mapping().native_mapping,
			0,
			attempt.mapped_receipt.observed_attachment(),
			attempt.mapped_receipt.zero_resize_effect_receipt(),
			attempt.session_no_pointer_terminal_receipt);
	}

	struct unpublished_cleanup_admitted
	{
		unpublished_cleanup_candidate candidate;
		sqlite_shm_reader_attachment_map_request map_request;
		sqlite_backend_opaque_identity mapped_effect_receipt;
		sqlite_backend_opaque_identity session_no_pointer_terminal_receipt;
		sqlite_shm_reader_unpublished_cleanup_obligation cleanup;
	};

	[[nodiscard]] unpublished_cleanup_admitted
	begin_mapped_validation_failure_cleanup(unpublished_cleanup_attempt attempt)
	{
		const auto receipt = mapped_validation_failure_cleanup_receipt(attempt);
		auto cleanup = attempt.candidate.fixture.registry->begin_reader_unpublished_cleanup(
			*attempt.candidate.fixture.family_pin,
			attempt.inflight,
			receipt,
			attempt.candidate.session);
		const auto snapshot = attempt.candidate.fixture.coordinator->snapshot();
		const auto lifecycle = sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(
			*attempt.candidate.fixture.coordinator);
		const auto& reservation = only_attachment_reservation(lifecycle);
		require(
			cleanup && cleanup->valid() && !attempt.inflight.valid() &&
				!attempt.candidate.session.valid() && !snapshot.quarantined &&
				snapshot.reader_inflight_count == 0U &&
				snapshot.reader_session_reservation_count == 0U &&
				snapshot.reader_session_owner_count == 0U &&
				snapshot.reader_attachment_live_member_count == 0U &&
				snapshot.reader_unpublished_cleanup_admitted_count == 1U &&
				snapshot.reader_unpublished_cleanup_confirmed_count == 0U &&
				snapshot.reader_logical_ack_awaiting_count == 0U &&
				lifecycle.live_custody_kind_counts[enum_index(
					detail::sqlite_shm_reader_custody_kind::generation_group_count)] == 0U &&
				lifecycle.live_custody_kind_counts[enum_index(
					detail::sqlite_shm_reader_custody_kind::attachment_group_handoff)] == 0U &&
				reservation.phase ==
					detail::sqlite_shm_reader_attachment_reservation_phase::
						unpublished_cleanup_admitted &&
				reservation.logical_ack_phase ==
					detail::sqlite_shm_reader_logical_ack_phase::not_applicable,
			"mapped first-map cleanup admission must consume map/session with zero publication");
		return {
			std::move(attempt.candidate),
			std::move(attempt.map_request),
			attempt.mapped_receipt.zero_resize_effect_receipt(),
			std::move(attempt.session_no_pointer_terminal_receipt),
			std::move(*cleanup),
		};
	}

	struct unpublished_cleanup_confirmed
	{
		unpublished_cleanup_candidate candidate;
		sqlite_shm_reader_attachment_map_request map_request;
		sqlite_backend_opaque_identity mapped_effect_receipt;
		sqlite_backend_opaque_identity session_no_pointer_terminal_receipt;
		sqlite_backend_opaque_identity cleanup_effect_receipt;
		sqlite_backend_opaque_identity latch_reset_receipt;
	};

	[[nodiscard]] unpublished_cleanup_confirmed
	confirm_mapped_validation_failure_cleanup(unpublished_cleanup_attempt attempt,
											  const std::uint8_t marker)
	{
		auto admitted = begin_mapped_validation_failure_cleanup(std::move(attempt));
		const auto cleanup_effect = identity("test.phase2.cleanup-effect", marker);
		const auto latch_reset = identity("test.phase2.latch-reset", marker);
		const auto terminal =
			sqlite_same_process_shm_lease_test_peer::reader_unpublished_cleanup_terminal(
				admitted.cleanup,
				admitted.map_request.callback,
				sqlite_shm_reader_unpublished_cleanup_evidence_kind::exact_native_result,
				sqlite_ok_status,
				0,
				0,
				cleanup_effect,
				latch_reset);
		auto completed = admitted.candidate.fixture.registry->complete_reader_unpublished_cleanup(
			*admitted.candidate.fixture.family_pin, admitted.cleanup, terminal);
		const auto snapshot = admitted.candidate.fixture.coordinator->snapshot();
		const auto lifecycle = sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(
			*admitted.candidate.fixture.coordinator);
		const auto& reservation = only_attachment_reservation(lifecycle);
		require(completed &&
					completed->kind() ==
						sqlite_shm_reader_unpublished_cleanup_terminal_kind::confirmed &&
					completed->native_status() == sqlite_ok_status &&
					completed->outward_status() == sqlite_ioerr_status &&
					completed->native_effect_receipt() == cleanup_effect &&
					completed->latch_reset_receipt() == latch_reset && !admitted.cleanup.valid() &&
					snapshot.reader_unpublished_cleanup_admitted_count == 0U &&
					snapshot.reader_unpublished_cleanup_confirmed_count == 1U &&
					snapshot.reader_logical_ack_awaiting_count == 1U &&
					snapshot.reader_attachment_live_member_count == 0U &&
					lifecycle.live_custody_kind_counts[enum_index(
						detail::sqlite_shm_reader_custody_kind::generation_group_count)] == 0U &&
					lifecycle.live_custody_kind_counts[enum_index(
						detail::sqlite_shm_reader_custody_kind::attachment_group_handoff)] == 0U &&
					reservation.logical_ack_phase ==
						detail::sqlite_shm_reader_logical_ack_phase::awaiting_sqlite_ack &&
					!snapshot.quarantined,
				"exact cleanup OK must store one logical ack without group decrement");
		return {
			std::move(admitted.candidate),
			std::move(admitted.map_request),
			std::move(admitted.mapped_effect_receipt),
			std::move(admitted.session_no_pointer_terminal_receipt),
			cleanup_effect,
			latch_reset,
		};
	}

	[[nodiscard]] unpublished_cleanup_confirmed
	confirm_mapped_validation_failure_cleanup(const std::uint8_t marker)
	{
		return confirm_mapped_validation_failure_cleanup(
			prepare_mapped_validation_failure_cleanup(marker), marker);
	}

	void close_unpublished_cleanup_open(unpublished_cleanup_confirmed& setup,
										const std::uint8_t marker)
	{
		const auto close_callback = callback(31U, marker);
		auto close = setup.candidate.fixture.registry->begin_reader_close(
			*setup.candidate.fixture.family_pin,
			setup.candidate.open,
			sqlite_shm_reader_close_request{close_callback});
		require(close && close->valid() &&
					close->route() == sqlite_shm_reader_close_route::close_after_confirmed_unmap,
				"begin close after confirmed unpublished cleanup");
		const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
			*close,
			close_callback,
			sqlite_shm_reader_close_evidence_kind::exact_native_result,
			sqlite_ok_status,
			identity("test.phase2.close-effect", marker));
		auto completed = setup.candidate.fixture.registry->complete_reader_close(
			*setup.candidate.fixture.family_pin, setup.candidate.open, *close, receipt);
		require(completed && completed->kind() == sqlite_shm_reader_close_terminal_kind::closed &&
					completed->route() ==
						sqlite_shm_reader_close_route::close_after_confirmed_unmap &&
					!close->valid(),
				"complete close after confirmed unpublished cleanup");
		require(
			setup.candidate.fixture.registry->release_reader_open(setup.candidate.open).has_value(),
			"release closed unpublished-cleanup open");
	}

	void close_unmapped_registry_open(unpublished_cleanup_registry_fixture& fixture,
									  sqlite_shm_reader_open_authority& open,
									  const std::uint8_t marker)
	{
		const auto close_callback = callback(33U, marker);
		auto close = fixture.registry->begin_reader_close(
			*fixture.family_pin, open, sqlite_shm_reader_close_request{close_callback});
		require(close && close->valid() &&
					close->route() == sqlite_shm_reader_close_route::close_without_group,
				"begin unmapped registry reader close");
		const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
			*close,
			close_callback,
			sqlite_shm_reader_close_evidence_kind::exact_native_result,
			sqlite_ok_status,
			identity("test.phase2.unmapped-close-effect", marker));
		auto completed =
			fixture.registry->complete_reader_close(*fixture.family_pin, open, *close, receipt);
		require(completed && completed->kind() == sqlite_shm_reader_close_terminal_kind::closed &&
					completed->route() == sqlite_shm_reader_close_route::close_without_group,
				"complete unmapped registry reader close");
		require(fixture.registry->release_reader_open(open).has_value(),
				"release unmapped registry reader open");
	}

	void retire_unpublished_cleanup_writer(unpublished_cleanup_confirmed& setup,
										   const std::uint8_t marker)
	{
		retire_last(*setup.candidate.fixture.coordinator,
					setup.candidate.writer.holder,
					callback(32U, marker));
		require(setup.candidate.fixture.coordinator
					->revoke_writer_eligibility(setup.candidate.writer.eligibility)
					.has_value(),
				"revoke unpublished-cleanup writer eligibility");
	}

	void verify_extend_pair_classifier()
	{
		require(classify_sqlite_shm_writer_extend_pair(1, 1) ==
					sqlite_shm_writer_extend_pair::one_one,
				"classify one-one");
		require(classify_sqlite_shm_writer_extend_pair(0, 0) ==
					sqlite_shm_writer_extend_pair::zero_zero,
				"classify zero-zero");
		for (const auto [caller, delegated] :
			 {std::pair{1, 0}, std::pair{0, 1}, std::pair{-1, 0}, std::pair{2, 1}})
			require(!classify_sqlite_shm_writer_extend_pair(caller, delegated),
					"reject invalid extend pair");
	}

	void verify_native_attachment_identity_and_census_groundwork()
	{
		{
			constexpr std::uint8_t marker = 90;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto alias_a = identity("test.alias-lifetime", marker);
			const auto alias_b = identity("test.alias-lifetime", marker + 1U);
			const auto open_epoch = identity("test.open-epoch", marker);
			const auto shared_attachment_epoch = identity("test.writer-attachment-epoch", marker);

			auto invalid = sqlite_shm_native_attachment_identity::bind(
				binding,
				alias_a,
				connection,
				identity("test.main-native-file-receipt", marker),
				identity("test.main-xopen-receipt", marker),
				open_epoch,
				identity("test.native-shm-callback-cohort", marker),
				{});
			require(!invalid, "attachment binding rejects an absent nonreusable epoch");

			const auto attachment_a = writer_attachment(
				binding, alias_a, connection, open_epoch, marker, shared_attachment_epoch);
			require(attachment_a.family() == binding && attachment_a.alias_lifetime() == alias_a &&
						attachment_a.connection_token() == connection &&
						attachment_a.open_epoch() == open_epoch &&
						attachment_a.attachment_epoch() == shared_attachment_epoch,
					"valid exact attachment bind retains every checked binding");
			const auto colliding_attachment = writer_attachment(
				binding, alias_b, connection, open_epoch, marker + 1U, shared_attachment_epoch);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};

			auto request_a = writer_request(binding, connection, marker, 1, marker, 0, 1);
			request_a.attachment = attachment_a;
			auto begun_a = coordinator.begin_writer_map(request_a);
			require(begun_a.has_value(), "first checked attachment member is admitted");
			auto member_a = std::move(*begun_a);

			auto collision = writer_request(binding, connection, marker + 1U, 2, marker + 1U, 0, 1);
			collision.attachment = colliding_attachment;
			auto rejected_collision = coordinator.begin_writer_map(collision);
			require(!rejected_collision &&
						rejected_collision.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch,
					"one attachment epoch cannot alias a different attachment binding");
			require(coordinator.snapshot().writer_attachment_identity_count == 1U &&
						coordinator.snapshot().writer_attachment_member_count == 1U,
					"cross-attachment epoch collision never enters the attachment census");

			auto request_b = writer_request(binding, connection, marker + 1U, 2, marker + 2U, 0, 1);
			const auto attachment_b =
				writer_attachment(binding,
								  request_b.alias_lifetime,
								  connection,
								  identity("test.open-epoch", marker + 1U),
								  marker + 1U,
								  identity("test.writer-attachment-epoch", marker + 1U));
			request_b.attachment = attachment_b;
			auto begun_b = coordinator.begin_writer_map(request_b);
			require(begun_b.has_value(), "a distinct valid attachment remains admissible");
			auto member_b = std::move(*begun_b);
			const auto two_live = coordinator.snapshot();
			require(two_live.writer_attachment_identity_count == 2U &&
						two_live.writer_attachment_member_count == 2U &&
						two_live.writer_attachment_unresolved_count == 2U &&
						two_live.writer_attachment_unresolved_member_count == 2U,
					"distinct attachments retain separate central census records");

			require(coordinator.resolve_writer_map_failure(member_a).has_value() &&
						coordinator.resolve_writer_map_failure(member_b).has_value(),
					"native no-map resolves only the two member attempts");
			const auto resolved_no_map = coordinator.snapshot();
			require(resolved_no_map.writer_attachment_identity_count == 2U &&
						resolved_no_map.writer_attachment_member_count == 0U &&
						resolved_no_map.writer_attachment_unresolved_count == 0U &&
						resolved_no_map.writer_attachment_unresolved_member_count == 0U,
					"resolved no-map attempts retain epoch binding without unbounded members");
			auto exact_retry = writer_request(binding, connection, marker, 3, marker + 3U, 0, 1);
			exact_retry.attachment = attachment_a;
			auto retried = coordinator.begin_writer_map(exact_retry);
			require(retried.has_value(),
					"a no-map attempt does not retire the still-open exact attachment epoch");
			require(coordinator.resolve_writer_map_failure(*retried).has_value(),
					"resolve exact attachment retry");
		}

		{
			constexpr std::uint8_t marker = 80;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			const auto alias = identity("test.alias-lifetime", marker);
			const auto attachment =
				writer_attachment(binding, alias, connection, open_epoch, marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			request.attachment = attachment;
			auto begun = coordinator.begin_writer_map(request);
			require(begun.has_value(), "begin attachment open-epoch mismatch fixture");
			auto inflight = std::move(*begun);
			auto post_native = record_native_mapping(coordinator, inflight, &page);
			auto mismatch =
				coordinator.install_pending(post_native,
											writer_receipt(request,
														   identity("test.open-epoch", marker + 1U),
														   mapping(0, &page, 4096U),
														   sqlite_shm_writer_extend_pair::one_one,
														   marker));
			require(!mismatch &&
						mismatch.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch &&
						mismatch.error().action ==
							sqlite_shm_lease_recovery_action::
								attempt_nonremoving_unmap_then_outer_ioerr &&
						post_native.valid(),
					"post-map open epoch must equal the checked attachment binding");
			cleanup_rejected_writer(coordinator, post_native, request.callback);
		}

		{
			constexpr std::uint8_t marker = 81;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			const auto alias = identity("test.alias-lifetime", marker);
			const auto attachment =
				writer_attachment(binding, alias, connection, open_epoch, marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			request.attachment = attachment;
			auto pending = install_pending(coordinator,
										   request,
										   open_epoch,
										   mapping(0, &page, 4096U),
										   sqlite_shm_writer_extend_pair::one_one,
										   marker);
			const auto cleanup_callback = callback(2, marker + 1U);
			auto cleanup_result = coordinator.begin_writer_cleanup(pending, cleanup_callback);
			require(cleanup_result.has_value() && !pending.valid(),
					"single-member attachment admits its one cleanup");
			auto cleanup = std::move(*cleanup_result);

			auto later = writer_request(binding, connection, marker, 3, marker + 2U, 0, 1);
			later.attachment = attachment;
			auto unmap_won = coordinator.begin_writer_map(later);
			require(!unmap_won &&
						unmap_won.error().reason == sqlite_shm_lease_rejection_reason::retiring,
					"cleanup-admitted attachment rejects later same-epoch predelegation");
			require(
				coordinator
					.complete_writer_cleanup(cleanup,
											 cleanup_callback,
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"complete unmap-winning attachment cleanup");
		}

		{
			constexpr std::uint8_t marker = 82;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			const auto alias = identity("test.alias-lifetime", marker);
			const auto attachment =
				writer_attachment(binding, alias, connection, open_epoch, marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto first_request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			first_request.attachment = attachment;
			auto pending = install_pending(coordinator,
										   first_request,
										   open_epoch,
										   mapping(0, &page, 4096U),
										   sqlite_shm_writer_extend_pair::one_one,
										   marker);
			auto second_request = writer_request(binding, connection, marker, 2, marker + 1U, 1, 1);
			second_request.attachment = attachment;
			auto second = coordinator.begin_writer_map(second_request);
			require(second.has_value(), "pending partial-cleanup fixture has a second member");

			auto partial = coordinator.begin_writer_cleanup(pending, callback(3, marker + 2U));
			require(!partial && !pending.valid() && second->valid() &&
						coordinator.snapshot().quarantined &&
						coordinator.snapshot().writer_cleanup_count == 1U,
					"same-attachment inflight cleanup fails closed without an owner");
			require(coordinator.resolve_writer_map_failure(*second).has_value(),
					"resolve the already-started second-page native no-map under quarantine");
			auto retry = coordinator.begin_writer_cleanup(pending, callback(3, marker + 2U));
			require(!retry &&
						retry.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
					"draining the inflight blocker cannot revive the consumed pending source");
			auto blocked = coordinator.begin_writer_map(first_request);
			require(!blocked &&
						blocked.error().reason == sqlite_shm_lease_rejection_reason::quarantined,
					"the inflight cleanup fence blocks all later attachment admission");
		}

		{
			constexpr std::uint8_t marker = 83;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			const auto alias = identity("test.alias-lifetime", marker);
			const auto attachment =
				writer_attachment(binding, alias, connection, open_epoch, marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto first_request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			first_request.attachment = attachment;
			auto begun = coordinator.begin_writer_map(first_request);
			require(begun.has_value(), "post-native partial-cleanup fixture begins first member");
			auto first = std::move(*begun);
			auto post_native = record_native_mapping(coordinator, first, &page);
			auto second_request = writer_request(binding, connection, marker, 2, marker + 1U, 1, 1);
			second_request.attachment = attachment;
			auto second = coordinator.begin_writer_map(second_request);
			require(second.has_value(), "post-native partial-cleanup fixture has second member");

			auto partial = coordinator.begin_writer_cleanup(post_native, callback(3, marker + 2U));
			require(!partial && !post_native.valid() && second->valid() &&
						coordinator.snapshot().quarantined,
					"post-native anchor plus inflight sibling fails closed without an owner");
			require(coordinator.resolve_writer_map_failure(*second).has_value(),
					"resolve fenced post-native fixture sibling without native mapping");
			auto retry = coordinator.begin_writer_cleanup(post_native, callback(3, marker + 2U));
			require(!retry &&
						retry.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
					"draining the blocker cannot revive the consumed post-native source");
		}

		{
			constexpr std::uint8_t marker = 93;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			const auto alias = identity("test.alias-lifetime", marker);
			const auto attachment =
				writer_attachment(binding, alias, connection, open_epoch, marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page_zero{};
			int page_one{};
			auto gate = install_eligibility(coordinator, binding, connection, open_epoch, marker);

			auto request_zero = writer_request(binding, connection, marker, 1, marker, 0, 1);
			request_zero.attachment = attachment;
			auto pending_zero = install_pending(coordinator,
												request_zero,
												open_epoch,
												mapping(0, &page_zero, 4096U),
												sqlite_shm_writer_extend_pair::one_one,
												marker);
			auto holder_zero = promote(coordinator, pending_zero, gate);

			auto request_one = writer_request(binding, connection, marker, 2, marker + 1U, 1, 1);
			request_one.attachment = attachment;
			auto pending_one = install_pending(coordinator,
											   request_one,
											   open_epoch,
											   mapping(1, &page_one, 8192U),
											   sqlite_shm_writer_extend_pair::one_one,
											   marker + 1U);
			auto holder_one = promote(coordinator, pending_one, gate);
			const auto grouped = coordinator.snapshot();
			require(grouped.writer_attachment_identity_count == 1U &&
						grouped.writer_attachment_member_count == 2U &&
						grouped.writer_attachment_unresolved_count == 1U &&
						grouped.writer_attachment_unresolved_member_count == 2U &&
						grouped.writer_holder_count == 2U,
					"same attachment accumulates two members without cross-attachment grouping");

			auto grouped_release =
				coordinator.release_writer_holder(holder_zero, callback(3, marker + 2U));
			require(grouped_release &&
						grouped_release->decision() ==
							sqlite_shm_writer_retirement_decision::ready &&
						!holder_zero.valid() && holder_one.valid() &&
						grouped_release->cleanup().valid() &&
						coordinator.snapshot().writer_cleanup_count == 1U,
					"any one holder anchor seals one cleanup owner for the complete attachment");
			auto duplicate =
				coordinator.release_writer_holder(holder_one, callback(4, marker + 3U));
			require(!duplicate && holder_one.valid() && !coordinator.snapshot().quarantined,
					"a sibling wrapper cannot mint a duplicate attachment cleanup owner");
			require(coordinator
							.complete_writer_cleanup(
								grouped_release->cleanup(),
								callback(3, marker + 2U),
								sqlite_shm_native_cleanup_outcome::confirmed_success)
							.has_value() &&
						!grouped_release->cleanup().valid() && !coordinator.snapshot().quarantined,
					"page zero and page one complete through exactly one native unmap outcome");
			const auto grouped_retired = coordinator.snapshot();
			require(grouped_retired.writer_attachment_audit_member_count == 2U &&
						grouped_retired.writer_attachment_audit_native_mapping_count == 2U &&
						grouped_retired.writer_attachment_audit_post_map_count == 2U &&
						grouped_retired.writer_attachment_audit_promotion_count == 2U,
					"page zero and page one retain exact per-map tombstone evidence");
		}

		{
			constexpr std::uint8_t marker = 96;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			const auto alias = identity("test.alias-lifetime", marker);
			const auto attachment =
				writer_attachment(binding, alias, connection, open_epoch, marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto gate = install_eligibility(coordinator, binding, connection, open_epoch, marker);
			auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			request.attachment = attachment;
			auto pending = install_pending(coordinator,
										   request,
										   open_epoch,
										   mapping(0, &page, 4096U),
										   sqlite_shm_writer_extend_pair::one_one,
										   marker);
			auto holder = promote(coordinator, pending, gate);
			retire_last(coordinator, holder, callback(2, marker + 1U));
			const auto retired = coordinator.snapshot();
			require(retired.writer_attachment_identity_count == 1U &&
						retired.writer_attachment_member_count == 1U &&
						retired.writer_attachment_unresolved_count == 0U &&
						retired.writer_attachment_unresolved_member_count == 0U &&
						retired.writer_attachment_audit_member_count == 1U &&
						retired.writer_attachment_audit_native_mapping_count == 1U &&
						retired.writer_attachment_audit_post_map_count == 1U &&
						retired.writer_attachment_audit_promotion_count == 1U,
					"confirmed cleanup retains a non-reuse attachment tombstone");

			auto reused = writer_request(binding, connection, marker, 3, marker + 2U, 0, 1);
			reused.attachment = attachment;
			auto rejected_reuse = coordinator.begin_writer_map(reused);
			require(!rejected_reuse &&
						rejected_reuse.error().reason ==
							sqlite_shm_lease_rejection_reason::stale_token,
					"confirmed-unmap attachment epoch cannot be reused");
			auto fresh = writer_request(binding, connection, marker, 4, marker + 3U, 0, 1);
			fresh.attachment =
				writer_attachment(binding,
								  alias,
								  connection,
								  open_epoch,
								  marker,
								  identity("test.writer-attachment-epoch", marker + 1U));
			auto fresh_epoch = coordinator.begin_writer_map(fresh);
			require(fresh_epoch.has_value(),
					"same native binding may remap only with a fresh attachment epoch");
			require(coordinator.resolve_writer_map_failure(*fresh_epoch).has_value(),
					"resolve fresh-epoch no-map fixture");
			require(coordinator.revoke_writer_eligibility(gate).has_value(),
					"revoke retired attachment gate");
		}
	}

	void verify_writer_attachment_group_cleanup_is_exact_and_one_shot()
	{
		{
			constexpr std::uint8_t marker = 97;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			const auto alias = identity("test.alias-lifetime", marker);
			const auto attachment =
				writer_attachment(binding, alias, connection, open_epoch, marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto gate = install_eligibility(coordinator, binding, connection, open_epoch, marker);

			auto first_request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			first_request.attachment = attachment;
			auto first_pending = install_pending(coordinator,
												 first_request,
												 open_epoch,
												 mapping(0, &page, 4096U),
												 sqlite_shm_writer_extend_pair::one_one,
												 marker);
			auto first = promote(coordinator, first_pending, gate);

			auto repeat_request = writer_request(binding, connection, marker, 2, marker + 1U, 0, 0);
			repeat_request.attachment = attachment;
			auto repeat_pending = install_pending(coordinator,
												  repeat_request,
												  open_epoch,
												  mapping(0, &page, 4096U),
												  sqlite_shm_writer_extend_pair::zero_zero,
												  marker + 1U);
			auto repeated = promote(coordinator, repeat_pending, gate);

			const auto cleanup_callback = callback(3, marker + 2U);
			auto release = coordinator.release_writer_holder(repeated, cleanup_callback);
			require(release &&
						release->decision() == sqlite_shm_writer_retirement_decision::ready &&
						release->cleanup().valid() && first.valid() && !repeated.valid() &&
						coordinator.snapshot().writer_cleanup_count == 1U &&
						coordinator.snapshot().writer_holder_count == 0U,
					"repeated same-page receipts produce one attachment cleanup owner");
			require(
				coordinator
					.complete_writer_cleanup(release->cleanup(),
											 cleanup_callback,
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"repeated same-page attachment completes one native outcome");
			auto duplicate = coordinator.complete_writer_cleanup(
				release->cleanup(),
				cleanup_callback,
				sqlite_shm_native_cleanup_outcome::confirmed_success);
			require(!duplicate &&
						duplicate.error().reason ==
							sqlite_shm_lease_rejection_reason::stale_token &&
						!coordinator.snapshot().quarantined,
					"completed attachment owner cannot complete twice");
			require(coordinator.revoke_writer_eligibility(gate).has_value(),
					"revoke repeated-page attachment gate");
		}

		{
			constexpr std::uint8_t marker = 98;
			const auto binding = family(marker);
			const auto connection_a = identity("test.connection", marker);
			const auto connection_b = identity("test.connection", marker + 1U);
			const auto open_epoch_a = identity("test.open-epoch", marker);
			const auto open_epoch_b = identity("test.open-epoch", marker + 1U);
			const auto alias_a = identity("test.alias-lifetime", marker);
			const auto alias_b = identity("test.alias-lifetime", marker + 1U);
			const auto attachment_a =
				writer_attachment(binding, alias_a, connection_a, open_epoch_a, marker);
			const auto attachment_b =
				writer_attachment(binding, alias_b, connection_b, open_epoch_b, marker + 1U);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto gate_a =
				install_eligibility(coordinator, binding, connection_a, open_epoch_a, marker);
			auto gate_b =
				install_eligibility(coordinator, binding, connection_b, open_epoch_b, marker + 1U);

			auto install_member = [&](const sqlite_shm_native_attachment_identity& attachment,
									  const sqlite_backend_opaque_identity& connection,
									  const sqlite_backend_opaque_identity& open_epoch,
									  const std::uint8_t alias,
									  const std::uint8_t invocation,
									  const int extend,
									  const std::uint8_t receipt_marker,
									  const sqlite_shm_writer_eligibility& gate)
			{
				auto request =
					writer_request(binding, connection, alias, invocation, invocation, 0, extend);
				request.attachment = attachment;
				auto pending =
					install_pending(coordinator,
									request,
									open_epoch,
									mapping(0, &page, 4096U),
									extend == 1 ? sqlite_shm_writer_extend_pair::one_one
												: sqlite_shm_writer_extend_pair::zero_zero,
									receipt_marker);
				return promote(coordinator, pending, gate);
			};

			auto a0 = install_member(
				attachment_a, connection_a, open_epoch_a, marker, 1, 1, marker, gate_a);
			auto a1 = install_member(
				attachment_a, connection_a, open_epoch_a, marker, 2, 0, marker + 1U, gate_a);
			auto b0 = install_member(
				attachment_b, connection_b, open_epoch_b, marker + 1U, 3, 0, marker + 2U, gate_b);
			auto b1 = install_member(
				attachment_b, connection_b, open_epoch_b, marker + 1U, 4, 0, marker + 3U, gate_b);

			const auto callback_a = callback(5, marker + 4U);
			auto release_a = coordinator.release_writer_holder(a1, callback_a);
			const auto a_sealed = coordinator.snapshot();
			require(release_a &&
						release_a->decision() ==
							sqlite_shm_writer_retirement_decision::not_last_attachment &&
						a_sealed.generation_authority_count == 2U &&
						a_sealed.writer_attachment_audit_member_count == 2U &&
						a_sealed.writer_attachment_audit_native_mapping_count == 2U &&
						a_sealed.writer_attachment_audit_post_map_count == 2U &&
						a_sealed.writer_attachment_audit_promotion_count == 2U &&
						coordinator
							.complete_writer_cleanup(
								release_a->cleanup(),
								callback_a,
								sqlite_shm_native_cleanup_outcome::confirmed_success)
							.has_value() &&
						coordinator.snapshot().writer_holder_count == 2U,
					"first multi-member attachment completes independently in one outcome");

			const auto callback_b = callback(6, marker + 5U);
			auto release_b = coordinator.release_writer_holder(b0, callback_b);
			require(release_b &&
						release_b->decision() == sqlite_shm_writer_retirement_decision::ready &&
						coordinator
							.complete_writer_cleanup(
								release_b->cleanup(),
								callback_b,
								sqlite_shm_native_cleanup_outcome::confirmed_success)
							.has_value() &&
						coordinator.snapshot().phase == sqlite_shm_mapping_generation_phase::empty,
					"second multi-member attachment owns the second and final outcome");
			require(a0.valid() && b1.valid() &&
						coordinator.snapshot().writer_attachment_audit_member_count == 4U &&
						!coordinator.snapshot().quarantined,
					"sibling wrappers survive locally without fabricating extra cleanup");
			require(coordinator.revoke_writer_eligibility(gate_a).has_value() &&
						coordinator.revoke_writer_eligibility(gate_b).has_value(),
					"revoke two-attachment gates");
		}

		{
			constexpr std::uint8_t marker = 107;
			const auto binding = family(marker);
			const auto connection_a = identity("test.connection", marker);
			const auto connection_high = identity("test.connection", marker + 1U);
			const auto connection_b = identity("test.connection", marker + 2U);
			const auto open_epoch_a = identity("test.open-epoch", marker);
			const auto open_epoch_high = identity("test.open-epoch", marker + 1U);
			const auto open_epoch_b = identity("test.open-epoch", marker + 2U);
			const auto alias_a = identity("test.alias-lifetime", marker);
			const auto alias_high = identity("test.alias-lifetime", marker + 1U);
			const auto alias_b = identity("test.alias-lifetime", marker + 2U);
			const auto attachment_a =
				writer_attachment(binding, alias_a, connection_a, open_epoch_a, marker);
			const auto attachment_high = writer_attachment(
				binding, alias_high, connection_high, open_epoch_high, marker + 1U);
			const auto attachment_b =
				writer_attachment(binding, alias_b, connection_b, open_epoch_b, marker + 2U);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page_zero{};
			int page_one{};
			auto gate_a =
				install_eligibility(coordinator, binding, connection_a, open_epoch_a, marker);
			auto gate_high = install_eligibility(
				coordinator, binding, connection_high, open_epoch_high, marker + 1U);
			auto gate_b =
				install_eligibility(coordinator, binding, connection_b, open_epoch_b, marker + 2U);

			auto request_a = writer_request(binding, connection_a, marker, 1, marker, 0, 1);
			request_a.attachment = attachment_a;
			auto pending_a = install_pending(coordinator,
											 request_a,
											 open_epoch_a,
											 mapping(0, &page_zero, 4096U),
											 sqlite_shm_writer_extend_pair::one_one,
											 marker);
			auto holder_a = promote(coordinator, pending_a, gate_a);

			auto high_request =
				writer_request(binding, connection_high, marker + 1U, 2, marker + 1U, 1, 1);
			high_request.attachment = attachment_high;
			auto high_pending = install_pending(coordinator,
												high_request,
												open_epoch_high,
												mapping(1, &page_one, 8192U),
												sqlite_shm_writer_extend_pair::one_one,
												marker + 1U);
			auto high_holder = promote(coordinator, high_pending, gate_high);

			auto request_b =
				writer_request(binding, connection_b, marker + 2U, 3, marker + 2U, 0, 0);
			request_b.attachment = attachment_b;
			auto pending_b = install_pending(coordinator,
											 request_b,
											 open_epoch_b,
											 mapping(0, &page_zero, 8192U),
											 sqlite_shm_writer_extend_pair::zero_zero,
											 marker + 2U);
			auto holder_b = promote(coordinator, pending_b, gate_b);

			const auto cleanup_a = callback(4, marker + 3U);
			auto release_a = coordinator.release_writer_holder(holder_a, cleanup_a);
			require(release_a &&
						release_a->decision() ==
							sqlite_shm_writer_retirement_decision::not_last_attachment &&
						coordinator.snapshot().sealed_shm_size == 8192U &&
						coordinator.snapshot().generation_authority_count == 2U,
					"redundant page identity ignores attachment-local sealed high-water");
			require(
				coordinator
					.complete_writer_cleanup(release_a->cleanup(),
											 cleanup_a,
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"page-zero attachment retires while exact page-zero support remains");

			auto reader = coordinator.begin_reader_map(
				reader_request(binding, connection_b, marker + 3U, 5, marker + 4U, 0));
			require(reader && coordinator.snapshot().reader_admission_visible,
					"remaining high-water support admits a fresh page-zero reader");
			require(coordinator.resolve_reader_map_failure(*reader).has_value(),
					"resolve fresh high-water reader as native no-map");

			auto retired_a_cannot_support_b =
				coordinator.release_writer_holder(holder_b, callback(6, marker + 5U));
			require(!retired_a_cannot_support_b && !holder_b.valid() && high_holder.valid() &&
						coordinator.snapshot().quarantined,
					"retired inactive page-zero evidence cannot support later nonlast cleanup");
			require(coordinator.revoke_writer_eligibility(gate_a).has_value() &&
						coordinator.revoke_writer_eligibility(gate_high).has_value() &&
						coordinator.revoke_writer_eligibility(gate_b).has_value(),
					"revoke high-water support gates");
		}

		{
			constexpr std::uint8_t marker = 101;
			const auto binding = family(marker);
			const auto connection_a = identity("test.connection", marker);
			const auto connection_b = identity("test.connection", marker + 1U);
			const auto open_epoch_a = identity("test.open-epoch", marker);
			const auto open_epoch_b = identity("test.open-epoch", marker + 1U);
			const auto alias_a = identity("test.alias-lifetime", marker);
			const auto alias_b = identity("test.alias-lifetime", marker + 1U);
			const auto attachment_a =
				writer_attachment(binding, alias_a, connection_a, open_epoch_a, marker);
			const auto attachment_b =
				writer_attachment(binding, alias_b, connection_b, open_epoch_b, marker + 1U);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page_zero{};
			int page_one{};
			auto gate_a =
				install_eligibility(coordinator, binding, connection_a, open_epoch_a, marker);
			auto gate_b =
				install_eligibility(coordinator, binding, connection_b, open_epoch_b, marker + 1U);

			auto request_a = writer_request(binding, connection_a, marker, 1, marker, 0, 1);
			request_a.attachment = attachment_a;
			auto pending_a = install_pending(coordinator,
											 request_a,
											 open_epoch_a,
											 mapping(0, &page_zero, 4096U),
											 sqlite_shm_writer_extend_pair::one_one,
											 marker);
			auto holder_a = promote(coordinator, pending_a, gate_a);

			auto request_b =
				writer_request(binding, connection_b, marker + 1U, 2, marker + 1U, 1, 1);
			request_b.attachment = attachment_b;
			auto pending_b = install_pending(coordinator,
											 request_b,
											 open_epoch_b,
											 mapping(1, &page_one, 8192U),
											 sqlite_shm_writer_extend_pair::one_one,
											 marker + 1U);
			auto holder_b = promote(coordinator, pending_b, gate_b);

			auto fenced = coordinator.release_writer_holder(holder_a, callback(3, marker + 2U));
			require(!fenced && !holder_a.valid() && holder_b.valid() &&
						coordinator.snapshot().quarantined &&
						coordinator.snapshot().generation_authority_count == 2U &&
						coordinator.snapshot().writer_attachment_audit_member_count == 0U,
					"nonlast cleanup cannot retire sole support for a different generation page");
			auto blocked = coordinator.begin_writer_map(request_a);
			require(!blocked &&
						blocked.error().reason == sqlite_shm_lease_rejection_reason::quarantined,
					"sole-page support fence blocks later admission without an owner");
			auto fresh_request =
				writer_request(binding, connection_a, marker, 4, marker + 3U, 0, 1);
			fresh_request.attachment =
				writer_attachment(binding,
								  alias_a,
								  connection_a,
								  open_epoch_a,
								  marker,
								  identity("test.writer-attachment-epoch", marker + 3U));
			auto fresh = coordinator.begin_writer_map(fresh_request);
			require(!fresh &&
						fresh.error().reason == sqlite_shm_lease_rejection_reason::quarantined,
					"sole-page support fence also blocks a fresh attachment epoch");
			require(coordinator.revoke_writer_eligibility(gate_a).has_value() &&
						coordinator.revoke_writer_eligibility(gate_b).has_value(),
					"revoke sole-page fence gates");
		}

		{
			constexpr std::uint8_t marker = 99;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			const auto alias = identity("test.alias-lifetime", marker);
			const auto attachment =
				writer_attachment(binding, alias, connection, open_epoch, marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page_zero{};
			int page_one{};
			auto gate = install_eligibility(coordinator, binding, connection, open_epoch, marker);

			auto first_request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			first_request.attachment = attachment;
			auto first_pending = install_pending(coordinator,
												 first_request,
												 open_epoch,
												 mapping(0, &page_zero, 4096U),
												 sqlite_shm_writer_extend_pair::one_one,
												 marker);
			auto first = promote(coordinator, first_pending, gate);

			auto second_request = writer_request(binding, connection, marker, 2, marker + 1U, 1, 1);
			second_request.attachment = attachment;
			auto second_begun = coordinator.begin_writer_map(second_request);
			require(second_begun.has_value(), "begin second-page validation-failure member");
			auto second_inflight = std::move(*second_begun);
			auto second_post = record_native_mapping(coordinator, second_inflight, &page_one);
			auto invalid =
				coordinator.install_pending(second_post,
											writer_receipt(second_request,
														   open_epoch,
														   mapping(2, &page_one, 12288U),
														   sqlite_shm_writer_extend_pair::one_one,
														   marker + 1U));
			require(!invalid && second_post.valid(),
					"second-page post-native validation failure retains its exact anchor");

			const auto cleanup_callback = second_request.callback;
			auto cleanup = coordinator.begin_writer_cleanup(second_post, cleanup_callback);
			require(!cleanup && !second_post.valid() && first.valid() &&
						coordinator.snapshot().writer_holder_count == 1U &&
						coordinator.snapshot().writer_cleanup_count == 1U &&
						coordinator.snapshot().quarantined,
					"post-native failure mixed with a live member is fenced without an owner");
			auto same_epoch = coordinator.begin_writer_map(first_request);
			require(!same_epoch &&
						same_epoch.error().reason == sqlite_shm_lease_rejection_reason::quarantined,
					"post-native/live fence blocks same-epoch admission");

			auto fresh_request = writer_request(binding, connection, marker, 4, marker + 3U, 0, 1);
			fresh_request.attachment =
				writer_attachment(binding,
								  alias,
								  connection,
								  open_epoch,
								  marker,
								  identity("test.writer-attachment-epoch", marker + 1U));
			auto fresh = coordinator.begin_writer_map(fresh_request);
			require(!fresh &&
						fresh.error().reason == sqlite_shm_lease_rejection_reason::quarantined,
					"post-native/live fence also blocks a fresh attachment epoch");
			require(coordinator.revoke_writer_eligibility(gate).has_value(),
					"revoke post-native grouped-cleanup gate");
		}

		{
			constexpr std::uint8_t marker = 100;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			const auto alias = identity("test.alias-lifetime", marker);
			const auto attachment =
				writer_attachment(binding, alias, connection, open_epoch, marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto gate = install_eligibility(coordinator, binding, connection, open_epoch, marker);

			auto first_request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			first_request.attachment = attachment;
			auto pending = install_pending(coordinator,
										   first_request,
										   open_epoch,
										   mapping(0, &page, 4096U),
										   sqlite_shm_writer_extend_pair::one_one,
										   marker);
			auto holder = promote(coordinator, pending, gate);

			auto sibling_request =
				writer_request(binding, connection, marker, 2, marker + 1U, 1, 1);
			sibling_request.attachment = attachment;
			auto sibling = coordinator.begin_writer_map(sibling_request);
			require(sibling.has_value(), "begin same-attachment inflight release blocker");
			const auto cleanup_callback = callback(3, marker + 2U);
			auto fenced = coordinator.release_writer_holder(holder, cleanup_callback);
			require(!fenced && !holder.valid() && sibling->valid() &&
						coordinator.snapshot().quarantined,
					"same-attachment inflight release fails closed without a cleanup owner");
			require(coordinator.resolve_writer_map_failure(*sibling).has_value(),
					"resolve already-started same-attachment inflight under quarantine");
			auto blocked = coordinator.begin_writer_map(first_request);
			require(!blocked &&
						blocked.error().reason == sqlite_shm_lease_rejection_reason::quarantined,
					"same-attachment inflight fence remains terminal after resolution");
			require(coordinator.revoke_writer_eligibility(gate).has_value(),
					"revoke retained-holder attachment gate");
		}
	}

	void verify_writer_attachment_gate_boundary_is_exact()
	{
		{
			constexpr std::uint8_t marker = 102;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			const auto alias = identity("test.alias-lifetime", marker);
			const auto attachment =
				writer_attachment(binding, alias, connection, open_epoch, marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page_zero{};
			int page_one{};
			auto gate = install_eligibility(coordinator, binding, connection, open_epoch, marker);

			auto request_zero = writer_request(binding, connection, marker, 1, marker, 0, 1);
			request_zero.attachment = attachment;
			auto pending_zero = install_pending(coordinator,
												request_zero,
												open_epoch,
												mapping(0, &page_zero, 4096U),
												sqlite_shm_writer_extend_pair::one_one,
												marker);
			auto request_one = writer_request(binding, connection, marker, 2, marker + 1U, 1, 1);
			request_one.attachment = attachment;
			auto pending_one = install_pending(coordinator,
											   request_one,
											   open_epoch,
											   mapping(1, &page_one, 8192U),
											   sqlite_shm_writer_extend_pair::one_one,
											   marker + 1U);

			auto partial_zero = coordinator.promote_writer(pending_zero, gate);
			auto partial_one = coordinator.promote_writer(pending_one, gate);
			require(
				!partial_zero && !partial_one && pending_zero.valid() && pending_one.valid() &&
					partial_zero.error().reason ==
						sqlite_shm_lease_rejection_reason::pending_or_eligibility_only &&
					partial_zero.error().action ==
						sqlite_shm_lease_recovery_action::await_complete_attachment_gate_boundary &&
					partial_one.error().reason ==
						sqlite_shm_lease_rejection_reason::pending_or_eligibility_only &&
					partial_one.error().action ==
						sqlite_shm_lease_recovery_action::await_complete_attachment_gate_boundary &&
					coordinator.snapshot().writer_holder_count == 0U,
				"multiple pre-gate members cannot create partial holder authority");

			const auto cleanup_callback = callback(3, marker + 2U);
			auto cleanup = coordinator.begin_writer_cleanup(pending_zero, cleanup_callback);
			require(cleanup && !pending_zero.valid() && pending_one.valid() &&
						coordinator.snapshot().writer_attachment_audit_promotion_count == 0U,
					"ungated complete attachment retains no promotion evidence");
			require(
				coordinator
					.complete_writer_cleanup(*cleanup,
											 cleanup_callback,
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"ungated multi-pending attachment cleans through one complete owner");
			require(coordinator.revoke_writer_eligibility(gate).has_value(),
					"revoke multi-pending gate fence");
		}

		{
			constexpr std::uint8_t marker = 103;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			const auto alias = identity("test.alias-lifetime", marker);
			const auto attachment =
				writer_attachment(binding, alias, connection, open_epoch, marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto first_gate =
				install_eligibility(coordinator, binding, connection, open_epoch, marker);
			auto exact_gate_copy =
				install_eligibility(coordinator, binding, connection, open_epoch, marker);

			auto first_request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			first_request.attachment = attachment;
			auto first_pending = install_pending(coordinator,
												 first_request,
												 open_epoch,
												 mapping(0, &page, 4096U),
												 sqlite_shm_writer_extend_pair::one_one,
												 marker);
			auto first = promote(coordinator, first_pending, first_gate);

			auto second_request = writer_request(binding, connection, marker, 2, marker + 1U, 0, 0);
			second_request.attachment = attachment;
			auto second_pending = install_pending(coordinator,
												  second_request,
												  open_epoch,
												  mapping(0, &page, 4096U),
												  sqlite_shm_writer_extend_pair::zero_zero,
												  marker + 1U);
			auto second = coordinator.promote_writer(second_pending, exact_gate_copy);
			require(second && !second_pending.valid() &&
						coordinator.snapshot().writer_holder_count == 2U,
					"field-exact first gate receipt may be reused by a later member");
			auto second_holder = std::move(*second);

			auto mismatched_gate =
				install_eligibility(coordinator, binding, connection, open_epoch, marker + 1U);
			auto third_request = writer_request(binding, connection, marker, 3, marker + 2U, 0, 0);
			third_request.attachment = attachment;
			auto third_pending = install_pending(coordinator,
												 third_request,
												 open_epoch,
												 mapping(0, &page, 4096U),
												 sqlite_shm_writer_extend_pair::zero_zero,
												 marker + 2U);
			auto mismatched = coordinator.promote_writer(third_pending, mismatched_gate);
			require(!mismatched && third_pending.valid() &&
						mismatched.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch &&
						coordinator.snapshot().writer_holder_count == 2U &&
						!coordinator.snapshot().quarantined,
					"a different later gate receipt cannot mutate attachment authority");
			require(first.valid() && second_holder.valid(),
					"gate mismatch preserves only the two exact-gate holders");
			require(coordinator.revoke_writer_eligibility(first_gate).has_value() &&
						coordinator.revoke_writer_eligibility(exact_gate_copy).has_value() &&
						coordinator.revoke_writer_eligibility(mismatched_gate).has_value(),
					"revoke exact and mismatched attachment gates");
		}

		{
			constexpr std::uint8_t marker = 105;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			const auto alias = identity("test.alias-lifetime", marker);
			const auto attachment =
				writer_attachment(binding, alias, connection, open_epoch, marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto gate = install_eligibility(coordinator, binding, connection, open_epoch, marker);

			auto complete_request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			complete_request.attachment = attachment;
			auto complete_pending = install_pending(coordinator,
													complete_request,
													open_epoch,
													mapping(0, &page, 4096U),
													sqlite_shm_writer_extend_pair::one_one,
													marker);
			auto inflight_request =
				writer_request(binding, connection, marker, 2, marker + 1U, 1, 1);
			inflight_request.attachment = attachment;
			auto inflight = coordinator.begin_writer_map(inflight_request);
			require(inflight.has_value(), "begin visible inflight gate-boundary sibling");

			auto awaiting = coordinator.promote_writer(complete_pending, gate);
			require(
				!awaiting && complete_pending.valid() && inflight->valid() &&
					awaiting.error().action ==
						sqlite_shm_lease_recovery_action::await_complete_attachment_gate_boundary &&
					coordinator.snapshot().writer_holder_count == 0U &&
					!coordinator.snapshot().quarantined,
				"visible inflight sibling returns the dedicated nonterminal gate action");
			require(coordinator.resolve_writer_map_failure(*inflight).has_value(),
					"resolve visible gate sibling as native no-map");
			auto promoted = coordinator.promote_writer(complete_pending, gate);
			require(promoted && !complete_pending.valid() &&
						coordinator.snapshot().writer_holder_count == 1U,
					"remaining complete member may promote after exact sibling resolution");
			auto holder = std::move(*promoted);
			retire_last(coordinator, holder, callback(3, marker + 2U));
			require(coordinator.revoke_writer_eligibility(gate).has_value(),
					"revoke resolved gate-boundary fixture");
		}
	}

	void verify_writer_attachment_pre_owner_failure_is_terminal()
	{
		constexpr std::uint8_t marker = 106;
		const auto binding = family(marker);
		const auto connection = identity("test.connection", marker);
		const auto open_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		const auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
		auto pending = install_pending(coordinator,
									   request,
									   open_epoch,
									   mapping(0, &page, 4096U),
									   sqlite_shm_writer_extend_pair::one_one,
									   marker);
		const auto cleanup_callback = callback(2, marker + 1U);
		sqlite_same_process_shm_lease_test_peer::fail_next_writer_attachment_seal_transition(
			coordinator);
		auto failed = coordinator.begin_writer_cleanup(pending, cleanup_callback);
		require(!failed && !pending.valid() && coordinator.snapshot().quarantined &&
					coordinator.snapshot().writer_cleanup_count == 1U &&
					coordinator.snapshot().writer_attachment_audit_member_count == 0U,
				"pre-owner seal failure consumes the exact source into a terminal fence");
		auto retry = coordinator.begin_writer_cleanup(pending, cleanup_callback);
		require(!retry && retry.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
				"pre-owner seal failure cannot mint a cleanup owner on retry");
		auto later = coordinator.begin_writer_map(request);
		require(!later && later.error().reason == sqlite_shm_lease_rejection_reason::quarantined,
				"pre-owner failure blocks later mapping without a native cleanup outcome");
	}

	void verify_nonowner_attachment_wait_is_terminal_without_retry()
	{
		const auto exercise = [](const std::uint8_t marker, const bool post_native_anchor)
		{
			const auto binding = family(marker);
			const auto connection_a = identity("test.connection", marker);
			const auto connection_b = identity("test.connection", marker + 1U);
			const auto connection_target = identity("test.connection", marker + 2U);
			const auto open_epoch_a = identity("test.open-epoch", marker);
			const auto open_epoch_b = identity("test.open-epoch", marker + 1U);
			const auto open_epoch_target = identity("test.open-epoch", marker + 2U);
			const auto alias_a = identity("test.alias-lifetime", marker);
			const auto alias_b = identity("test.alias-lifetime", marker + 1U);
			const auto alias_target = identity("test.alias-lifetime", marker + 2U);
			const auto attachment_a =
				writer_attachment(binding, alias_a, connection_a, open_epoch_a, marker);
			const auto attachment_b =
				writer_attachment(binding, alias_b, connection_b, open_epoch_b, marker + 1U);
			const auto attachment_target = writer_attachment(
				binding, alias_target, connection_target, open_epoch_target, marker + 2U);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto gate_a =
				install_eligibility(coordinator, binding, connection_a, open_epoch_a, marker);
			auto gate_b =
				install_eligibility(coordinator, binding, connection_b, open_epoch_b, marker + 1U);

			auto request_a = writer_request(binding, connection_a, marker, 1, marker, 0, 1);
			request_a.attachment = attachment_a;
			auto pending_a = install_pending(coordinator,
											 request_a,
											 open_epoch_a,
											 mapping(0, &page, 4096U),
											 sqlite_shm_writer_extend_pair::one_one,
											 marker);
			auto holder_a = promote(coordinator, pending_a, gate_a);

			auto request_b =
				writer_request(binding, connection_b, marker + 1U, 2, marker + 1U, 0, 0);
			request_b.attachment = attachment_b;
			auto pending_b = install_pending(coordinator,
											 request_b,
											 open_epoch_b,
											 mapping(0, &page, 4096U),
											 sqlite_shm_writer_extend_pair::zero_zero,
											 marker + 1U);
			auto holder_b = promote(coordinator, pending_b, gate_b);

			auto target_request =
				writer_request(binding, connection_target, marker + 2U, 3, marker + 2U, 0, 0);
			target_request.attachment = attachment_target;
			auto target_inflight = coordinator.begin_writer_map(target_request);
			require(target_inflight.has_value(), "begin non-owner wait target attachment");

			const auto cleanup_a = callback(4, marker + 3U);
			const auto consume_target = [&](auto& source)
			{
				auto release_a = coordinator.release_writer_holder(holder_a, cleanup_a);
				require(release_a &&
							release_a->decision() ==
								sqlite_shm_writer_retirement_decision::not_last_attachment &&
							release_a->cleanup().valid(),
						"seal a nonlast cleanup as the external wait blocker");
				auto terminal_b = coordinator.release_writer_holder(
					holder_b, sqlite_shm_callback_execution_receipt{});
				require(!terminal_b && !holder_b.valid() && coordinator.snapshot().quarantined,
						"remove the redundant live group while preserving the sealed blocker");
				// A is a nonlast sealed cleanup (not a live attachment group), B is terminal,
				// and this target is excluded from its own census. A's callback is therefore
				// the sole retirement blocker and yields wait_for_inflight.
				auto failed = coordinator.begin_writer_cleanup(source, target_request.callback);
				require(!failed && !source.valid() && release_a->cleanup().valid(),
						"non-owner wait consumes its exact source without minting an owner");
				auto drained = coordinator.complete_writer_cleanup(
					release_a->cleanup(),
					cleanup_a,
					sqlite_shm_native_cleanup_outcome::confirmed_success);
				require(!drained && !release_a->cleanup().valid(),
						"terminal family drain consumes the external cleanup blocker");
				auto retry = coordinator.begin_writer_cleanup(source, target_request.callback);
				require(!retry &&
							retry.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
						"draining an external blocker cannot revive a consumed non-owner source");
			};

			if (post_native_anchor)
			{
				auto source = record_native_mapping(coordinator, *target_inflight, &page);
				consume_target(source);
			}
			else
			{
				auto post_native = record_native_mapping(coordinator, *target_inflight, &page);
				auto installed = coordinator.install_pending(
					post_native,
					writer_receipt(target_request,
								   open_epoch_target,
								   mapping(0, &page, 4096U),
								   sqlite_shm_writer_extend_pair::zero_zero,
								   marker + 2U));
				require(installed.has_value() && !post_native.valid(),
						"install pending non-owner wait target");
				auto source = std::move(*installed);
				consume_target(source);
			}
			require(coordinator.revoke_writer_eligibility(gate_a).has_value() &&
						coordinator.revoke_writer_eligibility(gate_b).has_value(),
					"revoke non-owner wait fixture gates");
		};

		exercise(108, false);
		exercise(109, true);
	}

	void verify_writer_attachment_completion_failure_is_one_shot()
	{
		constexpr std::uint8_t marker = 104;
		const auto binding = family(marker);
		const auto connection = identity("test.connection", marker);
		const auto open_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto live =
			install_live_writer(coordinator, binding, connection, open_epoch, marker, &page);
		const auto cleanup_callback = callback(2, marker + 1U);
		auto release = coordinator.release_writer_holder(live.holder, cleanup_callback);
		require(release && release->decision() == sqlite_shm_writer_retirement_decision::ready &&
					release->cleanup().valid() &&
					coordinator.snapshot().writer_attachment_audit_member_count == 1U &&
					coordinator.snapshot().generation_authority_count == 0U,
				"completion-failure fixture seals audit and retires live authority");

		sqlite_same_process_shm_lease_test_peer::fail_next_writer_completion_transition(
			coordinator);
		auto failed = coordinator.complete_writer_cleanup(
			release->cleanup(),
			cleanup_callback,
			sqlite_shm_native_cleanup_outcome::confirmed_success);
		require(!failed && !release->cleanup().valid() && coordinator.snapshot().quarantined &&
					coordinator.snapshot().writer_attachment_audit_member_count == 1U,
				"post-outcome internal failure consumes the sole owner into a terminal tombstone");
		auto replay = coordinator.complete_writer_cleanup(
			release->cleanup(),
			cleanup_callback,
			sqlite_shm_native_cleanup_outcome::confirmed_success);
		require(!replay && replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
				"post-outcome internal failure cannot replay native cleanup");
		require(coordinator.revoke_writer_eligibility(live.eligibility).has_value(),
				"revoke completion-failure gate");
	}

	void verify_post_native_writer_receipt_requires_exact_cleanup()
	{
		const auto binding = family(30);
		const auto connection = identity("test.connection", 30);
		const auto open_epoch = identity("test.open-epoch", 30);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int first_page{};
		int second_page{};

		const auto no_mapping_request = writer_request(binding, connection, 30, 1, 29, 0, 1);
		auto no_mapping = coordinator.begin_writer_map(no_mapping_request);
		require(no_mapping.has_value(), "begin writer before native no-mapping result");
		require(coordinator.resolve_writer_map_failure(*no_mapping).has_value() &&
					!no_mapping->valid() && coordinator.snapshot().writer_inflight_count == 0U,
				"pre-native token resolves only when no native mapping exists");

		auto request = writer_request(binding, connection, 30, 1, 30, 0, 1);
		request.attachment = no_mapping_request.attachment;
		auto begun = coordinator.begin_writer_map(request);
		require(begun.has_value(), "begin writer before cleanup-only native receipt");
		auto inflight = std::move(*begun);
		auto post_native = record_native_mapping(coordinator, inflight, &first_page);
		require(coordinator.snapshot().writer_inflight_count == 1U &&
					!coordinator.snapshot().reader_admission_visible,
				"cleanup-only native receipt grants no mapping or reader authority");
		auto unsafe_resolution = coordinator.resolve_writer_map_failure(inflight);
		require(!unsafe_resolution &&
					unsafe_resolution.error().reason ==
						sqlite_shm_lease_rejection_reason::stale_token,
				"post-native mapping cannot use the no-mapping failure transition");
		cleanup_rejected_writer(coordinator, post_native, request.callback);

		const auto mismatch_request = writer_request(binding, connection, 30, 1, 31, 0, 1);
		auto mismatch_begun = coordinator.begin_writer_map(mismatch_request);
		require(mismatch_begun.has_value(), "begin writer before malformed post receipt");
		auto mismatch_inflight = std::move(*mismatch_begun);
		auto mismatch_post_native =
			record_native_mapping(coordinator, mismatch_inflight, &first_page);
		auto mismatched_page =
			coordinator.install_pending(mismatch_post_native,
										writer_receipt(mismatch_request,
													   open_epoch,
													   mapping(1, &second_page, 8192U),
													   sqlite_shm_writer_extend_pair::one_one,
													   31));
		require(!mismatched_page &&
					mismatched_page.error().reason ==
						sqlite_shm_lease_rejection_reason::receipt_mismatch &&
					mismatch_post_native.valid() &&
					coordinator.snapshot().writer_holder_count == 0U &&
					!coordinator.snapshot().reader_admission_visible,
				"post receipt page and range must match the predelegate request");
		cleanup_rejected_writer(coordinator, mismatch_post_native, mismatch_request.callback);

		const auto pointer_request = writer_request(binding, connection, 30, 1, 32, 0, 1);
		auto pointer_begun = coordinator.begin_writer_map(pointer_request);
		require(pointer_begun.has_value(), "begin writer before native pointer mismatch");
		auto pointer_inflight = std::move(*pointer_begun);
		auto pointer_post_native =
			record_native_mapping(coordinator, pointer_inflight, &first_page);
		auto pointer_mismatch =
			coordinator.install_pending(pointer_post_native,
										writer_receipt(pointer_request,
													   open_epoch,
													   mapping(0, &second_page, 4096U),
													   sqlite_shm_writer_extend_pair::one_one,
													   32));
		require(!pointer_mismatch && pointer_post_native.valid() &&
					pointer_mismatch.error().reason ==
						sqlite_shm_lease_rejection_reason::receipt_mismatch,
				"full post-map receipt must retain the cleanup-only native pointer");
		cleanup_rejected_writer(coordinator, pointer_post_native, pointer_request.callback);

		const auto pair_request = writer_request(binding, connection, 30, 1, 33, 0, 1);
		auto pair_begun = coordinator.begin_writer_map(pair_request);
		require(pair_begun.has_value(), "begin writer before pair mismatch");
		auto pair_inflight = std::move(*pair_begun);
		auto pair_post_native = record_native_mapping(coordinator, pair_inflight, &first_page);
		auto pair_mismatch =
			coordinator.install_pending(pair_post_native,
										writer_receipt(pair_request,
													   open_epoch,
													   mapping(0, &first_page, 4096U),
													   sqlite_shm_writer_extend_pair::zero_zero,
													   33));
		require(!pair_mismatch &&
					pair_mismatch.error().reason ==
						sqlite_shm_lease_rejection_reason::receipt_mismatch &&
					pair_post_native.valid(),
				"sealed extend pair must match caller intent");
		auto begun_cleanup =
			coordinator.begin_writer_cleanup(pair_post_native, pair_request.callback);
		require(begun_cleanup.has_value() && !pair_post_native.valid(),
				"unknown writer cleanup becomes an obligation");
		auto cleanup = std::move(*begun_cleanup);
		auto unknown_cleanup = coordinator.complete_writer_cleanup(
			cleanup, pair_request.callback, sqlite_shm_native_cleanup_outcome::unknown);
		require(!unknown_cleanup && !cleanup.valid() && coordinator.snapshot().quarantined,
				"unknown post-native cleanup permanently quarantines the family");
	}

	void verify_writer_native_map_receipt_validator_is_closed()
	{
		{
			constexpr std::uint8_t marker = 73;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			const auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			auto begun = coordinator.begin_writer_map(request);
			require(begun.has_value(), "begin exact native-map validator fixture");
			auto inflight = std::move(*begun);
			auto validated = sqlite_writer_shm_native_map_receipt_validator::validate(
				inflight, sqlite_ok_status, &page);
			require(validated.has_value() && inflight.valid() &&
						validated->native_mapping() == &page,
					"exact SQLITE_OK plus non-null mints only a bound cleanup receipt");
			const auto sealed = coordinator.snapshot();
			require(sealed.writer_inflight_count == 1U && sealed.generation_authority_count == 0U &&
						sealed.writer_cleanup_count == 0U && sealed.writer_holder_count == 0U &&
						sealed.reader_inflight_count == 0U && sealed.reader_cleanup_count == 0U &&
						sealed.reader_handoff_count == 0U && !sealed.reader_admission_visible,
					"native-map receipt grants no generation holder or reader authority");
			auto recorded = coordinator.record_writer_native_mapping(inflight, *validated);
			require(recorded.has_value() && !inflight.valid(),
					"validator receipt leaves coordinator as the sole inflight consumer");
			auto cleanup = coordinator.begin_writer_cleanup(*recorded, request.callback);
			require(cleanup.has_value() && !recorded->valid(),
					"validated native mapping retains one cleanup owner");
			auto completed = coordinator.complete_writer_cleanup(
				*cleanup, request.callback, sqlite_shm_native_cleanup_outcome::confirmed_success);
			require(completed.has_value() && !cleanup->valid() &&
						!coordinator.snapshot().quarantined,
					"validated cleanup-only receipt grants no generation authority");
		}

		{
			constexpr std::uint8_t marker = 74;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			const auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			auto begun = coordinator.begin_writer_map(request);
			require(begun.has_value(), "begin SQLITE_OK-null validator fixture");
			auto inflight = std::move(*begun);
			auto rejected = sqlite_writer_shm_native_map_receipt_validator::validate(
				inflight, sqlite_ok_status, nullptr);
			require(!rejected && inflight.valid() &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch &&
						rejected.error().action ==
							sqlite_shm_lease_recovery_action::outer_ioerr_no_retry,
					"SQLITE_OK plus null cannot mint and retains no-map resolution");
			auto resolved = coordinator.resolve_writer_map_failure(inflight);
			require(resolved.has_value() && !inflight.valid(),
					"SQLITE_OK-null resolves only the unobserved inflight");
			int page{};
			auto stale = sqlite_writer_shm_native_map_receipt_validator::validate(
				inflight, sqlite_ok_status, &page);
			require(
				!stale && stale.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
					stale.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry,
				"stale inflight plus unbound mapping requires fail-closed quarantine");
			auto stale_no_map = sqlite_writer_shm_native_map_receipt_validator::validate(
				inflight, sqlite_readonly_status, nullptr);
			require(!stale_no_map &&
						stale_no_map.error().reason ==
							sqlite_shm_lease_rejection_reason::stale_token &&
						stale_no_map.error().action ==
							sqlite_shm_lease_recovery_action::outer_ioerr_no_retry,
					"stale inflight plus null has no native mapping cleanup to bind");
		}

		{
			constexpr std::uint8_t marker = 75;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			const auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			auto begun = coordinator.begin_writer_map(request);
			require(begun.has_value(), "begin native non-OK-null validator fixture");
			auto inflight = std::move(*begun);
			auto rejected = sqlite_writer_shm_native_map_receipt_validator::validate(
				inflight, sqlite_readonly_status, nullptr);
			require(!rejected && inflight.valid() &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch &&
						rejected.error().action ==
							sqlite_shm_lease_recovery_action::outer_ioerr_no_retry,
					"native non-OK plus null cannot mint and retains no-map resolution");
			auto resolved = coordinator.resolve_writer_map_failure(inflight);
			require(resolved.has_value() && !inflight.valid(),
					"native non-OK-null resolves only its inflight attempt");
		}

		{
			constexpr std::uint8_t marker = 76;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			{
				const auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
				auto begun = coordinator.begin_writer_map(request);
				require(begun.has_value(), "begin native non-OK-nonnull validator fixture");
				auto inflight = std::move(*begun);
				auto rejected = sqlite_writer_shm_native_map_receipt_validator::validate(
					inflight, sqlite_readonly_status, &page);
				require(!rejected && inflight.valid() &&
							rejected.error().reason ==
								sqlite_shm_lease_rejection_reason::receipt_mismatch &&
							rejected.error().action ==
								sqlite_shm_lease_recovery_action::
									attempt_nonremoving_unmap_then_outer_ioerr,
						"native non-OK plus non-null requires raw cleanup and never mints");
				auto unsafe_no_map = coordinator.resolve_writer_map_failure(inflight);
				require(!unsafe_no_map &&
							unsafe_no_map.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							unsafe_no_map.error().action ==
								sqlite_shm_lease_recovery_action::quarantine_no_retry &&
							coordinator.snapshot().quarantined,
						"nonnull native result cannot be erased by no-map resolution");
			}
			require(coordinator.snapshot().quarantined,
					"unresolved contradictory native mapping quarantines on token abandonment");
		}

		{
			constexpr std::uint8_t marker = 77;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			{
				const auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
				auto begun = coordinator.begin_writer_map(request);
				require(begun.has_value(), "begin null-to-nonnull replay fixture");
				auto inflight = std::move(*begun);
				auto first = sqlite_writer_shm_native_map_receipt_validator::validate(
					inflight, sqlite_ok_status, nullptr);
				require(!first &&
							first.error().reason ==
								sqlite_shm_lease_rejection_reason::receipt_mismatch,
						"first SQLITE_OK-null result is classified once");
				auto replacement = sqlite_writer_shm_native_map_receipt_validator::validate(
					inflight, sqlite_ok_status, &page);
				require(!replacement &&
							replacement.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							replacement.error().action ==
								sqlite_shm_lease_recovery_action::quarantine_no_retry,
						"SQLITE_OK-null cannot be replaced by a later nonnull validation");
				auto unsafe_no_map = coordinator.resolve_writer_map_failure(inflight);
				require(!unsafe_no_map &&
							unsafe_no_map.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							unsafe_no_map.error().action ==
								sqlite_shm_lease_recovery_action::quarantine_no_retry &&
							coordinator.snapshot().quarantined,
						"replayed nonnull result cannot be erased by no-map resolution");
			}
			require(coordinator.snapshot().quarantined,
					"null-to-nonnull replay abandons the unresolved token under quarantine");
		}

		{
			constexpr std::uint8_t marker = 78;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			{
				const auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
				auto begun = coordinator.begin_writer_map(request);
				require(begun.has_value(), "begin duplicate null validation fixture");
				auto inflight = std::move(*begun);
				auto first = sqlite_writer_shm_native_map_receipt_validator::validate(
					inflight, sqlite_readonly_status, nullptr);
				require(!first &&
							first.error().reason ==
								sqlite_shm_lease_rejection_reason::receipt_mismatch,
						"first native non-OK-null result is classified once");
				auto moved = std::move(inflight);
				auto duplicate = sqlite_writer_shm_native_map_receipt_validator::validate(
					moved, sqlite_readonly_status, nullptr);
				require(!duplicate && moved.valid() &&
							duplicate.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							duplicate.error().action ==
								sqlite_shm_lease_recovery_action::quarantine_no_retry,
						"duplicate null validation is rejected after latch ownership moves");
				auto unsafe_no_map = coordinator.resolve_writer_map_failure(moved);
				require(!unsafe_no_map &&
							unsafe_no_map.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							unsafe_no_map.error().action ==
								sqlite_shm_lease_recovery_action::quarantine_no_retry &&
							coordinator.snapshot().quarantined,
						"duplicate null validation cannot be erased as one no-map result");
			}
			require(coordinator.snapshot().quarantined,
					"duplicate null validation cannot silently resolve or replay");
		}

		{
			constexpr std::uint8_t marker = 79;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			{
				const auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
				auto begun = coordinator.begin_writer_map(request);
				require(begun.has_value(), "begin duplicate nonnull validation fixture");
				auto inflight = std::move(*begun);
				auto first = sqlite_writer_shm_native_map_receipt_validator::validate(
					inflight, sqlite_ok_status, &page);
				require(first.has_value(), "first exact nonnull validation mints one receipt");
				auto duplicate = sqlite_writer_shm_native_map_receipt_validator::validate(
					inflight, sqlite_ok_status, &page);
				require(!duplicate &&
							duplicate.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							duplicate.error().action ==
								sqlite_shm_lease_recovery_action::quarantine_no_retry,
						"duplicate nonnull validation poisons the exact inflight");
				auto unsafe_record = coordinator.record_writer_native_mapping(inflight, *first);
				require(!unsafe_record && inflight.valid() &&
							unsafe_record.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							unsafe_record.error().action ==
								sqlite_shm_lease_recovery_action::quarantine_no_retry &&
							coordinator.snapshot().quarantined,
						"poisoned validation cannot transition its first receipt to authority");
			}
			require(coordinator.snapshot().quarantined,
					"duplicate nonnull validation retains terminal quarantine");
		}

		{
			constexpr std::uint8_t marker = 80;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			{
				const auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
				auto begun = coordinator.begin_writer_map(request);
				require(begun.has_value(), "begin validated-null record replacement fixture");
				auto inflight = std::move(*begun);
				auto first = sqlite_writer_shm_native_map_receipt_validator::validate(
					inflight, sqlite_ok_status, nullptr);
				require(!first &&
							first.error().reason ==
								sqlite_shm_lease_rejection_reason::receipt_mismatch,
						"validated null result remains a no-map result only");
				const auto forged_replacement =
					sqlite_same_process_shm_lease_test_peer::unchecked_writer_native_map(inflight,
																						 &page);
				auto replaced =
					coordinator.record_writer_native_mapping(inflight, forged_replacement);
				require(!replaced && inflight.valid() &&
							replaced.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							replaced.error().action ==
								sqlite_shm_lease_recovery_action::quarantine_no_retry &&
							coordinator.snapshot().quarantined,
						"record transition cannot replace a validated null result");
			}
			require(coordinator.snapshot().quarantined,
					"validated-null record replacement remains terminal");
		}
	}

	void verify_native_writer_receipt_binding_and_replay()
	{
		{
			constexpr std::uint8_t marker = 33;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			const auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			auto begun = coordinator.begin_writer_map(request);
			require(begun.has_value(), "begin writer before invalid native-map receipt");
			auto inflight = std::move(*begun);
			const auto invalid =
				sqlite_same_process_shm_lease_test_peer::unchecked_writer_native_map(inflight,
																					 nullptr);
			auto rejected = coordinator.record_writer_native_mapping(inflight, invalid);
			require(!rejected && inflight.valid() && coordinator.snapshot().quarantined &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch,
					"null native-map receipt creates no authority but retains the source attempt");
			require(!coordinator.resolve_writer_map_failure(inflight),
					"invalid post-native receipt cannot restore the no-map transition");
			const auto exact = sqlite_same_process_shm_lease_test_peer::unchecked_writer_native_map(
				inflight, &page);
			auto recovered = coordinator.record_writer_native_mapping(inflight, exact);
			require(recovered.has_value() && !inflight.valid(),
					"exact native receipt recovers cleanup after a malformed seal");
			auto cleanup = coordinator.begin_writer_cleanup(*recovered, request.callback);
			require(cleanup.has_value() && !recovered->valid(),
					"malformed seal retains one mandatory cleanup admission");
			require(!coordinator.complete_writer_cleanup(
						*cleanup,
						request.callback,
						sqlite_shm_native_cleanup_outcome::confirmed_success),
					"malformed-seal quarantine remains terminal after cleanup");
		}

		{
			constexpr std::uint8_t marker = 34;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			const auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			auto begun = coordinator.begin_writer_map(request);
			require(begun.has_value(), "begin writer before duplicate native-map receipt");
			auto inflight = std::move(*begun);
			auto receipt = sqlite_writer_shm_native_map_receipt_validator::validate(
				inflight, sqlite_ok_status, &page);
			require(receipt.has_value() && inflight.valid(),
					"production validator seals exact native-map receipt");
			const auto replay_receipt = *receipt;
			auto recorded = coordinator.record_writer_native_mapping(inflight, *receipt);
			require(recorded.has_value() && !inflight.valid(),
					"exact native-map receipt creates one cleanup-only token");
			auto post_native = std::move(*recorded);
			auto duplicate = coordinator.record_writer_native_mapping(inflight, replay_receipt);
			require(!duplicate && post_native.valid() && coordinator.snapshot().quarantined,
					"duplicate native-map transition quarantines without consuming cleanup duty");
			auto cleanup = coordinator.begin_writer_cleanup(post_native, request.callback);
			require(cleanup.has_value() && !post_native.valid(),
					"duplicate transition still admits the one mandatory native cleanup");
			auto completed = coordinator.complete_writer_cleanup(
				*cleanup, request.callback, sqlite_shm_native_cleanup_outcome::confirmed_success);
			require(!completed && !cleanup->valid() &&
						completed.error().action ==
							sqlite_shm_lease_recovery_action::quarantine_no_retry,
					"cleanup under duplicate-callback quarantine never revives authority");
		}

		{
			constexpr std::uint8_t marker = 35;
			const auto binding = family(marker);
			const auto source_connection = identity("test.connection", marker);
			const auto target_connection = identity("test.connection", marker + 1U);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator source{binding, generations};
			sqlite_same_process_shm_mapping_lease_coordinator target{binding, generations};
			int source_page{};
			int target_page{};
			const auto source_request =
				writer_request(binding, source_connection, marker, 1, marker, 0, 1);
			const auto target_request =
				writer_request(binding, target_connection, marker + 1U, 2, marker + 1U, 0, 1);
			auto source_begun = source.begin_writer_map(source_request);
			auto target_begun = target.begin_writer_map(target_request);
			require(source_begun && target_begun, "begin cross-coordinator receipt fixture");
			auto source_inflight = std::move(*source_begun);
			auto target_inflight = std::move(*target_begun);
			const auto source_receipt =
				sqlite_same_process_shm_lease_test_peer::unchecked_writer_native_map(
					source_inflight, &source_page);
			const auto target_receipt =
				sqlite_same_process_shm_lease_test_peer::unchecked_writer_native_map(
					target_inflight, &target_page);
			auto cross_bound = target.record_writer_native_mapping(target_inflight, source_receipt);
			auto reverse_cross_bound =
				source.record_writer_native_mapping(source_inflight, target_receipt);
			require(!cross_bound && !reverse_cross_bound && target_inflight.valid() &&
						source_inflight.valid() && target.snapshot().quarantined &&
						source.snapshot().quarantined &&
						cross_bound.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch &&
						reverse_cross_bound.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch,
					"cross-routed native receipts retain both exact cleanup routes");
			require(!target.resolve_writer_map_failure(target_inflight) &&
						!source.resolve_writer_map_failure(source_inflight),
					"cross-routed post-native attempts cannot resolve as no-map failures");
			auto source_post = source.record_writer_native_mapping(source_inflight, source_receipt);
			auto target_post = target.record_writer_native_mapping(target_inflight, target_receipt);
			require(source_post.has_value() && target_post.has_value(),
					"both exact receipts recover one cleanup-only token after cross-routing");
			auto source_cleanup =
				source.begin_writer_cleanup(*source_post, source_request.callback);
			auto target_cleanup =
				target.begin_writer_cleanup(*target_post, target_request.callback);
			require(source_cleanup && target_cleanup && !source_post->valid() &&
						!target_post->valid(),
					"both cross-routed mappings retain mandatory cleanup admission");
			require(!source.complete_writer_cleanup(
						*source_cleanup,
						source_request.callback,
						sqlite_shm_native_cleanup_outcome::confirmed_success) &&
						!target.complete_writer_cleanup(
							*target_cleanup,
							target_request.callback,
							sqlite_shm_native_cleanup_outcome::confirmed_success),
					"cross-routing quarantine remains terminal after both native cleanups");
		}

		{
			constexpr std::uint8_t marker = 36;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			const auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			auto begun = coordinator.begin_writer_map(request);
			require(begun.has_value(), "begin writer transition-failure fixture");
			auto inflight = std::move(*begun);
			auto receipt = sqlite_writer_shm_native_map_receipt_validator::validate(
				inflight, sqlite_ok_status, &page);
			require(receipt.has_value(), "validate native mapping before injected transition");
			sqlite_same_process_shm_lease_test_peer::fail_next_writer_native_transition(
				coordinator);
			auto failed = coordinator.record_writer_native_mapping(inflight, *receipt);
			require(!failed && inflight.valid() && coordinator.snapshot().quarantined,
					"injected native transition failure retains the exact source token");
			auto unsafe_resolution = coordinator.resolve_writer_map_failure(inflight);
			require(!unsafe_resolution &&
						unsafe_resolution.error().action ==
							sqlite_shm_lease_recovery_action::quarantine_no_retry,
					"transition failure cannot erase a native mapping as a no-map result");
			auto recovered = coordinator.record_writer_native_mapping(inflight, *receipt);
			require(recovered.has_value() && !inflight.valid(),
					"exact retry recovers the cleanup-only post-native token");
			auto cleanup = coordinator.begin_writer_cleanup(*recovered, request.callback);
			require(cleanup.has_value() && !recovered->valid(),
					"transition failure preserves mandatory cleanup admission");
			auto completed = coordinator.complete_writer_cleanup(
				*cleanup, request.callback, sqlite_shm_native_cleanup_outcome::confirmed_success);
			require(!completed && !cleanup->valid(),
					"transition failure quarantine remains terminal after native cleanup");
		}
	}

	void verify_cleanup_completion_requires_exact_callback()
	{
		constexpr std::uint8_t marker = 53;
		const auto binding = family(marker);
		const auto connection = identity("test.connection", marker);
		const auto open_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto pending = install_pending(coordinator,
									   writer_request(binding, connection, marker, 1, marker, 0, 1),
									   open_epoch,
									   mapping(0, &page, 4096U),
									   sqlite_shm_writer_extend_pair::one_one,
									   marker);
		const auto admitted_callback = callback(1, 100);
		auto cleanup = coordinator.begin_writer_cleanup(pending, admitted_callback);
		require(cleanup.has_value() && !pending.valid(),
				"writer cleanup callback is admitted before native unmap");
		auto mismatched = coordinator.complete_writer_cleanup(
			*cleanup, callback(1, 101), sqlite_shm_native_cleanup_outcome::confirmed_success);
		const auto snapshot = coordinator.snapshot();
		require(!mismatched && !cleanup->valid() && snapshot.quarantined &&
					snapshot.writer_cleanup_count == 1U,
				"cleanup completion under another callback becomes a terminal tombstone");
	}

	void verify_failed_cleanup_admission_is_terminal_without_retry()
	{
		{
			constexpr std::uint8_t marker = 54;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			const auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			auto begun = coordinator.begin_writer_map(request);
			require(begun.has_value(), "begin rejected writer cleanup-admission fixture");
			auto inflight = std::move(*begun);
			auto post_native = record_native_mapping(coordinator, inflight, &page);
			auto rejected =
				coordinator.install_pending(post_native,
											writer_receipt(request,
														   open_epoch,
														   mapping(1, &page, 8192U),
														   sqlite_shm_writer_extend_pair::one_one,
														   marker));
			require(!rejected && post_native.valid(),
					"writer post-native mismatch retains one cleanup-admission attempt");

			auto bad = coordinator.begin_writer_cleanup(post_native, callback(1, marker + 1U));
			require(!bad && !post_native.valid() && coordinator.snapshot().quarantined &&
						coordinator.snapshot().writer_cleanup_count == 1U,
					"reordered writer cleanup callback consumes the token into a tombstone");
			auto retry = coordinator.begin_writer_cleanup(post_native, request.callback);
			require(!retry &&
						retry.error().action ==
							sqlite_shm_lease_recovery_action::quarantine_no_retry &&
						coordinator.snapshot().writer_cleanup_count == 1U,
					"rejected writer cleanup admission cannot issue a second obligation");
		}

		{
			constexpr std::uint8_t marker = 55;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			const auto request = writer_request(binding, connection, marker, 1, marker, 0, 1);
			auto pending = install_pending(coordinator,
										   request,
										   open_epoch,
										   mapping(0, &page, 4096U),
										   sqlite_shm_writer_extend_pair::one_one,
										   marker);

			auto bad =
				coordinator.begin_writer_cleanup(pending, sqlite_shm_callback_execution_receipt{});
			require(!bad && !pending.valid() && coordinator.snapshot().quarantined &&
						coordinator.snapshot().writer_cleanup_count == 1U,
					"invalid pending cleanup callback consumes the token into a tombstone");
			auto retry = coordinator.begin_writer_cleanup(pending, request.callback);
			require(!retry &&
						retry.error().action ==
							sqlite_shm_lease_recovery_action::quarantine_no_retry &&
						coordinator.snapshot().writer_cleanup_count == 1U,
					"pending writer cleanup cannot retry after failed callback admission");
		}

		{
			constexpr std::uint8_t marker = 56;
			const auto binding = family(marker);
			const auto writer_connection = identity("test.connection", marker);
			const auto reader_connection = identity("test.connection", marker + 1U);
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			int mismatched_page{};
			auto live = install_live_writer(
				coordinator, binding, writer_connection, open_epoch, marker, &page);
			const auto request =
				reader_request(binding, reader_connection, marker + 1U, 2, marker + 2U);
			auto begun = coordinator.begin_reader_map(request);
			require(begun.has_value(), "begin rejected reader cleanup-admission fixture");
			auto inflight = std::move(*begun);
			auto rejected =
				coordinator.promote_reader(inflight,
										   sqlite_same_process_shm_lease_test_peer::reader_map(
											   request,
											   live.holder.generation(),
											   mapping(0, &mismatched_page, 4096U),
											   identity("test.zero-reader-resize", marker)));
			require(!rejected && inflight.valid(),
					"reader post-native mismatch retains one cleanup-admission attempt");

			auto bad = coordinator.begin_reader_cleanup(inflight, callback(2, marker + 1U));
			require(!bad && !inflight.valid() && coordinator.snapshot().quarantined &&
						coordinator.snapshot().reader_cleanup_count == 1U,
					"reordered reader cleanup callback consumes the token into a tombstone");
			auto retry = coordinator.begin_reader_cleanup(inflight, request.callback);
			require(!retry &&
						retry.error().action ==
							sqlite_shm_lease_recovery_action::quarantine_no_retry &&
						coordinator.snapshot().reader_cleanup_count == 1U,
					"reader cleanup cannot retry after failed callback admission");
		}

		{
			constexpr std::uint8_t marker = 57;
			const auto binding = family(marker);
			const auto writer_connection = identity("test.connection", marker);
			const auto reader_connection = identity("test.connection", marker + 1U);
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto live = install_live_writer(
				coordinator, binding, writer_connection, open_epoch, marker, &page);
			const auto request =
				reader_request(binding, reader_connection, marker + 1U, 2, marker + 3U);
			auto pin = coordinator.begin_reader_map(request);
			require(pin.has_value(), "begin reader unmap-admission fixture");
			auto promoted =
				coordinator.promote_reader(*pin,
										   sqlite_same_process_shm_lease_test_peer::reader_map(
											   request,
											   live.holder.generation(),
											   mapping(0, &page, 4096U),
											   identity("test.zero-reader-resize", marker)));
			require(promoted.has_value(), "create reader handoff before bad unmap callback");
			auto handoff = std::move(*promoted);

			auto bad =
				coordinator.begin_reader_unmap(handoff, sqlite_shm_callback_execution_receipt{});
			require(!bad && !handoff.valid() && coordinator.snapshot().quarantined &&
						coordinator.snapshot().reader_handoff_count == 1U &&
						coordinator.snapshot().reader_cleanup_count == 1U,
					"invalid reader unmap callback consumes the handoff into a tombstone");
			auto retry = coordinator.begin_reader_unmap(handoff, callback(2, marker + 1U));
			require(!retry &&
						retry.error().action ==
							sqlite_shm_lease_recovery_action::quarantine_no_retry &&
						coordinator.snapshot().reader_handoff_count == 1U &&
						coordinator.snapshot().reader_cleanup_count == 1U,
					"reader unmap cannot retry after failed callback admission");
		}

		{
			constexpr std::uint8_t marker = 60;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto live =
				install_live_writer(coordinator, binding, connection, open_epoch, marker, &page);

			auto bad = coordinator.release_writer_holder(live.holder,
														 sqlite_shm_callback_execution_receipt{});
			require(!bad && !live.holder.valid() && coordinator.snapshot().quarantined &&
						coordinator.snapshot().writer_cleanup_count == 1U,
					"invalid holder release callback consumes the holder into a tombstone");
			auto retry = coordinator.release_writer_holder(live.holder, callback(1, 100));
			require(!retry &&
						retry.error().action ==
							sqlite_shm_lease_recovery_action::outer_ioerr_no_retry &&
						coordinator.snapshot().writer_cleanup_count == 1U,
					"holder release cannot retry after failed callback admission");
		}
	}

	void verify_family_quarantine_preserves_unattempted_mandatory_drains()
	{
		{
			constexpr std::uint8_t marker = 58;
			const auto binding = family(marker);
			const auto first_connection = identity("test.connection", marker);
			const auto second_connection = identity("test.connection", marker + 1U);
			const auto first_epoch = identity("test.open-epoch", marker);
			const auto second_epoch = identity("test.open-epoch", marker + 1U);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			const auto first_request =
				writer_request(binding, first_connection, marker, 1, marker, 0, 1);
			const auto second_request =
				writer_request(binding, second_connection, marker + 1U, 2, marker + 1U, 0, 1);
			auto first = install_pending(coordinator,
										 first_request,
										 first_epoch,
										 mapping(0, &page, 4096U),
										 sqlite_shm_writer_extend_pair::one_one,
										 marker);
			auto second = install_pending(coordinator,
										  second_request,
										  second_epoch,
										  mapping(0, &page, 4096U),
										  sqlite_shm_writer_extend_pair::one_one,
										  marker + 1U);

			auto bad =
				coordinator.begin_writer_cleanup(first, sqlite_shm_callback_execution_receipt{});
			require(!bad && coordinator.snapshot().quarantined && !first.valid(),
					"one failed writer callback admission quarantines its own cleanup token");
			const auto second_cleanup_callback = callback(1, marker + 2U);
			auto drain = coordinator.begin_writer_cleanup(second, second_cleanup_callback);
			require(drain.has_value() && !second.valid() &&
						coordinator.snapshot().writer_cleanup_count == 2U,
					"terminal writer callbacks do not block another mapping's first drain");
			auto completed = coordinator.complete_writer_cleanup(
				*drain,
				second_cleanup_callback,
				sqlite_shm_native_cleanup_outcome::confirmed_success);
			require(!completed && !drain->valid() &&
						coordinator.snapshot().writer_cleanup_count == 2U,
					"mandatory drain under family quarantine terminates as a tombstone");
		}

		{
			constexpr std::uint8_t marker = 59;
			const auto binding = family(marker);
			const auto writer_connection = identity("test.connection", marker);
			const auto first_reader_connection = identity("test.connection", marker + 1U);
			const auto second_reader_connection = identity("test.connection", marker + 2U);
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto live = install_live_writer(
				coordinator, binding, writer_connection, open_epoch, marker, &page);
			const auto first_request =
				reader_request(binding, first_reader_connection, marker + 1U, 2, marker + 1U);
			const auto second_request =
				reader_request(binding, second_reader_connection, marker + 2U, 3, marker + 2U);
			auto first_pin = coordinator.begin_reader_map(first_request);
			auto second_pin = coordinator.begin_reader_map(second_request);
			require(first_pin && second_pin, "acquire two reader pins before family quarantine");
			auto first_handoff =
				coordinator.promote_reader(*first_pin,
										   sqlite_same_process_shm_lease_test_peer::reader_map(
											   first_request,
											   live.holder.generation(),
											   mapping(0, &page, 4096U),
											   identity("test.zero-reader-resize", marker + 1U)));
			auto second_handoff =
				coordinator.promote_reader(*second_pin,
										   sqlite_same_process_shm_lease_test_peer::reader_map(
											   second_request,
											   live.holder.generation(),
											   mapping(0, &page, 4096U),
											   identity("test.zero-reader-resize", marker + 2U)));
			require(first_handoff && second_handoff,
					"promote two attachments before family quarantine");

			auto bad = coordinator.begin_reader_unmap(*first_handoff,
													  sqlite_shm_callback_execution_receipt{});
			require(!bad && coordinator.snapshot().quarantined && !first_handoff->valid(),
					"one failed unmap admission quarantines its own handoff");
			const auto second_callback = callback(3, 100);
			auto drain = coordinator.begin_reader_unmap(*second_handoff, second_callback);
			require(drain.has_value() && !second_handoff->valid() &&
						coordinator.snapshot().reader_cleanup_count == 2U,
					"family quarantine preserves another attachment's first mandatory drain");
			auto completed = coordinator.complete_reader_unmap(
				*drain, second_callback, sqlite_shm_native_cleanup_outcome::confirmed_success);
			require(!completed && !drain->valid() &&
						coordinator.snapshot().reader_handoff_count == 2U &&
						coordinator.snapshot().reader_cleanup_count == 2U,
					"attachment drain under family quarantine terminates as a tombstone");
		}

		{
			constexpr std::uint8_t marker = 61;
			const auto binding = family(marker);
			const auto writer_connection = identity("test.connection", marker);
			const auto first_reader_connection = identity("test.connection", marker + 1U);
			const auto second_reader_connection = identity("test.connection", marker + 2U);
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			int first_mismatch{};
			int second_mismatch{};
			auto live = install_live_writer(
				coordinator, binding, writer_connection, open_epoch, marker, &page);
			const auto first_request =
				reader_request(binding, first_reader_connection, marker + 1U, 2, marker + 1U);
			const auto second_request =
				reader_request(binding, second_reader_connection, marker + 2U, 3, marker + 2U);
			auto first_pin = coordinator.begin_reader_map(first_request);
			auto second_pin = coordinator.begin_reader_map(second_request);
			require(first_pin && second_pin,
					"acquire two rejected reader cleanup sources before quarantine");
			auto first_rejected =
				coordinator.promote_reader(*first_pin,
										   sqlite_same_process_shm_lease_test_peer::reader_map(
											   first_request,
											   live.holder.generation(),
											   mapping(0, &first_mismatch, 4096U),
											   identity("test.zero-reader-resize", marker + 1U)));
			auto second_rejected =
				coordinator.promote_reader(*second_pin,
										   sqlite_same_process_shm_lease_test_peer::reader_map(
											   second_request,
											   live.holder.generation(),
											   mapping(0, &second_mismatch, 4096U),
											   identity("test.zero-reader-resize", marker + 2U)));
			require(!first_rejected && !second_rejected,
					"retain both post-native reader cleanup attempts");

			auto bad = coordinator.begin_reader_cleanup(*first_pin, callback(2, marker + 3U));
			require(!bad && coordinator.snapshot().quarantined && !first_pin->valid(),
					"one failed reader cleanup admission becomes a terminal callback");
			const auto second_cleanup_callback = callback(2, marker + 4U);
			auto drain = coordinator.begin_reader_cleanup(*second_pin, second_cleanup_callback);
			require(drain.has_value() && !second_pin->valid() &&
						coordinator.snapshot().reader_cleanup_count == 2U,
					"terminal reader callbacks do not block another mapping's first drain");
			auto completed = coordinator.complete_reader_cleanup(
				*drain,
				second_cleanup_callback,
				sqlite_shm_native_cleanup_outcome::confirmed_success);
			require(!completed && !drain->valid() &&
						coordinator.snapshot().reader_cleanup_count == 2U,
					"reader drain under family quarantine terminates as a tombstone");
		}

		{
			constexpr std::uint8_t marker = 62;
			const auto binding = family(marker);
			const auto first_connection = identity("test.connection", marker);
			const auto second_connection = identity("test.connection", marker + 1U);
			const auto pending_connection = identity("test.connection", marker + 2U);
			const auto first_epoch = identity("test.open-epoch", marker);
			const auto second_epoch = identity("test.open-epoch", marker + 1U);
			const auto pending_epoch = identity("test.open-epoch", marker + 2U);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			const auto mapped = mapping(0, &page, 4096U);
			auto first = install_live_writer(
				coordinator, binding, first_connection, first_epoch, marker, &page);
			auto second_gate = install_eligibility(
				coordinator, binding, second_connection, second_epoch, marker + 1U);
			auto second_pending = install_pending(
				coordinator,
				writer_request(binding, second_connection, marker + 1U, 2, marker + 1U, 0, 0),
				second_epoch,
				mapped,
				sqlite_shm_writer_extend_pair::zero_zero,
				marker + 1U);
			auto second_holder = promote(coordinator, second_pending, second_gate);
			const auto pending_request =
				writer_request(binding, pending_connection, marker + 2U, 6, marker + 2U, 0, 1);
			auto pending = install_pending(coordinator,
										   pending_request,
										   pending_epoch,
										   mapped,
										   sqlite_shm_writer_extend_pair::one_one,
										   marker + 2U);

			const auto first_release_callback = callback(5, 100);
			auto first_release =
				coordinator.release_writer_holder(first.holder, first_release_callback);
			require(first_release &&
						first_release->decision() ==
							sqlite_shm_writer_retirement_decision::not_last_attachment,
					"first of two holders admits native cleanup");
			auto failed_first_cleanup =
				coordinator.complete_writer_cleanup(first_release->cleanup(),
													first_release_callback,
													sqlite_shm_native_cleanup_outcome::unknown);
			require(!failed_first_cleanup && coordinator.snapshot().quarantined,
					"unknown non-last holder cleanup creates a terminal holder callback");

			const auto pending_cleanup_callback = callback(5, 101);
			auto pending_drain =
				coordinator.begin_writer_cleanup(pending, pending_cleanup_callback);
			require(pending_drain.has_value() && !pending.valid(),
					"terminal holder callback does not block another mapping's first drain");
			auto completed_pending = coordinator.complete_writer_cleanup(
				*pending_drain,
				pending_cleanup_callback,
				sqlite_shm_native_cleanup_outcome::confirmed_success);
			require(!completed_pending && !pending_drain->valid(),
					"pending drain under holder quarantine becomes a tombstone");

			const auto last_release_callback = callback(7, 102);
			auto last_release =
				coordinator.release_writer_holder(second_holder, last_release_callback);
			require(last_release &&
						last_release->decision() == sqlite_shm_writer_retirement_decision::ready &&
						!second_holder.valid() && last_release->cleanup().valid(),
					"prior family quarantine still admits the last holder mandatory drain");
			auto completed_last = coordinator.complete_writer_cleanup(
				last_release->cleanup(),
				last_release_callback,
				sqlite_shm_native_cleanup_outcome::confirmed_success);
			require(!completed_last && !last_release->cleanup().valid() &&
						coordinator.snapshot().writer_holder_count == 0U,
					"last holder drain under family quarantine terminates as a tombstone");
			require(coordinator.revoke_writer_eligibility(first.eligibility).has_value() &&
						coordinator.revoke_writer_eligibility(second_gate).has_value(),
					"revoke holder-drain eligibility receipts");
		}
	}

	void verify_reader_cleanup_failures_quarantine()
	{
		std::uint8_t marker = 31;
		for (const auto outcome : {sqlite_shm_native_cleanup_outcome::non_ok,
								   sqlite_shm_native_cleanup_outcome::unknown})
		{
			const auto binding = family(marker);
			const auto writer_connection = identity("test.connection", marker);
			const auto reader_connection =
				identity("test.connection", static_cast<std::uint8_t>(marker + 40U));
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int leased_page{};
			int mismatched_page{};
			const auto mapped = mapping(0, &leased_page, 4096U);
			auto gate =
				install_eligibility(coordinator, binding, writer_connection, open_epoch, marker);
			auto pending =
				install_pending(coordinator,
								writer_request(binding, writer_connection, marker, 1, marker, 0, 1),
								open_epoch,
								mapped,
								sqlite_shm_writer_extend_pair::one_one,
								marker);
			auto holder = promote(coordinator, pending, gate);

			const auto request = reader_request(binding,
												reader_connection,
												static_cast<std::uint8_t>(marker + 40U),
												2,
												static_cast<std::uint8_t>(marker + 40U));
			auto begun = coordinator.begin_reader_map(request);
			require(begun.has_value(), "begin reader before post-native mismatch");
			auto inflight = std::move(*begun);
			auto rejected =
				coordinator.promote_reader(inflight,
										   sqlite_same_process_shm_lease_test_peer::reader_map(
											   request,
											   holder.generation(),
											   mapping(0, &mismatched_page, 4096U),
											   identity("test.zero-reader-resize", marker)));
			require(!rejected &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch &&
						inflight.valid(),
					"reader post receipt mismatch retains a cleanup obligation");
			auto unsafe_resolution = coordinator.resolve_reader_map_failure(inflight);
			require(!unsafe_resolution &&
						unsafe_resolution.error().reason ==
							sqlite_shm_lease_rejection_reason::stale_token,
					"post-native reader rejection cannot use the no-mapping transition");

			auto begun_cleanup = coordinator.begin_reader_cleanup(inflight, request.callback);
			require(begun_cleanup.has_value() && !inflight.valid() &&
						coordinator.snapshot().reader_cleanup_count == 1U,
					"reader cleanup obligation is visible before native unmap");
			auto cleanup = std::move(*begun_cleanup);
			auto completed =
				coordinator.complete_reader_cleanup(cleanup, request.callback, outcome);
			require(!completed && !cleanup.valid() && coordinator.snapshot().quarantined,
					"reader non-OK or unknown cleanup permanently quarantines");
			++marker;
		}
	}

	void verify_reader_unmap_failure_retains_terminal_handoff()
	{
		constexpr std::uint8_t marker = 52;
		const auto binding = family(marker);
		const auto writer_connection = identity("test.connection", marker);
		const auto reader_connection = identity("test.connection", marker + 1U);
		const auto open_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto live =
			install_live_writer(coordinator, binding, writer_connection, open_epoch, marker, &page);
		const auto request =
			reader_request(binding, reader_connection, marker + 1U, 2, marker + 1U);
		auto pin = coordinator.begin_reader_map(request);
		require(pin.has_value(), "reader unmap failure fixture acquires a pin");
		auto promoted =
			coordinator.promote_reader(*pin,
									   sqlite_same_process_shm_lease_test_peer::reader_map(
										   request,
										   live.holder.generation(),
										   mapping(0, &page, 4096U),
										   identity("test.zero-reader-resize", marker)));
		require(promoted.has_value(), "reader unmap failure fixture creates a handoff");
		auto handoff = std::move(*promoted);
		const auto unmap_callback = callback(2, 100);
		auto unmap = coordinator.begin_reader_unmap(handoff, unmap_callback);
		require(unmap.has_value() && !handoff.valid(),
				"reader handoff is hidden before failing native unmap");
		auto failed = coordinator.complete_reader_unmap(
			*unmap, unmap_callback, sqlite_shm_native_cleanup_outcome::unknown);
		const auto snapshot = coordinator.snapshot();
		require(!failed && !unmap->valid() && snapshot.quarantined &&
					snapshot.reader_handoff_count == 1U && snapshot.reader_cleanup_count >= 1U,
				"unknown reader unmap retains a terminal handoff tombstone with no retry");
	}

	void verify_pending_and_eligibility_are_not_reader_authority()
	{
		const auto binding = family(1);
		const auto connection = identity("test.connection", 1);
		const auto open_epoch = identity("test.open-epoch", 1);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};

		auto eligibility = install_eligibility(coordinator, binding, connection, open_epoch, 1);
		const auto reader = reader_request(binding, connection, 2, 2, 2);
		auto eligibility_only = coordinator.begin_reader_map(reader);
		require(!eligibility_only &&
					eligibility_only.error().reason ==
						sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
				"eligibility alone is not reader authority");

		const auto writer = writer_request(binding, connection, 1, 1, 1, 0, 1);
		auto pending = install_pending(coordinator,
									   writer,
									   open_epoch,
									   mapping(0, &page, 4096U),
									   sqlite_shm_writer_extend_pair::one_one,
									   1);
		auto pending_only = coordinator.begin_reader_map(reader);
		require(!pending_only &&
					pending_only.error().reason ==
						sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
				"pending plus eligibility remains invisible before promotion");
		cleanup_writer(coordinator, pending, writer.callback);
		require(coordinator.revoke_writer_eligibility(eligibility).has_value(),
				"revoke eligibility");
	}

	void verify_map_before_gate_and_gate_before_map()
	{
		for (const bool gate_first : {false, true})
		{
			const auto marker = static_cast<std::uint8_t>(gate_first ? 3 : 2);
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};

			std::optional<sqlite_shm_writer_eligibility> eligibility;
			if (gate_first)
				eligibility.emplace(
					install_eligibility(coordinator, binding, connection, open_epoch, marker));
			auto pending =
				install_pending(coordinator,
								writer_request(binding, connection, marker, marker, marker, 0, 0),
								open_epoch,
								mapping(0, &page, 4096U),
								sqlite_shm_writer_extend_pair::zero_zero,
								marker);
			require(!coordinator.snapshot().reader_admission_visible,
					"pending never exposes reader admission");
			if (!gate_first)
				eligibility.emplace(
					install_eligibility(coordinator, binding, connection, open_epoch, marker));
			auto holder = promote(coordinator, pending, *eligibility);
			const auto live = coordinator.snapshot();
			require(live.phase == sqlite_shm_mapping_generation_phase::live &&
						live.writer_holder_count == 1U && live.reader_admission_visible,
					"both receipt orders promote exactly one live holder");
			retire_last(coordinator, holder, callback(9, 100));
			require(coordinator.revoke_writer_eligibility(*eligibility).has_value(),
					"revoke ordered eligibility");
		}
	}

	void verify_cross_alias_mixed_pair_join_in_both_directions()
	{
		for (const auto first_pair :
			 {sqlite_shm_writer_extend_pair::one_one, sqlite_shm_writer_extend_pair::zero_zero})
		{
			const auto marker = static_cast<std::uint8_t>(
				first_pair == sqlite_shm_writer_extend_pair::one_one ? 4 : 5);
			const auto second_pair = first_pair == sqlite_shm_writer_extend_pair::one_one
				? sqlite_shm_writer_extend_pair::zero_zero
				: sqlite_shm_writer_extend_pair::one_one;
			const auto binding = family(marker);
			const auto first_connection = identity("test.connection", marker);
			const auto second_connection =
				identity("test.connection", static_cast<std::uint8_t>(marker + 20U));
			const auto first_epoch = identity("test.open-epoch", marker);
			const auto second_epoch =
				identity("test.open-epoch", static_cast<std::uint8_t>(marker + 20U));
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			const auto mapped = mapping(0, &page, 4096U);

			auto first_gate =
				install_eligibility(coordinator, binding, first_connection, first_epoch, marker);
			auto first_pending = install_pending(
				coordinator,
				writer_request(binding,
							   first_connection,
							   marker,
							   1,
							   marker,
							   0,
							   first_pair == sqlite_shm_writer_extend_pair::one_one ? 1 : 0),
				first_epoch,
				mapped,
				first_pair,
				marker);
			auto first_holder = promote(coordinator, first_pending, first_gate);

			auto second_gate = install_eligibility(coordinator,
												   binding,
												   second_connection,
												   second_epoch,
												   static_cast<std::uint8_t>(marker + 20U));
			auto second_pending = install_pending(
				coordinator,
				writer_request(binding,
							   second_connection,
							   static_cast<std::uint8_t>(marker + 20U),
							   2,
							   static_cast<std::uint8_t>(marker + 20U),
							   0,
							   second_pair == sqlite_shm_writer_extend_pair::one_one ? 1 : 0),
				second_epoch,
				mapped,
				second_pair,
				static_cast<std::uint8_t>(marker + 20U));
			auto second_holder = promote(coordinator, second_pending, second_gate);
			require(first_holder.generation() == second_holder.generation() &&
						coordinator.snapshot().writer_holder_count == 2U &&
						coordinator.snapshot().writer_attachment_identity_count == 2U &&
						coordinator.snapshot().writer_attachment_unresolved_count == 2U,
					"distinct aliases and mixed holder receipts join one generation");

			auto first_release = coordinator.release_writer_holder(first_holder, callback(8, 100));
			require(first_release.has_value() &&
						first_release->decision() ==
							sqlite_shm_writer_retirement_decision::not_last_attachment,
					"first mixed holder is not last");
			require(
				coordinator
					.complete_writer_cleanup(first_release->cleanup(),
											 callback(8, 100),
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"non-last mixed holder requires exact native cleanup");
			retire_last(coordinator, second_holder, callback(8, 101));
			require(coordinator.revoke_writer_eligibility(first_gate).has_value() &&
						coordinator.revoke_writer_eligibility(second_gate).has_value(),
					"revoke mixed holder gates");
		}
	}

	void verify_simultaneous_first_writer_total_order_and_mismatch()
	{
		const auto binding = family(6);
		const auto connection_a = identity("test.connection", 6);
		const auto connection_b = identity("test.connection", 7);
		const auto epoch_a = identity("test.open-epoch", 6);
		const auto epoch_b = identity("test.open-epoch", 7);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		const auto mapped = mapping(0, &page, 4096U);
		const auto request_a = writer_request(binding, connection_a, 6, 1, 6, 0, 1);
		const auto request_b = writer_request(binding, connection_b, 7, 2, 7, 0, 0);
		auto inflight_a = coordinator.begin_writer_map(request_a);
		auto inflight_b = coordinator.begin_writer_map(request_b);
		require(inflight_a && inflight_b && coordinator.snapshot().writer_inflight_count == 2U,
				"simultaneous first writers acquire one cohort before a generation exists");
		auto post_native_a = record_native_mapping(coordinator, *inflight_a, mapped.native_mapping);
		auto post_native_b = record_native_mapping(coordinator, *inflight_b, mapped.native_mapping);
		auto pending_a = coordinator.install_pending(
			post_native_a,
			writer_receipt(request_a, epoch_a, mapped, sqlite_shm_writer_extend_pair::one_one, 6));
		auto pending_b = coordinator.install_pending(
			post_native_b,
			writer_receipt(
				request_b, epoch_b, mapped, sqlite_shm_writer_extend_pair::zero_zero, 7));
		require(pending_a && pending_b, "both first writers install post-map pending");
		auto gate_a = install_eligibility(coordinator, binding, connection_a, epoch_a, 6);
		auto gate_b = install_eligibility(coordinator, binding, connection_b, epoch_b, 7);
		std::barrier promotion_start{3};
		std::optional<sqlite_shm_writer_holder> holder_a;
		std::optional<sqlite_shm_writer_holder> holder_b;
		std::exception_ptr error_a;
		std::exception_ptr error_b;
		auto promote_concurrently =
			[&coordinator, &promotion_start](sqlite_shm_pending_mapping& pending,
											 const sqlite_shm_writer_eligibility& eligibility,
											 std::optional<sqlite_shm_writer_holder>& output,
											 std::exception_ptr& error)
		{
			promotion_start.arrive_and_wait();
			try
			{
				auto promoted = coordinator.promote_writer(pending, eligibility);
				require(promoted.has_value(), "concurrent first writer promotes");
				output.emplace(std::move(*promoted));
			}
			catch (...)
			{
				error = std::current_exception();
			}
		};
		std::jthread thread_a{promote_concurrently,
							  std::ref(*pending_a),
							  std::cref(gate_a),
							  std::ref(holder_a),
							  std::ref(error_a)};
		std::jthread thread_b{promote_concurrently,
							  std::ref(*pending_b),
							  std::cref(gate_b),
							  std::ref(holder_b),
							  std::ref(error_b)};
		promotion_start.arrive_and_wait();
		thread_a.join();
		thread_b.join();
		if (error_a)
			std::rethrow_exception(error_a);
		if (error_b)
			std::rethrow_exception(error_b);
		require(holder_a && holder_b && holder_a->generation() == holder_b->generation(),
				"racing first promotions install and join one generation");
		auto released_a = coordinator.release_writer_holder(*holder_a, callback(9, 100));
		require(released_a &&
					released_a->decision() ==
						sqlite_shm_writer_retirement_decision::not_last_attachment,
				"simultaneous first holder retains the joined generation");
		require(coordinator
					.complete_writer_cleanup(released_a->cleanup(),
											 callback(9, 100),
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"simultaneous non-last holder completes native cleanup");
		retire_last(coordinator, *holder_b, callback(9, 101));
		require(coordinator.revoke_writer_eligibility(gate_a).has_value() &&
					coordinator.revoke_writer_eligibility(gate_b).has_value(),
				"revoke simultaneous gates");

		const auto mismatch_binding = family(8);
		const auto mismatch_connection_a = identity("test.connection", 8);
		const auto mismatch_connection_b = identity("test.connection", 9);
		const auto mismatch_epoch_a = identity("test.open-epoch", 8);
		const auto mismatch_epoch_b = identity("test.open-epoch", 9);
		sqlite_same_process_shm_mapping_lease_coordinator mismatch{mismatch_binding, generations};
		int first_page{};
		int different_page{};
		auto mismatch_gate_a = install_eligibility(
			mismatch, mismatch_binding, mismatch_connection_a, mismatch_epoch_a, 8);
		auto mismatch_gate_b = install_eligibility(
			mismatch, mismatch_binding, mismatch_connection_b, mismatch_epoch_b, 9);
		auto mismatch_pending_a =
			install_pending(mismatch,
							writer_request(mismatch_binding, mismatch_connection_a, 8, 1, 8, 0, 1),
							mismatch_epoch_a,
							mapping(0, &first_page, 4096U),
							sqlite_shm_writer_extend_pair::one_one,
							8);
		auto mismatch_pending_b =
			install_pending(mismatch,
							writer_request(mismatch_binding, mismatch_connection_b, 9, 2, 9, 0, 0),
							mismatch_epoch_b,
							mapping(0, &different_page, 4096U),
							sqlite_shm_writer_extend_pair::zero_zero,
							9);
		auto mismatch_holder = promote(mismatch, mismatch_pending_a, mismatch_gate_a);
		auto rejected = mismatch.promote_writer(mismatch_pending_b, mismatch_gate_b);
		require(!rejected &&
					rejected.error().reason == sqlite_shm_lease_rejection_reason::mapping_mismatch,
				"later first-writer pointer mismatch cannot join");
		cleanup_writer(mismatch, mismatch_pending_b, callback(9, 110));
		retire_last(mismatch, mismatch_holder, callback(9, 100));
		require(mismatch.revoke_writer_eligibility(mismatch_gate_a).has_value() &&
					mismatch.revoke_writer_eligibility(mismatch_gate_b).has_value(),
				"revoke mismatch gates");
	}

	void verify_last_release_and_writer_admission_race()
	{
		constexpr std::uint8_t marker = 48;
		const auto binding = family(marker);
		const auto connection_a = identity("test.connection", marker);
		const auto connection_b = identity("test.connection", marker + 1U);
		const auto open_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto live =
			install_live_writer(coordinator, binding, connection_a, open_epoch, marker, &page);
		const auto writer =
			writer_request(binding, connection_b, marker + 1U, 2, marker + 1U, 0, 0);
		const auto release_callback = callback(1, 100);

		std::barrier start{3};
		std::optional<sqlite_shm_writer_release> released;
		std::optional<sqlite_shm_writer_map_inflight> admitted;
		std::optional<sqlite_shm_lease_rejection> rejected;
		std::exception_ptr release_error;
		std::exception_ptr admission_error;
		std::jthread release_thread{
			[&]
			{
				start.arrive_and_wait();
				try
				{
					auto result = coordinator.release_writer_holder(live.holder, release_callback);
					require(result.has_value(), "racing last holder release succeeds");
					released.emplace(std::move(*result));
				}
				catch (...)
				{
					release_error = std::current_exception();
				}
			}};
		std::jthread admission_thread{[&]
									  {
										  start.arrive_and_wait();
										  try
										  {
											  auto result = coordinator.begin_writer_map(writer);
											  if (result)
												  admitted.emplace(std::move(*result));
											  else
												  rejected = result.error();
										  }
										  catch (...)
										  {
											  admission_error = std::current_exception();
										  }
									  }};
		start.arrive_and_wait();
		release_thread.join();
		admission_thread.join();
		if (release_error)
			std::rethrow_exception(release_error);
		if (admission_error)
			std::rethrow_exception(admission_error);
		require(released.has_value() && (admitted.has_value() != rejected.has_value()),
				"release/admission race has one total order");

		if (admitted)
		{
			require(released->decision() ==
						sqlite_shm_writer_retirement_decision::wait_for_inflight,
					"writer admission winning the mutex becomes a retirement blocker");
			require(coordinator.resolve_writer_map_failure(*admitted).has_value(),
					"racing writer native failure releases its pin");
			auto ready = coordinator.poll_writer_retirement(released->cleanup(), release_callback);
			require(ready && ready->decision == sqlite_shm_writer_retirement_decision::ready,
					"retirement becomes admitted after the racing writer drains");
		}
		else
		{
			require(released->decision() == sqlite_shm_writer_retirement_decision::ready &&
						rejected->reason == sqlite_shm_lease_rejection_reason::retiring,
					"last release winning the mutex rejects later writer predelegation");
		}
		require(coordinator
					.complete_writer_cleanup(released->cleanup(),
											 release_callback,
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"racing last writer performs cleanup only after exact admission");
		require(coordinator.revoke_writer_eligibility(live.eligibility).has_value(),
				"revoke release/admission race gate");
	}

	void verify_concurrent_holder_release_orders_cleanup()
	{
		constexpr std::uint8_t marker = 49;
		const auto binding = family(marker);
		const auto connection_a = identity("test.connection", marker);
		const auto connection_b = identity("test.connection", marker + 1U);
		const auto epoch_a = identity("test.open-epoch", marker);
		const auto epoch_b = identity("test.open-epoch", marker + 1U);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		const auto mapped = mapping(0, &page, 4096U);
		auto gate_a = install_eligibility(coordinator, binding, connection_a, epoch_a, marker);
		auto gate_b = install_eligibility(coordinator, binding, connection_b, epoch_b, marker + 1U);
		auto pending_a =
			install_pending(coordinator,
							writer_request(binding, connection_a, marker, 1, marker, 0, 1),
							epoch_a,
							mapped,
							sqlite_shm_writer_extend_pair::one_one,
							marker);
		auto pending_b = install_pending(
			coordinator,
			writer_request(binding, connection_b, marker + 1U, 2, marker + 1U, 0, 0),
			epoch_b,
			mapped,
			sqlite_shm_writer_extend_pair::zero_zero,
			marker + 1U);
		auto holder_a = promote(coordinator, pending_a, gate_a);
		auto holder_b = promote(coordinator, pending_b, gate_b);
		const auto callback_a = callback(3, 100);
		const auto callback_b = callback(4, 101);
		std::barrier start{3};
		std::optional<sqlite_shm_writer_release> release_a;
		std::optional<sqlite_shm_writer_release> release_b;
		std::exception_ptr error_a;
		std::exception_ptr error_b;
		auto release_concurrently =
			[&coordinator, &start](sqlite_shm_writer_holder& holder,
								   const sqlite_shm_callback_execution_receipt& release_callback,
								   std::optional<sqlite_shm_writer_release>& output,
								   std::exception_ptr& error)
		{
			start.arrive_and_wait();
			try
			{
				auto released = coordinator.release_writer_holder(holder, release_callback);
				require(released.has_value(), "concurrent holder release succeeds");
				output.emplace(std::move(*released));
			}
			catch (...)
			{
				error = std::current_exception();
			}
		};
		std::jthread thread_a{release_concurrently,
							  std::ref(holder_a),
							  std::cref(callback_a),
							  std::ref(release_a),
							  std::ref(error_a)};
		std::jthread thread_b{release_concurrently,
							  std::ref(holder_b),
							  std::cref(callback_b),
							  std::ref(release_b),
							  std::ref(error_b)};
		start.arrive_and_wait();
		thread_a.join();
		thread_b.join();
		if (error_a)
			std::rethrow_exception(error_a);
		if (error_b)
			std::rethrow_exception(error_b);
		require(release_a && release_b,
				"both concurrent holder releases retain cleanup obligations");

		auto* nonlast =
			release_a->decision() == sqlite_shm_writer_retirement_decision::not_last_attachment
			? &*release_a
			: &*release_b;
		auto* last = nonlast == &*release_a ? &*release_b : &*release_a;
		const auto& nonlast_callback = nonlast == &*release_a ? callback_a : callback_b;
		const auto& last_callback = last == &*release_a ? callback_a : callback_b;
		require(last->decision() == sqlite_shm_writer_retirement_decision::wait_for_inflight,
				"last concurrent release waits for the admitted non-last cleanup");
		require(coordinator
					.complete_writer_cleanup(nonlast->cleanup(),
											 nonlast_callback,
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"non-last concurrent cleanup drains first");
		auto ready = coordinator.poll_writer_retirement(last->cleanup(), last_callback);
		require(ready && ready->decision == sqlite_shm_writer_retirement_decision::ready,
				"last cleanup becomes admitted after the other holder drains");
		require(coordinator
					.complete_writer_cleanup(last->cleanup(),
											 last_callback,
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"last concurrent holder retires the generation");
		require(coordinator.revoke_writer_eligibility(gate_a).has_value() &&
					coordinator.revoke_writer_eligibility(gate_b).has_value(),
				"revoke concurrent holder gates");
	}

	void verify_writer_inflight_blocks_last_holder_retirement()
	{
		const auto binding = family(10);
		const auto connection_a = identity("test.connection", 10);
		const auto connection_b = identity("test.connection", 11);
		const auto epoch_a = identity("test.open-epoch", 10);
		const auto epoch_b = identity("test.open-epoch", 11);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		const auto mapped = mapping(0, &page, 4096U);
		auto gate_a = install_eligibility(coordinator, binding, connection_a, epoch_a, 10);
		auto pending_a = install_pending(coordinator,
										 writer_request(binding, connection_a, 10, 1, 10, 0, 1),
										 epoch_a,
										 mapped,
										 sqlite_shm_writer_extend_pair::one_one,
										 10);
		auto holder_a = promote(coordinator, pending_a, gate_a);

		const auto request_b = writer_request(binding, connection_b, 11, 2, 11, 0, 0);
		auto inflight_b_result = coordinator.begin_writer_map(request_b);
		require(inflight_b_result.has_value(), "W2 acquires predelegate in-flight pin");
		auto inflight_b = std::move(*inflight_b_result);
		const auto retirement_callback = callback(1, 100);
		auto retirement = coordinator.release_writer_holder(holder_a, retirement_callback);
		require(retirement &&
					retirement->decision() ==
						sqlite_shm_writer_retirement_decision::wait_for_inflight &&
					!coordinator.snapshot().reader_admission_visible,
				"W1 retirement hides admission and waits only for W2 resolution");
		auto post_native_b = record_native_mapping(coordinator, inflight_b, mapped.native_mapping);
		auto pending_b_result = coordinator.install_pending(
			post_native_b,
			writer_receipt(
				request_b, epoch_b, mapped, sqlite_shm_writer_extend_pair::zero_zero, 11));
		require(pending_b_result.has_value(), "pre-admitted W2 can finish its native receipt");
		auto pending_b = std::move(*pending_b_result);
		auto gate_b = install_eligibility(coordinator, binding, connection_b, epoch_b, 11);
		auto late_join = coordinator.promote_writer(pending_b, gate_b);
		require(!late_join &&
					late_join.error().reason == sqlite_shm_lease_rejection_reason::retiring,
				"last-holder retirement wins total order and generation never revives");
		cleanup_writer(coordinator, pending_b, request_b.callback);
		auto ready = coordinator.poll_writer_retirement(retirement->cleanup(), retirement_callback);
		require(ready && ready->decision == sqlite_shm_writer_retirement_decision::ready,
				"retirement becomes ready after W2 resolution");
		require(coordinator
					.complete_writer_cleanup(retirement->cleanup(),
											 retirement_callback,
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"complete W1 retirement");
		require(coordinator.revoke_writer_eligibility(gate_a).has_value() &&
					coordinator.revoke_writer_eligibility(gate_b).has_value(),
				"revoke retirement gates");
	}

	void verify_waiting_retirement_rejects_early_native_completion()
	{
		constexpr std::uint8_t marker = 50;
		const auto binding = family(marker);
		const auto connection_a = identity("test.connection", marker);
		const auto connection_b = identity("test.connection", marker + 1U);
		const auto open_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto live =
			install_live_writer(coordinator, binding, connection_a, open_epoch, marker, &page);
		auto blocker = coordinator.begin_writer_map(
			writer_request(binding, connection_b, marker + 1U, 2, marker + 1U, 0, 0));
		require(blocker.has_value(), "early-completion fixture has a writer blocker");
		const auto release_callback = callback(1, 100);
		auto released = coordinator.release_writer_holder(live.holder, release_callback);
		require(released &&
					released->decision() ==
						sqlite_shm_writer_retirement_decision::wait_for_inflight,
				"last holder is waiting before native cleanup admission");
		auto reordered = coordinator.complete_writer_cleanup(
			released->cleanup(),
			release_callback,
			sqlite_shm_native_cleanup_outcome::confirmed_success);
		const auto snapshot = coordinator.snapshot();
		require(!reordered && !released->cleanup().valid() && snapshot.quarantined &&
					snapshot.writer_cleanup_count >= 1U,
				"native completion before readiness becomes a terminal quarantine tombstone");
	}

	void verify_retirement_wait_failure_quarantines_without_retry()
	{
		std::uint8_t marker = 33;
		for (const auto failure : {sqlite_shm_retirement_wait_failure::timeout,
								   sqlite_shm_retirement_wait_failure::unknown})
		{
			const auto binding = family(marker);
			const auto connection_a = identity("test.connection", marker);
			const auto connection_b =
				identity("test.connection", static_cast<std::uint8_t>(marker + 40U));
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			const auto mapped = mapping(0, &page, 4096U);
			auto gate = install_eligibility(coordinator, binding, connection_a, open_epoch, marker);
			auto pending =
				install_pending(coordinator,
								writer_request(binding, connection_a, marker, 1, marker, 0, 1),
								open_epoch,
								mapped,
								sqlite_shm_writer_extend_pair::one_one,
								marker);
			auto holder = promote(coordinator, pending, gate);
			auto blocker =
				coordinator.begin_writer_map(writer_request(binding,
															connection_b,
															static_cast<std::uint8_t>(marker + 40U),
															2,
															static_cast<std::uint8_t>(marker + 40U),
															0,
															0));
			require(blocker.has_value(), "writer pin blocks retirement before timeout");

			const auto retirement_callback = callback(1, 120);
			auto release = coordinator.release_writer_holder(holder, retirement_callback);
			require(release &&
						release->decision() ==
							sqlite_shm_writer_retirement_decision::wait_for_inflight &&
						release->cleanup().valid() &&
						coordinator.snapshot().writer_cleanup_count == 1U,
					"last writer release retains cleanup while bounded wait is active");
			auto failed = coordinator.fail_writer_retirement_wait(
				release->cleanup(), retirement_callback, failure);
			require(!failed && coordinator.snapshot().quarantined &&
						!coordinator.snapshot().reader_admission_visible,
					"timeout or unknown retirement wait permanently quarantines");
			auto no_retry =
				coordinator.poll_writer_retirement(release->cleanup(), retirement_callback);
			require(no_retry &&
						no_retry->decision == sqlite_shm_writer_retirement_decision::quarantined,
					"retirement wait failure cannot later revive or retry");
			++marker;
		}
	}

	void verify_quarantined_retirement_drains_without_revival()
	{
		{
			constexpr std::uint8_t marker = 63;
			const auto binding = family(marker);
			const auto holder_connection = identity("test.connection", marker);
			const auto blocker_connection = identity("test.connection", marker + 1U);
			const auto bad_connection = identity("test.connection", marker + 2U);
			const auto holder_epoch = identity("test.open-epoch", marker);
			const auto blocker_epoch = identity("test.open-epoch", marker + 1U);
			const auto bad_epoch = identity("test.open-epoch", marker + 2U);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			int mismatched_page{};
			auto live = install_live_writer(
				coordinator, binding, holder_connection, holder_epoch, marker, &page);

			const auto blocker_request =
				writer_request(binding, blocker_connection, marker + 1U, 2, marker + 1U, 0, 1);
			auto blocker_begun = coordinator.begin_writer_map(blocker_request);
			require(blocker_begun.has_value(), "begin quarantined-retirement blocker");
			auto blocker_inflight = std::move(*blocker_begun);
			auto blocker = record_native_mapping(coordinator, blocker_inflight, &mismatched_page);
			auto blocker_rejected =
				coordinator.install_pending(blocker,
											writer_receipt(blocker_request,
														   blocker_epoch,
														   mapping(1, &mismatched_page, 8192U),
														   sqlite_shm_writer_extend_pair::one_one,
														   marker + 1U));
			require(!blocker_rejected && blocker.valid(),
					"retain post-native blocker for mandatory cleanup");

			const auto bad_request =
				writer_request(binding, bad_connection, marker + 2U, 3, marker + 2U, 0, 1);
			auto bad_pending = install_pending(coordinator,
											   bad_request,
											   bad_epoch,
											   mapping(0, &page, 4096U),
											   sqlite_shm_writer_extend_pair::one_one,
											   marker + 2U);
			auto bad = coordinator.begin_writer_cleanup(bad_pending,
														sqlite_shm_callback_execution_receipt{});
			require(!bad && coordinator.snapshot().quarantined,
					"independent bad cleanup admission quarantines the family");

			const auto release_callback = callback(9, 100);
			auto release = coordinator.release_writer_holder(live.holder, release_callback);
			require(release &&
						release->decision() ==
							sqlite_shm_writer_retirement_decision::wait_for_inflight &&
						!live.holder.valid(),
					"quarantined last holder waits for an existing post-native blocker");
			auto blocker_drain =
				coordinator.begin_writer_cleanup(blocker, blocker_request.callback);
			require(blocker_drain.has_value() && !blocker.valid(),
					"post-native blocker retains its first mandatory drain");
			auto blocker_completed = coordinator.complete_writer_cleanup(
				*blocker_drain,
				blocker_request.callback,
				sqlite_shm_native_cleanup_outcome::confirmed_success);
			require(!blocker_completed && !blocker_drain->valid(),
					"blocker drain under quarantine becomes terminal");

			auto ready = coordinator.poll_writer_retirement(release->cleanup(), release_callback);
			require(ready && ready->decision == sqlite_shm_writer_retirement_decision::ready,
					"quarantined generation becomes cleanup-ready after terminal blocker drain");
			auto completed = coordinator.complete_writer_cleanup(
				release->cleanup(),
				release_callback,
				sqlite_shm_native_cleanup_outcome::confirmed_success);
			require(!completed && !release->cleanup().valid() && coordinator.snapshot().quarantined,
					"ready quarantine drain cannot revive the generation");
			require(coordinator.revoke_writer_eligibility(live.eligibility).has_value(),
					"revoke quarantined-retirement eligibility");
		}

		{
			constexpr std::uint8_t marker = 64;
			const auto binding = family(marker);
			const auto holder_connection = identity("test.connection", marker);
			const auto blocker_connection = identity("test.connection", marker + 1U);
			const auto holder_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto live = install_live_writer(
				coordinator, binding, holder_connection, holder_epoch, marker, &page);
			auto blocker = coordinator.begin_writer_map(
				writer_request(binding, blocker_connection, marker + 1U, 2, marker + 1U, 0, 0));
			require(blocker.has_value(), "acquire wrong-poll retirement blocker");

			const auto release_callback = callback(9, 110);
			auto release = coordinator.release_writer_holder(live.holder, release_callback);
			require(release &&
						release->decision() ==
							sqlite_shm_writer_retirement_decision::wait_for_inflight,
					"wrong-poll fixture enters bounded retirement wait");
			auto wrong = coordinator.poll_writer_retirement(release->cleanup(), callback(9, 111));
			require(!wrong && coordinator.snapshot().quarantined,
					"wrong retirement callback terminalizes the exact holder");
			require(coordinator.resolve_writer_map_failure(*blocker).has_value(),
					"drain wrong-poll fixture blocker");
			auto no_retry =
				coordinator.poll_writer_retirement(release->cleanup(), release_callback);
			require(no_retry &&
						no_retry->decision == sqlite_shm_writer_retirement_decision::quarantined,
					"correct callback cannot revive a terminalized retirement poll");
			require(coordinator.revoke_writer_eligibility(live.eligibility).has_value(),
					"revoke wrong-poll eligibility");
		}
	}

	void verify_reader_reservation_terminal_requires_fresh_attachment_epoch()
	{
		constexpr std::uint8_t marker = 116;
		const auto binding = family(marker);
		const auto writer_connection = identity("test.connection", marker);
		const auto reader_connection = identity("test.connection", marker + 1U);
		const auto writer_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto writer = install_live_writer(
			coordinator, binding, writer_connection, writer_epoch, marker, &page);
		const auto generation = writer.holder.generation();
		auto map_request = reader_attachment_request(
			binding, reader_connection, marker + 1U, 2, 20, 0, generation);
		const auto first_request = reader_session_request(map_request, 20);
		auto first_result = coordinator.begin_reader_session(first_request);
		require(first_result &&
					first_result->phase() ==
						sqlite_shm_reader_session_phase::reserved_for_first_map,
				"no-map fixture reserves one expected attachment epoch");
		auto first = std::move(*first_result);
		auto no_pointer_failure = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
			first_request,
			sqlite_shm_reader_session_terminal_kind::failure,
			identity("test.reader-session-terminal", 20));
		require(coordinator.complete_reader_session(first, no_pointer_failure) && !first.valid(),
				"reserved no-pointer failure consumes the session exactly once");

		auto same_epoch_request = reader_session_request(map_request, 21);
		auto same_epoch = coordinator.begin_reader_session(same_epoch_request);
		require(!same_epoch &&
					same_epoch.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
					!coordinator.snapshot().quarantined,
				"reserved terminal tombstone revokes the expected attachment epoch");

		auto fresh_map_request =
			reader_attachment_request(binding,
									  reader_connection,
									  marker + 1U,
									  2,
									  21,
									  0,
									  generation,
									  identity("test.reader-attachment-epoch", 99));
		const auto fresh_request = reader_session_request(fresh_map_request, 22);
		auto fresh_result = coordinator.begin_reader_session(fresh_request);
		require(fresh_result &&
					fresh_result->phase() ==
						sqlite_shm_reader_session_phase::reserved_for_first_map,
				"fresh non-reusable attachment epoch may form a new reservation");
		auto fresh = std::move(*fresh_result);
		auto no_wal_success = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
			fresh_request,
			sqlite_shm_reader_session_terminal_kind::success,
			identity("test.reader-session-terminal", 21));
		require(coordinator.complete_reader_session(fresh, no_wal_success) &&
					coordinator.snapshot().reader_session_terminal_count == 2U,
				"reserved no-WAL success is another closed no-pointer terminal route");
		retire_last(coordinator, writer.holder, callback(3, 22));
		require(coordinator.revoke_writer_eligibility(writer.eligibility).has_value(),
				"revoke reservation terminal fixture writer gate");
	}

	void verify_reader_terminal_kind_is_closed_and_fail_closed()
	{
		constexpr std::uint8_t marker = 115;
		const auto binding = family(marker);
		const auto writer_connection = identity("test.connection", marker);
		const auto reader_connection = identity("test.connection", marker + 1U);
		const auto writer_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto writer = install_live_writer(
			coordinator, binding, writer_connection, writer_epoch, marker, &page);
		auto map_request = reader_attachment_request(
			binding, reader_connection, marker + 1U, 2, 19, 0, writer.holder.generation());
		const auto request = reader_session_request(map_request, 19);
		auto session_result = coordinator.begin_reader_session(request);
		require(session_result.has_value(), "invalid-kind fixture reserves a session");
		auto session = std::move(*session_result);
		auto invalid = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
			request,
			static_cast<sqlite_shm_reader_session_terminal_kind>(0xffU),
			identity("test.reader-session-terminal", 19));
		auto rejected = coordinator.complete_reader_session(session, invalid);
		require(!rejected && !session.valid() && coordinator.snapshot().quarantined &&
					coordinator.snapshot().reader_session_terminal_count == 0U,
				"unknown terminal kind consumes no terminal tombstone and quarantines");
	}

	void verify_reader_predecessor_first_map_transfers_without_proposal_authority()
	{
		struct row
		{
			sqlite_shm_reader_predecessor_map_kind kind;
			int native_status;
			bool mapped;
		};
		const std::array rows{
			row{sqlite_shm_reader_predecessor_map_kind::exact_predecessor_no_attachment_route,
				sqlite_readonly_status,
				false},
			row{sqlite_shm_reader_predecessor_map_kind::exact_predecessor_no_attachment_route,
				sqlite_readonly_cantinit_status,
				false},
			row{sqlite_shm_reader_predecessor_map_kind::exact_predecessor_mapped_route,
				sqlite_readonly_status,
				true},
		};

		for (std::size_t index = 0U; index < std::size(rows); ++index)
		{
			const auto marker = static_cast<std::uint8_t>(210U + index * 8U);
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto map_request = reader_attachment_request(
				binding,
				identity("test.connection", static_cast<std::uint8_t>(marker + 1U)),
				static_cast<std::uint8_t>(marker + 1U),
				2,
				static_cast<std::uint8_t>(marker + 2U),
				0,
				writer.holder.generation());
			const auto session_request =
				reader_session_request(map_request, static_cast<std::uint8_t>(marker + 2U));
			auto session = coordinator.begin_reader_session(session_request);
			require(session && session->valid(), "predecessor row reserves one first session");
			auto inflight = coordinator.begin_reader_map(*session, map_request);
			require(inflight && inflight->valid(), "predecessor row starts one exact map attempt");
			const auto& current = rows.at(index);
			const auto effect = identity("test.reader-predecessor-native-effect",
										 static_cast<std::uint8_t>(marker + 3U));
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_predecessor_map(
				*inflight,
				current.kind,
				map_request,
				current.native_status,
				current.mapped ? &page : nullptr,
				effect);
			auto completed =
				coordinator.complete_reader_predecessor_map(*inflight, receipt, *session);
			const auto snapshot = coordinator.snapshot();
			const auto lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				completed && completed->kind() == current.kind &&
					completed->native_status() ==
						(current.mapped ? sqlite_readonly_status
										: sqlite_readonly_cantinit_status) &&
					completed->native_mapping() == (current.mapped ? &page : nullptr) &&
					!inflight->valid() && !session->valid() &&
					snapshot.reader_inflight_count == 0U &&
					snapshot.reader_attachment_group_count == 0U &&
					snapshot.reader_attachment_live_member_count == 0U &&
					snapshot.reader_attachment_audit_count == 0U &&
					snapshot.reader_session_reservation_count == 0U &&
					snapshot.reader_session_owner_count == 0U &&
					snapshot.reader_handoff_count == 0U &&
					snapshot.reader_predecessor_map_terminal_count == 1U &&
					snapshot.reader_predecessor_route_active_count == 1U &&
					lifecycle.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(lifecycle) &&
					lifecycle.attachment_reservations.size() == 1U &&
					lifecycle.attachment_reservations.front().phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::
							predecessor_route_active &&
					lifecycle.session_reservations.size() == 1U &&
					lifecycle.session_reservations.front().phase ==
						detail::sqlite_shm_reader_session_reservation_phase::
							transferred_to_existing_predecessor &&
					lifecycle.predecessor_map_terminals.size() == 1U &&
					lifecycle.predecessor_map_terminals.front().kind == current.kind &&
					lifecycle.predecessor_map_terminals.front().native_effect_receipt == effect &&
					lifecycle.predecessor_map_terminals.front().observed_attachment_retained ==
						current.mapped &&
					lifecycle.predecessor_map_terminals.front().exact_terminal_receipt_retained &&
					!snapshot.quarantined,
				"first predecessor result transfers the session and native evidence without "
				"proposal group/member/handoff/owner authority");

			auto replay = coordinator.complete_reader_predecessor_map(*inflight, receipt, *session);
			auto same_epoch = coordinator.begin_reader_session(session_request);
			require(!replay &&
						replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
						!same_epoch &&
						same_epoch.error().reason ==
							sqlite_shm_lease_rejection_reason::stale_token &&
						coordinator.snapshot().reader_predecessor_map_terminal_count == 1U &&
						!coordinator.snapshot().quarantined,
					"predecessor receipt cannot replay and its active route blocks proposal remap");

			const auto unmap_effect = identity("test.reader-predecessor-unmap-effect",
											   static_cast<std::uint8_t>(marker + 4U));
			const auto unmap_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_predecessor_unmap(
					*completed,
					callback(4, static_cast<std::uint8_t>(marker + 5U)),
					sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
					sqlite_ok_status,
					unmap_effect);
			auto retired = coordinator.complete_reader_predecessor_unmap(unmap_receipt);
			const auto retired_snapshot = coordinator.snapshot();
			const auto retired_lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				retired &&
					retired->kind() == sqlite_shm_reader_unmap_terminal_kind::retired_confirmed &&
					retired->evidence_kind() ==
						sqlite_shm_reader_unmap_evidence_kind::exact_native_result &&
					retired->native_status() == sqlite_ok_status &&
					retired->outward_status() == sqlite_ok_status &&
					retired->native_effect_receipt() == unmap_effect &&
					retired_snapshot.reader_predecessor_route_active_count == 0U &&
					retired_snapshot.reader_predecessor_route_retired_count == 1U &&
					retired_lifecycle.attachment_reservations.size() == 1U &&
					retired_lifecycle.attachment_reservations.front().phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::
							predecessor_route_retired_confirmed &&
					retired_lifecycle.predecessor_map_terminals.size() == 1U &&
					retired_lifecycle.predecessor_map_terminals.front()
							.retirement_terminal_sequence != 0U &&
					retired_lifecycle.predecessor_map_terminals.front().retirement_callback ==
						unmap_receipt.callback() &&
					retired_lifecycle.predecessor_map_terminals.front()
							.retirement_native_effect_receipt == unmap_effect &&
					!retired_snapshot.quarantined,
				"exact existing-route unmap retires the predecessor reservation without proposal "
				"cleanup authority");
			auto unmap_replay = coordinator.complete_reader_predecessor_unmap(unmap_receipt);
			require(!unmap_replay &&
						unmap_replay.error().reason ==
							sqlite_shm_lease_rejection_reason::stale_token &&
						!coordinator.snapshot().quarantined,
					"confirmed predecessor unmap receipt is one-shot and replay-stale");

			retire_last(
				coordinator, writer.holder, callback(3, static_cast<std::uint8_t>(marker + 6U)));
			require(coordinator.revoke_writer_eligibility(writer.eligibility).has_value(),
					"revoke predecessor transfer fixture writer gate");
		}
	}

	void verify_reader_predecessor_receipt_partition_is_fail_closed()
	{
		struct invalid_row
		{
			sqlite_shm_reader_predecessor_map_kind kind;
			int native_status;
			bool mapped;
			int delegated_extend;
			bool valid_effect;
		};
		const std::array rows{
			invalid_row{
				sqlite_shm_reader_predecessor_map_kind::exact_predecessor_no_attachment_route,
				sqlite_busy_status,
				false,
				0,
				true},
			invalid_row{
				sqlite_shm_reader_predecessor_map_kind::exact_predecessor_no_attachment_route,
				sqlite_readonly_status,
				true,
				0,
				true},
			invalid_row{sqlite_shm_reader_predecessor_map_kind::exact_predecessor_mapped_route,
						sqlite_readonly_status,
						false,
						0,
						true},
			invalid_row{sqlite_shm_reader_predecessor_map_kind::exact_predecessor_mapped_route,
						sqlite_readonly_cantinit_status,
						true,
						0,
						true},
			invalid_row{sqlite_shm_reader_predecessor_map_kind::exact_predecessor_mapped_route,
						sqlite_readonly_status,
						true,
						1,
						true},
			invalid_row{static_cast<sqlite_shm_reader_predecessor_map_kind>(0xffU),
						sqlite_readonly_status,
						false,
						0,
						true},
			invalid_row{
				sqlite_shm_reader_predecessor_map_kind::exact_predecessor_no_attachment_route,
				sqlite_readonly_status,
				false,
				0,
				false},
		};

		for (std::size_t index = 0U; index < std::size(rows); ++index)
		{
			const auto marker = static_cast<std::uint8_t>(150U + index * 8U);
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto map_request = reader_attachment_request(
				binding,
				identity("test.connection", static_cast<std::uint8_t>(marker + 1U)),
				static_cast<std::uint8_t>(marker + 1U),
				2,
				static_cast<std::uint8_t>(marker + 2U),
				0,
				writer.holder.generation());
			const auto session_request =
				reader_session_request(map_request, static_cast<std::uint8_t>(marker + 2U));
			auto session = coordinator.begin_reader_session(session_request);
			require(session.has_value(), "invalid predecessor fixture reserves session");
			auto inflight = coordinator.begin_reader_map(*session, map_request);
			require(inflight.has_value(), "invalid predecessor fixture begins map");
			const auto& current = rows.at(index);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_predecessor_map(
				*inflight,
				current.kind,
				map_request,
				current.native_status,
				current.mapped ? &page : nullptr,
				current.valid_effect ? identity("test.reader-invalid-predecessor-effect",
												static_cast<std::uint8_t>(marker + 3U))
									 : sqlite_backend_opaque_identity{},
				current.delegated_extend);
			auto rejected =
				coordinator.complete_reader_predecessor_map(*inflight, receipt, *session);
			const auto snapshot = coordinator.snapshot();
			const auto lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(!rejected && !inflight->valid() && !session->valid() && snapshot.quarantined &&
						snapshot.reader_predecessor_map_terminal_count == 0U &&
						snapshot.reader_predecessor_route_active_count == 0U &&
						lifecycle.predecessor_map_terminals.empty() &&
						lifecycle.attachment_reservations.size() == 1U &&
						lifecycle.attachment_reservations.front().phase ==
							detail::sqlite_shm_reader_attachment_reservation_phase::
								terminal_quarantined,
					"invalid predecessor status/pointer/effect shape terminalizes without route "
					"publication");
		}
	}

	void verify_reader_predecessor_unmap_terminal_partition_is_fail_closed()
	{
		struct row
		{
			sqlite_shm_reader_unmap_evidence_kind evidence_kind;
			std::optional<int> native_status;
			bool native_effect;
			int caller_delete_flag;
			bool inject_commit_failure;
			bool exact_terminal_receipt;
			int expected_outward_status;
		};
		const std::array rows{
			row{sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
				sqlite_busy_status,
				true,
				0,
				false,
				true,
				sqlite_busy_status},
			row{sqlite_shm_reader_unmap_evidence_kind::throw_or_unknown,
				std::nullopt,
				false,
				0,
				false,
				true,
				sqlite_ioerr_status},
			row{sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
				sqlite_ok_status,
				false,
				0,
				false,
				false,
				sqlite_ioerr_status},
			row{sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
				sqlite_ok_status,
				true,
				1,
				false,
				false,
				sqlite_ioerr_status},
			row{sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
				sqlite_ok_status,
				true,
				0,
				true,
				true,
				sqlite_ioerr_status},
		};

		for (std::size_t index = 0U; index < std::size(rows); ++index)
		{
			const auto marker = static_cast<std::uint8_t>(70U + index * 10U);
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto map_request = reader_attachment_request(
				binding,
				identity("test.connection", static_cast<std::uint8_t>(marker + 1U)),
				static_cast<std::uint8_t>(marker + 1U),
				2,
				static_cast<std::uint8_t>(marker + 2U),
				0,
				writer.holder.generation());
			const auto session_request =
				reader_session_request(map_request, static_cast<std::uint8_t>(marker + 2U));
			auto session = coordinator.begin_reader_session(session_request);
			require(session.has_value(), "predecessor unmap fixture reserves session");
			auto inflight = coordinator.begin_reader_map(*session, map_request);
			require(inflight.has_value(), "predecessor unmap fixture begins map");
			const auto map_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_predecessor_map(
					*inflight,
					sqlite_shm_reader_predecessor_map_kind::exact_predecessor_no_attachment_route,
					map_request,
					sqlite_readonly_cantinit_status,
					nullptr,
					identity("test.reader-predecessor-map-effect",
							 static_cast<std::uint8_t>(marker + 3U)));
			auto predecessor =
				coordinator.complete_reader_predecessor_map(*inflight, map_receipt, *session);
			require(predecessor.has_value(), "predecessor unmap fixture transfers map");

			const auto& current = rows.at(index);
			const auto terminal_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_predecessor_unmap(
					*predecessor,
					callback(5, static_cast<std::uint8_t>(marker + 4U)),
					current.evidence_kind,
					current.native_status,
					current.native_effect ? std::optional<sqlite_backend_opaque_identity>{identity(
												"test.reader-predecessor-unmap-terminal-effect",
												static_cast<std::uint8_t>(marker + 5U))}
										  : std::nullopt,
					current.caller_delete_flag);
			if (current.inject_commit_failure)
				sqlite_same_process_shm_lease_test_peer::fail_next_reader_unmap_terminal_commit(
					coordinator);
			auto terminal = coordinator.complete_reader_predecessor_unmap(terminal_receipt);
			const auto snapshot = coordinator.snapshot();
			const auto lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				(terminal.has_value() == current.exact_terminal_receipt) &&
					(!terminal ||
					 (terminal->kind() ==
						  sqlite_shm_reader_unmap_terminal_kind::terminal_quarantined &&
					  terminal->outward_status() == current.expected_outward_status)) &&
					snapshot.quarantined && snapshot.reader_predecessor_route_active_count == 0U &&
					snapshot.reader_predecessor_route_retired_count == 0U &&
					snapshot.reader_predecessor_map_terminal_count == 1U &&
					lifecycle.attachment_reservations.size() == 1U &&
					lifecycle.attachment_reservations.front().phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::
							terminal_quarantined &&
					lifecycle.predecessor_map_terminals.size() == 1U &&
					(lifecycle.predecessor_map_terminals.front().retirement_callback.has_value() ==
					 current.exact_terminal_receipt),
				"predecessor unmap non-OK, unknown, malformed, and commit-failure rows never "
				"publish retirement success");
		}
	}

	void verify_reader_zero_attachment_status_partition()
	{
		struct accepted_row
		{
			sqlite_shm_reader_attachment_zero_effect_kind kind;
			int native_status;
			bool native_mapping;
			int projected_status;
		};
		const accepted_row accepted_rows[]{
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
			 sqlite_busy_status,
			 false,
			 sqlite_busy_status},
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
			 sqlite_ioerr_read_status,
			 false,
			 sqlite_ioerr_read_status},
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_protocol_invalid_no_attachment,
			 sqlite_ok_status,
			 false,
			 sqlite_ioerr_status},
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_protocol_invalid_no_attachment,
			 sqlite_readonly_unsupported_extended_status,
			 false,
			 sqlite_ioerr_status},
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_protocol_invalid_no_attachment,
			 sqlite_readonly_cantinit_status,
			 true,
			 sqlite_ioerr_status},
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_protocol_invalid_no_attachment,
			 sqlite_busy_status,
			 true,
			 sqlite_ioerr_status},
		};

		for (std::size_t index = 0; index < std::size(accepted_rows); ++index)
		{
			const auto marker = static_cast<std::uint8_t>(130U + index * 2U);
			const auto binding = family(marker);
			const auto writer_connection = identity("test.connection", marker);
			const auto reader_connection = identity("test.connection", marker + 1U);
			const auto writer_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(
				coordinator, binding, writer_connection, writer_epoch, marker, &page);
			const auto map_request = reader_attachment_request(binding,
															   reader_connection,
															   marker + 1U,
															   2,
															   marker + 1U,
															   0,
															   writer.holder.generation());
			const auto session_request = reader_session_request(map_request, marker + 1U);
			auto session_result = coordinator.begin_reader_session(session_request);
			require(session_result.has_value(),
					"zero-attachment accepted row reserves a first session");
			auto session = std::move(*session_result);
			const auto reserved_view =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto& reserved_attachment = only_attachment_reservation(reserved_view);
			const auto& reserved_session = only_session_reservation(reserved_view);
			require(
				reserved_view.sequence_source_identity != nullptr &&
					reserved_view.last_committed_sequence != 0U &&
					reserved_view.last_issued_sequence == reserved_view.last_committed_sequence &&
					reader_event_sequences_are_dense(reserved_view) &&
					reserved_view.outstanding_terminal_permit_count == 1U &&
					reader_terminal_permit_slots_are_exact(reserved_view) &&
					reserved_attachment.phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::reserved &&
					reserved_attachment.origin_sequence == reserved_view.last_committed_sequence &&
					reserved_attachment.destination_sequence == 0U &&
					reserved_session.phase ==
						detail::sqlite_shm_reader_session_reservation_phase::
							reserved_before_sqlite &&
					reserved_session.origin_sequence == reserved_view.last_committed_sequence &&
					reserved_session.destination_sequence == 0U &&
					reserved_session.terminal_permit_slot != 0U &&
					count_reader_lifecycle_events(
						reserved_view,
						detail::sqlite_shm_reader_lifecycle_event_kind::session_start_admission) ==
						1U,
				"first session atomically binds reserved attachment/session phases to one "
				"lifecycle sequence");
			auto map_result = coordinator.begin_reader_map(session, map_request);
			require(map_result.has_value(),
					"zero-attachment accepted row begins its exact native attempt");
			auto inflight = std::move(*map_result);
			const auto admitted_view =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto& admitted_map = only_reader_map_attempt(admitted_view);
			require(
				admitted_view.last_committed_sequence > reserved_view.last_committed_sequence &&
					admitted_view.last_issued_sequence == admitted_view.last_committed_sequence &&
					reader_event_sequences_are_dense(admitted_view) &&
					admitted_view.outstanding_terminal_permit_count == 4U &&
					reader_terminal_permit_slots_are_exact(admitted_view) &&
					only_session_reservation(admitted_view).terminal_permit_slot ==
						reserved_session.terminal_permit_slot &&
					admitted_map.admission_sequence == admitted_view.last_committed_sequence &&
					admitted_map.terminal_permit_slot != 0U &&
					admitted_map.terminal_permit_slot != reserved_session.terminal_permit_slot &&
					admitted_map.potential_group_cut_permit_slot != 0U &&
					admitted_map.potential_group_terminal_permit_slot != 0U &&
					admitted_map.potential_group_cut_permit_slot !=
						admitted_map.potential_group_terminal_permit_slot &&
					admitted_map.potential_group_cut_permit_slot !=
						admitted_map.terminal_permit_slot &&
					admitted_map.potential_group_terminal_permit_slot !=
						admitted_map.terminal_permit_slot &&
					admitted_map.potential_group_cut_permit_slot !=
						reserved_session.terminal_permit_slot &&
					admitted_map.potential_group_terminal_permit_slot !=
						reserved_session.terminal_permit_slot &&
					count_reader_lifecycle_events(
						admitted_view,
						detail::sqlite_shm_reader_lifecycle_event_kind::map_admission) == 1U &&
					only_attachment_reservation(admitted_view).phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::reserved &&
					only_session_reservation(admitted_view).phase ==
						detail::sqlite_shm_reader_session_reservation_phase::reserved_before_sqlite,
				"map admission consumes a later sequence without changing reservation phases");
			const auto& row = accepted_rows[index];
			const auto receipt =
				sqlite_same_process_shm_lease_test_peer::reader_attachment_zero_effect(
					inflight,
					row.kind,
					map_request,
					row.native_status,
					row.native_mapping ? &page : nullptr,
					identity("test.reader-zero-attachment-effect", marker));
			const auto exhaust_with_pending_terminals = index + 1U == std::size(accepted_rows);
			if (exhaust_with_pending_terminals)
				sqlite_same_process_shm_lease_test_peer::exhaust_reader_lifecycle_sequences(
					coordinator);
			auto completed =
				coordinator.complete_reader_zero_attachment_map(inflight, receipt, session);
			const auto terminal = coordinator.snapshot();
			const auto terminal_view =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto& terminal_attachment = only_attachment_reservation(terminal_view);
			const auto& terminal_session = only_session_reservation(terminal_view);
			require(
				completed && completed->kind() == row.kind &&
					completed->native_status() == row.projected_status &&
					completed->native_mapping() == nullptr && !inflight.valid() &&
					!session.valid() && terminal.reader_inflight_count == 0U &&
					terminal.reader_attachment_group_count == 0U &&
					terminal.reader_attachment_live_member_count == 0U &&
					terminal.reader_attachment_audit_count == 0U &&
					terminal.reader_session_reservation_count == 0U &&
					terminal.reader_session_owner_count == 0U &&
					terminal.reader_handoff_count == 0U &&
					terminal.reader_attachment_zero_effect_terminal_count == 1U &&
					terminal.reader_attachment_revoked_no_map_count == 1U &&
					terminal_view.last_committed_sequence > admitted_view.last_committed_sequence &&
					terminal_view.last_issued_sequence == terminal_view.last_committed_sequence &&
					reader_event_sequences_are_dense(terminal_view) &&
					terminal_view.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(terminal_view) &&
					terminal_view.map_attempts.empty() &&
					terminal_attachment.phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::revoked_no_map &&
					terminal_attachment.origin_sequence == reserved_view.last_committed_sequence &&
					terminal_attachment.destination_sequence ==
						terminal_view.last_committed_sequence &&
					terminal_session.phase ==
						detail::sqlite_shm_reader_session_reservation_phase::consumed_no_pointer &&
					terminal_session.origin_sequence == reserved_view.last_committed_sequence &&
					terminal_session.destination_sequence ==
						terminal_view.last_committed_sequence &&
					terminal_view.attachment_reservation_phase_counts[enum_index(
						detail::sqlite_shm_reader_attachment_reservation_phase::revoked_no_map)] ==
						1U &&
					terminal_view.session_reservation_phase_counts[enum_index(
						detail::sqlite_shm_reader_session_reservation_phase::
							consumed_no_pointer)] == 1U &&
					count_reader_lifecycle_events(
						terminal_view,
						detail::sqlite_shm_reader_lifecycle_event_kind::map_terminal) == 1U &&
					last_reader_lifecycle_event_sequence(
						terminal_view,
						detail::sqlite_shm_reader_lifecycle_event_kind::map_terminal) ==
						last_reader_lifecycle_event_sequence(
							terminal_view,
							detail::sqlite_shm_reader_lifecycle_event_kind::use_session_terminal) &&
					(!exhaust_with_pending_terminals || terminal_view.sequence_source_exhausted) &&
					!terminal.quarantined,
				"each accepted complete-proof row publishes one revoked no-map terminal");

			if (index == 0U)
			{
				auto replay =
					coordinator.complete_reader_zero_attachment_map(inflight, receipt, session);
				require(
					!replay &&
						replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
						coordinator.snapshot().reader_attachment_zero_effect_terminal_count == 1U &&
						!coordinator.snapshot().quarantined,
					"zero-attachment completion and its receipt cannot replay");
				auto same_epoch = coordinator.begin_reader_session(session_request);
				require(!same_epoch &&
							same_epoch.error().reason ==
								sqlite_shm_lease_rejection_reason::stale_token &&
							!coordinator.snapshot().quarantined,
						"revoked no-map tombstone rejects the consumed attachment epoch");

				auto fresh_map_request = reader_attachment_request(
					binding,
					reader_connection,
					marker + 1U,
					2,
					marker + 2U,
					0,
					writer.holder.generation(),
					identity("test.reader-attachment-epoch", marker + 2U));
				const auto fresh_session_request =
					reader_session_request(fresh_map_request, marker + 2U);
				auto fresh_session_result = coordinator.begin_reader_session(fresh_session_request);
				require(fresh_session_result.has_value(),
						"a fresh attachment epoch remains admissible after no-map revocation");
				auto fresh_session = std::move(*fresh_session_result);
				const auto fresh_terminal =
					sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
						fresh_session_request,
						sqlite_shm_reader_session_terminal_kind::failure,
						identity("test.reader-session-terminal", marker + 2U));
				require(coordinator.complete_reader_session(fresh_session, fresh_terminal) &&
							!fresh_session.valid(),
						"fresh-epoch proof fixture closes without leaking a reservation");
			}

			retire_last(coordinator, writer.holder, callback(3, marker + 3U));
			require(coordinator.revoke_writer_eligibility(writer.eligibility).has_value(),
					"revoke accepted zero-attachment fixture writer gate");
		}

		struct rejected_row
		{
			sqlite_shm_reader_attachment_zero_effect_kind kind;
			int native_status;
			bool native_mapping;
		};
		const rejected_row rejected_rows[]{
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_protocol_invalid_no_attachment,
			 sqlite_readonly_status,
			 false},
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_protocol_invalid_no_attachment,
			 sqlite_readonly_status,
			 true},
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_protocol_invalid_no_attachment,
			 sqlite_readonly_cantinit_status,
			 false},
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_protocol_invalid_no_attachment,
			 sqlite_readonly_unsupported_extended_status,
			 true},
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_protocol_invalid_no_attachment,
			 sqlite_ok_status,
			 true},
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_protocol_invalid_no_attachment,
			 sqlite_busy_status,
			 false},
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
			 sqlite_ok_status,
			 false},
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
			 sqlite_invalid_extended_ok_status,
			 false},
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
			 sqlite_undefined_primary_status,
			 false},
			{sqlite_shm_reader_attachment_zero_effect_kind::exact_protocol_invalid_no_attachment,
			 sqlite_invalid_extended_ok_status,
			 true},
		};

		for (std::size_t index = 0; index < std::size(rejected_rows); ++index)
		{
			const auto marker = static_cast<std::uint8_t>(150U + index * 2U);
			const auto binding = family(marker);
			const auto writer_connection = identity("test.connection", marker);
			const auto reader_connection = identity("test.connection", marker + 1U);
			const auto writer_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(
				coordinator, binding, writer_connection, writer_epoch, marker, &page);
			const auto map_request = reader_attachment_request(binding,
															   reader_connection,
															   marker + 1U,
															   2,
															   marker + 1U,
															   0,
															   writer.holder.generation());
			const auto session_request = reader_session_request(map_request, marker + 1U);
			auto session_result = coordinator.begin_reader_session(session_request);
			require(session_result.has_value(),
					"zero-attachment rejected row reserves a first session");
			auto session = std::move(*session_result);
			auto map_result = coordinator.begin_reader_map(session, map_request);
			require(map_result.has_value(),
					"zero-attachment rejected row begins its exact native attempt");
			auto inflight = std::move(*map_result);
			const auto& row = rejected_rows[index];
			const auto receipt =
				sqlite_same_process_shm_lease_test_peer::reader_attachment_zero_effect(
					inflight,
					row.kind,
					map_request,
					row.native_status,
					row.native_mapping ? &page : nullptr,
					identity("test.reader-zero-attachment-effect", marker));
			auto rejected =
				coordinator.complete_reader_zero_attachment_map(inflight, receipt, session);
			const auto terminal = coordinator.snapshot();
			require(!rejected &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						!inflight.valid() && !session.valid() && terminal.quarantined &&
						terminal.reader_inflight_count == 0U &&
						terminal.reader_attachment_group_count == 0U &&
						terminal.reader_attachment_zero_effect_terminal_count == 0U &&
						terminal.reader_attachment_revoked_no_map_count == 0U,
					"excluded status/pointer row fails closed without publishing a terminal");
		}
	}

	void verify_reader_zero_attachment_receipt_binding_and_effect_proof()
	{
		const auto verify_invalid_receipt = [](const std::uint8_t marker,
											   const bool omit_effect,
											   const bool mismatch_request,
											   const int delegated_extend)
		{
			const auto binding = family(marker);
			const auto writer_connection = identity("test.connection", marker);
			const auto reader_connection = identity("test.connection", marker + 1U);
			const auto writer_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(
				coordinator, binding, writer_connection, writer_epoch, marker, &page);
			const auto map_request = reader_attachment_request(binding,
															   reader_connection,
															   marker + 1U,
															   2,
															   marker + 1U,
															   0,
															   writer.holder.generation());
			const auto session_request = reader_session_request(map_request, marker + 1U);
			auto session_result = coordinator.begin_reader_session(session_request);
			require(session_result.has_value(), "invalid zero-effect fixture reserves a session");
			auto session = std::move(*session_result);
			auto map_result = coordinator.begin_reader_map(session, map_request);
			require(map_result.has_value(), "invalid zero-effect fixture begins native map");
			auto inflight = std::move(*map_result);
			auto receipt_request = map_request;
			if (mismatch_request)
				receipt_request.callback = callback(9, marker + 2U);
			const auto receipt =
				sqlite_same_process_shm_lease_test_peer::reader_attachment_zero_effect(
					inflight,
					sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
					std::move(receipt_request),
					sqlite_busy_status,
					nullptr,
					omit_effect ? sqlite_backend_opaque_identity{}
								: identity("test.reader-zero-attachment-effect", marker),
					delegated_extend);
			auto rejected =
				coordinator.complete_reader_zero_attachment_map(inflight, receipt, session);
			const auto snapshot = coordinator.snapshot();
			require(!rejected &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						!inflight.valid() && !session.valid() && snapshot.quarantined &&
						snapshot.reader_attachment_zero_effect_terminal_count == 0U &&
						snapshot.reader_attachment_revoked_no_map_count == 0U,
					"missing effect, mismatched request, or nonzero delegation cannot "
					"self-authorize no-map");
		};
		verify_invalid_receipt(170, true, false, 0);
		verify_invalid_receipt(172, false, true, 0);
		verify_invalid_receipt(174, false, false, 1);

		{
			constexpr std::uint8_t marker = 176;
			const auto binding = family(marker);
			const auto writer_connection = identity("test.connection", marker);
			const auto reader_connection_a = identity("test.connection", marker + 1U);
			const auto reader_connection_b = identity("test.connection", marker + 2U);
			const auto writer_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(
				coordinator, binding, writer_connection, writer_epoch, marker, &page);
			const auto request_a = reader_attachment_request(binding,
															 reader_connection_a,
															 marker + 1U,
															 2,
															 marker + 1U,
															 0,
															 writer.holder.generation());
			const auto request_b = reader_attachment_request(binding,
															 reader_connection_b,
															 marker + 2U,
															 3,
															 marker + 2U,
															 0,
															 writer.holder.generation());
			auto session_a_result =
				coordinator.begin_reader_session(reader_session_request(request_a, marker + 1U));
			auto session_b_result =
				coordinator.begin_reader_session(reader_session_request(request_b, marker + 2U));
			require(session_a_result && session_b_result,
					"duplicate-effect fixture reserves two distinct attachment sessions");
			auto session_a = std::move(*session_a_result);
			auto session_b = std::move(*session_b_result);
			auto map_a_result = coordinator.begin_reader_map(session_a, request_a);
			auto map_b_result = coordinator.begin_reader_map(session_b, request_b);
			require(map_a_result && map_b_result,
					"duplicate-effect fixture begins two distinct native attempts");
			auto map_a = std::move(*map_a_result);
			auto map_b = std::move(*map_b_result);
			const auto active =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto map_a_record =
				std::ranges::find(active.map_attempts,
								  request_a.expected_attachment,
								  &sqlite_shm_reader_map_attempt_test_view::attachment);
			require(map_a_record != active.map_attempts.end(),
					"locate first zero-effect owner before exact terminal");
			const auto map_a_token = map_a_record->map_token;
			const auto effect = identity("test.reader-zero-attachment-effect", marker);
			const auto receipt_a =
				sqlite_same_process_shm_lease_test_peer::reader_attachment_zero_effect(
					map_a,
					sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
					request_a,
					sqlite_busy_status,
					nullptr,
					effect);
			auto completed_a =
				coordinator.complete_reader_zero_attachment_map(map_a, receipt_a, session_a);
			require(completed_a && !map_a.valid() && !session_a.valid() &&
						coordinator.snapshot().reader_attachment_zero_effect_terminal_count == 1U,
					"first exact effect identity publishes one no-map terminal");
			const auto before_duplicate =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto map_b_record =
				std::ranges::find(before_duplicate.map_attempts,
								  request_b.expected_attachment,
								  &sqlite_shm_reader_map_attempt_test_view::attachment);
			const auto session_b_record =
				std::ranges::find(before_duplicate.session_reservations,
								  request_b.expected_attachment,
								  &sqlite_shm_reader_session_reservation_test_view::attachment);
			require(map_b_record != before_duplicate.map_attempts.end() &&
						session_b_record != before_duplicate.session_reservations.end() &&
						before_duplicate.outstanding_terminal_permit_count == 4U,
					"duplicate zero-effect fixture lacks exact second-owner custody");
			const auto map_b_token = map_b_record->map_token;
			const auto session_b_token = session_b_record->session_token;

			const auto receipt_b =
				sqlite_same_process_shm_lease_test_peer::reader_attachment_zero_effect(
					map_b,
					sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
					request_b,
					sqlite_busy_status,
					nullptr,
					effect);
			auto duplicate =
				coordinator.complete_reader_zero_attachment_map(map_b, receipt_b, session_b);
			const auto snapshot = coordinator.snapshot();
			const auto lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto* map_quarantine = find_reader_terminal_quarantine(lifecycle, map_b_token);
			const auto* session_quarantine =
				find_reader_terminal_quarantine(lifecycle, session_b_token);
			const auto attachment_quarantine = std::ranges::find_if(
				lifecycle.terminal_quarantines,
				[&](const sqlite_shm_reader_terminal_quarantine_test_view& terminal)
				{
					return terminal.attachment == request_b.expected_attachment &&
						terminal.owner_token != map_b_token &&
						terminal.owner_token != session_b_token;
				});
			const auto first_terminal =
				std::ranges::find(lifecycle.zero_effect_terminals,
								  map_a_token,
								  &sqlite_shm_reader_zero_effect_terminal_test_view::owner_token);
			require(!duplicate &&
						duplicate.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch &&
						duplicate.error().action ==
							sqlite_shm_lease_recovery_action::quarantine_no_retry &&
						!map_b.valid() && !session_b.valid() && snapshot.quarantined &&
						snapshot.reader_attachment_zero_effect_terminal_count == 1U &&
						snapshot.reader_attachment_revoked_no_map_count == 1U,
					"duplicate zero-effect identity did not terminally reject its target owner");
			require(
				lifecycle.last_issued_sequence == before_duplicate.last_issued_sequence + 2U &&
					lifecycle.last_committed_sequence == lifecycle.last_issued_sequence &&
					lifecycle.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(lifecycle) &&
					all_reader_live_custody_released(lifecycle),
				"duplicate zero-effect identity did not consume exact target slots and custody");
			require(
				map_quarantine != nullptr &&
					map_quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::presented_invalid &&
					map_quarantine->terminal_sequence ==
						before_duplicate.last_issued_sequence + 1U &&
					!map_quarantine->exact_terminal_receipt_retained &&
					session_quarantine != nullptr &&
					session_quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::presented_invalid &&
					session_quarantine->terminal_sequence ==
						before_duplicate.last_issued_sequence + 2U &&
					!session_quarantine->exact_terminal_receipt_retained,
				"duplicate zero-effect identity did not publish ordered map/session quarantine");
			require(
				attachment_quarantine != lifecycle.terminal_quarantines.end() &&
					attachment_quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::presented_invalid &&
					attachment_quarantine->terminal_sequence ==
						before_duplicate.last_issued_sequence + 2U &&
					first_terminal != lifecycle.zero_effect_terminals.end() &&
					first_terminal->zero_attachment_effect_receipt &&
					*first_terminal->zero_attachment_effect_receipt == effect &&
					first_terminal->exact_terminal_receipt_retained,
				"duplicate zero-effect identity lost its first exact terminal or target "
				"attachment quarantine");
			auto replay =
				coordinator.complete_reader_zero_attachment_map(map_b, receipt_b, session_b);
			require(!replay &&
						replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
					"duplicate zero-effect presentation retained a second completion path");
		}

		constexpr std::uint8_t marker = 174;
		const auto binding = family(marker);
		const auto writer_connection = identity("test.connection", marker);
		const auto reader_connection_a = identity("test.connection", marker + 1U);
		const auto reader_connection_b = identity("test.connection", marker + 2U);
		const auto writer_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto writer = install_live_writer(
			coordinator, binding, writer_connection, writer_epoch, marker, &page);
		const auto request_a = reader_attachment_request(binding,
														 reader_connection_a,
														 marker + 1U,
														 2,
														 marker + 1U,
														 0,
														 writer.holder.generation());
		const auto request_b = reader_attachment_request(binding,
														 reader_connection_b,
														 marker + 2U,
														 3,
														 marker + 2U,
														 0,
														 writer.holder.generation());
		auto session_a_result =
			coordinator.begin_reader_session(reader_session_request(request_a, marker + 1U));
		auto session_b_result =
			coordinator.begin_reader_session(reader_session_request(request_b, marker + 2U));
		require(session_a_result && session_b_result,
				"attempt-binding fixture reserves two distinct attachment sessions");
		auto session_a = std::move(*session_a_result);
		auto session_b = std::move(*session_b_result);
		auto map_a_result = coordinator.begin_reader_map(session_a, request_a);
		auto map_b_result = coordinator.begin_reader_map(session_b, request_b);
		require(map_a_result && map_b_result,
				"attempt-binding fixture begins two distinct native attempts");
		auto map_a = std::move(*map_a_result);
		auto map_b = std::move(*map_b_result);
		const auto wrong_attempt_receipt =
			sqlite_same_process_shm_lease_test_peer::reader_attachment_zero_effect(
				map_a,
				sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
				request_b,
				sqlite_busy_status,
				nullptr,
				identity("test.reader-zero-attachment-effect", marker));
		auto wrong_attempt = coordinator.complete_reader_zero_attachment_map(
			map_b, wrong_attempt_receipt, session_b);
		const auto quarantined = coordinator.snapshot();
		require(!wrong_attempt &&
					wrong_attempt.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					!map_b.valid() && !session_b.valid() && map_a.valid() && session_a.valid() &&
					quarantined.quarantined &&
					quarantined.reader_attachment_zero_effect_terminal_count == 0U &&
					quarantined.reader_attachment_revoked_no_map_count == 0U,
				"a complete proof sealed for another attempt fails closed without publication");
	}

	void verify_reader_zero_attachment_later_group_preserves_authority()
	{
		constexpr std::uint8_t marker = 180;
		const auto binding = family(marker);
		const auto writer_connection = identity("test.connection", marker);
		const auto reader_connection = identity("test.connection", marker + 1U);
		const auto writer_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto writer = install_live_writer(
			coordinator, binding, writer_connection, writer_epoch, marker, &page);
		auto map_request = reader_attachment_request(
			binding, reader_connection, marker + 1U, 2, marker + 1U, 0, writer.holder.generation());
		const auto session_request = reader_session_request(map_request, marker + 1U);
		auto session_result = coordinator.begin_reader_session(session_request);
		require(session_result.has_value(), "later no-map fixture reserves a reader session");
		auto session = std::move(*session_result);
		auto first_map_result = coordinator.begin_reader_map(session, map_request);
		require(first_map_result.has_value(), "later no-map fixture begins its first map");
		auto first_map = std::move(*first_map_result);
		auto first_commit_result = coordinator.commit_reader_map(
			first_map,
			sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
				map_request,
				writer.holder.generation(),
				mapping(0, &page, 4096U),
				identity("test.zero-reader-resize", marker)),
			session);
		require(first_commit_result.has_value(), "later no-map fixture forms its exact group");
		auto first_commit = std::move(*first_commit_result);
		auto handoff_result = first_commit.take_handoff();
		require(handoff_result.has_value(), "later no-map fixture owns its group handoff");
		auto handoff = std::move(*handoff_result);
		const auto before = coordinator.snapshot();
		const auto before_lifecycle =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(before_lifecycle.attachment_groups.size() == 1U &&
					before_lifecycle.outstanding_terminal_permit_count == 3U &&
					reader_terminal_permit_slots_are_exact(before_lifecycle) &&
					before_lifecycle.attachment_groups.front().unmap_cut_permit_slot != 0U &&
					before_lifecycle.attachment_groups.front().unmap_terminal_permit_slot != 0U,
				"later no-map fixture retains its session and pre-reserved group cut/terminal "
				"capacity");
		const auto before_attachment = only_attachment_reservation(before_lifecycle);
		const auto before_session = only_session_reservation(before_lifecycle);
		const auto before_group = before_lifecycle.attachment_groups.front();

		map_request.callback = callback(2, marker + 2U);
		auto later_map_result = coordinator.begin_reader_map(session, map_request);
		require(later_map_result.has_value(), "active group admits a later native map attempt");
		auto later_map = std::move(*later_map_result);
		const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_attachment_zero_effect(
			later_map,
			sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
			map_request,
			sqlite_busy_status,
			nullptr,
			identity("test.reader-zero-attachment-effect", marker));
		auto completed =
			coordinator.complete_reader_zero_attachment_map(later_map, receipt, session);
		const auto after = coordinator.snapshot();
		const auto after_lifecycle =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto& after_attachment = only_attachment_reservation(after_lifecycle);
		const auto& after_session = only_session_reservation(after_lifecycle);
		require(
			completed &&
				completed->kind() ==
					sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change &&
				completed->native_status() == sqlite_busy_status &&
				completed->native_mapping() == nullptr && !later_map.valid() && session.valid() &&
				session.phase() == sqlite_shm_reader_session_phase::active_group_owner &&
				handoff.valid() && !after.quarantined &&
				after.reader_inflight_count == before.reader_inflight_count &&
				after.reader_attachment_group_count == before.reader_attachment_group_count &&
				after.reader_attachment_live_member_count ==
					before.reader_attachment_live_member_count &&
				after.reader_attachment_audit_count == before.reader_attachment_audit_count &&
				after.reader_session_reservation_count == before.reader_session_reservation_count &&
				after.reader_session_owner_count == before.reader_session_owner_count &&
				after.reader_handoff_count == before.reader_handoff_count &&
				after.reader_attachment_revoked_no_map_count ==
					before.reader_attachment_revoked_no_map_count &&
				after.reader_attachment_zero_effect_terminal_count ==
					before.reader_attachment_zero_effect_terminal_count + 1U &&
				after_lifecycle.last_committed_sequence >
					before_lifecycle.last_committed_sequence &&
				after_lifecycle.outstanding_terminal_permit_count == 3U &&
				reader_terminal_permit_slots_are_exact(after_lifecycle) &&
				after_lifecycle.map_attempts.empty() &&
				after_attachment.attachment == before_attachment.attachment &&
				after_attachment.phase == before_attachment.phase &&
				after_attachment.origin_sequence == before_attachment.origin_sequence &&
				after_attachment.destination_sequence == before_attachment.destination_sequence &&
				after_attachment.group_payload_present == before_attachment.group_payload_present &&
				after_session.session_token == before_session.session_token &&
				after_session.phase == before_session.phase &&
				after_session.origin_sequence == before_session.origin_sequence &&
				after_session.destination_sequence == before_session.destination_sequence &&
				after_lifecycle.attachment_groups.size() == 1U &&
				after_lifecycle.attachment_groups.front().attachment == before_group.attachment &&
				after_lifecycle.attachment_groups.front().phase == before_group.phase &&
				after_lifecycle.attachment_groups.front().origin_sequence ==
					before_group.origin_sequence &&
				after_lifecycle.attachment_groups.front().destination_sequence ==
					before_group.destination_sequence &&
				after_lifecycle.attachment_groups.front().unmap_cut_permit_slot ==
					before_group.unmap_cut_permit_slot &&
				after_lifecycle.attachment_groups.front().unmap_terminal_permit_slot ==
					before_group.unmap_terminal_permit_slot,
			"later determinate no-map adds only its terminal audit and preserves the group");

		auto protocol_request = map_request;
		protocol_request.callback = callback(2, marker + 3U);
		auto protocol_map_result = coordinator.begin_reader_map(session, protocol_request);
		require(protocol_map_result.has_value(),
				"active group admits a later protocol-invalid no-map attempt");
		auto protocol_map = std::move(*protocol_map_result);
		const auto protocol_receipt =
			sqlite_same_process_shm_lease_test_peer::reader_attachment_zero_effect(
				protocol_map,
				sqlite_shm_reader_attachment_zero_effect_kind::exact_protocol_invalid_no_attachment,
				protocol_request,
				sqlite_ok_status,
				nullptr,
				identity("test.reader-zero-attachment-effect", marker + 1U));
		auto protocol_completed = coordinator.complete_reader_zero_attachment_map(
			protocol_map, protocol_receipt, session);
		const auto after_protocol = coordinator.snapshot();
		const auto after_protocol_lifecycle =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto& protocol_attachment = only_attachment_reservation(after_protocol_lifecycle);
		const auto& protocol_session = only_session_reservation(after_protocol_lifecycle);
		require(protocol_completed &&
					protocol_completed->kind() ==
						sqlite_shm_reader_attachment_zero_effect_kind::
							exact_protocol_invalid_no_attachment &&
					protocol_completed->native_status() == sqlite_ioerr_status &&
					protocol_completed->native_mapping() == nullptr && !protocol_map.valid() &&
					session.valid() && handoff.valid() && !after_protocol.quarantined &&
					after_protocol.reader_inflight_count == after.reader_inflight_count &&
					after_protocol.reader_attachment_group_count ==
						after.reader_attachment_group_count &&
					after_protocol.reader_attachment_live_member_count ==
						after.reader_attachment_live_member_count &&
					after_protocol.reader_attachment_audit_count ==
						after.reader_attachment_audit_count &&
					after_protocol.reader_session_reservation_count ==
						after.reader_session_reservation_count &&
					after_protocol.reader_session_owner_count == after.reader_session_owner_count &&
					after_protocol.reader_handoff_count == after.reader_handoff_count &&
					after_protocol.reader_attachment_revoked_no_map_count ==
						after.reader_attachment_revoked_no_map_count &&
					after_protocol.reader_attachment_zero_effect_terminal_count ==
						after.reader_attachment_zero_effect_terminal_count + 1U &&
					after_protocol_lifecycle.last_committed_sequence >
						after_lifecycle.last_committed_sequence &&
					after_protocol_lifecycle.outstanding_terminal_permit_count == 3U &&
					reader_terminal_permit_slots_are_exact(after_protocol_lifecycle) &&
					after_protocol_lifecycle.map_attempts.empty() &&
					protocol_attachment.attachment == before_attachment.attachment &&
					protocol_attachment.phase == before_attachment.phase &&
					protocol_attachment.origin_sequence == before_attachment.origin_sequence &&
					protocol_attachment.destination_sequence ==
						before_attachment.destination_sequence &&
					protocol_session.session_token == before_session.session_token &&
					protocol_session.phase == before_session.phase &&
					protocol_session.origin_sequence == before_session.origin_sequence &&
					protocol_session.destination_sequence == before_session.destination_sequence &&
					after_protocol_lifecycle.attachment_groups.size() == 1U &&
					after_protocol_lifecycle.attachment_groups.front().attachment ==
						before_group.attachment &&
					after_protocol_lifecycle.attachment_groups.front().phase ==
						before_group.phase &&
					after_protocol_lifecycle.attachment_groups.front().origin_sequence ==
						before_group.origin_sequence &&
					after_protocol_lifecycle.attachment_groups.front().destination_sequence ==
						before_group.destination_sequence &&
					after_protocol_lifecycle.attachment_groups.front().unmap_cut_permit_slot ==
						before_group.unmap_cut_permit_slot &&
					after_protocol_lifecycle.attachment_groups.front().unmap_terminal_permit_slot ==
						before_group.unmap_terminal_permit_slot,
				"later protocol-invalid no-map projects IOERR without changing group authority");

		auto replay = coordinator.complete_reader_zero_attachment_map(later_map, receipt, session);
		auto callback_replay = coordinator.begin_reader_map(session, map_request);
		require(
			!replay && replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
				!callback_replay &&
				callback_replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
				session.valid() && handoff.valid() &&
				coordinator.snapshot().reader_attachment_zero_effect_terminal_count ==
					after_protocol.reader_attachment_zero_effect_terminal_count &&
				!coordinator.snapshot().quarantined,
			"later no-map attempt and callback invocation cannot replay");

		auto mismatch_request = map_request;
		mismatch_request.callback = callback(2, marker + 4U);
		auto mismatch_map_result = coordinator.begin_reader_map(session, mismatch_request);
		require(mismatch_map_result.has_value(),
				"active proposal group admits the exact later mismatch attempt");
		auto mismatch_map = std::move(*mismatch_map_result);
		const auto mismatch_receipt =
			sqlite_same_process_shm_lease_test_peer::reader_existing_group_predecessor_mismatch(
				mismatch_map,
				mismatch_request,
				sqlite_readonly_status,
				&page,
				identity("test.reader-existing-group-mismatch-effect", marker));
		const auto before_mismatch_lifecycle =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		auto mismatch = coordinator.complete_reader_existing_group_predecessor_mismatch(
			mismatch_map, mismatch_receipt, session);
		const auto after_mismatch = coordinator.snapshot();
		const auto after_mismatch_lifecycle =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto handoff_index =
			enum_index(detail::sqlite_shm_reader_custody_kind::attachment_group_handoff);
		const auto deferred_unmap_index =
			enum_index(detail::sqlite_shm_reader_custody_kind::normal_or_deferred_unmap);
		require(mismatch && mismatch->native_status() == sqlite_readonly_status &&
					mismatch->outward_status() == sqlite_ioerr_status &&
					mismatch->native_mapping() == nullptr && !mismatch_map.valid() &&
					session.valid() && handoff.valid(),
				"later mapped mismatch projects IOERR/null without consuming use owners");
		require(!after_mismatch.quarantined &&
					after_mismatch.reader_existing_group_deferred_cleanup_count == 1U &&
					after_mismatch.reader_predecessor_map_terminal_count == 0U,
				"later mapped mismatch hides the group without minting predecessor authority");
		require(
			after_mismatch_lifecycle.map_attempts.empty() &&
				after_mismatch_lifecycle.live_custody_kind_counts[handoff_index] == 0U &&
				after_mismatch_lifecycle.live_custody_kind_counts[deferred_unmap_index] == 1U &&
				after_mismatch_lifecycle.last_committed_sequence >
					before_mismatch_lifecycle.last_committed_sequence &&
				after_mismatch_lifecycle.outstanding_terminal_permit_count == 3U &&
				reader_terminal_permit_slots_are_exact(after_mismatch_lifecycle),
			"later mapped mismatch hides the group and transfers exactly one deferred unmap owner");

		auto rejected_request = mismatch_request;
		rejected_request.callback = callback(2, marker + 5U);
		auto rejected_map = coordinator.begin_reader_map(session, rejected_request);
		require(!rejected_map &&
					rejected_map.error().reason == sqlite_shm_lease_rejection_reason::retiring &&
					session.valid() && handoff.valid() &&
					coordinator.snapshot().reader_existing_group_deferred_cleanup_count == 1U &&
					!coordinator.snapshot().quarantined,
				"deferred existing group rejects every fresh map before native work");

		const auto terminal_receipt =
			sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				session_request,
				sqlite_shm_reader_session_terminal_kind::failure,
				identity("test.reader-session-terminal", marker));
		require(coordinator.complete_reader_session(session, terminal_receipt) && !session.valid(),
				"later no-map fixture closes its unchanged session owner");
		const auto unmap_callback = callback(3, marker + 6U);
		auto unmap = coordinator.begin_reader_unmap(
			handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
		require(unmap.has_value() && !handoff.valid(),
				"later no-map fixture transfers its unchanged group handoff");
		const auto unmap_receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
			*unmap,
			unmap_callback,
			sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
			sqlite_ok_status,
			0,
			0,
			identity("test.reader-later-no-map-unmap-effect", marker),
			identity("test.reader-later-no-map-latch-reset", marker));
		require(coordinator.complete_reader_unmap(*unmap, unmap_receipt).has_value() &&
					!unmap->valid(),
				"later no-map fixture closes its original group");
		require(coordinator.snapshot().reader_existing_group_deferred_cleanup_count == 0U,
				"confirmed deferred unmap retires the mismatch cleanup obligation");
		retire_last(coordinator, writer.holder, callback(4, marker + 7U));
		require(coordinator.revoke_writer_eligibility(writer.eligibility).has_value(),
				"revoke later zero-attachment fixture writer gate");
	}

	void verify_cross_family_reader_terminal_receipts_are_owner_bound()
	{
		{
			constexpr std::uint8_t target_marker = 180U;
			constexpr std::uint8_t source_marker = 184U;
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator target{family(target_marker),
																	 generations};
			sqlite_same_process_shm_mapping_lease_coordinator source{family(source_marker),
																	 generations};
			int target_page{};
			int source_page{};
			auto target_writer = install_live_writer(target,
													 family(target_marker),
													 identity("test.connection", target_marker),
													 identity("test.open-epoch", target_marker),
													 target_marker,
													 &target_page);
			auto source_writer = install_live_writer(source,
													 family(source_marker),
													 identity("test.connection", source_marker),
													 identity("test.open-epoch", source_marker),
													 source_marker,
													 &source_page);
			const auto target_request =
				reader_attachment_request(family(target_marker),
										  identity("test.connection", target_marker + 1U),
										  target_marker + 1U,
										  2,
										  target_marker + 1U,
										  0,
										  target_writer.holder.generation());
			const auto source_request =
				reader_attachment_request(family(source_marker),
										  identity("test.connection", source_marker + 1U),
										  source_marker + 1U,
										  2,
										  source_marker + 1U,
										  0,
										  source_writer.holder.generation());
			const auto target_session_request =
				reader_session_request(target_request, target_marker + 1U);
			const auto source_session_request =
				reader_session_request(source_request, source_marker + 1U);
			auto target_session = target.begin_reader_session(target_session_request);
			auto source_session = source.begin_reader_session(source_session_request);
			auto target_map = target.begin_reader_map(*target_session, target_request);
			auto source_map = source.begin_reader_map(*source_session, source_request);
			require(target_session && source_session && target_map && source_map,
					"cross-family mapped receipt fixture begins two native owners");
			const auto source_effect =
				identity("test.reader-cross-family-map-effect", source_marker);
			const auto source_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
					source_request,
					source_writer.holder.generation(),
					mapping(0, &source_page, 4096U),
					source_effect);
			const auto target_before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(target);
			const auto target_token = only_reader_map_attempt(target_before).map_token;
			auto rejected = target.commit_reader_map(*target_map, source_receipt, *target_session);
			const auto target_after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(target);
			const auto* target_quarantine =
				find_reader_terminal_quarantine(target_after, target_token);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::receipt_mismatch &&
					rejected.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					!target_map->valid() && !target_session->valid() && source_map->valid() &&
					source_session->valid() && target.snapshot().quarantined &&
					!source.snapshot().quarantined &&
					target_after.outstanding_terminal_permit_count == 0U &&
					all_reader_live_custody_released(target_after) &&
					target_quarantine != nullptr &&
					target_quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::presented_invalid &&
					!target_quarantine->exact_terminal_receipt_retained,
				"cross-family mapped receipt did not close only the target owner");
			auto committed = source.commit_reader_map(*source_map, source_receipt, *source_session);
			require(committed && committed->formed_group(),
					"cross-family mapped rejection consumed the exact source receipt");
			auto source_handoff = committed->take_handoff();
			const auto source_terminal =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					source_session_request,
					sqlite_shm_reader_session_terminal_kind::success,
					identity("test.reader-cross-family-map-session-terminal", source_marker));
			require(source.complete_reader_session(*source_session, source_terminal).has_value(),
					"drain cross-family mapped source session");
			const auto unmap_callback = callback(7, source_marker + 2U);
			auto unmap = source.begin_reader_unmap(
				*source_handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
			const auto unmap_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
					*unmap,
					unmap_callback,
					sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
					sqlite_ok_status,
					0,
					0,
					identity("test.reader-cross-family-map-unmap-effect", source_marker),
					identity("test.reader-cross-family-map-unmap-latch", source_marker));
			require(source.complete_reader_unmap(*unmap, unmap_receipt).has_value() &&
						sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(source)
								.outstanding_terminal_permit_count == 0U,
					"cross-family mapped target rejection stranded source drains");
		}

		{
			constexpr std::uint8_t target_marker = 188U;
			constexpr std::uint8_t source_marker = 192U;
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator target{family(target_marker),
																	 generations};
			sqlite_same_process_shm_mapping_lease_coordinator source{family(source_marker),
																	 generations};
			int target_page{};
			int source_page{};
			auto target_writer = install_live_writer(target,
													 family(target_marker),
													 identity("test.connection", target_marker),
													 identity("test.open-epoch", target_marker),
													 target_marker,
													 &target_page);
			auto source_writer = install_live_writer(source,
													 family(source_marker),
													 identity("test.connection", source_marker),
													 identity("test.open-epoch", source_marker),
													 source_marker,
													 &source_page);
			const auto target_request =
				reader_attachment_request(family(target_marker),
										  identity("test.connection", target_marker + 1U),
										  target_marker + 1U,
										  2,
										  target_marker + 1U,
										  0,
										  target_writer.holder.generation());
			const auto source_request =
				reader_attachment_request(family(source_marker),
										  identity("test.connection", source_marker + 1U),
										  source_marker + 1U,
										  2,
										  source_marker + 1U,
										  0,
										  source_writer.holder.generation());
			const auto target_session_request =
				reader_session_request(target_request, target_marker + 1U);
			const auto source_session_request =
				reader_session_request(source_request, source_marker + 1U);
			auto target_session = target.begin_reader_session(target_session_request);
			auto source_session = source.begin_reader_session(source_session_request);
			auto target_map = target.begin_reader_map(*target_session, target_request);
			auto source_map = source.begin_reader_map(*source_session, source_request);
			require(target_session && source_session && target_map && source_map,
					"cross-family zero receipt fixture begins two native owners");
			const auto source_effect =
				identity("test.reader-cross-family-zero-effect", source_marker);
			const auto source_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_attachment_zero_effect(
					*source_map,
					sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
					source_request,
					sqlite_busy_status,
					nullptr,
					source_effect);
			const auto target_before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(target);
			const auto target_token = only_reader_map_attempt(target_before).map_token;
			auto rejected = target.complete_reader_zero_attachment_map(
				*target_map, source_receipt, *target_session);
			const auto target_after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(target);
			const auto* target_quarantine =
				find_reader_terminal_quarantine(target_after, target_token);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					rejected.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					!target_map->valid() && !target_session->valid() && source_map->valid() &&
					source_session->valid() && target.snapshot().quarantined &&
					!source.snapshot().quarantined &&
					target_after.outstanding_terminal_permit_count == 0U &&
					all_reader_live_custody_released(target_after) &&
					target_quarantine != nullptr &&
					target_quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::presented_invalid &&
					!target_quarantine->exact_terminal_receipt_retained,
				"cross-family zero receipt did not close only the target owner");
			auto completed = source.complete_reader_zero_attachment_map(
				*source_map, source_receipt, *source_session);
			require(
				completed &&
					completed->kind() ==
						sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change &&
					!source_map->valid() && !source_session->valid() &&
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(source)
							.outstanding_terminal_permit_count == 0U,
				"cross-family zero rejection consumed the exact source receipt");
		}

		{
			constexpr std::uint8_t target_marker = 196U;
			constexpr std::uint8_t source_marker = 200U;
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator target{family(target_marker),
																	 generations};
			sqlite_same_process_shm_mapping_lease_coordinator source{family(source_marker),
																	 generations};
			int target_page{};
			int source_page{};
			auto target_writer = install_live_writer(target,
													 family(target_marker),
													 identity("test.connection", target_marker),
													 identity("test.open-epoch", target_marker),
													 target_marker,
													 &target_page);
			auto source_writer = install_live_writer(source,
													 family(source_marker),
													 identity("test.connection", source_marker),
													 identity("test.open-epoch", source_marker),
													 source_marker,
													 &source_page);
			const auto target_request =
				reader_attachment_request(family(target_marker),
										  identity("test.connection", target_marker + 1U),
										  target_marker + 1U,
										  2,
										  target_marker + 1U,
										  0,
										  target_writer.holder.generation());
			const auto source_request =
				reader_attachment_request(family(source_marker),
										  identity("test.connection", source_marker + 1U),
										  source_marker + 1U,
										  2,
										  source_marker + 1U,
										  0,
										  source_writer.holder.generation());
			const auto target_session_request =
				reader_session_request(target_request, target_marker + 1U);
			const auto source_session_request =
				reader_session_request(source_request, source_marker + 1U);
			auto target_session = target.begin_reader_session(target_session_request);
			auto source_session = source.begin_reader_session(source_session_request);
			require(target_session && source_session,
					"cross-family session receipt fixture reserves two owners");
			const auto source_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					source_session_request,
					sqlite_shm_reader_session_terminal_kind::cancelled_before_authority_read,
					identity("test.reader-cross-family-session-terminal", source_marker));
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(target);
			const auto target_token = only_session_reservation(before).session_token;
			auto rejected = target.complete_reader_session(*target_session, source_receipt);
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(target);
			const auto* target_quarantine = find_reader_terminal_quarantine(after, target_token);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					rejected.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					!target_session->valid() && source_session->valid() &&
					target.snapshot().quarantined && !source.snapshot().quarantined &&
					after.outstanding_terminal_permit_count == 0U &&
					all_reader_live_custody_released(after) && target_quarantine != nullptr &&
					target_quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::presented_invalid &&
					!target_quarantine->exact_terminal_receipt_retained,
				"cross-family session receipt did not close only the target owner");
			require(source.complete_reader_session(*source_session, source_receipt).has_value() &&
						sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(source)
								.outstanding_terminal_permit_count == 0U,
					"cross-family session rejection consumed the exact source receipt");
		}
	}

	void verify_reused_mapped_and_session_terminal_identities_fail_closed()
	{
		{
			constexpr std::uint8_t marker = 200U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto request_a =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 1U),
										  marker + 1U,
										  2,
										  marker + 1U,
										  0,
										  writer.holder.generation());
			const auto session_request_a = reader_session_request(request_a, marker + 1U);
			auto session_a = coordinator.begin_reader_session(session_request_a);
			require(session_a && session_a->valid(),
					"mapped identity-reuse fixture reserves its source session");
			auto map_a = coordinator.begin_reader_map(*session_a, request_a);
			require(map_a && map_a->valid(), "mapped identity-reuse fixture begins its source map");
			const auto effect = identity("test.reader-reused-positive-map-effect", marker);
			auto committed_a = coordinator.commit_reader_map(
				*map_a,
				sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
					request_a, writer.holder.generation(), mapping(0, &page, 4096U), effect),
				*session_a);
			require(committed_a && committed_a->formed_group(),
					"mapped identity-reuse fixture retains its source exact receipt");
			auto handoff_a = committed_a->take_handoff();
			require(handoff_a && handoff_a->valid(),
					"mapped identity-reuse fixture takes its source handoff");

			const auto request_b =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 2U),
										  marker + 2U,
										  3,
										  marker + 2U,
										  0,
										  writer.holder.generation());
			const auto session_request_b = reader_session_request(request_b, marker + 2U);
			auto session_b = coordinator.begin_reader_session(session_request_b);
			require(session_b && session_b->valid(),
					"mapped identity-reuse fixture reserves its target session");
			auto map_b = coordinator.begin_reader_map(*session_b, request_b);
			require(map_b && map_b->valid(),
					"mapped identity-reuse fixture begins its target native map");
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto map_b_record =
				std::ranges::find(before.map_attempts,
								  request_b.expected_attachment,
								  &sqlite_shm_reader_map_attempt_test_view::attachment);
			const auto session_b_record =
				std::ranges::find(before.session_reservations,
								  request_b.expected_attachment,
								  &sqlite_shm_reader_session_reservation_test_view::attachment);
			require(map_b_record != before.map_attempts.end() &&
						session_b_record != before.session_reservations.end() &&
						before.outstanding_terminal_permit_count == 7U,
					"mapped identity-reuse fixture lacks source-group plus target ownership");
			const auto map_b_token = map_b_record->map_token;
			const auto session_b_token = session_b_record->session_token;
			const auto receipt_b = sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
				request_b, writer.holder.generation(), mapping(0, &page, 4096U), effect);
			auto rejected = coordinator.commit_reader_map(*map_b, receipt_b, *session_b);
			const auto lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto* map_quarantine = find_reader_terminal_quarantine(lifecycle, map_b_token);
			const auto* session_quarantine =
				find_reader_terminal_quarantine(lifecycle, session_b_token);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::receipt_mismatch &&
					rejected.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					!map_b->valid() && !session_b->valid() && session_a->valid() &&
					handoff_a->valid() && coordinator.snapshot().quarantined &&
					coordinator.snapshot().reader_attachment_group_count == 1U &&
					coordinator.snapshot().reader_attachment_audit_count == 1U &&
					lifecycle.last_issued_sequence == before.last_issued_sequence + 2U &&
					lifecycle.last_committed_sequence == lifecycle.last_issued_sequence &&
					lifecycle.outstanding_terminal_permit_count == 3U &&
					lifecycle.outstanding_terminal_permit_slots.size() == 3U &&
					lifecycle.attachment_groups.size() == 1U &&
					lifecycle.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::active &&
					map_quarantine != nullptr &&
					map_quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::presented_invalid &&
					map_quarantine->terminal_sequence == before.last_issued_sequence + 1U &&
					!map_quarantine->callback && !map_quarantine->native_effect_receipt &&
					!map_quarantine->exact_terminal_receipt_retained &&
					session_quarantine != nullptr &&
					session_quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::presented_invalid &&
					session_quarantine->terminal_sequence == before.last_issued_sequence + 2U &&
					!session_quarantine->exact_terminal_receipt_retained,
				"reused mapped effect did not terminally reject only its target owner while "
				"preserving the source group and exact audit");
			auto replay = coordinator.commit_reader_map(*map_b, receipt_b, *session_b);
			require(!replay &&
						replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
					"reused mapped effect left a second target completion path");

			const auto session_terminal =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					session_request_a,
					sqlite_shm_reader_session_terminal_kind::success,
					identity("test.reader-reused-map-source-session-terminal", marker));
			require(coordinator.complete_reader_session(*session_a, session_terminal).has_value(),
					"drain source session after mapped identity-reuse quarantine");
			const auto unmap_callback = callback(7, marker + 3U);
			auto unmap = coordinator.begin_reader_unmap(
				*handoff_a, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
			require(unmap && !handoff_a->valid(),
					"admit source group drain after mapped identity-reuse quarantine");
			const auto unmap_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
					*unmap,
					unmap_callback,
					sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
					sqlite_ok_status,
					0,
					0,
					identity("test.reader-reused-map-source-unmap-effect", marker),
					identity("test.reader-reused-map-source-latch", marker));
			auto drained = coordinator.complete_reader_unmap(*unmap, unmap_receipt);
			require(drained &&
						drained->kind() ==
							sqlite_shm_reader_unmap_terminal_kind::retired_confirmed &&
						sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator)
								.outstanding_terminal_permit_count == 0U,
					"mapped identity-reuse quarantine stranded the source group's owned drain");
		}

		{
			constexpr std::uint8_t marker = 208U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto source = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			const auto shared_effect = identity("test.reader-group-zero-resize", marker + 1U);
			const auto target_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 2U),
										  marker + 2U,
										  3,
										  marker + 2U,
										  0,
										  writer.holder.generation());
			const auto target_session_request = reader_session_request(target_request, marker + 2U);
			auto target_session = coordinator.begin_reader_session(target_session_request);
			require(target_session && target_session->valid(),
					"mapped-to-zero replay fixture reserves its target session");
			auto target_map = coordinator.begin_reader_map(*target_session, target_request);
			require(target_map && target_map->valid(),
					"mapped-to-zero replay fixture begins its target map");
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto target_map_record =
				std::ranges::find(before.map_attempts,
								  target_request.expected_attachment,
								  &sqlite_shm_reader_map_attempt_test_view::attachment);
			require(target_map_record != before.map_attempts.end() &&
						before.outstanding_terminal_permit_count == 7U,
					"mapped-to-zero replay fixture lacks source and target permits");
			const auto receipt =
				sqlite_same_process_shm_lease_test_peer::reader_attachment_zero_effect(
					*target_map,
					sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
					target_request,
					sqlite_busy_status,
					nullptr,
					shared_effect);
			auto rejected = coordinator.complete_reader_zero_attachment_map(
				*target_map, receipt, *target_session);
			const auto lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto* target_quarantine =
				find_reader_terminal_quarantine(lifecycle, target_map_record->map_token);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::receipt_mismatch &&
					rejected.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					!target_map->valid() && !target_session->valid() && source.session.valid() &&
					source.handoff.valid() && coordinator.snapshot().quarantined &&
					coordinator.snapshot().reader_attachment_group_count == 1U &&
					coordinator.snapshot().reader_attachment_audit_count == 1U &&
					coordinator.snapshot().reader_attachment_zero_effect_terminal_count == 0U &&
					lifecycle.last_issued_sequence == before.last_issued_sequence + 2U &&
					lifecycle.last_committed_sequence == lifecycle.last_issued_sequence &&
					lifecycle.outstanding_terminal_permit_count == 3U &&
					lifecycle.outstanding_terminal_permit_slots.size() == 3U &&
					target_quarantine != nullptr &&
					target_quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::presented_invalid &&
					!target_quarantine->exact_terminal_receipt_retained,
				"mapped effect reused as zero-effect did not terminally reject only the target "
				"while preserving the source audit and drain");
			const auto source_terminal =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					source.session_request,
					sqlite_shm_reader_session_terminal_kind::success,
					identity("test.reader-mapped-to-zero-source-terminal", marker));
			require(
				coordinator.complete_reader_session(source.session, source_terminal).has_value(),
				"drain mapped-to-zero source session");
			const auto unmap_callback = callback(7, marker + 3U);
			auto unmap = coordinator.begin_reader_unmap(
				source.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
			require(unmap.has_value(), "admit mapped-to-zero source group drain");
			const auto unmap_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
					*unmap,
					unmap_callback,
					sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
					sqlite_ok_status,
					0,
					0,
					identity("test.reader-mapped-to-zero-source-unmap", marker),
					identity("test.reader-mapped-to-zero-source-latch", marker));
			require(coordinator.complete_reader_unmap(*unmap, unmap_receipt).has_value() &&
						sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator)
								.outstanding_terminal_permit_count == 0U,
					"mapped-to-zero replay stranded the source group drain");
		}

		{
			constexpr std::uint8_t marker = 214U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto source_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 1U),
										  marker + 1U,
										  2,
										  marker + 1U,
										  0,
										  writer.holder.generation());
			const auto source_session_request = reader_session_request(source_request, marker + 1U);
			auto source_session = coordinator.begin_reader_session(source_session_request);
			require(source_session.has_value(),
					"zero-to-mapped replay fixture reserves source session");
			auto source_map = coordinator.begin_reader_map(*source_session, source_request);
			require(source_map.has_value(), "zero-to-mapped replay fixture begins source map");
			const auto shared_effect = identity("test.reader-zero-to-mapped-effect", marker);
			const auto source_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_attachment_zero_effect(
					*source_map,
					sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
					source_request,
					sqlite_busy_status,
					nullptr,
					shared_effect);
			require(coordinator
						.complete_reader_zero_attachment_map(
							*source_map, source_receipt, *source_session)
						.has_value(),
					"zero-to-mapped replay fixture stores source exact terminal");
			const auto source_lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				source_lifecycle.zero_effect_terminals.size() == 1U &&
					source_lifecycle.zero_effect_terminals.front().zero_attachment_effect_receipt &&
					*source_lifecycle.zero_effect_terminals.front()
							.zero_attachment_effect_receipt == shared_effect,
				"zero-to-mapped source terminal lacks its exact retained effect");

			const auto target_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 2U),
										  marker + 2U,
										  3,
										  marker + 2U,
										  0,
										  writer.holder.generation());
			const auto target_session_request = reader_session_request(target_request, marker + 2U);
			auto target_session = coordinator.begin_reader_session(target_session_request);
			auto target_map = coordinator.begin_reader_map(*target_session, target_request);
			require(target_session && target_map,
					"zero-to-mapped replay fixture begins target native map");
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto target_map_record =
				std::ranges::find(before.map_attempts,
								  target_request.expected_attachment,
								  &sqlite_shm_reader_map_attempt_test_view::attachment);
			require(target_map_record != before.map_attempts.end() &&
						before.outstanding_terminal_permit_count == 4U,
					"zero-to-mapped replay fixture lacks exact target permits");
			const auto target_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
					target_request,
					writer.holder.generation(),
					mapping(0, &page, 4096U),
					shared_effect);
			auto rejected =
				coordinator.commit_reader_map(*target_map, target_receipt, *target_session);
			const auto lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto* target_quarantine =
				find_reader_terminal_quarantine(lifecycle, target_map_record->map_token);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::receipt_mismatch &&
					rejected.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					!target_map->valid() && !target_session->valid() &&
					coordinator.snapshot().quarantined &&
					coordinator.snapshot().reader_attachment_group_count == 0U &&
					coordinator.snapshot().reader_attachment_audit_count == 0U &&
					coordinator.snapshot().reader_attachment_zero_effect_terminal_count == 1U &&
					lifecycle.last_issued_sequence == before.last_issued_sequence + 2U &&
					lifecycle.last_committed_sequence == lifecycle.last_issued_sequence &&
					lifecycle.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(lifecycle) &&
					all_reader_live_custody_released(lifecycle) && target_quarantine != nullptr &&
					target_quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::presented_invalid &&
					!target_quarantine->exact_terminal_receipt_retained &&
					lifecycle.zero_effect_terminals.size() == 1U &&
					lifecycle.zero_effect_terminals.front().zero_attachment_effect_receipt &&
					*lifecycle.zero_effect_terminals.front().zero_attachment_effect_receipt ==
						shared_effect,
				"zero-effect reused as mapped effect did not preserve only the source terminal "
				"while closing target permits and custody");
		}

		{
			constexpr std::uint8_t marker = 204U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto request_a =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 1U),
										  marker + 1U,
										  2,
										  marker + 1U,
										  0,
										  writer.holder.generation());
			const auto request_b =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 2U),
										  marker + 2U,
										  3,
										  marker + 2U,
										  0,
										  writer.holder.generation());
			const auto session_request_a = reader_session_request(request_a, marker + 1U);
			const auto session_request_b = reader_session_request(request_b, marker + 2U);
			auto session_a = coordinator.begin_reader_session(session_request_a);
			auto session_b = coordinator.begin_reader_session(session_request_b);
			require(session_a && session_b,
					"session terminal identity-reuse fixture reserves two owners");
			const auto terminal_identity = identity("test.reader-reused-session-terminal", marker);
			const auto terminal_a =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					session_request_a,
					sqlite_shm_reader_session_terminal_kind::cancelled_before_authority_read,
					terminal_identity);
			require(coordinator.complete_reader_session(*session_a, terminal_a).has_value(),
					"session identity-reuse fixture stores its first exact terminal");
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto session_b_record =
				std::ranges::find(before.session_reservations,
								  request_b.expected_attachment,
								  &sqlite_shm_reader_session_reservation_test_view::attachment);
			require(session_b_record != before.session_reservations.end() &&
						before.outstanding_terminal_permit_count == 1U,
					"session identity-reuse fixture lacks its exact target permit");
			const auto session_b_token = session_b_record->session_token;
			const auto terminal_b =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					session_request_b,
					sqlite_shm_reader_session_terminal_kind::cancelled_before_authority_read,
					terminal_identity);
			auto rejected = coordinator.complete_reader_session(*session_b, terminal_b);
			const auto lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto* session_quarantine =
				find_reader_terminal_quarantine(lifecycle, session_b_token);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::receipt_mismatch &&
					rejected.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					!session_b->valid() && coordinator.snapshot().quarantined &&
					coordinator.snapshot().reader_session_terminal_count == 1U &&
					lifecycle.last_issued_sequence == before.last_issued_sequence + 1U &&
					lifecycle.last_committed_sequence == lifecycle.last_issued_sequence &&
					lifecycle.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(lifecycle) &&
					all_reader_live_custody_released(lifecycle) && session_quarantine != nullptr &&
					session_quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::presented_invalid &&
					session_quarantine->terminal_sequence == before.last_issued_sequence + 1U &&
					!session_quarantine->exact_terminal_receipt_retained,
				"reused session terminal identity did not preserve only the first exact "
				"terminal while closing the target owner");
			auto replay = coordinator.complete_reader_session(*session_b, terminal_b);
			require(!replay &&
						replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
					"reused session terminal identity left a second target completion path");
		}
	}

	void verify_reader_zero_attachment_terminal_commit_exception_has_no_half_publish()
	{
		constexpr std::uint8_t marker = 190;
		const auto binding = family(marker);
		const auto writer_connection = identity("test.connection", marker);
		const auto reader_connection = identity("test.connection", marker + 1U);
		const auto writer_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto writer = install_live_writer(
			coordinator, binding, writer_connection, writer_epoch, marker, &page);
		const auto map_request = reader_attachment_request(
			binding, reader_connection, marker + 1U, 2, marker + 1U, 0, writer.holder.generation());
		const auto session_request = reader_session_request(map_request, marker + 1U);
		auto session_result = coordinator.begin_reader_session(session_request);
		require(session_result.has_value(), "injected no-map fixture reserves a session");
		auto session = std::move(*session_result);
		auto map_result = coordinator.begin_reader_map(session, map_request);
		require(map_result.has_value(), "injected no-map fixture begins native map");
		auto inflight = std::move(*map_result);
		const auto before_failure =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto map_token = only_reader_map_attempt(before_failure).map_token;
		const auto zero_effect = identity("test.reader-zero-attachment-effect", marker);
		const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_attachment_zero_effect(
			inflight,
			sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
			map_request,
			sqlite_busy_status,
			nullptr,
			zero_effect);
		sqlite_same_process_shm_lease_test_peer::fail_next_reader_map_terminal_commit(coordinator);
		auto failed = coordinator.complete_reader_zero_attachment_map(inflight, receipt, session);
		const auto quarantined = coordinator.snapshot();
		const auto lifecycle =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto* terminal_quarantine = find_reader_terminal_quarantine(lifecycle, map_token);
		require(!failed &&
					failed.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					!inflight.valid() && !session.valid() && quarantined.quarantined &&
					quarantined.reader_inflight_count == 0U &&
					quarantined.reader_attachment_group_count == 0U &&
					quarantined.reader_session_reservation_count == 0U &&
					quarantined.reader_session_owner_count == 0U &&
					quarantined.reader_attachment_zero_effect_terminal_count == 0U &&
					quarantined.reader_attachment_revoked_no_map_count == 0U,
				"terminal-commit injection quarantines without half-published no-map evidence");
		require(
			lifecycle.outstanding_terminal_permit_count == 0U &&
				reader_terminal_permit_slots_are_exact(lifecycle) &&
				lifecycle.last_issued_sequence == lifecycle.last_committed_sequence &&
				reader_event_sequences_are_dense(lifecycle) &&
				all_reader_live_custody_released(lifecycle) && lifecycle.map_attempts.empty() &&
				lifecycle.attachment_reservations.size() == 1U &&
				lifecycle.attachment_reservations.front().phase ==
					detail::sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined &&
				lifecycle.attachment_reservations.front().destination_sequence != 0U &&
				lifecycle.session_reservations.size() == 1U &&
				lifecycle.session_reservations.front().phase ==
					detail::sqlite_shm_reader_session_reservation_phase::terminal_quarantined &&
				lifecycle.session_reservations.front().destination_sequence ==
					lifecycle.attachment_reservations.front().destination_sequence,
			"injected zero-first terminal failure consumes map/session permits and custody into "
			"one sequenced quarantine tombstone");
		require(
			terminal_quarantine != nullptr &&
				terminal_quarantine->attachment == map_request.expected_attachment &&
				terminal_quarantine->reason ==
					detail::sqlite_shm_reader_terminal_quarantine_reason::injected_commit_failure &&
				terminal_quarantine->terminal_sequence ==
					lifecycle.attachment_reservations.front().destination_sequence &&
				terminal_quarantine->callback &&
				*terminal_quarantine->callback == map_request.callback &&
				terminal_quarantine->native_effect_receipt &&
				*terminal_quarantine->native_effect_receipt == zero_effect &&
				terminal_quarantine->exact_terminal_receipt_retained,
			"injected zero-first failure retains exact callback, effect, owner, receipt, reason, "
			"and consumed terminal sequence");
		auto replay = coordinator.complete_reader_zero_attachment_map(inflight, receipt, session);
		require(!replay &&
					replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
					coordinator.snapshot().reader_attachment_zero_effect_terminal_count == 0U,
				"injected no-map terminal cannot replay into a later publication");
	}

	void verify_reserved_reader_session_terminal_commit_exception_is_exact_and_one_shot()
	{
		constexpr std::uint8_t marker = 194U;
		const auto binding = family(marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto writer = install_live_writer(coordinator,
										  binding,
										  identity("test.connection", marker),
										  identity("test.open-epoch", marker),
										  marker,
										  &page);
		const auto map_request = reader_attachment_request(binding,
														   identity("test.connection", marker + 1U),
														   marker + 1U,
														   2,
														   marker + 1U,
														   0,
														   writer.holder.generation());
		const auto session_request = reader_session_request(map_request, marker + 1U);
		auto session = coordinator.begin_reader_session(session_request);
		require(session && session->valid(), "reserve injected pre-map terminal session");
		const auto before =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto session_token = only_session_reservation(before).session_token;
		const auto terminal_receipt =
			sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				session_request,
				sqlite_shm_reader_session_terminal_kind::cancelled_before_authority_read,
				identity("test.reader-reserved-session-terminal", marker));
		sqlite_same_process_shm_lease_test_peer::fail_next_reader_session_terminal_commit(
			coordinator);
		auto failed = coordinator.complete_reader_session(*session, terminal_receipt);
		const auto lifecycle =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto* terminal_quarantine = find_reader_terminal_quarantine(lifecycle, session_token);
		require(
			!failed &&
				failed.error().reason == sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				!session->valid() && coordinator.snapshot().quarantined &&
				before.outstanding_terminal_permit_count == 1U &&
				lifecycle.outstanding_terminal_permit_count == 0U &&
				reader_terminal_permit_slots_are_exact(lifecycle) &&
				all_reader_live_custody_released(lifecycle) &&
				lifecycle.last_issued_sequence == lifecycle.last_committed_sequence &&
				reader_event_sequences_are_dense(lifecycle) &&
				lifecycle.attachment_reservations.size() == 1U &&
				lifecycle.attachment_reservations.front().phase ==
					detail::sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined &&
				lifecycle.session_reservations.size() == 1U &&
				lifecycle.session_reservations.front().phase ==
					detail::sqlite_shm_reader_session_reservation_phase::terminal_quarantined &&
				lifecycle.session_reservations.front().destination_sequence ==
					lifecycle.attachment_reservations.front().destination_sequence,
			"injected reserved-session terminal consumes its accepted permit and custody into "
			"one conservative terminal quarantine");
		require(
			terminal_quarantine != nullptr &&
				terminal_quarantine->attachment == session_request.attachment &&
				terminal_quarantine->reason ==
					detail::sqlite_shm_reader_terminal_quarantine_reason::injected_commit_failure &&
				terminal_quarantine->terminal_sequence ==
					lifecycle.session_reservations.front().destination_sequence &&
				!terminal_quarantine->callback && !terminal_quarantine->native_effect_receipt &&
				terminal_quarantine->exact_terminal_receipt_retained,
			"injected reserved-session terminal retains exact owner, receipt, reason, and "
			"consumed sequence");
		auto replay = coordinator.complete_reader_session(*session, terminal_receipt);
		require(!replay && replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
				"injected reserved-session terminal is one-shot");
	}

	void verify_reader_map_terminal_commit_exception_is_exact_and_one_shot()
	{
		constexpr std::uint8_t marker = 120;
		const auto binding = family(marker);
		const auto writer_connection = identity("test.connection", marker);
		const auto reader_connection = identity("test.connection", marker + 1U);
		const auto writer_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto writer = install_live_writer(
			coordinator, binding, writer_connection, writer_epoch, marker, &page);
		auto map_request = reader_attachment_request(
			binding, reader_connection, marker + 1U, 2, 50, 0, writer.holder.generation());
		const auto session_request = reader_session_request(map_request, 50);
		auto session_result = coordinator.begin_reader_session(session_request);
		require(session_result.has_value(), "map-terminal fixture reserves a reader session");
		auto session = std::move(*session_result);
		auto first_map_result = coordinator.begin_reader_map(session, map_request);
		require(first_map_result.has_value(), "map-terminal fixture begins its first native map");
		auto first_map = std::move(*first_map_result);
		auto first_commit_result = coordinator.commit_reader_map(
			first_map,
			sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
				map_request,
				writer.holder.generation(),
				mapping(0, &page, 4096U),
				identity("test.zero-reader-resize", 50)),
			session);
		require(first_commit_result.has_value(), "map-terminal fixture forms an exact group");
		auto first_commit = std::move(*first_commit_result);
		auto handoff_result = first_commit.take_handoff();
		require(handoff_result.has_value(), "map-terminal fixture retains its group handoff");
		auto handoff = std::move(*handoff_result);

		map_request.callback = callback(2, 51);
		auto revalidation_result = coordinator.begin_reader_map(session, map_request);
		require(revalidation_result.has_value(),
				"map-terminal fixture begins one exact revalidation");
		auto revalidation = std::move(*revalidation_result);
		const auto before_failure =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto map_token = only_reader_map_attempt(before_failure).map_token;
		const auto zero_resize_effect = identity("test.zero-reader-resize", 51);
		const auto post_native_receipt =
			sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
				map_request,
				writer.holder.generation(),
				mapping(0, &page, 4096U),
				zero_resize_effect);
		sqlite_same_process_shm_lease_test_peer::fail_next_reader_map_terminal_commit(coordinator);
		auto failed = coordinator.commit_reader_map(revalidation, post_native_receipt, session);
		const auto quarantined = coordinator.snapshot();
		const auto lifecycle =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto* terminal_quarantine = find_reader_terminal_quarantine(lifecycle, map_token);
		require(!failed && !revalidation.valid() && !session.valid() && !handoff.valid() &&
					quarantined.quarantined &&
					quarantined.phase == sqlite_shm_mapping_generation_phase::quarantined &&
					quarantined.reader_inflight_count == 0U &&
					quarantined.reader_attachment_group_count == 1U &&
					quarantined.reader_attachment_live_member_count == 0U &&
					quarantined.reader_attachment_audit_count == 1U &&
					quarantined.reader_session_reservation_count == 0U &&
					quarantined.reader_session_owner_count == 0U &&
					quarantined.reader_session_terminal_count == 0U &&
					!quarantined.reader_admission_visible,
				"post-native map exception hides the exact map, session, and existing group "
				"without publishing an audit");
		require(
			lifecycle.outstanding_terminal_permit_count == 0U &&
				reader_terminal_permit_slots_are_exact(lifecycle) &&
				lifecycle.last_issued_sequence == lifecycle.last_committed_sequence &&
				reader_event_sequences_are_dense(lifecycle) &&
				all_reader_live_custody_released(lifecycle) && lifecycle.map_attempts.empty() &&
				lifecycle.attachment_reservations.size() == 1U &&
				lifecycle.attachment_reservations.front().phase ==
					detail::sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined &&
				lifecycle.session_reservations.size() == 1U &&
				lifecycle.session_reservations.front().phase ==
					detail::sqlite_shm_reader_session_reservation_phase::terminal_quarantined &&
				lifecycle.attachment_groups.size() == 1U &&
				lifecycle.attachment_groups.front().phase ==
					detail::sqlite_shm_reader_attachment_group_phase::terminal_quarantined &&
				lifecycle.attachment_reservations.front().destination_sequence != 0U &&
				terminal_quarantine != nullptr &&
				terminal_quarantine->terminal_sequence <
					lifecycle.session_reservations.front().destination_sequence &&
				lifecycle.session_reservations.front().destination_sequence <
					lifecycle.attachment_reservations.front().destination_sequence &&
				lifecycle.attachment_groups.front().destination_sequence ==
					lifecycle.attachment_reservations.front().destination_sequence,
			"injected post-native map failure consumes session/map/group permits and all live "
			"custody into one sequenced durable quarantine");
		require(
			terminal_quarantine != nullptr &&
				terminal_quarantine->attachment == map_request.expected_attachment &&
				terminal_quarantine->reason ==
					detail::sqlite_shm_reader_terminal_quarantine_reason::injected_commit_failure &&
				terminal_quarantine->terminal_sequence <
					lifecycle.session_reservations.front().destination_sequence &&
				terminal_quarantine->callback &&
				*terminal_quarantine->callback == map_request.callback &&
				terminal_quarantine->native_effect_receipt &&
				*terminal_quarantine->native_effect_receipt == zero_resize_effect &&
				terminal_quarantine->exact_terminal_receipt_retained,
			"injected positive map failure retains exact callback, effect, owner, receipt, reason, "
			"and consumed terminal sequence");

		auto replay = coordinator.commit_reader_map(revalidation, post_native_receipt, session);
		const auto replayed = coordinator.snapshot();
		require(!replay &&
					replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
					replayed.reader_attachment_live_member_count == 0U &&
					replayed.reader_attachment_audit_count == 1U &&
					replayed.reader_session_terminal_count == 0U,
				"post-native map exception leaves no valid wrapper or successful retry");
	}

	void verify_session_terminal_rejects_a_native_started_map_after_receipt_retention()
	{
		constexpr std::uint8_t marker = 128U;
		const auto binding = family(marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto writer = install_live_writer(coordinator,
										  binding,
										  identity("test.connection", marker),
										  identity("test.open-epoch", marker),
										  marker,
										  &page);
		const auto map_request = reader_attachment_request(binding,
														   identity("test.connection", marker + 1U),
														   marker + 1U,
														   2,
														   marker + 1U,
														   0,
														   writer.holder.generation());
		const auto session_request = reader_session_request(map_request, marker + 1U);
		auto session = coordinator.begin_reader_session(session_request);
		require(session && session->valid(),
				"active-map session-terminal fixture reserves its first session");
		auto map = coordinator.begin_reader_map(*session, map_request);
		require(map && map->valid(), "active-map session-terminal fixture begins one native map");
		const auto before =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(before.map_attempts.size() == 1U && before.session_reservations.size() == 1U &&
					before.outstanding_terminal_permit_count == 4U,
				"active-map session-terminal fixture lacks exact four-slot ownership");
		const auto map_token = before.map_attempts.front().map_token;
		const auto session_token = before.session_reservations.front().session_token;
		const auto terminal_identity = identity("test.reader-active-map-session-terminal", marker);
		const auto terminal = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
			session_request,
			sqlite_shm_reader_session_terminal_kind::cancelled_before_authority_read,
			terminal_identity);
		auto rejected = coordinator.complete_reader_session(*session, terminal);
		const auto lifecycle =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto* map_quarantine = find_reader_terminal_quarantine(lifecycle, map_token);
		const auto* session_quarantine = find_reader_terminal_quarantine(lifecycle, session_token);
		require(
			!rejected &&
				rejected.error().reason == sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				!session->valid() && !map->valid() && coordinator.snapshot().quarantined &&
				lifecycle.last_issued_sequence == before.last_issued_sequence + 2U &&
				lifecycle.last_committed_sequence == lifecycle.last_issued_sequence &&
				lifecycle.outstanding_terminal_permit_count == 0U &&
				reader_terminal_permit_slots_are_exact(lifecycle) &&
				all_reader_live_custody_released(lifecycle) && lifecycle.map_attempts.empty() &&
				lifecycle.attachment_reservations.size() == 1U &&
				lifecycle.attachment_reservations.front().phase ==
					detail::sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined &&
				lifecycle.session_reservations.size() == 1U &&
				lifecycle.session_reservations.front().phase ==
					detail::sqlite_shm_reader_session_reservation_phase::terminal_quarantined &&
				map_quarantine != nullptr &&
				map_quarantine->reason ==
					detail::sqlite_shm_reader_terminal_quarantine_reason::internal_failure &&
				map_quarantine->terminal_sequence == before.last_issued_sequence + 1U &&
				!map_quarantine->exact_terminal_receipt_retained && session_quarantine != nullptr &&
				session_quarantine->reason ==
					detail::sqlite_shm_reader_terminal_quarantine_reason::internal_failure &&
				session_quarantine->terminal_sequence == before.last_issued_sequence + 2U &&
				session_quarantine->exact_terminal_receipt_retained,
			"session terminal beside a native-started map did not retain its exact receipt and "
			"consume every owner into ordered internal-failure quarantine");

		const auto map_receipt = sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
			map_request,
			writer.holder.generation(),
			mapping(0, &page, 4096U),
			identity("test.reader-active-map-native-effect", marker));
		const auto before_replay =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		auto session_replay = coordinator.complete_reader_session(*session, terminal);
		auto map_replay = coordinator.commit_reader_map(*map, map_receipt, *session);
		const auto after_replay =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(!session_replay && !map_replay &&
					session_replay.error().reason ==
						sqlite_shm_lease_rejection_reason::stale_token &&
					map_replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
					after_replay.last_issued_sequence == before_replay.last_issued_sequence &&
					after_replay.last_committed_sequence == before_replay.last_committed_sequence &&
					after_replay.outstanding_terminal_permit_slots ==
						before_replay.outstanding_terminal_permit_slots &&
					after_replay.terminal_quarantines.size() ==
						before_replay.terminal_quarantines.size(),
				"active-map/session terminal contradiction left a second completion path");
	}

	void verify_reader_session_terminal_commit_exception_is_exact_and_one_shot()
	{
		constexpr std::uint8_t marker = 122;
		const auto binding = family(marker);
		const auto writer_connection = identity("test.connection", marker);
		const auto reader_connection = identity("test.connection", marker + 1U);
		const auto writer_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto writer = install_live_writer(
			coordinator, binding, writer_connection, writer_epoch, marker, &page);
		const auto map_request = reader_attachment_request(
			binding, reader_connection, marker + 1U, 2, 60, 0, writer.holder.generation());
		const auto session_request = reader_session_request(map_request, 60);
		auto session_result = coordinator.begin_reader_session(session_request);
		require(session_result.has_value(), "session-terminal fixture reserves a reader session");
		auto session = std::move(*session_result);
		auto map_result = coordinator.begin_reader_map(session, map_request);
		require(map_result.has_value(), "session-terminal fixture begins its first native map");
		auto map = std::move(*map_result);
		auto commit_result = coordinator.commit_reader_map(
			map,
			sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
				map_request,
				writer.holder.generation(),
				mapping(0, &page, 4096U),
				identity("test.zero-reader-resize", 60)),
			session);
		require(commit_result.has_value(), "session-terminal fixture forms an exact group");
		auto commit = std::move(*commit_result);
		auto handoff_result = commit.take_handoff();
		require(handoff_result.has_value(), "session-terminal fixture retains its group handoff");
		auto handoff = std::move(*handoff_result);

		const auto terminal_receipt =
			sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				session_request,
				sqlite_shm_reader_session_terminal_kind::success,
				identity("test.reader-session-terminal", 60));
		const auto before_failure =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto session_token = only_session_reservation(before_failure).session_token;
		sqlite_same_process_shm_lease_test_peer::fail_next_reader_session_terminal_commit(
			coordinator);
		auto failed = coordinator.complete_reader_session(session, terminal_receipt);
		const auto quarantined = coordinator.snapshot();
		const auto lifecycle =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto* terminal_quarantine = find_reader_terminal_quarantine(lifecycle, session_token);
		require(!failed && !session.valid() && !handoff.valid() && quarantined.quarantined &&
					quarantined.phase == sqlite_shm_mapping_generation_phase::quarantined &&
					quarantined.reader_attachment_group_count == 1U &&
					quarantined.reader_attachment_live_member_count == 0U &&
					quarantined.reader_attachment_audit_count == 1U &&
					quarantined.reader_session_reservation_count == 0U &&
					quarantined.reader_session_owner_count == 0U &&
					quarantined.reader_session_terminal_count == 0U &&
					!quarantined.reader_admission_visible,
				"terminal tombstone exception hides the exact session and group without "
				"publishing terminal success");
		require(
			lifecycle.outstanding_terminal_permit_count == 0U &&
				reader_terminal_permit_slots_are_exact(lifecycle) &&
				lifecycle.last_issued_sequence == lifecycle.last_committed_sequence &&
				reader_event_sequences_are_dense(lifecycle) &&
				all_reader_live_custody_released(lifecycle) &&
				lifecycle.attachment_reservations.size() == 1U &&
				lifecycle.attachment_reservations.front().phase ==
					detail::sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined &&
				lifecycle.session_reservations.size() == 1U &&
				lifecycle.session_reservations.front().phase ==
					detail::sqlite_shm_reader_session_reservation_phase::terminal_quarantined &&
				lifecycle.attachment_groups.size() == 1U &&
				lifecycle.attachment_groups.front().phase ==
					detail::sqlite_shm_reader_attachment_group_phase::terminal_quarantined &&
				lifecycle.attachment_reservations.front().destination_sequence != 0U &&
				lifecycle.session_reservations.front().destination_sequence <
					lifecycle.attachment_reservations.front().destination_sequence &&
				lifecycle.attachment_groups.front().destination_sequence ==
					lifecycle.attachment_reservations.front().destination_sequence,
			"injected session terminal failure consumes session/group permits and all live "
			"custody into one sequenced durable quarantine");
		require(
			terminal_quarantine != nullptr &&
				terminal_quarantine->attachment == session_request.attachment &&
				terminal_quarantine->reason ==
					detail::sqlite_shm_reader_terminal_quarantine_reason::injected_commit_failure &&
				terminal_quarantine->terminal_sequence ==
					lifecycle.session_reservations.front().destination_sequence &&
				!terminal_quarantine->callback && !terminal_quarantine->native_effect_receipt &&
				terminal_quarantine->exact_terminal_receipt_retained,
			"injected session terminal failure retains exact owner, receipt, reason, and consumed "
			"terminal sequence without inventing callback/effect authority");

		auto replay = coordinator.complete_reader_session(session, terminal_receipt);
		const auto replayed = coordinator.snapshot();
		require(!replay &&
					replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
					replayed.reader_attachment_live_member_count == 0U &&
					replayed.reader_attachment_audit_count == 1U &&
					replayed.reader_session_terminal_count == 0U,
				"terminal tombstone exception disarms the session and permits no retry");
	}

	void verify_reader_recovery_mutex_reacquire_failure_preserves_exact_wrappers()
	{
		struct reserved_reader_fixture
		{
			live_writer_tokens writer;
			sqlite_shm_reader_attachment_map_request map_request;
			sqlite_shm_reader_session_request session_request;
			sqlite_shm_reader_session session;
		};
		const auto reserve_reader =
			[](sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
			   const sqlite_shm_lease_family_binding& binding,
			   const std::uint8_t marker,
			   const volatile void* page)
		{
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  page);
			auto map_request = reader_attachment_request(binding,
														 identity("test.connection", marker + 1U),
														 marker + 1U,
														 2,
														 marker + 1U,
														 0,
														 writer.holder.generation());
			auto session_request = reader_session_request(map_request, marker + 1U);
			auto session = coordinator.begin_reader_session(session_request);
			require(session.has_value(), "reserve recovery-lock reader session");
			return reserved_reader_fixture{std::move(writer),
										   std::move(map_request),
										   std::move(session_request),
										   std::move(*session)};
		};

		{
			constexpr std::uint8_t marker = 33U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto map_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 1U),
										  marker + 1U,
										  2,
										  marker + 1U,
										  0,
										  writer.holder.generation());
			{
				auto session_result = coordinator.begin_reader_session(
					reader_session_request(map_request, marker + 1U));
				require(session_result.has_value(),
						"map recovery-lock fixture reserves a reader session");
				auto session = std::move(*session_result);
				auto inflight_result = coordinator.begin_reader_map(session, map_request);
				require(inflight_result.has_value(),
						"map recovery-lock fixture begins a native map");
				auto inflight = std::move(*inflight_result);
				const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
					map_request,
					writer.holder.generation(),
					mapping(0, &page, 4096U),
					identity("test.reader-recovery-lock-map-effect", marker));
				sqlite_same_process_shm_lease_test_peer::fail_next_reader_map_terminal_commit(
					coordinator);
				sqlite_same_process_shm_lease_test_peer::fail_next_reader_recovery_mutex_reacquire(
					coordinator);
				auto failed = coordinator.commit_reader_map(inflight, receipt, session);
				require(!failed &&
							failed.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							!inflight.valid() && !session.valid() &&
							coordinator.snapshot().quarantined,
						"recovery-lock failure disables map and session terminal presentation "
						"while retaining abandonment custody");
				const auto before_replay =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				auto replayed = coordinator.commit_reader_map(inflight, receipt, session);
				const auto after_replay =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				require(!replayed &&
							replayed.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							!inflight.valid() && !session.valid() &&
							after_replay.last_issued_sequence ==
								before_replay.last_issued_sequence &&
							after_replay.last_committed_sequence ==
								before_replay.last_committed_sequence &&
							after_replay.outstanding_terminal_permit_slots ==
								before_replay.outstanding_terminal_permit_slots &&
							after_replay.map_attempts.size() == before_replay.map_attempts.size() &&
							after_replay.attachment_groups.size() ==
								before_replay.attachment_groups.size(),
						"map recovery double-fault retains only abandonment custody and no "
						"second terminal presentation path");
			}
			const auto abandoned =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(abandoned.outstanding_terminal_permit_count == 0U &&
						reader_terminal_permit_slots_are_exact(abandoned) &&
						all_reader_live_custody_released(abandoned),
					"retained map/session wrappers did not transfer ownership through abandonment");
		}

		{
			constexpr std::uint8_t marker = 38U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto map_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 1U),
										  marker + 1U,
										  2,
										  marker + 1U,
										  0,
										  writer.holder.generation());
			const auto session_request = reader_session_request(map_request, marker + 1U);
			{
				auto session_result = coordinator.begin_reader_session(session_request);
				require(session_result.has_value(),
						"zero-effect recovery fixture reserves a reader session");
				auto session = std::move(*session_result);
				auto inflight_result = coordinator.begin_reader_map(session, map_request);
				require(inflight_result.has_value(),
						"zero-effect recovery fixture begins its native map");
				auto inflight = std::move(*inflight_result);
				const auto receipt =
					sqlite_same_process_shm_lease_test_peer::reader_attachment_zero_effect(
						inflight,
						sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
						map_request,
						sqlite_busy_status,
						nullptr,
						identity("test.reader-recovery-lock-zero-effect", marker));
				sqlite_same_process_shm_lease_test_peer::fail_next_reader_operation_mutex_acquire(
					coordinator);
				sqlite_same_process_shm_lease_test_peer::fail_next_reader_recovery_mutex_reacquire(
					coordinator);
				auto failed =
					coordinator.complete_reader_zero_attachment_map(inflight, receipt, session);
				require(!failed &&
							failed.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							!inflight.valid() && !session.valid() &&
							coordinator.snapshot().quarantined,
						"zero-effect terminal double-fault disables terminal presentation while "
						"retaining poisoned abandonment custody");
				const auto before_replay =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				auto replayed =
					coordinator.complete_reader_zero_attachment_map(inflight, receipt, session);
				const auto after_replay =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				require(!replayed &&
							replayed.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							!inflight.valid() && !session.valid() &&
							after_replay.last_issued_sequence ==
								before_replay.last_issued_sequence &&
							after_replay.last_committed_sequence ==
								before_replay.last_committed_sequence &&
							after_replay.outstanding_terminal_permit_slots ==
								before_replay.outstanding_terminal_permit_slots &&
							after_replay.zero_effect_terminals.size() ==
								before_replay.zero_effect_terminals.size(),
						"zero-effect recovery double-fault retains no second terminal publication "
						"path");
			}
			const auto abandoned =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(abandoned.outstanding_terminal_permit_count == 0U &&
						reader_terminal_permit_slots_are_exact(abandoned) &&
						all_reader_live_custody_released(abandoned),
					"dropped zero-effect recovery handles release every retained custody");
		}

		{
			constexpr std::uint8_t marker = 43U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto map_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 1U),
										  marker + 1U,
										  2,
										  marker + 1U,
										  0,
										  writer.holder.generation());
			const auto session_request = reader_session_request(map_request, marker + 1U);
			{
				auto session_result = coordinator.begin_reader_session(session_request);
				require(session_result.has_value(),
						"session recovery-lock fixture reserves a reader session");
				auto session = std::move(*session_result);
				const auto receipt =
					sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
						session_request,
						sqlite_shm_reader_session_terminal_kind::cancelled_before_authority_read,
						identity("test.reader-recovery-lock-session-terminal", marker));
				sqlite_same_process_shm_lease_test_peer::fail_next_reader_session_terminal_commit(
					coordinator);
				sqlite_same_process_shm_lease_test_peer::fail_next_reader_recovery_mutex_reacquire(
					coordinator);
				auto failed = coordinator.complete_reader_session(session, receipt);
				require(!failed &&
							failed.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							!session.valid() && coordinator.snapshot().quarantined,
						"recovery-lock failure disables session terminal presentation while "
						"retaining abandonment custody");
				const auto before_replay =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				auto replayed = coordinator.complete_reader_session(session, receipt);
				const auto after_replay =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				require(!replayed &&
							replayed.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							!session.valid() &&
							after_replay.last_issued_sequence ==
								before_replay.last_issued_sequence &&
							after_replay.last_committed_sequence ==
								before_replay.last_committed_sequence &&
							after_replay.outstanding_terminal_permit_slots ==
								before_replay.outstanding_terminal_permit_slots &&
							after_replay.session_reservations.size() ==
								before_replay.session_reservations.size(),
						"session recovery double-fault retains only abandonment custody and no "
						"second terminal presentation path");
			}
			const auto abandoned =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(abandoned.outstanding_terminal_permit_count == 0U &&
						reader_terminal_permit_slots_are_exact(abandoned) &&
						all_reader_live_custody_released(abandoned),
					"retained session wrapper did not transfer ownership through abandonment");
		}

		{
			constexpr std::uint8_t marker = 53U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			{
				auto reader = install_live_reader_group(coordinator,
														binding,
														identity("test.connection", marker + 1U),
														marker + 1U,
														writer.holder.generation(),
														&page);
				const auto session_terminal =
					sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
						reader.session_request,
						sqlite_shm_reader_session_terminal_kind::success,
						identity("test.reader-recovery-lock-unmap-session", marker));
				require(coordinator.complete_reader_session(reader.session, session_terminal)
							.has_value(),
						"unmap recovery-lock fixture terminalizes its reader session");
				const auto unmap_callback = callback(7, marker + 2U);
				auto unmap_result = coordinator.begin_reader_unmap(
					reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
				require(unmap_result.has_value(),
						"unmap recovery-lock fixture admits its native obligation");
				auto unmap = std::move(*unmap_result);
				sqlite_same_process_shm_lease_test_peer::throw_reader_unmap_terminal_exception(
					coordinator);
				sqlite_same_process_shm_lease_test_peer::fail_next_reader_recovery_mutex_reacquire(
					coordinator);
				auto failed = coordinator.complete_reader_unmap(
					unmap, unmap_callback, sqlite_shm_native_cleanup_outcome::confirmed_success);
				require(!failed &&
							failed.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							!unmap.valid() && coordinator.snapshot().quarantined,
						"recovery-lock failure disables unmap terminal presentation while "
						"retaining abandonment custody");
				const auto before_replay =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				auto replayed = coordinator.complete_reader_unmap(
					unmap, unmap_callback, sqlite_shm_native_cleanup_outcome::confirmed_success);
				const auto after_replay =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				require(!replayed &&
							replayed.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							!unmap.valid() &&
							after_replay.last_issued_sequence ==
								before_replay.last_issued_sequence &&
							after_replay.last_committed_sequence ==
								before_replay.last_committed_sequence &&
							after_replay.outstanding_terminal_permit_slots ==
								before_replay.outstanding_terminal_permit_slots &&
							after_replay.attachment_groups.size() ==
								before_replay.attachment_groups.size(),
						"unmap recovery double-fault retains only abandonment custody and no "
						"second native terminal presentation path");
			}
			const auto abandoned =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(abandoned.outstanding_terminal_permit_count == 0U &&
						reader_terminal_permit_slots_are_exact(abandoned) &&
						all_reader_live_custody_released(abandoned),
					"retained unmap wrapper did not transfer ownership through abandonment");
		}

		{
			constexpr std::uint8_t marker = 58U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			{
				auto reader = install_live_reader_group(coordinator,
														binding,
														identity("test.connection", marker + 1U),
														marker + 1U,
														writer.holder.generation(),
														&page);
				const auto session_terminal =
					sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
						reader.session_request,
						sqlite_shm_reader_session_terminal_kind::success,
						identity("test.reader-exact-double-lock-session", marker));
				require(coordinator.complete_reader_session(reader.session, session_terminal)
							.has_value(),
						"exact unmap double-lock fixture terminalizes its session");
				const auto unmap_callback = callback(7U, marker + 2U);
				auto unmap_result = coordinator.begin_reader_unmap(
					reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
				require(unmap_result && unmap_result->valid(),
						"exact unmap double-lock fixture admits its native obligation");
				auto unmap = std::move(*unmap_result);
				const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
					unmap,
					unmap_callback,
					sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
					sqlite_ok_status,
					0,
					0,
					identity("test.reader-exact-double-lock-effect", marker),
					identity("test.reader-exact-double-lock-latch", marker));
				sqlite_same_process_shm_lease_test_peer::fail_next_reader_operation_mutex_acquire(
					coordinator);
				sqlite_same_process_shm_lease_test_peer::fail_next_reader_recovery_mutex_reacquire(
					coordinator);
				auto failed = coordinator.complete_reader_unmap(unmap, receipt);
				require(!failed &&
							failed.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							!unmap.valid() && coordinator.snapshot().quarantined,
						"exact unmap double-lock fault disables terminal presentation while "
						"retaining poisoned abandonment custody");
				const auto before_replay =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				auto replayed = coordinator.complete_reader_unmap(unmap, receipt);
				const auto after_replay =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				require(!replayed &&
							replayed.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							!unmap.valid() &&
							after_replay.last_issued_sequence ==
								before_replay.last_issued_sequence &&
							after_replay.last_committed_sequence ==
								before_replay.last_committed_sequence &&
							after_replay.outstanding_terminal_permit_slots ==
								before_replay.outstanding_terminal_permit_slots &&
							after_replay.attachment_groups.size() ==
								before_replay.attachment_groups.size(),
						"exact unmap double-lock fault cannot republish native terminal evidence");
			}
			const auto abandoned =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(abandoned.outstanding_terminal_permit_count == 0U &&
						reader_terminal_permit_slots_are_exact(abandoned) &&
						all_reader_live_custody_released(abandoned),
					"dropped exact-unmap double-fault handle releases all retained custody");
		}

		{
			constexpr std::uint8_t marker = 63U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			{
				auto reader = install_live_reader_group(coordinator,
														binding,
														identity("test.connection", marker + 1U),
														marker + 1U,
														writer.holder.generation(),
														&page);
				const auto session_terminal =
					sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
						reader.session_request,
						sqlite_shm_reader_session_terminal_kind::success,
						identity("test.reader-recovery-lock-begin-session", marker));
				require(coordinator.complete_reader_session(reader.session, session_terminal)
							.has_value(),
						"begin recovery-lock fixture terminalizes its reader session");
				sqlite_same_process_shm_lease_test_peer::fail_reader_unmap_begin_preparation(
					coordinator);
				sqlite_same_process_shm_lease_test_peer::fail_next_reader_recovery_mutex_reacquire(
					coordinator);
				auto failed = coordinator.begin_reader_unmap(
					reader.handoff,
					sqlite_shm_reader_unmap_request{callback(7, marker + 2U), 0, 0});
				require(!failed &&
							failed.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							reader.handoff.valid() && coordinator.snapshot().quarantined,
						"recovery-lock failure disarmed a handoff before exact state transition");
			}
			const auto abandoned =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(abandoned.outstanding_terminal_permit_count == 0U &&
						reader_terminal_permit_slots_are_exact(abandoned) &&
						all_reader_live_custody_released(abandoned),
					"retained handoff wrapper did not transfer ownership through abandonment");
		}

		{
			constexpr std::uint8_t target_marker = 83U;
			constexpr std::uint8_t source_marker = 93U;
			const auto target_binding = family(target_marker);
			const auto source_binding = family(source_marker);
			auto target_generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			auto source_generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator target{target_binding,
																	 target_generations};
			sqlite_same_process_shm_mapping_lease_coordinator source{source_binding,
																	 source_generations};
			int target_page{};
			int source_page{};
			auto target_reader =
				reserve_reader(target, target_binding, target_marker, &target_page);
			auto source_reader =
				reserve_reader(source, source_binding, source_marker, &source_page);
			auto target_map_result =
				target.begin_reader_map(target_reader.session, target_reader.map_request);
			auto source_map_result =
				source.begin_reader_map(source_reader.session, source_reader.map_request);
			require(target_map_result.has_value() && source_map_result.has_value(),
					"cross-owner map recovery fixtures begin matching native maps");
			auto target_map = std::move(*target_map_result);
			auto source_map = std::move(*source_map_result);
			const auto target_before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(target);
			const auto source_before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(source);
			require(only_reader_map_attempt(target_before).map_token ==
							only_reader_map_attempt(source_before).map_token &&
						only_session_reservation(target_before).session_token ==
							only_session_reservation(source_before).session_token,
					"cross-owner map recovery fixtures do not expose colliding local tokens");
			const auto source_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
					source_reader.map_request,
					source_reader.writer.holder.generation(),
					mapping(0, &source_page, 4096U),
					identity("test.reader-operation-lock-map-effect", source_marker));
			sqlite_same_process_shm_lease_test_peer::fail_next_reader_operation_mutex_acquire(
				target);
			auto failed =
				target.commit_reader_map(source_map, source_receipt, source_reader.session);
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						target_map.valid() && target_reader.session.valid() && source_map.valid() &&
						source_reader.session.valid() && target.snapshot().quarantined,
					"map recovery consumed colliding records or wrappers from another coordinator");
		}

		{
			constexpr std::uint8_t target_marker = 103U;
			constexpr std::uint8_t source_marker = 113U;
			const auto target_binding = family(target_marker);
			const auto source_binding = family(source_marker);
			auto target_generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			auto source_generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator target{target_binding,
																	 target_generations};
			sqlite_same_process_shm_mapping_lease_coordinator source{source_binding,
																	 source_generations};
			int target_page{};
			int source_page{};
			auto target_reader =
				reserve_reader(target, target_binding, target_marker, &target_page);
			auto source_reader =
				reserve_reader(source, source_binding, source_marker, &source_page);
			const auto target_before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(target);
			const auto source_before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(source);
			require(only_session_reservation(target_before).session_token ==
						only_session_reservation(source_before).session_token,
					"cross-owner session recovery fixtures do not expose colliding local tokens");
			const auto source_terminal =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					source_reader.session_request,
					sqlite_shm_reader_session_terminal_kind::cancelled_before_authority_read,
					identity("test.reader-operation-lock-session-terminal", source_marker));
			sqlite_same_process_shm_lease_test_peer::fail_next_reader_operation_mutex_acquire(
				target);
			auto failed = target.complete_reader_session(source_reader.session, source_terminal);
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						target_reader.session.valid() && source_reader.session.valid() &&
						target.snapshot().quarantined,
					"session recovery consumed a colliding wrapper from another coordinator");
		}

		{
			constexpr std::uint8_t target_marker = 123U;
			constexpr std::uint8_t source_marker = 133U;
			const auto target_binding = family(target_marker);
			const auto source_binding = family(source_marker);
			auto target_generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			auto source_generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator target{target_binding,
																	 target_generations};
			sqlite_same_process_shm_mapping_lease_coordinator source{source_binding,
																	 source_generations};
			int target_page{};
			int source_page{};
			auto target_writer = install_live_writer(target,
													 target_binding,
													 identity("test.connection", target_marker),
													 identity("test.open-epoch", target_marker),
													 target_marker,
													 &target_page);
			auto source_writer = install_live_writer(source,
													 source_binding,
													 identity("test.connection", source_marker),
													 identity("test.open-epoch", source_marker),
													 source_marker,
													 &source_page);
			auto target_reader =
				install_live_reader_group(target,
										  target_binding,
										  identity("test.connection", target_marker + 1U),
										  target_marker + 1U,
										  target_writer.holder.generation(),
										  &target_page);
			auto source_reader =
				install_live_reader_group(source,
										  source_binding,
										  identity("test.connection", source_marker + 1U),
										  source_marker + 1U,
										  source_writer.holder.generation(),
										  &source_page);
			const auto target_terminal =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					target_reader.session_request,
					sqlite_shm_reader_session_terminal_kind::success,
					identity("test.reader-operation-lock-begin-session", target_marker));
			const auto source_terminal =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					source_reader.session_request,
					sqlite_shm_reader_session_terminal_kind::success,
					identity("test.reader-operation-lock-begin-session", source_marker));
			require(target.complete_reader_session(target_reader.session, target_terminal)
							.has_value() &&
						source.complete_reader_session(source_reader.session, source_terminal)
							.has_value(),
					"cross-owner begin recovery fixtures terminalize matching sessions");
			require(sqlite_same_process_shm_lease_test_peer::reader_handoff_token(
						target_reader.handoff) ==
						sqlite_same_process_shm_lease_test_peer::reader_handoff_token(
							source_reader.handoff),
					"cross-owner begin recovery fixtures do not expose colliding local tokens");
			sqlite_same_process_shm_lease_test_peer::fail_next_reader_operation_mutex_acquire(
				target);
			auto failed = target.begin_reader_unmap(
				source_reader.handoff,
				sqlite_shm_reader_unmap_request{callback(7, source_marker + 2U), 0, 0});
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						target_reader.handoff.valid() && source_reader.handoff.valid() &&
						target.snapshot().quarantined,
					"begin recovery consumed a colliding handoff from another coordinator");
		}

		{
			constexpr std::uint8_t target_marker = 143U;
			constexpr std::uint8_t source_marker = 153U;
			const auto target_binding = family(target_marker);
			const auto source_binding = family(source_marker);
			auto target_generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			auto source_generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator target{target_binding,
																	 target_generations};
			sqlite_same_process_shm_mapping_lease_coordinator source{source_binding,
																	 source_generations};
			int target_page{};
			int source_page{};
			auto target_writer = install_live_writer(target,
													 target_binding,
													 identity("test.connection", target_marker),
													 identity("test.open-epoch", target_marker),
													 target_marker,
													 &target_page);
			auto source_writer = install_live_writer(source,
													 source_binding,
													 identity("test.connection", source_marker),
													 identity("test.open-epoch", source_marker),
													 source_marker,
													 &source_page);
			auto target_reader =
				install_live_reader_group(target,
										  target_binding,
										  identity("test.connection", target_marker + 1U),
										  target_marker + 1U,
										  target_writer.holder.generation(),
										  &target_page);
			auto source_reader =
				install_live_reader_group(source,
										  source_binding,
										  identity("test.connection", source_marker + 1U),
										  source_marker + 1U,
										  source_writer.holder.generation(),
										  &source_page);
			const auto target_terminal =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					target_reader.session_request,
					sqlite_shm_reader_session_terminal_kind::success,
					identity("test.reader-operation-lock-unmap-session", target_marker));
			const auto source_terminal =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					source_reader.session_request,
					sqlite_shm_reader_session_terminal_kind::success,
					identity("test.reader-operation-lock-unmap-session", source_marker));
			require(target.complete_reader_session(target_reader.session, target_terminal)
							.has_value() &&
						source.complete_reader_session(source_reader.session, source_terminal)
							.has_value(),
					"cross-owner unmap recovery fixtures terminalize matching sessions");
			const auto target_callback = callback(7, target_marker + 2U);
			const auto source_callback = callback(7, source_marker + 2U);
			auto target_unmap_result = target.begin_reader_unmap(
				target_reader.handoff, sqlite_shm_reader_unmap_request{target_callback, 0, 0});
			auto source_unmap_result = source.begin_reader_unmap(
				source_reader.handoff, sqlite_shm_reader_unmap_request{source_callback, 0, 0});
			require(target_unmap_result.has_value() && source_unmap_result.has_value(),
					"cross-owner unmap recovery fixtures admit matching native obligations");
			auto target_unmap = std::move(*target_unmap_result);
			auto source_unmap = std::move(*source_unmap_result);
			const auto target_before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(target);
			const auto source_before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(source);
			require(
				last_reader_lifecycle_event_owner(
					target_before, detail::sqlite_shm_reader_lifecycle_event_kind::unmap_cut) ==
					last_reader_lifecycle_event_owner(
						source_before, detail::sqlite_shm_reader_lifecycle_event_kind::unmap_cut),
				"cross-owner unmap recovery fixtures do not expose colliding local tokens");
			sqlite_same_process_shm_lease_test_peer::fail_next_reader_operation_mutex_acquire(
				target);
			auto failed =
				target.complete_reader_unmap(source_unmap,
											 source_callback,
											 sqlite_shm_native_cleanup_outcome::confirmed_success);
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						target_unmap.valid() && source_unmap.valid() &&
						target.snapshot().quarantined,
					"unmap recovery consumed a colliding wrapper from another coordinator");
		}

		{
			constexpr std::uint8_t marker = 73U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto request = reader_request(
				binding, identity("test.connection", marker + 1U), marker + 1U, 2, marker + 1U);
			auto inflight = coordinator.begin_reader_map(request);
			require(inflight.has_value(), "legacy recovery fixture begins its reader map");
			auto handoff = coordinator.promote_reader(
				*inflight,
				sqlite_same_process_shm_lease_test_peer::reader_map(
					request,
					writer.holder.generation(),
					mapping(0, &page, 4096U),
					identity("test.reader-recovery-lock-legacy-map", marker)));
			require(handoff.has_value(), "legacy recovery fixture promotes its reader handoff");
			const auto unmap_callback = callback(7, marker + 2U);
			auto unmap = coordinator.begin_reader_unmap(*handoff, unmap_callback);
			require(unmap.has_value(), "legacy recovery fixture admits its unmap");
			sqlite_same_process_shm_lease_test_peer::throw_reader_unmap_terminal_exception(
				coordinator);
			auto failed = coordinator.complete_reader_unmap(
				*unmap, unmap_callback, sqlite_shm_native_cleanup_outcome::confirmed_success);
			const auto snapshot = coordinator.snapshot();
			require(!failed && !unmap->valid() && snapshot.quarantined &&
						snapshot.reader_handoff_count == 1U && snapshot.reader_cleanup_count >= 1U,
					"successful legacy recovery did not transition and disarm its exact wrapper");
		}
	}

	void verify_reader_terminal_callback_invocations_cannot_authorize_writer_release()
	{
		const auto require_replay_rejected =
			[](sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
			   sqlite_shm_writer_holder& holder,
			   const sqlite_shm_callback_execution_receipt& replayed_callback,
			   const std::string_view context)
		{
			const auto before_snapshot = coordinator.snapshot();
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			auto replayed = coordinator.release_writer_holder(holder, replayed_callback);
			const auto after_snapshot = coordinator.snapshot();
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(!replayed &&
						replayed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
					context);
			require(!holder.valid(), "callback replay cannot retain writer native-call authority");
			require(after_snapshot.generation_authority_count <=
							before_snapshot.generation_authority_count &&
						after.last_issued_sequence == before.last_issued_sequence &&
						after.last_committed_sequence == before.last_committed_sequence &&
						after.outstanding_terminal_permit_slots ==
							before.outstanding_terminal_permit_slots,
					"callback replay publishes no generation authority or reader lifecycle event");
		};

		{
			constexpr std::uint8_t marker = 181U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto map_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 1U),
										  marker + 1U,
										  2U,
										  marker + 1U,
										  0,
										  writer.holder.generation());
			auto session =
				coordinator.begin_reader_session(reader_session_request(map_request, marker + 1U));
			require(session && session->valid(),
					"map-callback quarantine fixture reserves its session");
			auto map = coordinator.begin_reader_map(*session, map_request);
			require(map && map->valid(), "map-callback quarantine fixture begins native work");
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
				map_request,
				writer.holder.generation(),
				mapping(0, &page, 4096U),
				identity("test.reader-callback-quarantine-effect", marker));
			sqlite_same_process_shm_lease_test_peer::fail_next_reader_map_terminal_commit(
				coordinator);
			auto failed = coordinator.commit_reader_map(*map, receipt, *session);
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						!map->valid() && !session->valid(),
					"retain map-terminal ambiguity before callback replay");
			require_replay_rejected(
				coordinator,
				writer.holder,
				callback(200U, marker + 1U),
				"map-terminal quarantine invocation cannot authorize writer native cleanup");
		}

		{
			constexpr std::uint8_t marker = 185U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto reader = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			const auto session_terminal =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					reader.session_request,
					sqlite_shm_reader_session_terminal_kind::success,
					identity("test.reader-callback-abandoned-unmap-session", marker));
			require(
				coordinator.complete_reader_session(reader.session, session_terminal).has_value(),
				"abandoned-unmap callback fixture terminalizes its session");
			const auto unmap_callback = callback(7U, marker + 2U);
			{
				auto unmap = coordinator.begin_reader_unmap(
					reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
				require(unmap && unmap->valid(), "admit callback fixture abandoned unmap cut");
			}
			require(coordinator.snapshot().quarantined,
					"dropping the unmap cut retains a conservative terminal row");
			require_replay_rejected(
				coordinator,
				writer.holder,
				callback(201U, marker + 2U),
				"abandoned/unknown unmap-cut invocation cannot authorize writer native cleanup");
		}

		{
			constexpr std::uint8_t marker = 189U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto reader = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			const auto session_terminal =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					reader.session_request,
					sqlite_shm_reader_session_terminal_kind::success,
					identity("test.reader-callback-confirmed-unmap-session", marker));
			require(
				coordinator.complete_reader_session(reader.session, session_terminal).has_value(),
				"confirmed-unmap callback fixture terminalizes its session");
			const auto unmap_callback = callback(7U, marker + 2U);
			auto unmap = coordinator.begin_reader_unmap(
				reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
			require(unmap && unmap->valid(), "admit callback fixture confirmed unmap");
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
				*unmap,
				unmap_callback,
				sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
				sqlite_ok_status,
				0,
				0,
				identity("test.reader-callback-confirmed-unmap-effect", marker),
				identity("test.reader-callback-confirmed-unmap-latch", marker));
			auto completed = coordinator.complete_reader_unmap(*unmap, receipt);
			require(completed &&
						completed->kind() ==
							sqlite_shm_reader_unmap_terminal_kind::retired_confirmed &&
						!unmap->valid(),
					"retain exact confirmed-unmap callback receipt");
			require_replay_rejected(
				coordinator,
				writer.holder,
				callback(202U, marker + 2U),
				"confirmed-unmap receipt invocation cannot authorize writer native cleanup");
		}

		{
			constexpr std::uint8_t marker = 193U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto open = register_reader_open(
				coordinator, 871U, reader_open_epoch_binding(binding, marker + 1U));
			const auto close_callback = callback(12U, marker + 2U);
			{
				auto close = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
					coordinator,
					open.registry_open_token,
					open.seal,
					open.binding,
					sqlite_shm_reader_close_request{close_callback});
				require(close && close->valid(), "admit callback fixture abandoned close cut");
			}
			require(coordinator.snapshot().quarantined,
					"dropping the close cut retains a conservative terminal row");
			require_replay_rejected(
				coordinator,
				writer.holder,
				callback(203U, marker + 2U),
				"abandoned close-cut invocation cannot authorize writer native cleanup");
		}
	}

	void verify_reader_effect_identities_are_nonreusable_across_map_and_unmap_roles()
	{
		for (std::size_t index = 0U; index < 2U; ++index)
		{
			const auto marker = static_cast<std::uint8_t>(197U + index * 3U);
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto reader = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			const auto map_effect = identity("test.reader-group-zero-resize", marker + 1U);
			const auto session_terminal =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					reader.session_request,
					sqlite_shm_reader_session_terminal_kind::success,
					identity("test.reader-map-to-unmap-effect-session", marker));
			require(
				coordinator.complete_reader_session(reader.session, session_terminal).has_value(),
				"map-to-unmap effect fixture terminalizes its session");
			const auto unmap_callback = callback(7U, marker + 2U);
			auto unmap = coordinator.begin_reader_unmap(
				reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
			require(unmap && unmap->valid(), "admit map-to-unmap effect fixture native cut");
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
				*unmap,
				unmap_callback,
				sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
				sqlite_ok_status,
				0,
				0,
				index == 0U ? map_effect
							: identity("test.reader-map-to-unmap-fresh-effect", marker),
				index == 0U ? identity("test.reader-map-to-unmap-fresh-latch", marker)
							: map_effect);
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			auto rejected = coordinator.complete_reader_unmap(*unmap, receipt);
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					!unmap->valid() && coordinator.snapshot().quarantined &&
					before.attachment_groups.size() == 1U && after.attachment_groups.size() == 1U &&
					after.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::terminal_quarantined &&
					after.close_terminals.empty(),
				index == 0U ? "successful map effect cannot be reused as same-group unmap effect"
							: "successful map effect cannot be reused as same-group unmap latch");
		}

		for (std::size_t index = 0U; index < 2U; ++index)
		{
			const auto marker = static_cast<std::uint8_t>(205U + index * 4U);
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto source_reader = install_live_reader_group(coordinator,
														   binding,
														   identity("test.connection", marker + 1U),
														   marker + 1U,
														   writer.holder.generation(),
														   &page);
			const auto source_session_terminal =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					source_reader.session_request,
					sqlite_shm_reader_session_terminal_kind::success,
					identity("test.reader-unmap-to-map-effect-session", marker));
			require(
				coordinator.complete_reader_session(source_reader.session, source_session_terminal)
					.has_value(),
				"unmap-to-map effect fixture terminalizes its source session");
			const auto unmap_callback = callback(7U, marker + 2U);
			auto unmap = coordinator.begin_reader_unmap(
				source_reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
			require(unmap && unmap->valid(), "admit unmap-to-map source terminal");
			const auto unmap_effect = identity("test.reader-unmap-to-map-effect", marker);
			const auto unmap_latch = identity("test.reader-unmap-to-map-latch", marker);
			const auto unmap_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
					*unmap,
					unmap_callback,
					sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
					sqlite_ok_status,
					0,
					0,
					unmap_effect,
					unmap_latch);
			auto unmap_completed = coordinator.complete_reader_unmap(*unmap, unmap_receipt);
			require(unmap_completed &&
						unmap_completed->kind() ==
							sqlite_shm_reader_unmap_terminal_kind::retired_confirmed &&
						!unmap->valid(),
					"retain exact unmap effect and latch before fresh map");

			const auto fresh_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 3U),
										  marker + 3U,
										  8U,
										  marker + 3U,
										  0,
										  writer.holder.generation());
			auto fresh_session = coordinator.begin_reader_session(
				reader_session_request(fresh_request, marker + 3U));
			require(fresh_session && fresh_session->valid(),
					"fresh remap reserves a distinct attachment epoch");
			auto fresh_map = coordinator.begin_reader_map(*fresh_session, fresh_request);
			require(fresh_map && fresh_map->valid(), "fresh remap reaches its native terminal");
			const auto reused_identity = index == 0U ? unmap_effect : unmap_latch;
			const auto fresh_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
					fresh_request,
					writer.holder.generation(),
					mapping(0, &page, 4096U),
					reused_identity);
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			auto rejected =
				coordinator.commit_reader_map(*fresh_map, fresh_receipt, *fresh_session);
			const auto after_snapshot = coordinator.snapshot();
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(!rejected &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch &&
						rejected.error().action ==
							sqlite_shm_lease_recovery_action::quarantine_no_retry &&
						!fresh_map->valid() && !fresh_session->valid() &&
						after_snapshot.quarantined &&
						after_snapshot.reader_attachment_live_member_count == 0U &&
						after_snapshot.reader_attachment_audit_count == 1U &&
						after.last_issued_sequence > before.last_issued_sequence &&
						after.last_committed_sequence == after.last_issued_sequence,
					index == 0U ? "confirmed unmap effect cannot authorize a fresh map effect"
								: "confirmed unmap latch cannot authorize a fresh map effect");
		}
	}

	void verify_writer_terminal_identities_cannot_authorize_reader_work()
	{
		for (std::size_t index = 0U; index < 2U; ++index)
		{
			const auto marker = static_cast<std::uint8_t>(217U + index * 5U);
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto original = install_live_writer(coordinator,
												binding,
												identity("test.connection", marker),
												identity("test.open-epoch", marker),
												marker,
												&page);
			std::optional<live_writer_tokens> successor;
			auto active_generation = original.holder.generation();
			if (index == 1U)
			{
				retire_last(coordinator, original.holder, callback(3U, marker + 1U));
				successor.emplace(install_live_writer(coordinator,
													  binding,
													  identity("test.connection", marker + 2U),
													  identity("test.open-epoch", marker + 2U),
													  marker + 2U,
													  &page));
				active_generation = successor->holder.generation();
			}

			auto reader_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 3U),
										  marker + 3U,
										  201U,
										  marker + 3U,
										  0,
										  active_generation);
			reader_request.callback = callback(201U, marker);
			const auto session_request = reader_session_request(reader_request, marker + 3U);
			auto session = coordinator.begin_reader_session(session_request);
			require(session && session->valid(),
					"writer-map callback replay fixture reserves a fresh reader session");
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			auto rejected = coordinator.begin_reader_map(*session, reader_request);
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!rejected &&
					rejected.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
					session->valid() && after.last_issued_sequence == before.last_issued_sequence &&
					after.last_committed_sequence == before.last_committed_sequence &&
					after.outstanding_terminal_permit_slots ==
						before.outstanding_terminal_permit_slots &&
					after.map_attempts.size() == before.map_attempts.size(),
				index == 0U
					? "live writer-map callback invocation cannot start a reader map"
					: "retired writer sealed-audit callback invocation cannot start a reader map");
			const auto cancelled = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				session_request,
				sqlite_shm_reader_session_terminal_kind::cancelled_before_authority_read,
				identity("test.writer-callback-reader-session-terminal", marker));
			require(coordinator.complete_reader_session(*session, cancelled).has_value(),
					"terminalize callback-replay reader session without native work");
		}

		for (std::size_t index = 0U; index < 2U; ++index)
		{
			const auto marker = static_cast<std::uint8_t>(227U + index * 5U);
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto original = install_live_writer(coordinator,
												binding,
												identity("test.connection", marker),
												identity("test.open-epoch", marker),
												marker,
												&page);
			const auto original_effect = identity("test.holder-effect", marker);
			std::optional<live_writer_tokens> successor;
			auto active_generation = original.holder.generation();
			if (index == 1U)
			{
				retire_last(coordinator, original.holder, callback(3U, marker + 1U));
				successor.emplace(install_live_writer(coordinator,
													  binding,
													  identity("test.connection", marker + 2U),
													  identity("test.open-epoch", marker + 2U),
													  marker + 2U,
													  &page));
				active_generation = successor->holder.generation();
			}

			const auto reader_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 3U),
										  marker + 3U,
										  202U,
										  marker + 3U,
										  0,
										  active_generation);
			auto session = coordinator.begin_reader_session(
				reader_session_request(reader_request, marker + 3U));
			require(session && session->valid(),
					"writer-map effect replay fixture reserves a fresh reader session");
			auto map = coordinator.begin_reader_map(*session, reader_request);
			require(map && map->valid(),
					"writer-map effect replay fixture reaches the reader native terminal");
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
				reader_request, active_generation, mapping(0, &page, 4096U), original_effect);
			auto rejected = coordinator.commit_reader_map(*map, receipt, *session);
			const auto snapshot = coordinator.snapshot();
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::receipt_mismatch &&
					rejected.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					!map->valid() && !session->valid() && snapshot.quarantined &&
					snapshot.reader_attachment_live_member_count == 0U &&
					after.map_attempts.empty(),
				index == 0U
					? "live writer-map effect cannot authorize a reader map effect"
					: "retired writer sealed-audit effect cannot authorize a reader map effect");
		}

		for (std::size_t index = 0U; index < 2U; ++index)
		{
			const auto marker = static_cast<std::uint8_t>(237U + index * 5U);
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto open = register_reader_open(
				coordinator, 881U + index, reader_open_epoch_binding(binding, marker + 2U));
			const auto cleanup_callback = callback(5U, marker + 1U);
			auto retirement = coordinator.release_writer_holder(writer.holder, cleanup_callback);
			require(retirement &&
						retirement->decision() == sqlite_shm_writer_retirement_decision::ready &&
						!writer.holder.valid() && retirement->cleanup().valid(),
					"writer-cleanup callback replay fixture reaches its exact native drain");
			auto completed = coordinator.complete_writer_cleanup(
				retirement->cleanup(),
				cleanup_callback,
				index == 0U ? sqlite_shm_native_cleanup_outcome::confirmed_success
							: sqlite_shm_native_cleanup_outcome::unknown);
			require(index == 0U ? completed.has_value() : !completed,
					"writer cleanup reaches the selected retired or terminal state");

			auto rejected = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				coordinator,
				open.registry_open_token,
				open.seal,
				open.binding,
				sqlite_shm_reader_close_request{callback(203U, marker + 1U)});
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					after.open_epochs.size() == 1U &&
					after.open_epochs.front().phase ==
						detail::sqlite_shm_reader_connection_close_phase::terminal_quarantined &&
					after.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(after) &&
					all_reader_live_custody_released(after),
				index == 0U ? "retired writer-cleanup callback cannot authorize reader close"
							: "terminal writer-cleanup callback cannot authorize reader close");
		}
	}

	void verify_remaining_reader_map_terminal_commit_injection_matrix()
	{
		{
			constexpr std::uint8_t marker = 132U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto map_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 1U),
										  marker + 1U,
										  2,
										  marker + 1U,
										  0,
										  writer.holder.generation());
			const auto session_request = reader_session_request(map_request, marker + 1U);
			auto session = coordinator.begin_reader_session(session_request);
			require(session && session->valid(),
					"first-positive injection fixture reserves its session");
			auto map = coordinator.begin_reader_map(*session, map_request);
			require(map && map->valid(), "first-positive injection fixture begins its map");
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto map_token = only_reader_map_attempt(before).map_token;
			const auto native_effect =
				identity("test.reader-first-positive-injected-effect", marker);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
				map_request, writer.holder.generation(), mapping(0, &page, 4096U), native_effect);
			sqlite_same_process_shm_lease_test_peer::fail_next_reader_map_terminal_commit(
				coordinator);
			auto failed = coordinator.commit_reader_map(*map, receipt, *session);
			const auto lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto* map_quarantine = find_reader_terminal_quarantine(lifecycle, map_token);
			require(
				!failed &&
					failed.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					!map->valid() && !session->valid() && coordinator.snapshot().quarantined &&
					lifecycle.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(lifecycle) &&
					lifecycle.last_issued_sequence == lifecycle.last_committed_sequence &&
					reader_event_sequences_are_dense(lifecycle) &&
					all_reader_live_custody_released(lifecycle) && lifecycle.map_attempts.empty() &&
					lifecycle.attachment_reservations.size() == 1U &&
					lifecycle.attachment_reservations.front().phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::
							terminal_quarantined &&
					!lifecycle.attachment_reservations.front().group_payload_present &&
					lifecycle.session_reservations.size() == 1U &&
					lifecycle.session_reservations.front().phase ==
						detail::sqlite_shm_reader_session_reservation_phase::terminal_quarantined &&
					lifecycle.attachment_groups.empty() && map_quarantine != nullptr &&
					map_quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::
							injected_commit_failure &&
					map_quarantine->terminal_sequence <
						lifecycle.session_reservations.front().destination_sequence &&
					lifecycle.session_reservations.front().destination_sequence <
						lifecycle.attachment_reservations.front().destination_sequence &&
					map_quarantine->callback && *map_quarantine->callback == map_request.callback &&
					map_quarantine->native_effect_receipt &&
					*map_quarantine->native_effect_receipt == native_effect &&
					map_quarantine->exact_terminal_receipt_retained,
				"first positive map terminal injection retains its exact native receipt and "
				"consumes map/session/potential-group permits into ordered quarantine");
		}

		{
			constexpr std::uint8_t marker = 136U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto reader = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			auto peer_request = reader.session_request;
			peer_request.read_transaction_epoch =
				identity("test.reader-peer-transaction", marker + 2U);
			peer_request.decode_attempt = identity("test.reader-peer-decode", marker + 2U);
			peer_request.authority_read_receipt =
				identity("test.reader-peer-authority", marker + 2U);
			auto peer_session = coordinator.begin_reader_session(peer_request);
			require(peer_session && peer_session->valid() &&
						peer_session->phase() ==
							sqlite_shm_reader_session_phase::active_group_owner,
					"reserve an exact peer session before later zero-map injection");

			auto later_map_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 1U),
										  marker + 1U,
										  2,
										  marker + 1U,
										  0,
										  writer.holder.generation());
			later_map_request.callback = callback(3, marker + 3U);
			auto later_map = coordinator.begin_reader_map(reader.session, later_map_request);
			require(later_map && later_map->valid(),
					"begin later zero-attachment map beside an established peer session");
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto map_token = only_reader_map_attempt(before).map_token;
			const auto peer_token =
				last_reader_lifecycle_event_owner(before,
												  detail::sqlite_shm_reader_lifecycle_event_kind::
													  use_session_owner_promotion_or_admission);
			const auto peer_before =
				std::ranges::find(before.session_reservations,
								  peer_token,
								  &sqlite_shm_reader_session_reservation_test_view::session_token);
			require(peer_before != before.session_reservations.end(),
					"locate peer owner before later zero-map injection");
			const auto zero_effect =
				identity("test.reader-later-zero-injected-effect", marker + 4U);
			const auto zero_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_attachment_zero_effect(
					*later_map,
					sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
					later_map_request,
					sqlite_busy_status,
					nullptr,
					zero_effect);
			sqlite_same_process_shm_lease_test_peer::fail_next_reader_map_terminal_commit(
				coordinator);
			auto failed = coordinator.complete_reader_zero_attachment_map(
				*later_map, zero_receipt, reader.session);
			const auto quarantined =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto* map_quarantine = find_reader_terminal_quarantine(quarantined, map_token);
			const auto peer_after =
				std::ranges::find(quarantined.session_reservations,
								  peer_token,
								  &sqlite_shm_reader_session_reservation_test_view::session_token);
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						!later_map->valid() && !reader.session.valid() && peer_session->valid() &&
						!reader.handoff.valid() && coordinator.snapshot().quarantined &&
						quarantined.outstanding_terminal_permit_count == 1U &&
						reader_terminal_permit_slots_are_exact(quarantined) &&
						map_quarantine != nullptr &&
						map_quarantine->reason ==
							detail::sqlite_shm_reader_terminal_quarantine_reason::
								injected_commit_failure &&
						map_quarantine->callback &&
						*map_quarantine->callback == later_map_request.callback &&
						map_quarantine->native_effect_receipt &&
						*map_quarantine->native_effect_receipt == zero_effect &&
						map_quarantine->exact_terminal_receipt_retained &&
						peer_after != quarantined.session_reservations.end() &&
						peer_after->phase == peer_before->phase &&
						peer_after->origin_sequence == peer_before->origin_sequence &&
						peer_after->destination_sequence == peer_before->destination_sequence &&
						peer_after->terminal_permit_slot == peer_before->terminal_permit_slot &&
						quarantined.live_custody_kind_counts[enum_index(
							detail::sqlite_shm_reader_custody_kind::use_session)] == 1U,
					"later zero-map injection quarantines only the failing session/map/group while "
					"preserving an already-owned peer session drain");

			const auto peer_terminal =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					peer_request,
					sqlite_shm_reader_session_terminal_kind::success,
					identity("test.reader-peer-terminal", marker + 5U));
			require(coordinator.complete_reader_session(*peer_session, peer_terminal).has_value() &&
						!peer_session->valid(),
					"already-owned peer session drains after group terminal quarantine");
			const auto drained =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(drained.outstanding_terminal_permit_count == 0U &&
						reader_terminal_permit_slots_are_exact(drained) &&
						drained.last_issued_sequence == drained.last_committed_sequence &&
						reader_event_sequences_are_dense(drained) &&
						all_reader_live_custody_released(drained),
					"peer session terminal strands no permit or custody after later zero-map "
					"injection");
		}
	}

	void verify_reader_native_attachment_group_and_session_core()
	{
		constexpr std::uint8_t marker = 118;
		const auto binding = family(marker);
		const auto writer_connection = identity("test.connection", marker);
		const auto reader_connection = identity("test.connection", marker + 1U);
		const auto writer_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page_zero{};
		int page_one{};

		auto gate =
			install_eligibility(coordinator, binding, writer_connection, writer_epoch, marker);
		auto writer_zero = writer_request(binding, writer_connection, marker, 1, marker, 0, 1);
		const auto shared_writer_attachment = writer_zero.attachment;
		auto pending_zero = install_pending(coordinator,
											writer_zero,
											writer_epoch,
											mapping(0, &page_zero, 4096U),
											sqlite_shm_writer_extend_pair::one_one,
											marker);
		auto holder_zero = promote(coordinator, pending_zero, gate);
		auto writer_one = writer_request(binding, writer_connection, marker, 2, marker + 1U, 1, 1);
		writer_one.attachment = shared_writer_attachment;
		auto pending_one = install_pending(coordinator,
										   writer_one,
										   writer_epoch,
										   mapping(1, &page_one, 8192U),
										   sqlite_shm_writer_extend_pair::one_one,
										   marker + 1U);
		auto holder_one = promote(coordinator, pending_one, gate);
		const auto generation = holder_zero.generation();
		require(holder_one.generation() == generation,
				"reader group fixture seals two pages in one writer generation");

		auto reader_zero = reader_attachment_request(
			binding, reader_connection, marker + 1U, 3, 30, 0, generation);
		const auto first_session_request = reader_session_request(reader_zero, 40);
		auto first_session_result = coordinator.begin_reader_session(first_session_request);
		require(first_session_result &&
					first_session_result->phase() ==
						sqlite_shm_reader_session_phase::reserved_for_first_map,
				"first reader session reserves exact attachment before SQLite map");
		auto first_session = std::move(*first_session_result);
		const auto first_reserved_view =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(
			only_attachment_reservation(first_reserved_view).phase ==
					detail::sqlite_shm_reader_attachment_reservation_phase::reserved &&
				only_session_reservation(first_reserved_view).phase ==
					detail::sqlite_shm_reader_session_reservation_phase::reserved_before_sqlite &&
				first_reserved_view.outstanding_terminal_permit_count == 1U &&
				first_reserved_view.last_issued_sequence ==
					first_reserved_view.last_committed_sequence &&
				reader_event_sequences_are_dense(first_reserved_view) &&
				reader_terminal_permit_slots_are_exact(first_reserved_view) &&
				only_session_reservation(first_reserved_view).terminal_permit_slot != 0U &&
				first_reserved_view.last_committed_sequence ==
					only_attachment_reservation(first_reserved_view).origin_sequence &&
				first_reserved_view.last_committed_sequence ==
					only_session_reservation(first_reserved_view).origin_sequence,
			"first positive session reserves attachment and session at one sequence");
		auto duplicate = coordinator.begin_reader_session(first_session_request);
		require(!duplicate &&
					duplicate.error().reason ==
						sqlite_shm_lease_rejection_reason::receipt_mismatch &&
					first_session.valid() && !coordinator.snapshot().quarantined,
				"one exact session request has one live move-only owner");
		auto same_owner_key = first_session_request;
		same_owner_key.authority_read_receipt = identity("test.reader-authority-read-receipt", 99);
		auto authority_only_duplicate = coordinator.begin_reader_session(same_owner_key);
		require(!authority_only_duplicate &&
					authority_only_duplicate.error().reason ==
						sqlite_shm_lease_rejection_reason::receipt_mismatch,
				"authority receipt cannot mint a second owner for the same session key");
		auto epoch_collision_request =
			reader_attachment_request(binding,
									  reader_connection,
									  marker + 2U,
									  3,
									  29,
									  0,
									  generation,
									  reader_zero.expected_attachment.attachment_epoch());
		auto epoch_collision =
			coordinator.begin_reader_session(reader_session_request(epoch_collision_request, 39));
		require(!epoch_collision &&
					epoch_collision.error().reason ==
						sqlite_shm_lease_rejection_reason::receipt_mismatch &&
					!coordinator.snapshot().quarantined,
				"attachment epoch cannot be rebound to another expected identity");

		auto first_map_result = coordinator.begin_reader_map(first_session, reader_zero);
		require(first_map_result && first_session.valid(),
				"reserved session admits its exact first native map");
		auto first_map = std::move(*first_map_result);
		const auto first_map_admitted_view =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto& first_map_attempt = only_reader_map_attempt(first_map_admitted_view);
		require(first_map_admitted_view.last_committed_sequence >
						first_reserved_view.last_committed_sequence &&
					first_map_admitted_view.outstanding_terminal_permit_count == 4U &&
					first_map_admitted_view.last_issued_sequence ==
						first_map_admitted_view.last_committed_sequence &&
					reader_event_sequences_are_dense(first_map_admitted_view) &&
					reader_terminal_permit_slots_are_exact(first_map_admitted_view) &&
					first_map_attempt.admission_sequence ==
						first_map_admitted_view.last_committed_sequence &&
					first_map_attempt.terminal_permit_slot != 0U &&
					first_map_attempt.terminal_permit_slot !=
						only_session_reservation(first_reserved_view).terminal_permit_slot &&
					first_map_attempt.potential_group_cut_permit_slot != 0U &&
					first_map_attempt.potential_group_terminal_permit_slot != 0U &&
					first_map_attempt.potential_group_cut_permit_slot !=
						first_map_attempt.potential_group_terminal_permit_slot &&
					first_map_attempt.potential_group_cut_permit_slot !=
						first_map_attempt.terminal_permit_slot &&
					first_map_attempt.potential_group_terminal_permit_slot !=
						first_map_attempt.terminal_permit_slot &&
					first_map_attempt.potential_group_cut_permit_slot !=
						only_session_reservation(first_reserved_view).terminal_permit_slot &&
					first_map_attempt.potential_group_terminal_permit_slot !=
						only_session_reservation(first_reserved_view).terminal_permit_slot &&
					last_reader_lifecycle_event_sequence(
						first_map_admitted_view,
						detail::sqlite_shm_reader_lifecycle_event_kind::map_admission) ==
						first_map_admitted_view.last_committed_sequence,
				"first positive map admission reserves map terminal and potential group "
				"cut/terminal capacity at one later checked sequence");
		auto first_commit_result = coordinator.commit_reader_map(
			first_map,
			sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
				reader_zero,
				generation,
				mapping(0, &page_zero, 8192U),
				identity("test.zero-reader-resize", 40)),
			first_session);
		require(first_commit_result && !first_map.valid() &&
					first_commit_result->kind() ==
						sqlite_shm_reader_map_commit_kind::first_member &&
					first_commit_result->formed_group() &&
					first_session.phase() == sqlite_shm_reader_session_phase::active_group_owner,
				"first positive map atomically forms group and promotes session owner");
		const auto first_positive_view =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto& positive_attachment = only_attachment_reservation(first_positive_view);
		const auto& positive_session = only_session_reservation(first_positive_view);
		require(
			first_positive_view.last_committed_sequence >
					first_map_admitted_view.last_committed_sequence &&
				first_positive_view.outstanding_terminal_permit_count == 3U &&
				first_positive_view.last_issued_sequence ==
					first_positive_view.last_committed_sequence &&
				reader_event_sequences_are_dense(first_positive_view) &&
				reader_terminal_permit_slots_are_exact(first_positive_view) &&
				first_positive_view.map_attempts.empty() &&
				positive_session.terminal_permit_slot ==
					only_session_reservation(first_reserved_view).terminal_permit_slot &&
				positive_attachment.phase ==
					detail::sqlite_shm_reader_attachment_reservation_phase::observed_present &&
				positive_attachment.origin_sequence ==
					first_reserved_view.last_committed_sequence &&
				positive_attachment.destination_sequence ==
					first_positive_view.last_committed_sequence &&
				positive_attachment.group_payload_present &&
				positive_session.phase ==
					detail::sqlite_shm_reader_session_reservation_phase::promoted_to_group_owner &&
				positive_session.origin_sequence == first_reserved_view.last_committed_sequence &&
				positive_session.destination_sequence ==
					first_positive_view.last_committed_sequence &&
				first_positive_view.attachment_reservation_phase_counts[enum_index(
					detail::sqlite_shm_reader_attachment_reservation_phase::observed_present)] ==
					1U &&
				first_positive_view.session_reservation_phase_counts[enum_index(
					detail::sqlite_shm_reader_session_reservation_phase::
						promoted_to_group_owner)] == 1U &&
				first_positive_view.attachment_groups.size() == 1U &&
				first_positive_view.attachment_groups.front().phase ==
					detail::sqlite_shm_reader_attachment_group_phase::active &&
				first_positive_view.attachment_groups.front().origin_sequence ==
					first_positive_view.last_committed_sequence &&
				first_positive_view.attachment_groups.front().unmap_cut_permit_slot != 0U &&
				first_positive_view.attachment_groups.front().unmap_terminal_permit_slot != 0U &&
				first_positive_view.attachment_groups.front().unmap_cut_permit_slot ==
					first_map_attempt.potential_group_cut_permit_slot &&
				first_positive_view.attachment_groups.front().unmap_terminal_permit_slot ==
					first_map_attempt.potential_group_terminal_permit_slot &&
				first_positive_view.attachment_groups.front().unmap_cut_permit_slot !=
					first_positive_view.attachment_groups.front().unmap_terminal_permit_slot &&
				first_positive_view.attachment_groups.front().unmap_cut_permit_slot !=
					positive_session.terminal_permit_slot &&
				first_positive_view.attachment_groups.front().unmap_terminal_permit_slot !=
					positive_session.terminal_permit_slot &&
				last_reader_lifecycle_event_sequence(
					first_positive_view,
					detail::sqlite_shm_reader_lifecycle_event_kind::map_terminal) ==
					first_positive_view.last_committed_sequence &&
				last_reader_lifecycle_event_sequence(
					first_positive_view,
					detail::sqlite_shm_reader_lifecycle_event_kind::
						use_session_owner_promotion_or_admission) ==
					first_positive_view.last_committed_sequence,
			"first positive terminal atomically binds reservation, session, group, pointer owner, "
			"and map terminal to one sequence");
		auto first_commit = std::move(*first_commit_result);
		require(!first_commit_result->take_handoff(),
				"moved-from map commit retains no observable handoff");
		auto handoff_result = first_commit.take_handoff();
		require(handoff_result && handoff_result->valid() && !first_commit.take_handoff(),
				"first commit transfers the only group handoff exactly once");
		auto handoff = std::move(*handoff_result);

		auto callback_replay = coordinator.begin_reader_map(first_session, reader_zero);
		require(!callback_replay &&
					callback_replay.error().reason ==
						sqlite_shm_lease_rejection_reason::stale_token &&
					first_session.valid() && !coordinator.snapshot().quarantined,
				"completed callback invocation cannot append a duplicate group audit");
		auto reader_one = reader_zero;
		reader_one.page_number = 1;
		reader_one.callback = callback(3, 31);
		auto new_member_map = coordinator.begin_reader_map(first_session, reader_one);
		require(new_member_map.has_value(), "active session admits a new generation page");
		const auto before_overlapping_map =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto before_overlapping_snapshot = coordinator.snapshot();
		auto overlapping_request = reader_zero;
		overlapping_request.callback = callback(3, 32);
		auto overlapping_map = coordinator.begin_reader_map(first_session, overlapping_request);
		const auto after_overlapping_map =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto after_overlapping_snapshot = coordinator.snapshot();
		require(
			!overlapping_map &&
				overlapping_map.error().reason ==
					sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				first_session.valid() && !after_overlapping_snapshot.quarantined &&
				before_overlapping_map.last_issued_sequence ==
					after_overlapping_map.last_issued_sequence &&
				before_overlapping_map.last_committed_sequence ==
					after_overlapping_map.last_committed_sequence &&
				before_overlapping_map.outstanding_terminal_permit_count ==
					after_overlapping_map.outstanding_terminal_permit_count &&
				before_overlapping_map.live_custody_kind_counts ==
					after_overlapping_map.live_custody_kind_counts &&
				before_overlapping_map.custody_state_counts ==
					after_overlapping_map.custody_state_counts &&
				before_overlapping_map.events.size() == after_overlapping_map.events.size() &&
				before_overlapping_map.map_attempts.size() == 1U &&
				after_overlapping_map.map_attempts.size() == 1U &&
				before_overlapping_map.map_attempts.front().map_token ==
					after_overlapping_map.map_attempts.front().map_token &&
				before_overlapping_map.map_attempts.front().admission_sequence ==
					after_overlapping_map.map_attempts.front().admission_sequence &&
				before_overlapping_map.map_attempts.front().terminal_permit_slot ==
					after_overlapping_map.map_attempts.front().terminal_permit_slot &&
				before_overlapping_snapshot.reader_inflight_count ==
					after_overlapping_snapshot.reader_inflight_count &&
				before_overlapping_snapshot.reader_attachment_group_count ==
					after_overlapping_snapshot.reader_attachment_group_count,
			"a session with one nonterminal map rejects a second begin before sequence, custody, "
			"permit, or native-attempt publication");
		const auto release_callback = callback(4, 32);
		auto writer_release = coordinator.release_writer_holder(holder_zero, release_callback);
		require(writer_release &&
					writer_release->decision() ==
						sqlite_shm_writer_retirement_decision::wait_for_inflight &&
					holder_one.valid(),
				"pre-hide new-page map pin blocks writer native cleanup");
		auto new_member_commit = coordinator.commit_reader_map(
			*new_member_map,
			sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
				reader_one,
				generation,
				mapping(1, &page_one, 8192U),
				identity("test.zero-reader-resize", 41)),
			first_session);
		require(new_member_commit &&
					new_member_commit->kind() == sqlite_shm_reader_map_commit_kind::new_member &&
					!new_member_commit->formed_group() && !new_member_commit->take_handoff(),
				"pre-hide page pin joins during retirement without another group handoff");
		auto ready =
			coordinator.poll_writer_retirement(writer_release->cleanup(), release_callback);
		require(ready && ready->decision == sqlite_shm_writer_retirement_decision::ready,
				"writer retirement becomes ready after fresh map pin terminalizes");
		require(coordinator
					.complete_writer_cleanup(writer_release->cleanup(),
											 release_callback,
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"writer attachment retires while reader attachment group survives");
		require(coordinator.snapshot().phase == sqlite_shm_mapping_generation_phase::retired,
				"group-wide handoff retains the retired generation");

		const auto second_session_request = reader_session_request(reader_zero, 41);
		auto second_session_result = coordinator.begin_reader_session(second_session_request);
		require(second_session_result &&
					second_session_result->phase() ==
						sqlite_shm_reader_session_phase::active_group_owner,
				"new session joins an existing group after writer retirement");
		auto second_session = std::move(*second_session_result);
		auto wrong_attachment = reader_attachment_request(
			binding, reader_connection, marker + 2U, 5, 33, 0, generation);
		auto wrong_map = coordinator.begin_reader_map(second_session, wrong_attachment);
		require(!wrong_map && second_session.valid() && !coordinator.snapshot().quarantined,
				"session cannot authorize another native attachment identity");

		reader_zero.callback = callback(5, 34);
		auto revalidation_map = coordinator.begin_reader_map(second_session, reader_zero);
		require(revalidation_map.has_value(),
				"existing member remains revalidatable through the retained group");
		auto revalidation_commit = coordinator.commit_reader_map(
			*revalidation_map,
			sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
				reader_zero,
				generation,
				mapping(0, &page_zero, 8192U),
				identity("test.zero-reader-resize", 42)),
			second_session);
		require(revalidation_commit &&
					revalidation_commit->kind() ==
						sqlite_shm_reader_map_commit_kind::existing_member_revalidation &&
					!revalidation_commit->take_handoff(),
				"existing member revalidation appends audit without duplicating membership");

		auto second_terminal = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
			second_session_request,
			sqlite_shm_reader_session_terminal_kind::success,
			identity("test.reader-session-terminal", 41));
		const auto before_second_terminal =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(before_second_terminal.outstanding_terminal_permit_count == 4U &&
					reader_terminal_permit_slots_are_exact(before_second_terminal) &&
					before_second_terminal.map_attempts.empty() &&
					std::ranges::count_if(
						before_second_terminal.session_reservations,
						[](const sqlite_shm_reader_session_reservation_test_view& reservation)
						{
							return reservation.terminal_permit_slot != 0U;
						}) == 2,
				"two active sessions retain two distinct terminal permits");
		require(coordinator.complete_reader_session(second_session, second_terminal) &&
					!second_session.valid(),
				"second session consumes one exact issuer-sealed terminal receipt");
		const auto after_second_terminal =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto second_terminal_sequence = last_reader_lifecycle_event_sequence(
			after_second_terminal,
			detail::sqlite_shm_reader_lifecycle_event_kind::use_session_terminal);
		require(after_second_terminal.last_committed_sequence >
						before_second_terminal.last_committed_sequence &&
					second_terminal_sequence == after_second_terminal.last_committed_sequence &&
					after_second_terminal.outstanding_terminal_permit_count == 3U &&
					after_second_terminal.last_issued_sequence ==
						after_second_terminal.last_committed_sequence &&
					reader_event_sequences_are_dense(after_second_terminal) &&
					reader_terminal_permit_slots_are_exact(after_second_terminal) &&
					std::ranges::count_if(
						after_second_terminal.session_reservations,
						[](const sqlite_shm_reader_session_reservation_test_view& reservation)
						{
							return reservation.terminal_permit_slot != 0U;
						}) == 1,
				"second session terminal consumes one distinct lifecycle sequence");

		const auto grouped = coordinator.snapshot();
		require(grouped.reader_attachment_group_count == 1U &&
					grouped.reader_attachment_live_member_count == 2U &&
					grouped.reader_attachment_audit_count == 3U &&
					grouped.reader_session_reservation_count == 0U &&
					grouped.reader_session_owner_count == 1U &&
					grouped.reader_session_terminal_count == 1U &&
					grouped.reader_handoff_count == 1U,
				"snapshot exposes one group, two members, three audits, and exact sessions");

		const auto unmap_callback = callback(6, 35);
		auto unmap = coordinator.begin_reader_unmap(
			handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
		const auto cut_lifecycle =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto bounded_waiter_index =
			enum_index(detail::sqlite_shm_reader_custody_kind::bounded_waiter_or_continuation);
		const auto terminal_reporter_index =
			enum_index(detail::sqlite_shm_reader_custody_kind::terminal_reporter);
		const auto normal_unmap_index =
			enum_index(detail::sqlite_shm_reader_custody_kind::normal_or_deferred_unmap);
		const auto handoff_index =
			enum_index(detail::sqlite_shm_reader_custody_kind::attachment_group_handoff);
		require(unmap.has_value(), "group-wide unmap cut is admitted with a live owner");
		require(!handoff.valid() && unmap->valid() && !unmap->native_effect_ready(),
				"group-wide unmap consumes its handoff before waiting");
		require(cut_lifecycle.outstanding_terminal_permit_count == 2U &&
					cut_lifecycle.last_issued_sequence == cut_lifecycle.last_committed_sequence &&
					reader_event_sequences_are_dense(cut_lifecycle) &&
					reader_terminal_permit_slots_are_exact(cut_lifecycle),
				"unmap cut consumes one reserved lifecycle permit");
		require(cut_lifecycle.attachment_groups.size() == 1U &&
					cut_lifecycle.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::unmap_cut_sealing &&
					cut_lifecycle.attachment_groups.front().unmap_cut_permit_slot == 0U &&
					cut_lifecycle.attachment_groups.front().unmap_terminal_permit_slot != 0U,
				"group lifecycle records a sealed unmap cut");
		require(cut_lifecycle.live_custody_kind_counts[handoff_index] == 0U &&
					cut_lifecycle.live_custody_kind_counts[normal_unmap_index] == 1U &&
					cut_lifecycle.live_custody_kind_counts[bounded_waiter_index] == 1U &&
					cut_lifecycle.live_custody_kind_counts[terminal_reporter_index] == 1U &&
					!coordinator.snapshot().quarantined,
				"sealed unmap cut retains its exact continuation and reporter custodies");
		auto still_waiting = coordinator.poll_reader_unmap_cut(*unmap, unmap_callback);
		require(still_waiting &&
					still_waiting->progress == sqlite_shm_reader_unmap_cut_progress::waiting &&
					!unmap->native_effect_ready(),
				"other-thread session owner keeps the cut obligation in bounded waiting");

		auto first_terminal = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
			first_session_request,
			sqlite_shm_reader_session_terminal_kind::success,
			identity("test.reader-session-terminal", 40));
		const auto before_first_terminal =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(coordinator.complete_reader_session(first_session, first_terminal) &&
					!first_session.valid(),
				"last live session reaches one-shot terminal");
		const auto after_first_terminal =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto first_terminal_sequence = last_reader_lifecycle_event_sequence(
			after_first_terminal,
			detail::sqlite_shm_reader_lifecycle_event_kind::use_session_terminal);
		require(after_first_terminal.last_committed_sequence >
						before_first_terminal.last_committed_sequence &&
					first_terminal_sequence == after_first_terminal.last_committed_sequence &&
					first_terminal_sequence > second_terminal_sequence &&
					count_reader_lifecycle_events(
						after_first_terminal,
						detail::sqlite_shm_reader_lifecycle_event_kind::use_session_terminal) ==
						2U &&
					after_first_terminal.outstanding_terminal_permit_count == 1U &&
					after_first_terminal.last_issued_sequence ==
						after_first_terminal.last_committed_sequence &&
					reader_event_sequences_are_dense(after_first_terminal) &&
					reader_terminal_permit_slots_are_exact(after_first_terminal),
				"separate session receipts consume separate strictly ordered terminal sequences");
		auto terminal_replay = coordinator.complete_reader_session(first_session, first_terminal);
		require(!terminal_replay &&
					terminal_replay.error().reason ==
						sqlite_shm_lease_rejection_reason::stale_token &&
					!coordinator.snapshot().quarantined,
				"copied terminal receipt cannot be replayed after owner consumption");
		auto request_replay = coordinator.begin_reader_session(first_session_request);
		require(!request_replay &&
					request_replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
				"terminal session request remains a non-reusable tombstone");

		auto unmap_ready = coordinator.poll_reader_unmap_cut(*unmap, unmap_callback);
		require(unmap_ready &&
					unmap_ready->progress ==
						sqlite_shm_reader_unmap_cut_progress::native_effect_ready &&
					unmap->native_effect_ready(),
				"the same cut obligation becomes native-ready after the last owner drains");
		const auto unmap_admitted_view =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(unmap_admitted_view.outstanding_terminal_permit_count == 1U &&
					unmap_admitted_view.last_issued_sequence ==
						unmap_admitted_view.last_committed_sequence &&
					reader_event_sequences_are_dense(unmap_admitted_view) &&
					reader_terminal_permit_slots_are_exact(unmap_admitted_view) &&
					unmap_admitted_view.attachment_groups.size() == 1U &&
					unmap_admitted_view.attachment_groups.front().unmap_cut_permit_slot == 0U &&
					unmap_admitted_view.attachment_groups.front().unmap_terminal_permit_slot != 0U,
				"group unmap admission consumes only the pre-reserved cut and retains one exact "
				"future-terminal permit");
		const auto unmap_receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
			*unmap,
			unmap_callback,
			sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
			sqlite_ok_status,
			0,
			0,
			identity("test.reader-group-unmap-effect", 36U),
			identity("test.reader-group-latch-reset", 36U));
		require(coordinator.complete_reader_unmap(*unmap, unmap_receipt).has_value() &&
					!unmap->valid(),
				"one confirmed native unmap closes every group member");
		const auto closed_lifecycle =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(closed_lifecycle.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(closed_lifecycle) &&
					closed_lifecycle.last_issued_sequence ==
						closed_lifecycle.last_committed_sequence &&
					reader_event_sequences_are_dense(closed_lifecycle),
				"confirmed native unmap consumes its exact terminal permit");
		const auto closed = coordinator.snapshot();
		require(closed.phase == sqlite_shm_mapping_generation_phase::empty &&
					closed.reader_attachment_group_count == 0U &&
					closed.reader_attachment_live_member_count == 0U &&
					closed.reader_attachment_audit_count == 3U &&
					closed.reader_session_owner_count == 0U &&
					closed.reader_session_terminal_count == 2U &&
					closed.reader_handoff_count == 0U && !closed.quarantined,
				"confirmed group unmap retains audit tombstones and releases generation count");
		require(coordinator.revoke_writer_eligibility(gate).has_value(),
				"revoke reader group fixture writer gate");
	}

	void verify_reader_session_execution_is_validated_and_exactly_bound()
	{
		{
			constexpr std::uint8_t marker = 158U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto map_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 1U),
										  marker + 1U,
										  2,
										  marker + 1U,
										  0,
										  writer.holder.generation());
			auto invalid = reader_session_request(map_request, marker + 1U);
			invalid.execution = {};
			const auto before = coordinator.snapshot();
			const auto before_lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			auto rejected = coordinator.begin_reader_session(invalid);
			const auto after = coordinator.snapshot();
			const auto after_lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!rejected &&
					rejected.error().reason == sqlite_shm_lease_rejection_reason::invalid_request &&
					before.reader_session_reservation_count ==
						after.reader_session_reservation_count &&
					before.reader_session_owner_count == after.reader_session_owner_count &&
					before_lifecycle.last_issued_sequence == after_lifecycle.last_issued_sequence &&
					before_lifecycle.last_committed_sequence ==
						after_lifecycle.last_committed_sequence &&
					!after.quarantined,
				"an empty authority-read execution receipt is rejected before mutation");
			retire_last(coordinator, writer.holder, callback(9U, marker + 2U));
			require(coordinator.revoke_writer_eligibility(writer.eligibility).has_value(),
					"revoke invalid execution fixture writer gate");
		}

		{
			constexpr std::uint8_t marker = 162U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto map_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 1U),
										  marker + 1U,
										  2,
										  marker + 1U,
										  0,
										  writer.holder.generation());
			const auto request = reader_session_request(map_request, marker + 1U);
			auto session = coordinator.begin_reader_session(request);
			require(session && session->valid(), "admit execution-binding reader session");
			auto mismatched = request;
			mismatched.execution = reader_session_execution(marker + 2U);
			const auto terminal = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				mismatched,
				sqlite_shm_reader_session_terminal_kind::cancelled_before_authority_read,
				identity("test.reader-session-execution-terminal", marker));
			auto rejected = coordinator.complete_reader_session(*session, terminal);
			require(!rejected && !session->valid() && coordinator.snapshot().quarantined,
					"session terminal cannot replace the execution receipt bound at admission");
		}
	}

	void verify_reader_unmap_cut_wait_policy_is_fail_closed()
	{
		enum class scenario : std::uint8_t
		{
			same_thread,
			reentrant,
			timeout,
		};

		const auto run = [](const std::uint8_t marker, const scenario selected)
		{
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto reader = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			retire_last(coordinator, writer.holder, callback(9U, marker + 2U));
			require(coordinator.revoke_writer_eligibility(writer.eligibility).has_value(),
					"revoke unmap-cut wait-policy writer gate");

			const auto unmap_callback =
				selected == scenario::same_thread || selected == scenario::reentrant
				? sqlite_shm_callback_execution_receipt{reader.session_request.execution
															.thread_identity,
														selected == scenario::reentrant ? 1U : 0U,
														identity("test.reader-unmap-same-thread",
																 marker)}
				: callback(10U, marker + 3U);
			auto unmap = coordinator.begin_reader_unmap(
				reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});

			if (selected == scenario::timeout)
			{
				require(unmap && !reader.handoff.valid() && unmap->valid() &&
							!unmap->native_effect_ready() && !coordinator.snapshot().quarantined,
						"other-thread blocker yields one live cut-wait obligation");
				auto failed = coordinator.fail_reader_unmap_cut_wait(
					*unmap, unmap_callback, sqlite_shm_retirement_wait_failure::timeout);
				require(
					!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						!unmap->valid() && coordinator.snapshot().quarantined,
					"bounded wait timeout quarantines the consumed cut without native authority");
				auto replay = coordinator.poll_reader_unmap_cut(*unmap, unmap_callback);
				require(!replay &&
							replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
						"timed-out cut obligation cannot be retried");
			}
			else
			{
				require(!unmap &&
							unmap.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							!reader.handoff.valid() && coordinator.snapshot().quarantined,
						selected == scenario::same_thread
							? "same-thread blocker quarantines after consuming the cut"
							: "reentrant blocker quarantines after consuming the cut");
			}

			const auto terminal = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				reader.session_request,
				sqlite_shm_reader_session_terminal_kind::success,
				identity("test.reader-unmap-cut-peer-drain", marker));
			require(coordinator.complete_reader_session(reader.session, terminal).has_value() &&
						!reader.session.valid(),
					"established session drains after cut quarantine without reviving admission");
		};

		run(232U, scenario::same_thread);
		run(236U, scenario::reentrant);
		run(240U, scenario::timeout);
	}

	void verify_reader_unmap_cut_drains_preexisting_zero_attachment_map()
	{
		constexpr std::uint8_t marker = 244U;
		const auto binding = family(marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto writer = install_live_writer(coordinator,
										  binding,
										  identity("test.connection", marker),
										  identity("test.open-epoch", marker),
										  marker,
										  &page);
		auto reader = install_live_reader_group(coordinator,
												binding,
												identity("test.connection", marker + 1U),
												marker + 1U,
												writer.holder.generation(),
												&page);
		require(reader.cached_member.has_value(),
				"pre-cut map fixture retains its exact cached member");

		auto map_request = reader_attachment_request(binding,
													 identity("test.connection", marker + 1U),
													 marker + 1U,
													 2,
													 marker + 1U,
													 0,
													 writer.holder.generation());
		map_request.callback = callback(3U, marker + 2U);
		auto map = coordinator.begin_reader_map(reader.session, map_request);
		require(map && map->valid(), "begin one exact map before the unmap cut");

		const auto unmap_callback = callback(7U, marker + 3U);
		auto unmap = coordinator.begin_reader_unmap(
			reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
		const auto after_cut =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(unmap && !reader.handoff.valid() && unmap->valid() &&
					!unmap->native_effect_ready() && after_cut.map_attempts.size() == 1U &&
					after_cut.outstanding_terminal_permit_count == 3U &&
					after_cut.attachment_groups.size() == 1U &&
					after_cut.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::unmap_cut_sealing,
				"cut freezes the earlier native-started map and its session before waiting");

		const auto zero_receipt =
			sqlite_same_process_shm_lease_test_peer::reader_attachment_zero_effect(
				*map,
				sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change,
				map_request,
				sqlite_busy_status,
				nullptr,
				identity("test.reader-unmap-cut-zero-effect", marker + 4U));
		auto completed =
			coordinator.complete_reader_zero_attachment_map(*map, zero_receipt, reader.session);
		const auto after_map =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(completed &&
					completed->kind() ==
						sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change &&
					completed->native_status() == sqlite_busy_status &&
					completed->native_mapping() == nullptr && !map->valid() &&
					reader.session.valid() && unmap->valid() && !unmap->native_effect_ready() &&
					after_map.map_attempts.empty() &&
					after_map.outstanding_terminal_permit_count == 2U &&
					after_map.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::unmap_cut_sealing &&
					!coordinator.snapshot().quarantined,
				"pre-cut exact no-attachment result consumes only its map attempt");

		auto later_request = map_request;
		later_request.callback = callback(3U, marker + 5U);
		const auto before_rejection =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		auto rejected_map = coordinator.begin_reader_map(reader.session, later_request);
		auto rejected_cached = coordinator.authenticate_reader_cached_member_use(
			reader.session, *reader.cached_member);
		const auto after_rejection =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(!rejected_map && !rejected_cached &&
					rejected_cached.error().reason == sqlite_shm_lease_rejection_reason::retiring &&
					before_rejection.last_issued_sequence == after_rejection.last_issued_sequence &&
					before_rejection.last_committed_sequence ==
						after_rejection.last_committed_sequence &&
					before_rejection.live_custody_kind_counts ==
						after_rejection.live_custody_kind_counts &&
					before_rejection.events.size() == after_rejection.events.size(),
				"cut-first rejects later native map and cached pointer use before mutation");

		auto waiting = coordinator.poll_reader_unmap_cut(*unmap, unmap_callback);
		require(waiting && waiting->progress == sqlite_shm_reader_unmap_cut_progress::waiting,
				"the established session remains the sole blocker after map terminal");
		const auto session_terminal =
			sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				reader.session_request,
				sqlite_shm_reader_session_terminal_kind::success,
				identity("test.reader-unmap-cut-zero-session", marker + 6U));
		require(coordinator.complete_reader_session(reader.session, session_terminal).has_value() &&
					!reader.session.valid(),
				"terminalize the last established session after its map attempt");
		auto ready = coordinator.poll_reader_unmap_cut(*unmap, unmap_callback);
		require(ready &&
					ready->progress == sqlite_shm_reader_unmap_cut_progress::native_effect_ready &&
					unmap->native_effect_ready(),
				"the same cut obligation becomes ready after map and session drains");

		const auto unmap_receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
			*unmap,
			unmap_callback,
			sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
			sqlite_ok_status,
			0,
			0,
			identity("test.reader-unmap-cut-zero-unmap-effect", marker + 7U),
			identity("test.reader-unmap-cut-zero-latch", marker + 8U));
		require(coordinator.complete_reader_unmap(*unmap, unmap_receipt).has_value() &&
					!unmap->valid(),
				"confirm native unmap only after the pre-cut map and session are terminal");
		retire_last(coordinator, writer.holder, callback(9U, marker + 9U));
		require(coordinator.revoke_writer_eligibility(writer.eligibility).has_value(),
				"revoke pre-cut zero-map fixture writer gate");
	}

	void verify_reader_unmap_cut_suppresses_preexisting_mapped_result()
	{
		constexpr std::uint8_t marker = 24U;
		const auto binding = family(marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto writer = install_live_writer(coordinator,
										  binding,
										  identity("test.connection", marker),
										  identity("test.open-epoch", marker),
										  marker,
										  &page);
		auto reader = install_live_reader_group(coordinator,
												binding,
												identity("test.connection", marker + 1U),
												marker + 1U,
												writer.holder.generation(),
												&page);

		auto map_request = reader_attachment_request(binding,
													 identity("test.connection", marker + 1U),
													 marker + 1U,
													 2,
													 marker + 1U,
													 0,
													 writer.holder.generation());
		map_request.callback = callback(3U, marker + 2U);
		auto map = coordinator.begin_reader_map(reader.session, map_request);
		require(map && map->valid(), "begin mapped-result attempt before the unmap cut");
		const auto unmap_callback = callback(7U, marker + 3U);
		auto unmap = coordinator.begin_reader_unmap(
			reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
		require(unmap && !reader.handoff.valid() && unmap->valid() && !unmap->native_effect_ready(),
				"cut freezes the earlier mapped-result attempt");

		const auto mapped_receipt =
			sqlite_same_process_shm_lease_test_peer::reader_existing_group_predecessor_mismatch(
				*map,
				map_request,
				sqlite_readonly_status,
				&page,
				identity("test.reader-unmap-cut-mapped-effect", marker + 4U));
		auto suppressed = coordinator.complete_reader_existing_group_predecessor_mismatch(
			*map, mapped_receipt, reader.session);
		const auto after_map =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(suppressed && suppressed->native_status() == sqlite_readonly_status &&
					suppressed->outward_status() == sqlite_ioerr_status &&
					suppressed->native_mapping() == nullptr && !map->valid() &&
					reader.session.valid() && unmap->valid() && !unmap->native_effect_ready() &&
					coordinator.snapshot().reader_existing_group_deferred_cleanup_count == 1U &&
					after_map.map_attempts.empty() &&
					after_map.outstanding_terminal_permit_count == 2U &&
					after_map.attachment_groups.size() == 1U &&
					after_map.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::unmap_cut_sealing &&
					!coordinator.snapshot().quarantined,
				"post-cut mapped result records exact effect but publishes IOERR/null");
		const auto normal_unmap_index =
			enum_index(detail::sqlite_shm_reader_custody_kind::normal_or_deferred_unmap);
		require(after_map.live_custody_kind_counts[normal_unmap_index] == 1U,
				"mapped suppression reuses the cut-owned unmap custody without a second mint");

		auto waiting = coordinator.poll_reader_unmap_cut(*unmap, unmap_callback);
		require(waiting && waiting->progress == sqlite_shm_reader_unmap_cut_progress::waiting,
				"mapped suppression retains its established session as the last blocker");
		const auto session_terminal =
			sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				reader.session_request,
				sqlite_shm_reader_session_terminal_kind::failure,
				identity("test.reader-unmap-cut-mapped-session", marker + 5U));
		require(coordinator.complete_reader_session(reader.session, session_terminal).has_value() &&
					!reader.session.valid(),
				"terminalize mapped-suppression session");
		auto ready = coordinator.poll_reader_unmap_cut(*unmap, unmap_callback);
		require(ready &&
					ready->progress == sqlite_shm_reader_unmap_cut_progress::native_effect_ready &&
					unmap->native_effect_ready(),
				"mapped-suppression cut becomes ready after session drain");

		const auto unmap_receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
			*unmap,
			unmap_callback,
			sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
			sqlite_ok_status,
			0,
			0,
			identity("test.reader-unmap-cut-mapped-unmap-effect", marker + 6U),
			identity("test.reader-unmap-cut-mapped-latch", marker + 7U));
		require(coordinator.complete_reader_unmap(*unmap, unmap_receipt).has_value() &&
					!unmap->valid() &&
					coordinator.snapshot().reader_existing_group_deferred_cleanup_count == 0U &&
					!coordinator.snapshot().quarantined,
				"one confirmed unmap retires the post-cut mapped suppression lineage");
		retire_last(coordinator, writer.holder, callback(9U, marker + 8U));
		require(coordinator.revoke_writer_eligibility(writer.eligibility).has_value(),
				"revoke mapped-suppression fixture writer gate");
	}

	void verify_reader_unmap_cut_quarantines_ambiguous_preexisting_map()
	{
		constexpr std::uint8_t marker = 33U;
		const auto binding = family(marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto writer = install_live_writer(coordinator,
										  binding,
										  identity("test.connection", marker),
										  identity("test.open-epoch", marker),
										  marker,
										  &page);
		auto reader = install_live_reader_group(coordinator,
												binding,
												identity("test.connection", marker + 1U),
												marker + 1U,
												writer.holder.generation(),
												&page);

		auto map_request = reader_attachment_request(binding,
													 identity("test.connection", marker + 1U),
													 marker + 1U,
													 2,
													 marker + 1U,
													 0,
													 writer.holder.generation());
		map_request.callback = callback(3U, marker + 2U);
		auto map = coordinator.begin_reader_map(reader.session, map_request);
		require(map && map->valid(), "begin ambiguous map before the unmap cut");
		const auto before =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto map_token = only_reader_map_attempt(before).map_token;
		const auto session_token = only_session_reservation(before).session_token;
		const auto unmap_callback = callback(7U, marker + 3U);
		auto unmap = coordinator.begin_reader_unmap(
			reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
		require(unmap && !reader.handoff.valid() && unmap->valid() && !unmap->native_effect_ready(),
				"cut freezes the earlier ambiguous map attempt");

		auto opaque =
			coordinator.complete_reader_opaque_attachment_uncertainty(*map, reader.session);
		const auto snapshot = coordinator.snapshot();
		const auto lifecycle =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto* map_quarantine = find_reader_terminal_quarantine(lifecycle, map_token);
		const auto* session_quarantine = find_reader_terminal_quarantine(lifecycle, session_token);
		require(opaque.has_value(), "post-cut ambiguity reaches its closed outward terminal");
		require(opaque->outward_status() == sqlite_ioerr_status &&
					opaque->native_mapping() == nullptr,
				"post-cut ambiguity projects IOERR/null");
		require(!map->valid() && !reader.session.valid() && !unmap->valid() &&
					!unmap->native_effect_ready(),
				"post-cut ambiguity invalidates map, session, and cut cleanup authority");
		require(snapshot.quarantined && snapshot.reader_attachment_group_count == 1U &&
					snapshot.reader_attachment_live_member_count == 0U &&
					snapshot.reader_opaque_attachment_uncertainty_count == 0U,
				"post-cut ambiguity retains one quarantined group without first-map opacity");
		require(lifecycle.attachment_groups.size() == 1U &&
					lifecycle.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::terminal_quarantined &&
					lifecycle.outstanding_terminal_permit_count == 0U &&
					all_reader_live_custody_released(lifecycle),
				"post-cut ambiguity transfers every exact group/cut custody to quarantine");
		require(
			map_quarantine != nullptr && session_quarantine != nullptr &&
				map_quarantine->reason ==
					detail::sqlite_shm_reader_terminal_quarantine_reason::
						native_non_ok_or_unknown &&
				session_quarantine->reason ==
					detail::sqlite_shm_reader_terminal_quarantine_reason::native_non_ok_or_unknown,
			"post-cut ambiguity retains exact map/session terminal reasons");

		auto rejected = coordinator.poll_reader_unmap_cut(*unmap, unmap_callback);
		require(!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					!unmap->valid() && coordinator.snapshot().reader_attachment_group_count == 1U,
				"quarantined cut rejects native unmap without reconstructing cleanup authority");
	}

	void verify_equal_pointer_reader_connections_unmap_independently()
	{
		constexpr std::uint8_t marker = 218U;
		const auto binding = family(marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int shared_page{};
		auto writer = install_live_writer(coordinator,
										  binding,
										  identity("test.connection", marker),
										  identity("test.open-epoch", marker),
										  marker,
										  &shared_page);
		auto first = install_live_reader_group(coordinator,
											   binding,
											   identity("test.connection", marker + 1U),
											   marker + 1U,
											   writer.holder.generation(),
											   &shared_page);
		auto second = install_live_reader_group(coordinator,
												binding,
												identity("test.connection", marker + 2U),
												marker + 2U,
												writer.holder.generation(),
												&shared_page);
		const auto live =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(coordinator.snapshot().reader_attachment_group_count == 2U &&
					coordinator.snapshot().reader_attachment_live_member_count == 2U &&
					coordinator.snapshot().reader_handoff_count == 2U && first.cached_member &&
					second.cached_member && live.attachment_groups.size() == 2U &&
					live.attachment_groups[0U].attachment !=
						live.attachment_groups[1U].attachment &&
					std::ranges::count(live.attachment_groups,
									   detail::sqlite_shm_reader_attachment_group_phase::active,
									   &sqlite_shm_reader_attachment_group_test_view::phase) == 2,
				"two reader connections sharing one native page pointer remain two exact groups");
		const auto before_cross_use =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		auto first_cross_use =
			coordinator.authenticate_reader_cached_member_use(first.session, *second.cached_member);
		auto second_cross_use =
			coordinator.authenticate_reader_cached_member_use(second.session, *first.cached_member);
		const auto after_cross_use =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(!first_cross_use && !second_cross_use &&
					first_cross_use.error().reason ==
						sqlite_shm_lease_rejection_reason::receipt_mismatch &&
					second_cross_use.error().reason ==
						sqlite_shm_lease_rejection_reason::receipt_mismatch &&
					first.session.valid() && second.session.valid() &&
					before_cross_use.last_issued_sequence == after_cross_use.last_issued_sequence &&
					before_cross_use.last_committed_sequence ==
						after_cross_use.last_committed_sequence &&
					before_cross_use.live_custody_kind_counts ==
						after_cross_use.live_custody_kind_counts,
				"equal pointer and generation cannot authenticate a member across group sessions");

		const auto terminalize =
			[&coordinator](live_reader_group_tokens& reader, const std::uint8_t receipt_marker)
		{
			const auto terminal = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				reader.session_request,
				sqlite_shm_reader_session_terminal_kind::success,
				identity("test.reader-equal-pointer-session-terminal", receipt_marker));
			require(coordinator.complete_reader_session(reader.session, terminal).has_value() &&
						!reader.session.valid(),
					"terminalize one equal-pointer reader session");
		};
		terminalize(first, marker + 3U);
		terminalize(second, marker + 4U);

		const auto unmap_one =
			[&coordinator](live_reader_group_tokens& reader, const std::uint8_t receipt_marker)
		{
			const auto unmap_callback = callback(7U, receipt_marker);
			auto unmap = coordinator.begin_reader_unmap(
				reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
			require(unmap && !reader.handoff.valid(), "admit one exact equal-pointer group unmap");
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
				*unmap,
				unmap_callback,
				sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
				sqlite_ok_status,
				0,
				0,
				identity("test.reader-equal-pointer-unmap-effect", receipt_marker),
				identity("test.reader-equal-pointer-unmap-latch", receipt_marker));
			auto completed = coordinator.complete_reader_unmap(*unmap, receipt);
			require(completed &&
						completed->kind() ==
							sqlite_shm_reader_unmap_terminal_kind::retired_confirmed &&
						!unmap->valid(),
					"confirm one distinct equal-pointer group unmap");
		};

		unmap_one(first, marker + 5U);
		const auto after_first =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(!coordinator.snapshot().quarantined &&
					coordinator.snapshot().reader_attachment_group_count == 1U &&
					coordinator.snapshot().reader_handoff_count == 1U && second.handoff.valid() &&
					std::ranges::count(
						after_first.attachment_groups,
						detail::sqlite_shm_reader_attachment_group_phase::native_unmap_confirmed,
						&sqlite_shm_reader_attachment_group_test_view::phase) == 1 &&
					std::ranges::count(after_first.attachment_groups,
									   detail::sqlite_shm_reader_attachment_group_phase::active,
									   &sqlite_shm_reader_attachment_group_test_view::phase) == 1,
				"the first equal-pointer unmap cannot consume or quarantine the second group");

		unmap_one(second, marker + 6U);
		const auto retired =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto snapshot = coordinator.snapshot();
		require(!snapshot.quarantined && snapshot.reader_attachment_group_count == 0U &&
					snapshot.reader_attachment_live_member_count == 0U &&
					snapshot.reader_handoff_count == 0U &&
					snapshot.reader_session_owner_count == 0U &&
					retired.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(retired) &&
					all_reader_live_custody_released(retired) &&
					std::ranges::count(
						retired.attachment_groups,
						detail::sqlite_shm_reader_attachment_group_phase::native_unmap_confirmed,
						&sqlite_shm_reader_attachment_group_test_view::phase) == 2 &&
					std::ranges::count(
						retired.attachment_reservations,
						detail::sqlite_shm_reader_attachment_reservation_phase::retired_confirmed,
						&sqlite_shm_reader_attachment_reservation_test_view::phase) == 2,
				"two equal-pointer reader groups retain two distinct confirmed unmap terminals");
	}

	void verify_callback_free_cached_member_use_requires_a_live_session_owner()
	{
		constexpr std::uint8_t marker = 226U;
		const auto binding = family(marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto writer = install_live_writer(coordinator,
										  binding,
										  identity("test.connection", marker),
										  identity("test.open-epoch", marker),
										  marker,
										  &page);
		auto reader = install_live_reader_group(coordinator,
												binding,
												identity("test.connection", marker + 1U),
												marker + 1U,
												writer.holder.generation(),
												&page);
		require(reader.cached_member && reader.cached_member->mapping().native_mapping == &page,
				"positive map commit returns its unforgeable cached-member identity");

		const auto first_terminal =
			sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				reader.session_request,
				sqlite_shm_reader_session_terminal_kind::success,
				identity("test.reader-cached-first-session-terminal", marker));
		require(coordinator.complete_reader_session(reader.session, first_terminal).has_value() &&
					!reader.session.valid(),
				"finish the callback-producing session before cached reuse");

		auto cached_request = reader.session_request;
		cached_request.read_transaction_epoch =
			identity("test.reader-cached-read-transaction", marker + 2U);
		cached_request.decode_attempt = identity("test.reader-cached-decode-attempt", marker + 2U);
		cached_request.authority_read_receipt =
			identity("test.reader-cached-authority-read", marker + 2U);
		auto cached_session = coordinator.begin_reader_session(cached_request);
		require(cached_session &&
					cached_session->phase() == sqlite_shm_reader_session_phase::active_group_owner,
				"admit a distinct callback-free session from the exact retained group");
		const auto before_use =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto before_snapshot = coordinator.snapshot();
		auto first_use = coordinator.authenticate_reader_cached_member_use(*cached_session,
																		   *reader.cached_member);
		auto second_use = coordinator.authenticate_reader_cached_member_use(*cached_session,
																			*reader.cached_member);
		const auto after_use =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto after_snapshot = coordinator.snapshot();
		require(first_use && second_use && *first_use == reader.cached_member->mapping() &&
					*second_use == reader.cached_member->mapping() &&
					first_use->native_mapping == &page &&
					before_snapshot.reader_inflight_count == after_snapshot.reader_inflight_count &&
					before_snapshot.reader_attachment_audit_count ==
						after_snapshot.reader_attachment_audit_count &&
					before_snapshot.reader_session_owner_count == 1U &&
					after_snapshot.reader_session_owner_count == 1U &&
					before_use.last_issued_sequence == after_use.last_issued_sequence &&
					before_use.last_committed_sequence == after_use.last_committed_sequence &&
					before_use.events.size() == after_use.events.size() &&
					before_use.map_attempts.empty() && after_use.map_attempts.empty(),
				"one exact owner authenticates repeated cached uses with zero xShmMap or ledger "
				"mutation");

		const auto cached_terminal =
			sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				cached_request,
				sqlite_shm_reader_session_terminal_kind::success,
				identity("test.reader-cached-second-session-terminal", marker));
		require(coordinator.complete_reader_session(*cached_session, cached_terminal).has_value() &&
					!cached_session->valid(),
				"consume the exact callback-free session owner");
		auto after_terminal = coordinator.authenticate_reader_cached_member_use(
			*cached_session, *reader.cached_member);
		require(!after_terminal &&
					after_terminal.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
				"a terminated owner cannot authenticate a later cached pointer use");

		const auto unmap_callback = callback(7U, marker + 3U);
		auto unmap = coordinator.begin_reader_unmap(
			reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
		require(unmap && !reader.handoff.valid(),
				"cached-use fixture admits its exact final group unmap");
		const auto unmap_receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
			*unmap,
			unmap_callback,
			sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
			sqlite_ok_status,
			0,
			0,
			identity("test.reader-cached-unmap-effect", marker),
			identity("test.reader-cached-unmap-latch", marker));
		require(coordinator.complete_reader_unmap(*unmap, unmap_receipt).has_value() &&
					!unmap->valid(),
				"cached-use fixture retires its one native group owner");
	}

	void verify_reader_lifecycle_sequence_source_is_shared_and_exhausts_without_partials()
	{
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		auto sequences = std::make_shared<sqlite_shm_reader_lifecycle_sequence_source>();
		const auto first_family = family(201U);
		const auto second_family = family(203U);
		sqlite_same_process_shm_mapping_lease_coordinator first{
			first_family, generations, sequences};
		sqlite_same_process_shm_mapping_lease_coordinator second{
			second_family, generations, sequences};
		int first_page{};
		int second_page{};
		auto first_writer = install_live_writer(first,
												first_family,
												identity("test.connection", 201U),
												identity("test.open-epoch", 201U),
												201U,
												&first_page);
		auto second_writer = install_live_writer(second,
												 second_family,
												 identity("test.connection", 203U),
												 identity("test.open-epoch", 203U),
												 203U,
												 &second_page);
		const auto first_map = reader_attachment_request(first_family,
														 identity("test.connection", 202U),
														 202U,
														 2,
														 202U,
														 0,
														 first_writer.holder.generation());
		const auto second_map = reader_attachment_request(second_family,
														  identity("test.connection", 204U),
														  204U,
														  2,
														  204U,
														  0,
														  second_writer.holder.generation());
		const auto first_request = reader_session_request(first_map, 202U);
		const auto second_request = reader_session_request(second_map, 204U);
		auto first_session = first.begin_reader_session(first_request);
		require(first_session.has_value(), "first family reserves a shared-sequence session");
		const auto after_first =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(first);
		auto second_session = second.begin_reader_session(second_request);
		require(second_session.has_value(), "second family reserves a shared-sequence session");
		const auto after_second =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(second);
		require(after_first.sequence_source_identity != nullptr &&
					after_first.sequence_source_identity == after_second.sequence_source_identity &&
					after_first.last_committed_sequence != 0U &&
					after_first.outstanding_terminal_permit_count == 1U &&
					only_session_reservation(after_first).terminal_permit_slot != 0U &&
					after_second.last_committed_sequence > after_first.last_committed_sequence &&
					after_second.outstanding_terminal_permit_count == 2U &&
					reader_shared_terminal_permit_slots_are_exact(
						after_second, after_first, after_second) &&
					only_session_reservation(after_second).terminal_permit_slot != 0U &&
					only_session_reservation(after_second).terminal_permit_slot !=
						only_session_reservation(after_first).terminal_permit_slot &&
					after_second.last_issued_sequence == after_second.last_committed_sequence,
				"two family coordinators share one strictly monotonic non-reusable sequence");

		sqlite_same_process_shm_lease_test_peer::exhaust_reader_lifecycle_sequences(first);
		auto exhausted_map =
			reader_attachment_request(first_family,
									  identity("test.connection", 205U),
									  205U,
									  3,
									  205U,
									  0,
									  first_writer.holder.generation(),
									  identity("test.reader-attachment-epoch", 205U));
		const auto exhausted_request = reader_session_request(exhausted_map, 205U);
		const auto before_exhausted =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(first);
		auto exhausted = first.begin_reader_session(exhausted_request);
		const auto after_exhausted =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(first);
		require(!exhausted &&
					exhausted.error().reason ==
						sqlite_shm_lease_rejection_reason::generation_exhausted &&
					after_exhausted.sequence_source_exhausted &&
					after_exhausted.last_issued_sequence == before_exhausted.last_issued_sequence &&
					after_exhausted.last_committed_sequence ==
						before_exhausted.last_committed_sequence &&
					before_exhausted.outstanding_terminal_permit_count == 2U &&
					after_exhausted.outstanding_terminal_permit_count == 2U &&
					after_exhausted.outstanding_terminal_permit_slots ==
						before_exhausted.outstanding_terminal_permit_slots &&
					reader_shared_terminal_permit_slots_are_exact(
						after_exhausted, after_exhausted, after_second) &&
					after_exhausted.attachment_reservations.size() ==
						before_exhausted.attachment_reservations.size() &&
					after_exhausted.session_reservations.size() ==
						before_exhausted.session_reservations.size() &&
					after_exhausted.live_custody_kind_counts ==
						before_exhausted.live_custody_kind_counts &&
					after_exhausted.custody_state_counts == before_exhausted.custody_state_counts &&
					first.snapshot().reader_session_reservation_count == 1U &&
					first.snapshot().reader_inflight_count == 0U,
				"sequence exhaustion rejects before reservation, record, or native custody "
				"publication");

		const auto first_terminal =
			sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				first_request,
				sqlite_shm_reader_session_terminal_kind::cancelled_before_authority_read,
				identity("test.reader-session-terminal", 202U));
		const auto second_terminal =
			sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				second_request,
				sqlite_shm_reader_session_terminal_kind::cancelled_before_authority_read,
				identity("test.reader-session-terminal", 204U));
		require(first.complete_reader_session(*first_session, first_terminal).has_value(),
				"reserved terminal capacity closes first session after source exhaustion");
		const auto after_first_terminal =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(first);
		require(second.complete_reader_session(*second_session, second_terminal).has_value(),
				"reserved terminal capacity closes second session after source exhaustion");
		const auto fully_ordered =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(second);
		require(after_first_terminal.last_committed_sequence >
						after_second.last_committed_sequence &&
					after_first_terminal.outstanding_terminal_permit_count == 1U &&
					reader_shared_terminal_permit_slots_are_exact(
						after_first_terminal, after_first_terminal, after_second) &&
					fully_ordered.last_committed_sequence >
						after_first_terminal.last_committed_sequence &&
					fully_ordered.last_issued_sequence == fully_ordered.last_committed_sequence &&
					fully_ordered.outstanding_terminal_permit_count == 0U &&
					reader_shared_terminal_permit_slots_are_exact(
						fully_ordered, after_first_terminal, fully_ordered) &&
					fully_ordered.sequence_source_exhausted,
				"separate family terminal permits preserve actual order after admission "
				"capacity exhaustion");

		retire_last(first, first_writer.holder, callback(4, 206U));
		retire_last(second, second_writer.holder, callback(4, 207U));
		require(first.revoke_writer_eligibility(first_writer.eligibility).has_value() &&
					second.revoke_writer_eligibility(second_writer.eligibility).has_value(),
				"revoke shared-sequence fixture writer gates");
	}

	void verify_first_reader_map_reserves_sequence_and_three_terminals_atomically()
	{
		constexpr std::uint8_t marker = 198U;
		const auto binding = family(marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		auto sequences = std::make_shared<sqlite_shm_reader_lifecycle_sequence_source>(
			std::numeric_limits<std::uint64_t>::max() - 3U);
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{
			binding, generations, sequences};
		int page{};
		auto writer = install_live_writer(coordinator,
										  binding,
										  identity("test.connection", marker),
										  identity("test.open-epoch", marker),
										  marker,
										  &page);
		const auto map_request = reader_attachment_request(binding,
														   identity("test.connection", marker + 1U),
														   marker + 1U,
														   2,
														   marker + 1U,
														   0,
														   writer.holder.generation());
		const auto session_request = reader_session_request(map_request, marker + 1U);
		auto session = coordinator.begin_reader_session(session_request);
		require(session && session->valid(),
				"tight-capacity source still reserves one accepted session terminal");
		const auto before =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(before.outstanding_terminal_permit_count == 1U &&
					reader_terminal_permit_slots_are_exact(before),
				"tight-capacity session starts with one exact record-bound permit");
		auto rejected = coordinator.begin_reader_map(*session, map_request);
		const auto after =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::generation_exhausted &&
					session->valid() && !coordinator.snapshot().quarantined &&
					after.sequence_source_exhausted &&
					after.last_issued_sequence == before.last_issued_sequence &&
					after.last_committed_sequence == before.last_committed_sequence &&
					after.outstanding_terminal_permit_slots ==
						before.outstanding_terminal_permit_slots &&
					reader_terminal_permit_slots_are_exact(after) &&
					after.events.size() == before.events.size() && after.map_attempts.empty() &&
					after.attachment_reservations.size() == before.attachment_reservations.size() &&
					after.session_reservations.size() == before.session_reservations.size() &&
					after.live_custody_kind_counts == before.live_custody_kind_counts &&
					after.custody_state_counts == before.custody_state_counts,
				"insufficient capacity rejects first-map sequence plus three-terminal reservation "
				"atomically without an issued gap or partial record");
		const auto terminal = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
			session_request,
			sqlite_shm_reader_session_terminal_kind::cancelled_before_authority_read,
			identity("test.reader-capacity-session-terminal", marker));
		require(coordinator.complete_reader_session(*session, terminal).has_value() &&
					!session->valid(),
				"pre-reserved session terminal drains after atomic map-reservation exhaustion");
		const auto drained =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(drained.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(drained) &&
					all_reader_live_custody_released(drained),
				"capacity exhaustion strands no accepted session permit or custody");
		retire_last(coordinator, writer.holder, callback(3, marker + 2U));
		require(coordinator.revoke_writer_eligibility(writer.eligibility).has_value(),
				"revoke atomic-capacity writer gate");
	}

	void verify_unavailable_shared_reader_sequence_source_preserves_owned_drains()
	{
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		auto sequences = std::make_shared<sqlite_shm_reader_lifecycle_sequence_source>();
		const auto first_family = family(145U);
		const auto second_family = family(149U);
		sqlite_same_process_shm_mapping_lease_coordinator first{
			first_family, generations, sequences};
		sqlite_same_process_shm_mapping_lease_coordinator second{
			second_family, generations, sequences};
		int first_page{};
		int second_page{};
		auto first_writer = install_live_writer(first,
												first_family,
												identity("test.connection", 145U),
												identity("test.open-epoch", 145U),
												145U,
												&first_page);
		auto second_writer = install_live_writer(second,
												 second_family,
												 identity("test.connection", 149U),
												 identity("test.open-epoch", 149U),
												 149U,
												 &second_page);
		const auto first_map_request = reader_attachment_request(first_family,
																 identity("test.connection", 146U),
																 146U,
																 2,
																 146U,
																 0,
																 first_writer.holder.generation());
		const auto first_session_request = reader_session_request(first_map_request, 146U);
		auto first_session = first.begin_reader_session(first_session_request);
		require(first_session && first_session->valid(),
				"reserve first shared-source session before unavailability");
		auto first_map = first.begin_reader_map(*first_session, first_map_request);
		require(first_map && first_map->valid(),
				"reserve first shared-source map terminal capacity before unavailability");

		const auto second_map_request =
			reader_attachment_request(second_family,
									  identity("test.connection", 150U),
									  150U,
									  2,
									  150U,
									  0,
									  second_writer.holder.generation());
		const auto second_session_request = reader_session_request(second_map_request, 150U);
		auto second_session = second.begin_reader_session(second_session_request);
		require(second_session && second_session->valid(),
				"reserve second shared-source session before unavailability");
		const auto before_first =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(first);
		const auto before_second =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(second);
		require(before_first.sequence_source_identity != nullptr &&
					before_first.sequence_source_identity ==
						before_second.sequence_source_identity &&
					before_first.outstanding_terminal_permit_count == 5U &&
					before_second.outstanding_terminal_permit_count == 5U &&
					reader_shared_terminal_permit_slots_are_exact(
						before_first, before_first, before_second),
				"two families expose the exact five pre-bound terminal permits before source loss");

		sqlite_same_process_shm_lease_test_peer::make_reader_lifecycle_sequences_unavailable(first);
		auto rejected_map_request = second_map_request;
		rejected_map_request.connection_token = identity("test.connection", 151U);
		rejected_map_request.callback = callback(2, 151U);
		rejected_map_request.expected_attachment =
			reader_attachment_request(second_family,
									  rejected_map_request.connection_token,
									  151U,
									  3,
									  151U,
									  0,
									  second_writer.holder.generation(),
									  identity("test.reader-attachment-epoch", 151U))
				.expected_attachment;
		const auto rejected_session_request = reader_session_request(rejected_map_request, 151U);
		const auto before_rejected =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(second);
		auto rejected = second.begin_reader_session(rejected_session_request);
		const auto after_rejected =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(second);
		require(
			!rejected &&
				rejected.error().reason == sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				first_session->valid() && first_map->valid() && second_session->valid() &&
				!first.snapshot().quarantined && !second.snapshot().quarantined &&
				after_rejected.sequence_source_exhausted &&
				after_rejected.last_issued_sequence == before_rejected.last_issued_sequence &&
				after_rejected.last_committed_sequence == before_rejected.last_committed_sequence &&
				after_rejected.outstanding_terminal_permit_slots ==
					before_rejected.outstanding_terminal_permit_slots &&
				after_rejected.attachment_reservations.size() ==
					before_rejected.attachment_reservations.size() &&
				after_rejected.session_reservations.size() ==
					before_rejected.session_reservations.size() &&
				after_rejected.events.size() == before_rejected.events.size() &&
				after_rejected.live_custody_kind_counts ==
					before_rejected.live_custody_kind_counts &&
				after_rejected.custody_state_counts == before_rejected.custody_state_counts,
			"unavailable shared source rejects new admission without a sequence, record, custody, "
			"permit, or family-quarantine side effect");

		auto first_commit =
			first.commit_reader_map(*first_map,
									sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
										first_map_request,
										first_writer.holder.generation(),
										mapping(0, &first_page, 4096U),
										identity("test.reader-unavailable-map-effect", 147U)),
									*first_session);
		require(first_commit && first_commit->formed_group() && !first_map->valid(),
				"pre-bound positive map terminal commits after shared source loss");
		auto first_handoff = first_commit->take_handoff();
		require(first_handoff && first_handoff->valid(),
				"take pre-bound group handoff after shared source loss");
		const auto after_map =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(first);
		require(
			after_map.outstanding_terminal_permit_count == 4U &&
				reader_shared_terminal_permit_slots_are_exact(after_map, after_map, after_rejected),
			"positive map consumes only its exact pre-bound terminal after source loss");

		const auto first_terminal =
			sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				first_session_request,
				sqlite_shm_reader_session_terminal_kind::success,
				identity("test.reader-unavailable-session-terminal", 147U));
		require(first.complete_reader_session(*first_session, first_terminal).has_value() &&
					!first_session->valid(),
				"first pre-bound session drains after shared source loss");
		const auto unmap_callback = callback(7, 148U);
		auto first_unmap = first.begin_reader_unmap(
			*first_handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
		require(first_unmap && first_unmap->valid() && !first_handoff->valid(),
				"pre-bound group cut drains after shared source loss");
		const auto unmap_receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
			*first_unmap,
			unmap_callback,
			sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
			sqlite_ok_status,
			0,
			0,
			identity("test.reader-unavailable-unmap-effect", 148U),
			identity("test.reader-unavailable-latch-reset", 148U));
		require(first.complete_reader_unmap(*first_unmap, unmap_receipt).has_value() &&
					!first_unmap->valid(),
				"pre-bound group terminal drains after shared source loss");
		const auto after_first_drain =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(first);
		const auto second_before_drain =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(second);
		require(after_first_drain.outstanding_terminal_permit_count == 1U &&
					reader_shared_terminal_permit_slots_are_exact(
						after_first_drain, after_first_drain, second_before_drain) &&
					all_reader_live_custody_released(after_first_drain),
				"first family drains exactly while the second owned session permit remains");

		const auto second_terminal =
			sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				second_session_request,
				sqlite_shm_reader_session_terminal_kind::cancelled_before_authority_read,
				identity("test.reader-unavailable-session-terminal", 150U));
		require(second.complete_reader_session(*second_session, second_terminal).has_value() &&
					!second_session->valid(),
				"second pre-bound session drains after shared source loss");
		const auto fully_drained =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(second);
		require(fully_drained.outstanding_terminal_permit_count == 0U &&
					fully_drained.outstanding_terminal_permit_slots.empty() &&
					fully_drained.last_issued_sequence == fully_drained.last_committed_sequence &&
					all_reader_live_custody_released(fully_drained) &&
					fully_drained.sequence_source_exhausted,
				"unavailable shared source strands no accepted terminal permit or custody");

		retire_last(first, first_writer.holder, callback(8, 149U));
		retire_last(second, second_writer.holder, callback(8, 151U));
		require(first.revoke_writer_eligibility(first_writer.eligibility).has_value() &&
					second.revoke_writer_eligibility(second_writer.eligibility).has_value(),
				"revoke unavailable-source fixture writer gates");
	}

	void verify_reader_group_handoff_abandonment_consumes_joined_terminal_capacity()
	{
		const auto form_commit = [](sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
									const sqlite_shm_lease_family_binding& binding,
									const sqlite_backend_opaque_identity& reader_connection,
									const std::uint8_t marker,
									const std::uint64_t generation,
									const volatile void* page)
		{
			const auto map_request = reader_attachment_request(
				binding, reader_connection, marker, 2, marker, 0, generation);
			const auto session_request = reader_session_request(map_request, marker);
			auto session = coordinator.begin_reader_session(session_request);
			require(session.has_value(), "reserve abandonment reader session");
			auto map = coordinator.begin_reader_map(*session, map_request);
			require(map.has_value(), "begin abandonment first reader map");
			auto commit = coordinator.commit_reader_map(
				*map,
				sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
					map_request,
					generation,
					mapping(0, page, 4096U),
					identity("test.reader-abandon-zero-resize", marker)),
				*session);
			require(commit && commit->formed_group(), "form abandonment reader attachment group");
			return std::tuple{session_request, std::move(*session), std::move(*commit)};
		};

		{
			constexpr std::uint8_t marker = 166U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto formed = form_commit(coordinator,
									  binding,
									  identity("test.connection", marker + 1U),
									  marker + 1U,
									  writer.holder.generation(),
									  &page);
			const auto session_request = std::move(std::get<0>(formed));
			auto session = std::move(std::get<1>(formed));
			std::optional<sqlite_shm_reader_session_reservation_test_view>
				session_before_abandonment;
			{
				auto commit = std::move(std::get<2>(formed));
				const auto live =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				session_before_abandonment.emplace(only_session_reservation(live));
				require(commit.formed_group() && live.outstanding_terminal_permit_count == 3U &&
							live.attachment_groups.size() == 1U &&
							live.attachment_groups.front().unmap_cut_permit_slot != 0U &&
							live.attachment_groups.front().unmap_terminal_permit_slot != 0U,
						"untaken first-map commit owns one joined group handoff and both group "
						"terminal permits");
			}
			const auto abandoned =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				coordinator.snapshot().quarantined && session.valid() &&
					abandoned.outstanding_terminal_permit_count == 1U &&
					abandoned.attachment_groups.size() == 1U &&
					abandoned.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::terminal_quarantined &&
					abandoned.attachment_groups.front().unmap_cut_permit_slot == 0U &&
					abandoned.attachment_groups.front().unmap_terminal_permit_slot == 0U &&
					abandoned.session_reservations.size() == 1U && session_before_abandonment &&
					abandoned.session_reservations.front().session_token ==
						session_before_abandonment->session_token &&
					abandoned.session_reservations.front().phase ==
						session_before_abandonment->phase &&
					abandoned.session_reservations.front().origin_sequence ==
						session_before_abandonment->origin_sequence &&
					abandoned.session_reservations.front().destination_sequence ==
						session_before_abandonment->destination_sequence &&
					abandoned.session_reservations.front().terminal_permit_slot ==
						session_before_abandonment->terminal_permit_slot &&
					abandoned.live_custody_kind_counts[enum_index(
						detail::sqlite_shm_reader_custody_kind::use_session)] == 1U,
				"destroying a first-map commit without take_handoff consumes both joined group "
				"permits while preserving the exact already-owned session phase, permit, and "
				"custody drain");
			const auto terminal = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				session_request,
				sqlite_shm_reader_session_terminal_kind::success,
				identity("test.reader-abandon-session-terminal", marker));
			require(coordinator.complete_reader_session(session, terminal).has_value() &&
						!session.valid(),
					"drain the already-owned session after untaken commit abandonment");
			const auto drained =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(drained.outstanding_terminal_permit_count == 0U &&
						all_reader_live_custody_released(drained),
					"untaken commit abandonment plus owned-session drain strands no permit or "
					"reader custody");
		}

		{
			constexpr std::uint8_t marker = 170U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto formed = form_commit(coordinator,
									  binding,
									  identity("test.connection", marker + 1U),
									  marker + 1U,
									  writer.holder.generation(),
									  &page);
			const auto session_request = std::move(std::get<0>(formed));
			auto session = std::move(std::get<1>(formed));
			auto commit = std::move(std::get<2>(formed));
			auto handoff_result = commit.take_handoff();
			require(handoff_result && handoff_result->valid(),
					"take handoff before explicit abandonment");
			const auto terminal = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				session_request,
				sqlite_shm_reader_session_terminal_kind::success,
				identity("test.reader-abandon-session-terminal", marker));
			require(coordinator.complete_reader_session(session, terminal).has_value() &&
						!session.valid(),
					"close session before dropping taken handoff");
			const auto before_drop =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(before_drop.outstanding_terminal_permit_count == 2U,
					"taken handoff retains exactly its pre-reserved cut and terminal permits");
			{
				auto handoff = std::move(*handoff_result);
				require(handoff.valid(), "taken group handoff remains live until scope exit");
			}
			const auto abandoned =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				coordinator.snapshot().quarantined &&
					abandoned.outstanding_terminal_permit_count == 0U &&
					all_reader_live_custody_released(abandoned) &&
					abandoned.attachment_reservations.size() == 1U &&
					abandoned.attachment_reservations.front().phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::
							terminal_quarantined &&
					abandoned.attachment_reservations.front().destination_sequence != 0U &&
					abandoned.attachment_groups.size() == 1U &&
					abandoned.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::terminal_quarantined &&
					abandoned.attachment_groups.front().destination_sequence ==
						abandoned.attachment_reservations.front().destination_sequence &&
					abandoned.attachment_groups.front().unmap_cut_permit_slot == 0U &&
					abandoned.attachment_groups.front().unmap_terminal_permit_slot == 0U,
				"dropping a taken handoff consumes both joined permits and transfers all reader "
				"custody to one sequenced abandonment tombstone");
		}
	}

	void verify_exact_reader_unmap_terminal_receipts_are_closed_and_one_shot()
	{
		const auto form_ready_unmap =
			[](sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
			   live_reader_group_tokens& reader,
			   const std::uint8_t marker)
		{
			const auto terminal = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				reader.session_request,
				sqlite_shm_reader_session_terminal_kind::success,
				identity("test.reader-unmap-session-terminal", marker));
			require(coordinator.complete_reader_session(reader.session, terminal).has_value(),
					"terminalize exact-unmap reader session");
			const auto unmap_callback = callback(7, marker);
			const auto before_legacy =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			auto legacy = coordinator.begin_reader_unmap(reader.handoff, unmap_callback);
			const auto after_legacy =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!legacy &&
					legacy.error().reason == sqlite_shm_lease_rejection_reason::invalid_request &&
					reader.handoff.valid() &&
					after_legacy.last_issued_sequence == before_legacy.last_issued_sequence &&
					after_legacy.last_committed_sequence == before_legacy.last_committed_sequence &&
					after_legacy.outstanding_terminal_permit_slots ==
						before_legacy.outstanding_terminal_permit_slots &&
					after_legacy.live_custody_kind_counts ==
						before_legacy.live_custody_kind_counts &&
					after_legacy.custody_state_counts == before_legacy.custody_state_counts &&
					after_legacy.events.size() == before_legacy.events.size(),
				"legacy callback-only unmap overload rejects activated groups before native "
				"or lifecycle mutation");
			auto unmap = coordinator.begin_reader_unmap(
				reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
			require(unmap.has_value(),
					"admit exact typed reader unmap result marker " + std::to_string(marker));
			require(!reader.handoff.valid(), "consume exact typed reader handoff");
			return std::pair{std::move(*unmap), unmap_callback};
		};

		const auto verify_cross_role_replay =
			[&form_ready_unmap](const bool reuse_source_latch_as_effect, const std::uint8_t marker)
		{
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto source_reader = install_live_reader_group(coordinator,
														   binding,
														   identity("test.connection", marker + 1U),
														   marker + 1U,
														   writer.holder.generation(),
														   &page);
			auto target_reader = install_live_reader_group(coordinator,
														   binding,
														   identity("test.connection", marker + 2U),
														   marker + 2U,
														   writer.holder.generation(),
														   &page);
			auto [source_unmap, source_callback] =
				form_ready_unmap(coordinator, source_reader, marker + 3U);
			const auto source_effect =
				identity("test.reader-unmap-cross-role-source-effect", marker);
			const auto source_latch = identity("test.reader-unmap-cross-role-source-latch", marker);
			const auto source_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
					source_unmap,
					source_callback,
					sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
					sqlite_ok_status,
					0,
					0,
					source_effect,
					source_latch);
			auto source_completed = coordinator.complete_reader_unmap(source_unmap, source_receipt);
			require(source_completed &&
						source_completed->kind() ==
							sqlite_shm_reader_unmap_terminal_kind::retired_confirmed &&
						!source_unmap.valid() && target_reader.handoff.valid(),
					"store cross-role source receipt before admitting the target native call");
			auto [target_unmap, target_callback] =
				form_ready_unmap(coordinator, target_reader, marker + 4U);
			const auto target_effect = reuse_source_latch_as_effect
				? source_latch
				: identity("test.reader-unmap-cross-role-target-effect", marker);
			const auto target_latch = reuse_source_latch_as_effect
				? identity("test.reader-unmap-cross-role-target-latch", marker)
				: source_effect;
			const auto target_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
					target_unmap,
					target_callback,
					sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
					sqlite_ok_status,
					0,
					0,
					target_effect,
					target_latch);
			auto rejected = coordinator.complete_reader_unmap(target_unmap, target_receipt);
			const auto lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto target_terminal = std::ranges::find_if(
				lifecycle.terminal_quarantines,
				[&](const sqlite_shm_reader_terminal_quarantine_test_view& value)
				{
					return value.callback && *value.callback == target_callback;
				});
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					rejected.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					!target_unmap.valid() && coordinator.snapshot().quarantined &&
					lifecycle.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(lifecycle) &&
					all_reader_live_custody_released(lifecycle) &&
					std::ranges::count(
						lifecycle.attachment_groups,
						detail::sqlite_shm_reader_attachment_group_phase::native_unmap_confirmed,
						&sqlite_shm_reader_attachment_group_test_view::phase) == 1U &&
					target_terminal != lifecycle.terminal_quarantines.end() &&
					target_terminal->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::presented_invalid &&
					!target_terminal->native_effect_receipt &&
					!target_terminal->latch_reset_receipt &&
					!target_terminal->exact_terminal_receipt_retained,
				"unmap effect/latch cross-role reuse did not preserve the source receipt and "
				"terminally reject the target");
		};

		verify_cross_role_replay(true, 176U);
		verify_cross_role_replay(false, 181U);

		{
			constexpr std::uint8_t marker = 186U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto reader = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			const auto session_terminal =
				sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
					reader.session_request,
					sqlite_shm_reader_session_terminal_kind::success,
					identity("test.reader-unmap-begin-session-terminal", marker));
			require(
				coordinator.complete_reader_session(reader.session, session_terminal).has_value(),
				"terminalize reader session before injected unmap preparation failure");
			const auto unmap_callback = callback(7, marker + 2U);
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				reader.handoff.valid() && before.attachment_groups.size() == 1U &&
					before.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::active &&
					before.outstanding_terminal_permit_count == 2U &&
					reader_terminal_permit_slots_are_exact(before),
				"unmap preparation-failure fixture owns one active group and both reserved cuts");
			const auto before_unmap_cut_count = count_reader_lifecycle_events(
				before, detail::sqlite_shm_reader_lifecycle_event_kind::unmap_cut);
			sqlite_same_process_shm_lease_test_peer::fail_reader_unmap_begin_preparation(
				coordinator);
			auto failed = coordinator.begin_reader_unmap(
				reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto* quarantine =
				after.terminal_quarantines.empty() ? nullptr : &after.terminal_quarantines.front();
			require(
				!failed &&
					failed.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					failed.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					!reader.handoff.valid() && coordinator.snapshot().quarantined &&
					after.outstanding_terminal_permit_count == 0U &&
					after.outstanding_terminal_permit_slots.empty() &&
					reader_terminal_permit_slots_are_exact(after) &&
					all_reader_live_custody_released(after) &&
					after.attachment_reservations.size() == 1U &&
					after.attachment_reservations.front().phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::
							terminal_quarantined &&
					after.attachment_groups.size() == 1U &&
					after.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::terminal_quarantined &&
					after.attachment_groups.front().unmap_cut_permit_slot == 0U &&
					after.attachment_groups.front().unmap_terminal_permit_slot == 0U &&
					count_reader_lifecycle_events(
						after, detail::sqlite_shm_reader_lifecycle_event_kind::unmap_cut) ==
						before_unmap_cut_count &&
					quarantine != nullptr &&
					quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::internal_failure &&
					!quarantine->callback && !quarantine->native_effect_receipt &&
					!quarantine->exact_terminal_receipt_retained,
				"fallible unmap preparation did not fail before native ownership while "
				"terminally closing the exact group, permits, and custody");
		}

		{
			constexpr std::uint8_t marker = 207U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto reader = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			const auto terminal = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				reader.session_request,
				sqlite_shm_reader_session_terminal_kind::success,
				identity("test.reader-unmap-session-terminal", marker));
			require(coordinator.complete_reader_session(reader.session, terminal).has_value(),
					"terminalize typed-deleteFlag reader session");
			const auto unmap_callback = callback(7, marker + 2U);
			for (const auto [caller_delete_flag, delegated_delete_flag] :
				 {std::pair{1, 0}, std::pair{0, 1}})
			{
				const auto before =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				const auto before_snapshot = coordinator.snapshot();
				auto rejected = coordinator.begin_reader_unmap(
					reader.handoff,
					sqlite_shm_reader_unmap_request{
						unmap_callback, caller_delete_flag, delegated_delete_flag});
				const auto after =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				const auto after_snapshot = coordinator.snapshot();
				require(
					!rejected &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::invalid_request &&
						reader.handoff.valid() && !after_snapshot.quarantined &&
						before.outstanding_terminal_permit_count == 2U &&
						reader_terminal_permit_slots_are_exact(before) &&
						after.last_issued_sequence == before.last_issued_sequence &&
						after.last_committed_sequence == before.last_committed_sequence &&
						after.outstanding_terminal_permit_slots ==
							before.outstanding_terminal_permit_slots &&
						after.live_custody_kind_counts == before.live_custody_kind_counts &&
						after.custody_state_counts == before.custody_state_counts &&
						after.events.size() == before.events.size() &&
						after.attachment_groups.size() == 1U &&
						before.attachment_groups.size() == 1U &&
						after.attachment_groups.front().phase ==
							before.attachment_groups.front().phase &&
						after.attachment_groups.front().unmap_cut_permit_slot ==
							before.attachment_groups.front().unmap_cut_permit_slot &&
						after.attachment_groups.front().unmap_terminal_permit_slot ==
							before.attachment_groups.front().unmap_terminal_permit_slot &&
						after_snapshot.reader_handoff_count ==
							before_snapshot.reader_handoff_count &&
						after_snapshot.reader_cleanup_count == before_snapshot.reader_cleanup_count,
					"nonzero typed caller/delegated deleteFlag rejects before native ownership, "
					"cut, sequence, custody, or permit mutation");
			}
			auto unmap = coordinator.begin_reader_unmap(
				reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
			require(unmap && !reader.handoff.valid(),
					"zero/zero typed request remains admissible after deleteFlag rejection");
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
				*unmap,
				unmap_callback,
				sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
				sqlite_ok_status,
				0,
				0,
				identity("test.reader-unmap-effect", marker),
				identity("test.reader-unmap-latch-reset", marker));
			require(coordinator.complete_reader_unmap(*unmap, receipt).has_value(),
					"close typed-deleteFlag fixture with exact zero/zero receipt");
			retire_last(coordinator, writer.holder, callback(8, marker + 3U));
			require(coordinator.revoke_writer_eligibility(writer.eligibility).has_value(),
					"revoke typed-deleteFlag writer gate");
		}

		{
			constexpr std::uint8_t marker = 205U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto reader = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			const auto terminal = sqlite_same_process_shm_lease_test_peer::reader_session_terminal(
				reader.session_request,
				sqlite_shm_reader_session_terminal_kind::success,
				identity("test.reader-generic-unmap-session-terminal", marker));
			require(coordinator.complete_reader_session(reader.session, terminal).has_value(),
					"terminalize generic-unmap reader session");
			const auto unmap_callback = callback(7, marker + 2U);
			auto unmap = coordinator.begin_reader_unmap(
				reader.handoff, sqlite_shm_reader_unmap_request{unmap_callback, 0, 0});
			require(unmap && !reader.handoff.valid(),
					"admit typed group cut before generic terminal rejection");
			auto rejected = coordinator.complete_reader_unmap(
				*unmap, unmap_callback, sqlite_shm_native_cleanup_outcome::confirmed_success);
			const auto lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					!unmap->valid() && coordinator.snapshot().quarantined &&
					lifecycle.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(lifecycle) &&
					lifecycle.last_issued_sequence == lifecycle.last_committed_sequence &&
					reader_event_sequences_are_dense(lifecycle) &&
					all_reader_live_custody_released(lifecycle) &&
					lifecycle.attachment_reservations.size() == 1U &&
					lifecycle.attachment_reservations.front().phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::
							terminal_quarantined &&
					lifecycle.attachment_groups.size() == 1U &&
					lifecycle.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::terminal_quarantined &&
					lifecycle.attachment_groups.front().destination_sequence ==
						lifecycle.attachment_reservations.front().destination_sequence &&
					lifecycle.terminal_quarantines.size() == 1U &&
					lifecycle.terminal_quarantines.front().reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::presented_invalid &&
					lifecycle.terminal_quarantines.front().terminal_sequence ==
						lifecycle.attachment_groups.front().destination_sequence &&
					lifecycle.terminal_quarantines.front().callback &&
					*lifecycle.terminal_quarantines.front().callback == unmap_callback &&
					!lifecycle.terminal_quarantines.front().native_effect_receipt &&
					!lifecycle.terminal_quarantines.front().exact_terminal_receipt_retained,
				"generic cleanup outcome cannot stand in for exact group receipt and consumes "
				"the accepted obligation into one no-retry quarantine");
		}

		{
			constexpr std::uint8_t marker = 210U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto reader = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			auto [unmap, unmap_callback] = form_ready_unmap(coordinator, reader, marker + 2U);
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(before.outstanding_terminal_permit_count == 1U &&
						before.attachment_groups.size() == 1U &&
						before.attachment_groups.front().unmap_terminal_permit_slot != 0U,
					"exact OK unmap cut owns one terminal permit before native completion");
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
				unmap,
				unmap_callback,
				sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
				sqlite_ok_status,
				0,
				0,
				identity("test.reader-unmap-effect", marker),
				identity("test.reader-unmap-latch-reset", marker));
			sqlite_same_process_shm_lease_test_peer::exhaust_reader_lifecycle_sequences(
				coordinator);
			auto completed = coordinator.complete_reader_unmap(unmap, receipt);
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				completed &&
					completed->kind() == sqlite_shm_reader_unmap_terminal_kind::retired_confirmed &&
					completed->evidence_kind() ==
						sqlite_shm_reader_unmap_evidence_kind::exact_native_result &&
					completed->native_status() == sqlite_ok_status &&
					completed->outward_status() == sqlite_ok_status &&
					completed->native_effect_receipt() && completed->latch_reset_receipt() &&
					!unmap.valid() && after.attachment_reservations.size() == 1U &&
					after.attachment_reservations.front().phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::retired_confirmed &&
					after.attachment_groups.size() == 1U &&
					after.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::native_unmap_confirmed &&
					after.last_committed_sequence > before.last_committed_sequence &&
					after.last_issued_sequence == after.last_committed_sequence &&
					reader_event_sequences_are_dense(after) &&
					after.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(after) &&
					all_reader_live_custody_released(after) &&
					after.attachment_groups.front().unmap_terminal_permit_slot == 0U &&
					after.sequence_source_exhausted,
				"exact OK unmap consumes its reserved terminal capacity after source exhaustion "
				"and publishes retired-confirmed");
			const auto issued_before_replay = after.last_issued_sequence;
			auto replay = coordinator.complete_reader_unmap(unmap, receipt);
			const auto replayed =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(!replay &&
						replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
						replayed.last_issued_sequence == issued_before_replay &&
						replayed.last_committed_sequence == after.last_committed_sequence &&
						replayed.outstanding_terminal_permit_count == 0U,
					"completed exact unmap replay is stale and allocates no sequence");
			retire_last(coordinator, writer.holder, callback(8, marker + 3U));
			require(coordinator.revoke_writer_eligibility(writer.eligibility).has_value(),
					"revoke exact OK unmap writer gate");
		}

		struct terminal_row
		{
			sqlite_shm_reader_unmap_evidence_kind evidence_kind;
			std::optional<int> native_status;
			bool native_effect;
			bool latch_reset;
			int expected_outward_status;
		};
		const terminal_row terminal_rows[]{
			{sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
			 sqlite_busy_status,
			 true,
			 false,
			 sqlite_busy_status},
			{sqlite_shm_reader_unmap_evidence_kind::throw_or_unknown,
			 std::nullopt,
			 false,
			 false,
			 sqlite_ioerr_status},
		};
		for (std::size_t index = 0; index < std::size(terminal_rows); ++index)
		{
			const auto marker = static_cast<std::uint8_t>(214U + index * 3U);
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto reader = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			auto [unmap, unmap_callback] = form_ready_unmap(coordinator, reader, marker + 2U);
			const auto& row = terminal_rows[index];
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
				unmap,
				unmap_callback,
				row.evidence_kind,
				row.native_status,
				0,
				0,
				row.native_effect ? std::optional{identity("test.reader-unmap-effect", marker)}
								  : std::nullopt,
				row.latch_reset ? std::optional{identity("test.reader-unmap-latch-reset", marker)}
								: std::nullopt);
			auto completed = coordinator.complete_reader_unmap(unmap, receipt);
			const auto completed_view =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				completed &&
					completed->kind() ==
						sqlite_shm_reader_unmap_terminal_kind::terminal_quarantined &&
					completed->evidence_kind() == row.evidence_kind &&
					completed->native_status() == row.native_status &&
					completed->outward_status() == row.expected_outward_status && !unmap.valid() &&
					coordinator.snapshot().quarantined &&
					completed_view.outstanding_terminal_permit_count == 0U &&
					completed_view.last_issued_sequence == completed_view.last_committed_sequence &&
					reader_event_sequences_are_dense(completed_view) &&
					reader_terminal_permit_slots_are_exact(completed_view) &&
					all_reader_live_custody_released(completed_view) &&
					completed_view.attachment_reservations.size() == 1U &&
					completed_view.attachment_reservations.front().phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::
							terminal_quarantined &&
					completed_view.attachment_groups.size() == 1U &&
					completed_view.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::terminal_quarantined &&
					completed_view.attachment_groups.front().destination_sequence ==
						completed_view.attachment_reservations.front().destination_sequence &&
					completed_view.attachment_groups.front().unmap_terminal_permit_slot == 0U,
				"non-OK or unknown unmap consumes one no-retry quarantine terminal");
		}

		struct invalid_row
		{
			bool wrong_callback;
			int caller_delete_flag;
			int delegated_delete_flag;
			std::optional<int> native_status;
			bool native_effect;
			bool latch_reset;
			sqlite_shm_reader_unmap_evidence_kind evidence_kind;
		};
		const invalid_row invalid_rows[]{
			{true,
			 0,
			 0,
			 sqlite_ok_status,
			 true,
			 true,
			 sqlite_shm_reader_unmap_evidence_kind::exact_native_result},
			{false,
			 1,
			 0,
			 sqlite_ok_status,
			 true,
			 true,
			 sqlite_shm_reader_unmap_evidence_kind::exact_native_result},
			{false,
			 0,
			 1,
			 sqlite_ok_status,
			 true,
			 true,
			 sqlite_shm_reader_unmap_evidence_kind::exact_native_result},
			{false,
			 0,
			 0,
			 sqlite_busy_status,
			 false,
			 false,
			 sqlite_shm_reader_unmap_evidence_kind::exact_native_result},
			{false,
			 0,
			 0,
			 sqlite_busy_status,
			 true,
			 true,
			 sqlite_shm_reader_unmap_evidence_kind::exact_native_result},
			{false,
			 0,
			 0,
			 sqlite_ok_status,
			 true,
			 false,
			 sqlite_shm_reader_unmap_evidence_kind::exact_native_result},
			{false,
			 0,
			 0,
			 std::nullopt,
			 true,
			 false,
			 sqlite_shm_reader_unmap_evidence_kind::exact_native_result},
			{false,
			 0,
			 0,
			 sqlite_invalid_extended_ok_status,
			 true,
			 false,
			 sqlite_shm_reader_unmap_evidence_kind::exact_native_result},
			{false,
			 0,
			 0,
			 std::nullopt,
			 true,
			 false,
			 sqlite_shm_reader_unmap_evidence_kind::throw_or_unknown},
		};
		for (std::size_t index = 0; index < std::size(invalid_rows); ++index)
		{
			const auto marker = static_cast<std::uint8_t>(220U + index * 4U);
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto reader = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			auto [unmap, unmap_callback] = form_ready_unmap(coordinator, reader, marker + 2U);
			const auto before_invalid =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(before_invalid.outstanding_terminal_permit_count == 1U &&
						before_invalid.attachment_groups.front().unmap_terminal_permit_slot != 0U,
					"invalid terminal row starts with one exact unmap permit");
			const auto& row = invalid_rows[index];
			auto receipt_callback = row.wrong_callback ? callback(9, marker + 3U) : unmap_callback;
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
				unmap,
				receipt_callback,
				row.evidence_kind,
				row.native_status,
				row.caller_delete_flag,
				row.delegated_delete_flag,
				row.native_effect ? std::optional{identity("test.reader-unmap-effect", marker)}
								  : std::nullopt,
				row.latch_reset ? std::optional{identity("test.reader-unmap-latch-reset", marker)}
								: std::nullopt);
			auto rejected = coordinator.complete_reader_unmap(unmap, receipt);
			const auto rejected_view =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					!unmap.valid() && coordinator.snapshot().quarantined &&
					rejected_view.outstanding_terminal_permit_count == 0U &&
					rejected_view.last_issued_sequence == rejected_view.last_committed_sequence &&
					reader_event_sequences_are_dense(rejected_view) &&
					reader_terminal_permit_slots_are_exact(rejected_view) &&
					all_reader_live_custody_released(rejected_view) &&
					rejected_view.attachment_reservations.size() == 1U &&
					rejected_view.attachment_reservations.front().phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::
							terminal_quarantined &&
					rejected_view.attachment_groups.size() == 1U &&
					rejected_view.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::terminal_quarantined &&
					rejected_view.attachment_groups.front().destination_sequence ==
						rejected_view.attachment_reservations.front().destination_sequence &&
					rejected_view.attachment_groups.front().unmap_terminal_permit_slot == 0U,
				"invalid callback/deleteFlag/effect/latch evidence consumes one terminal "
				"quarantine without retry");
		}

		{
			constexpr std::uint8_t first_marker = 246U;
			constexpr std::uint8_t second_marker = 249U;
			const auto first_binding = family(first_marker);
			const auto second_binding = family(second_marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator first{first_binding, generations};
			sqlite_same_process_shm_mapping_lease_coordinator second{second_binding, generations};
			int first_page{};
			int second_page{};
			auto first_writer = install_live_writer(first,
													first_binding,
													identity("test.connection", first_marker),
													identity("test.open-epoch", first_marker),
													first_marker,
													&first_page);
			auto second_writer = install_live_writer(second,
													 second_binding,
													 identity("test.connection", second_marker),
													 identity("test.open-epoch", second_marker),
													 second_marker,
													 &second_page);
			auto first_reader =
				install_live_reader_group(first,
										  first_binding,
										  identity("test.connection", first_marker + 1U),
										  first_marker + 1U,
										  first_writer.holder.generation(),
										  &first_page);
			auto second_reader =
				install_live_reader_group(second,
										  second_binding,
										  identity("test.connection", second_marker + 1U),
										  second_marker + 1U,
										  second_writer.holder.generation(),
										  &second_page);
			auto [first_unmap, first_callback] =
				form_ready_unmap(first, first_reader, first_marker + 2U);
			(void)first_callback;
			auto [second_unmap, second_callback] =
				form_ready_unmap(second, second_reader, second_marker + 2U);
			const auto cross_owner_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
					second_unmap,
					second_callback,
					sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
					sqlite_ok_status,
					0,
					0,
					identity("test.reader-unmap-effect", second_marker),
					identity("test.reader-unmap-latch-reset", second_marker));
			const auto first_before_cross =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(first);
			const auto second_before_cross =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(second);
			auto rejected = first.complete_reader_unmap(first_unmap, cross_owner_receipt);
			const auto first_after_cross =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(first);
			const auto second_after_cross =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(second);
			require(!rejected &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						!first_unmap.valid() && second_unmap.valid() &&
						first.snapshot().quarantined &&
						first_before_cross.outstanding_terminal_permit_count == 1U &&
						second_before_cross.outstanding_terminal_permit_count == 1U &&
						first_after_cross.outstanding_terminal_permit_count == 0U &&
						reader_terminal_permit_slots_are_exact(first_after_cross) &&
						all_reader_live_custody_released(first_after_cross) &&
						second_after_cross.outstanding_terminal_permit_count == 1U,
					"cross-owner unmap receipt terminally rejects the target without consuming "
					"the source owner");
			auto source_completed = second.complete_reader_unmap(second_unmap, cross_owner_receipt);
			require(source_completed &&
						source_completed->kind() ==
							sqlite_shm_reader_unmap_terminal_kind::retired_confirmed &&
						!second_unmap.valid() && !second.snapshot().quarantined &&
						sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(second)
								.outstanding_terminal_permit_count == 0U,
					"cross-owner rejection preserves the receipt for its exact source obligation");
			retire_last(second, second_writer.holder, callback(10, second_marker + 3U));
			require(second.revoke_writer_eligibility(second_writer.eligibility).has_value(),
					"revoke exact source-owner writer gate");
		}

		{
			constexpr std::uint8_t marker = 252U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto reader = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			auto [unmap, unmap_callback] = form_ready_unmap(coordinator, reader, marker + 2U);
			const auto native_effect = identity("test.reader-unmap-effect", marker);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
				unmap,
				unmap_callback,
				sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
				sqlite_ok_status,
				0,
				0,
				native_effect,
				identity("test.reader-unmap-latch-reset", marker));
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto group_token = last_reader_lifecycle_event_owner(
				before, detail::sqlite_shm_reader_lifecycle_event_kind::unmap_cut);
			sqlite_same_process_shm_lease_test_peer::fail_next_reader_unmap_terminal_commit(
				coordinator);
			auto failed = coordinator.complete_reader_unmap(unmap, receipt);
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto* terminal_quarantine = find_reader_terminal_quarantine(after, group_token);
			require(
				!failed &&
					failed.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					!unmap.valid() && coordinator.snapshot().quarantined &&
					before.outstanding_terminal_permit_count == 1U &&
					after.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(after) &&
					all_reader_live_custody_released(after) &&
					after.attachment_reservations.size() == 1U &&
					after.attachment_reservations.front().phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::
							terminal_quarantined &&
					after.attachment_groups.size() == 1U &&
					after.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::terminal_quarantined &&
					after.attachment_groups.front().destination_sequence ==
						after.attachment_reservations.front().destination_sequence &&
					after.last_issued_sequence > before.last_issued_sequence &&
					after.last_committed_sequence == after.last_issued_sequence &&
					reader_event_sequences_are_dense(after),
				"unmap terminal commit failure consumes its reserved sequence into a "
				"conservative quarantine without publishing success");
			require(
				terminal_quarantine != nullptr &&
					terminal_quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::
							injected_commit_failure &&
					terminal_quarantine->terminal_sequence ==
						after.attachment_groups.front().destination_sequence &&
					terminal_quarantine->callback &&
					*terminal_quarantine->callback == unmap_callback &&
					terminal_quarantine->native_effect_receipt &&
					*terminal_quarantine->native_effect_receipt == native_effect &&
					terminal_quarantine->exact_terminal_receipt_retained,
				"injected unmap failure retains exact callback, effect, owner, receipt, reason, "
				"and consumed terminal sequence");
		}

		{
			constexpr std::uint8_t marker = 166U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto first_reader = install_live_reader_group(coordinator,
														  binding,
														  identity("test.connection", marker + 1U),
														  marker + 1U,
														  writer.holder.generation(),
														  &page);
			auto second_reader = install_live_reader_group(coordinator,
														   binding,
														   identity("test.connection", marker + 2U),
														   marker + 2U,
														   writer.holder.generation(),
														   &page);
			auto [first_unmap, first_callback] =
				form_ready_unmap(coordinator, first_reader, marker + 3U);
			const auto first_effect = identity("test.reader-unmap-latch-source-effect", marker);
			const auto shared_latch = identity("test.reader-unmap-shared-latch", marker);
			const auto first_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
					first_unmap,
					first_callback,
					sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
					sqlite_ok_status,
					0,
					0,
					first_effect,
					shared_latch);
			auto first_completed = coordinator.complete_reader_unmap(first_unmap, first_receipt);
			require(first_completed &&
						first_completed->kind() ==
							sqlite_shm_reader_unmap_terminal_kind::retired_confirmed &&
						first_completed->latch_reset_receipt() &&
						*first_completed->latch_reset_receipt() == shared_latch &&
						!first_unmap.valid(),
					"store exact source latch before cross-owner latch replay");
			auto [second_unmap, second_callback] =
				form_ready_unmap(coordinator, second_reader, marker + 4U);
			const auto before_replay =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto second_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
					second_unmap,
					second_callback,
					sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
					sqlite_ok_status,
					0,
					0,
					identity("test.reader-unmap-latch-target-effect", marker),
					shared_latch);
			auto rejected = coordinator.complete_reader_unmap(second_unmap, second_receipt);
			const auto after_replay =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto terminal = std::ranges::find_if(
				after_replay.terminal_quarantines,
				[&](const sqlite_shm_reader_terminal_quarantine_test_view& value)
				{
					return value.callback && *value.callback == second_callback;
				});
			const auto confirmed_group_count = std::ranges::count(
				after_replay.attachment_groups,
				detail::sqlite_shm_reader_attachment_group_phase::native_unmap_confirmed,
				&sqlite_shm_reader_attachment_group_test_view::phase);
			const auto quarantined_group_count = std::ranges::count(
				after_replay.attachment_groups,
				detail::sqlite_shm_reader_attachment_group_phase::terminal_quarantined,
				&sqlite_shm_reader_attachment_group_test_view::phase);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					rejected.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					!second_unmap.valid() && coordinator.snapshot().quarantined &&
					before_replay.outstanding_terminal_permit_count == 1U &&
					after_replay.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(after_replay) &&
					all_reader_live_custody_released(after_replay) && confirmed_group_count == 1U &&
					quarantined_group_count == 1U &&
					terminal != after_replay.terminal_quarantines.end() &&
					terminal->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::presented_invalid &&
					!terminal->native_effect_receipt && !terminal->latch_reset_receipt &&
					!terminal->exact_terminal_receipt_retained,
				"reused latch identity did not preserve the source receipt while terminally "
				"rejecting the target obligation");
		}

		{
			constexpr std::uint8_t marker = 171U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto reader = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			auto [unmap, unmap_callback] = form_ready_unmap(coordinator, reader, marker + 2U);
			const auto aliased_identity =
				identity("test.reader-unmap-aliased-effect-latch", marker);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
				unmap,
				unmap_callback,
				sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
				sqlite_ok_status,
				0,
				0,
				aliased_identity,
				aliased_identity);
			auto rejected = coordinator.complete_reader_unmap(unmap, receipt);
			const auto lifecycle =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					rejected.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					!unmap.valid() && coordinator.snapshot().quarantined &&
					lifecycle.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(lifecycle) &&
					all_reader_live_custody_released(lifecycle) &&
					lifecycle.terminal_quarantines.size() == 1U &&
					lifecycle.terminal_quarantines.front().reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::presented_invalid &&
					!lifecycle.terminal_quarantines.front().native_effect_receipt &&
					!lifecycle.terminal_quarantines.front().latch_reset_receipt &&
					!lifecycle.terminal_quarantines.front().exact_terminal_receipt_retained,
				"one identity cannot authorize both native effect and latch-reset roles");
		}

		{
			constexpr std::uint8_t marker = 154U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto reader = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			auto [unmap, unmap_callback] = form_ready_unmap(coordinator, reader, marker + 2U);
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto group_token = last_reader_lifecycle_event_owner(
				before, detail::sqlite_shm_reader_lifecycle_event_kind::unmap_cut);
			const auto effect = identity("test.reader-unmap-post-receipt-effect", marker);
			const auto latch = identity("test.reader-unmap-post-receipt-latch", marker);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_unmap_terminal(
				unmap,
				unmap_callback,
				sqlite_shm_reader_unmap_evidence_kind::exact_native_result,
				sqlite_ok_status,
				0,
				0,
				effect,
				latch);
			sqlite_same_process_shm_lease_test_peer::fail_reader_unmap_after_exact_receipt(
				coordinator);
			auto failed = coordinator.complete_reader_unmap(unmap, receipt);
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto* quarantine = find_reader_terminal_quarantine(after, group_token);
			require(
				!failed &&
					failed.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					failed.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					!unmap.valid() && coordinator.snapshot().quarantined &&
					before.outstanding_terminal_permit_count == 1U &&
					after.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(after) &&
					all_reader_live_custody_released(after) &&
					after.attachment_groups.size() == 1U &&
					after.attachment_groups.front().phase ==
						detail::sqlite_shm_reader_attachment_group_phase::terminal_quarantined &&
					quarantine != nullptr &&
					quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::internal_failure &&
					quarantine->callback && *quarantine->callback == unmap_callback &&
					quarantine->native_effect_receipt &&
					*quarantine->native_effect_receipt == effect &&
					quarantine->unmap_evidence_kind ==
						sqlite_shm_reader_unmap_evidence_kind::exact_native_result &&
					quarantine->native_status == sqlite_ok_status &&
					quarantine->latch_reset_receipt && *quarantine->latch_reset_receipt == latch &&
					quarantine->exact_terminal_receipt_retained,
				"exact unmap receipt was not retained before downstream state failure closed "
				"the obligation");
			const auto before_replay = after;
			auto replay = coordinator.complete_reader_unmap(unmap, receipt);
			const auto replayed =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(!replay &&
						replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
						replayed.last_issued_sequence == before_replay.last_issued_sequence &&
						replayed.last_committed_sequence == before_replay.last_committed_sequence &&
						replayed.outstanding_terminal_permit_slots ==
							before_replay.outstanding_terminal_permit_slots,
					"post-receipt unmap state failure retained a second native completion path");
		}

		{
			constexpr std::uint8_t marker = 162U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			auto reader = install_live_reader_group(coordinator,
													binding,
													identity("test.connection", marker + 1U),
													marker + 1U,
													writer.holder.generation(),
													&page);
			auto [unmap, unmap_callback] = form_ready_unmap(coordinator, reader, marker + 2U);
			sqlite_same_process_shm_lease_test_peer::throw_reader_unmap_terminal_exception(
				coordinator);
			auto failed = coordinator.complete_reader_unmap(
				unmap, unmap_callback, sqlite_shm_native_cleanup_outcome::confirmed_success);
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!failed &&
					failed.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					failed.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					!unmap.valid() && coordinator.snapshot().quarantined &&
					after.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(after) &&
					all_reader_live_custody_released(after) &&
					after.terminal_quarantines.size() == 1U &&
					after.terminal_quarantines.front().reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::internal_failure &&
					!after.terminal_quarantines.front().exact_terminal_receipt_retained,
				"coarse unmap terminal exception did not invalidate and quarantine the accepted "
				"native obligation");
			auto replay = coordinator.complete_reader_unmap(
				unmap, unmap_callback, sqlite_shm_native_cleanup_outcome::confirmed_success);
			require(!replay &&
						replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
					"coarse unmap terminal exception retained a second native completion path");
		}
	}

	void verify_reader_open_epoch_close_routes_are_exact_and_one_shot()
	{
		{
			constexpr std::uint8_t marker = 41U;
			constexpr std::uint64_t open_token = 701U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto open = register_reader_open(
				coordinator, open_token, reader_open_epoch_binding(family_binding, marker));
			const auto issued =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto issued_snapshot = coordinator.snapshot();
			require(issued.open_epochs.size() == 1U,
					"authenticated open publishes one exact lifecycle row");
			require(
				issued.open_epochs.front().registry_open_token == open_token &&
					issued.open_epochs.front().binding == open.binding &&
					issued.open_epochs.front().close_owner_token != 0U &&
					issued.open_epochs.front().phase ==
						detail::sqlite_shm_reader_connection_close_phase::open &&
					issued.open_epochs.front().origin_sequence != 0U &&
					issued.open_epochs.front().initial_close_cut_permit_slot != 0U &&
					issued.open_epochs.front().initial_close_terminal_permit_slot != 0U &&
					issued.open_epochs.front().initial_close_terminal_permit_slot !=
						issued.open_epochs.front().initial_close_cut_permit_slot &&
					issued.open_epochs.front().close_cut_permit_slot ==
						issued.open_epochs.front().initial_close_cut_permit_slot &&
					issued.open_epochs.front().close_terminal_permit_slot ==
						issued.open_epochs.front().initial_close_terminal_permit_slot &&
					issued.open_epochs.front().close_cut_sequence == 0U &&
					issued.open_epochs.front().destination_sequence == 0U,
				"authenticated open row retains exact owner, binding, and distinct close slots");
			require(issued.outstanding_terminal_permit_count == 2U &&
						reader_terminal_permit_slots_are_exact(issued) &&
						issued.live_custody_kind_counts[enum_index(
							detail::sqlite_shm_reader_custody_kind::connection_close)] == 1U,
					"authenticated open retains two permits and one connection-close custody");
			require(issued_snapshot.reader_registry_open_count == 1U &&
						issued_snapshot.reader_open_close_owner_count == 1U &&
						issued_snapshot.reader_close_admitted_count == 0U &&
						issued_snapshot.reader_close_terminal_count == 0U,
					"authenticated open snapshot exposes one independent close owner");

			const auto close_callback = callback(9U, marker);
			auto begun = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				coordinator,
				open.registry_open_token,
				open.seal,
				open.binding,
				sqlite_shm_reader_close_request{close_callback});
			require(begun && begun->valid() &&
						begun->route() == sqlite_shm_reader_close_route::close_without_group,
					"admit no-group reader close");
			const auto cut =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto cut_snapshot = coordinator.snapshot();
			require(cut.open_epochs.size() == 1U, "close cut retains one exact open row");
			require(cut.open_epochs.front().phase ==
							detail::sqlite_shm_reader_connection_close_phase::close_admitted &&
						cut.open_epochs.front().route ==
							sqlite_shm_reader_close_route::close_without_group &&
						cut.open_epochs.front().close_cut_permit_slot == 0U &&
						cut.open_epochs.front().close_terminal_permit_slot ==
							cut.open_epochs.front().initial_close_terminal_permit_slot &&
						cut.open_epochs.front().close_cut_sequence >
							cut.open_epochs.front().origin_sequence,
					"close cut consumes its cut slot and commits a later lifecycle sequence");
			require(cut.outstanding_terminal_permit_count == 1U &&
						reader_terminal_permit_slots_are_exact(cut),
					"close cut leaves only its exact terminal permit");
			require(count_reader_lifecycle_events(
						cut, detail::sqlite_shm_reader_lifecycle_event_kind::close_cut) == 1U &&
						last_reader_lifecycle_event_owner(
							cut, detail::sqlite_shm_reader_lifecycle_event_kind::close_cut) ==
							cut.open_epochs.front().close_owner_token,
					"close cut event is bound to the exact close owner");
			require(cut.live_custody_kind_counts[enum_index(
						detail::sqlite_shm_reader_custody_kind::connection_close)] == 0U &&
						cut.live_custody_kind_counts[enum_index(
							detail::sqlite_shm_reader_custody_kind::close_cut_or_composite)] ==
							1U &&
						cut_snapshot.reader_open_close_owner_count == 0U &&
						cut_snapshot.reader_close_admitted_count == 1U,
					"close cut transfers connection custody to one exact close-cut owner");

			const auto effect = identity("test.reader-close-effect", marker);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
				*begun,
				close_callback,
				sqlite_shm_reader_close_evidence_kind::exact_native_result,
				sqlite_ok_status,
				effect);
			auto completed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator, open.registry_open_token, open.seal, open.binding, *begun, receipt);
			const auto closed =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				completed && completed->kind() == sqlite_shm_reader_close_terminal_kind::closed &&
					completed->route() == sqlite_shm_reader_close_route::close_without_group &&
					completed->evidence_kind() ==
						sqlite_shm_reader_close_evidence_kind::exact_native_result &&
					completed->native_status() == sqlite_ok_status &&
					completed->outward_status() == sqlite_ok_status &&
					completed->native_effect_receipt() &&
					*completed->native_effect_receipt() == effect && !begun->valid() &&
					closed.open_epochs.size() == 1U &&
					closed.open_epochs.front().phase ==
						detail::sqlite_shm_reader_connection_close_phase::closed &&
					closed.open_epochs.front().close_terminal_permit_slot == 0U &&
					closed.open_epochs.front().destination_sequence >
						closed.open_epochs.front().close_cut_sequence &&
					closed.close_terminals.size() == 1U &&
					closed.close_terminals.front().kind ==
						sqlite_shm_reader_close_terminal_kind::closed &&
					closed.close_terminals.front().native_effect_receipt == effect &&
					closed.close_terminals.front().exact_terminal_receipt_retained &&
					closed.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(closed) &&
					all_reader_live_custody_released(closed) &&
					count_reader_lifecycle_events(
						closed,
						detail::sqlite_shm_reader_lifecycle_event_kind::cut_terminal_commit) == 1U,
				"exact xClose terminal consumes the orthogonal owner once and retains its "
				"receipt");
			const auto before_replay = closed;
			auto replay = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator, open.registry_open_token, open.seal, open.binding, *begun, receipt);
			const auto after_replay =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!replay &&
					replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
					after_replay.last_issued_sequence == before_replay.last_issued_sequence &&
					after_replay.last_committed_sequence == before_replay.last_committed_sequence &&
					after_replay.close_terminals.size() == before_replay.close_terminals.size() &&
					after_replay.live_custody_kind_counts == before_replay.live_custody_kind_counts,
				"closed obligation and exact receipt are one-shot");
			require(sqlite_same_process_shm_lease_test_peer::release_reader_open(
						coordinator, open.registry_open_token, open.seal)
							.has_value() &&
						coordinator.snapshot().reader_registry_open_count == 0U,
					"release exact closed open lineage");
		}

		{
			constexpr std::uint8_t marker = 83U;
			constexpr std::uint64_t open_token = 702U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto attachment =
				sqlite_same_process_shm_lease_test_peer::reader_attachment_reservation(
					family_binding,
					identity("test.reader-runtime-lifetime-pin", marker),
					identity("test.alias-lifetime", marker),
					identity("test.connection", marker),
					identity("test.reader-main-native-file-receipt", marker),
					identity("test.reader-main-xopen-receipt", marker),
					identity("test.reader-open-epoch", marker),
					1U,
					identity("test.reader-callback-cohort", marker),
					identity("test.reader-forged-attachment-epoch", marker),
					open_token);
			require(attachment.has_value(), "bind forged registry-token reader attachment");
			auto open = register_reader_open(
				coordinator, open_token, reader_open_epoch_binding(*attachment));
			const sqlite_shm_reader_session_request forged_session{
				*attachment,
				reader_session_execution(marker),
				identity("test.reader-forged-transaction", marker),
				identity("test.reader-forged-decode", marker),
				identity("test.reader-forged-authority", marker),
			};
			const auto before_forgery =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			auto rejected = coordinator.begin_reader_session(forged_session);
			const auto after_forgery =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(!rejected &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch,
					"copied registry-open token is rejected by the direct session route");
			require(after_forgery.last_issued_sequence == before_forgery.last_issued_sequence &&
						after_forgery.last_committed_sequence ==
							before_forgery.last_committed_sequence &&
						after_forgery.outstanding_terminal_permit_slots ==
							before_forgery.outstanding_terminal_permit_slots &&
						after_forgery.live_custody_kind_counts ==
							before_forgery.live_custody_kind_counts,
					"forged direct session rejection has zero lifecycle mutation");
			require(after_forgery.open_epochs.size() == 1U &&
						after_forgery.live_custody_kind_counts[enum_index(
							detail::sqlite_shm_reader_custody_kind::connection_close)] == 1U,
					"a copied registry-open token cannot create a direct session or map-derived "
					"close authority");

			const auto close_callback = callback(8U, marker);
			auto close = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				coordinator,
				open.registry_open_token,
				open.seal,
				open.binding,
				sqlite_shm_reader_close_request{close_callback});
			require(close && close->valid() &&
						close->route() == sqlite_shm_reader_close_route::close_without_group,
					"forged direct descendant leaves only the authenticated no-group close");
			const auto close_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
					*close,
					close_callback,
					sqlite_shm_reader_close_evidence_kind::exact_native_result,
					sqlite_ok_status,
					identity("test.reader-forged-route-close-effect", marker));
			auto closed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator,
				open.registry_open_token,
				open.seal,
				open.binding,
				*close,
				close_receipt);
			require(closed && closed->kind() == sqlite_shm_reader_close_terminal_kind::closed &&
						closed->route() == sqlite_shm_reader_close_route::close_without_group,
					"clean the authenticated open after forged descendant rejection");
			require(sqlite_same_process_shm_lease_test_peer::release_reader_open(
						coordinator, open.registry_open_token, open.seal)
						.has_value(),
					"release forged-route test open lineage");
		}
	}

	void verify_reader_close_authentication_and_cross_owner_are_fail_closed()
	{
		{
			constexpr std::uint8_t marker = 51U;
			constexpr std::uint64_t open_token = 711U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto open = register_reader_open(
				coordinator, open_token, reader_open_epoch_binding(family_binding, marker));
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto close_request = sqlite_shm_reader_close_request{callback(13U, marker)};
			auto wrong_seal = std::make_shared<detail::sqlite_shm_reader_open_lineage_seal>();
			auto wrong_binding = reader_open_epoch_binding(family_binding, marker + 1U);
			auto wrong_token = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				coordinator, open_token + 99U, open.seal, open.binding, close_request);
			auto rejected_seal = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				coordinator, open_token, wrong_seal, open.binding, close_request);
			auto rejected_binding = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				coordinator, open_token, open.seal, wrong_binding, close_request);
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(!wrong_token &&
						wrong_token.error().reason ==
							sqlite_shm_lease_rejection_reason::stale_token &&
						!rejected_seal &&
						rejected_seal.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch &&
						!rejected_binding &&
						rejected_binding.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch,
					"wrong reader-open token, seal, and binding fail before close admission");
			require(after.last_issued_sequence == before.last_issued_sequence &&
						after.last_committed_sequence == before.last_committed_sequence &&
						after.outstanding_terminal_permit_slots ==
							before.outstanding_terminal_permit_slots &&
						after.live_custody_kind_counts == before.live_custody_kind_counts &&
						after.events.size() == before.events.size() &&
						after.open_epochs.size() == 1U &&
						after.open_epochs.front().phase ==
							detail::sqlite_shm_reader_connection_close_phase::open &&
						after.open_epochs.front().close_cut_sequence == 0U &&
						after.open_epochs.front().destination_sequence == 0U,
					"reader-close authentication rejects with zero lifecycle mutation");
			close_and_release_registered_reader_open(coordinator, open, marker + 2U);
		}

		{
			constexpr std::uint8_t marker = 61U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			auto sequences = std::make_shared<sqlite_shm_reader_lifecycle_sequence_source>();
			sqlite_same_process_shm_mapping_lease_coordinator source{
				family_binding, generations, sequences};
			sqlite_same_process_shm_mapping_lease_coordinator target{
				family_binding, generations, sequences};
			auto source_open = register_reader_open(
				source, 721U, reader_open_epoch_binding(family_binding, marker));
			auto target_open = register_reader_open(
				target, 722U, reader_open_epoch_binding(family_binding, marker + 1U));
			const auto source_callback = callback(14U, marker);
			const auto target_callback = callback(14U, marker + 1U);
			auto source_close = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				source,
				source_open.registry_open_token,
				source_open.seal,
				source_open.binding,
				sqlite_shm_reader_close_request{source_callback});
			auto target_close = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				target,
				target_open.registry_open_token,
				target_open.seal,
				target_open.binding,
				sqlite_shm_reader_close_request{target_callback});
			require(source_close && target_close && source_close->valid() && target_close->valid(),
					"admit two same-numeric-owner close obligations on distinct coordinators");
			const auto source_before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(source);
			require(source_before.open_epochs.size() == 1U,
					"source close exposes one exact open row");
			const auto source_terminal_slot =
				source_before.open_epochs.front().close_terminal_permit_slot;
			const auto source_effect =
				identity("test.reader-cross-coordinator-close-effect", marker);
			const auto source_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
					*source_close,
					source_callback,
					sqlite_shm_reader_close_evidence_kind::exact_native_result,
					sqlite_ok_status,
					source_effect);
			auto cross = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				target,
				target_open.registry_open_token,
				target_open.seal,
				target_open.binding,
				*target_close,
				source_receipt);
			const auto source_after_cross =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(source);
			const auto target_after_cross =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(target);
			require(
				!cross &&
					cross.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					!target_close->valid() && source_close->valid() &&
					target.snapshot().quarantined && !source.snapshot().quarantined,
				"a same-numeric-owner receipt from another coordinator quarantines only target");
			require(
				source_after_cross.last_issued_sequence > source_before.last_issued_sequence &&
					source_after_cross.last_committed_sequence ==
						source_before.last_committed_sequence &&
					source_after_cross.live_custody_kind_counts ==
						source_before.live_custody_kind_counts &&
					source_after_cross.open_epochs.size() == 1U &&
					source_after_cross.open_epochs.front().close_terminal_permit_slot ==
						source_terminal_slot &&
					std::ranges::contains(source_after_cross.outstanding_terminal_permit_slots,
										  source_terminal_slot) &&
					target_after_cross.open_epochs.size() == 1U &&
					target_after_cross.open_epochs.front().phase ==
						detail::sqlite_shm_reader_connection_close_phase::terminal_quarantined &&
					target_after_cross.open_epochs.front().close_cut_permit_slot == 0U &&
					target_after_cross.open_epochs.front().close_terminal_permit_slot == 0U,
				"cross-coordinator close receipt preserves source and terminalizes target");
			auto source_completed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				source,
				source_open.registry_open_token,
				source_open.seal,
				source_open.binding,
				*source_close,
				source_receipt);
			require(source_completed &&
						source_completed->kind() == sqlite_shm_reader_close_terminal_kind::closed &&
						!source_close->valid(),
					"source coordinator retains its one exact close completion path");
		}
	}

	void verify_reader_close_terminal_failures_are_fail_closed()
	{
		const auto begin_no_group_close =
			[](sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
			   const sqlite_shm_lease_family_binding& family_binding,
			   const std::uint64_t open_token,
			   const std::uint8_t marker)
		{
			auto open = register_reader_open(
				coordinator, open_token, reader_open_epoch_binding(family_binding, marker));
			const auto close_callback = callback(11U, marker);
			auto close = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				coordinator,
				open.registry_open_token,
				open.seal,
				open.binding,
				sqlite_shm_reader_close_request{close_callback});
			require(close && close->valid() &&
						close->route() == sqlite_shm_reader_close_route::close_without_group,
					"admit close-terminal failure fixture");
			return std::tuple{
				std::move(open),
				std::move(*close),
				close_callback,
			};
		};

		{
			constexpr std::uint8_t marker = 91U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto [open, close, close_callback] =
				begin_no_group_close(coordinator, family_binding, 801U, marker);
			const auto effect = identity("test.reader-close-non-ok-effect", marker);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
				close,
				close_callback,
				sqlite_shm_reader_close_evidence_kind::exact_native_result,
				sqlite_busy_status,
				effect);
			auto completed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator, open.registry_open_token, open.seal, open.binding, close, receipt);
			const auto terminal =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				completed &&
					completed->kind() ==
						sqlite_shm_reader_close_terminal_kind::terminal_quarantined &&
					completed->evidence_kind() ==
						sqlite_shm_reader_close_evidence_kind::exact_native_result &&
					completed->native_status() == sqlite_busy_status &&
					completed->outward_status() == sqlite_busy_status &&
					completed->native_effect_receipt() &&
					*completed->native_effect_receipt() == effect && !close.valid() &&
					coordinator.snapshot().quarantined && terminal.open_epochs.size() == 1U &&
					terminal.open_epochs.front().phase ==
						detail::sqlite_shm_reader_connection_close_phase::terminal_quarantined &&
					terminal.close_terminals.size() == 1U &&
					terminal.close_terminals.front().reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::
							native_non_ok_or_unknown &&
					terminal.close_terminals.front().exact_terminal_receipt_retained &&
					terminal.outstanding_terminal_permit_count == 0U &&
					all_reader_live_custody_released(terminal),
				"deterministic non-OK xClose is preserved only after a durable terminal "
				"quarantine");
			const auto before_replay = terminal;
			auto replay = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator, open.registry_open_token, open.seal, open.binding, close, receipt);
			const auto replayed =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(!replay &&
						replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
						replayed.last_issued_sequence == before_replay.last_issued_sequence &&
						replayed.close_terminals.size() == before_replay.close_terminals.size(),
					"non-OK close terminal cannot be retried");
		}

		{
			constexpr std::uint8_t marker = 92U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto [open, close, close_callback] =
				begin_no_group_close(coordinator, family_binding, 802U, marker);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
				close,
				close_callback,
				sqlite_shm_reader_close_evidence_kind::throw_or_unknown,
				std::nullopt,
				std::nullopt);
			auto completed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator, open.registry_open_token, open.seal, open.binding, close, receipt);
			const auto terminal =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(completed &&
						completed->kind() ==
							sqlite_shm_reader_close_terminal_kind::terminal_quarantined &&
						completed->evidence_kind() ==
							sqlite_shm_reader_close_evidence_kind::throw_or_unknown &&
						!completed->native_status() &&
						completed->outward_status() == sqlite_ioerr_status &&
						!completed->native_effect_receipt() && !close.valid() &&
						terminal.close_terminals.size() == 1U &&
						terminal.close_terminals.front().exact_terminal_receipt_retained &&
						terminal.outstanding_terminal_permit_count == 0U &&
						all_reader_live_custody_released(terminal),
					"throw or unknown xClose projects IOERR and never retains a retry path");
		}

		{
			constexpr std::uint8_t marker = 93U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto [open, close, close_callback] =
				begin_no_group_close(coordinator, family_binding, 803U, marker);
			const auto effect = identity("test.reader-close-commit-failure-effect", marker);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
				close,
				close_callback,
				sqlite_shm_reader_close_evidence_kind::exact_native_result,
				sqlite_ok_status,
				effect);
			sqlite_same_process_shm_lease_test_peer::fail_next_reader_close_terminal_commit(
				coordinator);
			auto failed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator, open.registry_open_token, open.seal, open.binding, close, receipt);
			const auto terminal =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						!close.valid() && coordinator.snapshot().quarantined &&
						terminal.close_terminals.size() == 1U &&
						terminal.close_terminals.front().kind ==
							sqlite_shm_reader_close_terminal_kind::terminal_quarantined &&
						terminal.close_terminals.front().reason ==
							detail::sqlite_shm_reader_terminal_quarantine_reason::
								injected_commit_failure &&
						terminal.close_terminals.front().native_effect_receipt == effect &&
						terminal.close_terminals.front().exact_terminal_receipt_retained &&
						terminal.outstanding_terminal_permit_count == 0U &&
						all_reader_live_custody_released(terminal),
					"close terminal commit failure retains exact evidence while publishing no "
					"success");
		}

		{
			constexpr std::uint8_t marker = 94U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto open = register_reader_open(
				coordinator, 804U, reader_open_epoch_binding(family_binding, marker));
			sqlite_same_process_shm_lease_test_peer::fail_reader_close_begin_preparation(
				coordinator);
			auto failed = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				coordinator,
				open.registry_open_token,
				open.seal,
				open.binding,
				sqlite_shm_reader_close_request{callback(11U, marker)});
			const auto terminal =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!failed &&
					failed.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					coordinator.snapshot().quarantined && terminal.open_epochs.size() == 1U &&
					terminal.open_epochs.front().phase ==
						detail::sqlite_shm_reader_connection_close_phase::terminal_quarantined &&
					terminal.close_terminals.empty() &&
					terminal.outstanding_terminal_permit_count == 0U &&
					all_reader_live_custody_released(terminal),
				"close preparation exception quarantines before exposing native authority");
		}

		{
			constexpr std::uint8_t marker = 95U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto [open, close, close_callback] =
				begin_no_group_close(coordinator, family_binding, 805U, marker);
			const auto effect = identity("test.reader-close-post-receipt-effect", marker);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
				close,
				close_callback,
				sqlite_shm_reader_close_evidence_kind::exact_native_result,
				sqlite_ok_status,
				effect);
			sqlite_same_process_shm_lease_test_peer::fail_reader_close_after_exact_receipt(
				coordinator);
			auto failed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator, open.registry_open_token, open.seal, open.binding, close, receipt);
			const auto terminal =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!failed &&
					failed.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					!close.valid() && coordinator.snapshot().quarantined &&
					terminal.open_epochs.size() == 1U &&
					terminal.open_epochs.front().phase ==
						detail::sqlite_shm_reader_connection_close_phase::terminal_quarantined &&
					terminal.close_terminals.size() == 1U &&
					terminal.close_terminals.front().native_effect_receipt == effect &&
					terminal.close_terminals.front().exact_terminal_receipt_retained &&
					terminal.outstanding_terminal_permit_count == 0U &&
					all_reader_live_custody_released(terminal),
				"post-receipt close exception retains exact native evidence and no retry");
		}
	}

	void verify_reader_close_invalid_receipts_abandonment_and_lock_faults()
	{
		const auto begin_no_group_close =
			[](sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
			   const sqlite_shm_lease_family_binding& family_binding,
			   const std::uint64_t open_token,
			   const std::uint8_t marker)
		{
			auto open = register_reader_open(
				coordinator, open_token, reader_open_epoch_binding(family_binding, marker));
			const auto close_callback = callback(15U, marker);
			auto close = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				coordinator,
				open.registry_open_token,
				open.seal,
				open.binding,
				sqlite_shm_reader_close_request{close_callback});
			require(close && close->valid() &&
						close->route() == sqlite_shm_reader_close_route::close_without_group,
					"admit invalid-close-receipt fixture");
			return std::tuple{
				std::move(open),
				std::move(*close),
				close_callback,
			};
		};

		const auto require_invalid_terminal =
			[](const sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
			   const sqlite_shm_reader_close_obligation& close,
			   const std::string_view context)
		{
			const auto terminal =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!close.valid() && coordinator.snapshot().quarantined &&
					terminal.open_epochs.size() == 1U &&
					terminal.open_epochs.front().phase ==
						detail::sqlite_shm_reader_connection_close_phase::terminal_quarantined &&
					terminal.open_epochs.front().close_cut_permit_slot == 0U &&
					terminal.open_epochs.front().close_terminal_permit_slot == 0U &&
					terminal.outstanding_terminal_permit_count == 0U &&
					terminal.close_terminals.empty() && all_reader_live_custody_released(terminal),
				context);
		};

		{
			constexpr std::uint8_t marker = 101U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto [open, close, close_callback] =
				begin_no_group_close(coordinator, family_binding, 811U, marker);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
				close,
				callback(15U, marker + 1U),
				sqlite_shm_reader_close_evidence_kind::exact_native_result,
				sqlite_ok_status,
				identity("test.reader-close-wrong-callback-effect", marker));
			auto failed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator, open.registry_open_token, open.seal, open.binding, close, receipt);
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
					"wrong close callback fails terminally");
			require_invalid_terminal(
				coordinator, close, "wrong close callback terminalizes only its exact owner");
			(void)close_callback;
		}

		{
			constexpr std::uint8_t marker = 102U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto [open, close, close_callback] =
				begin_no_group_close(coordinator, family_binding, 812U, marker);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
				close,
				close_callback,
				sqlite_shm_reader_close_evidence_kind::exact_native_result,
				std::nullopt,
				identity("test.reader-close-invalid-evidence-effect", marker));
			auto failed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator, open.registry_open_token, open.seal, open.binding, close, receipt);
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
					"internally inconsistent close evidence fails terminally");
			require_invalid_terminal(
				coordinator, close, "invalid close evidence terminalizes only its exact owner");
		}

		{
			constexpr std::uint8_t marker = 108U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto [open, close, close_callback] =
				begin_no_group_close(coordinator, family_binding, 819U, marker);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
				close,
				close_callback,
				sqlite_shm_reader_close_evidence_kind::exact_native_result,
				sqlite_ok_status,
				std::nullopt);
			auto failed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator, open.registry_open_token, open.seal, open.binding, close, receipt);
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
					"exact close status without its effect proof fails terminally");
			require_invalid_terminal(
				coordinator, close, "missing exact close effect terminalizes only its exact owner");
		}

		{
			constexpr std::uint8_t marker = 109U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto [open, close, close_callback] =
				begin_no_group_close(coordinator, family_binding, 820U, marker);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
				close,
				close_callback,
				sqlite_shm_reader_close_evidence_kind::exact_native_result,
				-1,
				identity("test.reader-close-negative-status-effect", marker));
			auto failed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator, open.registry_open_token, open.seal, open.binding, close, receipt);
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
					"negative exact close status fails terminally");
			require_invalid_terminal(
				coordinator, close, "negative close status terminalizes only its exact owner");
		}

		{
			constexpr std::uint8_t marker = 110U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto [open, close, close_callback] =
				begin_no_group_close(coordinator, family_binding, 821U, marker);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
				close,
				close_callback,
				sqlite_shm_reader_close_evidence_kind::throw_or_unknown,
				sqlite_ioerr_status,
				identity("test.reader-close-unknown-with-fields-effect", marker));
			auto failed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator, open.registry_open_token, open.seal, open.binding, close, receipt);
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
					"throw-or-unknown close evidence with result fields fails terminally");
			require_invalid_terminal(
				coordinator,
				close,
				"unknown close evidence fields terminalize only their exact owner");
		}

		{
			constexpr std::uint8_t marker = 103U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto first = register_reader_open(
				coordinator, 813U, reader_open_epoch_binding(family_binding, marker));
			const auto first_callback = callback(15U, marker);
			auto first_close = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				coordinator,
				first.registry_open_token,
				first.seal,
				first.binding,
				sqlite_shm_reader_close_request{first_callback});
			require(first_close && first_close->valid(), "admit first close effect owner");
			const auto replayed_effect = identity("test.reader-close-replayed-effect", marker);
			const auto first_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
					*first_close,
					first_callback,
					sqlite_shm_reader_close_evidence_kind::exact_native_result,
					sqlite_ok_status,
					replayed_effect);
			require(sqlite_same_process_shm_lease_test_peer::complete_reader_close(
						coordinator,
						first.registry_open_token,
						first.seal,
						first.binding,
						*first_close,
						first_receipt)
						.has_value(),
					"commit first exact close effect");
			auto second = register_reader_open(
				coordinator, 814U, reader_open_epoch_binding(family_binding, marker + 1U));
			const auto second_callback = callback(15U, marker + 1U);
			auto second_close = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				coordinator,
				second.registry_open_token,
				second.seal,
				second.binding,
				sqlite_shm_reader_close_request{second_callback});
			require(second_close && second_close->valid(), "admit second close effect owner");
			const auto replay_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
					*second_close,
					second_callback,
					sqlite_shm_reader_close_evidence_kind::exact_native_result,
					sqlite_ok_status,
					replayed_effect);
			auto replayed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator,
				second.registry_open_token,
				second.seal,
				second.binding,
				*second_close,
				replay_receipt);
			const auto terminal =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto first_row =
				std::ranges::find(terminal.open_epochs,
								  first.registry_open_token,
								  &sqlite_shm_reader_open_epoch_test_view::registry_open_token);
			const auto second_row =
				std::ranges::find(terminal.open_epochs,
								  second.registry_open_token,
								  &sqlite_shm_reader_open_epoch_test_view::registry_open_token);
			require(
				!replayed &&
					replayed.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					!second_close->valid() && first_row != terminal.open_epochs.end() &&
					second_row != terminal.open_epochs.end() &&
					first_row->phase == detail::sqlite_shm_reader_connection_close_phase::closed &&
					second_row->phase ==
						detail::sqlite_shm_reader_connection_close_phase::terminal_quarantined &&
					terminal.close_terminals.size() == 1U &&
					terminal.close_terminals.front().native_effect_receipt == replayed_effect,
				"a close effect identity can terminalize exactly one owner");
		}

		{
			constexpr std::uint8_t marker = 104U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto open = register_reader_open(
				coordinator, 815U, reader_open_epoch_binding(family_binding, marker));
			const auto issued =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(issued.outstanding_terminal_permit_count == 2U,
					"abandonment fixture reserves both close slots");
			{
				auto close = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
					coordinator,
					open.registry_open_token,
					open.seal,
					open.binding,
					sqlite_shm_reader_close_request{callback(15U, marker)});
				require(close && close->valid(), "admit close obligation to abandon");
			}
			const auto abandoned =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				coordinator.snapshot().quarantined && abandoned.open_epochs.size() == 1U &&
					abandoned.open_epochs.front().phase ==
						detail::sqlite_shm_reader_connection_close_phase::terminal_quarantined &&
					abandoned.open_epochs.front().close_cut_sequence >
						abandoned.open_epochs.front().origin_sequence &&
					abandoned.open_epochs.front().destination_sequence >
						abandoned.open_epochs.front().close_cut_sequence &&
					abandoned.open_epochs.front().close_cut_permit_slot == 0U &&
					abandoned.open_epochs.front().close_terminal_permit_slot == 0U &&
					abandoned.outstanding_terminal_permit_count == 0U &&
					abandoned.close_terminals.empty() &&
					all_reader_live_custody_released(abandoned),
				"abandoned close consumes both close slots and retains no native retry");
		}

		{
			constexpr std::uint8_t marker = 105U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto open = register_reader_open(
				coordinator, 816U, reader_open_epoch_binding(family_binding, marker));
			sqlite_same_process_shm_lease_test_peer::fail_next_reader_operation_mutex_acquire(
				coordinator);
			auto failed = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				coordinator,
				open.registry_open_token,
				open.seal,
				open.binding,
				sqlite_shm_reader_close_request{callback(15U, marker)});
			const auto terminal =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!failed &&
					failed.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					terminal.open_epochs.size() == 1U &&
					terminal.open_epochs.front().phase ==
						detail::sqlite_shm_reader_connection_close_phase::terminal_quarantined &&
					terminal.outstanding_terminal_permit_count == 0U &&
					all_reader_live_custody_released(terminal),
				"close-begin operation-lock failure recovers by terminalizing the exact open");
		}

		{
			constexpr std::uint8_t marker = 106U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto [open, close, close_callback] =
				begin_no_group_close(coordinator, family_binding, 817U, marker);
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
				close,
				close_callback,
				sqlite_shm_reader_close_evidence_kind::exact_native_result,
				sqlite_ok_status,
				identity("test.reader-close-operation-lock-effect", marker));
			sqlite_same_process_shm_lease_test_peer::fail_next_reader_operation_mutex_acquire(
				coordinator);
			auto failed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator, open.registry_open_token, open.seal, open.binding, close, receipt);
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
					"close-complete operation-lock failure reports terminal ambiguity");
			require_invalid_terminal(
				coordinator,
				close,
				"close-complete operation-lock recovery terminalizes the exact obligation");
		}

		{
			constexpr std::uint8_t marker = 107U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			{
				auto [open, close, close_callback] =
					begin_no_group_close(coordinator, family_binding, 818U, marker);
				const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
					close,
					close_callback,
					sqlite_shm_reader_close_evidence_kind::exact_native_result,
					sqlite_ok_status,
					identity("test.reader-close-double-lock-effect", marker));
				sqlite_same_process_shm_lease_test_peer::fail_next_reader_operation_mutex_acquire(
					coordinator);
				sqlite_same_process_shm_lease_test_peer::fail_next_reader_recovery_mutex_reacquire(
					coordinator);
				auto failed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
					coordinator, open.registry_open_token, open.seal, open.binding, close, receipt);
				require(!failed &&
							failed.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							!close.valid() && coordinator.snapshot().quarantined,
						"close-complete double-lock failure disables terminal presentation while "
						"retaining poisoned abandonment custody");
				const auto before_replay =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				auto replay = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
					coordinator, open.registry_open_token, open.seal, open.binding, close, receipt);
				const auto after_replay =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				require(
					!replay &&
						(replay.error().reason ==
							 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
						 replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token) &&
						!close.valid() &&
						after_replay.last_issued_sequence == before_replay.last_issued_sequence &&
						after_replay.last_committed_sequence ==
							before_replay.last_committed_sequence &&
						after_replay.outstanding_terminal_permit_slots ==
							before_replay.outstanding_terminal_permit_slots,
					"close-complete double-lock failure retains no success or retry path");
			}
			const auto abandoned =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				abandoned.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(abandoned) &&
					all_reader_live_custody_released(abandoned) &&
					abandoned.open_epochs.size() == 1U &&
					abandoned.open_epochs.front().phase ==
						detail::sqlite_shm_reader_connection_close_phase::terminal_quarantined,
				"dropped close double-fault handle consumes its slots and all retained custody");
		}
	}

	void verify_reader_close_open_sequence_and_import_exhaustion_are_atomic()
	{
		const auto require_no_partial_open =
			[](const sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
			   const std::string_view context)
		{
			const auto view =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(view.open_epochs.empty() && view.close_terminals.empty() &&
						view.outstanding_terminal_permit_count == 0U &&
						all_reader_live_custody_released(view) &&
						coordinator.snapshot().reader_registry_open_count == 0U &&
						coordinator.snapshot().reader_open_close_owner_count == 0U,
					context);
		};

		{
			constexpr std::uint8_t marker = 111U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			sqlite_same_process_shm_lease_test_peer::exhaust_reader_lifecycle_sequences(
				coordinator);
			auto seal = std::make_shared<detail::sqlite_shm_reader_open_lineage_seal>();
			auto failed = sqlite_same_process_shm_lease_test_peer::register_reader_open(
				coordinator, 821U, seal, reader_open_epoch_binding(family_binding, marker));
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::generation_exhausted,
					"exhausted sequence source rejects reader open before publication");
			require_no_partial_open(coordinator,
									"sequence exhaustion leaves no partial close owner or slot");
		}

		{
			constexpr std::uint8_t marker = 112U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			sqlite_same_process_shm_lease_test_peer::make_reader_lifecycle_sequences_unavailable(
				coordinator);
			auto seal = std::make_shared<detail::sqlite_shm_reader_open_lineage_seal>();
			auto failed = sqlite_same_process_shm_lease_test_peer::register_reader_open(
				coordinator, 822U, seal, reader_open_epoch_binding(family_binding, marker));
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
					"unavailable sequence source rejects reader open before publication");
			require_no_partial_open(
				coordinator, "unavailable sequence source leaves no partial close owner or slot");
		}

		{
			constexpr std::uint8_t marker = 113U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			auto sequences = std::make_shared<sqlite_shm_reader_lifecycle_sequence_source>();
			sqlite_same_process_shm_mapping_lease_coordinator source{
				family_binding, generations, sequences};
			auto first = register_reader_open(
				source, 823U, reader_open_epoch_binding(family_binding, marker));
			close_and_release_registered_reader_open(source, first, marker);
			auto second = register_reader_open(
				source, 824U, reader_open_epoch_binding(family_binding, marker + 1U));
			close_and_release_registered_reader_open(source, second, marker + 1U);
			auto exported =
				sqlite_same_process_shm_lease_test_peer::export_reader_open_close_tombstones(
					source);
			require(exported && exported->size() == 2U,
					"export two exact close tombstones for allocator reconstruction");
			exported->at(0).close_owner_token = std::numeric_limits<std::uint64_t>::max();
			exported->at(1).close_owner_token = std::numeric_limits<std::uint64_t>::max() - 1U;
			sqlite_same_process_shm_mapping_lease_coordinator recreated{
				family_binding, generations, sequences};
			auto imported =
				sqlite_same_process_shm_lease_test_peer::import_reader_open_close_tombstones(
					recreated, *exported);
			require(imported.has_value(),
					"max-owner-first tombstone batch imports independent of input order");
			auto fresh_seal = std::make_shared<detail::sqlite_shm_reader_open_lineage_seal>();
			auto exhausted = sqlite_same_process_shm_lease_test_peer::register_reader_open(
				recreated,
				825U,
				fresh_seal,
				reader_open_epoch_binding(family_binding, marker + 2U));
			const auto recreated_view =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(recreated);
			require(!exhausted &&
						exhausted.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						recreated_view.open_epoch_close_compact_tombstone_count == 2U &&
						recreated_view.open_epochs.empty() &&
						recreated_view.outstanding_terminal_permit_count == 0U &&
						all_reader_live_custody_released(recreated_view),
					"imported maximum close owner exhausts allocator without partial fresh owner");
		}
	}

	void verify_compact_reader_close_tombstones_preserve_replay_identities()
	{
		for (std::size_t index = 0U; index < 4U; ++index)
		{
			const auto marker = static_cast<std::uint8_t>(141U + index * 3U);
			const auto recreate = index >= 2U;
			const auto reuse_effect = index % 2U != 0U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			auto sequences = std::make_shared<sqlite_shm_reader_lifecycle_sequence_source>();
			sqlite_same_process_shm_mapping_lease_coordinator source{
				family_binding, generations, sequences};
			auto original = register_reader_open(
				source, 891U + index, reader_open_epoch_binding(family_binding, marker));
			const auto original_callback = callback(12U, marker);
			const auto original_effect =
				identity("test.reader-compact-close-replay-effect", marker);
			auto original_close = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				source,
				original.registry_open_token,
				original.seal,
				original.binding,
				sqlite_shm_reader_close_request{original_callback});
			require(original_close && original_close->valid(),
					"compact close replay fixture admits its original close");
			const auto original_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
					*original_close,
					original_callback,
					sqlite_shm_reader_close_evidence_kind::exact_native_result,
					sqlite_ok_status,
					original_effect);
			auto original_completed =
				sqlite_same_process_shm_lease_test_peer::complete_reader_close(
					source,
					original.registry_open_token,
					original.seal,
					original.binding,
					*original_close,
					original_receipt);
			require(original_completed &&
						original_completed->kind() ==
							sqlite_shm_reader_close_terminal_kind::closed &&
						!original_close->valid(),
					"compact close replay fixture commits its original exact receipt");
			require(sqlite_same_process_shm_lease_test_peer::release_reader_open(
						source, original.registry_open_token, original.seal)
						.has_value(),
					"compact close replay fixture releases its original open lineage");
			auto exported =
				sqlite_same_process_shm_lease_test_peer::export_reader_open_close_tombstones(
					source);
			require(
				exported && exported->size() == 1U &&
					exported->front().replay_identities.callback_invocation_tokens.size() == 1U &&
					exported->front().replay_identities.callback_invocation_tokens.front() ==
						original_callback.invocation_token &&
					exported->front().replay_identities.effect_receipts.size() == 1U &&
					exported->front().replay_identities.effect_receipts.front() == original_effect,
				"compact close tombstone retains the exact callback and native effect identities");

			std::unique_ptr<sqlite_same_process_shm_mapping_lease_coordinator> recreated;
			auto* active = &source;
			if (recreate)
			{
				recreated = std::make_unique<sqlite_same_process_shm_mapping_lease_coordinator>(
					family_binding, generations, sequences);
				require(
					sqlite_same_process_shm_lease_test_peer::import_reader_open_close_tombstones(
						*recreated, *exported)
						.has_value(),
					"recreate coordinator from exact compact close tombstone");
				active = recreated.get();
			}

			auto fresh = register_reader_open(
				*active, 901U + index, reader_open_epoch_binding(family_binding, marker + 1U));
			if (!reuse_effect)
			{
				auto rejected = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
					*active,
					fresh.registry_open_token,
					fresh.seal,
					fresh.binding,
					sqlite_shm_reader_close_request{callback(203U, marker)});
				const auto after =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(*active);
				require(!rejected &&
							rejected.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
							after.open_epochs.size() == 1U &&
							after.open_epochs.front().phase ==
								detail::sqlite_shm_reader_connection_close_phase::
									terminal_quarantined &&
							after.outstanding_terminal_permit_count == 0U &&
							reader_terminal_permit_slots_are_exact(after) &&
							all_reader_live_custody_released(after),
						recreate
							? "imported compact close callback cannot authorize a recreated close"
							: "compact close callback cannot authorize a later local close");
				continue;
			}

			const auto fresh_callback = callback(13U, marker + 1U);
			auto fresh_close = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				*active,
				fresh.registry_open_token,
				fresh.seal,
				fresh.binding,
				sqlite_shm_reader_close_request{fresh_callback});
			require(fresh_close && fresh_close->valid(),
					"compact effect replay fixture admits a fresh close callback");
			const auto replayed_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
					*fresh_close,
					fresh_callback,
					sqlite_shm_reader_close_evidence_kind::exact_native_result,
					sqlite_ok_status,
					original_effect);
			auto rejected = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				*active,
				fresh.registry_open_token,
				fresh.seal,
				fresh.binding,
				*fresh_close,
				replayed_receipt);
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(*active);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					!fresh_close->valid() && after.open_epochs.size() == 1U &&
					after.open_epochs.front().phase ==
						detail::sqlite_shm_reader_connection_close_phase::terminal_quarantined &&
					after.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(after) &&
					all_reader_live_custody_released(after),
				recreate ? "imported compact close effect cannot terminalize a recreated close"
						 : "compact close effect cannot terminalize a later local close");
		}
	}

	void verify_compact_reader_import_replay_census_is_atomic()
	{
		const auto require_zero_import_mutation =
			[](const sqlite_shm_reader_lifecycle_test_view& before,
			   const sqlite_shm_reader_lifecycle_test_view& after,
			   const std::string_view context)
		{
			require(after.last_issued_sequence == before.last_issued_sequence &&
						after.last_committed_sequence == before.last_committed_sequence &&
						after.outstanding_terminal_permit_slots ==
							before.outstanding_terminal_permit_slots &&
						after.live_custody_kind_counts == before.live_custody_kind_counts &&
						after.custody_state_counts == before.custody_state_counts &&
						after.attachment_reservation_phase_counts ==
							before.attachment_reservation_phase_counts &&
						after.session_reservation_phase_counts ==
							before.session_reservation_phase_counts &&
						after.compact_tombstone_count == before.compact_tombstone_count &&
						after.open_epoch_close_compact_tombstone_count ==
							before.open_epoch_close_compact_tombstone_count &&
						after.attachment_reservations.size() ==
							before.attachment_reservations.size() &&
						after.attachment_groups.size() == before.attachment_groups.size() &&
						after.session_reservations.size() == before.session_reservations.size() &&
						after.map_attempts.size() == before.map_attempts.size() &&
						after.open_epochs.size() == before.open_epochs.size() &&
						after.close_terminals.size() == before.close_terminals.size() &&
						after.events.size() == before.events.size(),
					context);
		};

		constexpr std::uint8_t marker = 165U;
		const auto family_binding = family(marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		auto sequences = std::make_shared<sqlite_shm_reader_lifecycle_sequence_source>();
		sqlite_same_process_shm_mapping_lease_coordinator close_source{
			family_binding, generations, sequences};
		auto source_open = register_reader_open(
			close_source, 931U, reader_open_epoch_binding(family_binding, marker));
		close_and_release_registered_reader_open(close_source, source_open, marker);
		auto close_tombstones =
			sqlite_same_process_shm_lease_test_peer::export_reader_open_close_tombstones(
				close_source);
		require(close_tombstones && close_tombstones->size() == 1U,
				"export exact close replay census for compact import counterexamples");
		const auto& close = close_tombstones->front();

		for (std::size_t index = 0U; index < 2U; ++index)
		{
			sqlite_same_process_shm_mapping_lease_coordinator target{
				family_binding, generations, sequences};
			const auto duplicate = identity(index == 0U ? "test.reader-import-duplicate-callback"
														: "test.reader-import-duplicate-effect",
											static_cast<std::uint8_t>(marker + index + 1U));
			auto first_callback = identity("test.reader-import-first-callback",
										   static_cast<std::uint8_t>(marker + index + 1U));
			auto second_callback = identity("test.reader-import-second-callback",
											static_cast<std::uint8_t>(marker + index + 1U));
			auto first_effect = identity("test.reader-import-first-effect",
										 static_cast<std::uint8_t>(marker + index + 1U));
			auto second_effect = identity("test.reader-import-second-effect",
										  static_cast<std::uint8_t>(marker + index + 1U));
			auto third_effect = identity("test.reader-import-third-effect",
										 static_cast<std::uint8_t>(marker + index + 1U));
			if (index == 0U)
			{
				first_callback = duplicate;
				second_callback = duplicate;
			}
			else
			{
				first_effect = duplicate;
				second_effect = duplicate;
			}
			const std::array tombstones{
				sqlite_shm_reader_lifecycle_compact_tombstone{
					reader_attachment_for_open(
						reader_open_epoch_binding(family_binding,
												  static_cast<std::uint8_t>(marker + index + 1U)),
						1U,
						identity("test.reader-import-duplicate-attachment",
								 static_cast<std::uint8_t>(marker + index + 1U)),
						941U + index),
					detail::sqlite_shm_reader_attachment_reservation_phase::retired_confirmed,
					close.origin_sequence,
					close.close_cut_sequence,
					{{first_callback, second_callback},
					 {first_effect, second_effect, third_effect},
					 {}},
				},
			};
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(target);
			auto rejected =
				sqlite_same_process_shm_lease_test_peer::import_reader_lifecycle_tombstones(
					target, tombstones);
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(target);
			require(!rejected &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::invalid_request,
					index == 0U ? "duplicate compact callback census is structurally invalid"
								: "duplicate compact effect census is structurally invalid");
			require_zero_import_mutation(
				before,
				after,
				index == 0U ? "duplicate callback import mutates no lifecycle ledger state"
							: "duplicate effect import mutates no lifecycle ledger state");
		}

		for (std::size_t index = 0U; index < 2U; ++index)
		{
			sqlite_same_process_shm_mapping_lease_coordinator target{
				family_binding, generations, sequences};
			require(sqlite_same_process_shm_lease_test_peer::import_reader_open_close_tombstones(
						target, *close_tombstones)
						.has_value(),
					"import exact close replay census before overlap counterexample");
			const auto overlap_callback = index == 0U
				? close.replay_identities.callback_invocation_tokens.front()
				: identity("test.reader-import-overlap-fresh-callback",
						   static_cast<std::uint8_t>(marker + index + 3U));
			const auto overlap_effect = index == 1U
				? close.replay_identities.effect_receipts.front()
				: identity("test.reader-import-overlap-fresh-effect",
						   static_cast<std::uint8_t>(marker + index + 3U));
			const std::array tombstones{
				sqlite_shm_reader_lifecycle_compact_tombstone{
					reader_attachment_for_open(
						reader_open_epoch_binding(family_binding,
												  static_cast<std::uint8_t>(marker + index + 3U)),
						1U,
						identity("test.reader-import-overlap-attachment",
								 static_cast<std::uint8_t>(marker + index + 3U)),
						951U + index),
					detail::sqlite_shm_reader_attachment_reservation_phase::revoked_no_map,
					close.origin_sequence,
					close.close_cut_sequence,
					{{overlap_callback}, {overlap_effect}, {}},
				},
			};
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(target);
			auto rejected =
				sqlite_same_process_shm_lease_test_peer::import_reader_lifecycle_tombstones(
					target, tombstones);
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(target);
			require(!rejected &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
					index == 0U ? "lifecycle callback cannot overlap an imported close callback"
								: "lifecycle effect cannot overlap an imported close effect");
			require_zero_import_mutation(
				before,
				after,
				index == 0U ? "close/lifecycle callback overlap mutates no lifecycle ledger state"
							: "close/lifecycle effect overlap mutates no lifecycle ledger state");
		}
	}

	void verify_invalid_top_level_close_completion_terminalizes_local_owner()
	{
		constexpr std::array<std::string_view, 6U> contexts{
			"zero registry token terminalizes its exact local close owner",
			"null lineage seal terminalizes its exact local close owner",
			"invalid open binding terminalizes its exact local close owner",
			"unknown nonzero registry token terminalizes its exact local close owner",
			"wrong nonnull lineage seal terminalizes its exact local close owner",
			"structurally valid wrong binding terminalizes its exact local close owner",
		};
		for (std::size_t index = 0U; index < contexts.size(); ++index)
		{
			const auto marker = static_cast<std::uint8_t>(153U + index * 2U);
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto open = register_reader_open(
				coordinator, 911U + index, reader_open_epoch_binding(family_binding, marker));
			const auto close_callback = callback(14U, marker);
			auto close = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				coordinator,
				open.registry_open_token,
				open.seal,
				open.binding,
				sqlite_shm_reader_close_request{close_callback});
			require(close && close->valid(),
					"invalid top-level close fixture admits one exact local owner");
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
				*close,
				close_callback,
				sqlite_shm_reader_close_evidence_kind::exact_native_result,
				sqlite_ok_status,
				identity("test.reader-invalid-top-level-close-effect", marker));

			const auto presented_token = index == 0U ? 0U
				: index == 3U						 ? open.registry_open_token + 1000U
													 : open.registry_open_token;
			const auto presented_seal = index == 1U
				? std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>{}
				: index == 4U ? std::make_shared<detail::sqlite_shm_reader_open_lineage_seal>()
							  : open.seal;
			const auto presented_binding = index == 2U ? sqlite_shm_reader_open_epoch_binding{}
				: index == 5U ? reader_open_epoch_binding(family_binding, marker + 1U)
							  : open.binding;
			auto rejected = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator, presented_token, presented_seal, presented_binding, *close, receipt);
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					!close->valid() && coordinator.snapshot().quarantined &&
					after.open_epochs.size() == 1U &&
					after.open_epochs.front().phase ==
						detail::sqlite_shm_reader_connection_close_phase::terminal_quarantined &&
					after.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(after) &&
					all_reader_live_custody_released(after),
				contexts[index]);
		}
	}

	void verify_guarded_reader_open_registration_has_one_atomic_publication_cut()
	{
		for (std::size_t index = 0U; index < 3U; ++index)
		{
			const auto marker = static_cast<std::uint8_t>(121U + index);
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto emergency_latch = std::make_shared<std::atomic_bool>(false);
			auto alias_latch = std::make_shared<std::atomic_bool>(true);
			auto family_latch = std::make_shared<std::atomic_bool>(true);
			if (index == 0U)
				alias_latch->store(false, std::memory_order_release);
			else if (index == 1U)
				family_latch->store(false, std::memory_order_release);
			else
				emergency_latch->store(true, std::memory_order_release);
			const detail::sqlite_shm_reader_open_admission_guard guard{
				emergency_latch,
				alias_latch,
				family_latch,
			};
			auto seal = std::make_shared<detail::sqlite_shm_reader_open_lineage_seal>();
			const auto binding = reader_open_epoch_binding(family_binding, marker);
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			auto rejected = sqlite_same_process_shm_lease_test_peer::register_reader_open(
				coordinator, 831U + index, seal, binding, guard);
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto snapshot = coordinator.snapshot();
			require(!rejected &&
						rejected.error().reason == sqlite_shm_lease_rejection_reason::quarantined &&
						after.last_issued_sequence == before.last_issued_sequence &&
						after.last_committed_sequence == before.last_committed_sequence &&
						after.outstanding_terminal_permit_slots ==
							before.outstanding_terminal_permit_slots &&
						after.live_custody_kind_counts == before.live_custody_kind_counts &&
						after.custody_state_counts == before.custody_state_counts &&
						after.open_epochs.empty() && after.close_terminals.empty() &&
						snapshot.reader_registry_open_count == 0U &&
						snapshot.reader_open_close_owner_count == 0U &&
						snapshot.reader_close_admitted_count == 0U &&
						snapshot.reader_close_terminal_count == 0U,
					"guard loss at the reader-open commit cut publishes no open, close custody, "
					"permit, or lifecycle sequence");

			alias_latch->store(true, std::memory_order_release);
			family_latch->store(true, std::memory_order_release);
			emergency_latch->store(false, std::memory_order_release);
			require(sqlite_same_process_shm_lease_test_peer::register_reader_open(
						coordinator, 831U + index, seal, binding, guard)
						.has_value(),
					"a valid guard may retry the exact unpublished reader-open identity");
			auto open =
				registered_reader_open_tokens{831U + index, std::move(seal), std::move(binding)};
			const auto admitted =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(admitted.open_epochs.size() == 1U &&
						admitted.open_epochs.front().close_owner_token == 1U &&
						admitted.open_epochs.front().origin_sequence == 1U,
					"guard rejection consumes neither the first close owner nor origin sequence");
			close_and_release_registered_reader_open(coordinator, open, marker);
		}
	}

	void verify_reader_lifecycle_import_rejects_live_open_token_or_binding_collisions()
	{
		const auto require_zero_import_mutation =
			[](const sqlite_shm_reader_lifecycle_test_view& before,
			   const sqlite_shm_reader_lifecycle_test_view& after,
			   const std::string_view context)
		{
			require(after.last_issued_sequence == before.last_issued_sequence &&
						after.last_committed_sequence == before.last_committed_sequence &&
						after.outstanding_terminal_permit_slots ==
							before.outstanding_terminal_permit_slots &&
						after.live_custody_kind_counts == before.live_custody_kind_counts &&
						after.custody_state_counts == before.custody_state_counts &&
						after.attachment_reservation_phase_counts ==
							before.attachment_reservation_phase_counts &&
						after.session_reservation_phase_counts ==
							before.session_reservation_phase_counts &&
						after.compact_tombstone_count == before.compact_tombstone_count &&
						after.open_epoch_close_compact_tombstone_count ==
							before.open_epoch_close_compact_tombstone_count &&
						after.attachment_reservations.size() ==
							before.attachment_reservations.size() &&
						after.attachment_groups.size() == before.attachment_groups.size() &&
						after.session_reservations.size() == before.session_reservations.size() &&
						after.map_attempts.size() == before.map_attempts.size() &&
						after.open_epochs.size() == before.open_epochs.size() &&
						after.close_terminals.size() == before.close_terminals.size() &&
						after.events.size() == before.events.size(),
					context);
			for (std::size_t index = 0U; index < before.open_epochs.size(); ++index)
			{
				require(after.open_epochs[index].registry_open_token ==
								before.open_epochs[index].registry_open_token &&
							after.open_epochs[index].binding == before.open_epochs[index].binding &&
							after.open_epochs[index].close_owner_token ==
								before.open_epochs[index].close_owner_token &&
							after.open_epochs[index].phase == before.open_epochs[index].phase &&
							after.open_epochs[index].origin_sequence ==
								before.open_epochs[index].origin_sequence &&
							after.open_epochs[index].close_cut_sequence ==
								before.open_epochs[index].close_cut_sequence &&
							after.open_epochs[index].destination_sequence ==
								before.open_epochs[index].destination_sequence,
						context);
			}
		};

		{
			constexpr std::uint8_t marker = 125U;
			const auto family_binding = family(marker);
			const auto helper_family = family(marker + 1U);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			auto sequences = std::make_shared<sqlite_shm_reader_lifecycle_sequence_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{
				family_binding, generations, sequences};
			sqlite_same_process_shm_mapping_lease_coordinator helper{
				helper_family, generations, sequences};
			auto open = register_reader_open(
				coordinator, 841U, reader_open_epoch_binding(family_binding, marker));
			auto helper_open = register_reader_open(
				helper, 842U, reader_open_epoch_binding(helper_family, marker + 1U));
			const auto mismatched_binding = reader_open_epoch_binding(family_binding, marker + 2U);
			const std::array tombstones{
				sqlite_shm_reader_lifecycle_compact_tombstone{
					reader_attachment_for_open(mismatched_binding,
											   1U,
											   identity("test.reader-live-token-collision", marker),
											   open.registry_open_token),
					detail::sqlite_shm_reader_attachment_reservation_phase::revoked_no_map,
					1U,
					2U,
					{{identity("test.reader-live-token-collision-callback", marker)},
					 {identity("test.reader-live-token-collision-effect", marker)},
					 {}},
				},
			};
			const auto before =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			auto rejected =
				sqlite_same_process_shm_lease_test_peer::import_reader_lifecycle_tombstones(
					coordinator, tombstones);
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(!rejected &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
					"active reader open rejects a compact attachment with the same token");
			require_zero_import_mutation(
				before,
				after,
				"same-token active-open tombstone rejection mutates no lease ledger state");
			close_and_release_registered_reader_open(coordinator, open, marker);
			close_and_release_registered_reader_open(helper, helper_open, marker + 1U);
		}

		{
			constexpr std::uint8_t marker = 129U;
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto open = register_reader_open(
				coordinator, 843U, reader_open_epoch_binding(family_binding, marker));
			const auto close_callback = callback(12U, marker);
			auto close = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
				coordinator,
				open.registry_open_token,
				open.seal,
				open.binding,
				sqlite_shm_reader_close_request{close_callback});
			require(close && close->valid(), "admit closed-unreleased import collision fixture");
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
				*close,
				close_callback,
				sqlite_shm_reader_close_evidence_kind::exact_native_result,
				sqlite_ok_status,
				identity("test.reader-closed-import-effect", marker));
			auto completed = sqlite_same_process_shm_lease_test_peer::complete_reader_close(
				coordinator, open.registry_open_token, open.seal, open.binding, *close, receipt);
			require(completed &&
						completed->kind() == sqlite_shm_reader_close_terminal_kind::closed &&
						!close->valid(),
					"retain an exactly closed but unreleased reader open");
			const auto closed =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(closed.open_epochs.size() == 1U &&
						closed.open_epochs.front().destination_sequence >
							closed.open_epochs.front().origin_sequence,
					"closed-unreleased fixture exposes ordered open lifecycle");
			const std::array tombstones{
				sqlite_shm_reader_lifecycle_compact_tombstone{
					reader_attachment_for_open(
						open.binding,
						1U,
						identity("test.reader-closed-binding-collision", marker),
						open.registry_open_token + 1U),
					detail::sqlite_shm_reader_attachment_reservation_phase::revoked_no_map,
					closed.open_epochs.front().origin_sequence,
					closed.open_epochs.front().destination_sequence,
					{{identity("test.reader-closed-binding-collision-callback", marker)},
					 {identity("test.reader-closed-binding-collision-effect", marker)},
					 {}},
				},
			};
			auto rejected =
				sqlite_same_process_shm_lease_test_peer::import_reader_lifecycle_tombstones(
					coordinator, tombstones);
			const auto after =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(!rejected &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
					"closed-unreleased reader open rejects a compact attachment with its full "
					"binding and a different token");
			require_zero_import_mutation(
				closed,
				after,
				"full-binding closed-open tombstone rejection mutates no lease ledger state");
			require(sqlite_same_process_shm_lease_test_peer::release_reader_open(
						coordinator, open.registry_open_token, open.seal)
						.has_value(),
					"rejected compact import leaves exact closed reader open releasable");
		}
	}

	void verify_attachment_tombstone_must_precede_its_exact_reader_close_cut()
	{
		for (std::size_t index = 0U; index < 2U; ++index)
		{
			const auto marker = static_cast<std::uint8_t>(133U + index);
			const auto family_binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{family_binding,
																		  generations};
			auto open = register_reader_open(
				coordinator, 851U + index, reader_open_epoch_binding(family_binding, marker));
			close_and_release_registered_reader_open(coordinator, open, marker);
			auto close_tombstones =
				sqlite_same_process_shm_lease_test_peer::export_reader_open_close_tombstones(
					coordinator);
			require(close_tombstones && close_tombstones->size() == 1U,
					"retain one exact reader-close tombstone for ordering counterexample");
			const auto& close = close_tombstones->front();
			const auto bad_destination =
				index == 0U ? close.close_cut_sequence : close.terminal_sequence;
			const std::array attachment_tombstones{
				sqlite_shm_reader_lifecycle_compact_tombstone{
					reader_attachment_for_open(
						close.binding,
						1U,
						identity("test.reader-close-ordering-attachment", marker),
						close.registry_open_token),
					detail::sqlite_shm_reader_attachment_reservation_phase::revoked_no_map,
					close.origin_sequence,
					bad_destination,
					{{identity("test.reader-close-ordering-callback", marker)},
					 {identity("test.reader-close-ordering-effect", marker)},
					 {}},
				},
			};
			require(sqlite_same_process_shm_lease_test_peer::import_reader_lifecycle_tombstones(
						coordinator, attachment_tombstones)
						.has_value(),
					"import structurally valid post-close attachment ordering counterexample");
			const auto before_export =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			auto rejected =
				sqlite_same_process_shm_lease_test_peer::export_reader_lifecycle_tombstones(
					coordinator);
			const auto after_export =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			require(
				!rejected &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					before_export.compact_tombstone_count == 1U &&
					after_export.compact_tombstone_count == before_export.compact_tombstone_count &&
					after_export.last_issued_sequence == before_export.last_issued_sequence &&
					after_export.last_committed_sequence == before_export.last_committed_sequence &&
					after_export.outstanding_terminal_permit_slots ==
						before_export.outstanding_terminal_permit_slots,
				index == 0U ? "attachment tombstone at the exact close cut cannot be exported"
							: "attachment tombstone after the close cut cannot be exported");
		}
	}

	void verify_different_token_same_binding_compact_group_blocks_reader_close_begin()
	{
		constexpr std::uint8_t marker = 137U;
		const auto family_binding = family(marker);
		const auto helper_family = family(marker + 1U);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		auto sequences = std::make_shared<sqlite_shm_reader_lifecycle_sequence_source>();
		sqlite_same_process_shm_mapping_lease_coordinator helper{
			helper_family, generations, sequences};
		auto first_helper = register_reader_open(
			helper, 861U, reader_open_epoch_binding(helper_family, marker + 1U));
		auto second_helper = register_reader_open(
			helper, 862U, reader_open_epoch_binding(helper_family, marker + 2U));

		sqlite_same_process_shm_mapping_lease_coordinator coordinator{
			family_binding, generations, sequences};
		const auto open_binding = reader_open_epoch_binding(family_binding, marker);
		constexpr std::uint64_t open_token = 863U;
		constexpr std::uint64_t foreign_token = 864U;
		const std::array tombstones{
			sqlite_shm_reader_lifecycle_compact_tombstone{
				reader_attachment_for_open(
					open_binding,
					1U,
					identity("test.reader-close-foreign-token-group", marker),
					foreign_token),
				detail::sqlite_shm_reader_attachment_reservation_phase::revoked_no_map,
				1U,
				2U,
				{{identity("test.reader-close-foreign-token-callback", marker)},
				 {identity("test.reader-close-foreign-token-effect", marker)},
				 {}},
			},
		};
		require(sqlite_same_process_shm_lease_test_peer::import_reader_lifecycle_tombstones(
					coordinator, tombstones)
					.has_value(),
				"import compact group before registering the colliding full reader-open binding");
		auto open = register_reader_open(coordinator, open_token, open_binding);
		const auto before =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(before.attachment_reservations.size() == 1U &&
					before.attachment_reservations.front().attachment.registry_open_token() ==
						foreign_token &&
					before.open_epochs.size() == 1U &&
					before.open_epochs.front().registry_open_token == open_token &&
					reader_open_epoch_binding(before.attachment_reservations.front().attachment) ==
						before.open_epochs.front().binding,
				"fixture retains a different token on the same full open-epoch binding");
		auto rejected = sqlite_same_process_shm_lease_test_peer::begin_reader_close(
			coordinator,
			open.registry_open_token,
			open.seal,
			open.binding,
			sqlite_shm_reader_close_request{callback(12U, marker)});
		const auto after =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		require(!rejected &&
					rejected.error().reason == sqlite_shm_lease_rejection_reason::retiring &&
					after.last_issued_sequence == before.last_issued_sequence &&
					after.last_committed_sequence == before.last_committed_sequence &&
					after.outstanding_terminal_permit_slots ==
						before.outstanding_terminal_permit_slots &&
					after.live_custody_kind_counts == before.live_custody_kind_counts &&
					after.custody_state_counts == before.custody_state_counts &&
					after.attachment_reservations.size() == before.attachment_reservations.size() &&
					after.open_epochs.size() == before.open_epochs.size() &&
					after.open_epochs.front().phase ==
						detail::sqlite_shm_reader_connection_close_phase::open &&
					after.open_epochs.front().close_cut_sequence == 0U &&
					after.close_terminals.empty() && !coordinator.snapshot().quarantined,
				"a registry-bound compact group with the same full binding and a different token "
				"blocks close before cut, custody transfer, or native authority");

		close_and_release_registered_reader_open(helper, first_helper, marker + 1U);
		close_and_release_registered_reader_open(helper, second_helper, marker + 2U);
	}

	void verify_reader_handoff_outlives_writer_and_blocks_successors()
	{
		const auto binding = family(12);
		const auto writer_connection = identity("test.connection", 12);
		const auto reader_connection = identity("test.connection", 13);
		const auto open_epoch = identity("test.open-epoch", 12);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int first_page{};
		int second_page{};
		const auto mapped = mapping(0, &first_page, 4096U);
		auto gate = install_eligibility(coordinator, binding, writer_connection, open_epoch, 12);
		auto pending = install_pending(coordinator,
									   writer_request(binding, writer_connection, 12, 1, 12, 0, 1),
									   open_epoch,
									   mapped,
									   sqlite_shm_writer_extend_pair::one_one,
									   12);
		auto holder = promote(coordinator, pending, gate);
		const auto generation = holder.generation();

		const auto request = reader_request(binding, reader_connection, 13, 2, 13);
		auto reader_inflight_result = coordinator.begin_reader_map(request);
		require(reader_inflight_result.has_value(), "reader acquires live generation pin");
		auto reader_inflight = std::move(*reader_inflight_result);
		const auto retirement_callback = callback(1, 100);
		auto retirement = coordinator.release_writer_holder(holder, retirement_callback);
		require(retirement &&
					retirement->decision() ==
						sqlite_shm_writer_retirement_decision::wait_for_inflight,
				"reader predelegate pin delays writer unmap");
		auto handoff_result = coordinator.promote_reader(
			reader_inflight,
			sqlite_same_process_shm_lease_test_peer::reader_map(
				request, generation, mapped, identity("test.zero-reader-resize", 13)));
		require(handoff_result.has_value(), "reader pin promotes once during retirement");
		auto handoff = std::move(*handoff_result);
		auto ready = coordinator.poll_writer_retirement(retirement->cleanup(), retirement_callback);
		require(ready && ready->decision == sqlite_shm_writer_retirement_decision::ready,
				"handoff lifetime does not block writer retirement");
		require(coordinator
					.complete_writer_cleanup(retirement->cleanup(),
											 retirement_callback,
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"writer retires while reader native attachment survives");
		const auto retired = coordinator.snapshot();
		require(retired.phase == sqlite_shm_mapping_generation_phase::retired &&
					retired.reader_handoff_count == 1U &&
					retired.generation_authority_count == 0U &&
					retired.writer_attachment_audit_promotion_count == 1U,
				"retired generation retains the reader handoff but only tombstone writer evidence");

		for (const auto request_page : {0, 1})
		{
			const auto successor = writer_request(binding,
												  writer_connection,
												  14,
												  3,
												  static_cast<std::uint8_t>(16 + request_page),
												  request_page,
												  1);
			auto denied = coordinator.begin_writer_map(successor);
			require(!denied &&
						denied.error().reason ==
							sqlite_shm_lease_rejection_reason::successor_handoff_live,
					request_page == 0
						? "same-page successor denied while prior handoff lives"
						: "different-page successor denied for the whole file family");
		}
		const auto unmap_callback = callback(2, 101);
		auto unmap = coordinator.begin_reader_unmap(handoff, unmap_callback);
		require(unmap.has_value() && !handoff.valid() && unmap->valid(),
				"reader handoff is hidden before delegated native unmap");
		require(coordinator
						.complete_reader_unmap(*unmap,
											   unmap_callback,
											   sqlite_shm_native_cleanup_outcome::confirmed_success)
						.has_value() &&
					!unmap->valid(),
				"successful delegated reader unmap releases handoff");
		require(coordinator.snapshot().phase == sqlite_shm_mapping_generation_phase::empty &&
					coordinator.snapshot().generation_authority_count == 0U,
				"last handoff drain permits a fresh generation");

		auto fresh_pending =
			install_pending(coordinator,
							writer_request(binding, writer_connection, 12, 3, 18, 0, 1),
							open_epoch,
							mapping(0, &first_page, 4096U),
							sqlite_shm_writer_extend_pair::one_one,
							18);
		auto fresh_holder = promote(coordinator, fresh_pending, gate);
		require(fresh_holder.generation() > generation,
				"same pointer remap receives a fresh non-reused generation");
		retire_last(coordinator, fresh_holder, callback(3, 102));
		require(coordinator.revoke_writer_eligibility(gate).has_value(),
				"revoke handoff test gate");
		(void)second_page;
	}

	void verify_reader_unmap_and_writer_retirement_race()
	{
		constexpr std::uint8_t marker = 51;
		const auto binding = family(marker);
		const auto writer_connection = identity("test.connection", marker);
		const auto reader_connection = identity("test.connection", marker + 1U);
		const auto open_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto live =
			install_live_writer(coordinator, binding, writer_connection, open_epoch, marker, &page);
		const auto reader = reader_request(binding, reader_connection, marker + 1U, 2, marker + 1U);
		auto reader_pin = coordinator.begin_reader_map(reader);
		require(reader_pin.has_value(), "reader-unmap race acquires a reader pin");
		auto handoff_result =
			coordinator.promote_reader(*reader_pin,
									   sqlite_same_process_shm_lease_test_peer::reader_map(
										   reader,
										   live.holder.generation(),
										   mapping(0, &page, 4096U),
										   identity("test.zero-reader-resize", marker)));
		require(handoff_result.has_value(), "reader-unmap race creates a handoff");
		auto handoff = std::move(*handoff_result);
		const auto release_callback = callback(1, 100);
		const auto unmap_callback = callback(2, 101);

		std::barrier admission_start{3};
		std::optional<sqlite_shm_writer_release> release;
		std::optional<sqlite_shm_reader_unmap_obligation> unmap;
		std::exception_ptr release_error;
		std::exception_ptr unmap_error;
		std::jthread release_thread{
			[&]
			{
				admission_start.arrive_and_wait();
				try
				{
					auto result = coordinator.release_writer_holder(live.holder, release_callback);
					require(result.has_value(), "writer retirement race admission succeeds");
					release.emplace(std::move(*result));
				}
				catch (...)
				{
					release_error = std::current_exception();
				}
			}};
		std::jthread unmap_thread{
			[&]
			{
				admission_start.arrive_and_wait();
				try
				{
					auto result = coordinator.begin_reader_unmap(handoff, unmap_callback);
					require(result.has_value(), "reader native-unmap race admission succeeds");
					unmap.emplace(std::move(*result));
				}
				catch (...)
				{
					unmap_error = std::current_exception();
				}
			}};
		admission_start.arrive_and_wait();
		release_thread.join();
		unmap_thread.join();
		if (release_error)
			std::rethrow_exception(release_error);
		if (unmap_error)
			std::rethrow_exception(unmap_error);
		require(release && unmap &&
					release->decision() == sqlite_shm_writer_retirement_decision::ready,
				"reader attachment cleanup never blocks exact writer retirement");

		std::barrier completion_start{3};
		bool writer_completed = false;
		bool reader_completed = false;
		std::jthread writer_completion{
			[&]
			{
				completion_start.arrive_and_wait();
				writer_completed = coordinator
									   .complete_writer_cleanup(
										   release->cleanup(),
										   release_callback,
										   sqlite_shm_native_cleanup_outcome::confirmed_success)
									   .has_value();
			}};
		std::jthread reader_completion{
			[&]
			{
				completion_start.arrive_and_wait();
				reader_completed =
					coordinator
						.complete_reader_unmap(*unmap,
											   unmap_callback,
											   sqlite_shm_native_cleanup_outcome::confirmed_success)
						.has_value();
			}};
		completion_start.arrive_and_wait();
		writer_completion.join();
		reader_completion.join();
		require(writer_completed && reader_completed &&
					coordinator.snapshot().phase == sqlite_shm_mapping_generation_phase::empty,
				"racing writer retirement and reader unmap drain one generation exactly once");
		require(coordinator.revoke_writer_eligibility(live.eligibility).has_value(),
				"revoke reader-unmap race gate");
	}

	void verify_same_thread_retirement_quarantines_without_wait()
	{
		const auto binding = family(14);
		const auto writer_connection = identity("test.connection", 14);
		const auto reader_connection = identity("test.connection", 15);
		const auto open_epoch = identity("test.open-epoch", 14);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto gate = install_eligibility(coordinator, binding, writer_connection, open_epoch, 14);
		auto pending = install_pending(coordinator,
									   writer_request(binding, writer_connection, 14, 1, 20, 0, 1),
									   open_epoch,
									   mapping(0, &page, 4096U),
									   sqlite_shm_writer_extend_pair::one_one,
									   14);
		auto holder = promote(coordinator, pending, gate);
		auto reader =
			coordinator.begin_reader_map(reader_request(binding, reader_connection, 15, 7, 21));
		require(reader.has_value(), "same-thread reader pin acquired");
		auto retirement = coordinator.release_writer_holder(holder, callback(7, 100, 1U));
		require(retirement &&
					retirement->decision() ==
						sqlite_shm_writer_retirement_decision::quarantine_same_thread &&
					coordinator.snapshot().quarantined,
				"same-thread reentrant retirement never waits and quarantines");
	}

	void verify_same_thread_writer_inflight_quarantines_without_wait()
	{
		constexpr std::uint8_t marker = 35;
		const auto binding = family(marker);
		const auto connection_a = identity("test.connection", marker);
		const auto connection_b = identity("test.connection", marker + 1U);
		const auto open_epoch = identity("test.open-epoch", marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto gate = install_eligibility(coordinator, binding, connection_a, open_epoch, marker);
		auto pending =
			install_pending(coordinator,
							writer_request(binding, connection_a, marker, 1, marker, 0, 1),
							open_epoch,
							mapping(0, &page, 4096U),
							sqlite_shm_writer_extend_pair::one_one,
							marker);
		auto holder = promote(coordinator, pending, gate);
		auto reentrant_writer = coordinator.begin_writer_map(
			writer_request(binding, connection_b, marker + 1U, 7, marker + 1U, 0, 0));
		require(reentrant_writer.has_value(), "same-thread writer pin acquired");

		const auto retirement_callback = callback(7, 100, 1U);
		auto retirement = coordinator.release_writer_holder(holder, retirement_callback);
		require(retirement &&
					retirement->decision() ==
						sqlite_shm_writer_retirement_decision::quarantine_same_thread &&
					retirement->cleanup().valid() && coordinator.snapshot().quarantined,
				"same-thread writer reentrancy never waits and quarantines");
	}

	void verify_generation_exhaustion_and_token_abandonment_fail_closed()
	{
		const auto binding = family(16);
		const auto connection = identity("test.connection", 16);
		const auto open_epoch = identity("test.open-epoch", 16);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>(
			std::numeric_limits<std::uint64_t>::max());
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int page{};
		auto gate = install_eligibility(coordinator, binding, connection, open_epoch, 16);
		auto first_pending = install_pending(coordinator,
											 writer_request(binding, connection, 16, 1, 23, 0, 1),
											 open_epoch,
											 mapping(0, &page, 4096U),
											 sqlite_shm_writer_extend_pair::one_one,
											 16);
		auto first_holder = promote(coordinator, first_pending, gate);
		require(first_holder.generation() == std::numeric_limits<std::uint64_t>::max(),
				"last available generation is valid exactly once");
		retire_last(coordinator, first_holder, callback(1, 100));

		auto exhausted_pending =
			install_pending(coordinator,
							writer_request(binding, connection, 16, 1, 25, 0, 1),
							open_epoch,
							mapping(0, &page, 4096U),
							sqlite_shm_writer_extend_pair::one_one,
							16);
		auto exhausted = coordinator.promote_writer(exhausted_pending, gate);
		require(!exhausted &&
					exhausted.error().reason ==
						sqlite_shm_lease_rejection_reason::generation_exhausted,
				"checked generation counter never wraps");
		cleanup_writer(coordinator, exhausted_pending, callback(1, 101));
		require(coordinator.revoke_writer_eligibility(gate).has_value(), "revoke exhausted gate");

		const auto abandoned_binding = family(17);
		const auto abandoned_connection = identity("test.connection", 17);
		const auto abandoned_epoch = identity("test.open-epoch", 17);
		auto ordinary_generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator abandoned{abandoned_binding,
																	ordinary_generations};
		int abandoned_page{};
		auto abandoned_gate = install_eligibility(
			abandoned, abandoned_binding, abandoned_connection, abandoned_epoch, 17);
		{
			auto abandoned_pending = install_pending(
				abandoned,
				writer_request(abandoned_binding, abandoned_connection, 17, 1, 26, 0, 1),
				abandoned_epoch,
				mapping(0, &abandoned_page, 4096U),
				sqlite_shm_writer_extend_pair::one_one,
				17);
			auto abandoned_holder = promote(abandoned, abandoned_pending, abandoned_gate);
			require(abandoned_holder.valid(), "holder exists before abandonment");
		}
		require(abandoned.snapshot().quarantined && !abandoned.snapshot().reader_admission_visible,
				"dropped holder never fabricates native cleanup or revives authority");
	}

	void verify_unpublished_cleanup_entry_and_terminal_partitions_are_closed()
	{
		{
			constexpr std::uint8_t marker = 201U;
			auto admitted = begin_mapped_validation_failure_cleanup(
				prepare_mapped_validation_failure_cleanup(marker));
			require(admitted.cleanup.valid(),
					"exact OK/non-null mapped validation failure owns cleanup");
		}

		{
			constexpr std::uint8_t marker = 204U;
			auto candidate = make_unpublished_cleanup_candidate(marker);
			const auto map_request = unpublished_cleanup_map_request(
				candidate.session_request, static_cast<std::uint8_t>(marker + 1U));
			auto inflight = candidate.fixture.registry->begin_reader_map(
				*candidate.fixture.family_pin, candidate.session, map_request);
			require(inflight && inflight->valid(), "begin protocol-invalid mapped first map");
			const auto mapped = sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
				map_request,
				candidate.writer.holder.generation(),
				mapping(0, candidate.native_page.get(), 4096U),
				identity("test.phase2.protocol-observation", marker));
			const auto receipt =
				sqlite_same_process_shm_lease_test_peer::reader_unpublished_cleanup(
					*inflight,
					sqlite_shm_reader_unpublished_cleanup_entry_kind::exact_protocol_invalid_mapped,
					map_request,
					candidate.session_request,
					candidate.writer.holder.generation(),
					sqlite_busy_status,
					candidate.native_page.get(),
					0,
					mapped.observed_attachment(),
					identity("test.phase2.protocol-mapped-effect", marker),
					identity("test.phase2.protocol-session-terminal", marker));
			auto cleanup = candidate.fixture.registry->begin_reader_unpublished_cleanup(
				*candidate.fixture.family_pin, *inflight, receipt, candidate.session);
			const auto snapshot = candidate.fixture.coordinator->snapshot();
			const auto lifecycle = sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(
				*candidate.fixture.coordinator);
			require(
				cleanup && cleanup->valid() && !inflight->valid() && !candidate.session.valid() &&
					snapshot.reader_attachment_live_member_count == 0U &&
					snapshot.reader_unpublished_cleanup_admitted_count == 1U &&
					lifecycle.live_custody_kind_counts[enum_index(
						detail::sqlite_shm_reader_custody_kind::generation_group_count)] == 0U &&
					lifecycle.live_custody_kind_counts[enum_index(
						detail::sqlite_shm_reader_custody_kind::attachment_group_handoff)] == 0U &&
					only_attachment_reservation(lifecycle).unpublished_cleanup_kind ==
						sqlite_shm_reader_unpublished_cleanup_entry_kind::
							exact_protocol_invalid_mapped &&
					!snapshot.quarantined,
				"closed protocol-invalid mapped pair did not select cleanup-only ownership");
			const auto terminal =
				sqlite_same_process_shm_lease_test_peer::reader_unpublished_cleanup_terminal(
					*cleanup,
					map_request.callback,
					sqlite_shm_reader_unpublished_cleanup_evidence_kind::exact_native_result,
					sqlite_busy_status,
					0,
					0,
					identity("test.phase2.protocol-cleanup-effect", marker),
					std::nullopt);
			auto completed = candidate.fixture.registry->complete_reader_unpublished_cleanup(
				*candidate.fixture.family_pin, *cleanup, terminal);
			require(
				completed &&
					completed->kind() ==
						sqlite_shm_reader_unpublished_cleanup_terminal_kind::terminal_quarantined &&
					completed->native_status() == sqlite_busy_status &&
					completed->outward_status() == sqlite_busy_status && !cleanup->valid() &&
					candidate.fixture.coordinator->snapshot().quarantined,
				"native non-OK cleanup did not quarantine with exact evidence");
		}

		{
			constexpr std::uint8_t marker = 207U;
			auto candidate = make_unpublished_cleanup_candidate(marker);
			const auto map_request = unpublished_cleanup_map_request(
				candidate.session_request, static_cast<std::uint8_t>(marker + 1U));
			auto inflight = candidate.fixture.registry->begin_reader_map(
				*candidate.fixture.family_pin, candidate.session, map_request);
			require(inflight && inflight->valid(), "begin rejected protocol pair map");
			const auto mapped = sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
				map_request,
				candidate.writer.holder.generation(),
				mapping(0, candidate.native_page.get(), 4096U),
				identity("test.phase2.invalid-protocol-observation", marker));
			const auto rejected_receipt =
				sqlite_same_process_shm_lease_test_peer::reader_unpublished_cleanup(
					*inflight,
					sqlite_shm_reader_unpublished_cleanup_entry_kind::exact_protocol_invalid_mapped,
					map_request,
					candidate.session_request,
					candidate.writer.holder.generation(),
					sqlite_readonly_status,
					candidate.native_page.get(),
					0,
					mapped.observed_attachment(),
					identity("test.phase2.invalid-protocol-effect", marker),
					identity("test.phase2.invalid-protocol-session-terminal", marker));
			auto rejected = candidate.fixture.registry->begin_reader_unpublished_cleanup(
				*candidate.fixture.family_pin, *inflight, rejected_receipt, candidate.session);
			require(!rejected &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						!inflight->valid() && !candidate.session.valid() &&
						candidate.fixture.coordinator->snapshot().quarantined,
					"base READONLY/non-null protocol pair entered cleanup ownership");
		}

		{
			constexpr std::uint8_t marker = 210U;
			auto admitted = begin_mapped_validation_failure_cleanup(
				prepare_mapped_validation_failure_cleanup(marker));
			const auto terminal =
				sqlite_same_process_shm_lease_test_peer::reader_unpublished_cleanup_terminal(
					admitted.cleanup,
					admitted.map_request.callback,
					sqlite_shm_reader_unpublished_cleanup_evidence_kind::throw_or_unknown,
					std::nullopt,
					0,
					0,
					std::nullopt,
					std::nullopt);
			auto completed =
				admitted.candidate.fixture.registry->complete_reader_unpublished_cleanup(
					*admitted.candidate.fixture.family_pin, admitted.cleanup, terminal);
			require(
				completed &&
					completed->kind() ==
						sqlite_shm_reader_unpublished_cleanup_terminal_kind::terminal_quarantined &&
					completed->evidence_kind() ==
						sqlite_shm_reader_unpublished_cleanup_evidence_kind::throw_or_unknown &&
					!completed->native_status() &&
					completed->outward_status() == sqlite_ioerr_status &&
					admitted.candidate.fixture.coordinator->snapshot().quarantined,
				"unknown cleanup execution did not quarantine without invented evidence");
		}

		{
			constexpr std::uint8_t marker = 213U;
			auto admitted = begin_mapped_validation_failure_cleanup(
				prepare_mapped_validation_failure_cleanup(marker));
			sqlite_same_process_shm_lease_test_peer::
				fail_next_reader_unpublished_cleanup_terminal_commit(
					*admitted.candidate.fixture.coordinator);
			const auto terminal =
				sqlite_same_process_shm_lease_test_peer::reader_unpublished_cleanup_terminal(
					admitted.cleanup,
					admitted.map_request.callback,
					sqlite_shm_reader_unpublished_cleanup_evidence_kind::exact_native_result,
					sqlite_ok_status,
					0,
					0,
					identity("test.phase2.commit-failure-effect", marker),
					identity("test.phase2.commit-failure-latch", marker));
			auto failed = admitted.candidate.fixture.registry->complete_reader_unpublished_cleanup(
				*admitted.candidate.fixture.family_pin, admitted.cleanup, terminal);
			const auto snapshot = admitted.candidate.fixture.coordinator->snapshot();
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						!admitted.cleanup.valid() && snapshot.quarantined &&
						snapshot.reader_logical_ack_awaiting_count == 0U,
					"terminal commit failure half-published a logical ack");
		}
	}

	void verify_unpublished_cleanup_logical_ack_is_exact_and_one_shot()
	{
		constexpr std::uint8_t marker = 216U;
		auto candidate = make_unpublished_cleanup_candidate(marker);
		const auto cross_pre =
			unpublished_cleanup_pre_sqlite_request(candidate.fixture, marker + 1U);
		auto cross_open = sqlite_same_process_shm_registry_test_peer::reader_open(
			*candidate.fixture.registry,
			*candidate.fixture.family_pin,
			unpublished_cleanup_open_binding(cross_pre));
		require(cross_open.has_value(), "acquire cross-bound logical-ack open before cleanup cut");
		auto setup = confirm_mapped_validation_failure_cleanup(
			prepare_mapped_validation_failure_cleanup(std::move(candidate), marker), marker);
		auto fresh = setup.candidate.pre_sqlite;
		fresh.read_transaction_epoch = identity("test.phase2.fresh-transaction", marker);
		fresh.decode_attempt = identity("test.phase2.fresh-decode", marker);
		fresh.authority_read_receipt = identity("test.phase2.fresh-authority-read", marker);
		auto blocked = setup.candidate.fixture.registry->admit_reader_session_before_sqlite(
			*setup.candidate.fixture.family_pin, setup.candidate.open, fresh);
		require(
			blocked &&
				blocked->kind() ==
					sqlite_shm_reader_session_admission_kind::rejected_before_sqlite &&
				!blocked->has_proposal_custody() && blocked->rejection() &&
				blocked->rejection()->reason == sqlite_shm_lease_rejection_reason::retiring &&
				setup.candidate.fixture.coordinator->snapshot().reader_logical_ack_awaiting_count ==
					1U,
			"awaiting logical ack admitted a fresh reader session");

		const auto cross_request =
			sqlite_shm_reader_logical_ack_request{callback(34U, marker + 1U), 0, 0};
		auto cross = setup.candidate.fixture.registry->consume_reader_logical_ack(
			*setup.candidate.fixture.family_pin, *cross_open, cross_request);
		require(
			!cross && cross.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
				cross.error().action == sqlite_shm_lease_recovery_action::outer_ioerr_no_retry &&
				setup.candidate.fixture.coordinator->snapshot().reader_logical_ack_awaiting_count ==
					1U &&
				!setup.candidate.fixture.coordinator->snapshot().quarantined,
			"cross-bound logical ack corrupted the exact pending winner");

		const auto ack_request =
			sqlite_shm_reader_logical_ack_request{callback(34U, marker + 2U), 0, 0};
		auto acknowledged = setup.candidate.fixture.registry->consume_reader_logical_ack(
			*setup.candidate.fixture.family_pin, setup.candidate.open, ack_request);
		const auto after = setup.candidate.fixture.coordinator->snapshot();
		require(acknowledged &&
					acknowledged->phase() ==
						detail::sqlite_shm_reader_logical_ack_phase::consumed_by_exact_unmap &&
					acknowledged->outward_status() == sqlite_ok_status &&
					!acknowledged->delegated_native_effect() &&
					after.reader_logical_ack_awaiting_count == 0U &&
					after.reader_registry_activity_authority_count == 0U && !after.quarantined,
				"exact logical ack delegated native work or retained its activity owner");
		auto duplicate = setup.candidate.fixture.registry->consume_reader_logical_ack(
			*setup.candidate.fixture.family_pin, setup.candidate.open, ack_request);
		require(
			!duplicate &&
				duplicate.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
				duplicate.error().action ==
					sqlite_shm_lease_recovery_action::outer_ioerr_no_retry &&
				setup.candidate.fixture.coordinator->snapshot().reader_logical_ack_awaiting_count ==
					0U &&
				!setup.candidate.fixture.coordinator->snapshot().quarantined,
			"duplicate logical ack changed the consumed winner");

		close_unmapped_registry_open(
			setup.candidate.fixture, *cross_open, static_cast<std::uint8_t>(marker + 3U));
		close_unpublished_cleanup_open(setup, static_cast<std::uint8_t>(marker + 4U));
		retire_unpublished_cleanup_writer(setup, static_cast<std::uint8_t>(marker + 5U));
	}

	void verify_close_consumes_pending_unpublished_ack_without_second_unmap()
	{
		constexpr std::uint8_t marker = 222U;
		auto setup = confirm_mapped_validation_failure_cleanup(marker);
		const auto before = sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(
			*setup.candidate.fixture.coordinator);
		const auto ack_index = enum_index(detail::sqlite_shm_reader_custody_kind::logical_ack);
		const auto unmap_index =
			enum_index(detail::sqlite_shm_reader_custody_kind::normal_or_deferred_unmap);
		const auto close_callback = callback(35U, static_cast<std::uint8_t>(marker + 2U));
		auto close = setup.candidate.fixture.registry->begin_reader_close(
			*setup.candidate.fixture.family_pin,
			setup.candidate.open,
			sqlite_shm_reader_close_request{close_callback});
		const auto admitted = sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(
			*setup.candidate.fixture.coordinator);
		const auto& reservation = only_attachment_reservation(admitted);
		require(
			close && close->valid() &&
				close->route() == sqlite_shm_reader_close_route::close_after_confirmed_unmap &&
				before.live_custody_kind_counts[ack_index] == 1U &&
				admitted.live_custody_kind_counts[ack_index] == 0U &&
				admitted.live_custody_kind_counts[unmap_index] ==
					before.live_custody_kind_counts[unmap_index] &&
				reservation.logical_ack_phase ==
					detail::sqlite_shm_reader_logical_ack_phase::consumed_by_close &&
				setup.candidate.fixture.coordinator->snapshot().reader_logical_ack_awaiting_count ==
					0U,
			"reader close did not consume the pending ack before its sole xClose");
		const auto close_receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
			*close,
			close_callback,
			sqlite_shm_reader_close_evidence_kind::exact_native_result,
			sqlite_ok_status,
			identity("test.phase2.pending-ack-close-effect", marker));
		auto completed = setup.candidate.fixture.registry->complete_reader_close(
			*setup.candidate.fixture.family_pin, setup.candidate.open, *close, close_receipt);
		require(completed && completed->kind() == sqlite_shm_reader_close_terminal_kind::closed &&
					completed->route() ==
						sqlite_shm_reader_close_route::close_after_confirmed_unmap,
				"complete pending-ack reader close");
		auto duplicate = setup.candidate.fixture.registry->begin_reader_close(
			*setup.candidate.fixture.family_pin,
			setup.candidate.open,
			sqlite_shm_reader_close_request{close_callback});
		require(
			!duplicate &&
				duplicate.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
				setup.candidate.fixture.coordinator->snapshot().reader_logical_ack_awaiting_count ==
					0U,
			"closed pending-ack route admitted a second unmap or close");
		require(
			setup.candidate.fixture.registry->release_reader_open(setup.candidate.open).has_value(),
			"release pending-ack closed open");
		retire_unpublished_cleanup_writer(setup, static_cast<std::uint8_t>(marker + 3U));
	}

	void verify_unpublished_ack_and_close_race_has_one_logical_winner()
	{
		constexpr std::uint8_t marker = 226U;
		auto setup = confirm_mapped_validation_failure_cleanup(marker);
		struct race_outcome
		{
			bool succeeded{};
			sqlite_shm_lease_rejection_reason reason{
				sqlite_shm_lease_rejection_reason::invalid_request};
			sqlite_shm_lease_recovery_action action{
				sqlite_shm_lease_recovery_action::deny_before_native_map};
		};
		race_outcome ack_outcome;
		race_outcome close_outcome;
		std::optional<sqlite_shm_reader_close_obligation> close_owner;
		const auto ack_request = sqlite_shm_reader_logical_ack_request{
			callback(36U, static_cast<std::uint8_t>(marker + 2U)), 0, 0};
		const auto close_callback = callback(37U, static_cast<std::uint8_t>(marker + 3U));
		std::barrier start{3};
		{
			std::jthread acknowledger{
				[&]
				{
					start.arrive_and_wait();
					auto result = setup.candidate.fixture.registry->consume_reader_logical_ack(
						*setup.candidate.fixture.family_pin, setup.candidate.open, ack_request);
					if (result)
						ack_outcome.succeeded = true;
					else
					{
						ack_outcome.reason = result.error().reason;
						ack_outcome.action = result.error().action;
					}
				}};
			std::jthread closer{[&]
								{
									start.arrive_and_wait();
									auto result =
										setup.candidate.fixture.registry->begin_reader_close(
											*setup.candidate.fixture.family_pin,
											setup.candidate.open,
											sqlite_shm_reader_close_request{close_callback});
									if (result)
									{
										close_outcome.succeeded = true;
										close_owner.emplace(std::move(*result));
									}
									else
									{
										close_outcome.reason = result.error().reason;
										close_outcome.action = result.error().action;
									}
								}};
			start.arrive_and_wait();
		}
		const auto lifecycle = sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(
			*setup.candidate.fixture.coordinator);
		const auto& reservation = only_attachment_reservation(lifecycle);
		const auto ack_won = reservation.logical_ack_phase ==
			detail::sqlite_shm_reader_logical_ack_phase::consumed_by_exact_unmap;
		const auto close_won = reservation.logical_ack_phase ==
			detail::sqlite_shm_reader_logical_ack_phase::consumed_by_close;
		require(
			ack_won != close_won && ack_outcome.succeeded == ack_won &&
				setup.candidate.fixture.coordinator->snapshot().reader_logical_ack_awaiting_count ==
					0U &&
				!setup.candidate.fixture.coordinator->snapshot().quarantined &&
				(!close_outcome.succeeded ||
				 (close_owner && close_owner->valid() &&
				  close_owner->route() ==
					  sqlite_shm_reader_close_route::close_after_confirmed_unmap)),
			"ack-vs-close race did not commit exactly one logical-ack winner");

		if (close_owner)
		{
			const auto receipt = sqlite_same_process_shm_lease_test_peer::reader_close_terminal(
				*close_owner,
				close_callback,
				sqlite_shm_reader_close_evidence_kind::exact_native_result,
				sqlite_ok_status,
				identity("test.phase2.race-close-effect", marker));
			auto completed = setup.candidate.fixture.registry->complete_reader_close(
				*setup.candidate.fixture.family_pin, setup.candidate.open, *close_owner, receipt);
			require(completed &&
						completed->kind() == sqlite_shm_reader_close_terminal_kind::closed &&
						!close_owner->valid(),
					"complete ack-vs-close race close winner");
			require(setup.candidate.fixture.registry->release_reader_open(setup.candidate.open)
						.has_value(),
					"release ack-vs-close race open");
		}
		else
			close_unpublished_cleanup_open(setup, static_cast<std::uint8_t>(marker + 4U));
		retire_unpublished_cleanup_writer(setup, static_cast<std::uint8_t>(marker + 5U));
	}

	void verify_unpublished_cleanup_compaction_and_replay_matrix_is_fail_closed()
	{
		constexpr std::uint8_t marker = 230U;
		auto setup = confirm_mapped_validation_failure_cleanup(marker);
		const auto attachment = setup.candidate.session_request.attachment;
		const auto ack_request = sqlite_shm_reader_logical_ack_request{
			callback(38U, static_cast<std::uint8_t>(marker + 2U)), 0, 0};
		auto ack = setup.candidate.fixture.registry->consume_reader_logical_ack(
			*setup.candidate.fixture.family_pin, setup.candidate.open, ack_request);
		require(ack && ack->outward_status() == sqlite_ok_status && !ack->delegated_native_effect(),
				"consume exact ack before compact replay test");
		close_unpublished_cleanup_open(setup, static_cast<std::uint8_t>(marker + 3U));
		retire_unpublished_cleanup_writer(setup, static_cast<std::uint8_t>(marker + 4U));
		auto compact = sqlite_same_process_shm_lease_test_peer::export_reader_lifecycle_tombstones(
			*setup.candidate.fixture.coordinator);
		auto close_compact =
			sqlite_same_process_shm_lease_test_peer::export_reader_open_close_tombstones(
				*setup.candidate.fixture.coordinator);
		require(compact && compact->size() == 1U && close_compact && close_compact->size() == 1U &&
					compact->front().phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::
							unpublished_cleanup_confirmed &&
					compact->front().logical_ack_phase ==
						detail::sqlite_shm_reader_logical_ack_phase::consumed_by_exact_unmap &&
					compact->front().logical_ack_sequence >
						compact->front().unpublished_cleanup_terminal_sequence &&
					compact->front().unpublished_cleanup_cut_sequence ==
						compact->front().unpublished_cleanup_session_terminal_sequence + 1U &&
					compact->front().unpublished_cleanup_terminal_sequence ==
						compact->front().destination_sequence &&
					compact->front().replay_identities.callback_invocation_tokens ==
						std::vector{setup.map_request.callback.invocation_token,
									ack_request.callback.invocation_token} &&
					compact->front().replay_identities.effect_receipts ==
						std::vector{setup.mapped_effect_receipt,
									setup.cleanup_effect_receipt,
									setup.latch_reset_receipt} &&
					compact->front().replay_identities.session_terminal_receipts ==
						std::vector{setup.session_no_pointer_terminal_receipt},
				"compact tombstone lost cleanup, ack, or T_session lineage");

		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		auto sequences = std::make_shared<sqlite_shm_reader_lifecycle_sequence_source>();
		sqlite_same_process_shm_mapping_lease_coordinator sequence_driver{
			setup.candidate.fixture.family, generations, sequences};
		const auto imported_high_water = std::max(compact->front().logical_ack_sequence,
												  close_compact->front().terminal_sequence);
		for (std::uint64_t index = 0U;
			 sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(sequence_driver)
				 .last_issued_sequence < imported_high_water;
			 ++index)
		{
			const auto driver_marker = static_cast<std::uint8_t>(240U + index);
			auto driver_open = register_reader_open(
				sequence_driver,
				1201U + index,
				reader_open_epoch_binding(setup.candidate.fixture.family, driver_marker));
			close_and_release_registered_reader_open(sequence_driver, driver_open, driver_marker);
		}
		sqlite_same_process_shm_mapping_lease_coordinator successor{
			setup.candidate.fixture.family, generations, sequences};
		require(sqlite_same_process_shm_lease_test_peer::import_reader_open_close_tombstones(
					successor, std::span{*close_compact})
					.has_value(),
				"import exact unpublished-cleanup close tombstone");
		require(sqlite_same_process_shm_lease_test_peer::import_reader_lifecycle_tombstones(
					successor, std::span{*compact})
					.has_value(),
				"import exact unpublished-cleanup tombstone");
		auto imported =
			sqlite_same_process_shm_lease_test_peer::export_reader_lifecycle_tombstones(successor);
		auto stale = sqlite_same_process_shm_lease_test_peer::check_reader_lifecycle_tombstone(
			successor, attachment);
		require(imported && *imported == *compact && !stale &&
					stale.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
				"compact replay revived the exact attachment");

		auto bad_cut_sequence = *compact;
		++bad_cut_sequence.front().unpublished_cleanup_cut_sequence;
		auto rejected_cut =
			sqlite_same_process_shm_lease_test_peer::import_reader_lifecycle_tombstones(
				successor, std::span{bad_cut_sequence});
		auto bad_ack_sequence = *compact;
		bad_ack_sequence.front().logical_ack_sequence =
			bad_ack_sequence.front().unpublished_cleanup_terminal_sequence;
		auto rejected_ack =
			sqlite_same_process_shm_lease_test_peer::import_reader_lifecycle_tombstones(
				successor, std::span{bad_ack_sequence});
		auto cross_domain_session = *compact;
		cross_domain_session.front().replay_identities.session_terminal_receipts.front() =
			cross_domain_session.front().replay_identities.effect_receipts.front();
		auto rejected_session =
			sqlite_same_process_shm_lease_test_peer::import_reader_lifecycle_tombstones(
				successor, std::span{cross_domain_session});
		auto cross_domain_callback = *compact;
		cross_domain_callback.front().replay_identities.callback_invocation_tokens.front() =
			cross_domain_callback.front().replay_identities.session_terminal_receipts.front();
		auto rejected_callback =
			sqlite_same_process_shm_lease_test_peer::import_reader_lifecycle_tombstones(
				successor, std::span{cross_domain_callback});
		auto bad_phase = *compact;
		bad_phase.front().logical_ack_phase =
			detail::sqlite_shm_reader_logical_ack_phase::awaiting_sqlite_ack;
		auto rejected_phase =
			sqlite_same_process_shm_lease_test_peer::import_reader_lifecycle_tombstones(
				successor, std::span{bad_phase});
		auto winner_after =
			sqlite_same_process_shm_lease_test_peer::export_reader_lifecycle_tombstones(successor);
		require(
			!rejected_cut &&
				rejected_cut.error().reason == sqlite_shm_lease_rejection_reason::invalid_request &&
				!rejected_ack &&
				rejected_ack.error().reason == sqlite_shm_lease_rejection_reason::invalid_request &&
				!rejected_session &&
				rejected_session.error().reason ==
					sqlite_shm_lease_rejection_reason::invalid_request &&
				!rejected_callback &&
				rejected_callback.error().reason ==
					sqlite_shm_lease_rejection_reason::invalid_request &&
				!rejected_phase &&
				rejected_phase.error().reason ==
					sqlite_shm_lease_rejection_reason::invalid_request &&
				winner_after && *winner_after == *compact,
			"invalid compact sequence/domain matrix changed the imported winner");
	}

	void verify_unpublished_cleanup_determinate_rejection_differs_from_true_ambiguity()
	{
		{
			auto exact = prepare_mapped_validation_failure_cleanup(234U);
			require(exact.inflight.valid() && exact.candidate.session.valid() &&
						!exact.candidate.fixture.coordinator->snapshot().quarantined,
					"determinate prepublication failure lost its recoverable cleanup owner");
		}

		{
			constexpr std::uint8_t marker = 237U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int native_page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &native_page);
			const auto map_request = reader_attachment_request(
				binding,
				identity("test.connection", static_cast<std::uint8_t>(marker + 1U)),
				static_cast<std::uint8_t>(marker + 1U),
				30U,
				static_cast<std::uint8_t>(marker + 1U),
				0,
				writer.holder.generation());
			const auto session_request =
				reader_session_request(map_request, static_cast<std::uint8_t>(marker + 1U));
			auto session = coordinator.begin_reader_session(session_request);
			require(session && session->valid(),
					"reserve truly ambiguous direct first-map session");
			auto inflight = coordinator.begin_reader_map(*session, map_request);
			require(inflight && inflight->valid(), "begin truly ambiguous first map");
			const auto mapped = sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
				map_request,
				writer.holder.generation(),
				mapping(0, &native_page, 4096U),
				identity("test.phase2.ambiguous-mapped-effect", marker));
			sqlite_same_process_shm_lease_test_peer::fail_next_reader_map_terminal_commit(
				coordinator);
			sqlite_same_process_shm_lease_test_peer::fail_next_reader_recovery_mutex_reacquire(
				coordinator);
			auto failed = coordinator.commit_reader_map(*inflight, mapped, *session);
			const auto snapshot = coordinator.snapshot();
			require(!failed &&
						failed.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						snapshot.quarantined &&
						snapshot.reader_unpublished_cleanup_admitted_count == 0U &&
						snapshot.reader_logical_ack_awaiting_count == 0U,
					"true recovery ambiguity was misclassified as deterministic cleanup lineage");
		}
	}

	void verify_direct_opaque_first_map_has_no_group_or_native_authority()
	{
		constexpr std::uint8_t marker = 239U;
		const auto binding = family(marker);
		auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
		sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
		int native_page{};
		auto writer = install_live_writer(coordinator,
										  binding,
										  identity("test.connection", marker),
										  identity("test.open-epoch", marker),
										  marker,
										  &native_page);
		const auto map_request =
			reader_attachment_request(binding,
									  identity("test.opaque-reader-connection", marker),
									  marker + 1U,
									  30U,
									  marker + 2U,
									  0,
									  writer.holder.generation());
		const auto session_request = reader_session_request(map_request, marker + 3U);
		auto session = coordinator.begin_reader_session(session_request);
		require(session && session->valid(), "reserve direct opaque first-map session");
		auto inflight = coordinator.begin_reader_map(*session, map_request);
		require(inflight && inflight->valid(), "begin direct opaque first map");
		auto opaque =
			coordinator.complete_reader_opaque_attachment_uncertainty(*inflight, *session);
		const auto snapshot = coordinator.snapshot();
		const auto lifecycle =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
		const auto durable_custody_index = static_cast<std::size_t>(
			detail::sqlite_shm_reader_custody_state::transferred_to_durable_tombstone);
		require(
			opaque && opaque->outward_status() == sqlite_ioerr_status &&
				opaque->native_mapping() == nullptr && !inflight->valid() && !session->valid() &&
				snapshot.quarantined && snapshot.reader_attachment_group_count == 0U &&
				snapshot.reader_opaque_attachment_uncertainty_count == 1U &&
				snapshot.reader_registry_activity_authority_count == 0U &&
				lifecycle.attachment_groups.empty() && lifecycle.map_attempts.empty() &&
				lifecycle.opaque_attachment_uncertainties.size() == 1U &&
				!lifecycle.opaque_attachment_uncertainties.front().predelegate_lifetime_retained &&
				!lifecycle.opaque_attachment_uncertainties.front().candidate_lifetime_retained &&
				lifecycle.outstanding_terminal_permit_count == 0U &&
				lifecycle.custody_state_counts[durable_custody_index] == 3U &&
				all_reader_live_custody_released(lifecycle),
			"direct opaque terminal retains only durable no-group custody and exposes no native "
			"authority");
		auto replay =
			coordinator.complete_reader_opaque_attachment_uncertainty(*inflight, *session);
		require(!replay &&
					replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
					coordinator.snapshot().reader_opaque_attachment_uncertainty_count == 1U,
				"opaque terminal replay cannot recreate custody or authority");
	}

	void verify_major_token_abandonment_quarantines()
	{
		{
			constexpr std::uint8_t marker = 40;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			{
				auto begun = coordinator.begin_writer_map(
					writer_request(binding, connection, marker, 1, marker, 0, 1));
				require(begun.has_value(), "writer in-flight token created");
				auto inflight = std::move(*begun);
				require(inflight.valid(), "writer in-flight token valid before abandonment");
			}
			require(coordinator.snapshot().quarantined,
					"dropped writer in-flight token quarantines");
		}

		{
			constexpr std::uint8_t marker = 48;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			{
				auto begun = coordinator.begin_writer_map(
					writer_request(binding, connection, marker, 1, marker, 0, 1));
				require(begun.has_value(), "writer native-map abandonment fixture begins");
				auto inflight = std::move(*begun);
				auto post_native = record_native_mapping(coordinator, inflight, &page);
				require(post_native.valid(),
						"post-native cleanup-only token valid before abandonment");
			}
			require(coordinator.snapshot().quarantined &&
						coordinator.snapshot().writer_cleanup_count == 1U,
					"dropped post-native token quarantines without fabricating cleanup");
		}

		{
			constexpr std::uint8_t marker = 41;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			{
				auto pending =
					install_pending(coordinator,
									writer_request(binding, connection, marker, 1, marker, 0, 1),
									open_epoch,
									mapping(0, &page, 4096U),
									sqlite_shm_writer_extend_pair::one_one,
									marker);
				require(pending.valid(), "pending token valid before abandonment");
			}
			require(coordinator.snapshot().quarantined,
					"dropped pending writer mapping quarantines");
		}

		{
			constexpr std::uint8_t marker = 42;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto pending =
				install_pending(coordinator,
								writer_request(binding, connection, marker, 1, marker, 0, 1),
								open_epoch,
								mapping(0, &page, 4096U),
								sqlite_shm_writer_extend_pair::one_one,
								marker);
			{
				auto begun = coordinator.begin_writer_cleanup(pending, callback(1, 100));
				require(begun.has_value() && !pending.valid(), "writer cleanup obligation created");
				auto cleanup = std::move(*begun);
				require(cleanup.valid(), "writer cleanup valid before abandonment");
			}
			require(coordinator.snapshot().quarantined,
					"dropped writer cleanup obligation quarantines");
		}

		{
			constexpr std::uint8_t marker = 43;
			const auto binding = family(marker);
			const auto writer_connection = identity("test.connection", marker);
			const auto reader_connection = identity("test.connection", marker + 40U);
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(
				coordinator, binding, writer_connection, open_epoch, marker, &page);
			{
				auto begun = coordinator.begin_reader_map(
					reader_request(binding, reader_connection, marker + 40U, 2, marker + 40U));
				require(begun.has_value(), "reader in-flight token created");
				auto inflight = std::move(*begun);
				require(inflight.valid(), "reader in-flight valid before abandonment");
			}
			require(coordinator.snapshot().quarantined,
					"dropped reader in-flight token quarantines");
		}

		{
			constexpr std::uint8_t marker = 44;
			const auto binding = family(marker);
			const auto writer_connection = identity("test.connection", marker);
			const auto reader_connection = identity("test.connection", marker + 40U);
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			int mismatch{};
			auto writer = install_live_writer(
				coordinator, binding, writer_connection, open_epoch, marker, &page);
			const auto request =
				reader_request(binding, reader_connection, marker + 40U, 2, marker + 40U);
			auto begun = coordinator.begin_reader_map(request);
			require(begun.has_value(), "reader cleanup source pin created");
			auto inflight = std::move(*begun);
			auto rejected =
				coordinator.promote_reader(inflight,
										   sqlite_same_process_shm_lease_test_peer::reader_map(
											   request,
											   writer.holder.generation(),
											   mapping(0, &mismatch, 4096U),
											   identity("test.zero-reader-resize", marker)));
			require(!rejected, "reader mismatch creates cleanup requirement");
			{
				auto cleanup_result = coordinator.begin_reader_cleanup(inflight, request.callback);
				require(cleanup_result.has_value() && !inflight.valid(),
						"reader cleanup obligation created");
				auto cleanup = std::move(*cleanup_result);
				require(cleanup.valid(), "reader cleanup valid before abandonment");
			}
			require(coordinator.snapshot().quarantined,
					"dropped reader cleanup obligation quarantines");
		}

		{
			constexpr std::uint8_t marker = 45;
			const auto binding = family(marker);
			const auto writer_connection = identity("test.connection", marker);
			const auto reader_connection = identity("test.connection", marker + 40U);
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(
				coordinator, binding, writer_connection, open_epoch, marker, &page);
			const auto request =
				reader_request(binding, reader_connection, marker + 40U, 2, marker + 40U);
			auto begun = coordinator.begin_reader_map(request);
			require(begun.has_value(), "reader handoff source pin created");
			auto inflight = std::move(*begun);
			{
				auto promoted =
					coordinator.promote_reader(inflight,
											   sqlite_same_process_shm_lease_test_peer::reader_map(
												   request,
												   writer.holder.generation(),
												   mapping(0, &page, 4096U),
												   identity("test.zero-reader-resize", marker)));
				require(promoted.has_value(), "reader handoff created");
				auto handoff = std::move(*promoted);
				require(handoff.valid(), "reader handoff valid before abandonment");
			}
			require(coordinator.snapshot().quarantined, "dropped reader handoff quarantines");
		}

		{
			constexpr std::uint8_t marker = 49U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto map_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 1U),
										  marker + 1U,
										  2,
										  marker + 1U,
										  0,
										  writer.holder.generation());
			const auto session_request = reader_session_request(map_request, marker + 1U);
			std::uint64_t session_token{};
			{
				auto session = coordinator.begin_reader_session(session_request);
				require(session && session->valid(),
						"activated reader session valid before abandonment");
				const auto before =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				session_token = only_session_reservation(before).session_token;
				require(before.outstanding_terminal_permit_count == 1U,
						"activated session abandonment starts with one exact permit");
			}
			const auto abandoned =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto* quarantine = find_reader_terminal_quarantine(abandoned, session_token);
			require(
				coordinator.snapshot().quarantined &&
					abandoned.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(abandoned) &&
					abandoned.last_issued_sequence == abandoned.last_committed_sequence &&
					reader_event_sequences_are_dense(abandoned) &&
					all_reader_live_custody_released(abandoned) &&
					abandoned.attachment_reservations.size() == 1U &&
					abandoned.attachment_reservations.front().phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::
							terminal_quarantined &&
					abandoned.session_reservations.size() == 1U &&
					abandoned.session_reservations.front().phase ==
						detail::sqlite_shm_reader_session_reservation_phase::terminal_quarantined &&
					abandoned.session_reservations.front().destination_sequence ==
						abandoned.attachment_reservations.front().destination_sequence &&
					quarantine != nullptr &&
					quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::owner_abandoned &&
					quarantine->terminal_sequence ==
						abandoned.session_reservations.front().destination_sequence &&
					!quarantine->callback && !quarantine->native_effect_receipt &&
					!quarantine->exact_terminal_receipt_retained,
				"activated session abandonment consumes its permit and custody into an exact "
				"owner-abandoned tombstone");
		}

		{
			constexpr std::uint8_t marker = 50U;
			const auto binding = family(marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer = install_live_writer(coordinator,
											  binding,
											  identity("test.connection", marker),
											  identity("test.open-epoch", marker),
											  marker,
											  &page);
			const auto map_request =
				reader_attachment_request(binding,
										  identity("test.connection", marker + 1U),
										  marker + 1U,
										  2,
										  marker + 1U,
										  0,
										  writer.holder.generation());
			const auto session_request = reader_session_request(map_request, marker + 1U);
			auto session = coordinator.begin_reader_session(session_request);
			require(session && session->valid(),
					"activated map-abandonment fixture reserves its session");
			std::uint64_t map_token{};
			{
				auto map = coordinator.begin_reader_map(*session, map_request);
				require(map && map->valid(), "activated attachment map valid before abandonment");
				const auto before =
					sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
				map_token = only_reader_map_attempt(before).map_token;
				require(before.outstanding_terminal_permit_count == 4U,
						"activated map abandonment starts with all four exact permits");
			}
			const auto abandoned =
				sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(coordinator);
			const auto* quarantine = find_reader_terminal_quarantine(abandoned, map_token);
			require(
				!session->valid() && coordinator.snapshot().quarantined &&
					abandoned.outstanding_terminal_permit_count == 0U &&
					reader_terminal_permit_slots_are_exact(abandoned) &&
					abandoned.last_issued_sequence == abandoned.last_committed_sequence &&
					reader_event_sequences_are_dense(abandoned) &&
					all_reader_live_custody_released(abandoned) && abandoned.map_attempts.empty() &&
					abandoned.attachment_reservations.size() == 1U &&
					abandoned.attachment_reservations.front().phase ==
						detail::sqlite_shm_reader_attachment_reservation_phase::
							terminal_quarantined &&
					abandoned.session_reservations.size() == 1U &&
					abandoned.session_reservations.front().phase ==
						detail::sqlite_shm_reader_session_reservation_phase::terminal_quarantined &&
					quarantine != nullptr &&
					quarantine->reason ==
						detail::sqlite_shm_reader_terminal_quarantine_reason::owner_abandoned &&
					quarantine->terminal_sequence != 0U && !quarantine->callback &&
					!quarantine->native_effect_receipt &&
					!quarantine->exact_terminal_receipt_retained,
				"activated map abandonment consumes map/session/potential-group permits and "
				"custody into an exact owner-abandoned tombstone");
		}

		{
			constexpr std::uint8_t marker = 46;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			int page{};
			auto writer =
				install_live_writer(coordinator, binding, connection, open_epoch, marker, &page);
			{
				auto release = coordinator.release_writer_holder(writer.holder, callback(1, 100));
				require(release && release->cleanup().valid(),
						"writer release owns cleanup before abandonment");
			}
			require(coordinator.snapshot().quarantined,
					"dropped writer release cleanup quarantines");
		}

		{
			constexpr std::uint8_t marker = 47;
			const auto binding = family(marker);
			const auto connection = identity("test.connection", marker);
			const auto open_epoch = identity("test.open-epoch", marker);
			auto generations = std::make_shared<sqlite_shm_mapping_generation_source>();
			sqlite_same_process_shm_mapping_lease_coordinator coordinator{binding, generations};
			{
				auto eligibility =
					install_eligibility(coordinator, binding, connection, open_epoch, marker);
				require(eligibility.valid(), "eligibility valid before scoped revocation");
			}
			const auto snapshot = coordinator.snapshot();
			require(!snapshot.quarantined && snapshot.eligibility_count == 0U,
					"dropped eligibility only revokes non-mapping authority");
		}
	}
} // namespace

int main()
{
	try
	{
		verify_production_writer_eligibility_factory_is_exact();
		verify_reader_lifecycle_vocabulary_is_closed();
		verify_extend_pair_classifier();
		verify_native_attachment_identity_and_census_groundwork();
		verify_writer_attachment_group_cleanup_is_exact_and_one_shot();
		verify_writer_attachment_gate_boundary_is_exact();
		verify_writer_attachment_pre_owner_failure_is_terminal();
		verify_nonowner_attachment_wait_is_terminal_without_retry();
		verify_writer_attachment_completion_failure_is_one_shot();
		verify_post_native_writer_receipt_requires_exact_cleanup();
		verify_writer_native_map_receipt_validator_is_closed();
		verify_native_writer_receipt_binding_and_replay();
		verify_cleanup_completion_requires_exact_callback();
		verify_failed_cleanup_admission_is_terminal_without_retry();
		verify_family_quarantine_preserves_unattempted_mandatory_drains();
		verify_reader_cleanup_failures_quarantine();
		verify_reader_unmap_failure_retains_terminal_handoff();
		verify_pending_and_eligibility_are_not_reader_authority();
		verify_map_before_gate_and_gate_before_map();
		verify_cross_alias_mixed_pair_join_in_both_directions();
		verify_simultaneous_first_writer_total_order_and_mismatch();
		verify_last_release_and_writer_admission_race();
		verify_concurrent_holder_release_orders_cleanup();
		verify_writer_inflight_blocks_last_holder_retirement();
		verify_waiting_retirement_rejects_early_native_completion();
		verify_retirement_wait_failure_quarantines_without_retry();
		verify_quarantined_retirement_drains_without_revival();
		verify_reader_reservation_terminal_requires_fresh_attachment_epoch();
		verify_reader_terminal_kind_is_closed_and_fail_closed();
		verify_reader_predecessor_first_map_transfers_without_proposal_authority();
		verify_reader_predecessor_receipt_partition_is_fail_closed();
		verify_reader_predecessor_unmap_terminal_partition_is_fail_closed();
		verify_reader_zero_attachment_status_partition();
		verify_reader_zero_attachment_receipt_binding_and_effect_proof();
		verify_cross_family_reader_terminal_receipts_are_owner_bound();
		verify_reused_mapped_and_session_terminal_identities_fail_closed();
		verify_reader_zero_attachment_later_group_preserves_authority();
		verify_reader_zero_attachment_terminal_commit_exception_has_no_half_publish();
		verify_reserved_reader_session_terminal_commit_exception_is_exact_and_one_shot();
		verify_reader_map_terminal_commit_exception_is_exact_and_one_shot();
		verify_session_terminal_rejects_a_native_started_map_after_receipt_retention();
		verify_reader_session_terminal_commit_exception_is_exact_and_one_shot();
		verify_reader_recovery_mutex_reacquire_failure_preserves_exact_wrappers();
		verify_reader_terminal_callback_invocations_cannot_authorize_writer_release();
		verify_reader_effect_identities_are_nonreusable_across_map_and_unmap_roles();
		verify_writer_terminal_identities_cannot_authorize_reader_work();
		verify_remaining_reader_map_terminal_commit_injection_matrix();
		verify_unpublished_cleanup_entry_and_terminal_partitions_are_closed();
		verify_unpublished_cleanup_logical_ack_is_exact_and_one_shot();
		verify_close_consumes_pending_unpublished_ack_without_second_unmap();
		verify_unpublished_ack_and_close_race_has_one_logical_winner();
		verify_unpublished_cleanup_compaction_and_replay_matrix_is_fail_closed();
		verify_unpublished_cleanup_determinate_rejection_differs_from_true_ambiguity();
		verify_direct_opaque_first_map_has_no_group_or_native_authority();
		verify_reader_session_execution_is_validated_and_exactly_bound();
		verify_reader_native_attachment_group_and_session_core();
		verify_reader_unmap_cut_wait_policy_is_fail_closed();
		verify_reader_unmap_cut_drains_preexisting_zero_attachment_map();
		verify_reader_unmap_cut_suppresses_preexisting_mapped_result();
		verify_reader_unmap_cut_quarantines_ambiguous_preexisting_map();
		verify_equal_pointer_reader_connections_unmap_independently();
		verify_callback_free_cached_member_use_requires_a_live_session_owner();
		verify_reader_lifecycle_sequence_source_is_shared_and_exhausts_without_partials();
		verify_first_reader_map_reserves_sequence_and_three_terminals_atomically();
		verify_unavailable_shared_reader_sequence_source_preserves_owned_drains();
		verify_reader_group_handoff_abandonment_consumes_joined_terminal_capacity();
		verify_exact_reader_unmap_terminal_receipts_are_closed_and_one_shot();
		verify_reader_open_epoch_close_routes_are_exact_and_one_shot();
		verify_reader_close_authentication_and_cross_owner_are_fail_closed();
		verify_reader_close_terminal_failures_are_fail_closed();
		verify_reader_close_invalid_receipts_abandonment_and_lock_faults();
		verify_reader_close_open_sequence_and_import_exhaustion_are_atomic();
		verify_compact_reader_close_tombstones_preserve_replay_identities();
		verify_compact_reader_import_replay_census_is_atomic();
		verify_invalid_top_level_close_completion_terminalizes_local_owner();
		verify_guarded_reader_open_registration_has_one_atomic_publication_cut();
		verify_reader_lifecycle_import_rejects_live_open_token_or_binding_collisions();
		verify_attachment_tombstone_must_precede_its_exact_reader_close_cut();
		verify_different_token_same_binding_compact_group_blocks_reader_close_begin();
		verify_reader_handoff_outlives_writer_and_blocks_successors();
		verify_reader_unmap_and_writer_retirement_race();
		verify_same_thread_retirement_quarantines_without_wait();
		verify_same_thread_writer_inflight_quarantines_without_wait();
		verify_generation_exhaustion_and_token_abandonment_fail_closed();
		verify_major_token_abandonment_quarantines();
	}
	catch (const std::exception& exception)
	{
		std::cerr << "FAIL: " << exception.what() << '\n';
		return 1;
	}
	return 0;
}
