#pragma once

/**
 * @file detached_application_materialization_file_service_internal.hpp
 * @brief Linux host service for authenticated detached-run file adoption.
 */

#include <string>
#include <vector>

#include "sdk/detached_provider_run_adoption_internal.hpp"
#include "sdk/openssl_detached_run_crypto_internal.hpp"

namespace cxxlens::runtime
{
	/** Explicit host authority required to read and trust one detached compile-unit result set. */
	struct detached_application_materialization_file_request
	{
		std::vector<std::string> detached_run_paths;
		std::string signer_id;
		std::string trusted_public_key_path;
		sdk::detail::detached_run_public_key_state public_key_state{
			sdk::detail::detached_run_public_key_state::trusted};
		sdk::import_limits limits;
	};

	/** Read, authenticate, task-match, and atomically publish one explicit detached-run set. */
	[[nodiscard]] sdk::result<sdk::detail::application_materialization_adoption>
	publish_detached_application_materializations_from_files(
		const sdk::relation_engine& engine,
		sdk::snapshot_store& store,
		const sdk::detail::application_materialization_execution_plan& plan,
		const detached_application_materialization_file_request& request);
} // namespace cxxlens::runtime
