#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/core/evidence.hpp>

namespace
{
	auto check(const bool condition, const std::string& message) -> bool
	{
		if (!condition)
			std::cerr << message << '\n';
		return condition;
	}

	auto fact(const char value) -> cxxlens::fact_id
	{
		return cxxlens::fact_id{"fact_" + std::string(64U, value)};
	}

	auto source_span() -> cxxlens::source_span
	{
		const cxxlens::file_id file{"file_" + std::string(64U, 'd')};
		cxxlens::source_span span;
		span.primary = {cxxlens::source_point::at(file, 1U, 1U, 2U),
						cxxlens::source_point::at(file, 3U, 1U, 4U),
						cxxlens::source_range_kind::token};
		span.origin = cxxlens::source_origin::directly_spelled;
		span.digest = {"sha256", 1U, "abcd"};
		return span;
	}

	auto structured_item(const char id, std::string summary) -> cxxlens::evidence_item
	{
		cxxlens::evidence_item item;
		item.kind = cxxlens::evidence_kind::ast_binding;
		item.summary = std::move(summary);
		item.supporting_facts = {fact(id)};
		item.attributes = {{"binding.kind", "declaration"}};
		return item;
	}

	auto context() -> cxxlens::evidence_validation_context
	{
		cxxlens::evidence_validation_context result;
		result.snapshot_facts = {fact('a'), fact('b'), fact('c')};
		result.custom_factories = {"plugin.semantic-evidence"};
		return result;
	}

	auto material_evidence() -> cxxlens::evidence
	{
		cxxlens::evidence result;
		result.add(structured_item('a', "binding"));
		return result;
	}

	auto approximation_evidence() -> cxxlens::evidence
	{
		cxxlens::evidence_item item;
		item.kind = cxxlens::evidence_kind::approximation;
		item.summary = "open world";
		item.attributes = {{"approximation.kind", "open-world"}};
		cxxlens::evidence result;
		result.add(std::move(item));
		return result;
	}

	auto complete_report() -> cxxlens::coverage_report
	{
		cxxlens::coverage_report report;
		report.request({"symbol", "symbol-a"})
			.classify({"symbol", "symbol-a", cxxlens::coverage_state::covered, std::nullopt});
		return report;
	}

	auto partial_report(const cxxlens::coverage_state state) -> cxxlens::coverage_report
	{
		cxxlens::coverage_report report;
		report.request({"symbol", "symbol-a"})
			.classify({"symbol", "symbol-a", state, "semantic.not-resolved"});
		return report;
	}

	auto test_evidence_contract() -> bool
	{
		bool passed = true;
		cxxlens::evidence forward;
		forward.add(structured_item('c', "third"))
			.add(structured_item('a', "first"))
			.add(structured_item('b', "second"));
		cxxlens::evidence reverse;
		reverse.add(structured_item('b', "second"))
			.add(structured_item('a', "first"))
			.add(structured_item('c', "third"));
		passed &= check(forward.validate(context()).has_value(), "valid evidence rejected");
		passed &= check(forward.semantic_representation() == reverse.semantic_representation(),
						"insertion order changed evidence semantics");
		passed &= check(forward.to_json() == reverse.to_json(),
						"insertion order changed evidence projection");
		const auto before = forward.semantic_representation();
		forward.merge(reverse).merge(reverse);
		passed &=
			check(forward.semantic_representation() == before, "evidence union was not idempotent");

		cxxlens::evidence prose_a;
		prose_a.add(structured_item('a', "aaa"));
		cxxlens::evidence prose_b;
		prose_b.add(structured_item('a', "zzz"));
		passed &= check(prose_a.semantic_representation() == prose_b.semantic_representation(),
						"summary prose changed evidence identity");

		auto facts_out_of_order = structured_item('b', "facts");
		facts_out_of_order.supporting_facts = {fact('c'), fact('a'), fact('c')};
		cxxlens::evidence normalized;
		normalized.add(std::move(facts_out_of_order));
		passed &=
			check(normalized.items().front().supporting_facts == std::vector{fact('a'), fact('c')},
				  "supporting facts were not canonicalized");

		cxxlens::evidence missing_fact;
		missing_fact.add(structured_item('d', "not in snapshot"));
		passed &= check(!missing_fact.validate(context()), "snapshot membership was not checked");
		cxxlens::evidence prose_only;
		cxxlens::evidence_item prose;
		prose.kind = cxxlens::evidence_kind::ast_binding;
		prose.summary = "only words";
		prose_only.add(std::move(prose));
		passed &= check(!prose_only.validate(context()), "prose-only evidence accepted");
		cxxlens::evidence source_missing;
		cxxlens::evidence_item source;
		source.kind = cxxlens::evidence_kind::source;
		source.attributes = {{"source.role", "primary"}};
		source_missing.add(std::move(source));
		passed &=
			check(!source_missing.validate(context()), "source evidence without source accepted");
		cxxlens::evidence source_present;
		cxxlens::evidence_item source_row;
		source_row.kind = cxxlens::evidence_kind::source;
		source_row.source = source_span();
		source_present.add(std::move(source_row));
		passed &= check(source_present.validate(context()).has_value(),
						"normalized source evidence rejected");
		cxxlens::evidence custom;
		cxxlens::evidence_item extension;
		extension.kind = cxxlens::evidence_kind::custom;
		extension.attributes = {{"factory.id", "plugin.unknown"}};
		custom.add(std::move(extension));
		passed &= check(!custom.validate(context()), "unknown custom factory accepted");
		return passed;
	}

