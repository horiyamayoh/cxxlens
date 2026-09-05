#pragma once

/**
 * @file clangcl_worker_command_internal.hpp
 * @brief Source-private authenticated clang-cl worker command service.
 */

#include <istream>
#include <ostream>
#include <string>

#include <cxxlens/sdk/application_analysis.hpp>

#include "clangcl_sandbox_process_port_internal.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	/** Exact launcher authority; none of these values are inferred from the capture payload. */
	struct clangcl_worker_launch_configuration
	{
		std::string provider_manifest;
		std::string provider_id;
		std::string provider_binary_digest;
		std::string provider_semantic_contract_digest;
		std::string sandbox_policy_digest;
		std::string task_id;
		std::string task_input_digest;
		std::string normalized_invocation_digest;
		std::string toolchain_digest;
		std::string environment_digest;
		std::string protocol_major;
		std::string protocol_minor;
		std::string provider_signature_digest;
		std::string provider_revocation_state;
		std::string detached_run_signer_id;
		std::string detached_run_private_key_file;
		std::string detached_run_public_key_file;
	};

	/** Validate launcher-owned worker authority independently from detached-run signing. */
	[[nodiscard]] sdk::result<provider_worker_authority>
	make_clangcl_worker_authority(const clangcl_worker_launch_configuration& configuration);

	/** Validate launcher authority, execute the worker, and emit only a signed detached envelope.
	 */
	[[nodiscard]] sdk::result<void>
	execute_clangcl_worker_command(std::istream& input,
								   std::ostream& output,
								   clangcl_worker_launch_configuration configuration,
								   const clangcl_sandbox_process_port& process,
								   sdk::import_limits limits = {});
} // namespace cxxlens::detail::clang23_gcc_replay
