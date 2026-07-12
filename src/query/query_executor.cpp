#include "query_executor.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

#include "../core/canonical_json.hpp"
#include "../core/json_projections.hpp"
#include "../runtime/hash_port.hpp"
#include "../select/selector_ast.hpp"

namespace cxxlens::detail::query
{
	namespace
	{
		using json_value = cxxlens::detail::json::json_value;
		using select::detail::node_operation;
		using select::detail::node_ptr;

		struct evaluation_context
		{
			const call_site& call;
			const std::map<std::string, symbol>& symbols;
			const std::vector<inheritance_edge>& inheritance;
			bool include_derived{};
		};

		using evaluation = predicate_evaluation;

		[[nodiscard]] error query_error(std::string code, std::string reason)
		{
			error failure;
			failure.code.value = std::move(code);
			failure.message = "query execution failed";
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		[[nodiscard]] unresolved partial(std::string code,
										 unresolved_kind kind = unresolved_kind::custom,
										 std::string missing = {})
		{
			unresolved item;
			item.kind = kind;
			item.stable_code = std::move(code);
			item.summary = "query result is partial";
			if (!missing.empty())
				item.missing_inputs.push_back(std::move(missing));
			return item;
		}

		[[nodiscard]] std::string argument(const node_ptr& node, const std::string_view key)
		{
			const auto found = std::ranges::find(
				node->arguments, key, &std::pair<std::string, std::string>::first);
			return found == node->arguments.end() ? std::string{} : found->second;
		}

		[[nodiscard]] std::optional<std::string> find_argument(const node_ptr& node,
															   const std::string_view predicate,
															   const std::string_view key)
		{
			if (node->operation == node_operation::predicate && node->predicate == predicate)
				return argument(node, key);
			for (const auto& operand : node->operands)
				if (auto value = find_argument(operand, predicate, key))
					return value;
			return std::nullopt;
		}

		[[nodiscard]] std::optional<std::string> find_conjunctive_argument(
			const node_ptr& node, const std::string_view predicate, const std::string_view key)
		{
			if (node->operation == node_operation::predicate)
				return node->predicate == predicate ? std::optional{argument(node, key)}
													: std::nullopt;
			if (node->operation != node_operation::all)
				return std::nullopt;
			for (const auto& operand : node->operands)
				if (auto value = find_conjunctive_argument(operand, predicate, key))
					return value;
			return std::nullopt;
		}

		[[nodiscard]] bool contains_csv(const std::string_view csv, const std::string_view expected)
		{
			std::size_t begin{};
			while (begin < csv.size())
			{
				const auto end = csv.find(',', begin);
				if (csv.substr(begin, end - begin) == expected)
					return true;
				if (end == std::string_view::npos)
					break;
				begin = end + 1U;
			}
			return false;
		}

		[[nodiscard]] std::string_view call_kind_name(const call_kind value)
		{
			constexpr std::array names{"direct_function",
									   "member",
									   "virtual_member",
									   "constructor",
									   "destructor",
									   "overloaded_operator",
									   "builtin_operator",
									   "function_pointer",
									   "callback",
									   "modeled",
									   "unknown"};
			return names.at(static_cast<std::size_t>(value));
		}

		[[nodiscard]] bool is_descendant(const symbol_id& derived,
										 const symbol_id& base,
										 const std::vector<inheritance_edge>& edges)
		{
			if (derived == base)
				return true;
			std::vector<symbol_id> work{derived};
			std::set<std::string> visited;
			while (!work.empty())
			{
				auto current = std::move(work.back());
				work.pop_back();
				if (!visited.insert(std::string{current.value()}).second)
					continue;
				for (const auto& edge : edges)
					if (edge.derived == current)
					{
						if (edge.base == base)
							return true;
						work.push_back(edge.base);
					}
			}
			return false;
		}

		[[nodiscard]] evaluation unresolved_evaluation(const node_ptr& node)
		{
			return {predicate_outcome::unresolved, node->reason_code};
		}

		[[nodiscard]] evaluation boolean_evaluation(const node_ptr& node, const bool value)
		{
			return {value ? predicate_outcome::matched : predicate_outcome::rejected,
					node->reason_code};
		}

		[[nodiscard]] evaluation evaluate_type(const node_ptr& node,
											   const std::optional<type_ref>& type,
											   const evaluation_context& context)
		{
			if (node->operation == node_operation::constant)
				return boolean_evaluation(node, node->constant);
			if (node->operation == node_operation::negate)
			{
				auto result = evaluate_type(node->operands.front(), type, context);
				if (result.outcome == predicate_outcome::matched)
					result.outcome = predicate_outcome::rejected;
				else if (result.outcome == predicate_outcome::rejected)
					result.outcome = predicate_outcome::matched;
				return result;
			}
			if (node->operation == node_operation::all || node->operation == node_operation::any)
			{
				const bool conjunction = node->operation == node_operation::all;
				std::optional<evaluation> unresolved_result;
				for (const auto& operand : node->operands)
				{
					auto result = evaluate_type(operand, type, context);
					if (conjunction && result.outcome == predicate_outcome::rejected)
						return result;
					if (!conjunction && result.outcome == predicate_outcome::matched)
						return result;
					if (result.outcome == predicate_outcome::unresolved && !unresolved_result)
						unresolved_result = std::move(result);
				}
				if (unresolved_result)
					return *unresolved_result;
				return {conjunction ? predicate_outcome::matched : predicate_outcome::rejected,
						conjunction ? "select.all" : "select.any"};
			}
			if (node->predicate == "type.any")
				return boolean_evaluation(node, type.has_value());
			if (!type)
				return unresolved_evaluation(node);
			if (node->predicate == "type.canonical")
			{
				const auto expected = argument(node, "value");
				if (type->canonical_spelling() == expected)
					return boolean_evaluation(node, true);
				if (!context.include_derived || !type->declaration())
					return boolean_evaluation(node, false);
				std::vector<symbol_id> requested;
				for (const auto& [id, symbol] : context.symbols)
					if ((symbol.qualified_name() == expected || symbol.name() == expected) &&
						(symbol.kind() == symbol_kind::record ||
						 symbol.kind() == symbol_kind::class_ ||
						 symbol.kind() == symbol_kind::struct_ ||
						 symbol.kind() == symbol_kind::union_))
						requested.emplace_back(id);
				if (requested.size() != 1U)
					return unresolved_evaluation(node);
				return boolean_evaluation(
					node,
					is_descendant(*type->declaration(), requested.front(), context.inheritance));
			}
			if (node->predicate == "type.spelling")
				return boolean_evaluation(node, type->spelling() == argument(node, "value"));
			if (node->predicate == "type.const_qualified")
				return boolean_evaluation(node,
										  type->is_const() == (argument(node, "value") == "true"));
			if (node->predicate == "type.pointer_to")
				return boolean_evaluation(node, type->is_pointer());
			if (node->predicate == "type.reference_to")
				return boolean_evaluation(node, type->is_reference());
			if (node->predicate == "type.any_cvref" || node->predicate == "type.including_derived")
				return boolean_evaluation(node, true);
			if (node->predicate == "type.declared_as" && !node->operands.empty())
				return evaluate_symbol_predicate(
					node->operands.front(), type->declaration(), context.symbols);
			return unresolved_evaluation(node);
		}

		[[nodiscard]] evaluation evaluate_predicate(const node_ptr& node,
													const evaluation_context& context)
		{
			const auto direct = context.call.direct_callee();
			if (node->predicate == "call.any" || node->predicate == "call.dispatch" ||
				node->predicate == "call.include_derived_types" ||
				node->predicate == "call.include_virtual_overrides" ||
				node->predicate == "call.precision" || node->predicate == "call.variant_policy")
				return boolean_evaluation(node, true);
			if (node->predicate == "call.kinds")
			{
				const auto requested = argument(node, "values");
				const bool exact = contains_csv(requested, call_kind_name(context.call.kind()));
				const bool virtual_is_member = context.call.kind() == call_kind::virtual_member &&
					contains_csv(requested, "member");
				return boolean_evaluation(node, exact || virtual_is_member);
			}
			if (node->predicate == "call.callee" && !node->operands.empty())
				return evaluate_symbol_predicate(node->operands.front(), direct, context.symbols);
			if (node->predicate == "call.method_name" || node->predicate == "call.function_name")
			{
				if (!direct)
					return unresolved_evaluation(node);
				const auto found = context.symbols.find(std::string{direct->value()});
				return found == context.symbols.end()
					? unresolved_evaluation(node)
					: boolean_evaluation(node, found->second.name() == argument(node, "value"));
			}
			if (node->predicate == "call.callee_name")
			{
				if (!direct)
					return unresolved_evaluation(node);
				const auto found = context.symbols.find(std::string{direct->value()});
				return found == context.symbols.end()
					? unresolved_evaluation(node)
					: boolean_evaluation(node,
										 found->second.qualified_name() == argument(node, "value"));
			}
			if (node->predicate == "call.receiver_type" && !node->operands.empty())
				return evaluate_type(
					node->operands.front(), context.call.receiver_static_type(), context);
			if (node->predicate == "call.inside" && !node->operands.empty())
				return evaluate_symbol_predicate(
					node->operands.front(), context.call.caller(), context.symbols);
			if (node->predicate == "call.in_file")
				return node->operands.size() == 1U &&
						node->operands.front()->operation == node_operation::predicate &&
						node->operands.front()->predicate == "file.any"
					? boolean_evaluation(node, true)
					: unresolved_evaluation(node);
			if (node->predicate == "call.implicit_policy")
			{
				const bool implicit =
					context.call.location().origin == source_origin::implicit_compiler_node;
				const auto policy = argument(node, "value");
				if (policy == "include_language_implicit")
					return boolean_evaluation(node, true);
				return boolean_evaluation(node, policy == "implicit_only" ? implicit : !implicit);
			}
			if (node->predicate == "call.macro_policy")
			{
				const auto origin = context.call.location().origin;
				const bool macro = origin == source_origin::macro_argument ||
					origin == source_origin::macro_body || origin == source_origin::macro_expansion;
				const auto policy = argument(node, "value");
				if (policy == "exclude")
					return boolean_evaluation(node, !macro);
				if (policy == "include_with_origin")
					return boolean_evaluation(node, true);
				if (policy == "only_macro_arguments")
					return boolean_evaluation(node, origin == source_origin::macro_argument);
				if (policy == "only_macro_bodies")
					return boolean_evaluation(node, origin == source_origin::macro_body);
				return boolean_evaluation(node, origin == source_origin::macro_expansion);
			}
			if (node->predicate == "call.template_policy" && argument(node, "value") == "patterns")
				return boolean_evaluation(node, true);
			return unresolved_evaluation(node);
		}

		[[nodiscard]] evaluation evaluate_node(const node_ptr& node,
											   const evaluation_context& context)
		{
			if (node->operation == node_operation::constant)
				return {node->constant ? predicate_outcome::matched : predicate_outcome::rejected,
						"select.constant"};
			if (node->operation == node_operation::predicate)
				return evaluate_predicate(node, context);
			if (node->operation == node_operation::negate)
			{
				auto result = evaluate_node(node->operands.front(), context);
				if (result.outcome == predicate_outcome::matched)
					result.outcome = predicate_outcome::rejected;
				else if (result.outcome == predicate_outcome::rejected)
					result.outcome = predicate_outcome::matched;
				return result;
			}
			const bool conjunction = node->operation == node_operation::all;
			std::optional<evaluation> unresolved_result;
			for (const auto& operand : node->operands)
			{
				auto result = evaluate_node(operand, context);
				if (conjunction && result.outcome == predicate_outcome::rejected)
					return result;
				if (!conjunction && result.outcome == predicate_outcome::matched)
					return result;
				if (result.outcome == predicate_outcome::unresolved && !unresolved_result)
					unresolved_result = std::move(result);
			}
			if (unresolved_result)
				return *unresolved_result;
			return {conjunction ? predicate_outcome::matched : predicate_outcome::rejected,
					conjunction ? "select.all" : "select.any"};
		}

		[[nodiscard]] graph::dispatch_form initial_form(const call_site& call)
		{
			if (call.kind() == call_kind::virtual_member)
				return graph::dispatch_form::virtual_open;
			if (call.kind() == call_kind::member || call.kind() == call_kind::constructor ||
				call.kind() == call_kind::destructor)
				return graph::dispatch_form::nonvirtual_member;
			if (call.dispatch() == dispatch_kind::direct_exact)
				return graph::dispatch_form::direct;
			if (call.kind() == call_kind::function_pointer ||
				call.dispatch() == dispatch_kind::indirect_candidate_set)
				return graph::dispatch_form::indirect;
			return graph::dispatch_form::dependent;
		}

		[[nodiscard]] select::variant_match_policy variant_policy(const node_ptr& root)
		{
			const auto value = find_argument(root, "call.variant_policy", "value");
			if (value == "any_variant")
				return select::variant_match_policy::any_variant;
			if (value == "all_variants")
				return select::variant_match_policy::all_variants;
			if (value == "report_per_variant")
				return select::variant_match_policy::report_per_variant;
			return select::variant_match_policy::reject_disagreement;
		}

		void add_reason(std::map<std::pair<std::string, predicate_outcome>, std::uint64_t>& reasons,
						const evaluation& value)
		{
			++reasons[{value.reason_code, value.outcome}];
		}

		[[nodiscard]] std::string_view outcome_name(const predicate_outcome value)
		{
			constexpr std::array names{"matched", "rejected", "unresolved"};
			return names.at(static_cast<std::size_t>(value));
		}

		[[nodiscard]] std::string_view guarantee_name(const result_guarantee value)
		{
			constexpr std::array names{"exact_within_coverage",
									   "sound_over_approximation",
									   "sound_under_approximation",
									   "best_effort",
									   "heuristic"};
			return names.at(static_cast<std::size_t>(value));
		}

		[[nodiscard]] std::string_view confidence_name(const confidence value)
		{
			constexpr std::array names{"speculative", "possible", "probable", "high", "certain"};
			return names.at(static_cast<std::size_t>(value));
		}

		[[nodiscard]] json_value symbol_array(const std::vector<symbol_id>& values)
		{
			json_value::array output;
			for (const auto& value : values)
				output.emplace_back(std::string{value.value()});
			return output;
		}

		[[nodiscard]] json_value unresolved_array(const std::vector<unresolved>& values)
		{
			json_value::array output;
			for (const auto& value : values)
				output.emplace_back(cxxlens::detail::json::unresolved_value(value));
			return output;
		}

		[[nodiscard]] json_value trace_value(const query_trace& trace)
		{
			json_value::array reason_values;
			for (const auto& reason : trace.reasons)
				reason_values.emplace_back(
					json_value::object{{"count", reason.count},
									   {"outcome", std::string{outcome_name(reason.outcome)}},
									   {"reason_code", reason.reason_code}});
			return json_value{cxxlens::detail::json::envelope(
				{trace.schema},
				{{"accounting",
				  json_value::object{{"considered", trace.accounting.considered},
									 {"matched", trace.accounting.matched},
									 {"rejected", trace.accounting.rejected},
									 {"unresolved", trace.accounting.unresolved}}},
				 {"budget_exhausted", trace.budget_exhausted},
				 {"cancelled", trace.cancelled},
				 {"output_limited", trace.output_limited},
				 {"reasons", std::move(reason_values)},
				 {"refinements_failed", trace.refinements_failed},
				 {"refinements_requested", trace.refinements_requested},
				 {"refinements_succeeded", trace.refinements_succeeded},
				 {"snapshot_key", trace.snapshot_key}})};
		}

		void canonicalize_unresolved(std::vector<unresolved>& values)
		{
			std::ranges::sort(values, {}, &unresolved::stable_code);
			values.erase(std::unique(values.begin(),
									 values.end(),
									 [](const unresolved& left, const unresolved& right)
									 {
										 return left.stable_code == right.stable_code;
									 }),
						 values.end());
		}
	} // namespace

