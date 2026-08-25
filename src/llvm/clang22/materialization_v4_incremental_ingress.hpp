#pragma once

/** @file materialization_v4_incremental_ingress.hpp
 *  @brief Strict task-v4 claim receipt aggregation before Store publication.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "materialization_v4_claim_binding.hpp"

namespace cxxlens::detail::clang22::materialization
{
	inline constexpr std::string_view materialization_v4_incremental_receipt_schema =
		"cxxlens.clang22.materialization-incremental-receipt.v4";

	/** Resource limit for one request's ordered task-v4 receipt set. */
	inline constexpr std::size_t materialization_v4_incremental_max_tasks = 4096U;

	/**
	 * Canonical receipt for one task-v4 incremental input.
	 *
	 * The complete claim/coverage payload remains in the sealed claim translation.  This value
	 * contains the exact per-task receipts needed to validate an ordered Store handoff; the
	 * counters below are derived summaries and never replace unresolved or conflict records.
	 */
	struct materialization_v4_incremental_receipt
	{
		std::string schema{materialization_v4_incremental_receipt_schema};
		std::string materialization_request_id;
		std::uint64_t task_count{};
		std::vector<materialization_v4_claim_receipt> task_receipts;
		std::uint64_t claim_count{};
		std::uint64_t unresolved_count{};
		std::uint64_t conflict_count{};
		std::uint64_t differential_disagreement_count{};
		bool complete{};
		std::string receipt_digest;

		[[nodiscard]] bool
		operator==(const materialization_v4_incremental_receipt&) const = default;
	};

	/**
	 * Authority that is still absent from the task-v4 worker receipt today.
	 *
	 * These identities must come from a validated analysis/output planner and publication owner;
	 * inventing them from a task receipt would turn a compiler execution into an unsafe Store
	 * write.
	 */
	struct materialization_v4_store_publication_authority
	{
		std::string analysis_recipe_digest;
		std::string output_plan_digest;
		std::string publication_target;

		[[nodiscard]] bool
		operator==(const materialization_v4_store_publication_authority&) const = default;
	};

	/**
	 * Validated prepublication handoff token.
	 *
	 * This slice deliberately stops before opening or mutating a Store.  A future writer may
	 * consume this token only after it supplies the same validated authority and the sealed task
	 * payloads; no prior request/task object is represented here.
	 */
	struct materialization_v4_store_ingress
	{
		materialization_v4_incremental_receipt receipt;
		materialization_v4_store_publication_authority authority;

		[[nodiscard]] bool operator==(const materialization_v4_store_ingress&) const = default;
	};

	/** Build an ordered, content-addressed receipt set from sealed task-v4 translations. */
	[[nodiscard]] sdk::result<materialization_v4_incremental_receipt>
	make_materialization_v4_incremental_receipt(
		const sdk::relation_engine& engine,
		std::span<const materialization_v4_claim_sealed* const> sealed_tasks,
		std::span<const sdk::claim> existing = {});

	/**
	 * Revalidate every sealed task and compare every aggregate field and digest.
	 *
	 * `sealed_tasks` is the payload source and must be in canonical task-index order.  A receipt
	 * cannot be accepted from counts or a success bit alone.
	 */
	[[nodiscard]] sdk::result<void> validate_materialization_v4_incremental_receipt(
		const sdk::relation_engine& engine,
		const materialization_v4_incremental_receipt& receipt,
		std::span<const materialization_v4_claim_sealed* const> sealed_tasks,
		std::span<const sdk::claim> existing = {});

	/**
	 * Admit a validated receipt set to the prepublication Store boundary.
	 *
	 * The current task-v4 worker has no recipe, output plan, or publication target, so callers must
	 * pass `std::nullopt` and receive a typed failure.  This is intentionally fail-closed until a
	 * real worker/materializer supplies all three identities.
	 */
	[[nodiscard]] sdk::result<materialization_v4_store_ingress>
	admit_materialization_v4_store_ingress(
		const sdk::relation_engine& engine,
		const materialization_v4_incremental_receipt& receipt,
		std::span<const materialization_v4_claim_sealed* const> sealed_tasks,
		std::optional<materialization_v4_store_publication_authority> authority);
} // namespace cxxlens::detail::clang22::materialization
