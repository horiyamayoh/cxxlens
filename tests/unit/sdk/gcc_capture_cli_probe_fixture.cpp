#include <iostream>
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
	return 9;
}
