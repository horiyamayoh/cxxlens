#include <string>
#include <type_traits>

#include <cxxlens/search.hpp>

int main()
{
	static_assert(std::is_copy_constructible_v<cxxlens::search_report<cxxlens::call_site>>);
	cxxlens::search_options options;
	options.result_limit = 8U;
	return options.result_limit == 8U ? 0 : 1;
}
