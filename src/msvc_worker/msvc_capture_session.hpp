#pragma once

/**
 * @file msvc_capture_session.hpp
 * @brief Windows-only orchestration for one shell-free MSVC capture invocation.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::application_analysis_worker
{
	struct msvc_capture_command_result
	{
		std::uint32_t compiler_exit_code{};
		std::optional<std::string> published_bundle;
		std::optional<sdk::error> capture_error;
	};

	/** Run the real compiler, then publish a bundle only after a successful complete capture. */
	[[nodiscard]] sdk::result<msvc_capture_command_result>
	capture_msvc_command(const std::wstring& compiler, const std::vector<std::wstring>& arguments);
} // namespace cxxlens::application_analysis_worker
