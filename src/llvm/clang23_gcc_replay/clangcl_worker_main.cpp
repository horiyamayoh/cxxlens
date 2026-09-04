#include <cstdlib>
#include <iostream>
#include <string_view>

#include "replay_frontend_authority.hpp"
#include "worker_ingress.hpp"

int main(const int argc, char** argv)
{
	if (argc == 2 && std::string_view{argv[1]} == "--version")
	{
		std::cout << "cxxlens-clangcl-worker-23 23.1.0\n";
		return EXIT_SUCCESS;
	}
	if (argc != 1)
	{
		std::cerr << "application-analysis.replay-worker-ingress-failed:arguments\n";
		return EXIT_FAILURE;
	}
	auto result = cxxlens::detail::clang23_gcc_replay::execute_worker_ingress(
		std::cin, std::cout, cxxlens::detail::clang23_gcc_replay::msvc_replay_frontend_id);
	if (!result)
	{
		std::cerr << result.error().code << ':' << result.error().field << '\n';
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
