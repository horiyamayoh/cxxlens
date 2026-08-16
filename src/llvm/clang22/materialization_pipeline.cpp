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
		/** Rebind the legacy request's mutable projections to its retained document authority. */
		[[nodiscard]] sdk::result<void>
		validate_legacy_request_binding(const validated_materialization_request& request)
		{
			auto rebound = validate_materialization_request(request.document);
			if (!rebound)
				return sdk::unexpected(sdk::error{"materialization.task-binding-mismatch",
												  "request",
												  "request-identity-or-task-census"});
			const auto& expected = *rebound;
			if (request.catalog.catalog_id != expected.catalog.catalog_id ||
				request.catalog.catalog_digest != expected.catalog.catalog_digest ||
				request.catalog.logical_root != expected.catalog.logical_root ||
				request.catalog.environment_digest != expected.catalog.environment_digest ||
				request.catalog.compile_units != expected.catalog.compile_units ||
				request.engine.registry_digest() != expected.engine.registry_digest() ||
				request.engine.generation() != expected.engine.generation() ||
				request.engine.descriptors() != expected.engine.descriptors() ||
				request.output_descriptors != expected.output_descriptors ||
				request.publication.backend != expected.publication.backend ||
				request.publication.selector != expected.publication.selector ||
				request.publication.series_id != expected.publication.series_id ||
				request.publication.genesis != expected.publication.genesis ||
				request.publication.expected_parent_publication !=
					expected.publication.expected_parent_publication ||
				request.publication.sqlite_path != expected.publication.sqlite_path ||
				request.tasks.size() != expected.tasks.size())
				return sdk::unexpected(sdk::error{"materialization.task-binding-mismatch",
												  "request",
												  "request-identity-or-task-census"});

			for (std::size_t index{}; index < request.tasks.size(); ++index)
			{
				const auto& actual = request.tasks[index];
				const auto& bound = expected.tasks[index];
				auto actual_payload = encode_task_input(actual.worker_input);
				auto bound_payload = encode_task_input(bound.worker_input);
				if (!actual_payload || !bound_payload || *actual_payload != *bound_payload ||
					actual.provider_task_id != bound.provider_task_id ||
					actual.provider_execution_id != bound.provider_execution_id ||
					actual.task_input_digest != bound.task_input_digest ||
					actual.sandbox.minimum != bound.sandbox.minimum ||
					actual.sandbox.policy_digest != bound.sandbox.policy_digest ||
					actual.worker_payload != bound.worker_payload ||
					actual.source_receipt != bound.source_receipt)
					return sdk::unexpected(sdk::error{"materialization.task-binding-mismatch",
													  "request",
													  "request-identity-or-task-census"});
			}
			return {};
		}

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
		if (auto valid = validate_legacy_request_binding(request); !valid)
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
		if (auto valid = validate_legacy_request_binding(request); !valid)
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
		if (auto valid = request.catalog.validate(); !valid)
			return sdk::unexpected(sdk::error{
				"materialization.identity-mismatch", "project.catalog", valid.error().code});
		if (!source.sealed() || source.partition_count() == 0U)
			return sdk::unexpected(
				sdk::error{"materialization.claim-invalid", "store.source", "unsealed-or-empty"});
		if (request.tasks.size() > std::numeric_limits<std::uint64_t>::max())
			return sdk::unexpected(
				sdk::error{"materialization.task-binding-mismatch", "tasks", "count-overflow"});
		if (auto valid = validate_legacy_request_binding(request); !valid)
			return sdk::unexpected(std::move(valid.error()));
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
