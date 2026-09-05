#pragma once

/**
 * @file openssl_detached_run_crypto_internal.hpp
 * @brief Source-private Ed25519 signing and trust verification for detached runs.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "detached_provider_run_adoption_internal.hpp"
#include "detached_provider_run_builder_internal.hpp"

namespace cxxlens::sdk::detail
{
	inline constexpr std::size_t detached_run_ed25519_private_key_bytes = 32U;
	inline constexpr std::size_t detached_run_ed25519_public_key_bytes = 32U;

	/** Secret material supplied by a worker-owned key port, never by a capture bundle. */
	struct detached_run_ed25519_signing_material
	{
		std::string scope;
		std::string signer_id;
		std::array<std::byte, detached_run_ed25519_private_key_bytes> private_key{};
		std::array<std::byte, detached_run_ed25519_public_key_bytes> public_key{};
	};

	class detached_run_ed25519_signing_material_port
	{
	  public:
		virtual ~detached_run_ed25519_signing_material_port() = default;
		[[nodiscard]] virtual result<detached_run_ed25519_signing_material>
		load(std::string_view scope, std::string_view signed_subject_digest) const = 0;
	};

	/** OpenSSL EVP implementation of the existing external signer port. */
	class openssl_detached_run_signer final : public detached_run_signer
	{
	  public:
		explicit openssl_detached_run_signer(
			const detached_run_ed25519_signing_material_port& material_port) noexcept;

		[[nodiscard]] result<detached_run_signature>
		sign(std::string_view scope, std::string_view signed_subject_digest) const override;

	  private:
		const detached_run_ed25519_signing_material_port* material_port_{};
	};

	enum class detached_run_public_key_state : std::uint8_t
	{
		trusted,
		revoked,
	};

	/** Public trust material supplied only by the Linux adoption authority. */
	struct trusted_detached_run_ed25519_public_key
	{
		detached_run_public_key_state state{detached_run_public_key_state::trusted};
		std::string scope;
		std::string signer_id;
		std::string key_fingerprint;
		std::array<std::byte, detached_run_ed25519_public_key_bytes> public_key{};
		std::string trust_store_digest;
	};

	class trusted_detached_run_ed25519_public_key_port
	{
	  public:
		virtual ~trusted_detached_run_ed25519_public_key_port() = default;
		[[nodiscard]] virtual std::optional<trusted_detached_run_ed25519_public_key>
		lookup(std::string_view scope,
			   std::string_view signer_id,
			   std::string_view key_fingerprint) const = 0;
	};

	/** OpenSSL EVP implementation of the trusted detached-run verifier port. */
	class openssl_detached_run_signature_verifier final
		: public trusted_detached_run_signature_verifier
	{
	  public:
		explicit openssl_detached_run_signature_verifier(
			const trusted_detached_run_ed25519_public_key_port& public_key_port) noexcept;

		[[nodiscard]] detached_run_signature_verification
		verify(std::string_view scope,
			   std::string_view signer_id,
			   std::string_view key_fingerprint,
			   std::string_view signed_subject_digest,
			   std::span<const std::byte> signature,
			   std::string_view signature_digest) const override;

	  private:
		const trusted_detached_run_ed25519_public_key_port* public_key_port_{};
	};
} // namespace cxxlens::sdk::detail
