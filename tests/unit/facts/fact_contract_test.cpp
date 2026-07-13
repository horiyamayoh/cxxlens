#include "facts/fact_contract.hpp"

#include <array>
#include <iostream>
#include <string>

#include <cxxlens/facts.hpp>

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::facts;

	template <class Id>
	Id id(const std::string& prefix, char value)
	{
		return Id{prefix + "_" + std::string(64U, value)};
	}

	source_span span()
	{
		const auto file = id<file_id>("file", 'a');
		source_span value;
		value.primary = {source_point::at(file, 0U, 1U, 1U),
						 source_point::at(file, 2U, 1U, 3U),
						 source_range_kind::token};
		value.origin = source_origin::directly_spelled;
		value.digest = {"sha256", 1U, "abcd"};
		return value;
	}

	bool check(bool condition, const char* message)
	{
		if (!condition)
			std::cerr << message << '\n';
		return condition;
	}
} // namespace

int main(int argc, char** argv)
{
	bool passed = true;
	const name_identity usr_name{"ns::f", "c:@N@ns@F@f#", std::nullopt, std::nullopt, std::nullopt};
	const name_identity structural_name{
		"ns::f", std::nullopt, "symbol_owner", "function", "fn(i32)->void"};
	const name_identity pretty_name{
		"ns::f", std::nullopt, std::nullopt, std::nullopt, std::nullopt};
	passed &= check(validate(usr_name).has_value(), "USR identity rejected");
	passed &= check(validate(structural_name).has_value(), "structural name identity rejected");
	passed &= check(!validate(pretty_name), "qualified-name-only identity accepted");

	type_identity structural_type{
		"const T*", "ptr(const(record))", std::nullopt, {id<type_id>("type", 'a')}, false};
	type_identity pretty_type{"const T*", {}, std::nullopt, {}, false};
	passed &= check(validate(structural_type).has_value(), "structural TypeIR rejected");
	passed &= check(!validate(pretty_type), "pretty-type-only identity accepted");

	observation_record observation;
	observation.adapter_id = "clang22.semantic";
	observation.adapter_version = "1.0.0";
	observation.llvm_major = 22U;
	observation.compile_unit = id<compile_unit_id>("cu", 'a');
	observation.variant = id<build_variant_id>("variant", 'b');
	observation.kind = fact_kind::symbol;
	observation.payload_version = 1U;
	observation.payload = {{"semantic_key", "symbol-a"}};
	observation.name = usr_name;
	passed &= check(validate(observation).has_value(), "valid observation rejected");
	observation.payload = {{"clang_ast_pointer", "0x1234"}};
	passed &= check(!validate(observation), "native pointer observation accepted");
	observation.payload = {{"semantic_key", "clang::Decl* 0x12345678"}};
	passed &= check(!validate(observation), "native pointer value accepted");

	detached_fact_record detached;
	detached.id = id<fact_id>("fact", 'c');
	detached.kind = fact_kind::symbol;
	detached.stable_key = "symbol-a";
	detached.origin = {{id<compile_unit_id>("cu", 'a'), id<compile_unit_id>("cu", 'b')},
					   {id<build_variant_id>("variant", 'a')},
					   "clang22.symbol",
					   "1.0.0"};
	detached.payload_version = 1U;
	detached.name = usr_name;
	passed &= check(validate(detached).has_value(), "detached fact rejected");
	std::swap(detached.origin.compile_units[0], detached.origin.compile_units[1]);
	passed &= check(!validate(detached), "noncanonical provenance accepted");

	call_fact_record call;
	call.id = id<fact_id>("fact", 'd');
	call.kind = call_kind::virtual_member;
	call.location = span();
	call.direct_callee = id<symbol_id>("symbol", 'a');
	call.possible_callees = {id<symbol_id>("symbol", 'b'), id<symbol_id>("symbol", 'c')};
	call.receiver_static_type = id<type_id>("type", 'd');
	call.dispatch = dispatch_kind::virtual_candidate_set;
	call.certainty = confidence::high;
	call.guarantee = result_guarantee::sound_over_approximation;
	evidence_item item;
	item.kind = evidence_kind::call_resolution;
	item.summary = "virtual candidates";
	item.attributes = {{"resolution.kind", "virtual"}};
	call.why.add(std::move(item));
	passed &= check(validate(call).has_value(), "separated call resolution fields rejected");
	call.possible_callees.clear();
	passed &= check(!validate(call), "empty virtual candidate set accepted");

	passed &= check(resolve_dependency("coverage") == std::vector{fact_kind::coverage_region},
					"coverage dependency unresolved");
	passed &= check(resolve_dependency("control_flow") == std::vector{fact_kind::cfg_summary},
					"control-flow dependency unresolved");
	passed &= check(resolve_dependency("not_registered").empty(), "unknown dependency resolved");

	const auto forward = fact_profile::semantic_search().include(fact_kind::conversion).to_json();
	const auto reverse = fact_profile::minimal()
							 .include(fact_kind::conversion)
							 .include(fact_kind::symbol)
							 .include(fact_kind::call)
							 .exclude(fact_kind::call)
							 .include(fact_kind::call)
							 .to_json();
	passed &= check(forward.find("cxxlens.fact-profile.v1") != std::string::npos,
					"profile schema missing");
	const std::array profiles{
		fact_profile::minimal(),
		fact_profile::semantic_search(),
		fact_profile::refactor(),
		fact_profile::generation(),
		fact_profile::flow(),
		fact_profile::full(),
	};
	for (const auto& profile : profiles)
		passed &= check(!profile.to_json().empty(), "fact profile factory produced no contract");
	const auto adjusted = fact_profile::full()
							  .exclude(fact_kind::macro_expansion)
							  .include(fact_kind::macro_expansion)
							  .precision(precision_level::workspace_semantic);
	passed &= check(adjusted.to_json().find("workspace_semantic") != std::string::npos,
					"fact profile include/exclude/precision family is incomplete");
	if (argc == 2 && std::string_view{argv[1]} == "--emit")
		std::cout << fact_profile::semantic_search().to_json() << '\n';
	(void)reverse;
	return passed ? 0 : 1;
}
