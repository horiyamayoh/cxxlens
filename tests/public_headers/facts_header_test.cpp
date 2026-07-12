#include <string>
#include <type_traits>

#include <cxxlens/facts.hpp>

int main()
{
	static_assert(std::is_copy_constructible_v<cxxlens::fact_profile>);
	return cxxlens::fact_profile::semantic_search().to_json().find("fact-profile.v1") ==
			std::string::npos
		? 1
		: 0;
}
