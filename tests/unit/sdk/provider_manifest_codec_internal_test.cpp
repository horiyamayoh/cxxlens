#include "sdk/provider_manifest_codec_internal.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::detail;
	using namespace cxxlens::sdk::provider;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] std::string digest(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] manifest fixture()
	{
		manifest value;
		value.provider_id = "cxxlens.clangcl23-msvc-replay";
		value.provider_version = {1U, 2U, 3U};
		value.package_identity = "cxxlens.application-analysis-worker.windows-x64";
		value.publisher = "cxxlens.project";
		value.license = "Apache-2.0 WITH LLVM-exception";
		value.signature = "request-signature:self-claim";
		value.protocol = {protocol_v2_major,
						  protocol_v2_minor,
						  protocol_v2_minor,
						  {"task-input-chunks-v2", "worker-observation-v1"},
						  {"diagnostic-summary-v1"}};
		value.platform_tuples = {"windows-x86_64-msvc"};
		value.provider_binary_digest = digest('1');
		value.provider_semantic_contract_digest = digest('2');
		value.offered_relations = {"cc.entity.v1", "source.span.v1"};
		value.required_relations = {};
		value.interpretation_domains = {"cc.clangcl23-msvc-replay-1"};
		value.invalidation_contract = digest('3');
		value.determinism_contract = digest('4');
		value.resource_class = "provider.application-analysis";
		value.sandbox_minimum = "enforced";
		value.requested_qualifications = {"experimental", "schema-conformant"};
		value.trust_flags = {"manifest-self-claim", "signature-unverified"};
		value.task_input_stage = "observation";
		value.task_output_stage = "observation";
		return value;
	}

	void round_trip_retains_self_claims_without_promoting_trust()
	{
		auto expected = fixture();
		const auto canonical = expected.canonical_json();
		auto decoded = decode_provider_manifest(canonical);
		require(decoded.has_value(), decoded ? "" : decoded.error().detail);
		require(decoded->canonical_json() == canonical, "manifest canonical round trip drifted");
		require(decoded->trust_flags == expected.trust_flags &&
					decoded->requested_qualifications == expected.requested_qualifications &&
					decoded->signature == expected.signature,
				"manifest self-claims were not retained exactly");

		expected.signature.reset();
		decoded = decode_provider_manifest(expected.canonical_json());
		require(decoded && !decoded->signature, "null signature did not round trip");
	}

	void reject(std::string encoded, const std::string_view detail)
	{
		auto decoded = decode_provider_manifest(encoded);
		require(!decoded, "invalid manifest was accepted");
		require(decoded.error().code == "provider.manifest-decode-invalid",
				"manifest rejection used the wrong error code");
		if (!decoded.error().detail.contains(detail))
		{
			std::cerr << "expected rejection reason " << detail << ", got "
					  << decoded.error().detail << '\n';
			std::exit(1);
		}
	}

	void structural_and_canonical_rejections()
	{
		const auto canonical = fixture().canonical_json();
		reject(" " + canonical, "noncanonical-json");
		auto unknown = canonical;
		unknown.insert(1U, R"("added":null,)");
		reject(std::move(unknown), "member-set");
		auto missing = canonical;
		const auto signature = missing.find(R"(,"signature":"request-signature:self-claim")");
		require(signature != std::string::npos, "fixture signature member was not found");
		missing.erase(signature,
					  std::string_view{R"(,"signature":"request-signature:self-claim")"}.size());
		reject(std::move(missing), "member-set");

		auto wrong_kind = canonical;
		const auto major = wrong_kind.find(R"("major":2)");
		require(major != std::string::npos, "fixture protocol major was not found");
		wrong_kind.replace(major, std::string_view{R"("major":2)"}.size(), R"("major":"2")");
		reject(std::move(wrong_kind), "uint32");

		auto reordered = canonical;
		const auto first = reordered.find(R"("determinism_contract")");
		require(first == 1U, "fixture did not start with the expected member");
		const auto comma = reordered.find(',', first);
		const auto first_member = reordered.substr(first, comma - first + 1U);
		reordered.erase(first, first_member.size());
		reordered.insert(reordered.size() - 1U,
						 "," + first_member.substr(0U, first_member.size() - 1U));
		reject(std::move(reordered), "noncanonical-json");
	}

	void semantic_and_resource_rejections()
	{
		auto leading_zero = fixture().canonical_json();
		const auto version = leading_zero.find(R"("provider_version":"1.2.3")");
		require(version != std::string::npos, "fixture version was not found");
		leading_zero.replace(version,
							 std::string_view{R"("provider_version":"1.2.3")"}.size(),
							 R"("provider_version":"01.2.3")");
		reject(std::move(leading_zero), "semantic-version");

		auto overflow = fixture().canonical_json();
		const auto major = overflow.find(R"("major":2)");
		overflow.replace(major, std::string_view{R"("major":2)"}.size(), R"("major":4294967296)");
		reject(std::move(overflow), "uint32");

		auto wrong_digest = fixture().canonical_json();
		const auto digest_value = wrong_digest.find(digest('1'));
		require(digest_value != std::string::npos, "fixture binary digest was not found");
		wrong_digest.replace(digest_value, digest('1').size(), "semantic-v2:" + digest('1'));
		reject(std::move(wrong_digest), "sha256-digest");

		reject(std::string(64U * 1024U + 1U, 'x'), "input-byte-limit");
	}
} // namespace

int main()
{
	round_trip_retains_self_claims_without_promoting_trust();
	structural_and_canonical_rejections();
	semantic_and_resource_rejections();
	return 0;
}
