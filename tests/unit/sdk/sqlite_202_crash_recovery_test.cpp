#include <cstdlib>
#include <iostream>

#include "sqlite202_crash_recovery_harness.hpp"

int main(int argc, char** argv)
{
	if (argc > 1)
		return cxxlens::test::sqlite202_crash_child_entry(argc, argv);
	if (!cxxlens::test::run_sqlite202_crash_recrash(argv[0]))
	{
		std::cerr << "SQLite #202 crash/recrash harness failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "SQLite #202 crash/recrash harness passed\n";
	return EXIT_SUCCESS;
}
