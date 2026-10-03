#pragma once

/** @file application_analyzer.hpp @brief Local compiler-to-public-query application service. */

#include <string>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::detail::clang22
{
	struct application_analysis_options
	{
		std::string project_root;
		std::string compile_commands;
		std::string source_file;
	};

	/** Analyze one explicitly selected compilation variant and export independent public scans. */
	[[nodiscard]] sdk::result<std::string>
	analyze_application(const application_analysis_options& options);
} // namespace cxxlens::detail::clang22
