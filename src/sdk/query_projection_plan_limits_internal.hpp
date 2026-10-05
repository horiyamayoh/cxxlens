#pragma once

#include <functional>
#include <stop_token>
#include <string_view>

#include <cxxlens/sdk/query_transfer.hpp>

namespace cxxlens::sdk::query::detail
{
	[[nodiscard]] result<void>
	check_source_plan_limits(const application_query_results& input,
							 std::size_t maximum_queries,
							 std::size_t maximum_bytes,
							 std::stop_token cancellation,
							 std::string_view error_prefix,
							 std::size_t* measured_bytes = nullptr,
							 const std::function<bool()>& active_cancelled = {});
} // namespace cxxlens::sdk::query::detail
