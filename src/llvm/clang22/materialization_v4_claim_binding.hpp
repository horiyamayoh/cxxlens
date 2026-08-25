#pragma once

/** @file materialization_v4_claim_binding.hpp
 *  @brief Source-closure-bound claim output for one task-v4 translation unit.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <cxxlens/sdk/claim.hpp>
#include <cxxlens/sdk/store.hpp>

#include "provider_task_v4.hpp"

namespace cxxlens::detail::clang22::materialization
{
	inline constexpr std::string_view materialization_v4_claim_binding_schema =
		"cxxlens.clang22.materialization-claim-binding.v4";
	inline constexpr std::string_view materialization_v4_claim_receipt_schema =
		"cxxlens.clang22.materialization-claim-receipt.v4";

	/**
	 * Semantic authority inherited by one task-v4 translation unit.
	 *
	 * This type contains only metadata and identities.  Source bytes remain owned by the
	 * authenticated source-closure transport; a claim producer cannot smuggle a second source
	 * representation into the claim binding.
	 */
	struct materialization_v4_claim_binding
	{
		std::string schema{materialization_v4_claim_binding_schema};
		std::string materialization_request_id;
		std::uint64_t task_index{};
		provider_task_v4_base_task base_task;
		provider_task_v4 task;
		source_closure_manifest manifest;
		std::string provider_id;
		std::string provider_semantic_contract_digest;
		std::string materializer_id;
		std::string materializer_semantic_contract_digest;
		std::string direct_basis_digest;
		std::string canonical_adoption_transform_digest;
		std::string base_ingestion_transform_digest;
		sdk::claim_guarantee guarantee;
		std::string assumption_set_id;
		std::string relation_descriptor_id;
		std::string scope;
		std::string interpretation;
		std::string precision_profile;
	};

	/** The exact semantic claim/coverage payload produced for one translation unit. */
	struct materialization_v4_claim_translation
	{
		materialization_v4_claim_binding binding;
		sdk::claim_batch_result batch;
		sdk::partition_draft partition;

		materialization_v4_claim_translation(materialization_v4_claim_binding binding,
											 sdk::claim_batch_result batch,
											 sdk::partition_draft partition)
			: binding{std::move(binding)}, batch{std::move(batch)}, partition{std::move(partition)}
		{
		}

		materialization_v4_claim_translation(const materialization_v4_claim_translation&) = delete;
		materialization_v4_claim_translation&
		operator=(const materialization_v4_claim_translation&) = delete;
		materialization_v4_claim_translation(materialization_v4_claim_translation&&) noexcept =
			default;
		materialization_v4_claim_translation&
		operator=(materialization_v4_claim_translation&&) noexcept = default;
	};

	/**
	 * Receipt of the independently validated claim translation.
	 *
	 * The receipt carries every result class count and all identities needed by a later Store
	 * handoff.  The claims, coverage, unresolved references, and conflict records remain in the
	 * sealed translation; they are not replaced by these counts or by a success flag.
	 */
	struct materialization_v4_claim_receipt
	{
		std::string schema{materialization_v4_claim_receipt_schema};
		std::string binding_digest;
		std::string materialization_request_id;
		std::uint64_t task_index{};
		std::string task_id;
		std::string task_v4_digest;
		std::string provider_execution_id;
		std::string source_closure_id;
		std::string source_closure_digest;
		std::string manifest_digest;
		std::string task_input_digest;
		std::string claim_batch_content_digest;
		std::string partition_id;
		std::string partition_content_digest;
		std::string coverage_digest;
		std::uint64_t claim_count{};
		std::uint64_t unresolved_count{};
		std::uint64_t conflict_count{};
		std::uint64_t differential_disagreement_count{};
		bool complete{};
		std::string receipt_digest;

		[[nodiscard]] bool operator==(const materialization_v4_claim_receipt&) const = default;
	};

	/** Sealed translation plus its derived Store partition identity and receipt. */
	struct materialization_v4_claim_sealed
	{
		materialization_v4_claim_translation translation;
		sdk::partition_manifest partition_manifest;
		sdk::snapshot_partition_binding partition_binding;
		materialization_v4_claim_receipt receipt;

		materialization_v4_claim_sealed(materialization_v4_claim_translation translation,
										sdk::partition_manifest partition_manifest,
										sdk::snapshot_partition_binding partition_binding,
										materialization_v4_claim_receipt receipt)
			: translation{std::move(translation)},
			  partition_manifest{std::move(partition_manifest)},
			  partition_binding{std::move(partition_binding)}, receipt{std::move(receipt)}
		{
		}

		materialization_v4_claim_sealed(const materialization_v4_claim_sealed&) = delete;
		materialization_v4_claim_sealed& operator=(const materialization_v4_claim_sealed&) = delete;
		materialization_v4_claim_sealed(materialization_v4_claim_sealed&&) noexcept = default;
		materialization_v4_claim_sealed&
		operator=(materialization_v4_claim_sealed&&) noexcept = default;
	};

	/** Validate the complete task/closure/provider authority and derive its canonical identity. */
	[[nodiscard]] sdk::result<std::string>
	materialization_v4_claim_binding_digest(const materialization_v4_claim_binding& binding);

	/** Validate one translation unit and issue its immutable partition/receipt boundary. */
	[[nodiscard]] sdk::result<materialization_v4_claim_sealed>
	seal_materialization_v4_claim_translation(const sdk::relation_engine& engine,
											  materialization_v4_claim_translation translation,
											  std::span<const sdk::claim> existing = {});

	/** Recompute every binding, claim, coverage, partition, and receipt identity. */
	[[nodiscard]] sdk::result<void>
	validate_materialization_v4_claim_receipt(const sdk::relation_engine& engine,
											  const materialization_v4_claim_sealed& sealed,
											  std::span<const sdk::claim> existing = {});
} // namespace cxxlens::detail::clang22::materialization
