#pragma once

/** @file application_analyzer.hpp @brief Local compiler-to-public-query application service. */

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::detail::clang22
{
	struct application_analysis_options
	{
		std::string project_root;
		std::string compile_commands;
		std::string source_file;
		std::size_t maximum_output_bytes{64U * 1024U * 1024U};
		std::function<void(std::string_view)> progress{};
	};

	/** Analyze all selected compile units/variants and export independent public scans. */
	[[nodiscard]] sdk::result<std::string>
	analyze_application(const application_analysis_options& options);
} // namespace cxxlens::detail::clang22
