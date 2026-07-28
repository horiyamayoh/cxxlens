#include "sqlite_writer_shm_mapping_semantics_internal.hpp"

#include <limits>
#include <optional>
#include <utility>

namespace cxxlens::sdk
{
	namespace
	{
		[[nodiscard]] bool valid_identity(const sqlite_backend_opaque_identity& identity) noexcept
		{
			return !identity.profile.empty() && !identity.bytes.empty();
		}

		[[nodiscard]] bool valid_family(const sqlite_shm_lease_family_binding& family) noexcept
		{
			return valid_identity(family.process_instance) &&
				valid_identity(family.shared_runtime_vfs_cohort) &&
				valid_identity(family.exact_file_family);
		}

		[[nodiscard]] bool
		valid_native_attachment(const sqlite_shm_native_attachment_identity& attachment) noexcept
		{
			return valid_family(attachment.family()) &&
				valid_identity(attachment.alias_lifetime()) &&
				valid_identity(attachment.connection_token()) &&
				valid_identity(attachment.main_native_file_receipt()) &&
				valid_identity(attachment.main_xopen_receipt()) &&
				valid_identity(attachment.open_epoch()) &&
				valid_identity(attachment.callback_cohort()) &&
				valid_identity(attachment.attachment_epoch());
		}

		[[nodiscard]] bool
		valid_callback(const sqlite_shm_callback_execution_receipt& callback) noexcept
		{
			return valid_identity(callback.thread_identity) &&
				valid_identity(callback.invocation_token);
		}

		[[nodiscard]] bool
		valid_writer_request(const sqlite_shm_writer_map_request& request) noexcept
		{
			return valid_family(request.family) && valid_identity(request.alias_lifetime) &&
				valid_identity(request.connection_token) &&
				valid_native_attachment(request.attachment) &&
				request.attachment.family() == request.family &&
				request.attachment.alias_lifetime() == request.alias_lifetime &&
				request.attachment.connection_token() == request.connection_token &&
				valid_callback(request.callback) && request.page_number >= 0 &&
				request.page_size > 0 && (request.caller_extend == 0 || request.caller_extend == 1);
		}

		[[nodiscard]] bool valid_stat_census(const sqlite_writer_shm_stat_census& census) noexcept
		{
			if (!valid_identity(census.parent_namespace_identity) ||
				!valid_identity(census.filesystem_profile) ||
				!valid_identity(census.mount_identity))
				return false;

			switch (census.state)
			{
				case sqlite_writer_shm_entry_state::absent:
					return !census.object_identity && !census.directory_entry_identity &&
						census.byte_count == 0U;
				case sqlite_writer_shm_entry_state::direct_regular:
					return census.object_identity && valid_identity(*census.object_identity) &&
						census.directory_entry_identity &&
						valid_identity(*census.directory_entry_identity);
			}
			return false;
		}

		[[nodiscard]] bool stat_context_matches(const sqlite_writer_shm_stat_census& pre,
												const sqlite_writer_shm_stat_census& post) noexcept
		{
			return pre.parent_namespace_identity == post.parent_namespace_identity &&
				pre.filesystem_profile == post.filesystem_profile &&
				pre.mount_identity == post.mount_identity;
		}

		[[nodiscard]] bool
		exact_same_direct_entry(const sqlite_writer_shm_stat_census& pre,
								const sqlite_writer_shm_stat_census& post) noexcept
		{
			return pre.state == sqlite_writer_shm_entry_state::direct_regular &&
				post.state == sqlite_writer_shm_entry_state::direct_regular &&
				pre.object_identity == post.object_identity &&
				pre.directory_entry_identity == post.directory_entry_identity;
		}

		[[nodiscard]] bool zero(const sqlite_writer_shm_bounded_count count) noexcept
		{
			return count == sqlite_writer_shm_bounded_count::zero;
		}

		[[nodiscard]] bool one(const sqlite_writer_shm_bounded_count count) noexcept
		{
			return count == sqlite_writer_shm_bounded_count::one;
		}

		[[nodiscard]] bool ambiguous(const sqlite_writer_shm_bounded_count count) noexcept
		{
			return count == sqlite_writer_shm_bounded_count::multiple_or_overflow;
		}

