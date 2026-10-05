#pragma once

/** @file query_transfer.hpp @brief Bounded, value-owned application query exchange. */

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/query.hpp>

namespace cxxlens::sdk::query
{
	/** @brief Decoder bounds checked before retaining external query data. */
	struct transfer_limits
	{
		std::size_t maximum_bytes{64U * 1024U * 1024U};
		std::size_t maximum_depth{64U};
		std::size_t maximum_scans{4096U};
		std::size_t maximum_rows{1000000U};
		std::size_t maximum_values{8000000U};
		[[nodiscard]] result<void> validate() const;
	};

	/** @brief One independent scan retaining its public plan and all result side channels. */
	struct application_relation_scan
	{
		std::string relation_id;
		logical_query_ir logical_ir;
		query_result result;
	};

	/** @brief Immutable-result values detached from the provider process and compiler job. */
	struct application_query_results
	{
		std::string snapshot_id;
		std::vector<application_relation_scan> scans;
	};

	/**
	 * @brief Decode cxxlens.application-query-results.v1 without dropping evidence or partiality.
	 * @details Each plan must be an independent scan of its supplied registry descriptor.
	 * An older ordered projection may omit current optional additive columns; omitted
	 * fields remain unobserved. Required columns, types and the saved plan digest must agree.
	 * This checks transfer consistency; it does not execute a plan, certify a provider, or adopt
	 * claims into a Store. JSON member order and whitespace do not affect the decoded values.
	 */
	[[nodiscard]] result<application_query_results> decode_application_queries(
		const relation_engine& engine, std::string_view input, transfer_limits limits = {});
} // namespace cxxlens::sdk::query
