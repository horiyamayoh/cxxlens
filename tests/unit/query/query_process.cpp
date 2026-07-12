#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

#include <cxxlens/select.hpp>

#include "graph/virtual_candidate_resolver.hpp"
#include "query/query_executor.hpp"
#include "query/query_plan.hpp"
#include "query_fixture.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail;
	using namespace cxxlens::test::query_fixture;

	[[nodiscard]] override_edge
	relation(const symbol_id& overriding, const symbol_id& overridden, const std::uint64_t offset)
	{
		override_edge value;
		value.overriding_method = overriding;
		value.overridden_method = overridden;
		value.source = span(offset);
		return value;
	}
} // namespace

int main(const int count, char** arguments)
{
	const bool reverse = count > 1 && std::string_view{arguments[1]} == "reverse";
	const auto calls = reverse
		? select::calls_to_method("Base", "step")
			  .dispatch(select::dispatch_policy::static_and_virtual_candidates)
			  .include_virtual_overrides()
			  .include_derived_types()
		: select::calls_to_method("Base", "step")
			  .include_derived_types()
			  .include_virtual_overrides()
			  .dispatch(select::dispatch_policy::static_and_virtual_candidates);
	const auto selector = select::semantic(calls);
	query::compile_options compile_options;
	compile_options.candidate_budget = 16U;
	compile_options.refinement_budget = 8U;
	auto compiled = query::compile(selector, compile_options, "snapshot:fixture");
	if (!compiled)
		return 1;

	std::vector<override_edge> edges{relation(derived_step, base_step, 20U),
									 relation(other_step, base_step, 21U)};
	if (reverse)
		std::ranges::reverse(edges);
	graph::virtual_candidate_resolver resolver{std::move(edges)};
	graph::candidate_request candidate_request;
	candidate_request.call_key = "fixture-call";
	candidate_request.static_target = base_step;
	candidate_request.form = graph::dispatch_form::virtual_open;
	candidate_request.closed_world = true;
	auto resolution = resolver.resolve(std::move(candidate_request));
	if (!resolution.validate())
		return 2;

	query::execution_options execution_options;
	execution_options.candidate_budget = 16U;
	execution_options.refinement_budget = 8U;
	execution_options.closed_world = false;
	execution_options.execution.parallelism = count > 2 && std::string_view{arguments[2]} == "8"
		? 8U
		: count > 2 && std::string_view{arguments[2]} == "2" ? 2U
															 : 1U;
	auto executed =
		query::execute(compiled.value(), fact_store_fixture(true, reverse), execution_options);
	if (!executed)
		return 3;

	std::cout << compiled.value().to_json() << '\n'
			  << resolution.to_json() << '\n'
			  << executed.value().trace.to_json() << '\n'
			  << executed.value().to_json() << '\n';
	return 0;
}
