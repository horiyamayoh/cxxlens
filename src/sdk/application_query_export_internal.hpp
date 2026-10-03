#pragma once

/** @file application_query_export_internal.hpp @brief Bounded lossless public query export. */

#include <cstddef>
#include <span>
#include <string>

#include <cxxlens/sdk/query.hpp>

namespace cxxlens::sdk::detail
{
	inline constexpr std::size_t maximum_application_query_export_bytes =
		std::size_t{64U} * 1024U * 1024U;

	/** Export independent scans from one immutable snapshot without reconstructing evidence. */
	[[nodiscard]] result<std::string>
	encode_application_queries(const relation_engine& engine,
							   const snapshot_handle& snapshot,
							   std::span<const std::string> relation_ids,
							   std::size_t maximum_bytes = maximum_application_query_export_bytes);
} // namespace cxxlens::sdk::detail
