#pragma once

#include <cstdint>
#include <string>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::detail::clang22::materialization
{
	/**
	 * Complete source-private identity for one materialization request authority.
	 *
	 * The request roots and catalog identity are retained alongside the derived digest so every
	 * bounded source, external receipt, and Store handoff can compare the same canonical value.
	 * The accepted cxxlens.df-0200.execution-journal-receipt-set.v1 seal remains the separate
	 * four-field projection; this complete binding is validated alongside it rather than folded
	 * into that digest domain. This type is deliberately source-private; it does not alter the
	 * public API or wire schema.
	 */
	struct materialization_claim_request_binding
	{
		std::string materialization_request_id;
		std::string request_digest;
		std::string semantic_request_digest;
		std::string catalog_id;
		std::string catalog_digest;
		std::uint64_t task_count{};
		std::string canonical_binding_digest;

		[[nodiscard]] bool operator==(const materialization_claim_request_binding&) const = default;
	};

	/** Recompute the canonical digest over the complete request/catalog binding projection. */
	[[nodiscard]] sdk::result<std::string> seal_materialization_claim_request_binding(
		const materialization_claim_request_binding& binding);
} // namespace cxxlens::detail::clang22::materialization