	auto test_coverage_equation() -> bool
	{
		bool passed = true;
		constexpr std::array states{cxxlens::coverage_state::covered,
									cxxlens::coverage_state::excluded,
									cxxlens::coverage_state::failed,
									cxxlens::coverage_state::unresolved,
									cxxlens::coverage_state::not_applicable};
		cxxlens::coverage_report report;
		for (std::size_t index = 0U; index < states.size(); ++index)
		{
			const auto id = "unit-" + std::to_string(index);
			report.request({"symbol", id});
			const auto reason = states.at(index) == cxxlens::coverage_state::covered
				? std::optional<std::string>{}
				: std::optional<std::string>{"coverage.fixture"};
			report.classify({"symbol", id, states.at(index), reason});
		}
		passed &= check(report.validate().has_value(), "coverage equation fixture rejected");
		for (const auto state : states)
			passed &= check(report.count(state) == 1U, "derived coverage count diverged");
		passed &= check(!report.complete(), "failed/unresolved coverage reported complete");

		cxxlens::coverage_report duplicate_request;
		duplicate_request.request({"symbol", "a"})
			.request({"symbol", "a"})
			.classify({"symbol", "a", cxxlens::coverage_state::covered, std::nullopt});
		passed &= check(!duplicate_request.validate(), "duplicate request accepted");
		cxxlens::coverage_report missing;
		missing.request({"symbol", "a"});
		passed &= check(!missing.validate(), "missing terminal state accepted");
		cxxlens::coverage_report multiple;
		multiple.request({"symbol", "a"})
			.classify({"symbol", "a", cxxlens::coverage_state::failed, "parse.failed"})
			.classify({"symbol", "a", cxxlens::coverage_state::unresolved, "semantic.unknown"});
		passed &= check(!multiple.validate(), "multiple terminal states accepted");
		cxxlens::coverage_report unrequested;
		unrequested.classify({"symbol", "a", cxxlens::coverage_state::covered, std::nullopt});
		passed &= check(!unrequested.validate(), "unrequested terminal row accepted");

		cxxlens::coverage_report empty_complete;
		cxxlens::coverage_report empty_excluded;
		empty_excluded.request({"symbol", "excluded"})
			.classify({"symbol", "excluded", cxxlens::coverage_state::excluded, "policy.excluded"});
		auto empty_unresolved = partial_report(cxxlens::coverage_state::unresolved);
		passed &= check(empty_complete.complete() && empty_complete.units().empty(),
						"empty-complete not observable");
		passed &= check(empty_excluded.complete() &&
							empty_excluded.count(cxxlens::coverage_state::excluded) == 1U,
						"empty-excluded not observable");
		passed &= check(!empty_unresolved.complete() &&
							empty_unresolved.count(cxxlens::coverage_state::unresolved) == 1U,
						"empty-unresolved not observable");
		return passed;
	}

