#include <exception>
#include <iostream>
#include <string_view>

#include "sdk_doctor_entry.hpp"

namespace
{
	void print_usage()
	{
		std::cerr << "usage: cxxlens doctor relation-presence <relation-id>... [--format "
					 "json|markdown]\n"
				  << "       cxxlens doctor missing --project <project.json> --use-case <id> "
					 "[--format json|markdown]\n"
				  << "       cxxlens run --project <project.json> --use-case <id> "
					 "[--format json|markdown]\n";
	}
} // namespace

namespace
{
	int run(int argc, char** argv)
	{
		if (argc < 2)
		{
			print_usage();
			return 2;
		}

		const std::string_view command{argv[1]};
		if (command == "doctor")
		{
			if (argc < 3)
			{
				print_usage();
				return 2;
			}
			// The doctor implementation owns validation and product result semantics;
			// shift only the executable name so its existing subcommand parser remains
			// the single authority.
			return cxxlens_sdk_doctor_main(argc - 1, argv + 1);
		}
		if (command == "run")
		{
			if (argc < 3)
			{
				print_usage();
				return 2;
			}
			// `run` is the thin product entrypoint for the currently admitted
			// materialize-and-query capability. Until the native orchestration service
			// is selected, use the same fail-closed capability resolution as `doctor
			// missing`; this preserves unknown reasons and completion actions instead of
			// inventing a second result model.
			argv[1] = const_cast<char*>("missing");
			return cxxlens_sdk_doctor_main(argc, argv);
		}

		print_usage();
		return 2;
	}
} // namespace

int main(int argc, char** argv) noexcept
{
	try
	{
		return run(argc, argv);
	}
	catch (const std::exception& error)
	{
		std::cerr << "cxxlens: unexpected failure: " << error.what() << '\n';
	}
	catch (...)
	{
		std::cerr << "cxxlens: unexpected failure\n";
	}
	return 2;
}
