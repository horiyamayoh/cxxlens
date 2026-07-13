#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

#include <unistd.h>

#include "llvm/common/frontend_worker_ipc.hpp"

namespace
{
	[[nodiscard]] int
	write_response(const cxxlens::result<cxxlens::detail::frontend::observation_batch>& response)
	{
		auto encoded = cxxlens::detail::frontend::encode_worker_response(response);
		if (!encoded)
			return EXIT_FAILURE;
		std::cout.write(encoded.value().data(),
						static_cast<std::streamsize>(encoded.value().size()));
		return std::cout.good() ? EXIT_SUCCESS : EXIT_FAILURE;
	}
} // namespace

int main(const int argument_count, const char* const* arguments)
{
	if (argument_count < 2)
		return EXIT_FAILURE;
	std::string input{std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{}};
	if (std::cin.bad())
		return EXIT_FAILURE;
	const std::string_view mode{arguments[1]};
	if (mode == "--block")
	{
		if (argument_count != 3)
			return EXIT_FAILURE;
		std::ofstream pid_file{arguments[2]};
		pid_file << ::getpid() << '\n';
		pid_file.close();
		if (!pid_file)
			return EXIT_FAILURE;
		std::this_thread::sleep_for(std::chrono::seconds{30});
		return EXIT_FAILURE;
	}
	if (mode == "--crash")
	{
		if (argument_count != 2)
			return EXIT_FAILURE;
		(void)::raise(SIGSEGV);
		return EXIT_FAILURE;
	}
	auto request = cxxlens::detail::frontend::decode_worker_request(input);
	if (!request)
		return EXIT_FAILURE;
	if (mode == "--mixed" && request.value().snapshot_key.contains("crash"))
	{
		(void)::raise(SIGABRT);
		return EXIT_FAILURE;
	}
	if (mode != "--success" && mode != "--mixed")
		return EXIT_FAILURE;
	cxxlens::detail::frontend::observation_batch batch;
	batch.adapter_id = "clang22.frontend";
	batch.adapter_version = "fixture-1.0.0";
	batch.unit = request.value().task.unit.id();
	batch.variant = request.value().task.unit.variant_id();
	batch.debug_context_identity = 1U;
	batch.coverage.parsed = 1U;
	return write_response(batch);
}