		[[nodiscard]] sqlite_shm_lease_rejection
		determinate_post_native_mismatch(const volatile void* native_mapping) noexcept
		{
			return {
				sqlite_shm_lease_rejection_reason::receipt_mismatch,
				native_mapping == nullptr
					? sqlite_shm_lease_recovery_action::outer_ioerr_no_retry
					: sqlite_shm_lease_recovery_action::attempt_nonremoving_unmap_then_outer_ioerr,
			};
		}

		[[nodiscard]] sqlite_shm_lease_rejection ambiguous_post_native_state() noexcept
		{
			return {
				sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
				sqlite_shm_lease_recovery_action::quarantine_no_retry,
			};
		}

		struct checked_mapping_range
		{
			std::uint64_t offset{};
			std::uint64_t byte_count{};
			std::uint64_t end{};
		};

		[[nodiscard]] std::optional<checked_mapping_range>
		derive_mapping_range(const sqlite_shm_writer_map_request& request) noexcept
		{
			if (request.page_number < 0 || request.page_size <= 0)
				return std::nullopt;
			const auto page = static_cast<std::uint64_t>(request.page_number);
			const auto size = static_cast<std::uint64_t>(request.page_size);
			if (page > std::numeric_limits<std::uint64_t>::max() / size)
				return std::nullopt;
			const auto offset = page * size;
			if (offset > std::numeric_limits<std::uint64_t>::max() - size)
				return std::nullopt;
			return checked_mapping_range{offset, size, offset + size};
		}

		[[nodiscard]] bool has_ambiguous_namespace_census(
			const sqlite_writer_shm_namespace_event_census& events) noexcept
		{
			return ambiguous(events.expected_leaf_create) || ambiguous(events.other_create) ||
				ambiguous(events.delete_event) || ambiguous(events.move_event) ||
				ambiguous(events.other_relevant_event) || events.watch_lost ||
				events.queue_overflow || events.census_overflow || events.replacement_or_aba;
		}

		[[nodiscard]] bool
		has_ambiguous_effect_census(const sqlite_writer_shm_effect_census& effects) noexcept
		{
			return ambiguous(effects.create_count) || ambiguous(effects.initialize_count) ||
				ambiguous(effects.truncate_count) || ambiguous(effects.extend_count) ||
				ambiguous(effects.delete_count) || ambiguous(effects.resize_count) ||
				effects.outcome_unknown || effects.census_overflow;
		}

		[[nodiscard]] bool
		common_namespace_matches(const sqlite_writer_shm_namespace_event_census& events,
								 const sqlite_writer_shm_mapping_epoch_receipt& receipt) noexcept
		{
			return valid_identity(events.watch_epoch) && valid_identity(events.expected_shm_leaf) &&
				events.watch_epoch == receipt.watch_arm_receipt() &&
				events.expected_shm_leaf == receipt.binding().expected_shm_leaf &&
				events.trusted_stat_watch_profile && zero(events.other_create) &&
				zero(events.delete_event) && zero(events.move_event) &&
				zero(events.other_relevant_event);
		}

		[[nodiscard]] bool common_effects_match(const sqlite_writer_shm_effect_census& effects,
												const sqlite_writer_shm_stat_census& pre,
												const sqlite_writer_shm_stat_census& post,
												const std::uint64_t range_end) noexcept
		{
			return valid_identity(effects.sqlite_source_id) &&
				valid_identity(effects.callback_transcript) &&
				valid_identity(effects.wal_write_lock_receipt) &&
				valid_identity(effects.effect_gate_receipt) &&
				valid_identity(effects.effect_receipt) && effects.complete &&
				effects.result_confirmed_success && !effects.outcome_unknown &&
				!effects.census_overflow && effects.size_before &&
				*effects.size_before == pre.byte_count && effects.size_after &&
				*effects.size_after == post.byte_count && effects.requested_range_end &&
				*effects.requested_range_end == range_end && zero(effects.initialize_count) &&
				zero(effects.truncate_count) && zero(effects.delete_count) &&
				zero(effects.resize_count);
		}

