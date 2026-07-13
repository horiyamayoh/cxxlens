#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>

#include "frontend_worker.hpp"

int main()
try
{
	std::ios::sync_with_stdio(false);
	std::string input{std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{}};
	if (std::cin.bad())
		return EXIT_FAILURE;
	auto output = cxxlens::detail::clang22::run_frontend_worker(input);
	if (!output)
		return EXIT_FAILURE;
	std::cout.write(output.value().data(), static_cast<std::streamsize>(output.value().size()));
	return std::cout.good() ? EXIT_SUCCESS : EXIT_FAILURE;
}
catch (...)
{
	return EXIT_FAILURE;
}
