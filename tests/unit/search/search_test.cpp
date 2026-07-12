#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

#include <cxxlens/explain.hpp>
#include <cxxlens/search.hpp>

#include "search_fixture.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::test;

	void require(const bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] bool contains_code(const std::span<const unresolved> values,
									 const std::string_view code)
	{
		return std::ranges::any_of(values,
								   [code](const unresolved& value)
								   {
									   return value.stable_code == code;
								   });
	}

	[[nodiscard]] select::call_selector flagship()
	{
		return select::calls_to_method("Base", "step")
			.include_derived_types()
			.include_virtual_overrides()
			.dispatch(select::dispatch_policy::static_and_virtual_candidates);
	}

	void check_flagship_and_projections()
	{
		auto opened = search_fixture::open();
		require(opened.has_value(), "search workspace fixture did not open");
		auto fixture = std::move(opened.value());
		auto report = search::calls_to_method(fixture.value, "Base", "step");
		require(report.has_value(), "flagship public search failed");
		require(report.value().matches().size() == 2U,
				"flagship search did not return exactly two calls");
		require(fixture.worker->calls == 2U,
				"cold multi-variant search did not parse each variant");
		const auto base_call =
			std::ranges::find_if(report.value().matches(),
								 [](const call_site& call)
								 {
									 return call.direct_callee() == query_fixture::base_step;
								 });
		require(
			base_call != report.value().matches().end() &&
				base_call->possible_callees().size() == 2U &&
				std::ranges::contains(base_call->possible_callees(), query_fixture::derived_step),
			"public call row collapsed static and possible virtual targets");
		require(base_call->receiver_static_type().has_value(),
				"public call row dropped the receiver type");
		require(!base_call->location().validate().has_value(),
				"public call row dropped or corrupted the source span");
		require(!base_call->why().items().empty(), "public call row dropped structured evidence");
		require(report.value().guarantee() == result_guarantee::best_effort &&
					contains_code(report.value().unresolved_items(),
								  "search.open-world-virtual-target"),
				"open-world public report was presented as exact");
		require(report.value().coverage().complete(), "complete fixture coverage became partial");
		const auto json = report.value().to_json();
		const auto markdown = report.value().to_markdown();
		require(json.find("cxxlens.search-report.v1") != std::string::npos &&
					json.find(std::string{query_fixture::other_step.value()}) ==
						std::string::npos &&
					json.find("per_variant") != std::string::npos &&
					markdown.find("Matches: 2") != std::string::npos &&
					markdown.find("Coverage complete: yes") != std::string::npos &&
					markdown.find("search.open-world-virtual-target") != std::string::npos,
				"JSON/Markdown projection lost authoritative report state");

		auto repeated = search::calls(fixture.value, flagship());
		require(repeated && repeated.value().to_json() == json && fixture.worker->calls == 2U,
				"warm public search reparsed or changed canonical JSON");

		search_options limited_options;
		limited_options.result_limit = 1U;
		auto limited = search::calls(fixture.value, flagship(), limited_options);
		require(
			limited && limited.value().matches().size() == 1U &&
				contains_code(limited.value().unresolved_items(), "search.result-limit-exhausted"),
			"public result limit silently reported completeness");
		search_options compact_options;
		compact_options.include_unresolved = false;
		auto compact = search::calls(fixture.value, flagship(), compact_options);
		require(compact &&
					compact.value().to_json().find("search.open-world-virtual-target") !=
						std::string::npos &&
					compact.value().to_markdown().find("enable `include_unresolved` for details") !=
						std::string::npos &&
					compact.value().to_markdown().find("search.open-world-virtual-target") ==
						std::string::npos,
				"compact Markdown changed or hid the authoritative unresolved JSON state");

		auto no_function = search::calls_to_function(fixture.value, "fixture::missing");
		require(no_function && no_function.value().matches().empty() &&
					no_function.value().coverage().complete(),
				"empty free-function result was confused with unresolved coverage");

		auto hierarchy = search::inheritance(fixture.value, select::record("Base"));
		require(hierarchy && hierarchy.value().matches().size() == 1U &&
					hierarchy.value().matches().front().derived == query_fixture::derived_record,
				"public inheritance search did not select the derived edge");
		search_options no_edges;
		no_edges.result_limit = 0U;
		auto limited_hierarchy =
			search::inheritance(fixture.value, select::record("Base"), true, no_edges);
		require(limited_hierarchy && limited_hierarchy.value().matches().empty() &&
					contains_code(limited_hierarchy.value().unresolved_items(),
								  "search.result-limit-exhausted"),
				"inheritance result limit silently discarded an edge");
		auto override_report = search::overrides(fixture.value, select::method("Base::step"), true);
		require(override_report && override_report.value().matches().size() == 1U &&
					override_report.value().matches().front().overriding_method ==
						query_fixture::derived_step,
				"public reverse override search did not select the overriding method");

		auto why = explain::why_not_matched(fixture.value, select::semantic(flagship()));
		require(why && why.value().properties.at("considered") == "3" &&
					why.value().properties.at("matched") == "2" &&
					why.value().properties.at("rejected") == "1" &&
					why.value().to_json().find("select.type.canonical") != std::string::npos,
				"why-not-matched did not preserve predicate accounting");

		const auto selector_explanation =
			explain::selector(select::semantic(flagship()), explain::detail_level::agent);
		const auto card = explain::for_selector(select::semantic(flagship()));
		require(selector_explanation.to_json().find("cxxlens.explanation.v1") !=
						std::string::npos &&
					card.to_json().find("cxxlens.agent-task-card.v1") != std::string::npos,
				"selector explanation or agent task card is not versioned");
	}

	void check_partial_strict_and_cancellation()
	{
		auto opened = search_fixture::open(true, false);
		require(opened.has_value(), "partial search workspace fixture did not open");
		auto fixture = std::move(opened.value());
		auto partial = search::calls_to_method(fixture.value, "Base", "step");
		require(partial.has_value(), "partial fact coverage prevented a best-effort report");
		require(partial.value().guarantee() == result_guarantee::best_effort,
				"partial fact coverage was reported with an exact guarantee");
		require(!partial.value().coverage().complete(),
				"partial fact coverage was presented as complete");
		require(contains_code(partial.value().unresolved_items(),
							  "search.override-coverage-incomplete"),
				"partial override coverage did not produce its stable unresolved code");
		search_options strict;
		strict.strict_coverage = true;
		auto rejected = search::calls_to_method(fixture.value, "Base", "step", strict);
		require(!rejected && rejected.error().code.value == "search.required-facts-unavailable",
				"strict coverage accepted a partial report");

		std::stop_source stop;
		stop.request_stop();
		search_options cancelled;
		cancelled.execution.cancellation = stop.get_token();
		auto cancellation = search::calls(fixture.value, flagship(), cancelled);
		require(!cancellation && cancellation.error().code.value == "core.cancelled",
				"public search ignored pre-execution cancellation");
	}
} // namespace

int main()
{
	search_report<call_site> unbound;
	require(unbound.matches().empty() && !unbound.to_json().empty() &&
				!unbound.to_markdown().empty(),
			"unbound public report accessors are invalid");
	check_flagship_and_projections();
	check_partial_strict_and_cancellation();
	return 0;
}
