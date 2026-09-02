#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#include "sdk/gcc_capture_command_service_internal.hpp"
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
					 "[--format json|markdown]\n"
				  << "       cxxlens capture --project-id <id> --project-root <absolute-path> "
					 "--compile-commands <path> --compiler <absolute-path>\n";
	}

	[[nodiscard]] bool assign_once(std::string& output, bool& assigned, const char* value)
	{
		if (assigned || value == nullptr)
			return false;
		output = value;
		assigned = true;
		return true;
	}

	int capture(int argc, char** argv)
	{
		cxxlens::sdk::detail::gcc_capture_command_request request;
		bool project_id{};
		bool project_root{};
		bool compile_commands{};
		bool compiler{};
		for (int index = 2; index < argc; ++index)
		{
			const std::string_view option{argv[index]};
			if (index + 1 >= argc)
			{
				print_usage();
				return 2;
			}
			const auto value = argv[++index];
			const bool accepted =
				(option == "--project-id" && assign_once(request.project_id, project_id, value)) ||
				(option == "--project-root" &&
				 assign_once(request.project_root, project_root, value)) ||
				(option == "--compile-commands" &&
				 assign_once(request.compile_commands_path, compile_commands, value)) ||
				(option == "--compiler" && assign_once(request.compiler_path, compiler, value));
			if (!accepted)
			{
				print_usage();
				return 2;
			}
		}
		if (!project_id || !project_root || !compile_commands || !compiler)
		{
			print_usage();
			return 2;
		}

		auto captured = cxxlens::sdk::detail::capture_gcc_command(request);
		if (!captured)
		{
			const auto& failure = captured.error();
			std::cerr << "cxxlens: " << failure.code << ": " << failure.field << ": "
					  << failure.detail << '\n';
			return 2;
		}
		if (captured->size() >
			static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
		{
			std::cerr << "cxxlens: application-analysis.capture-output-failed: stdout: "
						 "byte-count\n";
			return 2;
		}
		std::cout.write(reinterpret_cast<const char*>(captured->data()),
						static_cast<std::streamsize>(captured->size()));
		std::cout.flush();
		if (!std::cout)
		{
			std::cerr << "cxxlens: application-analysis.capture-output-failed: stdout: write\n";
			return 2;
		}
		return 0;
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
		if (command == "capture")
			return capture(argc, argv);
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
