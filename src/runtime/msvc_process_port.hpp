#pragma once

/**
 * @file msvc_process_port.hpp
 * @brief Windows-only shell-free process authority for the MSVC capture proxy.
 */

#include <cstdint>
#include <string>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::application_analysis_worker
{
	struct msvc_process_result
	{
		std::uint32_t exit_code{};
		std::wstring canonical_executable_path;
	};

	/** Execute one explicitly named compiler with inherited standard handles. */
	[[nodiscard]] sdk::result<msvc_process_result>
	run_msvc_process(const std::wstring& executable, const std::vector<std::wstring>& arguments);

	/** Read the mandatory compiler path captured before CLToolExe/CLToolPath override. */
	[[nodiscard]] sdk::result<std::wstring> configured_msvc_compiler();
} // namespace cxxlens::application_analysis_worker
