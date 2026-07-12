#include <iostream>
#include <string_view>

#include <cxxlens/select.hpp>

int main(const int count, char** arguments)
{
	using namespace cxxlens::select;
	const bool reverse = count > 1 && std::string_view{arguments[1]} == "reverse";
	const auto first = method("Base::start");
	const auto second = record("Derived");
	auto symbols =
		reverse ? any_symbol().all_of({second, first}) : any_symbol().all_of({first, second});
	auto calls = calls_to_method("Base", "start")
					 .include_derived_types()
					 .include_virtual_overrides()
					 .dispatch(dispatch_policy::static_and_virtual_candidates)
					 .in_file(file_selector{}.path_glob("src/**/*.cpp"));
	std::cout << symbols.to_json() << '\n' << calls.to_json() << '\n';
	return 0;
}
