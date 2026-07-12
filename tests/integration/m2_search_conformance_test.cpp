#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

#include <cxxlens/explain.hpp>
#include <cxxlens/search.hpp>
#include <cxxlens/testing.hpp>

#include "query/query_executor.hpp"
#include "workspace/provisioning.hpp"

namespace
{
	using namespace cxxlens;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error{std::string{message}};
	}

	[[nodiscard]] testing::workspace_fixture fixture(const bool reverse)
	{
		constexpr std::string_view header = R"cpp(#pragma once
struct Base { virtual int step(); };
struct Derived final : Base { int step() override; };
struct Other { int step(); };
#define INVOKE_STEP(value) (value).step()
)cpp";
		constexpr std::string_view main_source = R"cpp(#include "api.hpp"
int invoke(Base& base, Derived& derived, Other& other) {
  return base.step() + derived.step() + other.step();
}
int macro_invoke(Base& base) { return INVOKE_STEP(base); }
)cpp";
		constexpr std::string_view support_source = R"cpp(#include "api.hpp"
int Base::step() { return MODE; }
int Derived::step() { return MODE + 1; }
int Other::step() { return MODE + 2; }
)cpp";
		auto value = testing::workspace_fixture::cpp(std::string{main_source});
		if (reverse)
			return value.add_variant({"mode-two", {"-DMODE=2"}, {}})
				.add_variant({"mode-one", {"-DMODE=1"}, {}})
				.add_file("support.cpp", std::string{support_source})
				.add_header("api.hpp", std::string{header});
		return value.add_header("api.hpp", std::string{header})
			.add_file("support.cpp", std::string{support_source})
			.add_variant({"mode-one", {"-DMODE=1"}, {}})
			.add_variant({"mode-two", {"-DMODE=2"}, {}});
	}

	[[nodiscard]] std::string execute(const std::size_t jobs, const bool reverse)
	{
		execution_context context;
		context.parallelism = jobs;
		auto opened = fixture(reverse).open(context);
		require(opened.has_value(), "production search fixture did not open");
		auto workspace = std::move(opened.value());
		require(workspace.compile_units().size() == 4U,
				"search fixture lost the multi-TU x multi-variant universe");

		auto direct = search::calls_to_method(workspace, "Base", "step");
		if (!direct.has_value())
			throw std::runtime_error{"flagship direct search failed"};
		if (direct.value().matches().size() != 2U)
			throw std::runtime_error{
				"flagship search did not isolate Base and Derived direct spelling calls: matches=" +
				std::to_string(direct.value().matches().size())};
		require(direct.value().coverage().complete() &&
					direct.value().guarantee() == result_guarantee::best_effort,
				"open-world virtual search lost coverage or overstated exactness");
		const auto cold_trace = detail::workspace_provisioning_access::last_trace(workspace);
		require(cold_trace.scheduled == workspace.compile_units().size(),
				"cold flagship search repeated or omitted a compile-unit parse task");
		require(std::ranges::all_of(direct.value().matches(),
									[](const call_site& call)
									{
										return call.location().validate() == std::nullopt &&
											!call.why().items().empty();
									}),
				"production call results dropped source or evidence");

		auto include_macro_selector =
			select::calls_to_method("Base", "step")
				.include_derived_types()
				.include_virtual_overrides()
				.dispatch(select::dispatch_policy::static_and_virtual_candidates)
				.macro(select::macro_match_policy::include_with_origin);
		auto with_macro = search::calls(workspace, include_macro_selector);
		require(with_macro.has_value() && with_macro.value().matches().size() == 3U &&
					std::ranges::any_of(with_macro.value().matches(),
										[](const call_site& call)
										{
											return call.location().origin !=
												source_origin::directly_spelled;
										}),
				"macro policy did not preserve and select expansion origin");

		auto hierarchy = search::inheritance(workspace, select::record("Base"));
		auto override_report = search::overrides(workspace, select::method("Base::step"), true);
		require(hierarchy.has_value() && hierarchy.value().matches().size() == 1U &&
					override_report.has_value() && override_report.value().matches().size() == 1U,
				"production hierarchy/override search lost semantic edges");
		auto why = explain::why_not_matched(workspace, select::semantic(include_macro_selector));
		if (!why.has_value())
			throw std::runtime_error{"production why-not accounting failed"};
		if (why.value().properties.at("matched") != "3" ||
			why.value().properties.at("rejected") != "1")
			throw std::runtime_error{"production why-not accounting is incomplete: considered=" +
									 why.value().properties.at("considered") +
									 " matched=" + why.value().properties.at("matched") +
									 " rejected=" + why.value().properties.at("rejected") +
									 " unresolved=" + why.value().properties.at("unresolved")};

		const auto cold = direct.value().to_json();
		auto warm = search::calls_to_method(workspace, "Base", "step");
		require(warm.has_value() && warm.value().to_json() == cold,
				"warm production search changed canonical report bytes");
		const auto warm_trace = detail::workspace_provisioning_access::last_trace(workspace);
		require(warm_trace.scheduled == 0U,
				"warm flagship search scheduled duplicate parse/traversal work");
		detail::query::execution_options query_options;
		query_options.execution = context;
		auto internal_trace = detail::query::provision_compile_execute(
			workspace,
			select::semantic(select::calls_to_method("Base", "step")
								 .include_derived_types()
								 .include_virtual_overrides()
								 .dispatch(select::dispatch_policy::static_and_virtual_candidates)),
			query_options);
		require(internal_trace.has_value() &&
					internal_trace.value().trace.accounting.considered == 4U &&
					internal_trace.value().trace.refinements_requested == 0U,
				"flagship fact scan/refinement performance trace exceeded its bounded fixture");
		return cold + "\n" + why.value().to_json() + "\n" + cold_trace.to_json() + "\n" +
			warm_trace.to_json() + "\n" + internal_trace.value().trace.to_json() + "\n";
	}
} // namespace

int main(const int count, const char* const* arguments)
{
	try
	{
		if (count >= 2 && std::string_view{arguments[1]} == "--emit")
		{
			const auto jobs = count >= 3 ? static_cast<std::size_t>(std::stoul(arguments[2])) : 1U;
			const bool reverse = count >= 4 && std::string_view{arguments[3]} == "reverse";
			std::cout << execute(jobs, reverse);
			return EXIT_SUCCESS;
		}
		const auto canonical = execute(1U, false);
		for (const auto jobs : {2U, 8U})
			for (const bool reverse : {false, true})
				require(execute(jobs, reverse) == canonical,
						"production search changed with jobs or insertion order");
		return EXIT_SUCCESS;
	}
	catch (const std::exception& failure)
	{
		std::cerr << failure.what() << '\n';
		return EXIT_FAILURE;
	}
}
