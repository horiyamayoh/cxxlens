#pragma once

/**
 * @file detached_run_signing_file_port_internal.hpp
 * @brief Worker-owned filesystem port for exact Ed25519 signing keys.
 */

#include <string>
#include <string_view>

#include "sdk/openssl_detached_run_crypto_internal.hpp"

namespace cxxlens::runtime
{
	/**
	 * Load one exact raw Ed25519 seed/public-key pair from launcher-selected files.
	 *
	 * Paths and key bytes are source-private worker authority. They never enter a capture bundle,
	 * replay plan, detached run, diagnostic, or command-line argument.
	 */
	class detached_run_signing_file_port final
		: public sdk::detail::detached_run_ed25519_signing_material_port
	{
	  public:
		detached_run_signing_file_port(std::string signer_id,
									   std::string private_key_path,
									   std::string public_key_path);

		[[nodiscard]] sdk::result<sdk::detail::detached_run_ed25519_signing_material>
		load(std::string_view scope, std::string_view signed_subject_digest) const override;

	  private:
		std::string signer_id_;
		std::string private_key_path_;
		std::string public_key_path_;
	};
} // namespace cxxlens::runtime