	result<void> query_trace::validate() const
	{
		if (schema != "cxxlens.query-trace.v1" || snapshot_key.empty() || !accounting.balanced())
			return query_error("core.internal-invariant-violation", "trace-accounting-invalid");
		if (!std::ranges::is_sorted(reasons))
			return query_error("core.internal-invariant-violation", "reason-order-invalid");
		if (std::ranges::any_of(reasons,
								[](const reason_count& value)
								{
									return value.count == 0U;
								}) ||
			std::ranges::adjacent_find(reasons,
									   [](const reason_count& left, const reason_count& right)
									   {
										   return left.reason_code == right.reason_code &&
											   left.outcome == right.outcome;
									   }) != reasons.end())
			return query_error("core.internal-invariant-violation", "reason-count-invalid");
		if (refinements_succeeded + refinements_failed != refinements_requested)
			return query_error("core.internal-invariant-violation",
							   "refinement-accounting-invalid");
		return {};
	}

	std::string query_trace::to_json() const
	{
		return cxxlens::detail::json::write(trace_value(*this)).value();
	}

	result<void> execution_result::validate() const
	{
		if (auto status = trace.validate(); !status)
			return status;
		if (auto status = coverage.validate(); !status)
			return status;
		if (guarantee == result_guarantee::exact_within_coverage &&
			(!coverage.complete() || !unresolved_items.empty()))
			return query_error("core.internal-invariant-violation", "exact-result-is-partial");
		if (!std::ranges::is_sorted(matches,
									[](const raw_call_match& left, const raw_call_match& right)
									{
										return left.call.value() < right.call.value();
									}) ||
			!std::ranges::is_sorted(rejected,
									[](const fact_id& left, const fact_id& right)
									{
										return left.value() < right.value();
									}))
			return query_error("core.internal-invariant-violation", "result-order-invalid");
		if (trace.accounting.rejected != rejected.size() ||
			(!trace.output_limited && trace.accounting.matched != matches.size()) ||
			(trace.output_limited && trace.accounting.matched < matches.size()))
			return query_error("core.internal-invariant-violation", "result-accounting-invalid");
		std::set<std::string> call_ids;
		for (const auto& match : matches)
		{
			if (!match.call.valid() || !call_ids.insert(std::string{match.call.value()}).second ||
				match.why.items().empty() ||
				!std::ranges::is_sorted(match.possible_targets,
										{},
										[](const symbol_id& value)
										{
											return value.value();
										}) ||
				std::ranges::adjacent_find(match.possible_targets) != match.possible_targets.end())
				return query_error("core.internal-invariant-violation", "target-order-invalid");
		}
		for (const auto& id : rejected)
			if (!id.valid() || !call_ids.insert(std::string{id.value()}).second)
				return query_error("core.internal-invariant-violation", "call-partition-invalid");
		if (!std::ranges::is_sorted(unresolved_items, {}, &unresolved::stable_code) ||
			std::ranges::adjacent_find(unresolved_items,
									   [](const unresolved& left, const unresolved& right)
									   {
										   return left.stable_code == right.stable_code;
									   }) != unresolved_items.end())
			return query_error("core.internal-invariant-violation", "unresolved-order-invalid");
		return {};
	}

