#include <cstdlib>
#include <iostream>

#include "worker_ingress.hpp"

int main()
{
	auto validated =
		cxxlens::detail::clang23_gcc_replay::validate_worker_ingress(std::cin, std::cout);
	if (!validated)
	{
		std::cerr << validated.error().code << ':' << validated.error().field << '\n';
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
