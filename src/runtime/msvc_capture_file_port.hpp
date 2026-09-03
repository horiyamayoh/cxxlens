#pragma once

/** @file msvc_capture_file_port.hpp @brief Minimal worker filesystem capability. */

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::application_analysis_worker
{
	[[nodiscard]] sdk::result<std::string> read_worker_text_file(std::string_view path);
	[[nodiscard]] sdk::result<void> write_worker_binary_file(std::string_view path,
															 std::span<const std::byte> content);
} // namespace cxxlens::application_analysis_worker
