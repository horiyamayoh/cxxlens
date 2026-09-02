#pragma once

/**
 * @file gcc_capture_command_service_internal.hpp
 * @brief Source-private installed-command service for bounded GCC capture.
 */

#include <cstddef>
#include <optional>
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

	struct gcc_wrapper_command_request
	{
		std::string project_id;
		std::string project_root;
		std::string capture_directory;
		std::string compiler_path;
		std::vector<std::string> compiler_arguments;
	};

	struct gcc_wrapper_command_result
	{
		int compiler_exit_code{};
		std::optional<std::string> bundle_path;
	};

	/** Capture one canonical bundle through the production file and process ports. */
	[[nodiscard]] result<std::vector<std::byte>>
	capture_gcc_command(const gcc_capture_command_request& request);

	/** Execute and capture one exact shell-free GCC compilation. */
	[[nodiscard]] result<gcc_wrapper_command_result>
	capture_gcc_wrapper_command(const gcc_wrapper_command_request& request);
} // namespace cxxlens::sdk::detail
