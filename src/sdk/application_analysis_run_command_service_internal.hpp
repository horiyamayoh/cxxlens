#pragma once

/**
 * @file application_analysis_run_command_service_internal.hpp
 * @brief Source-private installed-command orchestration for application analysis.
 */

#include <string>

#include <cxxlens/sdk/application_analysis.hpp>
#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk::detail
{
	struct application_analysis_run_command_request
	{
		std::string bundle_path;
		std::string worker_path;
		std::string trusted_worker_digest;
		import_limits limits;
		provider::execution_budget budget;
	};

	struct application_analysis_run_command_result
	{
		materialization_terminal terminal{materialization_terminal::failed};
		std::string canonical_json;
	};

	/** Import, materialize the initial relation subset, and query the published snapshot. */
	[[nodiscard]] result<application_analysis_run_command_result>
	run_application_analysis_command(const application_analysis_run_command_request& request);
} // namespace cxxlens::sdk::detail
