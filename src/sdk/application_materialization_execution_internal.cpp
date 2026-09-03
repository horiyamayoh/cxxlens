#include "application_materialization_execution_internal.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <set>
#include <utility>

#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/source_file.hpp>

#include "gcc_build_capture_adapter_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error execution_error(std::string field, std::string detail)
		{
			return {
				"application-analysis.execution-plan-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] result<std::string> identity(std::string_view domain,
												   std::vector<canonical_value> fields)
		{
			return canonical_identity_digest(domain, fields);
		}

		[[nodiscard]] canonical_value strings(const std::span<const std::string> values)
		{
			std::vector<canonical_value> fields;
			fields.reserve(values.size());
			for (const auto& value : values)
				fields.push_back(canonical_value::from_string(value));
			return canonical_value::from_tuple(std::move(fields));
		}

		[[nodiscard]] bool contains(const std::span<const std::string> values,
									const std::string_view expected)
		{
			return std::ranges::find(values, expected) != values.end();
		}

		[[nodiscard]] detached_cell text_cell(value_type type, std::string value)
		{
			return {std::move(type), cell_state::present, scalar_value{std::move(value)}, {}};
		}

		[[nodiscard]] result<detached_row> project_row(const validated_build_capture& capture)
		{
			const auto& descriptor = build::relations::project::descriptor();
			detached_row row{
				descriptor.id,
				{{"build.project.v1.project",
				  detached_cell::typed("project_id", "identity:pending")},
				 {"build.project.v1.catalog",
				  detached_cell::typed("catalog_id", capture.value().catalog.catalog_id)},
				 {"build.project.v1.catalog_digest",
				  text_cell({scalar_kind::digest, {}, false},
							capture.value().catalog.catalog_digest)},
				 {"build.project.v1.logical_root",
				  detached_cell::typed("logical_path_id", capture.value().catalog.logical_root)},
				 {"build.project.v1.environment_digest",
				  text_cell({scalar_kind::digest, {}, false},
							capture.value().catalog.environment_digest)}}};
			auto identity = derive_domain_identity(descriptor, row);
			if (!identity)
				return unexpected(std::move(identity.error()));
			row.cells.at("build.project.v1.project") =
				detached_cell::typed("project_id", std::move(*identity));
			if (auto valid = validate_domain_identity(descriptor, row); !valid)
				return unexpected(std::move(valid.error()));
			return row;
		}

		[[nodiscard]] result<detached_row> source_file_row(const validated_build_capture& capture,
														   const std::string_view project_id)
		{
			const auto& source = capture.value().source;
			const auto& descriptor = source::relations::file::descriptor();
			detached_row row{
				descriptor.id,
				{{"source.file.v1.snapshot",
				  detached_cell::typed("source_snapshot_id", source.source_snapshot_id)},
				 {"source.file.v1.file", detached_cell::typed("file_id", source.file_id)},
				 {"source.file.v1.project",
				  detached_cell::typed("project_id", std::string{project_id})},
				 {"source.file.v1.logical_path",
				  detached_cell::typed("logical_path_id", source.logical_path)},
				 {"source.file.v1.content",
				  text_cell({scalar_kind::digest, {}, false}, source.content_digest)},
				 {"source.file.v1.size", detached_cell::unsigned_integer(source.size_bytes)},
				 {"source.file.v1.encoding",
				  text_cell({scalar_kind::open_symbol, "source.encoding/1", false},
							source.encoding)},
				 {"source.file.v1.line_index",
				  detached_cell::typed("line_index_id", source.line_index_id)},
				 {"source.file.v1.read_only", detached_cell::boolean(source.read_only)}}};
			if (auto valid = validate_domain_identity(descriptor, row); !valid)
				return unexpected(std::move(valid.error()));
			return row;
		}

		[[nodiscard]] result<std::vector<partition_draft>>
		host_capture_partitions(const relation_engine& engine,
								const validated_build_capture& capture,
								const validated_materialization_task& task,
								const std::string_view replay_plan_digest)
		{
			auto project = project_row(capture);
			if (!project)
				return unexpected(std::move(project.error()));
			const auto* project_identity =
				std::get_if<std::string>(&*project->cells.at("build.project.v1.project").value);
			if (project_identity == nullptr)
				return unexpected(execution_error("host.project", "identity-missing"));
			auto source = source_file_row(capture, *project_identity);
			if (!source)
				return unexpected(std::move(source.error()));

			const bool partial = !capture.gaps().empty();
			auto assumption =
				identity("application-analysis-host-capture-assumptions",
						 {canonical_value::from_string(std::string{capture.semantic_identity()}),
						  canonical_value::from_string(std::string{replay_plan_digest})});
			if (!assumption)
				return unexpected(std::move(assumption.error()));
			const auto assumption_id = "assumption-set:" + *assumption;
			const auto capture_identity = capture.semantic_identity();
			const auto host_semantics = content_digest(
				std::as_bytes(std::span{capture_identity.data(), capture_identity.size()}));
			const direct_claim_basis basis{task.value().provider_input_digest};
			auto basis_digest = claim_input_basis_digest(basis);
			if (!basis_digest)
				return unexpected(std::move(basis_digest.error()));
			claim_batch claims;
			for (auto& row : std::array{std::move(*project), std::move(*source)})
			{
				observation value{std::move(row),
								  {task.value().publication.snapshot.series.condition_universe_id,
								   {task.value().provider_task.condition}},
								  task.value().provider_task.interpretation,
								  {"cxxlens.application-analysis.capture@1.0.0", host_semantics},
								  basis,
								  std::string{capture.semantic_identity()},
								  {partial ? "under_approximation" : "exact",
								   capture.value().compile_unit_id,
								   assumption_id,
								   {"generic_build_capture", "validated_capture_bundle"}}};
				if (auto added = claims.add_observation(engine, std::move(value)); !added)
					return unexpected(std::move(added.error()));
			}
			auto committed = std::move(claims).commit(engine);
			if (!committed)
				return unexpected(std::move(committed.error()));

			std::vector<partition_draft> partitions;
			for (const auto& descriptor_id : {build::relations::project::descriptor().id,
											  source::relations::file::descriptor().id})
			{
				partition_draft partition;
				partition.relation_descriptor_id = descriptor_id;
				partition.scope = capture.value().project_id;
				partition.condition = {
					task.value().publication.snapshot.series.condition_universe_id,
					{task.value().provider_task.condition}};
				partition.interpretation = task.value().provider_task.interpretation;
				partition.producer_semantics = host_semantics;
				partition.producer_input_basis_digest = *basis_digest;
				partition.precision_profile = partial ? "under_approximation" : "exact";
				partition.assumption_set_id = assumption_id;
				partition.coverage = {{"relation", descriptor_id, "covered", ""}};
				for (const auto& claim : committed->claims)
					if (claim.descriptor == descriptor_id)
						partition.claims.push_back(claim);
				for (const auto& unresolved : committed->unresolved)
					if (unresolved.source_relation == descriptor_id)
						partition.unresolved.push_back(unresolved);
				partitions.push_back(std::move(partition));
			}
			return partitions;
		}
	} // namespace

	result<application_materialization_execution_plan>
	make_gcc_application_materialization_execution_plan(
		const imported_project::implementation& project,
		const relation_engine& engine,
		snapshot_draft publication,
		std::span<const std::string> relation_descriptor_ids,
		std::string interpretation,
		provider::provider_selection selection,
		const provider::execution_budget budget,
		const std::stop_token cancellation,
		const import_limits limits)
	{
		if (auto valid = selection.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = budget.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = limits.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (!project.capture || project.replay_plans.empty() ||
			project.replay_plans.size() != project.replay_plan_values.size() ||
			relation_descriptor_ids.empty() || interpretation.empty())
			return unexpected(execution_error("request", "omitted-authority"));
		if (publication.catalog_semantic_digest != project.catalog.catalog_digest)
			return unexpected(execution_error("publication.catalog", "project-mismatch"));

		const auto& candidate = selection.selected_candidate();
		const auto& manifest = candidate.description;
		if (candidate.executable_argv.empty() || candidate.executable_argv.front().empty() ||
			candidate.executable_argv.front().contains('\0') ||
			candidate.executable_argv.front().front() != '/')
			return unexpected(execution_error("provider.executable", "absolute-path-required"));
		if (!contains(manifest.interpretation_domains, interpretation))
			return unexpected(execution_error("interpretation", "provider-incompatible"));
		if (manifest.task_input_stage != "observation" ||
			manifest.task_output_stage != "observation")
			return unexpected(execution_error("provider.stage", "observation-required"));

		std::vector<std::string> relation_ids{relation_descriptor_ids.begin(),
											  relation_descriptor_ids.end()};
		std::ranges::sort(relation_ids);
		if (std::ranges::adjacent_find(relation_ids) != relation_ids.end())
			return unexpected(execution_error("relations", "duplicate"));
		std::vector<relation_descriptor> descriptors;
		descriptors.reserve(relation_ids.size());
		for (const auto& id : relation_ids)
		{
			if (!contains(manifest.offered_relations, id))
				return unexpected(execution_error(id, "provider-relation-unavailable"));
			auto descriptor = engine.require_id(id);
			if (!descriptor)
				return unexpected(execution_error(id, "registry-relation-unavailable"));
			descriptors.push_back(descriptor->descriptor());
		}

		auto qualifications = candidate.certified_qualifications;
		std::ranges::sort(qualifications);
		if (qualifications.empty())
			return unexpected(execution_error("provider.qualification", "empty"));

		auto selection_digest = semantic_digest(
			"cxxlens.application-analysis-provider-selection.v1", selection.canonical_form());
		auto recipe_digest =
			identity("application-analysis-recipe",
					 {strings(relation_ids), canonical_value::from_string(interpretation)});
		std::vector<std::string> replay_digests;
		replay_digests.reserve(project.replay_plans.size());
		for (const auto& plan : project.replay_plans)
			replay_digests.emplace_back(plan.digest());
		auto output_plan_digest = identity("application-analysis-output-plan",
										   {canonical_value::from_string(project.id),
											strings(relation_ids),
											strings(replay_digests),
											canonical_value::from_string(interpretation)});
		if (!selection_digest || !recipe_digest || !output_plan_digest)
			return unexpected(execution_error("identity", "derivation-failed"));
		auto request_identity = identity("application-analysis-materialization-request",
										 {canonical_value::from_string(project.id),
										  canonical_value::from_string(publication.series.id()),
										  canonical_value::from_string(*selection_digest),
										  canonical_value::from_string(*output_plan_digest)});
		if (!request_identity)
			return unexpected(std::move(request_identity.error()));
		const auto request_id = "materialization-request:" + *request_identity;

		application_materialization_execution_plan output{
			request_id, *recipe_digest, *output_plan_digest, {}};
		output.units.reserve(project.replay_plans.size());
		for (const auto& plan_value : project.replay_plan_values)
		{
			if (!plan_value)
				return unexpected(execution_error("replay_plan", "missing-immutable-value"));
			const auto& plan = *plan_value;
			auto provider_input =
				make_gcc_replay_input(project, plan, relation_ids, interpretation, limits);
			if (!provider_input)
				return unexpected(std::move(provider_input.error()));
			auto capture = make_gcc_build_capture(project, plan);
			if (!capture)
				return unexpected(std::move(capture.error()));

			auto condition_identity =
				identity("application-analysis-condition",
						 {canonical_value::from_string(publication.series.condition_universe_id),
						  canonical_value::from_string(capture->value().build_variant_id)});
			auto assumption_identity = identity("application-analysis-replay-assumptions",
												{canonical_value::from_string(plan.digest)});
			if (!condition_identity || !assumption_identity)
				return unexpected(execution_error("task.identity", "derivation-failed"));

			materialization_publication_requirement publication_requirement{
				publication, *recipe_digest, *output_plan_digest, publication.series.id()};
			generic_materialization_task_request task_request{
				request_id,
				std::string{provider_input->input_digest()},
				*capture,
				{manifest.provider_id,
				 manifest.provider_version,
				 manifest.provider_semantic_contract_digest,
				 descriptors,
				 {},
				 {interpretation},
				 manifest.task_input_stage,
				 manifest.task_output_stage},
				descriptors,
				publication.series.condition_universe_id,
				"condition:" + *condition_identity,
				interpretation,
				{"clang23-gcc-replay"},
				"clang23-gcc-replay-output-normalizer.v1",
				"assumption-set:" + *assumption_identity,
				plan.unresolved.empty() ? "exact" : "under_approximation",
				{"compile-unit", plan.compile_unit_id, "covered"},
				{},
				{manifest.provider_id,
				 manifest.provider_version,
				 manifest.provider_binary_digest,
				 manifest.provider_semantic_contract_digest,
				 qualifications.front(),
				 publication.series.trust_policy_digest,
				 selection.authority_request().sandbox,
				 budget},
				std::move(publication_requirement)};
			auto task = make_generic_materialization_task(engine, std::move(task_request));
			if (!task)
				return unexpected(std::move(task.error()));
			std::vector<partition_draft> host_partitions;
			const bool requests_project =
				contains(relation_ids, build::relations::project::descriptor().id);
			const bool requests_source_file =
				contains(relation_ids, source::relations::file::descriptor().id);
			if (requests_project != requests_source_file)
				return unexpected(execution_error(
					"host-relations", "build.project-and-source.file-must-be-requested-together"));
			if (requests_project)
			{
				auto built = host_capture_partitions(engine, *capture, *task, plan.digest);
				if (!built)
					return unexpected(std::move(built.error()));
				host_partitions = std::move(*built);
			}

			provider::process_task_request process{
				selection,
				descriptors,
				task->value().provider_task.task_id,
				{provider_input->bytes().begin(), provider_input->bytes().end()},
				std::string{provider_input->input_digest()},
				capture->value().invocation.effective_invocation_digest,
				capture->value().toolchain_digest,
				capture->value().invocation.environment_digest,
				selection.authority_request().sandbox,
				budget,
				{},
				{67108864U, 65536U},
				cancellation,
				{}};
			output.units.push_back({plan.digest,
									std::move(*provider_input),
									std::move(*capture),
									std::move(*task),
									std::move(process),
									std::move(host_partitions)});
		}
		return output;
	}
} // namespace cxxlens::sdk::detail
