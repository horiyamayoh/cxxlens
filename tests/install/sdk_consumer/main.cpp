#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/sdk.hpp>

int main()
{
	auto query = cxxlens::sdk::query::from<cxxlens::cc::relations::call_site>();
	return query && query->ir().validate() ? 0 : 1;
}