	std::string execution_result::to_json() const
	{
		json_value::array match_values;
		for (const auto& match : matches)
		{
			json_value::array variants;
			for (const auto& variant : match.per_variant)
				variants.emplace_back(
					json_value::object{{"candidates", symbol_array(variant.candidates)},
									   {"variant", std::string{variant.variant.value()}}});
			match_values.emplace_back(json_value::object{
				{"call", std::string{match.call.value()}},
				{"certainty", std::string{confidence_name(match.certainty)}},
				{"evidence", cxxlens::detail::json::evidence_value(match.why)},
				{"guarantee", std::string{guarantee_name(match.guarantee)}},
				{"per_variant", std::move(variants)},
				{"possible_targets", symbol_array(match.possible_targets)},
				{"static_target",
				 match.static_target ? json_value{std::string{match.static_target->value()}}
									 : json_value{}},
				{"unresolved", unresolved_array(match.unresolved_items)}});
		}
		json_value::array rejected_values;
		for (const auto& value : rejected)
			rejected_values.emplace_back(std::string{value.value()});
		json_value document{cxxlens::detail::json::envelope(
			{"cxxlens.query-execution.v1"},
			{{"coverage", cxxlens::detail::json::coverage_value(coverage)},
			 {"guarantee", std::string{guarantee_name(guarantee)}},
			 {"matches", std::move(match_values)},
			 {"rejected", std::move(rejected_values)},
			 {"trace", trace_value(trace)},
			 {"unresolved", unresolved_array(unresolved_items)}})};
		return cxxlens::detail::json::write(document).value();
	}

