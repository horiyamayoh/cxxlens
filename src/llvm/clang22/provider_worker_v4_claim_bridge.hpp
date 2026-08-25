#pragma once

/** @file provider_worker_v4_claim_bridge.hpp
 *  @brief Source-private task-v4 worker to detached claim handoff.
 */

#include <functional>
#include <string>

#include "materialization_v4_claim_binding.hpp"
#include "materialization_v4_incremental_ingress.hpp"
#include "provider_worker_v4.hpp"

namespace cxxlens::detail::clang22
{
	/**
	 * Explicit output authority supplied by the materializer/Store owner.
	 *
	 * The worker never derives these values from task identity, the source closure, or the
	 * callback result.  `publication` is only admitted at the prepublication boundary; this
	 * API does not open or mutate a Store.
	 */
	struct provider_worker_v4_output_authority
	{
		sdk::relation_engine engine;
		materialization::materialization_v4_store_publication_authority publication;

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/**
	 * Native callback result after all compiler-owned state has been detached.
	 *
	 * The callback may inspect the borrowed Clang unit synchronously, but its result contains only
	 * value-owned rows and typed metadata.  The bridge independently seals the translation before
	 * issuing any Store-ingress token.
	 */
	using provider_worker_v4_claim_translation_callback =
		std::move_only_function<sdk::result<materialization::materialization_v4_claim_translation>(
			provider::clang22::borrowed_translation_unit&)>;

	/** Extended worker input with explicit output authority. */
	struct provider_worker_v4_claim_input
	{
		provider_worker_v4_input worker;
		provider_worker_v4_output_authority output_authority;
		provider_worker_v4_claim_translation_callback output_callback;
	};

	/**
	 * Detached task-v4 claim result and validated prepublication handoff.
	 *
	 * `store_ingress` proves the receipt/authority/completeness checks performed by the existing
	 * v4 ingress API.  It deliberately does not perform Store mutation.
	 */
	struct provider_worker_v4_claim_receipt
	{
		provider_worker_v4_receipt execution;
		materialization::materialization_v4_claim_sealed claim;
		materialization::materialization_v4_store_ingress store_ingress;
	};

	/**
	 * Execute one authenticated task-v4 translation and seal its detached claim output.
	 *
	 * The ordinary `execute_provider_worker_v4` path remains execution-only and fail-closed.  This
	 * extended path requires both an explicit output authority and a detached-output callback; no
	 * output authority is inferred when either is absent.
	 */
	[[nodiscard]] sdk::result<provider_worker_v4_claim_receipt>
	execute_provider_worker_v4_with_claim_output(provider_worker_v4_claim_input input);
} // namespace cxxlens::detail::clang22
