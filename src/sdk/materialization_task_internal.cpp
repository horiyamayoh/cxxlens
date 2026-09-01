#include "materialization_task_internal.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error task_error(std::string field,
									   std::string detail,
									   std::string code = "sdk.materialization-task-invalid")
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] error result_error(std::string field,
										 std::string detail,
										 std::string code = "sdk.materialization-result-invalid")
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] result<void> require_strong(const std::string_view value,
												  std::string field,
												  const bool result_side = false)
		{
			if (auto valid = validate_strong_id(value); !valid)
				return unexpected(result_side ? result_error(std::move(field), "strong-id")
											  : task_error(std::move(field), "strong-id"));
			return {};
		}

		[[nodiscard]] canonical_value version_value(const semantic_version& version)
		{
			return canonical_value::from_tuple({
				canonical_value::from_integer(version.major),
				canonical_value::from_integer(version.minor),
				canonical_value::from_integer(version.patch),
			});
		}

		[[nodiscard]] std::string_view terminal_name(const materialization_terminal terminal)
		{
			switch (terminal)
			{
				case materialization_terminal::complete:
					return "complete";
				case materialization_terminal::partial:
					return "partial";
				case materialization_terminal::rejected:
					return "rejected";
				case materialization_terminal::failed:
					return "failed";
				case materialization_terminal::cancelled:
					return "cancelled";
			}
			return "invalid";
		}

		[[nodiscard]] snapshot_partition_binding binding_for(const partition_manifest& manifest,
															 const partition_draft& draft)
		{
			return {manifest.partition_id,
					draft.relation_descriptor_id,
					draft.scope,
					draft.condition,
					draft.interpretation,
					draft.producer_semantics,
					draft.producer_input_basis_digest,
					draft.precision_profile,
					draft.assumption_set_id};
		}

		[[nodiscard]] result<void>
		validate_provider_requirement(const materialization_provider_requirement& value,
									  const provider::task& task)
		{
			for (const auto& [field, input] :
				 {std::pair{std::string_view{"provider_binary_digest"},
							std::string_view{value.provider_binary_digest}},
				  std::pair{std::string_view{"provider_semantics_digest"},
							std::string_view{value.provider_semantics_digest}},
				  std::pair{std::string_view{"trust_policy_digest"},
							std::string_view{value.trust_policy_digest}}})
				if (auto valid = require_strong(input, std::string{field}); !valid)
					return valid;
			if (value.provider_id.empty() || value.required_qualification.empty())
				return unexpected(task_error("provider", "omitted-authority"));
			if (value.provider_id != task.session.provider_id ||
				value.provider_version != task.session.provider_version ||
				value.provider_semantics_digest != task.session.provider_semantic_contract_digest)
				return unexpected(task_error("provider", "portable-task-mismatch"));
			if (auto valid = value.sandbox.validate(); !valid)
				return unexpected(task_error("provider.sandbox", valid.error().code));
			if (auto valid = value.budget.validate(); !valid)
				return unexpected(task_error("provider.budget", valid.error().code));
			return {};
		}

		[[nodiscard]] result<void>
		validate_publication_requirement(const materialization_publication_requirement& value,
										 const validated_build_capture& capture)
		{
			if (auto valid = value.snapshot.series.validate(); !valid)
				return unexpected(task_error("publication.series", valid.error().code));
			if (value.snapshot.catalog_semantic_digest != capture.value().catalog.catalog_digest)
				return unexpected(task_error("publication.catalog", "capture-mismatch"));
			if (value.snapshot.expected_parent_publication)
				if (auto valid = require_strong(*value.snapshot.expected_parent_publication,
												"publication.expected_parent");
					!valid)
					return valid;
			for (const auto& [field, input] :
				 {std::pair{std::string_view{"publication.analysis_recipe_digest"},
							std::string_view{value.analysis_recipe_digest}},
				  std::pair{std::string_view{"publication.output_plan_digest"},
							std::string_view{value.output_plan_digest}}})
				if (auto valid = require_strong(input, std::string{field}); !valid)
					return valid;
			if (value.publication_target.empty())
				return unexpected(task_error("publication.target", "empty"));
			return {};
		}

		[[nodiscard]] result<std::string>
		task_binding_digest(const materialization_task_draft& value,
							const incremental::materialization_plan& plan)
		{
			auto portable_projection = value.provider_task.canonical_projection();
			if (!portable_projection)
				return unexpected(std::move(portable_projection.error()));
			std::vector<canonical_value> requested;
			requested.reserve(value.partitions.size());
			for (const auto& partition : value.partitions)
			{
				auto input = partition.candidate.current.input.digest();
				if (!input)
					return unexpected(std::move(input.error()));
				requested.push_back(canonical_value::from_tuple({
					canonical_value::from_string(partition.relation_descriptor_id),
					canonical_value::from_string(partition.candidate.current.partition_id),
					canonical_value::from_string(*input),
				}));
			}
			const std::array fields{
				canonical_value::from_string(value.materialization_request_id),
				canonical_value::from_string(value.provider_input_digest),
				canonical_value::from_string(std::string{value.capture.semantic_identity()}),
				canonical_value::from_string(content_digest(*portable_projection)),
				canonical_value::from_tuple(std::move(requested)),
				canonical_value::from_string(plan.plan_digest),
				canonical_value::from_string(value.provider.provider_id),
				version_value(value.provider.provider_version),
				canonical_value::from_string(value.provider.provider_binary_digest),
				canonical_value::from_string(value.provider.provider_semantics_digest),
				canonical_value::from_string(value.provider.required_qualification),
				canonical_value::from_string(value.provider.trust_policy_digest),
				canonical_value::from_string(value.provider.sandbox.policy_digest),
				canonical_value::from_string(value.publication.snapshot.series.id()),
				canonical_value::from_string(value.publication.snapshot.catalog_semantic_digest),
				canonical_value::from_string(value.publication.analysis_recipe_digest),
				canonical_value::from_string(value.publication.output_plan_digest),
				canonical_value::from_string(value.publication.publication_target),
			};
			return canonical_identity_digest("materialization-task-input", fields);
		}

		[[nodiscard]] result<void> validate_runtime(const materialization_runtime_binding& runtime,
													const validated_materialization_task& task)
		{
			const auto& requirement = task.value().provider;
			if (runtime.provider_id != requirement.provider_id ||
				runtime.provider_version != requirement.provider_version ||
				runtime.measured_provider_binary_digest != requirement.provider_binary_digest ||
				runtime.provider_semantics_digest != requirement.provider_semantics_digest ||
				runtime.task_input_digest != task.value().provider_input_digest)
				return unexpected(result_error("runtime", "task-or-provider-mismatch"));
			if (auto valid =
					require_strong(runtime.runtime_receipt_digest, "runtime.receipt_digest", true);
				!valid)
				return valid;
			return {};
		}

		[[nodiscard]] result<void>
		validate_closure(const closure_candidate& closure,
						 const std::map<std::string, partition_manifest, std::less<>>& manifests)
		{
			const auto partition = manifests.find(closure.subject_partition_id);
			if (partition == manifests.end() ||
				partition->second.relation_descriptor_id != closure.relation_descriptor_id ||
				partition->second.content_digest != closure.partition_content_digest ||
				partition->second.coverage_digest != closure.coverage_digest)
				return unexpected(result_error("closure", "partition-mismatch"));
			if (auto valid = closure.condition.validate(); !valid)
				return unexpected(result_error("closure.condition", valid.error().code));
			for (const auto& [field, input] :
				 {std::pair{std::string_view{"closure.key_domain_digest"},
							std::string_view{closure.key_domain_digest}},
				  std::pair{std::string_view{"closure.producer_semantics"},
							std::string_view{closure.producer_semantics}},
				  std::pair{std::string_view{"closure.evidence_digest"},
							std::string_view{closure.evidence_digest}}})
				if (auto valid = require_strong(input, std::string{field}, true); !valid)
					return valid;
			if (closure.interpretation.empty() || closure.assumption_set_id.empty() ||
				closure.closure_kind.empty())
				return unexpected(result_error("closure", "omitted-authority"));
			return {};
		}

		[[nodiscard]] result<std::string>
		result_identity(const materialization_result_draft& draft,
						const std::span<const validated_materialization_partition> partitions)
		{
			std::vector<canonical_value> partition_values;
			partition_values.reserve(partitions.size());
			for (const auto& partition : partitions)
				partition_values.push_back(canonical_value::from_tuple({
					canonical_value::from_string(partition.manifest.partition_id),
					canonical_value::from_string(partition.manifest.content_digest),
					canonical_value::from_string(partition.manifest.coverage_digest),
				}));

			std::vector<canonical_value> closure_values;
			closure_values.reserve(draft.closures.size());
			for (const auto& closure : draft.closures)
				closure_values.push_back(canonical_value::from_tuple({
					canonical_value::from_string(closure.relation_descriptor_id),
					canonical_value::from_string(closure.subject_partition_id),
					canonical_value::from_string(closure.partition_content_digest),
					canonical_value::from_string(closure.evidence_digest),
				}));

			std::vector<canonical_value> coverage_values;
			coverage_values.reserve(draft.coverage.size());
			for (const auto& unit : draft.coverage)
				coverage_values.push_back(canonical_value::from_tuple({
					canonical_value::from_string(unit.kind),
					canonical_value::from_string(unit.id),
					canonical_value::from_string(unit.state),
					canonical_value::from_string(unit.reason),
				}));

			std::vector<canonical_value> unresolved_values;
			unresolved_values.reserve(draft.unresolved.size());
			for (const auto& item : draft.unresolved)
				unresolved_values.push_back(canonical_value::from_tuple({
					canonical_value::from_string(item.code),
					canonical_value::from_string(item.subject),
					canonical_value::from_string(item.detail),
				}));

			auto disagreements = claim_batch_content_digest(
				{}, {}, draft.conflicts, draft.differential_disagreements);
			if (!disagreements)
				return unexpected(std::move(disagreements.error()));

			canonical_value runtime = canonical_value::null();
			if (draft.runtime)
				runtime = canonical_value::from_tuple({
					canonical_value::from_string(draft.runtime->provider_id),
					version_value(draft.runtime->provider_version),
					canonical_value::from_string(draft.runtime->measured_provider_binary_digest),
					canonical_value::from_string(draft.runtime->provider_semantics_digest),
					canonical_value::from_string(draft.runtime->runtime_receipt_digest),
				});
			const std::array fields{
				canonical_value::from_string(std::string{terminal_name(draft.terminal)}),
				canonical_value::from_string(draft.task_id),
				canonical_value::from_string(draft.task_input_digest),
				std::move(runtime),
				canonical_value::from_tuple(std::move(partition_values)),
				canonical_value::from_tuple(std::move(closure_values)),
				canonical_value::from_tuple(std::move(coverage_values)),
				canonical_value::from_tuple(std::move(unresolved_values)),
				canonical_value::from_string(*disagreements),
			};
			return canonical_identity_digest("materialization-result", fields);
		}
	} // namespace

	result<validated_materialization_task>
	validate_materialization_task(materialization_task_draft draft)
	{
		if (auto valid =
				require_strong(draft.materialization_request_id, "materialization_request_id");
			!valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = require_strong(draft.provider_input_digest, "provider_input_digest");
			!valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = draft.provider_task.validate(); !valid)
			return unexpected(task_error("provider_task", valid.error().code));
		const auto& capture = draft.capture.value();
		if (draft.provider_task.project.catalog_id != capture.catalog.catalog_id ||
			draft.provider_task.project.catalog_digest != capture.catalog.catalog_digest)
			return unexpected(task_error("provider_task.project", "capture-mismatch"));
		if (auto valid = validate_provider_requirement(draft.provider, draft.provider_task); !valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = validate_publication_requirement(draft.publication, draft.capture); !valid)
			return unexpected(std::move(valid.error()));
		if (draft.partitions.empty() ||
			draft.partitions.size() != draft.provider_task.outputs.size())
			return unexpected(task_error("partitions", "output-census-mismatch"));
		std::ranges::sort(draft.partitions,
						  {},
						  [](const auto& value) -> const std::string&
						  {
							  return value.relation_descriptor_id;
						  });

		std::map<std::string, const relation_descriptor*, std::less<>> outputs;
		for (const auto& descriptor : draft.provider_task.outputs)
			outputs.emplace(descriptor.id, &descriptor);
		std::set<std::string, std::less<>> partition_ids;
		std::set<std::string, std::less<>> relation_ids;
		std::vector<incremental::partition_candidate> candidates;
		candidates.reserve(draft.partitions.size());
		for (const auto& partition : draft.partitions)
		{
			const auto descriptor = outputs.find(partition.relation_descriptor_id);
			if (descriptor == outputs.end() ||
				!relation_ids.insert(partition.relation_descriptor_id).second ||
				!partition_ids.insert(partition.candidate.current.partition_id).second)
				return unexpected(task_error("partitions", "duplicate-or-unrequested"));
			if (partition.candidate.current.input.relation_descriptor_digest !=
				descriptor->second->descriptor_digest)
				return unexpected(task_error("partitions.input", "descriptor-mismatch"));
			if (partition.candidate.current.input.source_digest != capture.source.content_digest ||
				partition.candidate.current.input.invocation_digest !=
					capture.invocation.effective_invocation_digest ||
				partition.candidate.current.input.toolchain_digest != capture.toolchain_digest ||
				partition.candidate.current.input.environment_digest !=
					capture.invocation.environment_digest ||
				partition.candidate.current.input.provider_binary_digest !=
					draft.provider.provider_binary_digest ||
				partition.candidate.current.input.provider_semantics_digest !=
					draft.provider.provider_semantics_digest ||
				partition.candidate.current.input.registry_digest !=
					draft.publication.snapshot.series.relation_registry_digest ||
				partition.candidate.current.input.interpretation_policy_digest !=
					draft.publication.snapshot.series.interpretation_policy_digest)
				return unexpected(task_error("partitions.input", "authority-mismatch"));
			candidates.push_back(partition.candidate);
		}
		auto plan = incremental::make_materialization_plan(candidates);
		if (!plan)
			return unexpected(std::move(plan.error()));
		auto binding = task_binding_digest(draft, *plan);
		if (!binding)
			return unexpected(std::move(binding.error()));
		const std::array task_fields{
			canonical_value::from_string(*binding),
			canonical_value::from_string(draft.provider_task.task_id),
			canonical_value::from_string(plan->plan_digest),
		};
		auto task_id = canonical_identity_digest("materialization-task", task_fields);
		if (!task_id)
			return unexpected(std::move(task_id.error()));
		return validated_materialization_task{
			std::move(draft), std::move(*plan), std::move(*binding), std::move(*task_id)};
	}

	result<validated_materialization_result>
	validate_materialization_result(const relation_engine& engine,
									const validated_materialization_task& task,
									materialization_result_draft draft)
	{
		if (draft.task_id != task.id() || draft.task_input_digest != task.input_binding_digest())
			return unexpected(result_error("task", "binding-mismatch"));
		if (draft.runtime)
			if (auto valid = validate_runtime(*draft.runtime, task); !valid)
				return unexpected(std::move(valid.error()));

		const bool adoptable = draft.terminal == materialization_terminal::complete ||
			draft.terminal == materialization_terminal::partial;
		if (adoptable != draft.runtime.has_value())
			return unexpected(result_error("runtime", "terminal-mismatch"));
		if (!adoptable &&
			(!draft.partitions.empty() || !draft.closures.empty() || !draft.conflicts.empty() ||
			 !draft.differential_disagreements.empty()))
			return unexpected(result_error("terminal", "publication-data-forbidden"));
		if (draft.terminal == materialization_terminal::partial && draft.partitions.empty() &&
			draft.unresolved.empty())
			return unexpected(result_error("terminal", "empty-partial"));
		if (draft.terminal == materialization_terminal::complete && !draft.unresolved.empty())
			return unexpected(result_error("terminal", "complete-with-unresolved"));
		if (draft.terminal == materialization_terminal::partial && !draft.closures.empty())
			return unexpected(result_error("closures", "partial-cannot-certify"));

		std::map<std::string, const materialization_partition_request*, std::less<>> requested;
		for (const auto& partition : task.value().partitions)
			requested.emplace(partition.relation_descriptor_id, &partition);
		std::map<std::string, partition_manifest, std::less<>> manifests;
		std::set<std::string, std::less<>> result_relations;
		std::vector<validated_materialization_partition> partitions;
		partitions.reserve(draft.partitions.size());
		for (auto& partition : draft.partitions)
		{
			auto manifest = make_partition_manifest(engine, partition);
			if (!manifest)
				return unexpected(result_error("partition", manifest.error().code));
			const auto expected = requested.find(manifest->relation_descriptor_id);
			if (expected == requested.end() ||
				!result_relations.insert(manifest->relation_descriptor_id).second)
				return unexpected(result_error(
					"partition", "unrequested-or-duplicate:" + manifest->relation_descriptor_id));
			if (!manifests.emplace(manifest->partition_id, *manifest).second)
				return unexpected(result_error("partition", "duplicate"));
			auto binding = binding_for(*manifest, partition);
			partitions.push_back({std::move(partition), *manifest, std::move(binding)});
		}
		std::ranges::sort(partitions,
						  {},
						  [](const auto& value) -> const std::string&
						  {
							  return value.manifest.partition_id;
						  });
		if (draft.terminal == materialization_terminal::complete &&
			(partitions.size() != requested.size() ||
			 !std::ranges::all_of(partitions,
								  [](const auto& partition)
								  {
									  return partition.manifest.complete;
								  })))
			return unexpected(result_error("terminal", "incomplete-partition-census"));

		std::ranges::sort(draft.closures,
						  {},
						  [](const auto& value) -> const std::string&
						  {
							  return value.subject_partition_id;
						  });
		std::set<std::string, std::less<>> closure_partitions;
		for (const auto& closure : draft.closures)
		{
			if (!closure_partitions.insert(closure.subject_partition_id).second)
				return unexpected(result_error("closure", "duplicate-partition"));
			if (auto valid = validate_closure(closure, manifests); !valid)
				return unexpected(std::move(valid.error()));
		}

		provider::coverage_builder coverage_builder;
		for (const auto& unit : draft.coverage)
			coverage_builder.request(unit.kind, unit.id);
		for (const auto& unit : draft.coverage)
			if (auto classified = coverage_builder.classify(unit); !classified)
				return unexpected(result_error("coverage", classified.error().code));
		auto canonical_coverage = std::move(coverage_builder).finish();
		if (!canonical_coverage)
			return unexpected(result_error("coverage", canonical_coverage.error().code));
		draft.coverage = std::move(*canonical_coverage);
		provider::unresolved_builder unresolved_builder;
		for (const auto& item : draft.unresolved)
			unresolved_builder.add(item);
		auto canonical_unresolved = std::move(unresolved_builder).finish();
		if (!canonical_unresolved)
			return unexpected(result_error("unresolved", canonical_unresolved.error().code));
		draft.unresolved = std::move(*canonical_unresolved);
		std::ranges::sort(draft.conflicts,
						  {},
						  [](const auto& value)
						  {
							  return std::tie(value.relation,
											  value.semantic_key,
											  value.interpretation,
											  value.overlap_fragments,
											  value.assertions,
											  value.contents);
						  });
		std::ranges::sort(draft.differential_disagreements,
						  {},
						  [](const auto& value)
						  {
							  return std::tie(value.relation,
											  value.semantic_key,
											  value.left_interpretation,
											  value.right_interpretation,
											  value.left_content,
											  value.right_content,
											  value.overlap_fragments);
						  });

		auto digest = result_identity(draft, partitions);
		if (!digest)
			return unexpected(std::move(digest.error()));
		return validated_materialization_result{draft.terminal,
												std::move(draft.task_id),
												std::move(draft.task_input_digest),
												std::move(draft.runtime),
												std::move(partitions),
												std::move(draft.closures),
												std::move(draft.coverage),
												std::move(draft.unresolved),
												std::move(draft.conflicts),
												std::move(draft.differential_disagreements),
												std::move(*digest)};
	}
} // namespace cxxlens::sdk::detail
