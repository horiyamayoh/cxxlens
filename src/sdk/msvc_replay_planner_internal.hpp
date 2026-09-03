#pragma once

/**
 * @file msvc_replay_planner_internal.hpp
 * @brief Source-private MSVC 19.51 response expansion and clang-cl replay authority.
 */

#include <cstddef>
#include <vector>

#include "application_analysis_internal.hpp"

namespace cxxlens::sdk::detail
{
	struct msvc_replay_mapping_result
	{
		std::vector<std::string> effective_arguments;
		std::vector<replay_option_mapping> option_mappings;
		std::vector<capture_gap> unresolved;
	};

	/** Map one validated MSVC 19.51 compile unit to bounded clang-cl 23.1 arguments. */
	[[nodiscard]] result<msvc_replay_mapping_result>
	map_msvc_19_51_replay_arguments(const decoded_capture_projection& capture,
									const decoded_capture_unit& unit,
									std::size_t unit_index,
									import_limits limits = {});
} // namespace cxxlens::sdk::detail
