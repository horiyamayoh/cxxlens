#pragma once

/**
 * @file gcc_capture_command_service_internal.hpp
 * @brief Source-private installed-command service for bounded GCC capture.
 */

#include <cstddef>
#include <string>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk::detail
{
	struct gcc_capture_command_request
	{
		std::string project_id;
		std::string project_root;
		std::string compile_commands_path;
		std::string compiler_path;
	};

	/** Capture one canonical bundle through the production file and process ports. */
	[[nodiscard]] result<std::vector<std::byte>>
	capture_gcc_command(const gcc_capture_command_request& request);
} // namespace cxxlens::sdk::detail
