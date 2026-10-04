#include <charconv>
#include <exception>
#include <iostream>
#include <string_view>

#include "llvm/clang22/application_analyzer.hpp"

int main(int argc, char** argv)
{
	if (argc == 2 && std::string_view{argv[1]} == "--help")
	{
		std::cout << "usage: cxxlens-clang22-analyze --project-root <path> "
					 "--compile-commands <path> [--file <path>] [--verbose] "
					 "[--maximum-output-bytes <1..536870912>]\n";
		return 0;
	}
	try
	{
		cxxlens::detail::clang22::application_analysis_options options;
		bool output_limit_set{};
		for (int index = 1; index < argc; ++index)
		{
			const std::string_view option{argv[index]};
			if (option == "--maximum-output-bytes" && !output_limit_set && index + 1 < argc)
			{
				const std::string_view value{argv[++index]};
				const auto parsed = std::from_chars(
					value.data(), value.data() + value.size(), options.maximum_output_bytes);
				if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
					options.maximum_output_bytes == 0U ||
					options.maximum_output_bytes > 512U * 1024U * 1024U)
				{
					std::cerr << "application-analysis.query-export-invalid: maximum_bytes: "
								 "out-of-range\n";
					return 2;
				}
				output_limit_set = true;
				continue;
			}
			if (option == "--verbose" && !options.progress)
			{
				options.progress = [](std::string_view message)
				{
					std::cerr << "cxxlens: " << message << '\n';
				};
				continue;
			}
			std::string* field = option == "--project-root" ? &options.project_root
				: option == "--compile-commands"			? &options.compile_commands
				: option == "--file"						? &options.source_file
															: nullptr;
			if (field == nullptr || !field->empty() || index + 1 >= argc)
			{
				std::cerr << "usage: cxxlens-clang22-analyze --project-root <path> "
							 "--compile-commands <path> [--file <path>] [--verbose]\n";
				return 2;
			}
			*field = argv[++index];
		}
		auto analyzed = cxxlens::detail::clang22::analyze_application(options);
		if (!analyzed)
		{
			const auto& error = analyzed.error();
			std::cerr << error.code << ": " << error.field << ": " << error.detail << '\n';
			return 1;
		}
		std::cout << *analyzed;
		return std::cout ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "cxxlens: " << error.what() << '\n';
		return 1;
	}
}
