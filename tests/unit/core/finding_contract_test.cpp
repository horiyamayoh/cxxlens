#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/core/finding.hpp>

namespace
{
	auto check(const bool condition, const std::string& message) -> bool
	{
		if (!condition)
			std::cerr << message << '\n';
		return condition;
	}

	auto file(const char value) -> cxxlens::file_id
	{
		return cxxlens::file_id{"file_" + std::string(64U, value)};
	}

	auto fact(const char value) -> cxxlens::fact_id
	{
		return cxxlens::fact_id{"fact_" + std::string(64U, value)};
	}

	auto span(const char file_value = 'a', const std::uint64_t begin = 1U) -> cxxlens::source_span
	{
		cxxlens::source_span result;
		result.primary = {cxxlens::source_point::at(file(file_value), begin, 1U, 2U),
						  cxxlens::source_point::at(file(file_value), begin + 2U, 1U, 4U),
						  cxxlens::source_range_kind::token};
		result.origin = cxxlens::source_origin::directly_spelled;
		result.digest = {"sha256", 1U, "abcd"};
		return result;
	}

	auto validation_context() -> cxxlens::finding_validation_context
	{
		cxxlens::finding_validation_context context;
		context.evidence_context.snapshot_facts = {fact('a'), fact('b'), fact('c')};
		context.capabilities = {"semantic.ast"};
		return context;
	}

	auto input(const char subject = 'a', const char source = 'a') -> cxxlens::finding_input
	{
		cxxlens::finding_input result;
		result.rule_or_recipe = "rules.example";
		result.subject_semantic_id = "symbol_" + std::string(64U, subject);
		result.primary = span(source);
		result.variant_signature = "variant-debug";
		result.identity_parameters = {{"mode", "strict"}};
		result.level = cxxlens::severity::warning;
		result.certainty = cxxlens::confidence::high;
		result.guarantee = cxxlens::result_guarantee::best_effort;
		result.message = "display wording";
		cxxlens::evidence_item evidence_item;
		evidence_item.kind = cxxlens::evidence_kind::ast_binding;
		evidence_item.supporting_facts = {fact(subject)};
		evidence_item.attributes = {{"binding.kind", "declaration"}};
		result.why.add(std::move(evidence_item));
		cxxlens::unresolved uncertainty;
		uncertainty.kind = cxxlens::unresolved_kind::dependent_type;
		uncertainty.stable_code = "semantic.dependent-type";
		uncertainty.summary = "dependent";
		result.unresolved_items = {std::move(uncertainty)};
		result.coverage.request({"symbol", "subject"})
			.classify({"symbol",
					   "subject",
					   cxxlens::coverage_state::unresolved,
					   "semantic.dependent-type"});
		result.achieved_precision = cxxlens::precision_level::workspace_semantic;
		result.required_precision = cxxlens::precision_level::local_semantic;
		return result;
	}

	auto make(cxxlens::finding_input value) -> cxxlens::finding
	{
		return cxxlens::finding::make(std::move(value), validation_context()).value();
	}

	auto test_identity_contract() -> bool
	{
		bool passed = true;
		auto base_input = input();
		const auto base = make(base_input);
		auto prose = base_input;
		prose.message = "different words";
		prose.level = cxxlens::severity::fatal;
		passed &=
			check(make(std::move(prose)).id() == base.id(), "message/severity changed finding ID");

		auto subject = base_input;
		subject.subject_semantic_id = "symbol_" + std::string(64U, 'b');
		subject.why = {};
		cxxlens::evidence_item subject_evidence;
		subject_evidence.kind = cxxlens::evidence_kind::ast_binding;
		subject_evidence.supporting_facts = {fact('b')};
		subject_evidence.attributes = {{"binding.kind", "declaration"}};
		subject.why.add(std::move(subject_evidence));
		passed &= check(make(std::move(subject)).id() != base.id(), "subject did not change ID");
		auto source = base_input;
		source.primary = span('b');
		passed &= check(make(std::move(source)).id() != base.id(), "source did not change ID");
		auto variant = base_input;
		variant.variant_signature = "variant-release";
		passed &= check(make(std::move(variant)).id() != base.id(), "variant did not change ID");
		auto parameter = base_input;
		parameter.identity_parameters["mode"] = "relaxed";
		passed &= check(make(std::move(parameter)).id() != base.id(),
						"identity parameter did not change ID");
		return passed;
	}

