#pragma once

/**
 * @file application_analysis_command_service_internal.hpp
 * @brief Source-private installed-command service for bounded capture import.
 */

#include <string>

#include <cxxlens/sdk/application_analysis.hpp>
#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk::detail
{
	struct application_analysis_import_command_request
	{
		std::string bundle_path;
		import_limits limits;
	};

	struct loaded_application_analysis
	{
		capture_bundle bundle;
		imported_project project;
	};

	/** Read, decode, and import one capture without reconstructing any missing value. */
	[[nodiscard]] result<loaded_application_analysis>
	load_application_analysis(const application_analysis_import_command_request& request);

	/** Decode, validate, import, and render one deterministic informational projection. */
	[[nodiscard]] result<std::string>
	import_application_analysis_command(const application_analysis_import_command_request& request);
} // namespace cxxlens::sdk::detail
