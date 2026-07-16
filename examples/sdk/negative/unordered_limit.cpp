#include <utility>

#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/sdk/query.hpp>

int main()
{
	auto query = cxxlens::sdk::query::from<cxxlens::cc::relations::call_site>();
	if (!query)
		return 1;
	auto rejected = std::move(*query).limit(1U);
	return !rejected && rejected.error().code == "sdk.query-limit-requires-order" ? 0 : 1;
}
