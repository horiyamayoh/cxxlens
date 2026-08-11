#include <array>
#include <utility>

#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_declaration.hpp>
#include <cxxlens/relations/cc_type_component.hpp>
#include <cxxlens/relations/company_lock_acquire.hpp>
#include <cxxlens/relations/core_claim_conflict.hpp>
#include <cxxlens/relations/core_differential_disagreement.hpp>
#include <cxxlens/relations/core_provider_execution.hpp>
#include <cxxlens/relations/core_unresolved.hpp>
#include <cxxlens/relations/source_origin.hpp>
#include <cxxlens/sdk/query.hpp>

int main()
{
	using call = cxxlens::cc::relations::call_site;
	using declaration = cxxlens::cc::relations::declaration;
	using type_component = cxxlens::cc::relations::type_component;
	using lock = cxxlens::company::relations::lock_acquire;
	using provider_execution = cxxlens::core::relations::provider_execution;
	using unresolved = cxxlens::core::relations::unresolved;
	using claim_conflict = cxxlens::core::relations::claim_conflict;
	using differential_disagreement = cxxlens::core::relations::differential_disagreement;
	using origin = cxxlens::source::relations::origin;
	auto calls = cxxlens::sdk::query::from<call>();
	auto locks = cxxlens::sdk::query::from<lock>();
	auto declarations = cxxlens::sdk::query::from<declaration>();
	auto components = cxxlens::sdk::query::from<type_component>();
	auto executions = cxxlens::sdk::query::from<provider_execution>();
	auto unresolved_rows = cxxlens::sdk::query::from<unresolved>();
	auto conflicts = cxxlens::sdk::query::from<claim_conflict>();
	auto disagreements = cxxlens::sdk::query::from<differential_disagreement>();
	auto origins = cxxlens::sdk::query::from<origin>();
	auto predicate = cxxlens::sdk::query::equals_present(
		cxxlens::sdk::query::col<call::caller>(), cxxlens::sdk::query::col<lock::function>());
	if (!calls || !locks || !declarations || !components || !executions || !unresolved_rows ||
		!conflicts || !disagreements || !origins || !predicate)
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
