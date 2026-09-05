#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "clangcl_worker_command_internal.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

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

	[[nodiscard]] bool use_binary_protocol_streams() noexcept
	{
#ifdef _WIN32
		return _setmode(_fileno(stdin), _O_BINARY) != -1 &&
			_setmode(_fileno(stdout), _O_BINARY) != -1;
#else
		return true;
#endif
	}

	template <class value_type>
	[[nodiscard]] bool environment_number(const char* name, value_type& output)
	{
		const auto value = environment(name);
		if (!value)
			return false;
		const auto [end, error] =
			std::from_chars(value->data(), value->data() + value->size(), output);
		return error == std::errc{} && end == value->data() + value->size();
	}

} // namespace

int main(const int argc, char** argv)
{
	if (argc == 2 && std::string_view{argv[1]} == "--version")
	{
		std::cout << "cxxlens-clangcl-worker-23 23.1.0\n";
		return EXIT_SUCCESS;
	}
	const bool sandbox_child = argc == 2 && std::string_view{argv[1]} == "--sandbox-child";
	if (argc != 1 && !sandbox_child)
	{
		std::cerr << "application-analysis.replay-provider-failed:arguments\n";
		return EXIT_FAILURE;
	}
	if (!use_binary_protocol_streams())
	{
		std::cerr << "application-analysis.replay-provider-failed:binary-streams\n";
		return EXIT_FAILURE;
	}
	using configuration = cxxlens::detail::clang23_gcc_replay::clangcl_worker_launch_configuration;
	configuration launch;
	const std::array worker_bindings{
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
	};
	for (const auto& [name, member] : worker_bindings)
	{
		auto value = environment(name);
		if (!value)
		{
			std::cerr << "application-analysis.replay-provider-failed:environment\n";
			return EXIT_FAILURE;
		}
		launch.*member = std::move(*value);
	}
	if (sandbox_child)
	{
		const std::array signing_environment{
			"CXXLENS_PROVIDER_SIGNATURE_DIGEST",
			"CXXLENS_PROVIDER_REVOCATION_STATE",
			"CXXLENS_DETACHED_RUN_SIGNER_ID",
			"CXXLENS_DETACHED_RUN_PRIVATE_KEY_FILE",
			"CXXLENS_DETACHED_RUN_PUBLIC_KEY_FILE",
		};
		for (const auto* name : signing_environment)
		{
			if (environment(name))
			{
				std::cerr
					<< "application-analysis.replay-provider-failed:child-signing-authority\n";
				return EXIT_FAILURE;
			}
		}
		auto authority = cxxlens::detail::clang23_gcc_replay::make_clangcl_worker_authority(launch);
		if (!authority)
		{
			std::cerr << authority.error().code << ':' << authority.error().field << '\n';
			return EXIT_FAILURE;
		}
		cxxlens::sdk::import_limits limits;
		if (!environment_number("CXXLENS_IMPORT_MAXIMUM_BUNDLE_BYTES",
								limits.maximum_bundle_bytes) ||
			!environment_number("CXXLENS_IMPORT_MAXIMUM_NESTING_DEPTH",
								limits.maximum_nesting_depth) ||
			!environment_number("CXXLENS_IMPORT_MAXIMUM_COMPILE_UNITS",
								limits.maximum_compile_units) ||
			!environment_number("CXXLENS_IMPORT_MAXIMUM_ARGUMENTS_PER_UNIT",
								limits.maximum_arguments_per_unit) ||
			!environment_number("CXXLENS_IMPORT_MAXIMUM_AUXILIARY_FILES_PER_UNIT",
								limits.maximum_auxiliary_files_per_unit) ||
			!environment_number("CXXLENS_IMPORT_MAXIMUM_ENVIRONMENT_EFFECTS_PER_UNIT",
								limits.maximum_environment_effects_per_unit) ||
			!environment_number("CXXLENS_IMPORT_MAXIMUM_PATH_MAPPINGS",
								limits.maximum_path_mappings) ||
			!environment_number("CXXLENS_IMPORT_MAXIMUM_STRING_BYTES",
								limits.maximum_string_bytes) ||
			!environment_number("CXXLENS_IMPORT_MAXIMUM_TOTAL_METADATA_BYTES",
								limits.maximum_total_metadata_bytes) ||
			!environment_number("CXXLENS_IMPORT_MAXIMUM_SOURCE_CLOSURE_MEMBERS",
								limits.maximum_source_closure_members) ||
			!environment_number("CXXLENS_IMPORT_MAXIMUM_SOURCE_CLOSURES",
								limits.maximum_source_closures) ||
			!environment_number("CXXLENS_IMPORT_MAXIMUM_SOURCE_CLOSURE_BLOBS",
								limits.maximum_source_closure_blobs) ||
			!environment_number("CXXLENS_IMPORT_MAXIMUM_SOURCE_CLOSURE_BYTES",
								limits.maximum_source_closure_bytes) ||
			!limits.validate())
		{
			std::cerr << "application-analysis.replay-provider-failed:child-limits\n";
			return EXIT_FAILURE;
		}
		auto executed = cxxlens::detail::clang23_gcc_replay::execute_provider_worker(
			std::cin, std::cout, std::move(*authority), limits);
		if (!executed)
		{
			std::cerr << executed.error().code << ':' << executed.error().field << ':'
					  << executed.error().detail << '\n';
			return EXIT_FAILURE;
		}
		return EXIT_SUCCESS;
	}

	const std::array signing_bindings{
		std::pair{"CXXLENS_PROVIDER_SIGNATURE_DIGEST", &configuration::provider_signature_digest},
		std::pair{"CXXLENS_PROVIDER_REVOCATION_STATE", &configuration::provider_revocation_state},
		std::pair{"CXXLENS_DETACHED_RUN_SIGNER_ID", &configuration::detached_run_signer_id},
		std::pair{"CXXLENS_DETACHED_RUN_PRIVATE_KEY_FILE",
				  &configuration::detached_run_private_key_file},
		std::pair{"CXXLENS_DETACHED_RUN_PUBLIC_KEY_FILE",
				  &configuration::detached_run_public_key_file},
	};
	for (const auto& [name, member] : signing_bindings)
	{
		auto value = environment(name);
		if (!value)
		{
			std::cerr << "application-analysis.replay-provider-failed:environment\n";
			return EXIT_FAILURE;
		}
		launch.*member = std::move(*value);
	}
	auto process = cxxlens::detail::clang23_gcc_replay::make_windows_clangcl_sandbox_process_port();
	if (!process)
	{
		std::cerr << "application-analysis.replay-provider-failed:sandbox-process-port\n";
		return EXIT_FAILURE;
	}
	auto validated = cxxlens::detail::clang23_gcc_replay::execute_clangcl_worker_command(
		std::cin, std::cout, std::move(launch), *process);
	if (!validated)
	{
		std::cerr << validated.error().code << ':' << validated.error().field << ':'
				  << validated.error().detail << '\n';
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
