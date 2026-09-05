#pragma once

/**
 * @file application_materialization_adoption_internal.hpp
 * @brief Host-owned preparation and atomic adoption of sealed application-analysis results.
 */

#include <span>
#include <string>

#include "materialization_task_internal.hpp"
#include "materialization_writer_internal.hpp"
#include "provider_validation_internal.hpp"

namespace cxxlens::sdk::detail
{
	struct prepared_application_materialization
	{
		validated_materialization_publication_source source;
		std::vector<provider::coverage_unit> coverage;
		std::vector<provider::unresolved_item> unresolved;
		std::vector<claim_conflict> conflicts;
		std::vector<differential_disagreement> differential_disagreements;
		materialization_runtime_binding runtime;
		std::string replay_plan_digest;
	};

	struct application_materialization_adoption
	{
		materialization_store_publication publication;
		std::vector<provider::coverage_unit> coverage;
		std::vector<provider::unresolved_item> unresolved;
		std::vector<claim_conflict> conflicts;
		std::vector<differential_disagreement> differential_disagreements;
		std::string provider_input_digest;
		std::string runtime_receipt_digest;
		std::string replay_plan_digest;
	};

	/**
	 * Convert only an immutable Protocol-v2 seal into a validated prepublication source.
	 * Diagnostic frames and provider prose are not accepted, and no Store effect occurs here.
	 */
	[[nodiscard]] result<prepared_application_materialization>
	prepare_sealed_application_materialization(
		const relation_engine& engine,
		const validated_materialization_task& task,
		const provider::detail::sealed_provider_transcript& sealed,
		materialization_runtime_binding runtime,
		const std::string& source_receipt_digest,
		std::string replay_plan_digest,
		std::string_view observation_technique,
		std::span<const partition_draft> host_partitions = {});

	/** Publish all fully validated unit sources through one Store transaction. */
	[[nodiscard]] result<application_materialization_adoption>
	publish_prepared_application_materializations(
		const relation_engine& engine,
		snapshot_store& store,
		std::vector<prepared_application_materialization> prepared);
} // namespace cxxlens::sdk::detail
