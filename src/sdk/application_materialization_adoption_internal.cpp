#include "application_materialization_adoption_internal.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>
#include <utility>

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error adoption_error(std::string field, std::string detail)
		{
			return {"application-analysis.adoption-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] result<std::string>
		assumption_identity(const validated_materialization_task& task,
							std::string_view replay_plan_digest)
		{
			const std::array fields{
				canonical_value::from_string(std::string{task.id()}),
				canonical_value::from_string(std::string{replay_plan_digest}),
				canonical_value::from_string(task.value().provider.provider_semantics_digest),
			};
			return canonical_identity_digest("application-analysis-assumptions", fields);
		}

		[[nodiscard]] result<partition_draft>
		partition_from_batch(const relation_engine& engine,
							 const validated_materialization_task& task,
							 const provider::detail::sealed_provider_batch& batch,
							 const std::span<const provider::coverage_unit> coverage,
							 const std::span<const provider::unresolved_item> unresolved,
							 const std::string_view source_receipt_digest,
							 const std::string_view replay_plan_digest,
							 const bool partial,
							 claim_batch& transaction_claims)
		{
			auto descriptor = engine.require_id(batch.descriptor_id());
			if (!descriptor ||
				descriptor->descriptor().descriptor_digest != batch.descriptor_digest())
				return unexpected(
					adoption_error(std::string{batch.descriptor_id()}, "descriptor-authority"));
			auto assumption = assumption_identity(task, replay_plan_digest);
			if (!assumption)
				return unexpected(std::move(assumption.error()));
			const direct_claim_basis basis{task.value().provider_input_digest};
			auto basis_digest = claim_input_basis_digest(basis);
			if (!basis_digest)
				return unexpected(std::move(basis_digest.error()));

			for (const auto& row : batch.rows())
			{
				observation value{
					row,
					{task.value().publication.snapshot.series.condition_universe_id,
					 {task.value().provider_task.condition}},
					task.value().provider_task.interpretation,
					{task.value().provider.provider_id,
					 task.value().provider.provider_semantics_digest},
					basis,
					std::string{source_receipt_digest},
					{partial ? "under_approximation" : "exact",
					 task.value().capture.value().compile_unit_id,
					 *assumption,
					 {"clang_gcc_mode_replay", "provider_protocol_v2"}},
				};
				if (auto added = transaction_claims.add_observation(engine, std::move(value));
					!added)
					return unexpected(adoption_error(std::string{batch.descriptor_id()},
													 added.error().code + ":" +
														 added.error().field + ":" +
														 added.error().detail));
			}

			partition_draft output;
			output.relation_descriptor_id = std::string{batch.descriptor_id()};
			output.scope = task.value().capture.value().project_id;
			output.condition = {task.value().publication.snapshot.series.condition_universe_id,
								{task.value().provider_task.condition}};
			output.interpretation = task.value().provider_task.interpretation;
			output.producer_semantics = task.value().provider.provider_semantics_digest;
			output.producer_input_basis_digest = std::move(*basis_digest);
			output.precision_profile = partial ? "under_approximation" : "exact";
			output.assumption_set_id = std::move(*assumption);
			for (const auto& unit : coverage)
				if (unit.kind == "relation" && unit.id == batch.descriptor_id())
					output.coverage.push_back({unit.kind,
											   unit.id,
											   unit.state,
											   unit.state == "covered" ? "" : unit.reason});
			if (output.coverage.empty())
				return unexpected(
					adoption_error(std::string{batch.descriptor_id()}, "coverage-missing"));
			if (std::ranges::any_of(output.coverage,
									[](const auto& unit)
									{
										return unit.state != "covered";
									}))
				for (const auto& item : unresolved)
				{
					const std::array fields{
						canonical_value::from_string(std::string{task.id()}),
						canonical_value::from_string(std::string{batch.descriptor_id()}),
						canonical_value::from_string(item.code),
						canonical_value::from_string(item.subject),
						canonical_value::from_string(item.detail),
					};
					auto id = canonical_identity_digest("application-analysis-unresolved", fields);
					if (!id)
						return unexpected(std::move(id.error()));
					output.unresolved.push_back({"unresolved:" + *id,
												 std::string{batch.descriptor_id()},
												 item.subject,
												 {},
												 item.code + ":" + item.detail});
				}
			return output;
		}
	} // namespace

	result<prepared_application_materialization> prepare_sealed_application_materialization(
		const relation_engine& engine,
		const validated_materialization_task& task,
		const provider::detail::sealed_provider_transcript& sealed,
		materialization_runtime_binding runtime,
		std::string source_receipt_digest,
		std::string replay_plan_digest,
		const std::span<const partition_draft> host_partitions)
	{
		if (auto valid = runtime.runtime_receipt_digest == source_receipt_digest
				? result<void>{}
				: unexpected(adoption_error("runtime_receipt", "source-mismatch"));
			!valid)
			return unexpected(std::move(valid.error()));
		if (replay_plan_digest.empty())
			return unexpected(adoption_error("replay_plan_digest", "empty"));
		const bool partial =
			std::ranges::any_of(task.value().partitions,
								[](const materialization_partition_request& value)
								{
									return value.candidate.current.input.precision_profile !=
										"exact";
								}) ||
			!sealed.unresolved().empty() ||
			std::ranges::any_of(sealed.coverage(),
								[](const provider::coverage_unit& value)
								{
									return value.state != "covered";
								});

		materialization_result_draft result_draft;
		result_draft.terminal =
			partial ? materialization_terminal::partial : materialization_terminal::complete;
		result_draft.task_id = task.id();
		result_draft.task_input_digest = task.input_binding_digest();
		result_draft.runtime = runtime;
		std::set<std::string, std::less<>> host_relations;
		for (const auto& partition : host_partitions)
			host_relations.insert(partition.relation_descriptor_id);
		for (const auto& unit : sealed.coverage())
		{
			if (unit.kind == "relation" && host_relations.contains(unit.id))
				result_draft.coverage.push_back({unit.kind, unit.id, "covered", {}});
			else
				result_draft.coverage.push_back(unit);
		}
		result_draft.unresolved.assign(sealed.unresolved().begin(), sealed.unresolved().end());

		std::set<std::string, std::less<>> relations;
		claim_batch transaction_claims;
		for (const auto& batch : sealed.batches())
		{
			if (!relations.insert(std::string{batch.descriptor_id()}).second)
				return unexpected(
					adoption_error(std::string{batch.descriptor_id()}, "duplicate-relation-batch"));
			auto partition = partition_from_batch(engine,
												  task,
												  batch,
												  sealed.coverage(),
												  sealed.unresolved(),
												  source_receipt_digest,
												  replay_plan_digest,
												  partial,
												  transaction_claims);
			if (!partition)
				return unexpected(std::move(partition.error()));
			result_draft.partitions.push_back(std::move(*partition));
		}
		if (result_draft.partitions.empty())
			return unexpected(adoption_error("partitions", "empty-provider-result"));
		std::vector<claim> host_claims;
		for (const auto& partition : host_partitions)
			host_claims.insert(host_claims.end(), partition.claims.begin(), partition.claims.end());
		auto checked = std::move(transaction_claims).commit(engine, host_claims);
		if (!checked)
			return unexpected(std::move(checked.error()));
		const auto partition_for = [&](const std::string_view relation) -> partition_draft*
		{
			const auto found = std::ranges::find(
				result_draft.partitions, relation, &partition_draft::relation_descriptor_id);
			return found == result_draft.partitions.end() ? nullptr : &*found;
		};
		for (auto& claim : checked->claims)
		{
			auto* partition = partition_for(claim.descriptor);
			if (partition == nullptr)
				return unexpected(adoption_error(claim.descriptor, "claim-partition-missing"));
			partition->claims.push_back(std::move(claim));
		}
		for (auto& unresolved : checked->unresolved)
		{
			auto* partition = partition_for(unresolved.source_relation);
			if (partition == nullptr)
				return unexpected(
					adoption_error(unresolved.source_relation, "unresolved-partition-missing"));
			partition->unresolved.push_back(std::move(unresolved));
		}
		result_draft.conflicts = std::move(checked->conflicts);
		result_draft.differential_disagreements = std::move(checked->differential_disagreements);

		auto validated = validate_materialization_result(engine, task, std::move(result_draft));
		if (!validated)
			return unexpected(std::move(validated.error()));
		auto source = make_materialization_publication_source(
			engine, task, *validated, host_partitions, source_receipt_digest);
		if (!source)
			return unexpected(std::move(source.error()));
		return prepared_application_materialization{
			std::move(*source),
			{validated->coverage().begin(), validated->coverage().end()},
			{validated->unresolved().begin(), validated->unresolved().end()},
			{validated->conflicts().begin(), validated->conflicts().end()},
			{validated->differential_disagreements().begin(),
			 validated->differential_disagreements().end()},
			std::move(runtime),
			std::move(replay_plan_digest),
		};
	}

	result<application_materialization_adoption> publish_prepared_application_materializations(
		const relation_engine& engine,
		snapshot_store& store,
		std::vector<prepared_application_materialization> prepared)
	{
		if (prepared.empty())
			return unexpected(adoption_error("prepared", "empty"));
		std::ranges::sort(prepared,
						  {},
						  [](const auto& value)
						  {
							  return value.source.task_id();
						  });

		std::map<std::pair<std::string, std::string>, provider::coverage_unit> coverage;
		std::vector<provider::unresolved_item> unresolved;
		std::vector<claim_conflict> conflicts;
		std::vector<differential_disagreement> disagreements;
		std::vector<canonical_value> replay_digests;
		std::vector<validated_materialization_publication_source> sources;
		sources.reserve(prepared.size());
		const auto state_rank = [](const std::string_view state)
		{
			if (state == "failed")
				return 4;
			if (state == "unresolved")
				return 3;
			if (state == "excluded")
				return 2;
			if (state == "not_applicable")
				return 1;
			return 0;
		};
		for (auto& value : prepared)
		{
			for (auto& unit : value.coverage)
			{
				auto [found, inserted] = coverage.try_emplace(std::pair{unit.kind, unit.id}, unit);
				if (!inserted && state_rank(unit.state) > state_rank(found->second.state))
					found->second = unit;
			}
			unresolved.insert(unresolved.end(),
							  std::make_move_iterator(value.unresolved.begin()),
							  std::make_move_iterator(value.unresolved.end()));
			conflicts.insert(conflicts.end(),
							 std::make_move_iterator(value.conflicts.begin()),
							 std::make_move_iterator(value.conflicts.end()));
			disagreements.insert(disagreements.end(),
								 std::make_move_iterator(value.differential_disagreements.begin()),
								 std::make_move_iterator(value.differential_disagreements.end()));
			replay_digests.push_back(canonical_value::from_string(value.replay_plan_digest));
			sources.push_back(std::move(value.source));
		}
		auto replay_digest = prepared.size() == 1U
			? result<std::string>{prepared.front().replay_plan_digest}
			: canonical_identity_digest(
				  "application-analysis-replay-plan-set",
				  std::array{canonical_value::from_tuple(std::move(replay_digests))});
		if (!replay_digest)
			return unexpected(std::move(replay_digest.error()));
		auto combined = combine_materialization_publication_sources(engine, std::move(sources));
		if (!combined)
			return unexpected(std::move(combined.error()));
		const auto provider_input_digest = std::string{combined->task_input_digest()};
		const auto runtime_receipt_digest = std::string{combined->source_receipt_digest()};
		auto published = publish_materialization_source(engine, store, std::move(*combined));
		if (!published)
			return unexpected(std::move(published.error()));

		std::vector<provider::coverage_unit> coverage_units;
		coverage_units.reserve(coverage.size());
		for (auto& [key, unit] : coverage)
			coverage_units.push_back(std::move(unit));
		std::ranges::sort(unresolved,
						  {},
						  [](const auto& value)
						  {
							  return std::tie(value.code, value.subject, value.detail);
						  });
		unresolved.erase(std::ranges::unique(unresolved).begin(), unresolved.end());
		return application_materialization_adoption{std::move(*published),
													std::move(coverage_units),
													std::move(unresolved),
													std::move(conflicts),
													std::move(disagreements),
													std::move(provider_input_digest),
													std::move(runtime_receipt_digest),
													std::move(*replay_digest)};
	}
} // namespace cxxlens::sdk::detail
