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
#include <type_traits>
#include <utility>

#include <barrier>

#include "sdk/sqlite_same_process_shm_mapping_lease_internal.hpp"

namespace cxxlens::sdk
{
	class sqlite_same_process_shm_lease_test_peer
	{
	  public:
		[[nodiscard]] static sqlite_shm_verified_writer_native_map_receipt
		writer_native_map(const sqlite_shm_writer_map_inflight& inflight,
						  const volatile void* native_mapping)
		{
			return {inflight, native_mapping};
		}

		static void fail_next_writer_native_transition(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_writer_native_transition_failure_for_testing();
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
	};
} // namespace cxxlens::sdk

namespace
{
	using namespace cxxlens::sdk;

	static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_eligibility>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_map_inflight>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_post_native_mapping>);
	static_assert(!std::is_default_constructible_v<sqlite_shm_verified_writer_native_map_receipt>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_pending_mapping>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_cleanup_obligation>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_holder>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_reader_map_inflight>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_reader_cleanup_obligation>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_reader_handoff>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_reader_unmap_obligation>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_release>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_writer_eligibility>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_writer_map_inflight>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_writer_post_native_mapping>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_pending_mapping>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_writer_cleanup_obligation>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_writer_holder>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_reader_map_inflight>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_reader_cleanup_obligation>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_reader_handoff>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_reader_unmap_obligation>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_writer_release>);
	static_assert(std::is_nothrow_destructible_v<sqlite_shm_writer_holder>);
	static_assert(std::is_nothrow_destructible_v<sqlite_shm_reader_handoff>);

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error{std::string{message}};
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

	[[nodiscard]] sqlite_shm_writer_map_request
	writer_request(const sqlite_shm_lease_family_binding& binding,
				   const sqlite_backend_opaque_identity& connection,
				   const std::uint8_t alias,
				   const std::uint8_t thread,
				   const std::uint8_t invocation,
				   const int page,
				   const int extend)
	{
		return {binding,
				identity("test.alias-lifetime", alias),
				connection,
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
		const auto receipt =
			sqlite_same_process_shm_lease_test_peer::writer_native_map(inflight, native_mapping);
		auto recorded = coordinator.record_writer_native_mapping(inflight, receipt);
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

		const auto request = writer_request(binding, connection, 30, 1, 30, 0, 1);
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
				sqlite_same_process_shm_lease_test_peer::writer_native_map(inflight, nullptr);
			auto rejected = coordinator.record_writer_native_mapping(inflight, invalid);
			require(!rejected && inflight.valid() && coordinator.snapshot().quarantined &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch,
					"null native-map receipt creates no authority but retains the source attempt");
			require(!coordinator.resolve_writer_map_failure(inflight),
					"invalid post-native receipt cannot restore the no-map transition");
			const auto exact =
				sqlite_same_process_shm_lease_test_peer::writer_native_map(inflight, &page);
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
			const auto receipt =
				sqlite_same_process_shm_lease_test_peer::writer_native_map(inflight, &page);
			auto recorded = coordinator.record_writer_native_mapping(inflight, receipt);
			require(recorded.has_value() && !inflight.valid(),
					"exact native-map receipt creates one cleanup-only token");
			auto post_native = std::move(*recorded);
			auto duplicate = coordinator.record_writer_native_mapping(inflight, receipt);
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
			const auto source_receipt = sqlite_same_process_shm_lease_test_peer::writer_native_map(
				source_inflight, &source_page);
			const auto target_receipt = sqlite_same_process_shm_lease_test_peer::writer_native_map(
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
			const auto receipt =
				sqlite_same_process_shm_lease_test_peer::writer_native_map(inflight, &page);
			sqlite_same_process_shm_lease_test_peer::fail_next_writer_native_transition(
				coordinator);
			auto failed = coordinator.record_writer_native_mapping(inflight, receipt);
			require(!failed && inflight.valid() && coordinator.snapshot().quarantined,
					"injected native transition failure retains the exact source token");
			auto unsafe_resolution = coordinator.resolve_writer_map_failure(inflight);
			require(!unsafe_resolution &&
						unsafe_resolution.error().action ==
							sqlite_shm_lease_recovery_action::quarantine_no_retry,
					"transition failure cannot erase a native mapping as a no-map result");
			auto recovered = coordinator.record_writer_native_mapping(inflight, receipt);
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
			const auto request = reader_request(binding, reader_connection, marker + 1U, 2, marker);
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
			const auto request = reader_request(binding, reader_connection, marker + 1U, 2, marker);
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
							sqlite_shm_writer_retirement_decision::not_last_holder,
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
						coordinator.snapshot().writer_holder_count == 2U,
					"distinct aliases and mixed holder receipts join one generation");

			auto first_release = coordinator.release_writer_holder(first_holder, callback(8, 100));
			require(first_release.has_value() &&
						first_release->decision() ==
							sqlite_shm_writer_retirement_decision::not_last_holder,
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
						sqlite_shm_writer_retirement_decision::not_last_holder,
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
			release_a->decision() == sqlite_shm_writer_retirement_decision::not_last_holder
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
					retired.reader_handoff_count == 1U && retired.generation_authority_count == 1U,
				"retired generation retains reader handoff and writer mapping authority");

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
		verify_extend_pair_classifier();
		verify_post_native_writer_receipt_requires_exact_cleanup();
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
