#include "clangcl_worker_command_internal.hpp"

#include <charconv>
#include <cstdint>
#include <string_view>
#include <utility>

#include "provider_worker.hpp"
#include "replay_frontend_authority.hpp"
#include "runtime/detached_run_signing_file_port_internal.hpp"
#include "sdk/openssl_detached_run_crypto_internal.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	namespace
	{
		[[nodiscard]] sdk::error failure(std::string field, std::string detail)
		{
			return {
				"application-analysis.replay-provider-failed", std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool parse_version(const std::string_view text, std::uint16_t& output)
		{
			const auto [end, error] =
				std::from_chars(text.data(), text.data() + text.size(), output);
			return error == std::errc{} && end == text.data() + text.size();
		}
	} // namespace

	sdk::result<void>
	execute_clangcl_worker_command(std::istream& input,
								   std::ostream& output,
								   clangcl_worker_launch_configuration configuration,
								   const sdk::import_limits limits)
	{
		using namespace sdk::provider;
		if (configuration.provider_manifest.empty() ||
			configuration.provider_id != msvc_provider_id ||
			configuration.provider_binary_digest.empty() ||
			configuration.provider_semantic_contract_digest.empty() ||
			configuration.sandbox_policy_digest.empty() || configuration.task_id.empty() ||
			configuration.task_input_digest.empty() ||
			configuration.normalized_invocation_digest.empty() ||
			configuration.toolchain_digest.empty() || configuration.environment_digest.empty() ||
			configuration.provider_signature_digest.empty() ||
			configuration.provider_revocation_state.empty() ||
			configuration.detached_run_signer_id.empty() ||
			configuration.detached_run_private_key_file.empty() ||
			configuration.detached_run_public_key_file.empty())
			return sdk::unexpected(failure("environment", "missing-or-foreign-authority"));

		protocol_limits protocol;
		if (!parse_version(configuration.protocol_major, protocol.protocol_major) ||
			!parse_version(configuration.protocol_minor, protocol.maximum_minor) ||
			protocol.protocol_major != protocol_v2_major ||
			protocol.maximum_minor != protocol_v2_minor)
			return sdk::unexpected(failure("protocol_version", "unsupported"));
		protocol.minimum_minor = protocol_v2_minor;

		detached_provider_worker_authority authority{
			{{std::move(configuration.provider_manifest),
			  {configuration.task_id,
			   configuration.task_input_digest,
			   configuration.normalized_invocation_digest,
			   configuration.toolchain_digest,
			   configuration.environment_digest},
			  protocol},
			 std::move(configuration.provider_binary_digest),
			 std::move(configuration.provider_semantic_contract_digest),
			 std::move(configuration.sandbox_policy_digest),
			 std::string{msvc_provider_id},
			 msvc_provider_version,
			 std::string{msvc_replay_frontend_id}},
			std::move(configuration.provider_signature_digest),
			std::move(configuration.provider_revocation_state)};
		const runtime::detached_run_signing_file_port signing_material{
			std::move(configuration.detached_run_signer_id),
			std::move(configuration.detached_run_private_key_file),
			std::move(configuration.detached_run_public_key_file)};
		const sdk::detail::openssl_detached_run_signer signer{signing_material};
		return execute_detached_provider_worker(
			input, output, std::move(authority), signer, limits);
	}
} // namespace cxxlens::detail::clang23_gcc_replay