	auto test_set_and_filters() -> bool
	{
		bool passed = true;
		auto low_input = input('a', 'c');
		low_input.level = cxxlens::severity::info;
		low_input.certainty = cxxlens::confidence::possible;
		const auto low = make(std::move(low_input));
		auto high_input = input('b', 'b');
		high_input.level = cxxlens::severity::error;
		high_input.certainty = cxxlens::confidence::certain;
		const auto high = make(std::move(high_input));
		cxxlens::finding_set set;
		passed &= check(set.add(high).has_value() && set.add(low).has_value(),
						"valid findings were rejected");
		passed &= check(set.add(high).has_value() && set.size() == 2U,
						"equivalent duplicate was not exactly once");
		const auto original_low = low.semantic_representation();
		const auto confidence_filtered = set.minimum_confidence(cxxlens::confidence::high);
		const auto severity_filtered = set.minimum_severity(cxxlens::severity::warning);
		passed &= check(set.size() == 2U && confidence_filtered.size() == 1U &&
							severity_filtered.size() == 1U,
						"pure filter count/source mutation failure");
		passed &= check(confidence_filtered.all().front().why().semantic_representation() ==
								high.why().semantic_representation() &&
							confidence_filtered.all().front().coverage().to_json() ==
								high.coverage().to_json() &&
							confidence_filtered.all().front().unresolved_items().size() ==
								high.unresolved_items().size(),
						"filter dropped evidence/coverage/unresolved");
		passed &= check(set.minimum_confidence(cxxlens::confidence::high)
								.minimum_confidence(cxxlens::confidence::high)
								.size() == confidence_filtered.size(),
						"confidence filter was not idempotent");
		passed &=
			check(low.semantic_representation() == original_low, "filter mutated source finding");

		auto conflict_input = input('b', 'b');
		conflict_input.level = cxxlens::severity::error;
		conflict_input.certainty = cxxlens::confidence::certain;
		conflict_input.message = "conflicting payload";
		const auto conflict = make(std::move(conflict_input));
		passed &= check(conflict.id() == high.id() && !set.add(conflict),
						"conflicting duplicate did not hard-fail");
		return passed;
	}

	auto ordered_ids(const std::vector<std::size_t>& order) -> std::vector<std::string>
	{
		const std::array values{
			make(input('a', 'c')), make(input('b', 'a')), make(input('c', 'b'))};
		cxxlens::finding_set set;
		for (const auto index : order)
			(void)set.add(values.at(index));
		std::vector<std::string> ids;
		for (const auto& value : set.all())
			ids.emplace_back(value.id().value());
		return ids;
	}

	auto test_total_order_and_parallelism() -> bool
	{
		const auto canonical = ordered_ids({0U, 1U, 2U});
		bool passed =
			check(canonical == ordered_ids({2U, 0U, 1U}), "insertion order changed total order");
		passed &= check(canonical == ordered_ids({1U, 2U, 0U}), "permutation changed total order");
		for (const std::size_t jobs : {1U, 2U, 8U})
		{
			std::vector<std::vector<std::size_t>> chunks(jobs);
			for (std::size_t index = 0U; index < 3U; ++index)
				chunks.at(index % jobs).push_back(2U - index);
			std::vector<std::size_t> completion;
			for (std::size_t index = jobs; index > 0U; --index)
				completion.insert(
					completion.end(), chunks.at(index - 1U).begin(), chunks.at(index - 1U).end());
			passed &= check(canonical == ordered_ids(completion),
							"jobs=" + std::to_string(jobs) + " changed total order");
		}
		return passed;
	}

	auto test_classification() -> bool
	{
		cxxlens::diagnostic observation;
		observation.id = "clang.warning";
		observation.message = "compiler prose";
		cxxlens::finding_set findings;
		cxxlens::result<cxxlens::finding_set> empty{findings};
		cxxlens::error operation_error;
		operation_error.code.value = "core.invalid-argument";
		operation_error.message = "bad input";
		cxxlens::result<cxxlens::finding_set> failed{std::move(operation_error)};
		return check(observation.validate().has_value() && findings.empty(),
					 "diagnostic became a finding") &&
			check(empty && empty.value().empty() && !failed,
				  "empty finding set and failed operation collapsed");
	}
} // namespace

auto main(const int argc, const char* const* argv) -> int
{
	if (argc == 2 && std::string_view{argv[1]} == "--emit")
	{
		for (const auto& id : ordered_ids({2U, 0U, 1U}))
			std::cout << id << '\n';
		return 0;
	}
	const bool passed = test_identity_contract() && test_set_and_filters() &&
		test_total_order_and_parallelism() && test_classification();
	return passed ? 0 : 1;
}
