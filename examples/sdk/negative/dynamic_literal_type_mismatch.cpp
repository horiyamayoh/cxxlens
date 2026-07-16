#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/sdk/query.hpp>

int main()
{
	cxxlens::sdk::relation_registry registry;
	if (!registry.add(cxxlens::cc::relations::call_site::descriptor()))
		return 1;
	auto relation = registry.require("cc.call_site", 1U);
	auto ordinal = relation->column("ordinal");
	auto rejected =
		cxxlens::sdk::query::equals_present(*ordinal, cxxlens::sdk::query::literal::utf8("zero"));
	return !rejected && rejected.error().code == "sdk.query-literal-type-mismatch" ? 0 : 1;
}
