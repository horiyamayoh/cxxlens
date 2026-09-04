#include <charconv>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "provider_worker.hpp"
#include "replay_frontend_authority.hpp"
#include "runtime/detached_run_signing_file_port_internal.hpp"
#include "sdk/openssl_detached_run_crypto_internal.hpp"

namespace
{
	[[nodiscard]] std::optional<std::string> environment(const char* name)
	{
#ifdef _WIN32
		char* raw{};
		std::size_t bytes{};
		if (_dupenv_s(&raw, &bytes, name) != 0 || raw == nullptr)
		{
			std::free(raw);
			return std::nullopt;
		}
		std::string value{raw};
		std::free(raw);
		return value;
#else
		const auto* value = std::getenv(name);
		return value == nullptr ? std::nullopt : std::optional<std::string>{value};
#endif
	}

	[[nodiscard]] bool parse_version(const std::string_view text, std::uint16_t& output)
	{
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), output);
		return error == std::errc{} && end == text.data() + text.size();
	}
} // namespace

int main(const int argc, char** argv)
{
	if (argc == 2 && std::string_view{argv[1]} == "--version")
	{
		std::cout << "cxxlens-clangcl-worker-23 23.1.0\n";
		return EXIT_SUCCESS;
	}
	if (argc != 1)
	{
		std::cerr << "application-analysis.replay-provider-failed:arguments\n";
		return EXIT_FAILURE;
	}
	using namespace cxxlens::sdk::provider;
	auto manifest = environment("CXXLENS_PROVIDER_MANIFEST");
	auto selected_provider = environment("CXXLENS_PROVIDER_ID");
	auto binary_digest = environment("CXXLENS_PROVIDER_BINARY_DIGEST");
	auto semantic_contract = environment("CXXLENS_PROVIDER_SEMANTIC_CONTRACT_DIGEST");
	auto sandbox_policy = environment("CXXLENS_PROVIDER_SANDBOX_POLICY_DIGEST");
	auto task_id = environment("CXXLENS_PROVIDER_TASK_ID");
	auto task_digest = environment("CXXLENS_PROVIDER_TASK_INPUT_DIGEST");
	auto invocation = environment("CXXLENS_PROVIDER_NORMALIZED_INVOCATION_DIGEST");
	auto toolchain = environment("CXXLENS_PROVIDER_TOOLCHAIN_DIGEST");
	auto effective_environment = environment("CXXLENS_PROVIDER_ENVIRONMENT_DIGEST");
	auto major = environment("CXXLENS_PROVIDER_PROTOCOL_MAJOR");
	auto minor = environment("CXXLENS_PROVIDER_PROTOCOL_MINOR");
	auto provider_signature = environment("CXXLENS_PROVIDER_SIGNATURE_DIGEST");
	auto provider_revocation = environment("CXXLENS_PROVIDER_REVOCATION_STATE");
	auto run_signer = environment("CXXLENS_DETACHED_RUN_SIGNER_ID");
	auto run_private_key = environment("CXXLENS_DETACHED_RUN_PRIVATE_KEY_FILE");
	auto run_public_key = environment("CXXLENS_DETACHED_RUN_PUBLIC_KEY_FILE");
	if (!manifest || !selected_provider || !binary_digest || !semantic_contract ||
		!sandbox_policy || !task_id || !task_digest || !invocation || !toolchain ||
		!effective_environment || !major || !minor || !provider_signature || !provider_revocation ||
		!run_signer || !run_private_key || !run_public_key ||
		*selected_provider != cxxlens::detail::clang23_gcc_replay::msvc_provider_id)
	{
		std::cerr << "application-analysis.replay-provider-failed:environment\n";
		return EXIT_FAILURE;
	}
	protocol_limits limits;
	if (!parse_version(*major, limits.protocol_major) ||
		!parse_version(*minor, limits.maximum_minor) ||
		limits.protocol_major != protocol_v2_major || limits.maximum_minor != protocol_v2_minor)
	{
		std::cerr << "application-analysis.replay-provider-failed:protocol-version\n";
		return EXIT_FAILURE;
	}
	limits.minimum_minor = protocol_v2_minor;
	cxxlens::detail::clang23_gcc_replay::detached_provider_worker_authority authority{
		{{std::move(*manifest),
		  {*task_id, *task_digest, *invocation, *toolchain, *effective_environment},
		  limits},
		 std::move(*binary_digest),
		 std::move(*semantic_contract),
		 std::move(*sandbox_policy),
		 std::string{cxxlens::detail::clang23_gcc_replay::msvc_provider_id},
		 cxxlens::detail::clang23_gcc_replay::msvc_provider_version,
		 std::string{cxxlens::detail::clang23_gcc_replay::msvc_replay_frontend_id}},
		std::move(*provider_signature),
		std::move(*provider_revocation)};
	const cxxlens::runtime::detached_run_signing_file_port signing_material{
		std::move(*run_signer), std::move(*run_private_key), std::move(*run_public_key)};
	const cxxlens::sdk::detail::openssl_detached_run_signer signer{signing_material};
	auto validated = cxxlens::detail::clang23_gcc_replay::execute_detached_provider_worker(
		std::cin, std::cout, std::move(authority), signer);
	if (!validated)
	{
		std::cerr << validated.error().code << ':' << validated.error().field << '\n';
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
