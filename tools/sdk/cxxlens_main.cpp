#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "sdk/application_analysis_command_service_internal.hpp"
#include "sdk/application_analysis_run_command_service_internal.hpp"
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
				  << "       cxxlens run --bundle <capture-bundle> --worker <absolute-path> "
					 "--trusted-worker-digest <sha256-digest>\n"
				  << "       cxxlens import --bundle <capture-bundle>\n"
				  << "       cxxlens capture --project-id <id> --project-root <absolute-path> "
					 "--compile-commands <path> --compiler <absolute-path>\n"
				  << "       cxxlens capture --project-id <id> --project-root <absolute-path> "
					 "--capture-directory <path> --compiler <absolute-path> -- "
					 "<absolute-compiler> <arguments>...\n";
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
		std::string capture_directory_value;
		std::vector<std::string> compiler_arguments;
		bool project_id{};
		bool project_root{};
		bool compile_commands{};
		bool capture_directory{};
		bool compiler{};
		for (int index = 2; index < argc; ++index)
		{
			const std::string_view option{argv[index]};
			if (option == "--")
			{
				for (++index; index < argc; ++index)
					compiler_arguments.emplace_back(argv[index]);
				break;
			}
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
				(option == "--capture-directory" &&
				 assign_once(capture_directory_value, capture_directory, value)) ||
				(option == "--compiler" && assign_once(request.compiler_path, compiler, value));
			if (!accepted)
			{
				print_usage();
				return 2;
			}
		}
		if (!project_id || !project_root || !compiler || compile_commands == capture_directory ||
			(compile_commands && !compiler_arguments.empty()) ||
			(capture_directory &&
			 (compiler_arguments.empty() || compiler_arguments.front() != request.compiler_path)))
		{
			print_usage();
			return 2;
		}

		if (capture_directory)
		{
			cxxlens::sdk::detail::gcc_wrapper_command_request wrapper;
			wrapper.project_id = std::move(request.project_id);
			wrapper.project_root = std::move(request.project_root);
			wrapper.capture_directory = std::move(capture_directory_value);
			wrapper.compiler_path = std::move(request.compiler_path);
			wrapper.compiler_arguments = std::move(compiler_arguments);
			auto captured = cxxlens::sdk::detail::capture_gcc_wrapper_command(wrapper);
			if (!captured)
			{
				const auto& failure = captured.error();
				std::cerr << "cxxlens: " << failure.code << ": " << failure.field << ": "
						  << failure.detail << '\n';
				return 2;
			}
			return captured->compiler_exit_code;
		}

		cxxlens::sdk::detail::gcc_capture_command_request database_request;
		database_request.project_id = std::move(request.project_id);
		database_request.project_root = std::move(request.project_root);
		database_request.compile_commands_path = std::move(request.compile_commands_path);
		database_request.compiler_path = std::move(request.compiler_path);
		auto captured = cxxlens::sdk::detail::capture_gcc_command(database_request);
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

	int import_capture(int argc, char** argv)
	{
		cxxlens::sdk::detail::application_analysis_import_command_request request;
		bool bundle{};
		for (int index = 2; index < argc; ++index)
		{
			const std::string_view option{argv[index]};
			if (index + 1 >= argc || option != "--bundle")
			{
				print_usage();
				return 2;
			}
			++index;
			if (!assign_once(request.bundle_path, bundle, argv[index]))
			{
				print_usage();
				return 2;
			}
		}
		if (!bundle)
		{
			print_usage();
			return 2;
		}
		auto imported = cxxlens::sdk::detail::import_application_analysis_command(request);
		if (!imported)
		{
			const auto& failure = imported.error();
			std::cerr << "cxxlens: " << failure.code << ": " << failure.field << ": "
					  << failure.detail << '\n';
			return 2;
		}
		std::cout << *imported;
		std::cout.flush();
		if (!std::cout)
		{
			std::cerr << "cxxlens: application-analysis.import-output-failed: stdout: write\n";
			return 2;
		}
		return 0;
	}

	int run_application_analysis(int argc, char** argv)
	{
		cxxlens::sdk::detail::application_analysis_run_command_request request;
		bool bundle{};
		bool worker{};
		bool trusted_worker_digest{};
		for (int index = 2; index < argc; ++index)
		{
			const std::string_view option{argv[index]};
			if (index + 1 >= argc)
			{
				print_usage();
				return 2;
			}
			const auto* value = argv[++index];
			const bool accepted =
				(option == "--bundle" && assign_once(request.bundle_path, bundle, value)) ||
				(option == "--worker" && assign_once(request.worker_path, worker, value)) ||
				(option == "--trusted-worker-digest" &&
				 assign_once(request.trusted_worker_digest, trusted_worker_digest, value));
			if (!accepted)
			{
				print_usage();
				return 2;
			}
		}
		if (!bundle || !worker || !trusted_worker_digest)
		{
			print_usage();
			return 2;
		}
		auto analyzed = cxxlens::sdk::detail::run_application_analysis_command(request);
		if (!analyzed)
		{
			const auto& failure = analyzed.error();
			std::cerr << "cxxlens: " << failure.code << ": " << failure.field << ": "
					  << failure.detail << '\n';
			return 2;
		}
		std::cout << analyzed->canonical_json;
		std::cout.flush();
		if (!std::cout)
		{
			std::cerr << "cxxlens: application-analysis.run-output-failed: stdout: write\n";
			return 2;
		}
		return analyzed->terminal == cxxlens::sdk::materialization_terminal::published_complete ||
				analyzed->terminal == cxxlens::sdk::materialization_terminal::published_partial
			? 0
			: 1;
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
		if (command == "import")
			return import_capture(argc, argv);
		if (command == "run")
		{
			if (argc < 3)
			{
				print_usage();
				return 2;
			}
			if (std::string_view{argv[2]} == "--bundle")
				return run_application_analysis(argc, argv);
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
