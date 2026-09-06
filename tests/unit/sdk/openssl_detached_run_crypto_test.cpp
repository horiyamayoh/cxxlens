#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

#include "sdk/openssl_detached_run_crypto_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::detail;

	[[noreturn]] void fail(const char* message)
	{
		std::fputs(message, stderr);
		std::fputc('\n', stderr);
		std::exit(EXIT_FAILURE);
	}

	void require(const bool condition, const char* message)
	{
		if (!condition)
			fail(message);
	}

	[[nodiscard]] std::byte hex_nibble(const char value)
	{
		if (value >= '0' && value <= '9')
			return static_cast<std::byte>(value - '0');
		if (value >= 'a' && value <= 'f')
			return static_cast<std::byte>(value - 'a' + 10);
		fail("invalid test hex");
	}

	template <std::size_t Size>
	[[nodiscard]] std::array<std::byte, Size> bytes(const std::string_view hex)
	{
		require(hex.size() == Size * 2U, "test hex size mismatch");
		std::array<std::byte, Size> output{};
		for (std::size_t index{}; index < Size; ++index)
			output[index] = (hex_nibble(hex[index * 2U]) << 4U) | hex_nibble(hex[index * 2U + 1U]);
		return output;
	}

	class signing_material_port final : public detached_run_ed25519_signing_material_port
	{
	  public:
		detached_run_ed25519_signing_material value;
		bool unavailable{};

		[[nodiscard]] result<detached_run_ed25519_signing_material>
		load(const std::string_view, const std::string_view) const override
		{
			if (unavailable)
				return unexpected(
					error{"test.detached-run-signing-unavailable", "signer", "unavailable"});
			return value;
		}
	};

	class public_key_port final : public trusted_detached_run_ed25519_public_key_port
	{
	  public:
		std::optional<trusted_detached_run_ed25519_public_key> value;

		[[nodiscard]] std::optional<trusted_detached_run_ed25519_public_key>
		lookup(std::string_view, std::string_view, std::string_view) const override
		{
			return value;
		}
	};
} // namespace

int main()
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::detail;

	// RFC 8032 test key 1. The message below is deliberately nonempty because detached runs sign
	// their canonical subject digest text, not arbitrary payload bytes.
	signing_material_port signing_port;
	signing_port.value.scope = "detached-provider-run";
	signing_port.value.signer_id = "worker:clang23-replay-test";
	signing_port.value.private_key =
		bytes<detached_run_ed25519_private_key_bytes>("9d61b19deffd5a60ba844af492ec2cc4"
													  "4449c5697b326919703bac031cae7f60");
	signing_port.value.public_key =
		bytes<detached_run_ed25519_public_key_bytes>("d75a980182b10ab7d54bfed3c964073a"
													 "0ee172f3daa62325af021a68f707511a");
	const openssl_detached_run_signer signer{signing_port};
	const std::string subject = "sha256:" + std::string(64U, 'a');
	auto signed_value = signer.sign("detached-provider-run", subject);
	require(signed_value.has_value(), "valid signing material was not accepted");
	auto repeated_signature = signer.sign("detached-provider-run", subject);
	require(repeated_signature && repeated_signature->signature == signed_value->signature,
			"Ed25519 signature was not deterministic for the same key and subject");
	require(signed_value->signer_id == signing_port.value.signer_id,
			"signer identity was not retained");
	require(signed_value->key_fingerprint == content_digest(signing_port.value.public_key),
			"public key fingerprint was not derived");

	public_key_port trust_port;
	trust_port.value =
		trusted_detached_run_ed25519_public_key{detached_run_public_key_state::trusted,
												signing_port.value.scope,
												signing_port.value.signer_id,
												signed_value->key_fingerprint,
												signing_port.value.public_key,
												"sha256:" + std::string(64U, 'b')};
	const openssl_detached_run_signature_verifier verifier{trust_port};
	const auto signature_digest = content_digest(signed_value->signature);
	const auto verified = verifier.verify(signing_port.value.scope,
										  signed_value->signer_id,
										  signed_value->key_fingerprint,
										  subject,
										  signed_value->signature,
										  signature_digest);
	require(verified.verdict == detached_run_signature_verdict::verified,
			"valid Ed25519 detached-run signature was not verified");
	require(verified.verifier_id == "openssl.evp-ed25519.v1" &&
				verified.trust_store_digest == trust_port.value->trust_store_digest,
			"trusted verifier authority was not retained");

	const auto changed_subject = verifier.verify(signing_port.value.scope,
												 signed_value->signer_id,
												 signed_value->key_fingerprint,
												 "sha256:" + std::string(64U, 'c'),
												 signed_value->signature,
												 signature_digest);
	require(changed_subject.verdict == detached_run_signature_verdict::rejected,
			"changed subject escaped verification");
	auto changed_signature = signed_value->signature;
	changed_signature[0] ^= std::byte{1U};
	require(verifier.verify(signing_port.value.scope,
							signed_value->signer_id,
							signed_value->key_fingerprint,
							subject,
							changed_signature,
							content_digest(changed_signature))
					.verdict == detached_run_signature_verdict::rejected,
			"changed signature escaped verification");

	auto mismatched_public_key = signing_port.value;
	mismatched_public_key.public_key[0] ^= std::byte{1U};
	signing_material_port mismatch_port;
	mismatch_port.value = std::move(mismatched_public_key);
	const openssl_detached_run_signer mismatch_signer{mismatch_port};
	require(!mismatch_signer.sign(signing_port.value.scope, subject),
			"private/public key mismatch was accepted");

	trust_port.value->key_fingerprint = "sha256:" + std::string(64U, 'd');
	require(verifier.verify(signing_port.value.scope,
							signed_value->signer_id,
							signed_value->key_fingerprint,
							subject,
							signed_value->signature,
							signature_digest)
					.verdict == detached_run_signature_verdict::rejected,
			"trust-key fingerprint mismatch was accepted");

	trust_port.value->key_fingerprint = signed_value->key_fingerprint;
	trust_port.value->state = detached_run_public_key_state::revoked;
	require(verifier.verify(signing_port.value.scope,
							signed_value->signer_id,
							signed_value->key_fingerprint,
							subject,
							signed_value->signature,
							signature_digest)
					.verdict == detached_run_signature_verdict::revoked,
			"revoked signing key was not rejected with its exact terminal");

	trust_port.value.reset();
	require(verifier.verify(signing_port.value.scope,
							signed_value->signer_id,
							signed_value->key_fingerprint,
							subject,
							signed_value->signature,
							signature_digest)
					.verdict == detached_run_signature_verdict::unavailable,
			"missing trust authority was promoted or misclassified");

	signing_port.unavailable = true;
	require(!signer.sign(signing_port.value.scope, subject),
			"unavailable signing authority was promoted");
	return EXIT_SUCCESS;
}
