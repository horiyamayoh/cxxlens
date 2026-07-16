#include <array>
#include <utility>

#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/sdk/query.hpp>

int main()
{
	using call = cxxlens::cc::relations::call_site;
	auto query = cxxlens::sdk::query::from<call>();
	auto predicate =
		cxxlens::sdk::query::equals_present(cxxlens::sdk::query::col<call::ordinal>(),
											cxxlens::sdk::query::literal::unsigned_integer(0U));
	if (!query || !predicate)
		return 1;
	auto filtered = std::move(*query).where(std::move(*predicate));
	const std::array output{cxxlens::sdk::query::col<call::call>(),
							cxxlens::sdk::query::col<call::source>()};
	if (!filtered)
		return 1;
	auto projected = std::move(*filtered).project(output);
	return projected && projected->ir().validate() ? 0 : 1;
}
