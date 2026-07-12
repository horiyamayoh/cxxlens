#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/search.hpp>

#include "../query/query_executor.hpp"

namespace cxxlens::detail
{
	template <class T>
	struct search_report_data
	{
		std::vector<T> matches;
		coverage_report coverage;
		std::vector<unresolved> unresolved_items;
		result_guarantee guarantee{result_guarantee::best_effort};
		precision_level precision{precision_level::ast_structural};
		std::string explanation;
		query::candidate_accounting accounting;
		std::vector<query::reason_count> reasons;
		std::vector<query::raw_call_match> call_rows;
		bool expand_unresolved{true};
	};

	struct search_report_access
	{
		template <class T>
		[[nodiscard]] static search_report<T>
		make(std::shared_ptr<const search_report_data<T>> data)
		{
			return search_report<T>{std::move(data)};
		}
	};
} // namespace cxxlens::detail
