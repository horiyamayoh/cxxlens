#pragma once

/** @file materialization_v4_store_source.hpp
 *  @brief Typed task-v4 output authority and replayable Store source.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/store.hpp>

#include "materialization_v4_incremental_ingress.hpp"

namespace cxxlens::detail::clang22::materialization
{
	inline constexpr std::string_view materialization_v4_provider_output_authority_schema =
		"cxxlens.clang22.materialization-provider-output-authority.v4";
	inline constexpr std::size_t materialization_v4_store_max_closures =
		materialization_v4_incremental_max_tasks;

	/**
	 * Explicit provider output authority required before a task-v4 result can enter Store.
	 *
	 * The recipe, output plan, and publication target are supplied by the provider/materializer
	 * planner.  They are never derived from task bytes, task IDs, or receipt success.  The snapshot
	 * draft and closure candidates are the typed SDK inputs covered by the same authority.
	 */
	struct materialization_v4_provider_output_authority
	{
		std::string schema{materialization_v4_provider_output_authority_schema};
		std::string materialization_request_id;
		materialization_v4_store_publication_authority publication;
		sdk::snapshot_draft snapshot;
		std::vector<sdk::closure_candidate> closures;
	};

	/** One immutable Store partition copied from a validated sealed task-v4 translation. */
	struct materialization_v4_store_partition
	{
		std::uint64_t task_index{};
		sdk::partition_draft draft;
		sdk::partition_manifest manifest;
		sdk::snapshot_partition_binding binding;
		materialization_v4_claim_receipt receipt;
	};

	struct materialization_v4_store_publication;

	/**
	 * Store-ready source made only from an explicitly authorized, complete task-v4 output.
	 *
	 * The source owns copies of the partition payloads and their exact identity projections.  The
	 * original sealed tasks may therefore be released after construction without creating a
	 * dangling replay source.  Publication consumes this value once and revalidates every stored
	 * projection against the caller's relation engine before opening a writer.
	 */
	class materialization_v4_store_source
	{
	  public:
		materialization_v4_store_source(materialization_v4_store_source&&) noexcept = default;
		materialization_v4_store_source&
		operator=(materialization_v4_store_source&&) noexcept = default;
		materialization_v4_store_source(const materialization_v4_store_source&) = default;
		materialization_v4_store_source&
		operator=(const materialization_v4_store_source&) = default;

		[[nodiscard]] const materialization_v4_provider_output_authority& authority() const noexcept
		{
			return authority_;
		}
		[[nodiscard]] const materialization_v4_incremental_receipt& receipt() const noexcept
		{
			return receipt_;
		}
		[[nodiscard]] std::span<const materialization_v4_store_partition>
		partitions() const noexcept
		{
			return partitions_;
		}

		/** Revalidate the copied authority, receipt, partition identities, and closure bindings. */
		[[nodiscard]] sdk::result<void> validate(const sdk::relation_engine& engine) const;

	  private:
		materialization_v4_store_source(materialization_v4_provider_output_authority authority,
										materialization_v4_incremental_receipt receipt,
										std::vector<materialization_v4_store_partition> partitions)
			: authority_{std::move(authority)}, receipt_{std::move(receipt)},
			  partitions_{std::move(partitions)}
		{
		}

		materialization_v4_provider_output_authority authority_;
		materialization_v4_incremental_receipt receipt_;
		std::vector<materialization_v4_store_partition> partitions_;

		friend sdk::result<materialization_v4_store_source> make_materialization_v4_store_source(
			const sdk::relation_engine&,
			const materialization_v4_incremental_receipt&,
			std::span<const materialization_v4_claim_sealed* const>,
			materialization_v4_provider_output_authority);
		friend sdk::result<materialization_v4_store_publication>
		publish_materialization_v4_store_source(const sdk::relation_engine&,
												sdk::snapshot_store&,
												materialization_v4_store_source);
	};

	/** Construct a Store source only after the existing sealed-task admission boundary succeeds. */
	[[nodiscard]] sdk::result<materialization_v4_store_source> make_materialization_v4_store_source(
		const sdk::relation_engine& engine,
		const materialization_v4_incremental_receipt& receipt,
		std::span<const materialization_v4_claim_sealed* const> sealed_tasks,
		materialization_v4_provider_output_authority authority);

	/**
	 * Publish one typed v4 source through the public SDK Store transaction.
	 *
	 * The operation order is fixed: validate source, begin, stage all partitions, stage all closure
	 * candidates, independently validate, and publish once.  No Store method is called before the
	 * source authority and all copied identities have passed validation.
	 */
	struct materialization_v4_store_publication
	{
		sdk::snapshot_handle snapshot;
		materialization_v4_provider_output_authority authority;
		materialization_v4_incremental_receipt receipt;
		/** Installed Clang 22 task-output receipt; empty for generic single-partition callers. */
		std::string output_receipt_digest;
		std::uint64_t output_batch_count{};
	};

	[[nodiscard]] sdk::result<materialization_v4_store_publication>
	publish_materialization_v4_store_source(const sdk::relation_engine& engine,
											sdk::snapshot_store& store,
											materialization_v4_store_source source);
} // namespace cxxlens::detail::clang22::materialization
