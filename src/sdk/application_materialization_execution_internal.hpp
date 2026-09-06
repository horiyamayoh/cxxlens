#pragma once

/**
 * @file application_materialization_execution_internal.hpp
 * @brief Pre-effect application materialization plan bound to explicit compiler provider authority.
 */

#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "application_analysis_internal.hpp"
#include "compiler_replay_input_internal.hpp"
#include "materialization_task_internal.hpp"

namespace cxxlens::sdk::detail
{
	/** Physical transport authorized to satisfy a prevalidated materialization plan. */
	enum class application_materialization_execution_transport : std::uint8_t
	{
		process,
		detached
	};

	/** One compile unit whose payload, generic task, and process request share exact identities. */
	struct application_materialization_execution_unit
	{
		std::string replay_plan_digest;
		std::string observation_technique;
		validated_compiler_replay_input provider_input;
		validated_build_capture capture;
		validated_materialization_task task;
		provider::process_task_request process;
		std::vector<partition_draft> host_partitions;
	};

	/** Immutable pre-effect census for one public materialize() call. */
	struct application_materialization_execution_plan
	{
		std::string materialization_request_id;
		std::string analysis_recipe_digest;
		std::string output_plan_digest;
		application_materialization_execution_transport transport{
			application_materialization_execution_transport::process};
		std::vector<application_materialization_execution_unit> units;
	};

	/**
	 * Bind every imported compile unit to the requested relations, selected executable, generic
	 * incremental task, and one publication authority. No process or Store effect occurs here.
	 */
	[[nodiscard]] result<application_materialization_execution_plan>
	make_application_materialization_execution_plan(
		const imported_project::implementation& project,
		const relation_engine& engine,
		snapshot_draft publication,
		std::span<const std::string> relation_descriptor_ids,
		std::string interpretation,
		const provider::provider_selection& selection,
		provider::execution_budget budget,
		const std::stop_token& cancellation,
		import_limits limits = {},
		application_materialization_execution_transport transport =
			application_materialization_execution_transport::process);
} // namespace cxxlens::sdk::detail
