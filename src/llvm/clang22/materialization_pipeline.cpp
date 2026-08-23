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

	[[nodiscard]] sdk::result<void>
	validate_exact_claims(const sealed_materialization_claims& claims)
	{
		const auto& final_batch = claims.final_claim_batch();
		if (!final_batch.unresolved.empty())
			return sdk::unexpected(sdk::error{"materialization.coverage-incomplete",
											  "complete-final-claim-batch",
											  "nonzero-unresolved"});
		if (!final_batch.conflicts.empty() || !final_batch.differential_disagreements.empty())
			return sdk::unexpected(sdk::error{"materialization.claim-invalid",
											  "complete-final-claim-batch",
											  "nonzero-conflict-or-differential"});
		if (claims.partitions().empty())
			return sdk::unexpected(
				sdk::error{"materialization.coverage-incomplete", "partitions", "empty"});
		for (const auto& partition : claims.partitions())
		{
			if (partition.draft.precision_profile != "exact" || !partition.manifest.complete ||
				partition.draft.coverage.empty() || !partition.draft.unresolved.empty())
				return sdk::unexpected(sdk::error{"materialization.coverage-incomplete",
												  "partition-coverage",
												  "empty-incomplete-or-non-exact"});
			for (const auto& coverage : partition.draft.coverage)
			{
				if (auto valid = coverage.validate(); !valid)
					return sdk::unexpected(sdk::error{
						"materialization.coverage-incomplete", "partition-coverage", "invalid"});
				if (coverage.state != "covered")
					return sdk::unexpected(sdk::error{"materialization.coverage-incomplete",
													  "partition-coverage",
													  "non-covered"});
			}
			for (const auto& claim : partition.draft.claims)
			{
				if (auto valid = claim.guarantee.validate(); !valid)
					return sdk::unexpected(
						sdk::error{"materialization.claim-invalid", "guarantee", "invalid"});
				if (claim.guarantee.approximation != partition.draft.precision_profile ||
					claim.guarantee.scope != partition.draft.scope ||
					claim.guarantee.assumptions != partition.draft.assumption_set_id)
					return sdk::unexpected(sdk::error{
						"materialization.coverage-incomplete", "guarantee", "partition-binding"});
			}
		}
		return {};
	}

	[[nodiscard]] sdk::result<void>
	validate_exact_bounded_source(const materialization_bounded_claim_source& source)
	{
		if (!source.exact_publication_ready())
			return sdk::unexpected(sdk::error{
				"materialization.coverage-incomplete", "claims", "non-exact-or-unresolved"});
		return {};
	}
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
		if (auto valid = validate_exact_claims(claims); !valid)
			return sdk::unexpected(std::move(valid.error()));

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
		if (auto valid = validate_exact_claims(claims); !valid)
			return sdk::unexpected(std::move(valid.error()));
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
		if (auto valid = validate_exact_bounded_source(source); !valid)
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
		if (auto valid = validate_exact_bounded_source(source); !valid)
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

	const source_closure_manifest* materialization_v2_2_closure_admission::manifest_for(
		const std::string_view closure_id) const noexcept
	{
		const auto found =
			std::ranges::find(manifests,
							  closure_id,
							  [](const source_closure_manifest& manifest) -> std::string_view
							  {
								  return manifest.closure_id;
							  });
		return found == manifests.end() ? nullptr : &*found;
	}

	sdk::result<materialization_v2_2_closure_admission>
	admit_materialization_request_v2_2_for_execution(
		materialization_request_v2_2 request,
		const std::span<const std::string> advertised_features,
		const std::span<const source_closure_manifest> manifests,
		const materialization_request_v2_2_limits limits)
	{
		if (manifests.empty())
			return sdk::unexpected(sdk::error{
				"materialization.source-closure-invalid", "manifests", "closure-not-validated"});

		// The request validator performs the complete one-to-one closure census, manifest digest
		// binding, task-v4 identity check, and main-member binding.  Do not expose its pre-transfer
		// overload to the production caller: that result is intentionally insufficient for this
		// boundary.
		auto validated = validate_materialization_request_v2_2(
			std::move(request), advertised_features, manifests, limits);
		if (!validated)
			return sdk::unexpected(std::move(validated.error()));

		try
		{
			materialization_v2_2_closure_admission admission{
				std::move(*validated),
				std::vector<source_closure_manifest>{manifests.begin(), manifests.end()}};
			for (const auto& task : admission.request.request.task_extensions)
			{
				const auto* manifest =
					admission.manifest_for(task.source_closure.source_closure_id);
				if (manifest == nullptr)
					return sdk::unexpected(sdk::error{"materialization.source-closure-invalid",
													  "task.source_closure",
													  "manifest-missing"});
			}
			return admission;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				sdk::error{"materialization.spool-failure", "source-closure", "allocation"});
		}
	}

	sdk::result<materialization_v2_2_task_admission>
	accept_materialization_task_v2_2(const materialization_v2_2_closure_admission& admission,
									 const std::uint64_t task_index)
	{
		if (task_index >= admission.request.request.task_extensions.size() ||
			task_index >= admission.request.request.base_tasks.size())
			return sdk::unexpected(
				sdk::error{"materialization.task-binding-mismatch", "task_index", "out-of-range"});

		const auto& task =
			admission.request.request.task_extensions[static_cast<std::size_t>(task_index)];
		const auto* manifest = admission.manifest_for(task.source_closure.source_closure_id);
		if (manifest == nullptr)
			return sdk::unexpected(sdk::error{"materialization.source-closure-invalid",
											  "task.source_closure",
											  "manifest-missing"});
		if (auto valid = bind_provider_task_v4_main_member(
				admission.request.request.base_tasks[static_cast<std::size_t>(task_index)],
				task,
				*manifest);
			!valid)
			return sdk::unexpected(std::move(valid.error()));

		try
		{
			return materialization_v2_2_task_admission{
				task_index,
				task.task_id,
				task.task_v4_digest,
				task.source_closure.source_closure_id,
				task.source_closure.source_closure_digest,
				task.source_closure.manifest_digest,
				materialization_v2_2_task_phase::task_accepted};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				sdk::error{"materialization.spool-failure", "task-acceptance", "allocation"});
		}
	}

	sdk::result<void> begin_materialization_task_v2_2(materialization_v2_2_task_admission& task)
	{
		if (task.phase != materialization_v2_2_task_phase::task_accepted || task.task_id.empty() ||
			task.task_v4_digest.empty() || task.source_closure_id.empty() ||
			task.source_closure_digest.empty() || task.manifest_digest.empty())
			return sdk::unexpected(sdk::error{
				"materialization.task-binding-mismatch", "task-phase", "closure-not-accepted"});
		task.phase = materialization_v2_2_task_phase::materialization_started;
		return {};
	}
} // namespace cxxlens::detail::clang22::materialization
