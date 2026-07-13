#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

#include <cxxlens/select.hpp>

#include "../../../src/select/selector_ast.hpp"

namespace
{
	void require(const bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	void replace_once(std::string& value, const std::string& from, const std::string& to)
	{
		const auto position = value.find(from);
		require(position != std::string::npos, "replacement source missing");
		value.replace(position, from.size(), to);
	}
} // namespace

int main()
{
	using namespace cxxlens;
	using namespace cxxlens::select;

	const auto original = any_symbol();
	const auto changed = original.kind(symbol_kind::method).name("Base::start");
	require(original.to_json() != changed.to_json(), "builder mutated or failed to add predicate");
	require(original.to_json() == any_symbol().to_json(), "original selector changed");

	const auto left = any_symbol().all_of({method("B::f"), record("R")});
	const auto right = any_symbol().all_of({record("R"), method("B::f"), method("B::f")});
	require(left.to_json() == right.to_json(), "commutative canonicalization failed");
	const auto normalized = semantic_selector::from_json(left.to_json());
	require(normalized && normalized.value().to_json() == left.to_json(),
			"normalization is not idempotent");
	require(normalized.value().requirements().facts.to_json() ==
				left.requirements().facts.to_json(),
			"round-trip requirements changed");

	auto legacy_json = changed.to_json();
	replace_once(legacy_json, "cxxlens.selector.v1", "cxxlens.selector.v0");
	const auto migrated = semantic_selector::from_json(legacy_json);
	require(migrated && migrated.value().to_json() == changed.to_json(), "v0 migration failed");

	auto unknown_schema = changed.to_json();
	replace_once(unknown_schema, "cxxlens.selector.v1", "cxxlens.selector.v9");
	const auto schema_failure = semantic_selector::from_json(unknown_schema);
	require(!schema_failure && schema_failure.error().code.value == "select.invalid-expression",
			"unknown schema did not fail closed");

	auto invalid_policy = any_call().to_json();
	replace_once(invalid_policy, "direct_only", "invented_dispatch");
	const auto policy_failure = semantic_selector::from_json(invalid_policy);
	require(!policy_failure && policy_failure.error().attributes.at("reason") == "unknown-policy",
			"unknown policy did not produce stable error");

	auto mismatched = calls_to_method("Base", "start").to_json();
	replace_once(mismatched, "type.any", "symbol.any");
	replace_once(mismatched, "select.type.any", "select.symbol.any");
	const auto mismatch_failure = semantic_selector::from_json(mismatched);
	require(!mismatch_failure && mismatch_failure.error().code.value == "select.type-mismatch",
			"nested domain mismatch was accepted");

	const auto flagship = calls_to_method("Base", "start")
							  .include_derived_types()
							  .include_virtual_overrides()
							  .dispatch(dispatch_policy::static_and_virtual_candidates);
	const auto flagship_requirements = flagship.requirements();
	const auto expected_facts = std::string{
		R"({"kinds":["call","inheritance","override_relation","type"],"library_version":"0.1.0","precision":"workspace_semantic","schema":"cxxlens.fact-profile.v1","semantics_version":"1.0.0"})"};
	require(flagship_requirements.facts.to_json() == expected_facts,
			"flagship fact requirements are not exact");
	require(flagship_requirements.minimum_precision == precision_level::workspace_semantic,
			"flagship precision is not workspace semantic");
	require(flagship_requirements.capabilities.empty(), "flagship invented a capability");

	const auto erased = semantic(flagship);
	require(erased.to_json() == flagship.to_json(), "type erasure changed structural identity");
	require(erased.requirements().facts.to_json() == flagship.requirements().facts.to_json(),
			"type erasure changed requirements");

	const auto default_call = any_call();
	require(default_call.to_json() == default_call.macro(macro_match_policy::exclude).to_json(),
			"explicit default macro policy changed identity");
	const auto macro_call = default_call.macro(macro_match_policy::include_with_origin);
	require(macro_call.to_json() != default_call.to_json(),
			"relevant macro policy did not change identity");
	require(macro_call.requirements().facts.to_json().find("macro_expansion") != std::string::npos,
			"macro policy did not add macro fact requirement");
	const auto implicit_call =
		default_call.implicit(implicit_node_policy::include_language_implicit);
	require(implicit_call.to_json() != default_call.to_json(),
			"implicit policy did not change identity");
	require(implicit_call.requirements().facts.to_json() ==
				default_call.requirements().facts.to_json(),
			"implicit policy invented requirements");

	require(any_symbol().any_of({}).to_json().find("\"value\":false") != std::string::npos,
			"empty any is not false");
	require(any_symbol().negate().negate().to_json() == any_symbol().to_json(),
			"double negation did not normalize");

	using namespace cxxlens::select::detail;
	const auto a = predicate_node(selector_domain::symbol, "symbol.any");
	const auto b = predicate_node(selector_domain::symbol, "symbol.defined", {{"value", "true"}});
	const std::map<std::string, selector_truth> ambiguous_b{
		{"select.symbol.any", selector_truth::matched}};
	require(evaluate_truth(all_node(selector_domain::symbol, {a, b}), ambiguous_b) ==
				selector_truth::ambiguous,
			"all truth table lost ambiguity");
	require(evaluate_truth(any_node(selector_domain::symbol, {a, b}), ambiguous_b) ==
				selector_truth::matched,
			"any truth table is incorrect");
	require(evaluate_truth(negate_node(selector_domain::symbol, b), ambiguous_b) ==
				selector_truth::ambiguous,
			"negate truth table lost ambiguity");
	const std::map<std::string, selector_truth> false_b{
		{"select.symbol.any", selector_truth::matched},
		{"select.symbol.defined", selector_truth::not_matched}};
	require(evaluate_truth(all_node(selector_domain::symbol, {a, b}), false_b) ==
				selector_truth::not_matched,
			"all false branch is incorrect");
	require(evaluate_truth(negate_node(selector_domain::symbol, b), false_b) ==
				selector_truth::matched,
			"negate false branch is incorrect");

	require(!flagship.explain().empty() &&
				flagship.explain().find("select.call.method-name") != std::string::npos,
			"predicate reason explanation is missing");

	const auto file_surface = file_selector{}
								  .path_exact("src/main.cpp")
								  .path_glob("src/*.cpp")
								  .generated(false)
								  .system(false)
								  .any_of({file_selector{}.path_exact("src/main.cpp")})
								  .negate();
	require(!file_surface.to_json().empty(), "file selector family surface is incomplete");

	const auto symbol_surface = any_symbol()
									.kind(symbol_kind::method)
									.kinds({symbol_kind::method, symbol_kind::function})
									.name("Base::step", name_match::qualified_exact)
									.declared_in(file_selector{}.path_glob("include/*.hpp"))
									.defined()
									.member_of(record("Base"))
									.derived_from(record("Root"))
									.overrides(method("Root::step"))
									.public_surface()
									.macro(macro_match_policy::include_with_origin)
									.variants(variant_match_policy::report_per_variant)
									.any_of({function("f"), method("Base::step")})
									.all_of({record("Base")})
									.negate();
	require(!symbol_surface.to_json().empty() && !symbol_surface.explain().empty() &&
				!symbol_surface.requirements().facts.to_json().empty(),
			"symbol selector family surface is incomplete");

	const auto type_surface = type_selector{}
								  .canonical("const Base*")
								  .spelling("Base const*")
								  .declared_as(record("Base"))
								  .pointer_to(type("Base"))
								  .reference_to(type("Base"))
								  .const_qualified()
								  .derived_from(record("Root"))
								  .including_derived()
								  .convertible_to(type("Root"))
								  .specialization_of("Box")
								  .any_cvref();
	require(!type_surface.to_json().empty() && !type_surface.explain().empty() &&
				!type_surface.requirements().facts.to_json().empty(),
			"type selector family surface is incomplete");

	const auto call_surface =
		any_call()
			.kind(call_kind::member)
			.kinds({call_kind::member, call_kind::virtual_member})
			.callee(method("Base::step"))
			.callee_name("Base::step")
			.function_name("fixture::run")
			.method_name("step")
			.receiver_type(type("Base"))
			.include_derived_types()
			.include_virtual_overrides()
			.dispatch(dispatch_policy::static_and_virtual_candidates)
			.argument_type(0U, type("int"))
			.inside(function("fixture::caller"))
			.in_file(file_selector{}.path_exact("src/main.cpp"))
			.implicit(implicit_node_policy::include_language_implicit)
			.macro(macro_match_policy::include_with_origin)
			.templates(template_selection_policy::patterns_and_observed_instantiations)
			.variants(variant_match_policy::report_per_variant)
			.precision(precision_level::workspace_semantic);
	require(!call_surface.to_json().empty() && !call_surface.explain().empty() &&
				!call_surface.requirements().facts.to_json().empty(),
			"call selector family surface is incomplete");

	const std::array helper_json{
		any_symbol().to_json(),
		function("f").to_json(),
		method("C::f").to_json(),
		record("C").to_json(),
		variable("v").to_json(),
		macro("M").to_json(),
		type("int").to_json(),
		any_call().to_json(),
		calls_to(function("f")).to_json(),
		calls_to_function("f").to_json(),
		calls_to_method("C", "f").to_json(),
	};
	for (const auto& json : helper_json)
		require(!json.empty(), "selector helper family member produced no contract");
	const std::array erased_json{
		semantic(file_selector{}).to_json(),
		semantic(any_symbol()).to_json(),
		semantic(type("int")).to_json(),
		semantic(any_call()).to_json(),
	};
	for (const auto& json : erased_json)
		require(!json.empty(), "semantic type-erasure overload produced no contract");
	return 0;
}