	result<execution_result> execute(const query_plan& plan,
									 const fact_store& store,
									 execution_options options,
									 targeted_refinement_port* refinement)
	{
		if (auto status = plan.validate(); !status)
			return std::move(status.error());
		if (options.candidate_budget != plan.options.candidate_budget ||
			options.refinement_budget != plan.options.refinement_budget ||
			options.result_limit != plan.options.result_limit ||
			options.scope.to_json() != plan.options.scope.to_json())
			return query_error("search.plan-compilation-failed", "execution-options-plan-mismatch");
		auto parsed_selector = select::semantic_selector::from_json(plan.selector_json);
		if (!parsed_selector)
			return std::move(parsed_selector.error());
		const auto root = select::detail::selector_access::get(parsed_selector.value());
		if (!root)
			return query_error("search.plan-compilation-failed", "selector-missing");
		auto calls = store.calls();
		if (!calls)
			return std::move(calls.error());
		if (const auto requested = find_conjunctive_argument(root, "call.kinds", "values"))
			std::erase_if(calls.value(),
						  [&](const call_site& call)
						  {
							  const bool exact =
								  contains_csv(*requested, call_kind_name(call.kind()));
							  const bool virtual_is_member =
								  call.kind() == call_kind::virtual_member &&
								  contains_csv(*requested, "member");
							  return !exact && !virtual_is_member;
						  });
		auto symbols = store.symbols();
		if (!symbols)
			return std::move(symbols.error());
		auto inheritance = store.inheritance();
		if (!inheritance)
			return std::move(inheritance.error());
		auto overrides = store.overrides();
		if (!overrides)
			return std::move(overrides.error());
		auto call_facts = store.find(fact_query::all().kind(fact_kind::call));
		if (!call_facts)
			return std::move(call_facts.error());
		std::map<std::string, std::vector<build_variant_id>> call_variants;
		for (const auto& value : call_facts.value())
			call_variants.emplace(std::string{value.id().value()},
								  std::vector<build_variant_id>{value.origin().variants.begin(),
																value.origin().variants.end()});
		std::map<std::string, symbol> symbol_index;
		for (auto& value : symbols.value())
			symbol_index.emplace(std::string{value.id().value()}, std::move(value));
		graph::virtual_candidate_resolver resolver{std::move(overrides.value())};

		execution_result output;
		output.coverage = store.coverage(plan.requirements.facts, options.scope);
		output.trace.snapshot_key = plan.snapshot_key;
		output.trace.accounting.considered = calls.value().size();
		std::map<std::pair<std::string, predicate_outcome>, std::uint64_t> reason_map;
		const bool include_derived =
			find_argument(root, "call.include_derived_types", "value") == "true";
		const bool include_overrides =
			find_argument(root, "call.include_virtual_overrides", "value") == "true";
		const auto dispatch = find_argument(root, "call.dispatch", "value").value_or("direct_only");
		const auto variants = variant_policy(root);
		std::size_t processed{};
		for (const auto& call : calls.value())
		{
			if (processed >= options.candidate_budget)
			{
				output.trace.budget_exhausted = true;
				const auto remaining = calls.value().size() - processed;
				output.trace.accounting.unresolved += remaining;
				reason_map[{"search.candidate-budget-exhausted", predicate_outcome::unresolved}] +=
					remaining;
				output.unresolved_items.push_back(
					partial("search.candidate-budget-exhausted",
							unresolved_kind::dataflow_budget_exceeded));
				break;
			}
			if (options.execution.cancellation.stop_requested())
			{
				output.trace.cancelled = true;
				const auto remaining = calls.value().size() - processed;
				output.trace.accounting.unresolved += remaining;
				reason_map[{"core.cancelled", predicate_outcome::unresolved}] += remaining;
				output.unresolved_items.push_back(partial("core.cancelled"));
				break;
			}
			++processed;
			const evaluation_context evaluation_input{
				call, symbol_index, inheritance.value(), include_derived};
			const auto evaluated = evaluate_node(root, evaluation_input);
			add_reason(reason_map, evaluated);
			if (evaluated.outcome == predicate_outcome::rejected)
			{
				++output.trace.accounting.rejected;
				output.rejected.push_back(call.id());
				continue;
			}
			if (evaluated.outcome == predicate_outcome::unresolved)
			{
				++output.trace.accounting.unresolved;
				output.unresolved_items.push_back(partial(evaluated.reason_code));
				continue;
			}

			auto form = initial_form(call);
			if (form == graph::dispatch_form::virtual_open &&
				(!include_overrides || dispatch != "static_and_virtual_candidates"))
				form = graph::dispatch_form::qualified_virtual;
			auto observed = std::vector<symbol_id>{call.possible_callees().begin(),
												   call.possible_callees().end()};
			bool final_receiver{};
			std::optional<unresolved> refinement_failure;
			const bool needs_refinement = form == graph::dispatch_form::virtual_open ||
				form == graph::dispatch_form::dependent || form == graph::dispatch_form::indirect;
			if (needs_refinement && refinement != nullptr &&
				output.trace.refinements_requested < options.refinement_budget)
			{
				++output.trace.refinements_requested;
				auto refined = refinement->refine(call, options.execution);
				if (refined)
				{
					++output.trace.refinements_succeeded;
					form = refined.value().form;
					observed.insert(observed.end(),
									refined.value().possible_targets.begin(),
									refined.value().possible_targets.end());
					final_receiver = refined.value().receiver_is_final;
					if (!refined.value().complete)
						refinement_failure = partial("search.refinement-incomplete");
				}
				else
				{
					++output.trace.refinements_failed;
					refinement_failure = partial("search.refinement-failed");
				}
			}
			else if (needs_refinement && refinement != nullptr)
			{
				output.trace.budget_exhausted = true;
				refinement_failure = partial("search.refinement-budget-exhausted",
											 unresolved_kind::dataflow_budget_exceeded);
			}
			graph::candidate_request request;
			request.call_key = std::string{call.id().value()};
			request.static_target = call.direct_callee();
			request.observed_candidates = std::move(observed);
			request.form = form;
			request.inheritance_coverage_complete = output.coverage.complete();
			request.override_coverage_complete = output.coverage.complete();
			request.closed_world = options.closed_world;
			request.receiver_is_final = final_receiver;
			if (const auto found = call_variants.find(request.call_key);
				found != call_variants.end())
				for (const auto& variant : found->second)
					request.variants.push_back({variant,
												request.static_target,
												request.observed_candidates,
												request.form});
			request.variant_policy = variants;
			request.candidate_budget = options.candidate_budget;
			request.cancellation = options.execution.cancellation;
			auto resolved = resolver.resolve(std::move(request));
			if (refinement_failure)
			{
				resolved.unresolved_items.push_back(*refinement_failure);
				resolved.status = graph::resolution_status::partial;
				resolved.guarantee = result_guarantee::best_effort;
			}
			canonicalize_unresolved(resolved.unresolved_items);
			raw_call_match match;
			match.call = call.id();
			match.site = call;
			match.static_target = resolved.static_target;
			match.possible_targets = std::move(resolved.possible_targets);
			match.per_variant = std::move(resolved.per_variant);
			match.certainty = resolved.certainty;
			match.guarantee = resolved.guarantee;
			match.why = call.why();
			match.why.merge(resolved.why);
			match.unresolved_items = std::move(resolved.unresolved_items);
			output.unresolved_items.insert(output.unresolved_items.end(),
										   match.unresolved_items.begin(),
										   match.unresolved_items.end());
			output.matches.push_back(std::move(match));
			++output.trace.accounting.matched;
		}

		for (const auto& [key, count] : reason_map)
			output.trace.reasons.push_back({key.first, key.second, count});
		std::ranges::sort(output.matches,
						  [](const raw_call_match& left, const raw_call_match& right)
						  {
							  return left.call.value() < right.call.value();
						  });
		std::ranges::sort(output.rejected,
						  [](const fact_id& left, const fact_id& right)
						  {
							  return left.value() < right.value();
						  });
		if (options.result_limit && output.matches.size() > *options.result_limit)
		{
			output.trace.output_limited = true;
			output.unresolved_items.push_back(partial("search.result-limit-exhausted",
													  unresolved_kind::dataflow_budget_exceeded));
			output.matches.resize(*options.result_limit);
		}
		canonicalize_unresolved(output.unresolved_items);
		output.guarantee = output.unresolved_items.empty() && output.coverage.complete()
			? result_guarantee::exact_within_coverage
			: result_guarantee::best_effort;
		if (auto status = output.validate(); !status)
			return std::move(status.error());
		return output;
	}

