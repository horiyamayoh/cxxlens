#include "provider_trust_issuer_internal.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace cxxlens::detail::clang22
{
	namespace
	{
		[[nodiscard]] sdk::error failure(std::string field, std::string detail = {})
		{
			return {"security.provider-certification-invalid", std::move(field), std::move(detail)};
		}
	} // namespace

	sdk::result<std::string>
	provider_trust_subject_digest(const std::string_view provider_id,
								  const sdk::semantic_version& provider_version,
								  const std::string_view measured_binary_digest,
								  const std::string_view required_qualification)
	{
		if (provider_id.empty() || provider_version.major == 0U || measured_binary_digest.empty() ||
			required_qualification.empty())
			return sdk::unexpected(failure("subject", "identity-missing"));
		return sdk::semantic_digest("cxxlens.provider-trust-subject.v1",
									std::string{provider_id} + "\n" + provider_version.string() +
										"\n" + std::string{measured_binary_digest} + "\n" +
										std::string{required_qualification});
	}

	sdk::result<void>
	provider_trust_issuance::validate(const std::string_view provider_id,
									  const sdk::semantic_version& provider_version,
									  const std::string_view measured_binary_digest,
									  const std::string_view required_qualification) const
	{
		if (!trust_valid)
			return sdk::unexpected(failure("trust", "untrusted"));
		if (!certification_valid || certified_qualifications.empty())
			return sdk::unexpected(failure("certification", "missing"));
		if (issuer_id.empty() || certificate_id.empty() || registry_sequence.empty())
			return sdk::unexpected(failure("certificate", "provenance-missing"));
		auto expected_subject = provider_trust_subject_digest(
			provider_id, provider_version, measured_binary_digest, required_qualification);
		if (!expected_subject || subject_digest != *expected_subject)
			return sdk::unexpected(failure("subject", "digest-mismatch"));
		if (std::ranges::find(certified_qualifications, required_qualification) ==
			certified_qualifications.end())
			return sdk::unexpected(failure("certification", "required-qualification-missing"));
		return {};
	}

	sdk::result<provider_trust_issuance>
	production_provider_trust_issuer::issue(const sdk::provider::manifest&,
											const std::string_view,
											const provider_task_v4_trust_authority&)
	{
		// The repository registry intentionally contains only conformance anchors and no production
		// certificate.  Do not turn a manifest request or requested qualification into authority.
		return sdk::unexpected(sdk::error{"security.certification-missing",
										  "provider-certificate",
										  "trusted-issuer-unavailable"});
	}
} // namespace cxxlens::detail::clang22
