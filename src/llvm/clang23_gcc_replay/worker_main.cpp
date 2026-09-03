#include <charconv>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "provider_worker.hpp"

namespace
{
	[[nodiscard]] std::optional<std::string> environment(const char* name)
	{
		const auto* value = std::getenv(name);
		return value == nullptr ? std::nullopt : std::optional<std::string>{value};
	}

	[[nodiscard]] bool parse_version(const std::string_view text, std::uint16_t& output)
	{
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), output);
		return error == std::errc{} && end == text.data() + text.size();
	}
} // namespace

int main()
{
	using namespace cxxlens::sdk::provider;
	auto manifest = environment("CXXLENS_PROVIDER_MANIFEST");
	auto selected_provider = environment("CXXLENS_PROVIDER_ID");
	auto semantic_contract = environment("CXXLENS_PROVIDER_SEMANTIC_CONTRACT_DIGEST");
	auto task_id = environment("CXXLENS_PROVIDER_TASK_ID");
	auto task_digest = environment("CXXLENS_PROVIDER_TASK_INPUT_DIGEST");
	auto invocation = environment("CXXLENS_PROVIDER_NORMALIZED_INVOCATION_DIGEST");
	auto toolchain = environment("CXXLENS_PROVIDER_TOOLCHAIN_DIGEST");
	auto effective_environment = environment("CXXLENS_PROVIDER_ENVIRONMENT_DIGEST");
	auto major = environment("CXXLENS_PROVIDER_PROTOCOL_MAJOR");
	auto minor = environment("CXXLENS_PROVIDER_PROTOCOL_MINOR");
	if (!manifest || !selected_provider || !semantic_contract || !task_id || !task_digest ||
		!invocation || !toolchain || !effective_environment || !major || !minor ||
		*selected_provider != cxxlens::detail::clang23_gcc_replay::provider_id)
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
	cxxlens::detail::clang23_gcc_replay::provider_worker_authority authority{
		{std::move(*manifest),
		 {*task_id, *task_digest, *invocation, *toolchain, *effective_environment},
		 limits},
		std::move(*semantic_contract)};
	auto validated = cxxlens::detail::clang23_gcc_replay::execute_provider_worker(
		std::cin, std::cout, std::move(authority));
	if (!validated)
	{
		std::cerr << validated.error().code << ':' << validated.error().field << '\n';
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
