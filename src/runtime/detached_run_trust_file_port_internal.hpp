#pragma once

/**
 * @file detached_run_trust_file_port_internal.hpp
 * @brief Linux host filesystem port for detached-run Ed25519 trust material.
 */

#include <optional>
#include <string>
#include <string_view>

#include "sdk/openssl_detached_run_crypto_internal.hpp"

namespace cxxlens::runtime
{
	/**
	 * Load one exact raw Ed25519 public key selected by Linux host configuration.
	 *
	 * The trusted/revoked state is host authority and is never inferred from the detached run.
	 * Paths and key bytes remain source-private and are omitted from diagnostics and product
	 * provenance.
	 */
	class detached_run_trust_file_port final
		: public sdk::detail::trusted_detached_run_ed25519_public_key_port
	{
	  public:
		detached_run_trust_file_port(std::string signer_id,
									 std::string public_key_path,
									 sdk::detail::detached_run_public_key_state state);

		[[nodiscard]] std::optional<sdk::detail::trusted_detached_run_ed25519_public_key>
		lookup(std::string_view scope,
			   std::string_view signer_id,
			   std::string_view key_fingerprint) const override;

	  private:
		std::string signer_id_;
		std::string public_key_path_;
		sdk::detail::detached_run_public_key_state state_;
	};
} // namespace cxxlens::runtime
