#pragma once

/**
 * @file detached_run_input_file_port_internal.hpp
 * @brief Linux host filesystem port for bounded detached provider-run input.
 */

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <cxxlens/sdk/application_analysis.hpp>

namespace cxxlens::runtime
{
	/** Read explicit detached-run envelopes without treating path or caller order as authority. */
	class detached_run_input_file_port final
	{
	  public:
		[[nodiscard]] sdk::result<std::vector<std::vector<std::byte>>>
		read(std::span<const std::string> paths, sdk::import_limits limits = {}) const;
	};
} // namespace cxxlens::runtime
