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
	return 0;
}
