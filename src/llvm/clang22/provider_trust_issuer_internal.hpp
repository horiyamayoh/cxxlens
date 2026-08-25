#pragma once

/**
 * @file provider_trust_issuer_internal.hpp
 * @brief Source-private boundary between provider discovery and trusted certification.
 */

#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>
#include <cxxlens/sdk/provider.hpp>

#include "provider_task_v4.hpp"

namespace cxxlens::detail::clang22
{
	/** The issuer-owned verdict consumed by provider selection. */
	struct provider_trust_issuance
	{
		bool trust_valid{};
		bool certification_valid{};
		std::vector<std::string> certified_qualifications;
		std::string subject_digest;
		std::string issuer_id;
		std::string certificate_id;
		std::string registry_sequence;

		[[nodiscard]] sdk::result<void> validate(std::string_view provider_id,
												 const sdk::semantic_version& provider_version,
												 std::string_view measured_binary_digest,
												 std::string_view required_qualification) const;
	};

	/** Canonical subject binding an issuer must sign before provider selection. */
	[[nodiscard]] sdk::result<std::string>
	provider_trust_subject_digest(std::string_view provider_id,
								  const sdk::semantic_version& provider_version,
								  std::string_view measured_binary_digest,
								  std::string_view required_qualification);

	/**
	 * A trusted issuer is the only component allowed to turn a measured provider manifest into
	 * certification evidence.  Requested qualifications and manifest trust flags are never an
	 * issuer implementation.
	 */
	class provider_trust_issuer_port
	{
	  public:
		virtual ~provider_trust_issuer_port() = default;
		[[nodiscard]] virtual sdk::result<provider_trust_issuance>
		issue(const sdk::provider::manifest& manifest,
			  std::string_view measured_binary_digest,
			  const provider_task_v4_trust_authority& policy) = 0;
	};

	/** Production default: no repository-bundled certificate is trusted. */
	class production_provider_trust_issuer final : public provider_trust_issuer_port
	{
	  public:
		[[nodiscard]] sdk::result<provider_trust_issuance>
		issue(const sdk::provider::manifest& manifest,
			  std::string_view measured_binary_digest,
			  const provider_task_v4_trust_authority& policy) override;
	};
} // namespace cxxlens::detail::clang22