	result<execution_result> provision_compile_execute(const workspace& workspace,
													   const select::semantic_selector& selector,
													   execution_options options,
													   targeted_refinement_port* refinement)
	{
		if (options.execution.cancellation.stop_requested())
			return query_error("core.cancelled", "cancelled-before-provisioning");
		compile_options compiler_options;
		compiler_options.scope = options.scope;
		compiler_options.candidate_budget = options.candidate_budget;
		compiler_options.refinement_budget = options.refinement_budget;
		compiler_options.result_limit = options.result_limit;
		compiler_options.allow_targeted_refinement = refinement != nullptr;
		auto provisional_plan = compile(selector, compiler_options, "snapshot:provisioning");
		if (!provisional_plan)
			return std::move(provisional_plan.error());
		auto execution_profile = provisional_plan.value().requirements.facts;
		for (const auto& stage : provisional_plan.value().stages)
			for (const auto kind : stage.required_facts)
				execution_profile = execution_profile.include(kind);
		if (auto provisioned =
				workspace.ensure(execution_profile, options.scope, options.execution);
			!provisioned)
		{
			auto failure = query_error("search.required-facts-unavailable", "provisioning-failed");
			failure.causes.push_back(std::move(provisioned.error()));
			return failure;
		}
		const auto store = workspace.facts();
		auto snapshot_facts = store.find(fact_query::all());
		if (!snapshot_facts)
			return query_error("search.required-facts-unavailable", "snapshot-unavailable");
		std::string snapshot_representation{"cxxlens.query-snapshot.v1"};
		const auto append_framed = [](std::string& output, const std::string_view value)
		{
			output += std::to_string(value.size()) + ":";
			output.append(value);
		};
		for (const auto& fact : snapshot_facts.value())
			append_framed(snapshot_representation, fact.to_json());
		append_framed(snapshot_representation,
					  store.coverage(execution_profile, options.scope).to_json());
		runtime::fnv1a_hash_adapter hashes;
		runtime::request_context hash_context;
		hash_context.operation = "query.snapshot-key";
		hash_context.cancellation = options.execution.cancellation;
		auto digest = hashes.calculate(
			runtime::make_hash_request("cxxlens.query-snapshot.v1", snapshot_representation),
			hash_context);
		if (!digest)
			return query_error("search.plan-compilation-failed", "snapshot-key-failed");
		auto plan = compile(selector, compiler_options, "fnv1a-v1:" + digest.value().hexadecimal);
		if (!plan)
			return std::move(plan.error());
		return execute(plan.value(), store, std::move(options), refinement);
	}
} // namespace cxxlens::detail::query
