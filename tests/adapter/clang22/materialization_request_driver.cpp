#include <iostream>
#include <iterator>
#include <string>

#include "llvm/clang22/materialization_json.hpp"
#include "llvm/clang22/materialization_request.hpp"

int main()
{
	using namespace cxxlens::detail::clang22::materialization;
	std::string input{std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{}};
	auto document = parse_json_object(std::move(input));
	if (!document)
	{
		std::cout << document.error().code << '|' << document.error().field << '|'
				  << document.error().detail << '\n';
		return 1;
	}
	auto request = validate_materialization_request(std::move(*document));
	if (!request)
	{
		std::cout << request.error().code << '|' << request.error().field << '|'
				  << request.error().detail << '\n';
		return 1;
	}
	std::cout << "ok\n";
}
