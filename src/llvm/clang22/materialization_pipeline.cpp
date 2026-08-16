#include "materialization_pipeline.hpp"

#include <cstdint>
#include <limits>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "materialization_incremental_receipt.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::result<void>
		validate_bounded_source_binding(const materialization_claim_request_binding& expected,
										const materialization_bounded_claim_source& source)
		{
			if (source.request_binding() != expected)
				return sdk::unexpected(sdk::error{"materialization.task-binding-mismatch",
												  "store.source",
												  "request-identity-or-task-census"});
			return {};
		}
	} // namespace

	sdk::result<prepared_store_transaction>
	make_materialization_store_transaction(const validated_materialization_request& request,
										   const sealed_materialization_claims& claims)
	{
		if (request.tasks.empty())
			return sdk::unexpected(
				sdk::error{"materialization.task-binding-mismatch", "tasks", "empty"});
		if (auto valid = validate_materialization_legacy_request_binding(request); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (auto valid = validate_materialization_claim_request_binding(request, claims); !valid)
			return sdk::unexpected(std::move(valid.error()));
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
		if (auto valid = validate_materialization_legacy_request_binding(request); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (auto valid = validate_materialization_claim_request_binding(request, claims); !valid)
			return sdk::unexpected(std::move(valid.error()));
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
		auto expected_binding = make_materialization_claim_request_binding(request);
		if (!expected_binding)
			return sdk::unexpected(std::move(expected_binding.error()));
		streaming_prepared_store_transaction result;
		result.draft = {request.publication.selector,
						{1U, 0U, 0U},
						catalog.catalog_digest,
						request.publication.expected_parent_publication};
		result.external_authority.expected_request_binding = std::move(*expected_binding);
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
		if (auto valid = validate_materialization_legacy_request_binding(request); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (auto valid = request.catalog.validate(); !valid)
			return sdk::unexpected(sdk::error{
				"materialization.identity-mismatch", "project.catalog", valid.error().code});
		if (!source.sealed() || source.partition_count() == 0U)
			return sdk::unexpected(
				sdk::error{"materialization.claim-invalid", "store.source", "unsealed-or-empty"});
		if (request.tasks.size() > std::numeric_limits<std::uint64_t>::max())
			return sdk::unexpected(
				sdk::error{"materialization.task-binding-mismatch", "tasks", "count-overflow"});
		auto expected_binding = make_materialization_claim_request_binding(request);
		if (!expected_binding)
			return sdk::unexpected(std::move(expected_binding.error()));
		if (auto valid = validate_bounded_source_binding(*expected_binding, source); !valid)
			return sdk::unexpected(std::move(valid.error()));
		streaming_prepared_store_transaction result;
		result.draft = {request.publication.selector,
						{1U, 0U, 0U},
						request.catalog.catalog_digest,
						request.publication.expected_parent_publication};
		result.external_authority.expected_request_binding = *expected_binding;
		return result;
	}

	sdk::result<streaming_prepared_store_transaction>
	make_materialization_streaming_store_transaction(
		const materialization_v2_1_claim_authority& authority,
		const materialization_bounded_claim_source& source)
	{
		if (authority.task_count() == 0U || authority.catalog() == nullptr ||
			authority.request() == nullptr)
			return sdk::unexpected(
				sdk::error{"materialization.task-binding-mismatch", "request", "unbound"});
		if (auto valid = authority.catalog()->validate(); !valid)
			return sdk::unexpected(sdk::error{
				"materialization.identity-mismatch", "project.catalog", valid.error().code});
		if (!source.sealed() || source.partition_count() == 0U)
			return sdk::unexpected(
				sdk::error{"materialization.claim-invalid", "store.source", "unsealed-or-empty"});
		auto expected_binding = make_materialization_claim_request_binding(authority);
		if (!expected_binding)
			return sdk::unexpected(std::move(expected_binding.error()));
		if (auto valid = validate_bounded_source_binding(*expected_binding, source); !valid)
			return sdk::unexpected(std::move(valid.error()));
		const auto& publication = authority.request()->request().publication();
		streaming_prepared_store_transaction result;
		result.draft = {publication.selector,
						{1U, 0U, 0U},
						authority.catalog()->catalog_digest,
						publication.expected_parent_publication};
		result.external_authority.expected_request_binding = std::move(*expected_binding);
		return result;
	}
} // namespace cxxlens::detail::clang22::materialization
