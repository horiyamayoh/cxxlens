#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

int main(const int argc, char** argv)
{
	if (argc == 3 && std::string_view{argv[1]} == "-dumpfullversion" &&
		std::string_view{argv[2]} == "-dumpversion")
	{
		std::cout << "16.2.0\n";
		return 0;
	}
	if (argc == 2 && std::string_view{argv[1]} == "-dumpmachine")
	{
		std::cout << "x86_64-pc-linux-gnu\n";
		return 0;
	}
	if (argc == 2 && std::string_view{argv[1]} == "--print-sysroot")
	{
		std::cout << '\n';
		return 0;
	}
	if (argc == 7 && std::string_view{argv[1]} == "-dM")
	{
		std::cout << "#define __GNUC__ 16\n#define __GNUC_MINOR__ 2\n";
		return 0;
	}
	if (argc == 7 && std::string_view{argv[1]} == "-E" && std::string_view{argv[5]} == "-v")
	{
		std::cerr << "#include <...> search starts here:\n"
				  << " /usr/include\n"
				  << "End of search list.\n";
		return 0;
	}
	std::string dependency;
	std::string object;
	std::string source;
	bool compile{};
	for (int index = 1; index < argc; ++index)
	{
		const std::string_view token{argv[index]};
		if (token == "-DFAIL_COMPILE")
			return 23;
		if (token == "-c")
			compile = true;
		else if (token == "-MF" && index + 1 < argc)
			dependency = argv[++index];
		else if (token == "-o" && index + 1 < argc)
			object = argv[++index];
		else if (!token.starts_with('-') && (token.ends_with(".c") || token.ends_with(".cpp")))
			source = token;
	}
	if (compile && !dependency.empty() && !source.empty())
	{
		if (!object.empty())
		{
			std::ofstream output{object, std::ios::binary};
			if (!output)
				return 10;
			output << "fixture-object";
		}
		std::ofstream dependencies{dependency};
		if (!dependencies)
			return 11;
		dependencies << (object.empty() ? "main.o" : object) << ": " << source
					 << " ../include/fixture.hpp\n";
		return dependencies ? 0 : 12;
	}
	return 9;
}
