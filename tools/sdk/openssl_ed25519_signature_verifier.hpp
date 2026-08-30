#pragma once

// OpenSSL-backed implementation of the product's trusted-signature-verifier port.
//
// This header deliberately keeps OpenSSL behind the source-private doctor/tool boundary.  The
// authority loader still consumes trusted_signature_verifier, while the material port below is
// the only place from which an installation may supply public-key and signature bytes.  No key,
// trust anchor, or filesystem lookup is compiled into this implementation.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <openssl/evp.h>

#include "doctor_product_provider_authority_internal.hpp"

namespace cxxlens::sdk::doctor
{
	inline constexpr std::size_t ed25519_public_key_bytes = 32U;
	inline constexpr std::size_t ed25519_signature_bytes = 64U;

	/**
	 * @brief External, bounded Ed25519 material for one authority signature.
	 *
	 * The caller supplies this value through a trusted installation port.  The public key is an
	 * external trust-anchor/issuer key and the signature is looked up by its digest; neither is
	 * accepted from provider or project JSON.  `key_fingerprint` is the canonical sha256 digest of
	 * `public_key`, and `scope`/`signer_id` bind the material to the request that retrieved it.
	 */
	struct trusted_ed25519_signature_material
	{
		std::string scope;
		std::string signer_id;
		std::string key_fingerprint;
		std::array<std::byte, ed25519_public_key_bytes> public_key{};
		std::array<std::byte, ed25519_signature_bytes> signature{};
	};

	/**
	 * @brief Port for obtaining already-authorized Ed25519 material.
	 *
	 * Implementations own the trusted update path, key rotation, and revocation policy.  Returning
	 * no value means that the authority is unavailable; it must not be interpreted as a valid
	 * signature.  The port is intentionally read-only and has no secret-key operation.
	 */
	class trusted_ed25519_signature_material_port
	{
	  public:
		virtual ~trusted_ed25519_signature_material_port() = default;
		[[nodiscard]] virtual std::optional<trusted_ed25519_signature_material>
		lookup(std::string_view signature_scope,
			   std::string_view signer_id,
			   std::string_view signed_subject_digest,
			   std::string_view signature_digest) const = 0;
	};

	/** @brief Explicitly unavailable material port used when an installation has no authority. */
	class unavailable_ed25519_signature_material_port final
		: public trusted_ed25519_signature_material_port
	{
	  public:
		[[nodiscard]] std::optional<trusted_ed25519_signature_material> lookup(
			std::string_view, std::string_view, std::string_view, std::string_view) const override
		{
			return std::nullopt;
		}
	};

	/**
	 * @brief Verifies the exact subject bytes using OpenSSL's Ed25519 EVP implementation.
	 *
	 * The existing trusted_signature_verifier interface is intentionally unchanged.  A missing
	 * material record returns `unverified`; malformed material, digest mismatch, key mismatch, and
	 * cryptographic failure return `rejected`.  Thus an unavailable authority remains unknown while
	 * an attempted-but-invalid authority can never be promoted by the caller.
	 */
	class openssl_ed25519_signature_verifier final : public trusted_signature_verifier
	{
	  public:
		explicit openssl_ed25519_signature_verifier(
			const trusted_ed25519_signature_material_port& material_port) noexcept
			: material_port_{&material_port}
		{
		}

		[[nodiscard]] signature_verification_result
		verify(const std::string_view signature_scope,
			   const std::string_view signer_id,
			   const std::string_view signed_subject_digest,
			   const std::string_view signature_digest) const override
		{
			if (material_port_ == nullptr)
				return {};
			if (!bounded_identifier(signature_scope) || !bounded_identifier(signer_id) ||
				!subject_digest_text(signed_subject_digest) || !digest_text(signature_digest))
				return rejected();

			const auto material = material_port_->lookup(
				signature_scope, signer_id, signed_subject_digest, signature_digest);
			if (!material)
				return {};
			if (material->scope != signature_scope || material->signer_id != signer_id ||
				!bounded_identifier(material->scope) || !bounded_identifier(material->signer_id) ||
				!bounded_identifier(material->key_fingerprint) || !digest_text(signature_digest) ||
				!digest_text(material->key_fingerprint))
				return rejected();

			const auto public_key_digest = sha256_digest(material->public_key);
			const auto signature_bytes_digest = sha256_digest(material->signature);
			if (public_key_digest.empty() || signature_bytes_digest.empty() ||
				public_key_digest != material->key_fingerprint ||
				signature_bytes_digest != signature_digest)
				return rejected();

			using pkey_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
			using md_context_ptr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
			pkey_ptr key{EVP_PKEY_new_raw_public_key(
							 EVP_PKEY_ED25519,
							 nullptr,
							 reinterpret_cast<const unsigned char*>(material->public_key.data()),
							 material->public_key.size()),
						 EVP_PKEY_free};
			md_context_ptr context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
			if (!key || !context ||
				EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1)
				return rejected();

			const auto verification = EVP_DigestVerify(
				context.get(),
				reinterpret_cast<const unsigned char*>(material->signature.data()),
				material->signature.size(),
				reinterpret_cast<const unsigned char*>(signed_subject_digest.data()),
				signed_subject_digest.size());
			if (verification != 1)
				return rejected();

			return {authority_verdict::verified,
					"openssl.evp-ed25519.v1",
					material->key_fingerprint,
					std::string{signed_subject_digest},
					std::string{signature_digest}};
		}

	  private:
		[[nodiscard]] static signature_verification_result rejected() noexcept
		{
			return {authority_verdict::rejected, {}, {}, {}, {}};
		}

		[[nodiscard]] static bool bounded_identifier(const std::string_view value) noexcept
		{
			return !value.empty() && value.size() <= maximum_json_string_bytes;
		}

		[[nodiscard]] static bool digest_text(const std::string_view value) noexcept
		{
			if (value.size() != std::string_view{"sha256:"}.size() + 64U ||
				!value.starts_with("sha256:"))
				return false;
			for (const char byte : value.substr(std::string_view{"sha256:"}.size()))
				if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
					return false;
			return true;
		}

		[[nodiscard]] static bool subject_digest_text(const std::string_view value) noexcept
		{
			if (digest_text(value))
				return true;
			return value.size() == std::string_view{"semantic-v2:sha256:"}.size() + 64U &&
				value.starts_with("semantic-v2:sha256:") &&
				std::ranges::all_of(value.substr(std::string_view{"semantic-v2:sha256:"}.size()),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] static std::string
		sha256_digest(const std::span<const std::byte> bytes) noexcept
		{
			std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
			unsigned int digest_size{};
			using md_context_ptr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
			md_context_ptr context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
			if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
				EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1 ||
				EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1 ||
				digest_size != 32U)
				return {};

			static constexpr char hex[] = "0123456789abcdef";
			std::string output{"sha256:"};
			output.reserve(output.size() + digest_size * 2U);
			for (unsigned int index = 0U; index < digest_size; ++index)
			{
				output.push_back(hex[(digest[index] >> 4U) & 0x0fU]);
				output.push_back(hex[digest[index] & 0x0fU]);
			}
			return output;
		}

		const trusted_ed25519_signature_material_port* material_port_{};
	};
} // namespace cxxlens::sdk::doctor
