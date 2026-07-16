#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "provider_worker.hpp"

int main()
{
	const std::string input{std::istreambuf_iterator<char>{std::cin},
							std::istreambuf_iterator<char>{}};
	std::vector<std::byte> bytes;
	bytes.reserve(input.size());
	for (const auto value : input)
		bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
	return cxxlens::detail::clang22::run_provider_worker(bytes, std::cout);
}
