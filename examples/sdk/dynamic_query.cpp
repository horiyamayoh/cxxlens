#include <array>
#include <utility>

#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/company_lock_acquire.hpp>
#include <cxxlens/sdk/query.hpp>

int main()
{
	cxxlens::sdk::relation_registry registry;
	if (!registry.add(cxxlens::cc::relations::call_site::descriptor()))
		return 1;
	if (!registry.add(cxxlens::company::relations::lock_acquire::descriptor()))
		return 1;
	auto calls = registry.require("cc.call_site", 1U);
	auto locks = registry.require("company.lock.acquire", 1U);
	if (!calls || !locks)
		return 1;
	auto caller = calls->column("caller");
	auto call = calls->column("call");
	auto source = calls->column("source");
	auto function = locks->column("function");
	auto acquire = locks->column("acquire");
	auto lock = locks->column("lock");
	auto mode = locks->column("mode");
	auto left = cxxlens::sdk::query::dynamic_query::from(*calls);
	auto right = cxxlens::sdk::query::dynamic_query::from(*locks);
	if (!caller || !call || !source || !function || !acquire || !lock || !mode || !left || !right)
		return 1;
	auto predicate = cxxlens::sdk::query::equals_present(*caller, *function);
	if (!predicate)
		return 1;
	auto joined = std::move(*left).inner_join(std::move(*right), std::move(*predicate));
	if (!joined)
		return 1;
	const std::array order{*call, *acquire};
	auto ordered = std::move(*joined).order_by(order);
	if (!ordered)
		return 1;
	const std::array output{*call, *source, *lock, *mode};
	auto projected = std::move(*ordered).project(output);
	return projected && projected->ir().validate() ? 0 : 1;
}
