#pragma once

/**
 * @file detached_provider_run_adoption_internal.hpp
 * @brief Trusted Linux adoption boundary for authenticated detached provider runs.
 */

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "application_materialization_adoption_internal.hpp"
#include "application_materialization_execution_internal.hpp"
#include "compiler_replay_input_internal.hpp"
#include "detached_provider_run_internal.hpp"

namespace cxxlens::sdk::detail
{
	enum class detached_run_signature_verdict : std::uint8_t
	{
		verified,
		rejected,
		revoked,
		unavailable,
	};

	struct detached_run_signature_verification
	{
		detached_run_signature_verdict verdict{detached_run_signature_verdict::unavailable};
		std::string verifier_id;
		std::string trust_store_digest;
		std::string signer_id;
		std::string key_fingerprint;
		std::string signed_subject_digest;
		std::string signature_digest;
	};

	/** Read-only cryptographic and revocation authority supplied by the Linux installation. */
	class trusted_detached_run_signature_verifier
	{
	  public:
		virtual ~trusted_detached_run_signature_verifier() = default;
		[[nodiscard]] virtual detached_run_signature_verification
		verify(std::string_view scope,
			   std::string_view signer_id,
			   std::string_view key_fingerprint,
			   std::string_view signed_subject_digest,
			   std::span<const std::byte> signature,
			   std::string_view signature_digest) const = 0;
	};

	/**
	 * Authenticate, bind, re-decode, and compare one detached run before the existing writer path.
	 * No Store effect occurs. Only the raw Protocol-v2 transcript is content authority.
	 */
	[[nodiscard]] result<prepared_application_materialization>
	prepare_detached_application_materialization(
		const relation_engine& engine,
		const validated_materialization_task& task,
		const validated_compiler_replay_input& replay_input,
		const provider::process_task_request& process_request,
		const validated_detached_provider_run& detached_run,
		const trusted_detached_run_signature_verifier& signature_verifier,
		std::span<const partition_draft> host_partitions = {});

	/**
	 * Decode and prepare every detached compile-unit result before one atomic Store publication.
	 * Runs are matched by task ID, not caller order. A missing, duplicate, extra, unauthenticated,
	 * or malformed run leaves the Store unchanged.
	 */
	[[nodiscard]] result<application_materialization_adoption>
	publish_detached_application_materializations(
		const relation_engine& engine,
		snapshot_store& store,
		const application_materialization_execution_plan& plan,
		std::span<const std::vector<std::byte>> detached_runs,
		const trusted_detached_run_signature_verifier& signature_verifier,
		import_limits limits = {});
} // namespace cxxlens::sdk::detail
