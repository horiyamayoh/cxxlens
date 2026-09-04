#pragma once

/**
 * @file detached_provider_run_builder_internal.hpp
 * @brief Canonical signed envelope builder for detached provider transcripts.
 */

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "detached_provider_run_internal.hpp"
#include "provider_runtime_internal.hpp"

namespace cxxlens::sdk::detail
{
	struct detached_run_signature
	{
		std::string signer_id;
		std::string key_fingerprint;
		std::array<std::byte, detached_provider_run_signature_bytes> signature{};
	};

	/** External signing authority. Private key material never enters the envelope builder. */
	class detached_run_signer
	{
	  public:
		virtual ~detached_run_signer() = default;
		[[nodiscard]] virtual result<detached_run_signature>
		sign(std::string_view scope, std::string_view signed_subject_digest) const = 0;
	};

	struct detached_transcript_projection
	{
		std::vector<detached_partition_projection> partitions;
		std::vector<detached_coverage_projection> coverage;
		std::vector<detached_unresolved_projection> unresolved;
		std::vector<detached_provenance_projection> provenance;
	};

	/** Compiler-neutral authority required to bind one validated transcript into an envelope. */
	struct detached_provider_run_authority
	{
		std::string task_id;
		std::string task_input_digest;
		std::string normalized_invocation_digest;
		std::string toolchain_digest;
		std::string environment_digest;
		std::string replay_plan_digest;
		detached_provider_identity provider;
	};

	/** Project only from the logical validator's immutable sealed transcript. */
	[[nodiscard]] detached_transcript_projection project_detached_provider_transcript(
		const provider::detail::sealed_provider_transcript& sealed);

	/**
	 * Bind a shared logical-validator result to an explicit compiler-neutral run authority and
	 * sign the canonical envelope. The runtime receipt, sealed transcript, and raw bytes are
	 * cross-checked before the signer is called.
	 */
	[[nodiscard]] result<validated_detached_provider_run>
	build_detached_provider_run_from_validated_transcript(
		detached_provider_run_authority authority,
		std::span<const std::byte> protocol_transcript,
		const provider::detail::validated_detached_provider_transcript& validated,
		const detached_run_signer& signer,
		import_limits limits = {});

	/**
	 * Re-decode and validate one exact Protocol-v2 stream, project it canonically, and sign the
	 * complete envelope through an external authority. No Store effect occurs.
	 */
	[[nodiscard]] result<validated_detached_provider_run>
	build_detached_provider_run(const provider::process_task_request& request,
								std::string replay_plan_digest,
								std::span<const std::byte> protocol_transcript,
								const detached_run_signer& signer,
								import_limits limits = {});
} // namespace cxxlens::sdk::detail
