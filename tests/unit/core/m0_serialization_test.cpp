#include <iostream>
#include <string>

#include <cxxlens/core/finding.hpp>

namespace
{
	auto file() -> cxxlens::file_id
	{
		return cxxlens::file_id{"file_" + std::string(64U, 'a')};
	}

	auto fact() -> cxxlens::fact_id
	{
		return cxxlens::fact_id{"fact_" + std::string(64U, 'b')};
	}

	auto span() -> cxxlens::source_span
	{
		cxxlens::source_span value;
		value.primary = {cxxlens::source_point::at(file(), 2U, 1U, 3U),
						 cxxlens::source_point::at(file(), 5U, 1U, 6U),
						 cxxlens::source_range_kind::token};
		value.origin = cxxlens::source_origin::directly_spelled;
		value.digest = {"sha256", 1U, "abcd"};
		return value;
	}

	auto uncertainty() -> cxxlens::unresolved
	{
		cxxlens::unresolved value;
		value.kind = cxxlens::unresolved_kind::dependent_type;
		value.stable_code = "semantic.dependent-type";
		value.summary = "dependent type";
		value.related = {span()};
		value.suggested_actions = {"analyze.with-instantiation"};
		return value;
	}

	auto why() -> cxxlens::evidence
	{
		cxxlens::evidence_item item;
		item.kind = cxxlens::evidence_kind::ast_binding;
		item.summary = "bound declaration";
		item.source = span();
		item.supporting_facts = {fact()};
		item.attributes = {{"binding.kind", "declaration"}};
		cxxlens::evidence result;
		result.add(std::move(item));
		return result;
	}

	auto coverage() -> cxxlens::coverage_report
	{
		cxxlens::coverage_report value;
		value.request({"symbol", "subject"})
			.classify({"symbol",
					   "subject",
					   cxxlens::coverage_state::unresolved,
					   "semantic.dependent-type"});
		return value;
	}
} // namespace

auto main() -> int
{
	const auto source = span();
	cxxlens::error failure;
	failure.code.value = "core.invalid-argument";
	failure.message = "invalid request";
	failure.locations = {source};
	failure.suggested_actions = {"fix.request"};
	failure.attributes = {{"argument.name", "selector"}};
	const auto unresolved = uncertainty();
	const auto evidence = why();
	const auto accounting = coverage();
	cxxlens::diagnostic diagnostic;
	diagnostic.id = "clang.warning";
	diagnostic.message = "compiler observation";
	diagnostic.primary = source;
	diagnostic.compiler_option = "-Wexample";

	cxxlens::finding_input input;
	input.rule_or_recipe = "rules.example";
	input.subject_semantic_id = "symbol_" + std::string(64U, 'c');
	input.primary = source;
	input.variant_signature = "variant-debug";
	input.identity_parameters = {{"mode", "strict"}};
	input.level = cxxlens::severity::warning;
	input.certainty = cxxlens::confidence::high;
	input.guarantee = cxxlens::result_guarantee::best_effort;
	input.message = "semantic finding";
	input.why = evidence;
	input.unresolved_items = {unresolved};
	input.coverage = accounting;
	input.achieved_precision = cxxlens::precision_level::local_semantic;
	input.required_precision = cxxlens::precision_level::workspace_semantic;
	cxxlens::finding_validation_context context;
	context.evidence_context.snapshot_facts = {fact()};
	const auto finding = cxxlens::finding::make(std::move(input), context);
	if (!finding || !failure.validate(cxxlens::common_error_codes()) || !unresolved.validate({}) ||
		!evidence.validate(context.evidence_context) || !accounting.validate() ||
		!diagnostic.validate())
		return 1;

	std::cout << source.to_canonical_json() << '\n'
			  << failure.to_json() << '\n'
			  << unresolved.to_json() << '\n'
			  << evidence.to_json() << '\n'
			  << accounting.to_json() << '\n'
			  << diagnostic.to_json() << '\n'
			  << finding.value().to_json() << '\n';
	return 0;
}
