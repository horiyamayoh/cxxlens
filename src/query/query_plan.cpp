#include "query_plan.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

#include "../core/canonical_json.hpp"
#include "../select/selector_ast.hpp"
#include "../workspace/value_access.hpp"

namespace cxxlens::detail::query
{
	namespace
	{
		using json_value = cxxlens::detail::json::json_value;
		using select::detail::node_operation;
		using select::detail::node_ptr;

		[[nodiscard]] error plan_error(std::string reason)
		{
			error failure;
			failure.code.value = "search.plan-compilation-failed";
			failure.message = "query plan invariant failed";
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		[[nodiscard]] std::string_view stage_name(const stage_kind value)
		{
			constexpr std::array names{"resolve_symbol",
									   "fact_index_scan",
									   "variant_join",
									   "hierarchy_closure",
									   "override_closure",
									   "candidate_filter",
									   "ast_refinement",
									   "confidence_assign",
									   "evidence_assemble",
									   "deduplicate_sort_limit"};
			return names.at(static_cast<std::size_t>(value));
		}

		void collect_predicates(const node_ptr& node, std::set<std::string>& output)
		{
			if (node->operation == node_operation::predicate)
				output.insert(node->predicate);
			for (const auto& operand : node->operands)
				collect_predicates(operand, output);
		}

		[[nodiscard]] bool has_prefix(const std::set<std::string>& values,
									  const std::string_view prefix)
		{
			return std::ranges::any_of(values,
									   [&](const std::string& value)
									   {
										   return value.starts_with(prefix);
									   });
		}

		[[nodiscard]] json_value string_array(const std::vector<std::string>& values)
		{
			json_value::array output;
			for (const auto& value : values)
				output.emplace_back(value);
			return output;
		}

		[[nodiscard]] std::string_view fact_name(const fact_kind value)
		{
			constexpr std::array names{"file",
									   "compile_command",
									   "symbol",
									   "type",
									   "declaration",
									   "definition",
									   "reference",
									   "call",
									   "construction",
									   "conversion",
									   "inheritance",
									   "override_relation",
									   "include_relation",
									   "macro_definition",
									   "macro_expansion",
									   "cfg_summary",
									   "flow_summary",
									   "effect_summary",
									   "dynamic_observation",
									   "coverage_region",
									   "custom"};
			return names.at(static_cast<std::size_t>(value));
		}

		[[nodiscard]] json_value fact_array(const std::vector<fact_kind>& values)
		{
			json_value::array output;
			for (const auto value : values)
				output.emplace_back(std::string{fact_name(value)});
			return output;
		}

		[[nodiscard]] std::string precision_name(const precision_level value)
		{
			constexpr std::array names{"ast_structural",
									   "local_semantic",
									   "workspace_semantic",
									   "local_flow",
									   "interprocedural_summary",
									   "path_sensitive",
									   "dynamic_observation"};
			return names.at(static_cast<std::size_t>(value));
		}
	} // namespace

	result<void> query_plan::validate() const
	{
		if (schema != "cxxlens.query-plan.v1" || selector_json.empty() ||
			reusable_subexpression_key.empty() || snapshot_key.empty() || stages.empty())
			return plan_error("required-field-missing");
		auto selector = select::semantic_selector::from_json(selector_json);
		if (!selector)
			return plan_error("selector-invalid");
		const auto selector_root = select::detail::selector_access::get(selector.value());
		if (!selector_root || selector_root->domain != select::detail::selector_domain::call)
			return plan_error("call-selector-required");
		const auto derived = selector.value().requirements();
		if (requirements.facts.to_json() != derived.facts.to_json() ||
			requirements.minimum_precision != derived.minimum_precision ||
			requirements.capabilities != derived.capabilities)
			return plan_error("selector-requirements-mismatch");
		const auto expected_reuse_key =
			"query.call.v1|" + selector_json + "|scope=" + options.scope.to_json();
		if (reusable_subexpression_key != expected_reuse_key)
			return plan_error("reuse-key-mismatch");
		if (options.candidate_budget == 0U || options.refinement_budget == 0U)
			return plan_error("budget-must-be-positive");
		std::set<std::string> seen;
		for (const auto& stage : stages)
		{
			if (stage.id.empty() || !seen.insert(stage.id).second)
				return plan_error("stage-id-invalid");
			for (const auto& input : stage.inputs)
				if (!seen.contains(input))
					return plan_error("stage-dependency-not-earlier");
			if (!std::ranges::is_sorted(stage.required_facts) ||
				std::ranges::adjacent_find(stage.required_facts) != stage.required_facts.end())
				return plan_error("stage-facts-not-canonical");
		}
		if (stages.back().kind != stage_kind::deduplicate_sort_limit)
			return plan_error("terminal-stage-missing");
		return {};
	}

