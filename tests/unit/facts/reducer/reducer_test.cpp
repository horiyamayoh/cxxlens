#include "facts/reducer.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail;

	[[nodiscard]] std::string id(const std::string_view prefix, const char fill)
	{
		return std::string{prefix} + std::string(64U, fill);
	}

	const compile_unit_id unit_a{id("cu_", 'a')};
	const compile_unit_id unit_b{id("cu_", 'b')};
	const build_variant_id variant_a{id("variant_", 'a')};
	const build_variant_id variant_b{id("variant_", 'b')};
	const symbol_id symbol_a{id("symbol_", 'a')};
	const symbol_id symbol_b{id("symbol_", 'b')};
	const type_id type_a{id("type_", 'a')};

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] source_span span(const char file_fill, const std::uint64_t offset)
	{
		const file_id file{id("file_", file_fill)};
		source_span output;
		output.primary = {
			source_point::at(file, offset, 1U, static_cast<std::uint32_t>(offset + 1U)),
			source_point::at(file, offset + 1U, 1U, static_cast<std::uint32_t>(offset + 2U)),
			source_range_kind::token};
		output.origin = source_origin::directly_spelled;
		output.digest = {"fnv1a64", 1U, std::string(16U, file_fill)};
		return output;
	}

	[[nodiscard]] facts::observation_record
	observation(const fact_kind kind,
				std::string key,
				const compile_unit_id unit,
				const build_variant_id variant,
				std::map<std::string, std::string> payload = {})
	{
		facts::observation_record output;
		output.adapter_id = "clang22.frontend";
		output.adapter_version = "1.0.0";
		output.llvm_major = 22U;
		output.compile_unit = unit;
		output.variant = variant;
		output.kind = kind;
		output.payload_version = 1U;
		output.payload = std::move(payload);
		output.payload.emplace("semantic_key", std::move(key));
		output.payload.emplace("variant", std::string{variant.value()});
		output.coverage_contributions.push_back({"fixture-observation",
												 output.payload.at("semantic_key"),
												 coverage_state::covered,
												 {}});
		return output;
	}

	[[nodiscard]] frontend::observation_batch
	batch(const compile_unit_id unit,
		  const build_variant_id variant,
		  std::vector<facts::observation_record> observations)
	{
		std::ranges::sort(observations,
						  {},
						  [](const auto& value)
						  {
							  return std::to_string(static_cast<std::uint16_t>(value.kind)) + ":" +
								  value.payload.at("semantic_key");
						  });
		frontend::observation_batch output;
		output.adapter_id = "clang22.frontend";
		output.adapter_version = "1.0.0";
		output.unit = unit;
		output.variant = variant;
		output.observations = std::move(observations);
		output.coverage.parsed = 1U;
		require(output.validate().has_value(), "synthetic reducer batch is invalid");
		return output;
	}

	[[nodiscard]] facts::observation_record symbol_observation(const compile_unit_id unit,
															   const build_variant_id variant,
															   const symbol_id semantic_id,
															   std::string usr,
															   std::string linkage = "external")
	{
		auto output = observation(fact_kind::symbol,
								  "symbol:" + std::string{semantic_id.value()},
								  unit,
								  variant,
								  {{"symbol.id", std::string{semantic_id.value()}},
								   {"symbol.kind", "function"},
								   {"symbol.linkage", std::move(linkage)},
								   {"symbol.name", "same"},
								   {"symbol.qualified_name", "scope::same"}});
		output.name = facts::name_identity{"scope::same", std::move(usr), {}, {}, {}};
		return output;
	}

	[[nodiscard]] std::vector<frontend::observation_batch> equivalent_fixture()
	{
		std::vector<facts::observation_record> first;
		std::vector<facts::observation_record> second;
		const std::array kinds{fact_kind::file,
							   fact_kind::declaration,
							   fact_kind::reference,
							   fact_kind::call,
							   fact_kind::inheritance,
							   fact_kind::override_relation,
							   fact_kind::include_relation,
							   fact_kind::macro_definition,
							   fact_kind::macro_expansion};
		for (const auto kind : kinds)
		{
			const auto key = std::to_string(static_cast<std::uint16_t>(kind)) + ":fixture";
			first.push_back(observation(kind, key, unit_a, variant_a, {{"domain.value", "equal"}}));
			second.push_back(
				observation(kind, key, unit_b, variant_a, {{"domain.value", "equal"}}));
		}
		auto declared_symbol = symbol_observation(unit_a, variant_a, symbol_a, "c:@F@same#");
		declared_symbol.payload.emplace("symbol.definition", "false");
		first.push_back(std::move(declared_symbol));
		auto defined_symbol = symbol_observation(unit_b, variant_a, symbol_a, "c:@F@same#");
		defined_symbol.payload.emplace("symbol.definition", "true");
		second.push_back(std::move(defined_symbol));
		first.push_back(observation(
			fact_kind::definition,
			"definition:fixture-a",
			unit_a,
			variant_a,
			{{"symbol.id", std::string{symbol_a.value()}}, {"definition.canonical", "true"}}));
		second.push_back(observation(
			fact_kind::definition,
			"definition:fixture-b",
			unit_b,
			variant_a,
			{{"symbol.id", std::string{symbol_a.value()}}, {"definition.canonical", "true"}}));
		auto first_type =
			observation(fact_kind::type,
						"type:" + std::string{type_a.value()},
						unit_a,
						variant_a,
						{{"type.id", std::string{type_a.value()}}, {"type.kind", "builtin"}});
		first_type.type = facts::type_identity{"int", "q(---):builtin(17)", {}, {}, true};
		auto second_type = first_type;
		second_type.compile_unit = unit_b;
		first.push_back(std::move(first_type));
		second.push_back(std::move(second_type));
		return {batch(unit_a, variant_a, std::move(first)),
				batch(unit_b, variant_a, std::move(second))};
	}

	void check_equivalent_and_permutations()
	{
		auto input = equivalent_fixture();
		auto baseline = facts::reduce_observations(input);
		require(baseline && baseline.value().validate(), "equivalent reduction failed validation");
		require(baseline.value().facts.size() == 12U,
				"one fixture per M1 fact kind was not reduced exactly once");
		for (const auto& fact : baseline.value().facts)
			require(fact.origin.compile_units == std::vector<compile_unit_id>{unit_a, unit_b},
					"equivalent TU contributors were not sorted/unique");

		for (std::uint32_t iteration = 0U; iteration < 8U; ++iteration)
		{
			auto permuted = input;
			if ((iteration & 1U) != 0U)
				std::ranges::reverse(permuted);
			if ((iteration & 2U) != 0U)
				permuted.push_back(permuted.front());
			auto reduced = facts::reduce_observations(std::move(permuted));
			require(reduced && reduced.value().to_json() == baseline.value().to_json(),
					"permutation/partition/completion order or retry changed reduction");
		}
	}

	void check_semantic_identity_and_variants()
	{
		auto first = symbol_observation(unit_a, variant_a, symbol_a, "c:@N@left@F@same#I#");
		auto second = symbol_observation(unit_a, variant_a, symbol_b, "c:@N@right@F@same#I#");
		auto distinct = facts::reduce_observations(
			{batch(unit_a, variant_a, {std::move(first), std::move(second)})});
		require(distinct && distinct.value().facts.size() == 2U,
				"qualified-name-equal semantic identities merged");

		auto left = observation(
			fact_kind::call,
			"call:fixture",
			unit_a,
			variant_a,
			{{"call.direct_callee", std::string{symbol_a.value()}}, {"call.kind", "direct"}});
		auto right = observation(
			fact_kind::call,
			"call:fixture",
			unit_a,
			variant_b,
			{{"call.direct_callee", std::string{symbol_b.value()}}, {"call.kind", "direct"}});
		auto split = facts::reduce_observations({batch(unit_a, variant_a, {std::move(left)}),
												 batch(unit_a, variant_b, {std::move(right)})});
		require(split && split.value().facts.size() == 2U && split.value().conflicts.empty() &&
					split.value().trace.front().decision ==
						facts::reduction_decision::variant_split,
				"variant divergence was first-win collapsed or misclassified as conflict");

		auto equal_a = observation(
			fact_kind::call, "call:equal-variant", unit_a, variant_a, {{"call.kind", "direct"}});
		auto equal_b = observation(
			fact_kind::call, "call:equal-variant", unit_a, variant_b, {{"call.kind", "direct"}});
		auto shared = facts::reduce_observations({batch(unit_a, variant_a, {std::move(equal_a)}),
												  batch(unit_a, variant_b, {std::move(equal_b)})});
		require(shared && shared.value().facts.size() == 1U &&
					shared.value().facts.front().origin.variants.size() == 2U,
				"variant-equal payload failed to share sorted provenance");

		auto definition_a = observation(fact_kind::definition,
										"definition:variant-a",
										unit_a,
										variant_a,
										{{"symbol.id", std::string{symbol_a.value()}}});
		auto definition_b = definition_a;
		definition_a.source = span('a', 4U);
		definition_b.variant = variant_b;
		definition_b.payload["variant"] = std::string{variant_b.value()};
		definition_b.source = span('b', 4U);
		auto variant_definitions =
			facts::reduce_observations({batch(unit_a, variant_a, {std::move(definition_a)}),
										batch(unit_a, variant_b, {std::move(definition_b)})});
		require(variant_definitions && variant_definitions.value().facts.size() == 2U &&
					variant_definitions.value().conflicts.empty(),
				"variant-specific definitions were misclassified as same-variant ODR conflict");
	}

	void check_conflicts_and_coverage()
	{
		auto first = symbol_observation(unit_a, variant_a, symbol_a, "c:@F@same#", "external");
		auto second = symbol_observation(unit_b, variant_a, symbol_a, "c:@F@same#", "internal");
		auto conflict = facts::reduce_observations({batch(unit_a, variant_a, {std::move(first)}),
													batch(unit_b, variant_a, {std::move(second)})});
		require(conflict && conflict.value().facts.empty() &&
					conflict.value().conflicts.size() == 1U &&
					conflict.value().coverage.count(coverage_state::unresolved) == 2U,
				"authoritative conflict selected a winner or lost unresolved coverage");
		auto reversed_first =
			symbol_observation(unit_b, variant_a, symbol_a, "c:@F@same#", "internal");
		auto reversed_second =
			symbol_observation(unit_a, variant_a, symbol_a, "c:@F@same#", "external");
		auto reversed_conflict =
			facts::reduce_observations({batch(unit_b, variant_a, {std::move(reversed_first)}),
										batch(unit_a, variant_a, {std::move(reversed_second)})});
		require(reversed_conflict &&
					reversed_conflict.value().conflicts.front().id ==
						conflict.value().conflicts.front().id,
				"conflict identity depended on arrival order or diagnostic prose");

		auto definition_a = observation(
			fact_kind::definition,
			"definition:a",
			unit_a,
			variant_a,
			{{"symbol.id", std::string{symbol_a.value()}}, {"definition.canonical", "true"}});
		auto definition_b = definition_a;
		definition_b.compile_unit = unit_b;
		definition_a.source = span('a', 1U);
		definition_b.source = span('b', 1U);
		auto odr =
			facts::reduce_observations({batch(unit_a, variant_a, {std::move(definition_a)}),
										batch(unit_b, variant_a, {std::move(definition_b)})});
		require(odr && odr.value().facts.empty() && odr.value().conflicts.size() == 1U,
				"ODR-like definition conflict silently chose a source");

		auto partial =
			batch(unit_a,
				  variant_a,
				  {observation(fact_kind::reference, "reference:partial", unit_a, variant_a)});
		partial.coverage.parsed = 0U;
		partial.coverage.failed = 1U;
		partial.diagnostics.push_back({"clang.diagnostic.7",
									   frontend::diagnostic_severity::error,
									   "main.cpp",
									   2U,
									   3U,
									   "partial parse"});
		auto reduced_partial = facts::reduce_observations({std::move(partial)});
		require(reduced_partial &&
					reduced_partial.value().coverage.count(coverage_state::failed) == 1U &&
					!reduced_partial.value().diagnostics.empty(),
				"failed batch coverage/diagnostic propagation became complete");

		auto source_b = observation(fact_kind::declaration,
									"declaration:source-policy",
									unit_b,
									variant_a,
									{{"symbol.id", std::string{symbol_b.value()}}});
		auto source_a = source_b;
		source_b.source = span('b', 2U);
		source_a.compile_unit = unit_a;
		source_a.source = span('a', 2U);
		auto source_selected =
			facts::reduce_observations({batch(unit_b, variant_a, {std::move(source_b)}),
										batch(unit_a, variant_a, {std::move(source_a)})});
		require(source_selected && source_selected.value().facts.front().source &&
					source_selected.value().facts.front().source->primary.begin.file ==
						file_id{id("file_", 'a')},
				"source preference depended on completion order");
	}

	void check_invalid_and_non_identity_inputs()
	{
		auto input = equivalent_fixture();
		auto first = facts::reduce_observations(input);
		require(first.has_value(), "baseline identity reduction failed");
		for (auto& batch_value : input)
			batch_value.diagnostics.push_back({"clang.diagnostic.1",
											   frontend::diagnostic_severity::warning,
											   "main.cpp",
											   1U,
											   1U,
											   "different diagnostic prose"});
		auto second = facts::reduce_observations(input);
		require(second && first.value().facts.front().id == second.value().facts.front().id,
				"diagnostic prose changed fact identity");

		auto invalid = equivalent_fixture();
		invalid.front().observations.front().payload_version = 0U;
		require(!facts::reduce_observations(std::move(invalid)),
				"invalid observation was repaired by reducer");

		auto rooted = observation(fact_kind::file,
								  "file:rooted",
								  unit_a,
								  variant_a,
								  {{"file.path", "/tmp/checkout/main.cpp"}});
		require(!facts::reduce_observations({batch(unit_a, variant_a, {std::move(rooted)})}),
				"absolute checkout root entered fact identity");
	}
} // namespace

int main(const int argument_count, const char* const* arguments)
{
	check_equivalent_and_permutations();
	check_semantic_identity_and_variants();
	check_conflicts_and_coverage();
	check_invalid_and_non_identity_inputs();
	if (argument_count == 2 && std::string_view{arguments[1]} == "--emit")
	{
		auto result = cxxlens::detail::facts::reduce_observations(equivalent_fixture());
		require(result.has_value(), "golden reduction failed");
		std::cout << result.value().to_json() << '\n';
	}
	return 0;
}
