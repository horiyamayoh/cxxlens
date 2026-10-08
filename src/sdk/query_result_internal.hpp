#pragma once

#include <cxxlens/sdk/query.hpp>

namespace cxxlens::sdk::query
{
	struct query_result::data
	{
		std::vector<annotated_row> row_values;
		execution_status status{execution_status::failed_before_result};
		bool ordered{};
		bool input_complete{};
		bool closed_world{};
		std::vector<snapshot_query_coverage> coverage;
		std::vector<std::string> closures;
		std::vector<query_unresolved> unresolved;
		std::vector<claim_conflict> conflict_values;
		std::vector<differential_disagreement> disagreement_values;
		std::vector<claim_producer> producers;
		query_summary_guarantee guarantee{"unknown",
										  "query-empty",
										  {"query-empty", {}},
										  {"query-empty"},
										  {"assumptions:unknown"},
										  {},
										  0U,
										  {},
										  {},
										  {}};
		query_explanation logical;
		query_explanation physical;
		std::string ir_digest;
		std::string snapshot;
		std::string publication;
		// Set only after all stored rows have passed generic input validation.
		bool rows_validated{};
	};

	struct query_transfer_access
	{
		[[nodiscard]] static std::span<const annotated_row>
		borrow_rows(const query_result& result) noexcept
		{
			return result.data_ ? std::span<const annotated_row>{result.data_->row_values}
								: std::span<const annotated_row>{};
		}
		[[nodiscard]] static bool rows_validated(const query_result& result) noexcept
		{
			return result.data_ && result.data_->rows_validated;
		}
		[[nodiscard]] static query_result make(std::shared_ptr<const query_result::data> data)
		{
			return query_result{std::move(data)};
		}
	};
} // namespace cxxlens::sdk::query