	std::string query_plan::to_json() const
	{
		json_value::array stage_values;
		for (const auto& stage : stages)
			stage_values.emplace_back(
				json_value::object{{"estimated_cost", stage.estimated_cost},
								   {"id", stage.id},
								   {"inputs", string_array(stage.inputs)},
								   {"kind", std::string{stage_name(stage.kind)}},
								   {"optional", stage.optional},
								   {"required_facts", fact_array(stage.required_facts)}});
		json_value document{cxxlens::detail::json::envelope(
			{schema},
			{{"candidate_budget", static_cast<std::uint64_t>(options.candidate_budget)},
			 {"refinement_budget", static_cast<std::uint64_t>(options.refinement_budget)},
			 {"result_limit",
			  options.result_limit ? json_value{static_cast<std::uint64_t>(*options.result_limit)}
								   : json_value{}},
			 {"reusable_subexpression_key", reusable_subexpression_key},
			 {"selector", selector_json},
			 {"snapshot_key", snapshot_key},
			 {"minimum_precision", precision_name(requirements.minimum_precision)},
			 {"required_capabilities", string_array(requirements.capabilities)},
			 {"required_facts",
			  fact_array(
				  std::vector<fact_kind>{fact_profile_access::kinds(requirements.facts).begin(),
										 fact_profile_access::kinds(requirements.facts).end()})},
			 {"stages", std::move(stage_values)}})};
		return cxxlens::detail::json::write(document).value();
	}

	result<query_plan> compile(const select::semantic_selector& selector,
							   compile_options options,
							   std::string snapshot_key)
	{
		const auto root = select::detail::selector_access::get(selector);
		if (!root || root->domain != select::detail::selector_domain::call)
			return plan_error("call-selector-required");
		query_plan output;
		output.selector_json = selector.to_json();
		output.snapshot_key = std::move(snapshot_key);
		output.requirements = selector.requirements();
		output.options = std::move(options);
		output.reusable_subexpression_key =
			"query.call.v1|" + output.selector_json + "|scope=" + output.options.scope.to_json();

		std::set<std::string> predicates;
		collect_predicates(root, predicates);
		std::string previous;
		auto add_stage = [&](std::string id,
							 const stage_kind kind,
							 std::vector<fact_kind> facts,
							 const std::uint64_t cost,
							 const bool optional = false)
		{
			std::ranges::sort(facts);
			facts.erase(std::unique(facts.begin(), facts.end()), facts.end());
			query_stage stage{std::move(id), kind, {}, std::move(facts), cost, optional};
			if (!previous.empty())
				stage.inputs.push_back(previous);
			previous = stage.id;
			output.stages.push_back(std::move(stage));
		};

		if (has_prefix(predicates, "symbol.name") || has_prefix(predicates, "type.canonical") ||
			has_prefix(predicates, "call.method_name") ||
			has_prefix(predicates, "call.function_name") ||
			has_prefix(predicates, "call.callee_name"))
			add_stage("01.resolve-symbol",
					  stage_kind::resolve_symbol,
					  {fact_kind::symbol, fact_kind::type},
					  2U);
		add_stage("02.fact-index-scan", stage_kind::fact_index_scan, {fact_kind::call}, 1U);
		if (has_prefix(predicates, "call.variant_policy") ||
			has_prefix(predicates, "symbol.variant_policy"))
			add_stage("03.variant-join", stage_kind::variant_join, {fact_kind::call}, 2U);
		if (predicates.contains("call.include_derived_types") ||
			predicates.contains("type.derived_from") ||
			predicates.contains("type.including_derived"))
			add_stage("04.hierarchy-closure",
					  stage_kind::hierarchy_closure,
					  {fact_kind::inheritance},
					  3U);
		if (predicates.contains("call.include_virtual_overrides") ||
			predicates.contains("call.dispatch") || predicates.contains("symbol.overrides"))
			add_stage("05.override-closure",
					  stage_kind::override_closure,
					  {fact_kind::override_relation},
					  3U);
		add_stage("06.candidate-filter", stage_kind::candidate_filter, {}, 2U);
		if (output.options.allow_targeted_refinement &&
			(predicates.contains("call.include_virtual_overrides") ||
			 predicates.contains("call.dispatch") || predicates.contains("call.argument_type")))
			add_stage("07.ast-refinement", stage_kind::ast_refinement, {}, 8U, true);
		add_stage("08.confidence-assign", stage_kind::confidence_assign, {}, 1U);
		add_stage("09.evidence-assemble", stage_kind::evidence_assemble, {}, 1U);
		add_stage("10.deduplicate-sort-limit", stage_kind::deduplicate_sort_limit, {}, 1U);

		if (auto status = output.validate(); !status)
			return std::move(status.error());
		return output;
	}
} // namespace cxxlens::detail::query
