#pragma once

/**
 * @file provider_worker.hpp
 * @brief Protocol v2 boundary for the detached Clang 23 GCC replay worker.
 */

#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <cxxlens/sdk/application_analysis.hpp>
#include <cxxlens/sdk/provider.hpp>

#include "sdk/detached_provider_run_builder_internal.hpp"
#include "sdk/provider_runtime_internal.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	inline constexpr std::string_view provider_id = "cxxlens.clang23-gcc-replay";
	inline constexpr sdk::semantic_version provider_version{1U, 0U, 0U};
	inline constexpr std::string_view msvc_provider_id = "cxxlens.clangcl23-msvc-replay";
	inline constexpr sdk::semantic_version msvc_provider_version{1U, 0U, 0U};

	struct provider_worker_authority
	{
		sdk::provider::host_transcript_expectation host;
		std::string provider_binary_digest;
		std::string provider_semantic_contract_digest;
		std::string sandbox_policy_digest;
		std::string provider_id;
		sdk::semantic_version provider_version;
		std::string replay_frontend;
	};

	struct provider_worker_result
	{
		std::vector<std::byte> protocol_transcript;
		std::string replay_plan_digest;
		std::optional<std::string> provider_signature_digest;
		sdk::provider::detail::validated_detached_provider_transcript validated_transcript;
	};

	/** Trust values supplied by the detached launcher, separately from the provider manifest. */
	struct detached_provider_worker_authority
	{
		provider_worker_authority worker;
		std::string provider_signature_digest;
		std::string provider_revocation_state;
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

	/** Build one authenticated detached envelope without granting Store publication authority. */
	[[nodiscard]] sdk::result<sdk::detail::validated_detached_provider_run>
	run_detached_provider_worker(std::istream& input,
								 detached_provider_worker_authority authority,
								 const sdk::detail::detached_run_signer& signer,
								 sdk::import_limits limits = {});

	/** Emit only the canonical authenticated detached-provider-run bytes. */
	[[nodiscard]] sdk::result<void>
	execute_detached_provider_worker(std::istream& input,
									 std::ostream& output,
									 detached_provider_worker_authority authority,
									 const sdk::detail::detached_run_signer& signer,
									 sdk::import_limits limits = {});
} // namespace cxxlens::detail::clang23_gcc_replay
