#include "materialization_pipeline.hpp"

#include <set>
#include <string>

namespace cxxlens::detail::clang22::materialization
{
	sdk::result<prepared_store_transaction>
	make_materialization_store_transaction(const validated_materialization_request& request,
										   const sealed_materialization_claims& claims)
	{
		if (request.tasks.empty())
			return sdk::unexpected(
				sdk::error{"materialization.task-binding-mismatch", "tasks", "empty"});
		const auto& catalog = request.tasks.front().worker_input.project_catalog;
		if (auto valid = catalog.validate(); !valid)
			return sdk::unexpected(sdk::error{
				"materialization.identity-mismatch", "project.catalog", valid.error().code});
		for (const auto& task : request.tasks)
			if (task.worker_input.project_catalog.catalog_id != catalog.catalog_id ||
				task.worker_input.project_catalog.catalog_digest != catalog.catalog_digest)
				return sdk::unexpected(sdk::error{"materialization.task-binding-mismatch",
												  "tasks.project_catalog",
												  "not-shared"});

		prepared_store_transaction result;
		result.draft = {request.publication.selector,
						{1U, 0U, 0U},
						catalog.catalog_digest,
						request.publication.expected_parent_publication};
		result.partitions.reserve(claims.partitions().size());
		std::set<std::string, std::less<>> partition_ids;
		for (const auto& partition : claims.partitions())
		{
			if (!partition_ids.insert(partition.manifest.partition_id).second ||
				partition.draft.relation_descriptor_id != partition.manifest.relation_descriptor_id)
				return sdk::unexpected(sdk::error{"materialization.claim-invalid",
												  "store.partitions",
												  "duplicate-or-descriptor-mismatch"});
			result.partitions.push_back(partition.draft);
		}
		if (result.partitions.empty())
			return sdk::unexpected(
				sdk::error{"materialization.claim-invalid", "store.partitions", "empty"});
		return result;
	}
} // namespace cxxlens::detail::clang22::materialization
