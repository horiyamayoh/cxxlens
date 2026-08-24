#include <charconv>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "provider_worker.hpp"

namespace
{
	[[nodiscard]] int descriptor_from_environment(const char* name) noexcept
	{
		const auto* value = std::getenv(name);
		if (value == nullptr)
			return -1;
		const std::string_view text{value};
		int descriptor{};
		const auto [end, error] =
			std::from_chars(text.data(), text.data() + text.size(), descriptor);
		return error == std::errc{} && end == text.data() + text.size() && descriptor >= 4
			? descriptor
			: -1;
	}
} // namespace

int main()
{
	const auto* mode = std::getenv("CXXLENS_PROVIDER_INGRESS_MODE");
	if (mode != nullptr)
	{
		if (std::string_view{mode} != "task-v4-source-closure-v2")
			return EXIT_FAILURE;
		const auto read_descriptor =
			descriptor_from_environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_READ_FD");
		const auto write_descriptor =
			descriptor_from_environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_WRITE_FD");
		if (read_descriptor < 4 || write_descriptor < 4 || read_descriptor == write_descriptor)
			return EXIT_FAILURE;
		return cxxlens::detail::clang22::run_provider_worker_v4_source_closure(
			std::cin, std::cout, read_descriptor, write_descriptor);
	}
	// The worker has one supported ingress contract. Do not fall back to the
	// retired stdin transcript path when the authenticated source-closure
	// boundary is absent.
	return EXIT_FAILURE;
}
