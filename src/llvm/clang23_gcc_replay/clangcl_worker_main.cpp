#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "clangcl_worker_command_internal.hpp"

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
	using configuration = cxxlens::detail::clang23_gcc_replay::clangcl_worker_launch_configuration;
	configuration launch;
	const std::array bindings{
		std::pair{"CXXLENS_PROVIDER_MANIFEST", &configuration::provider_manifest},
		std::pair{"CXXLENS_PROVIDER_ID", &configuration::provider_id},
		std::pair{"CXXLENS_PROVIDER_BINARY_DIGEST", &configuration::provider_binary_digest},
		std::pair{"CXXLENS_PROVIDER_SEMANTIC_CONTRACT_DIGEST",
				  &configuration::provider_semantic_contract_digest},
		std::pair{"CXXLENS_PROVIDER_SANDBOX_POLICY_DIGEST", &configuration::sandbox_policy_digest},
		std::pair{"CXXLENS_PROVIDER_TASK_ID", &configuration::task_id},
		std::pair{"CXXLENS_PROVIDER_TASK_INPUT_DIGEST", &configuration::task_input_digest},
		std::pair{"CXXLENS_PROVIDER_NORMALIZED_INVOCATION_DIGEST",
				  &configuration::normalized_invocation_digest},
		std::pair{"CXXLENS_PROVIDER_TOOLCHAIN_DIGEST", &configuration::toolchain_digest},
		std::pair{"CXXLENS_PROVIDER_ENVIRONMENT_DIGEST", &configuration::environment_digest},
		std::pair{"CXXLENS_PROVIDER_PROTOCOL_MAJOR", &configuration::protocol_major},
		std::pair{"CXXLENS_PROVIDER_PROTOCOL_MINOR", &configuration::protocol_minor},
		std::pair{"CXXLENS_PROVIDER_SIGNATURE_DIGEST", &configuration::provider_signature_digest},
		std::pair{"CXXLENS_PROVIDER_REVOCATION_STATE", &configuration::provider_revocation_state},
		std::pair{"CXXLENS_DETACHED_RUN_SIGNER_ID", &configuration::detached_run_signer_id},
		std::pair{"CXXLENS_DETACHED_RUN_PRIVATE_KEY_FILE",
				  &configuration::detached_run_private_key_file},
		std::pair{"CXXLENS_DETACHED_RUN_PUBLIC_KEY_FILE",
				  &configuration::detached_run_public_key_file},
	};
	for (const auto& [name, member] : bindings)
	{
		auto value = environment(name);
		if (!value)
		{
			std::cerr << "application-analysis.replay-provider-failed:environment\n";
			return EXIT_FAILURE;
		}
		launch.*member = std::move(*value);
	}
	auto validated = cxxlens::detail::clang23_gcc_replay::execute_clangcl_worker_command(
		std::cin, std::cout, std::move(launch));
	if (!validated)
	{
		std::cerr << validated.error().code << ':' << validated.error().field << '\n';
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
