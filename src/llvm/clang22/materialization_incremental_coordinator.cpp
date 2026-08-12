#include "materialization_incremental_coordinator.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "materialization_incremental_ingress.hpp"
#include "materialization_pipeline.hpp"
#include "sdk/provider_runtime_internal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::canonical_value text(const std::string_view value)
		{
			return sdk::canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] sdk::canonical_value tuple(std::vector<sdk::canonical_value> values)
		{
			return sdk::canonical_value::from_tuple(std::move(values));
		}

		[[nodiscard]] sdk::canonical_value number(const std::uint64_t value)
		{
			return text(std::to_string(value));
		}

		[[nodiscard]] sdk::canonical_value strings(const std::span<const std::string> values)
		{
			std::vector<sdk::canonical_value> output;
			output.reserve(values.size());
			for (const auto& value : values)
				output.push_back(text(value));
			return tuple(std::move(output));
		}

		[[nodiscard]] sdk::canonical_value row_values(const std::span<const sdk::detached_row> rows)
		{
			std::vector<sdk::canonical_value> output;
			output.reserve(rows.size());
			for (const auto& row : rows)
				output.push_back(text(row.canonical_form()));
			return tuple(std::move(output));
		}

		[[nodiscard]] sdk::canonical_value
		observation_values(const std::span<const sealed_observation_v2_row> rows)
		{
			std::vector<sdk::canonical_value> output;
			output.reserve(rows.size());
			for (const auto& row : rows)
			{
				std::vector<sdk::canonical_value> primary;
				if (row.observation.primary_span)
				{
					const auto& span = *row.observation.primary_span;
					primary = {text(span.span_id),
							   text(span.snapshot),
							   text(span.file),
							   number(span.begin),
							   number(span.end),
							   text(span.role),
							   sdk::canonical_value::from_boolean(span.read_only)};
				}
				std::vector<sdk::canonical_value> origins;
				origins.reserve(row.observation.origin_chain.size());
				for (const auto& origin : row.observation.origin_chain)
					origins.push_back(
						tuple({text(origin.kind),
							   text(origin.logical_path),
							   text(std::to_string(origin.begin)),
							   text(std::to_string(origin.end)),
							   sdk::canonical_value::from_boolean(origin.read_only)}));
				output.push_back(
					tuple({number(row.batch_index),
						   number(row.row_index),
						   number(static_cast<std::uint8_t>(row.observation.kind)),
						   text(row.observation.final_relation_compile_unit_id),
						   text(row.observation.semantic_key),
						   text(row.observation.payload_digest),
						   row.observation.primary_span ? tuple(std::move(primary))
														: sdk::canonical_value::null(),
						   tuple(std::move(origins)),
						   sdk::canonical_value::from_boolean(row.observation.exact_equivalence),
						   row.observation.limitation ? text(*row.observation.limitation)
													  : sdk::canonical_value::null()}));
			}
			return tuple(std::move(output));
		}

		[[nodiscard]] sdk::canonical_value
		provider_values(const sdk::provider::detail::sealed_provider_transcript& transcript)
		{
			std::vector<sdk::canonical_value> batches;
			batches.reserve(transcript.batches().size());
			for (const auto& batch : transcript.batches())
				batches.push_back(tuple({text(batch.task_id()),
										 text(batch.descriptor_id()),
										 text(batch.descriptor_digest()),
										 text(batch.dependency_group_id()),
										 text(batch.atomic_output_group_id()),
										 text(batch.batch_id()),
										 text(batch.batch_digest()),
										 strings(batch.ordered_chunk_digests()),
										 row_values(batch.rows())}));
			std::vector<sdk::canonical_value> coverage;
			coverage.reserve(transcript.coverage().size());
			for (const auto& item : transcript.coverage())
				coverage.push_back(
					tuple({text(item.kind), text(item.id), text(item.state), text(item.reason)}));
			std::vector<sdk::canonical_value> unresolved;
			unresolved.reserve(transcript.unresolved().size());
			for (const auto& item : transcript.unresolved())
				unresolved.push_back(
					tuple({text(item.code), text(item.subject), text(item.detail)}));
			std::vector<sdk::canonical_value> evidence;
			evidence.reserve(transcript.evidence().size());
			for (const auto& item : transcript.evidence())
				evidence.push_back(tuple({text(item.kind),
										  text(item.subject),
										  text(item.producer),
										  text(item.summary)}));
			return tuple({tuple(std::move(batches)),
						  tuple(std::move(coverage)),
						  tuple(std::move(unresolved)),
						  tuple(std::move(evidence))});
		}

		[[nodiscard]] sdk::error coordinator_error(const std::string_view field,
												   const std::string_view detail)
		{
			return sdk::error{
				"materialization.incremental-invalid", std::string{field}, std::string{detail}};
		}

		[[nodiscard]] bool result_matches_task(const sealed_materialization_result& result,
											   const validated_task_request& task) noexcept
		{
			return result.provider_task_id() == task.provider_task_id &&
				result.task_input_digest() == task.task_input_digest &&
				result.provider_execution_id() == task.provider_execution_id &&
				result.selected_catalog_compile_unit_id() ==
				task.worker_input.selected_catalog_compile_unit &&
				result.final_relation_compile_unit_id() == task.worker_input.compile_unit;
		}

		[[nodiscard]] bool
		identity_matches_task(const materialization_incremental_task_identity& identity,
							  const std::size_t task_index,
							  const validated_task_request& task) noexcept
		{
			return identity.canonical_task_ordinal == task_index &&
				identity.provider_task_id == task.provider_task_id &&
				identity.task_input_digest == task.task_input_digest &&
				identity.selected_catalog_compile_unit_id ==
				task.worker_input.selected_catalog_compile_unit &&
				identity.final_relation_compile_unit_id == task.worker_input.compile_unit;
		}

		[[nodiscard]] bool
		identity_matches_v2_1_task(const materialization_incremental_task_identity& identity,
								   const std::size_t task_index,
								   const materialization_v2_1_task_execution& task) noexcept
		{
			return identity.canonical_task_ordinal == task_index &&
				identity.provider_task_id == task.metadata.provider_task_id &&
				identity.task_input_digest == task.metadata.task_input_digest &&
				identity.selected_catalog_compile_unit_id ==
				task.metadata.selected_catalog_compile_unit_id &&
				identity.final_relation_compile_unit_id ==
				task.metadata.final_relation_compile_unit_id;
		}

		[[nodiscard]] sdk::error execution_error(const std::string_view detail)
		{
			return sdk::error{
				"materialization.incremental-execution-failed", "executor", std::string{detail}};
		}

		[[nodiscard]] sdk::result<void> validate_completeness_receipt(
			const validated_materialization_request& request,
			const std::size_t task_index,
			const materialization_incremental_provider_execution_receipt& receipt)
		{
			if (!receipt.pre_encoder_seal)
				return sdk::unexpected(coordinator_error("receipt", "missing-completeness"));
			auto valid = validate_materialization_incremental_task_receipt(
				request, task_index, receipt.pre_encoder_seal->task_receipt);
			if (!valid)
				return sdk::unexpected(
					coordinator_error("receipt", "completeness-" + valid.error().detail));
			return {};
		}

		/**
		 * Recompute the complete task receipt while the sealed result is still owned by the
		 * executor return value.  This call is deliberately before the delayed event encoder is
		 * obtained or invoked; the stream can therefore never become the source of its own
		 * completeness authority.
		 */
		[[nodiscard]] sdk::result<void> validate_pre_encoder_receipt(
			const validated_materialization_request& request,
			const std::size_t task_index,
			const sealed_materialization_result& result,
			const materialization_incremental_provider_execution_receipt& receipt,
			const std::span<const std::string> partition_ids)
		{
			if (!receipt.pre_encoder_seal)
				return sdk::unexpected(coordinator_error("receipt", "missing-completeness"));
			const auto& seal = *receipt.pre_encoder_seal;
			if (seal.partition_ids.size() != partition_ids.size() ||
				!std::ranges::equal(seal.partition_ids, partition_ids))
				return sdk::unexpected(coordinator_error("receipt", "partition-set-seal-mismatch"));
			auto partition_digest =
				seal_materialization_incremental_task_partition_set_digest(partition_ids);
			if (!partition_digest || seal.task_partition_set_digest != *partition_digest)
				return sdk::unexpected(coordinator_error("receipt", "partition-set-seal-mismatch"));
			auto artifact_digest = seal_materialization_incremental_artifact_digest(result);
			if (!artifact_digest || seal.result_artifact_digest != *artifact_digest)
				return sdk::unexpected(coordinator_error("receipt", "artifact-seal-mismatch"));
			auto events =
				materialization_incremental_result_event_projections(result, partition_ids);
			if (!events)
				return sdk::unexpected(coordinator_error(
					"receipt", "oracle-" + events.error().field + "/" + events.error().detail));
			const auto& supplied = seal.task_receipt;
			auto expected = make_materialization_incremental_task_receipt(
				request,
				task_index,
				supplied.provider_stdout_byte_count,
				supplied.provider_stdout_sha256,
				supplied.decoded_provider_frame_count,
				supplied.provider_frame_transcript_digest,
				supplied.provider_sealed_transcript_digest,
				std::span<const materialization_incremental_event_projection>{*events});
			if (!expected || *expected != supplied)
				return sdk::unexpected(coordinator_error("receipt",
														 expected ? "pre-encoder-seal-mismatch"
																  : "pre-encoder-build-failed"));
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_provider_sealed_transcript_binding(
			const sealed_materialization_result& result,
			const materialization_incremental_provider_execution_receipt& receipt)
		{
			if (!receipt.pre_encoder_seal)
				return sdk::unexpected(coordinator_error("receipt", "missing-completeness"));
			auto expected = sdk::provider::detail::provider_sealed_transcript_receipt_digest(
				result.provider_task_id(), "provider.success", result.provider_seal());
			if (!expected ||
				receipt.pre_encoder_seal->task_receipt.provider_sealed_transcript_digest !=
					*expected)
				return sdk::unexpected(coordinator_error("receipt", "sealed-transcript-mismatch"));
			return {};
		}
	} // namespace

	sdk::result<std::string>
	seal_materialization_incremental_artifact_digest(const sealed_materialization_result& result)
	{
		std::vector<sdk::canonical_value> fields{text(result.provider_task_id()),
												 text(result.task_input_digest()),
												 text(result.provider_execution_id()),
												 text(result.selected_catalog_compile_unit_id()),
												 text(result.final_relation_compile_unit_id()),
												 provider_values(result.provider_seal()),
												 row_values(result.base_claim_rows()),
												 row_values(result.source_span_claim_rows()),
												 observation_values(result.observation_rows())};
		return sdk::canonical_identity_digest("materialization.incremental-sealed-artifact",
											  fields);
	}

	sdk::result<std::string> seal_materialization_incremental_task_partition_set_digest(
		const std::span<const std::string> partition_ids)
	{
		if (partition_ids.empty() || !std::ranges::is_sorted(partition_ids) ||
			std::ranges::adjacent_find(partition_ids) != partition_ids.end())
			return sdk::unexpected(coordinator_error("task-partition-set", "noncanonical-order"));
		std::vector<sdk::canonical_value> fields;
		fields.reserve(partition_ids.size() + 1U);
		fields.push_back(number(static_cast<std::uint64_t>(partition_ids.size())));
		for (const auto& partition_id : partition_ids)
		{
			if (auto valid = sdk::validate_strong_id(partition_id); !valid)
				return sdk::unexpected(coordinator_error("task-partition-set", "partition-id"));
			fields.push_back(text(partition_id));
		}
		return sdk::canonical_identity_digest("materialization.incremental-task-partition-set",
											  fields);
	}

	sdk::result<void> run_materialization_incremental_v2_1_task_cursor(
		validated_materialization_request_v2_1& request,
		const sdk::incremental::materialization_plan& plan,
		const std::span<const materialization_incremental_task_binding> bindings,
		const materialization_v2_1_task_cursor_consumer& consumer)
	{
		try
		{
			if (!consumer)
				return sdk::unexpected(coordinator_error("cursor", "consumer-missing"));
			if (auto valid = plan.validate(); !valid)
				return sdk::unexpected(coordinator_error("plan", "plan-validation"));

			const auto task_count = request.request().task_count();
			if (task_count == 0U || task_count > std::numeric_limits<std::size_t>::max() ||
				bindings.size() != static_cast<std::size_t>(task_count))
				return sdk::unexpected(coordinator_error("tasks", "exact-census"));
			const auto task_count_size = static_cast<std::size_t>(task_count);

			std::vector<const materialization_incremental_task_binding*> by_task(task_count_size);
			std::map<std::string, const sdk::incremental::plan_entry*, std::less<>> plan_entries;
			for (const auto& entry : plan.entries)
			{
				if (!plan_entries.emplace(entry.partition_id, &entry).second)
					return sdk::unexpected(
						coordinator_error("plan.entries", "duplicate-partition"));
			}

			std::set<std::string, std::less<>> binding_ids;
			std::vector<std::optional<sdk::incremental::action>> task_actions(task_count_size);
			std::uint64_t recompute_partition_count{};
			for (const auto& binding : bindings)
			{
				const auto task_index = binding.task_identity.canonical_task_ordinal;
				if (task_index >= task_count_size || binding.partitions.empty() ||
					by_task[task_index] != nullptr ||
					!std::ranges::is_sorted(
						binding.partitions,
						{},
						&materialization_incremental_partition_binding::partition_id))
					return sdk::unexpected(coordinator_error("bindings", "task-partition-order"));

				std::optional<sdk::incremental::action> task_action;
				for (const auto& partition : binding.partitions)
				{
					const auto plan_entry = plan_entries.find(partition.partition_id);
					if (!sdk::validate_strong_id(partition.partition_id) ||
						plan_entry == plan_entries.end() ||
						!binding_ids.insert(partition.partition_id).second ||
						!partition.current_state || !partition.current_state->validate() ||
						partition.current_state->partition_id != partition.partition_id)
						return sdk::unexpected(
							coordinator_error("bindings", "partition-task-mismatch"));
					if (task_action && *task_action != plan_entry->second->decision)
						return sdk::unexpected(
							coordinator_error("bindings", "mixed-task-decisions"));
					task_action = plan_entry->second->decision;
					if (*task_action == sdk::incremental::action::recompute)
						++recompute_partition_count;
				}
				task_actions[task_index] = task_action;
				by_task[task_index] = &binding;
			}

			if (std::ranges::any_of(by_task,
									[](const auto* binding)
									{
										return binding == nullptr;
									}))
				return sdk::unexpected(coordinator_error("bindings", "missing-task"));

			std::set<std::string, std::less<>> plan_ids;
			for (const auto& [partition_id, entry] : plan_entries)
			{
				(void)entry;
				plan_ids.insert(partition_id);
			}
			if (binding_ids != plan_ids ||
				recompute_partition_count != plan.frontend_provider_executions)
				return sdk::unexpected(coordinator_error("bindings", "plan-partition-census"));

			auto cursor_result = make_materialization_v2_1_task_cursor(request);
			if (!cursor_result)
				return sdk::unexpected(std::move(cursor_result.error()));
			auto cursor = std::move(*cursor_result);
			for (std::uint64_t task_index{}; task_index < task_count; ++task_index)
			{
				auto next = cursor.next();
				if (!next)
					return sdk::unexpected(std::move(next.error()));
				if (!*next || (*next)->metadata.task_index != task_index ||
					cursor.next_task_index() != task_index + 1U)
					return sdk::unexpected(coordinator_error("cursor", "order-or-end"));

				const auto task_index_size = static_cast<std::size_t>(task_index);
				auto task = std::move(**next);
				if (!identity_matches_v2_1_task(
						by_task[task_index_size]->task_identity, task_index_size, task))
					return sdk::unexpected(coordinator_error("bindings", "task-identity-mismatch"));
				{
					// The cursor lease must be released before the next call to next(), and before
					// the successful finalize below.
					auto consumed = consumer(task_index_size,
											 *task_actions[task_index_size],
											 task,
											 *by_task[task_index_size]);
					if (!consumed)
						return sdk::unexpected(std::move(consumed.error()));
				}
			}

			auto end = cursor.next();
			if (!end)
				return sdk::unexpected(std::move(end.error()));
			if (*end)
				return sdk::unexpected(coordinator_error("cursor", "order-or-end"));
			if (auto finalized = std::move(cursor).finalize(); !finalized)
				return sdk::unexpected(std::move(finalized.error()));
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(coordinator_error("allocation", "unavailable"));
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(coordinator_error("allocation", "unavailable"));
		}
		catch (...)
		{
			return sdk::unexpected(coordinator_error("cursor", "exception"));
		}
	}

	sealed_materialization_incremental_result::sealed_materialization_incremental_result(
		sealed_materialization_claims claims,
		materialization_bounded_claim_source bounded_claim_source,
		materialization_incremental_execution_census execution_census,
		materialization_claim_stream_source claim_stream) noexcept
		: claims_{std::move(claims)}, bounded_claim_source_{std::move(bounded_claim_source)},
		  execution_census_{std::move(execution_census)}, claim_stream_{std::move(claim_stream)}
	{
	}

	const sealed_materialization_claims&
	sealed_materialization_incremental_result::claims() const noexcept
	{
		return claims_;
	}

	materialization_bounded_claim_source&
	sealed_materialization_incremental_result::bounded_claim_source() noexcept
	{
		return bounded_claim_source_;
	}

	const materialization_bounded_claim_source&
	sealed_materialization_incremental_result::bounded_claim_source() const noexcept
	{
		return bounded_claim_source_;
	}

	const materialization_incremental_execution_census&
	sealed_materialization_incremental_result::execution_census() const noexcept
	{
		return execution_census_;
	}

	const materialization_claim_stream_source*
	sealed_materialization_incremental_result::claim_stream() const noexcept
	{
		return &claim_stream_;
	}

	materialization_incremental_publication_result::materialization_incremental_publication_result(
		sealed_materialization_incremental_result materialization,
		materialization_store_observation store) noexcept
		: materialization_{std::move(materialization)}, store_{std::move(store)}
	{
	}

	const sealed_materialization_incremental_result&
	materialization_incremental_publication_result::materialization() const noexcept
	{
		return materialization_;
	}

	const materialization_store_observation&
	materialization_incremental_publication_result::store() const noexcept
	{
		return store_;
	}

	bool materialization_incremental_publication_result::publication_verified() const noexcept
	{
		return store_.publication_attempted && store_.publish_call_count == 1U &&
			store_.publish_returned_handle.has_value() &&
			store_.publish_returned_record.has_value() && store_.verification_store.has_value() &&
			!store_.first_issue.has_value();
	}

	sdk::result<sealed_materialization_incremental_result>
	run_materialization_incremental_coordinator(
		const validated_materialization_request& request,
		const sdk::incremental::materialization_plan& plan,
		std::vector<materialization_incremental_task_binding> bindings,
		materialization_incremental_task_executor& executor,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority)
	{
		std::optional<sdk::error> cleanup_failure;
		bool cleanup_failed{};
		try
		{
			if (auto valid = plan.validate(); !valid)
				return sdk::unexpected(coordinator_error("plan", "plan-validation"));
			if (request.tasks.empty() || bindings.size() != request.tasks.size())
				return sdk::unexpected(coordinator_error("tasks", "exact-census"));

			std::vector<materialization_incremental_task_binding*> by_task(request.tasks.size());
			std::map<std::string, const sdk::incremental::plan_entry*, std::less<>> plan_entries;
			for (const auto& entry : plan.entries)
			{
				if (!plan_entries.emplace(entry.partition_id, &entry).second)
					return sdk::unexpected(
						coordinator_error("plan.entries", "duplicate-partition"));
			}
			std::set<std::string, std::less<>> binding_ids;
			std::vector<std::optional<sdk::incremental::action>> task_actions(request.tasks.size());
			std::uint64_t recompute_partition_count{};
			std::uint64_t recompute_task_count{};
			for (auto& binding : bindings)
			{
				const auto task_index = binding.task_identity.canonical_task_ordinal;
				if (binding.partitions.empty() || task_index >= request.tasks.size() ||
					by_task[binding.task_identity.canonical_task_ordinal] != nullptr ||
					!identity_matches_task(binding.task_identity,
										   binding.task_identity.canonical_task_ordinal,
										   request.tasks[task_index]) ||
					!std::ranges::is_sorted(
						binding.partitions,
						{},
						&materialization_incremental_partition_binding::partition_id))
					return sdk::unexpected(coordinator_error("bindings", "task-partition-order"));

				std::optional<sdk::incremental::action> task_action;
				for (const auto& partition : binding.partitions)
				{
					const auto plan_entry = plan_entries.find(partition.partition_id);
					if (!sdk::validate_strong_id(partition.partition_id) ||
						plan_entry == plan_entries.end() ||
						!binding_ids.insert(partition.partition_id).second ||
						!partition.current_state || !partition.current_state->validate() ||
						partition.current_state->partition_id != partition.partition_id)
						return sdk::unexpected(
							coordinator_error("bindings", "partition-task-mismatch"));
					if (task_action && *task_action != plan_entry->second->decision)
						return sdk::unexpected(
							coordinator_error("bindings", "mixed-task-decisions"));
					task_action = plan_entry->second->decision;
					if (*task_action == sdk::incremental::action::recompute)
						++recompute_partition_count;
				}
				task_actions[task_index] = task_action;
				if (*task_action == sdk::incremental::action::recompute)
					++recompute_task_count;
				by_task[task_index] = &binding;
			}
			if (std::ranges::any_of(by_task,
									[](const auto* binding)
									{
										return binding == nullptr;
									}))
				return sdk::unexpected(coordinator_error("bindings", "missing-task"));

			std::set<std::string, std::less<>> plan_ids;
			for (const auto& [partition_id, entry] : plan_entries)
			{
				(void)entry;
				plan_ids.insert(partition_id);
			}
			if (binding_ids != plan_ids ||
				recompute_partition_count != plan.frontend_provider_executions)
				return sdk::unexpected(coordinator_error("bindings", "plan-partition-census"));

			std::vector<std::vector<std::string>> expected_partition_ids(request.tasks.size());
			for (std::size_t task_index{}; task_index < by_task.size(); ++task_index)
			{
				auto& expected = expected_partition_ids[task_index];
				expected.reserve(by_task[task_index]->partitions.size());
				for (const auto& partition : by_task[task_index]->partitions)
					expected.push_back(partition.partition_id);
			}
			auto ingress_begin = materialization_incremental_ingress::begin(
				request, std::move(expected_partition_ids));
			if (!ingress_begin)
				return sdk::unexpected(coordinator_error("ingress", ingress_begin.error().detail));
			std::optional<materialization_incremental_ingress> ingress{std::move(*ingress_begin)};

			materialization_incremental_execution_census census{plan.frontend_provider_executions,
																recompute_task_count,
																0U,
																0U,
																plan.warm_zero,
																{},
																{},
																{},
																{},
																{},
																{}};
			census.executed_partition_ids.reserve(
				static_cast<std::size_t>(plan.frontend_provider_executions));
			census.executed_provider_task_ids.reserve(
				static_cast<std::size_t>(plan.frontend_provider_executions));
			census.executed_provider_execution_ids.reserve(
				static_cast<std::size_t>(plan.frontend_provider_executions));
			census.executed_artifact_digests.reserve(
				static_cast<std::size_t>(plan.frontend_provider_executions));
			std::set<std::string, std::less<>> provider_execution_ids;
			std::optional<sealed_materialization_result> current;
			std::optional<std::size_t> current_task_index;
			std::optional<materialization_incremental_task_receipt> current_receipt;
			std::vector<std::unique_ptr<materialization_replayable_spool>> current_partition_spools;
			auto bounded_source = materialization_bounded_claim_source::begin(request);
			if (!bounded_source)
				return sdk::unexpected(
					coordinator_error("claim-source", bounded_source.error().detail));
			const auto adopt_bounded_current = [&]() -> sdk::result<void>
			{
				if (!current || !current_task_index)
					return sdk::unexpected(
						coordinator_error("claim-source", "missing-current-task"));
				auto task_claims =
					construct_materialization_bounded_task_claims(request,
																  *current_task_index,
																  *current,
																  producer_authority,
																  guarantee_authority);
				if (!task_claims)
					return sdk::unexpected(std::move(task_claims.error()));
				return bounded_source->consume_task(std::move(*task_claims));
			};
			const auto consume_current = [&]() -> sdk::result<void>
			{
				if (!current_task_index)
					return {};
				if (!current || !current_receipt || !ingress)
					return sdk::unexpected(coordinator_error("ingress", "missing-current-task"));
				materialization_incremental_task_ingress task{std::move(*current),
															  std::move(*current_receipt),
															  std::move(current_partition_spools)};
				current.reset();
				current_receipt.reset();
				current_task_index.reset();
				auto consumed = std::move(*ingress).consume_task(std::move(task));
				if (!consumed)
					return sdk::unexpected(coordinator_error(
						"ingress", consumed.error().field + "/" + consumed.error().detail));
				return {};
			};
			const auto record_cleanup_failure = [&](sdk::error failure) noexcept
			{
				cleanup_failed = true;
				try
				{
					if (!cleanup_failure)
						cleanup_failure.emplace(std::move(failure));
				}
				catch (...)
				{
				}
			};
			// Claim construction can reject a task after load() has installed the one live
			// sealed result. Keep that owner behind the same ingress boundary on every
			// unwinding path. A cleanup failure is itself terminal: returning the original
			// claim error would leave a sealed result outside the ingress journal.
			const auto cleanup_pending = [&](const void*) noexcept
			{
				try
				{
					if (auto cleaned = consume_current(); !cleaned)
						record_cleanup_failure(std::move(cleaned.error()));
				}
				catch (...)
				{
					record_cleanup_failure(execution_error("cleanup-exception"));
				}
			};
			const std::unique_ptr<const char, decltype(cleanup_pending)> pending_cleanup{
				reinterpret_cast<const char*>(&cleanup_pending), cleanup_pending};
			const materialization_task_result_loader load = [&](const std::size_t task_index)
				-> sdk::result<std::reference_wrapper<const sealed_materialization_result>>
			{
				try
				{
					if (task_index >= by_task.size() || by_task[task_index] == nullptr)
						return sdk::unexpected(coordinator_error("bindings", "missing-task"));
					auto* binding = by_task[task_index];
					if (!task_actions[task_index])
						return sdk::unexpected(
							coordinator_error("bindings", "missing-task-decision"));
					for (const auto& partition : binding->partitions)
						if (!partition.current_state ||
							partition.current_state->corruption_detected)
							return sdk::unexpected(
								coordinator_error("current", "corrupt-partition"));
					if (auto consumed = consume_current(); !consumed)
						return sdk::unexpected(std::move(consumed.error()));

					std::vector<std::string> partition_ids;
					partition_ids.reserve(binding->partitions.size());
					for (const auto& partition : binding->partitions)
						partition_ids.push_back(partition.partition_id);
					auto partition_set_digest =
						seal_materialization_incremental_task_partition_set_digest(partition_ids);
					if (!partition_set_digest)
						return sdk::unexpected(std::move(partition_set_digest.error()));

					if (*task_actions[task_index] == sdk::incremental::action::reuse)
					{
						for (const auto& partition : binding->partitions)
						{
							if (!partition.prior_artifact ||
								!partition.prior_artifact->state.validate() ||
								partition.prior_artifact->state.partition_id !=
									partition.partition_id ||
								partition.prior_artifact->state.corruption_detected ||
								partition.prior_artifact->state != *partition.current_state)
								return sdk::unexpected(
									coordinator_error("prior", "sealed-artifact-mismatch"));
						}
						auto prior =
							executor.load_reusable(task_index, request.tasks[task_index], *binding);
						if (!prior)
							return sdk::unexpected(std::move(prior.error()));
						if (prior->receipt.provider_call_count != 0U ||
							!result_matches_task(prior->result, request.tasks[task_index]) ||
							prior->receipt.provider_task_id != prior->result.provider_task_id() ||
							prior->receipt.provider_execution_id !=
								prior->result.provider_execution_id() ||
							prior->receipt.covered_partition_ids != partition_ids ||
							prior->receipt.task_partition_set_digest != *partition_set_digest)
							return sdk::unexpected(
								coordinator_error("prior", "sealed-artifact-mismatch"));
						auto completeness =
							validate_completeness_receipt(request, task_index, prior->receipt);
						if (!completeness)
							return sdk::unexpected(std::move(completeness.error()));
						auto transcript_binding = validate_provider_sealed_transcript_binding(
							prior->result, prior->receipt);
						if (!transcript_binding)
							return sdk::unexpected(std::move(transcript_binding.error()));
						auto pre_encoder = validate_pre_encoder_receipt(
							request, task_index, prior->result, prior->receipt, partition_ids);
						if (!pre_encoder)
							return sdk::unexpected(std::move(pre_encoder.error()));
						if (!prior->encode_partition_spools)
							return sdk::unexpected(
								coordinator_error("executor", "missing-delayed-encoder"));
						auto encoded_spools = prior->encode_partition_spools(
							prior->result, *prior->receipt.pre_encoder_seal);
						if (!encoded_spools)
							return sdk::unexpected(std::move(encoded_spools.error()));
						auto prior_digest =
							seal_materialization_incremental_artifact_digest(prior->result);
						if (!prior_digest ||
							*prior_digest != prior->receipt.sealed_artifact_digest ||
							!std::ranges::all_of(
								binding->partitions,
								[&](const auto& partition)
								{
									return partition.prior_artifact &&
										partition.prior_artifact->sealed_artifact_digest ==
										*prior_digest;
								}))
							return sdk::unexpected(
								coordinator_error("prior", "artifact-receipt-mismatch"));
						if (!provider_execution_ids.insert(prior->receipt.provider_execution_id)
								 .second)
							return sdk::unexpected(
								coordinator_error("prior", "duplicate-execution-id"));
						current.emplace(std::move(prior->result));
						current_task_index = task_index;
						current_receipt = std::move(prior->receipt.pre_encoder_seal->task_receipt);
						current_partition_spools = std::move(*encoded_spools);
						if (auto adopted = adopt_bounded_current(); !adopted)
							return sdk::unexpected(std::move(adopted.error()));
						return std::cref(*current);
					}

					if (executor.cancellation_requested())
						return sdk::unexpected(coordinator_error("executor", "cancelled"));
					auto executed =
						executor.execute(task_index, request.tasks[task_index], *binding);
					if (!executed)
						return sdk::unexpected(std::move(executed.error()));
					if (executed->receipt.provider_call_count != 1U ||
						!result_matches_task(executed->result, request.tasks[task_index]) ||
						executed->receipt.provider_task_id != executed->result.provider_task_id() ||
						executed->receipt.provider_execution_id !=
							executed->result.provider_execution_id() ||
						executed->receipt.covered_partition_ids != partition_ids ||
						executed->receipt.task_partition_set_digest != *partition_set_digest)
						return sdk::unexpected(
							coordinator_error("executor", "sealed-result-mismatch"));
					auto completeness =
						validate_completeness_receipt(request, task_index, executed->receipt);
					if (!completeness)
						return sdk::unexpected(std::move(completeness.error()));
					auto transcript_binding = validate_provider_sealed_transcript_binding(
						executed->result, executed->receipt);
					if (!transcript_binding)
						return sdk::unexpected(std::move(transcript_binding.error()));
					auto pre_encoder = validate_pre_encoder_receipt(
						request, task_index, executed->result, executed->receipt, partition_ids);
					if (!pre_encoder)
						return sdk::unexpected(std::move(pre_encoder.error()));
					if (!executed->encode_partition_spools)
						return sdk::unexpected(
							coordinator_error("executor", "missing-delayed-encoder"));
					auto encoded_spools = executed->encode_partition_spools(
						executed->result, *executed->receipt.pre_encoder_seal);
					if (!encoded_spools)
						return sdk::unexpected(std::move(encoded_spools.error()));
					auto execution_digest =
						seal_materialization_incremental_artifact_digest(executed->result);
					if (!execution_digest ||
						*execution_digest != executed->receipt.sealed_artifact_digest)
						return sdk::unexpected(
							coordinator_error("executor", "execution-receipt-mismatch"));
					if (!provider_execution_ids.insert(executed->receipt.provider_execution_id)
							 .second)
						return sdk::unexpected(
							coordinator_error("executor", "duplicate-execution-id"));
					current.emplace(std::move(executed->result));
					current_task_index = task_index;
					current_receipt = std::move(executed->receipt.pre_encoder_seal->task_receipt);
					current_partition_spools = std::move(*encoded_spools);
					if (auto adopted = adopt_bounded_current(); !adopted)
						return sdk::unexpected(std::move(adopted.error()));
					++census.actual_provider_executions;
					census.executed_partition_ids.insert(census.executed_partition_ids.end(),
														 partition_ids.begin(),
														 partition_ids.end());
					census.executed_provider_task_ids.push_back(executed->receipt.provider_task_id);
					census.executed_provider_execution_ids.push_back(
						executed->receipt.provider_execution_id);
					census.executed_artifact_digests.push_back(
						executed->receipt.sealed_artifact_digest);
					census.executed_task_partition_set_digests.push_back(
						executed->receipt.task_partition_set_digest);
					census.actual_recomputed_partition_count += partition_ids.size();
					return std::cref(*current);
				}
				catch (const std::bad_alloc&)
				{
					return sdk::unexpected(coordinator_error("allocation", "unavailable"));
				}
				catch (...)
				{
					return sdk::unexpected(execution_error("exception"));
				}
			};

			auto claims = construct_materialization_claims_from_loader(
				request, load, producer_authority, guarantee_authority);
			if (!claims)
			{
				if (auto cleaned = consume_current(); !cleaned)
					return sdk::unexpected(std::move(cleaned.error()));
				return sdk::unexpected(std::move(claims.error()));
			}
			if (auto consumed = consume_current(); !consumed)
				return sdk::unexpected(std::move(consumed.error()));
			if (executor.cancellation_requested())
				return sdk::unexpected(coordinator_error("executor", "cancelled"));
			std::ranges::sort(census.executed_partition_ids);
			if (census.actual_provider_executions != census.planned_provider_task_executions ||
				census.actual_recomputed_partition_count != plan.frontend_provider_executions ||
				census.executed_partition_ids.size() != census.actual_recomputed_partition_count ||
				census.executed_provider_task_ids.size() != census.actual_provider_executions ||
				census.executed_provider_execution_ids.size() !=
					census.actual_provider_executions ||
				census.executed_artifact_digests.size() != census.actual_provider_executions ||
				census.executed_task_partition_set_digests.size() !=
					census.actual_provider_executions ||
				census.warm_zero != (census.actual_provider_executions == 0U))
				return sdk::unexpected(coordinator_error("execution", "census-mismatch"));
			auto ingress_result = std::move(*ingress).finalize_with_claim_stream();
			if (!ingress_result)
				return sdk::unexpected(coordinator_error("receipt", ingress_result.error().detail));
			census.execution_journal_receipt = ingress_result->journal;
			auto claim_stream = materialization_claim_stream_source::begin(
				request, ingress_result->journal, std::move(ingress_result->claim_stream_tasks));
			if (!claim_stream)
				return sdk::unexpected(
					coordinator_error("claim-stream", claim_stream.error().detail));
			auto bounded = std::move(*bounded_source).finalize();
			if (!bounded)
				return sdk::unexpected(coordinator_error("claim-source", bounded.error().detail));

			return sealed_materialization_incremental_result{std::move(*claims),
															 std::move(*bounded),
															 std::move(census),
															 std::move(*claim_stream)};
		}
		catch (const std::bad_alloc&)
		{
			if (cleanup_failed && cleanup_failure)
				return sdk::unexpected(std::move(*cleanup_failure));
			if (cleanup_failed)
				return sdk::unexpected(execution_error("cleanup-failed"));
			return sdk::unexpected(coordinator_error("allocation", "unavailable"));
		}
		catch (const std::length_error&)
		{
			if (cleanup_failed && cleanup_failure)
				return sdk::unexpected(std::move(*cleanup_failure));
			if (cleanup_failed)
				return sdk::unexpected(execution_error("cleanup-failed"));
			return sdk::unexpected(coordinator_error("allocation", "unavailable"));
		}
		catch (...)
		{
			if (cleanup_failed && cleanup_failure)
				return sdk::unexpected(std::move(*cleanup_failure));
			if (cleanup_failed)
				return sdk::unexpected(execution_error("cleanup-failed"));
			return sdk::unexpected(execution_error("unexpected"));
		}
	}

	sdk::result<materialization_incremental_publication_result>
	run_materialization_incremental_coordinator_and_publish(
		const validated_materialization_request& request,
		const sdk::incremental::materialization_plan& plan,
		std::vector<materialization_incremental_task_binding> bindings,
		materialization_incremental_task_executor& executor,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority)
	{
		try
		{
			auto materialization = run_materialization_incremental_coordinator(request,
																			   plan,
																			   std::move(bindings),
																			   executor,
																			   producer_authority,
																			   guarantee_authority);
			if (!materialization)
				return sdk::unexpected(std::move(materialization.error()));
			auto transaction =
				make_materialization_store_transaction(request, materialization->claims());
			if (!transaction)
				return sdk::unexpected(std::move(transaction.error()));
			auto observation = execute_materialization_store(
				request.engine, request.publication, std::move(*transaction));
			return materialization_incremental_publication_result{std::move(*materialization),
																  std::move(observation)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(coordinator_error("allocation", "unavailable"));
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(coordinator_error("allocation", "unavailable"));
		}
		catch (...)
		{
			return sdk::unexpected(execution_error("unexpected"));
		}
	}
} // namespace cxxlens::detail::clang22::materialization
