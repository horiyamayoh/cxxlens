#include "openssl_detached_run_crypto_internal.hpp"

#include <algorithm>
#include <memory>
#include <ranges>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error crypto_error(std::string field, std::string detail)
		{
			return {"application-analysis.detached-run-crypto-failed",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] bool digest_like(const std::string_view value) noexcept
		{
			return value.starts_with("sha256:") && value.size() == 71U &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] detached_run_signature_verification rejected() noexcept
		{
			detached_run_signature_verification output;
			output.verdict = detached_run_signature_verdict::rejected;
			return output;
		}

		using pkey_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
		using context_ptr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
	} // namespace

	openssl_detached_run_signer::openssl_detached_run_signer(
		const detached_run_ed25519_signing_material_port& material_port) noexcept
		: material_port_{&material_port}
	{
	}

	result<detached_run_signature>
	openssl_detached_run_signer::sign(const std::string_view scope,
									  const std::string_view signed_subject_digest) const
	{
		if (material_port_ == nullptr || !validate_strong_id(scope) ||
			!digest_like(signed_subject_digest))
			return unexpected(crypto_error("signing_request", "invalid"));
		auto loaded = material_port_->load(scope, signed_subject_digest);
		if (!loaded)
			return unexpected(std::move(loaded.error()));
		auto material = std::move(*loaded);
		const auto cleanse = [&material]() noexcept
		{
			OPENSSL_cleanse(material.private_key.data(), material.private_key.size());
		};
		if (material.scope != scope || !validate_strong_id(material.signer_id))
		{
			cleanse();
			return unexpected(crypto_error("signing_material", "authority-mismatch"));
		}

		pkey_ptr key{EVP_PKEY_new_raw_private_key(
						 EVP_PKEY_ED25519,
						 nullptr,
						 reinterpret_cast<const unsigned char*>(material.private_key.data()),
						 material.private_key.size()),
					 EVP_PKEY_free};
		std::array<std::byte, detached_run_ed25519_public_key_bytes> derived_public_key{};
		std::size_t derived_public_key_size = derived_public_key.size();
		if (!key ||
			EVP_PKEY_get_raw_public_key(key.get(),
										reinterpret_cast<unsigned char*>(derived_public_key.data()),
										&derived_public_key_size) != 1 ||
			derived_public_key_size != derived_public_key.size() ||
			!std::ranges::equal(derived_public_key, material.public_key))
		{
			cleanse();
			return unexpected(crypto_error("signing_material", "public-key-mismatch"));
		}

		detached_run_signature output;
		output.signer_id = std::move(material.signer_id);
		output.key_fingerprint = content_digest(material.public_key);
		context_ptr context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
		std::size_t signature_size = output.signature.size();
		const bool signed_value = context &&
			EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1 &&
			EVP_DigestSign(context.get(),
						   reinterpret_cast<unsigned char*>(output.signature.data()),
						   &signature_size,
						   reinterpret_cast<const unsigned char*>(signed_subject_digest.data()),
						   signed_subject_digest.size()) == 1 &&
			signature_size == output.signature.size();
		cleanse();
		if (!signed_value)
			return unexpected(crypto_error("signature", "openssl-sign-failed"));
		return output;
	}

	openssl_detached_run_signature_verifier::openssl_detached_run_signature_verifier(
		const trusted_detached_run_ed25519_public_key_port& public_key_port) noexcept
		: public_key_port_{&public_key_port}
	{
	}

	detached_run_signature_verification
	openssl_detached_run_signature_verifier::verify(const std::string_view scope,
													const std::string_view signer_id,
													const std::string_view key_fingerprint,
													const std::string_view signed_subject_digest,
													const std::span<const std::byte> signature,
													const std::string_view signature_digest) const
	{
		if (public_key_port_ == nullptr || !validate_strong_id(scope) ||
			!validate_strong_id(signer_id) || !digest_like(key_fingerprint) ||
			!digest_like(signed_subject_digest) || !digest_like(signature_digest) ||
			signature.size() != detached_provider_run_signature_bytes ||
			content_digest(signature) != signature_digest)
			return rejected();
		const auto material = public_key_port_->lookup(scope, signer_id, key_fingerprint);
		if (!material)
			return {};
		if (material->state == detached_run_public_key_state::revoked)
		{
			detached_run_signature_verification output;
			output.verdict = detached_run_signature_verdict::revoked;
			return output;
		}
		if (material->scope != scope || material->signer_id != signer_id ||
			material->key_fingerprint != key_fingerprint ||
			content_digest(material->public_key) != key_fingerprint ||
			!digest_like(material->trust_store_digest))
			return rejected();

		pkey_ptr key{EVP_PKEY_new_raw_public_key(
						 EVP_PKEY_ED25519,
						 nullptr,
						 reinterpret_cast<const unsigned char*>(material->public_key.data()),
						 material->public_key.size()),
					 EVP_PKEY_free};
		context_ptr context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
		if (!key || !context ||
			EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
			EVP_DigestVerify(context.get(),
							 reinterpret_cast<const unsigned char*>(signature.data()),
							 signature.size(),
							 reinterpret_cast<const unsigned char*>(signed_subject_digest.data()),
							 signed_subject_digest.size()) != 1)
			return rejected();

		return {detached_run_signature_verdict::verified,
				"openssl.evp-ed25519.v1",
				material->trust_store_digest,
				std::string{signer_id},
				std::string{key_fingerprint},
				std::string{signed_subject_digest},
				std::string{signature_digest}};
	}
} // namespace cxxlens::sdk::detail
