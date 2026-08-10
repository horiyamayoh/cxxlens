#pragma once

#include <string>

#include "materialization_claims.hpp"
#include "materialization_occurrence.hpp"
#include "materialization_request_v2_1.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/**
	 * Source-private bridge from the admitted v2.1 request to the existing claims/store boundary.
	 *
	 * The returned request owns only task metadata and source receipts.  It deliberately contains
	 * no decoded source bytes and no resident task.v3 payload; the latter is reconstructed only by
	 * the per-task execution callback after this bridge has returned.
	 */
	struct materialization_v2_1_claim_context
	{
		validated_materialization_request request;
		materialization_producer_authority producer_authority;
		materialization_guarantee_authority guarantee_authority;
	};

	/** Build the source-private claims binding from an already admitted request occurrence. */
	[[nodiscard]] sdk::result<materialization_v2_1_claim_context>
	make_materialization_v2_1_claim_context(validated_materialization_request_v2_1& request,
											const materialization_occurrence_receipt& occurrence);
} // namespace cxxlens::detail::clang22::materialization
