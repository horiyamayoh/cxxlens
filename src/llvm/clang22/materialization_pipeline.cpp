#include "materialization_pipeline.hpp"

#include <new>
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
		const auto& catalog = request.catalog;
		if (auto valid = catalog.validate(); !valid)
			return sdk::unexpected(sdk::error{
				"materialization.identity-mismatch", "project.catalog", valid.error().code});

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

	sdk::result<void> materialization_claim_partition_replay_source::replay(
		const materialization_store_partition_consumer& consumer)
	{
		if (claims_ == nullptr || !consumer)
			return sdk::unexpected(
				sdk::error{"materialization.claim-invalid", "store.source", "consumer-or-claims"});
		try
		{
			for (const auto& partition : claims_->partitions())
			{
				sdk::partition_draft draft = partition.draft;
				if (auto consumed = consumer(std::move(draft)); !consumed)
					return consumed;
			}
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				sdk::error{"materialization.spool-failure", "store.source", "allocation"});
		}
	}

	sdk::result<streaming_prepared_store_transaction>
	make_materialization_streaming_store_transaction(
		const validated_materialization_request& request,
		const sealed_materialization_claims& claims)
	{
		if (request.tasks.empty())
			return sdk::unexpected(
				sdk::error{"materialization.task-binding-mismatch", "tasks", "empty"});
		const auto& catalog = request.catalog;
		if (auto valid = catalog.validate(); !valid)
			return sdk::unexpected(sdk::error{
				"materialization.identity-mismatch", "project.catalog", valid.error().code});
		std::set<std::string, std::less<>> partition_ids;
		for (const auto& partition : claims.partitions())
		{
			if (!partition_ids.insert(partition.manifest.partition_id).second ||
				partition.draft.relation_descriptor_id != partition.manifest.relation_descriptor_id)
				return sdk::unexpected(sdk::error{"materialization.claim-invalid",
												  "store.partitions",
												  "duplicate-or-descriptor-mismatch"});
		}
		if (partition_ids.empty())
			return sdk::unexpected(
				sdk::error{"materialization.claim-invalid", "store.partitions", "empty"});
		streaming_prepared_store_transaction result;
		result.draft = {request.publication.selector,
						{1U, 0U, 0U},
						catalog.catalog_digest,
						request.publication.expected_parent_publication};
		return result;
	}

	sdk::result<streaming_prepared_store_transaction>
	make_materialization_streaming_store_transaction(
		const validated_materialization_request& request,
		const materialization_bounded_claim_source& source)
	{
		if (request.tasks.empty())
			return sdk::unexpected(
				sdk::error{"materialization.task-binding-mismatch", "tasks", "empty"});
		if (auto valid = request.catalog.validate(); !valid)
			return sdk::unexpected(sdk::error{
				"materialization.identity-mismatch", "project.catalog", valid.error().code});
		if (!source.sealed() || source.partition_count() == 0U)
			return sdk::unexpected(
				sdk::error{"materialization.claim-invalid", "store.source", "unsealed-or-empty"});
		streaming_prepared_store_transaction result;
		result.draft = {request.publication.selector,
						{1U, 0U, 0U},
						request.catalog.catalog_digest,
						request.publication.expected_parent_publication};
		return result;
	}
} // namespace cxxlens::detail::clang22::materialization
