#include <iostream>

#include "provider_worker.hpp"

int main()
{
	return cxxlens::detail::clang22::run_provider_worker(std::cin, std::cout);
}
