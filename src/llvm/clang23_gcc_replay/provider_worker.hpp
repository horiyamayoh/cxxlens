#pragma once

/**
 * @file provider_worker.hpp
 * @brief Protocol v2 boundary for the detached Clang 23 GCC replay worker.
 */

#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include <cxxlens/sdk/application_analysis.hpp>
#include <cxxlens/sdk/provider.hpp>

#include "sdk/provider_validation_internal.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	inline constexpr std::string_view provider_id = "cxxlens.clang23-gcc-replay";
	inline constexpr sdk::semantic_version provider_version{1U, 0U, 0U};
	inline constexpr std::string_view msvc_provider_id = "cxxlens.clangcl23-msvc-replay";
	inline constexpr sdk::semantic_version msvc_provider_version{1U, 0U, 0U};

	struct provider_worker_authority
	{
		sdk::provider::host_transcript_expectation host;
		std::string provider_semantic_contract_digest;
		std::string provider_id;
		sdk::semantic_version provider_version;
		std::string replay_frontend;
	};

	struct provider_worker_result
	{
		std::vector<std::byte> protocol_transcript;
		std::string replay_plan_digest;
		sdk::provider::detail::sealed_host_input host_input;
	};

	/** Execute without an output side effect and retain the exact bounded Protocol-v2 result. */
	[[nodiscard]] sdk::result<provider_worker_result> run_provider_worker(
		std::istream& input, provider_worker_authority authority, sdk::import_limits limits = {});

	/**
	 * Validate one host transcript, execute the detached frontend, and emit only Protocol v2.
	 * Relation rows remain candidates; host-side adoption and Store publication are out of scope.
	 */
	[[nodiscard]] sdk::result<void> execute_provider_worker(std::istream& input,
															std::ostream& output,
															provider_worker_authority authority,
															sdk::import_limits limits = {});
} // namespace cxxlens::detail::clang23_gcc_replay
