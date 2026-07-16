#include <array>
#include <utility>

#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/company_lock_acquire.hpp>
#include <cxxlens/sdk/query.hpp>

int main()
{
	using call = cxxlens::cc::relations::call_site;
	using lock = cxxlens::company::relations::lock_acquire;
	auto calls = cxxlens::sdk::query::from<call>();
	auto locks = cxxlens::sdk::query::from<lock>();
	auto predicate = cxxlens::sdk::query::equals_present(
		cxxlens::sdk::query::col<call::caller>(), cxxlens::sdk::query::col<lock::function>());
	if (!calls || !locks || !predicate)
		return 1;
	auto joined = std::move(*calls).inner_join(std::move(*locks), std::move(*predicate));
	if (!joined)
		return 1;
	const std::array order{cxxlens::sdk::query::col<call::call>(),
						   cxxlens::sdk::query::col<lock::acquire>()};
	auto ordered = std::move(*joined).order_by(order);
	if (!ordered)
		return 1;
	const std::array output{cxxlens::sdk::query::col<call::call>(),
							cxxlens::sdk::query::col<call::source>(),
							cxxlens::sdk::query::col<lock::lock>(),
							cxxlens::sdk::query::col<lock::mode>()};
	auto projected = std::move(*ordered).project(output);
	return projected && projected->ir().validate() ? 0 : 1;
}
