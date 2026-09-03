#pragma once

/**
 * @file application_materialization_adoption_internal.hpp
 * @brief Host-owned adoption of one sealed application-analysis provider result.
 */

#include <span>
#include <string>

#include "materialization_task_internal.hpp"
#include "materialization_writer_internal.hpp"
#include "provider_validation_internal.hpp"

namespace cxxlens::sdk::detail
{
	struct application_materialization_adoption
	{
		materialization_store_publication publication;
		std::vector<provider::coverage_unit> coverage;
		std::vector<provider::unresolved_item> unresolved;
		std::vector<claim_conflict> conflicts;
		std::vector<differential_disagreement> differential_disagreements;
		materialization_runtime_binding runtime;
	};

	/**
	 * Convert only an immutable Protocol-v2 seal into claims, validate the generic result, and
	 * publish through the sole SDK writer. Diagnostic frames and provider prose are not accepted.
	 */
	[[nodiscard]] result<application_materialization_adoption>
	adopt_sealed_application_materialization(
		const relation_engine& engine,
		snapshot_store& store,
		const validated_materialization_task& task,
		const provider::detail::sealed_provider_transcript& sealed,
		materialization_runtime_binding runtime,
		std::string source_receipt_digest,
		std::string replay_plan_digest,
		std::span<const partition_draft> host_partitions = {});
} // namespace cxxlens::sdk::detail
