#include <exception>
#include <iostream>
#include <string_view>

#include "llvm/clang22/application_analyzer.hpp"

int main(int argc, char** argv)
{
	if (argc == 2 && std::string_view{argv[1]} == "--help")
	{
		std::cout << "usage: cxxlens-clang22-analyze --project-root <path> "
					 "--compile-commands <path> [--file <path>]\n";
		return 0;
	}
	try
	{
		cxxlens::detail::clang22::application_analysis_options options;
		for (int index = 1; index < argc; ++index)
		{
			const std::string_view option{argv[index]};
			std::string* field = option == "--project-root" ? &options.project_root
				: option == "--compile-commands"			? &options.compile_commands
				: option == "--file"						? &options.source_file
															: nullptr;
			if (field == nullptr || !field->empty() || index + 1 >= argc)
			{
				std::cerr << "usage: cxxlens-clang22-analyze --project-root <path> "
							 "--compile-commands <path> [--file <path>]\n";
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
