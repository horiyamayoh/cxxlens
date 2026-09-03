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
							 const std::string_view source_receipt_digest,
							 const std::string_view replay_plan_digest,
							 const bool partial)
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

			claim_batch claims;
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
				if (auto added = claims.add_observation(engine, std::move(value)); !added)
					return unexpected(std::move(added.error()));
			}
			auto committed = std::move(claims).commit(engine);
			if (!committed)
				return unexpected(std::move(committed.error()));

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
			output.claims = std::move(committed->claims);
			output.unresolved = std::move(committed->unresolved);
			for (const auto& unit : coverage)
				if (unit.kind == "relation" && unit.id == batch.descriptor_id())
					output.coverage.push_back({unit.kind,
											   unit.id,
											   unit.state,
											   unit.state == "covered" ? "" : unit.reason});
			if (output.coverage.empty())
				return unexpected(
					adoption_error(std::string{batch.descriptor_id()}, "coverage-missing"));
			return output;
		}
	} // namespace

	result<application_materialization_adoption> adopt_sealed_application_materialization(
		const relation_engine& engine,
		snapshot_store& store,
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
		const bool partial = !sealed.unresolved().empty() ||
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
		result_draft.coverage.assign(sealed.coverage().begin(), sealed.coverage().end());
		result_draft.unresolved.assign(sealed.unresolved().begin(), sealed.unresolved().end());

		std::set<std::string, std::less<>> relations;
		for (const auto& batch : sealed.batches())
		{
			if (!relations.insert(std::string{batch.descriptor_id()}).second)
				return unexpected(
					adoption_error(std::string{batch.descriptor_id()}, "duplicate-relation-batch"));
			auto partition = partition_from_batch(engine,
												  task,
												  batch,
												  sealed.coverage(),
												  source_receipt_digest,
												  replay_plan_digest,
												  partial);
			if (!partition)
				return unexpected(std::move(partition.error()));
			claim_batch conflict_check;
			for (const auto& claim : partition->claims)
				if (auto added = conflict_check.add(claim); !added)
					return unexpected(std::move(added.error()));
			auto checked = std::move(conflict_check).commit(engine);
			if (!checked)
				return unexpected(std::move(checked.error()));
			result_draft.conflicts.insert(
				result_draft.conflicts.end(), checked->conflicts.begin(), checked->conflicts.end());
			result_draft.differential_disagreements.insert(
				result_draft.differential_disagreements.end(),
				checked->differential_disagreements.begin(),
				checked->differential_disagreements.end());
			result_draft.partitions.push_back(std::move(*partition));
		}
		if (result_draft.partitions.empty())
			return unexpected(adoption_error("partitions", "empty-provider-result"));

		auto validated = validate_materialization_result(engine, task, std::move(result_draft));
		if (!validated)
			return unexpected(std::move(validated.error()));
		auto source = make_materialization_publication_source(
			engine, task, *validated, host_partitions, source_receipt_digest);
		if (!source)
			return unexpected(std::move(source.error()));
		auto published = publish_materialization_source(engine, store, std::move(*source));
		if (!published)
			return unexpected(std::move(published.error()));

		return application_materialization_adoption{
			std::move(*published),
			{validated->coverage().begin(), validated->coverage().end()},
			{validated->unresolved().begin(), validated->unresolved().end()},
			{validated->conflicts().begin(), validated->conflicts().end()},
			{validated->differential_disagreements().begin(),
			 validated->differential_disagreements().end()},
			std::move(runtime),
		};
	}
} // namespace cxxlens::sdk::detail
