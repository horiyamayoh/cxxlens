#include <iostream>

#include <cxxlens/configuration.hpp>

int main(const int argc, const char* const* argv)
{
	if (argc != 2)
		return 2;
	auto configuration = cxxlens::configuration::load(argv[1]);
	if (!configuration)
		return 1;
	std::cout << configuration.value().resolved_json() << '\n';
	std::cout << configuration.value().explain("execution.memory_budget_mb") << '\n';
	return 0;
}
