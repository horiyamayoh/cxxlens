#pragma once

/**
 * @file gcc_replay_planner_internal.hpp
 * @brief Source-private GCC 16.2 response expansion and replay-option authority.
 */

#include <cstddef>
#include <vector>

#include "application_analysis_internal.hpp"

namespace cxxlens::sdk::detail
{
	struct gcc_replay_mapping_result
	{
		std::vector<std::string> effective_arguments;
		std::vector<replay_option_mapping> option_mappings;
		std::vector<capture_gap> unresolved;
	};

	/** Map one validated GCC 16.2 compile unit to bounded clang GCC-mode arguments. */
	[[nodiscard]] result<gcc_replay_mapping_result>
	map_gcc_16_2_replay_arguments(const decoded_capture_projection& capture,
								  const decoded_capture_unit& unit,
								  std::size_t unit_index,
								  import_limits limits = {});
} // namespace cxxlens::sdk::detail
