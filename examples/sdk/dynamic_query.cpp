#include <array>
#include <utility>

#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/sdk/query.hpp>

int main()
{
	cxxlens::sdk::relation_registry registry;
	if (!registry.add(cxxlens::cc::relations::call_site::descriptor()))
		return 1;
	auto relation = registry.require("cc.call_site", 1U);
	if (!relation)
		return 1;
	auto ordinal = relation->column("ordinal");
	auto call = relation->column("call");
	auto source = relation->column("source");
	auto query = cxxlens::sdk::query::dynamic_query::from(*relation);
	if (!ordinal || !call || !source || !query)
		return 1;
	auto predicate = cxxlens::sdk::query::equals_present(
		*ordinal, cxxlens::sdk::query::literal::unsigned_integer(0U));
	if (!predicate)
		return 1;
	auto filtered = std::move(*query).where(std::move(*predicate));
	const std::array output{*call, *source};
	if (!filtered)
		return 1;
	auto projected = std::move(*filtered).project(output);
	return projected && projected->ir().validate() ? 0 : 1;
}