		[[nodiscard]] bool
		valid_binding(const sqlite_writer_shm_mapping_epoch_receipt& receipt) noexcept
		{
			const auto& binding = receipt.binding();
			return valid_identity(receipt.epoch_identity()) &&
				valid_identity(receipt.watch_arm_receipt()) &&
				valid_writer_request(binding.map_request) &&
				classify_sqlite_shm_writer_extend_pair(binding.map_request.caller_extend,
													   binding.delegated_extend)
					.has_value() &&
				valid_identity(binding.expected_shm_leaf) &&
				valid_identity(binding.retained_parent_receipt) &&
				valid_identity(binding.wal_native_file_receipt) &&
				valid_identity(binding.wal_xopen_receipt) &&
				valid_identity(binding.shm_native_attachment_receipt);
		}
	} // namespace

	sqlite_shm_verified_writer_route_proof::sqlite_shm_verified_writer_route_proof(
		const sqlite_writer_shm_mapping_semantic_route route,
		sqlite_shm_writer_map_request request,
		const int delegated_extend,
		sqlite_backend_opaque_identity authenticated_owned_forwarding_rw_main_route_seal,
		sqlite_backend_opaque_identity main_native_file_receipt,
		sqlite_backend_opaque_identity main_xopen_receipt,
		sqlite_backend_opaque_identity sqlite_source_id,
		sqlite_backend_opaque_identity callback_transcript,
		sqlite_backend_opaque_identity wal_write_lock_receipt,
		sqlite_backend_opaque_identity effect_gate_receipt,
		sqlite_backend_opaque_identity route_validation_seal)
		: route_{route}, request_{std::move(request)}, delegated_extend_{delegated_extend},
		  authenticated_owned_forwarding_rw_main_route_seal_{
			  std::move(authenticated_owned_forwarding_rw_main_route_seal)},
		  main_native_file_receipt_{std::move(main_native_file_receipt)},
		  main_xopen_receipt_{std::move(main_xopen_receipt)},
		  sqlite_source_id_{std::move(sqlite_source_id)},
		  callback_transcript_{std::move(callback_transcript)},
		  wal_write_lock_receipt_{std::move(wal_write_lock_receipt)},
		  effect_gate_receipt_{std::move(effect_gate_receipt)},
		  route_validation_seal_{std::move(route_validation_seal)}
	{
	}

