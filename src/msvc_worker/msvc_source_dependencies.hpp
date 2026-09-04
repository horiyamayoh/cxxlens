#pragma once

/**
 * @file msvc_source_dependencies.hpp
 * @brief Bounded decoder for MSVC /sourceDependencies output.
 */

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::application_analysis_worker
{
	struct msvc_source_dependencies
	{
		std::string source;
		std::vector<std::string> includes;
	};

	/** Decode the pinned MSVC dependency document without trusting its counts or paths. */
	[[nodiscard]] sdk::result<msvc_source_dependencies>
	decode_msvc_source_dependencies(std::string_view document,
									std::size_t maximum_sources = 4096U,
									std::size_t maximum_string_bytes = 4096U);
} // namespace cxxlens::application_analysis_worker