	auto test_guarantee_table() -> bool
	{
		bool passed = true;
		const auto complete = complete_report();
		const auto partial = partial_report(cxxlens::coverage_state::unresolved);
		const auto why = material_evidence();
		const auto approximation = approximation_evidence();
		cxxlens::coverage_report capability_partial;
		capability_partial.request({"capability", "semantic.flow"})
			.classify({"capability",
					   "semantic.flow",
					   cxxlens::coverage_state::unresolved,
					   "core.capability-unavailable"});
		passed &= check(capability_partial.validate().has_value() && !capability_partial.complete(),
						"capability-unavailable partial fixture collapsed");
		passed &= check(
			cxxlens::validate_result_contract(cxxlens::result_guarantee::exact_within_coverage,
											  cxxlens::precision_level::workspace_semantic,
											  cxxlens::precision_level::local_semantic,
											  complete,
											  why)
				.has_value(),
			"compatible exact guarantee rejected");
		passed &= check(
			!cxxlens::validate_result_contract(cxxlens::result_guarantee::exact_within_coverage,
											   cxxlens::precision_level::workspace_semantic,
											   cxxlens::precision_level::local_semantic,
											   partial,
											   why),
			"exact guarantee accepted incomplete coverage");
		passed &= check(
			!cxxlens::validate_result_contract(cxxlens::result_guarantee::exact_within_coverage,
											   cxxlens::precision_level::ast_structural,
											   cxxlens::precision_level::workspace_semantic,
											   complete,
											   why),
			"exact guarantee accepted insufficient precision");
		passed &= check(
			!cxxlens::validate_result_contract(cxxlens::result_guarantee::exact_within_coverage,
											   cxxlens::precision_level::workspace_semantic,
											   cxxlens::precision_level::workspace_semantic,
											   complete,
											   {}),
			"exact guarantee accepted missing evidence");
		passed &= check(
			!cxxlens::validate_result_contract(cxxlens::result_guarantee::sound_over_approximation,
											   cxxlens::precision_level::ast_structural,
											   cxxlens::precision_level::workspace_semantic,
											   partial,
											   why),
			"sound approximation accepted without approximation evidence");
		passed &= check(
			cxxlens::validate_result_contract(cxxlens::result_guarantee::sound_under_approximation,
											  cxxlens::precision_level::ast_structural,
											  cxxlens::precision_level::workspace_semantic,
											  partial,
											  approximation)
				.has_value(),
			"sound approximation evidence rejected");
		passed &= check(
			cxxlens::validate_result_contract(cxxlens::result_guarantee::sound_over_approximation,
											  cxxlens::precision_level::ast_structural,
											  cxxlens::precision_level::workspace_semantic,
											  partial,
											  approximation)
				.has_value(),
			"sound over-approximation evidence rejected");
		passed &= check(cxxlens::validate_result_contract(cxxlens::result_guarantee::best_effort,
														  cxxlens::precision_level::ast_structural,
														  cxxlens::precision_level::path_sensitive,
														  partial,
														  why)
							.has_value(),
						"best-effort partial result rejected");
		passed &= check(cxxlens::validate_result_contract(cxxlens::result_guarantee::heuristic,
														  cxxlens::precision_level::ast_structural,
														  cxxlens::precision_level::path_sensitive,
														  partial,
														  why)
							.has_value(),
						"heuristic partial result rejected");
		return passed;
	}

	auto deterministic_projection(const std::size_t jobs) -> std::string
	{
		std::vector<cxxlens::coverage_report> coverage_chunks(jobs);
		std::vector<cxxlens::evidence> evidence_chunks(jobs);
		for (std::size_t index = 0U; index < 8U; ++index)
		{
			const auto id = "symbol-" + std::to_string(7U - index);
			auto& report = coverage_chunks.at(index % jobs);
			report.request({"symbol", id})
				.classify({"symbol", id, cxxlens::coverage_state::covered, std::nullopt});
			evidence_chunks.at(index % jobs)
				.add(structured_item(static_cast<char>('a' + static_cast<char>(index % 3U)), id));
		}
		cxxlens::coverage_report coverage;
		cxxlens::evidence why;
		for (std::size_t index = jobs; index > 0U; --index)
		{
			coverage.merge(coverage_chunks.at(index - 1U));
			why.merge(evidence_chunks.at(index - 1U));
		}
		return coverage.to_json() + '\n' + why.semantic_representation();
	}

	auto test_parallel_determinism() -> bool
	{
		const auto one = deterministic_projection(1U);
		return check(one == deterministic_projection(2U), "jobs=2 changed canonical rows") &&
			check(one == deterministic_projection(8U), "jobs=8 changed canonical rows");
	}
} // namespace

auto main(const int argc, const char* const* argv) -> int
{
	if (argc == 2 && std::string_view{argv[1]} == "--emit")
	{
		std::cout << deterministic_projection(8U) << '\n';
		return 0;
	}
	const bool passed = test_evidence_contract() && test_coverage_equation() &&
		test_guarantee_table() && test_parallel_determinism();
	return passed ? 0 : 1;
}