	sqlite_shm_lease_result<sqlite_writer_shm_mapping_semantic_audit>
	validate_sqlite_writer_shm_mapping_semantics_for_audit(
		const sqlite_writer_shm_mapping_epoch_receipt& receipt) noexcept
	{
		const auto native_mapping = receipt.native_mapping();
		try
		{
			if (native_mapping == nullptr || !valid_binding(receipt))
				return determinate_post_native_mismatch(native_mapping);

			const auto& binding = receipt.binding();
			const auto& request = binding.map_request;
			const auto range = derive_mapping_range(request);
			if (!range)
				return determinate_post_native_mismatch(native_mapping);

			const auto& pre = receipt.pre_stat();
			const auto& post_observation = receipt.post_observation();
			const auto& post = post_observation.stat;
			const auto& events = post_observation.namespace_events;
			const auto& effects = post_observation.effects;

			if (has_ambiguous_namespace_census(events) || has_ambiguous_effect_census(effects))
				return ambiguous_post_native_state();

			if (!valid_stat_census(pre) || !valid_stat_census(post) ||
				!stat_context_matches(pre, post) || !common_namespace_matches(events, receipt) ||
				!common_effects_match(effects, pre, post, range->end) ||
				range->end > post.byte_count)
				return determinate_post_native_mismatch(native_mapping);

			const auto pair = classify_sqlite_shm_writer_extend_pair(request.caller_extend,
																	 binding.delegated_extend);
			if (!pair)
				return determinate_post_native_mismatch(native_mapping);

			std::optional<sqlite_writer_shm_mapping_semantic_route> route;
			if (*pair == sqlite_shm_writer_extend_pair::zero_zero)
			{
				if (exact_same_direct_entry(pre, post) &&
					post_observation.transition ==
						sqlite_writer_shm_observed_transition::preexisting_unchanged &&
					zero(events.expected_leaf_create) && post.byte_count == pre.byte_count &&
					range->end <= pre.byte_count && zero(effects.create_count) &&
					zero(effects.extend_count))
					route =
						sqlite_writer_shm_mapping_semantic_route::zero_zero_preexisting_unchanged;
			}
			else if (exact_same_direct_entry(pre, post) && zero(events.expected_leaf_create) &&
					 zero(effects.create_count))
			{
				if (post_observation.transition ==
						sqlite_writer_shm_observed_transition::preexisting_preallocated &&
					post.byte_count == pre.byte_count && range->end <= pre.byte_count &&
					zero(effects.extend_count))
					route =
						sqlite_writer_shm_mapping_semantic_route::one_one_preexisting_preallocated;
				else if (post_observation.transition ==
							 sqlite_writer_shm_observed_transition::preexisting_grown &&
						 pre.byte_count < range->end && post.byte_count == range->end &&
						 one(effects.extend_count))
					route = sqlite_writer_shm_mapping_semantic_route::one_one_preexisting_grown;
			}
			else if (pre.state == sqlite_writer_shm_entry_state::absent &&
					 post.state == sqlite_writer_shm_entry_state::direct_regular &&
					 post_observation.transition ==
						 sqlite_writer_shm_observed_transition::absent_created &&
					 one(events.expected_leaf_create) && post.byte_count == range->end &&
					 one(effects.create_count) && zero(effects.extend_count))
				route = sqlite_writer_shm_mapping_semantic_route::one_one_absent_created;

			if (!route)
				return determinate_post_native_mismatch(native_mapping);

			return sqlite_writer_shm_mapping_semantic_audit{
				*route,
				*pair,
				{
					request.page_number,
					request.page_size,
					range->offset,
					range->byte_count,
					native_mapping,
					post.byte_count,
				},
				effects.effect_receipt,
			};
		}
		catch (...)
		{
			return ambiguous_post_native_state();
		}
	}

	sqlite_shm_lease_result<sqlite_shm_verified_writer_post_map_receipt>
	sqlite_writer_shm_mapping_receipt_validator::validate(
		const sqlite_writer_shm_mapping_epoch_receipt& epoch,
		const sqlite_shm_verified_writer_route_proof& route) noexcept
	{
		auto state = epoch.begin_authoritative_validation();
		if (!state)
			return state.error();

		try
		{
			auto audit = validate_sqlite_writer_shm_mapping_semantics_for_audit(epoch);
			if (!audit)
				return audit.error();

			const auto& binding = epoch.binding();
			const auto& effects = epoch.post_observation().effects;
			const auto determinate_mismatch = route.route_ != audit->route ||
				route.request_ != binding.map_request ||
				route.delegated_extend_ != binding.delegated_extend ||
				!valid_identity(route.authenticated_owned_forwarding_rw_main_route_seal_) ||
				!valid_identity(route.route_validation_seal_) ||
				route.authenticated_owned_forwarding_rw_main_route_seal_ ==
					route.route_validation_seal_ ||
				route.main_native_file_receipt_ !=
					binding.map_request.attachment.main_native_file_receipt() ||
				route.main_xopen_receipt_ != binding.map_request.attachment.main_xopen_receipt() ||
				route.sqlite_source_id_ != effects.sqlite_source_id ||
				route.callback_transcript_ != effects.callback_transcript ||
				route.wal_write_lock_receipt_ != effects.wal_write_lock_receipt ||
				route.effect_gate_receipt_ != effects.effect_gate_receipt;
			if (determinate_mismatch)
				return determinate_post_native_mismatch(epoch.native_mapping());

			auto output = sqlite_shm_verified_writer_post_map_receipt{
				binding.map_request,
				binding.map_request.attachment.open_epoch(),
				audit->mapping,
				audit->extend_pair,
				audit->holder_specific_effect_receipt,
				*state,
				epoch.seal_sequence_,
			};
			if (!epoch.authoritative_validation_still_live(*state))
				return ambiguous_post_native_state();
			return output;
		}
		catch (...)
		{
			return ambiguous_post_native_state();
		}
	}
} // namespace cxxlens::sdk
