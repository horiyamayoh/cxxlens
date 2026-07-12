#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

#include <cxxlens/search.hpp>

#include "../query/predicate_evaluator.hpp"
#include "../query/query_executor.hpp"
#include "../select/selector_ast.hpp"
#include "../store/fact_store_access.hpp"
#include "search_report_access.hpp"

namespace cxxlens::search
{
	namespace
	{
		using detail::query::predicate_outcome;

		[[nodiscard]] error search_error(std::string code, std::string reason)
		{
			error failure;
			failure.code.value = std::move(code);
			failure.message = "semantic search failed";
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		[[nodiscard]] result<std::size_t> validate_options(const search_options& options)
		{
			if (static_cast<std::uint8_t>(options.precision) >
					static_cast<std::uint8_t>(precision_level::dynamic_observation) ||
				static_cast<std::uint8_t>(options.variants) >
					static_cast<std::uint8_t>(select::variant_match_policy::reject_disagreement))
				return search_error("core.invalid-argument", "search-option-enum-invalid");
			if (options.execution.cancellation.stop_requested())
				return search_error("core.cancelled", "cancelled-before-search");
			if (options.result_limit &&
				*options.result_limit >
					static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
				return search_error("core.invalid-argument", "result-limit-out-of-range");
			return options.result_limit ? static_cast<std::size_t>(*options.result_limit)
										: std::numeric_limits<std::size_t>::max();
		}

		[[nodiscard]] precision_level maximum(const precision_level left,
											  const precision_level right)
		{
			return static_cast<std::uint8_t>(left) < static_cast<std::uint8_t>(right) ? right
																					  : left;
		}

		[[nodiscard]] unresolved predicate_unresolved(std::string code)
		{
			unresolved value;
			value.kind = unresolved_kind::custom;
			value.stable_code = std::move(code);
			value.summary = "selector predicate could not be resolved";
			return value;
		}

		[[nodiscard]] unresolved result_limit_unresolved()
		{
			unresolved value;
			value.kind = unresolved_kind::dataflow_budget_exceeded;
			value.stable_code = "search.result-limit-exhausted";
			value.summary = "result limit truncated authoritative matches";
			return value;
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

		[[nodiscard]] result<void> enforce_coverage(const coverage_report& coverage,
													const search_options& options)
		{
			if (auto status = coverage.validate(); !status)
				return std::move(status.error());
			if (options.strict_coverage && !coverage.complete())
				return search_error("search.required-facts-unavailable",
									"strict-coverage-incomplete");
			return {};
		}

		struct selected_symbols
		{
			std::set<std::string> ids;
			std::vector<detail::query::reason_count> reasons;
			std::vector<unresolved> unresolved_items;
		};

		[[nodiscard]] result<selected_symbols>
		select_symbols(const select::symbol_selector& selector, const std::vector<symbol>& symbols)
		{
			const auto root = cxxlens::select::detail::selector_access::get(selector);
			if (!root || root->domain != cxxlens::select::detail::selector_domain::symbol)
				return search_error("select.type-mismatch", "symbol-selector-required");
			detail::query::symbol_index index;
			for (const auto& symbol : symbols)
				index.emplace(std::string{symbol.id().value()}, symbol);
			std::map<std::pair<std::string, predicate_outcome>, std::uint64_t> counts;
			selected_symbols output;
			for (const auto& symbol : symbols)
			{
				const auto evaluated =
					detail::query::evaluate_symbol_predicate(root, symbol.id(), index);
				++counts[{evaluated.reason_code, evaluated.outcome}];
				if (evaluated.outcome == predicate_outcome::matched)
					output.ids.insert(std::string{symbol.id().value()});
				else if (evaluated.outcome == predicate_outcome::unresolved)
					output.unresolved_items.push_back(predicate_unresolved(evaluated.reason_code));
			}
			for (const auto& [key, count] : counts)
				output.reasons.push_back({key.first, key.second, count});
			canonicalize_unresolved(output.unresolved_items);
			return output;
		}

		[[nodiscard]] detail::query::candidate_accounting
		accounting(const std::size_t considered,
				   const std::size_t matched,
				   const bool unresolved_selection)
		{
			detail::query::candidate_accounting output;
			output.considered = considered;
			output.matched = matched;
			if (unresolved_selection && matched == 0U)
				output.unresolved = considered;
			else
				output.rejected = considered - matched;
			return output;
		}

		template <class T>
		void apply_result_limit(detail::search_report_data<T>& data,
								const search_options& options,
								const std::size_t limit)
		{
			data.expand_unresolved = options.include_unresolved;
			if (options.result_limit && data.matches.size() > limit)
			{
				data.matches.resize(limit);
				data.unresolved_items.push_back(result_limit_unresolved());
				canonicalize_unresolved(data.unresolved_items);
				data.guarantee = result_guarantee::best_effort;
			}
		}

		[[nodiscard]] result<search_report<call_site>> call_report(const workspace& workspace,
																   select::call_selector selector,
																   const search_options& options)
		{
			auto limit = validate_options(options);
			if (!limit)
				return std::move(limit.error());
			selector = selector.variants(options.variants).precision(options.precision);
			const auto semantic = select::semantic(selector);
			detail::query::execution_options execution_options;
			execution_options.scope = options.scope;
			execution_options.result_limit =
				options.result_limit ? std::optional<std::size_t>{limit.value()} : std::nullopt;
			execution_options.closed_world = false;
			execution_options.execution = options.execution;
			auto raw =
				detail::query::provision_compile_execute(workspace, semantic, execution_options);
			if (!raw)
				return std::move(raw.error());
			if (auto status = enforce_coverage(raw.value().coverage, options); !status)
				return std::move(status.error());
			auto data = std::make_shared<detail::search_report_data<call_site>>();
			data->coverage = raw.value().coverage;
			data->unresolved_items = raw.value().unresolved_items;
			data->guarantee = raw.value().guarantee;
			data->precision = selector.requirements().minimum_precision;
			data->explanation = selector.explain();
			data->accounting = raw.value().trace.accounting;
			data->reasons = raw.value().trace.reasons;
			data->expand_unresolved = options.include_unresolved;
			data->matches.reserve(raw.value().matches.size());
			data->call_rows.reserve(raw.value().matches.size());
			for (auto& match : raw.value().matches)
			{
				auto enriched = detail::fact_store_access::enrich_call(match.site,
																	   match.static_target,
																	   match.possible_targets,
																	   match.certainty,
																	   match.guarantee,
																	   match.why);
				data->matches.push_back(std::move(enriched));
				data->call_rows.push_back(std::move(match));
			}
			return detail::search_report_access::make<call_site>(std::move(data));
		}
	} // namespace

	result<search_report<call_site>>
	calls(const workspace& workspace,
		  select::call_selector selector,
		  search_options options) // NOLINT(performance-unnecessary-value-param)
	{
		return call_report(workspace, std::move(selector), options);
	}

	result<search_report<call_site>>
	calls_to_function(const workspace& workspace,
					  std::string qualified_name,
					  search_options options) // NOLINT(performance-unnecessary-value-param)
	{
		if (qualified_name.empty())
			return search_error("core.invalid-argument", "qualified-name-empty");
		return call_report(
			workspace, select::calls_to_function(std::move(qualified_name)), options);
	}

	result<search_report<call_site>>
	calls_to_method(const workspace& workspace,
					std::string receiver_type,
					std::string method_name,
					search_options options) // NOLINT(performance-unnecessary-value-param)
	{
		if (receiver_type.empty() || method_name.empty())
			return search_error("core.invalid-argument", "method-input-empty");
		auto selector = select::calls_to_method(std::move(receiver_type), std::move(method_name))
							.include_derived_types()
							.include_virtual_overrides()
							.dispatch(select::dispatch_policy::static_and_virtual_candidates);
		return call_report(workspace, std::move(selector), options);
	}

	result<search_report<inheritance_edge>>
	inheritance(const workspace& workspace,
				select::symbol_selector record,
				const bool transitive,
				search_options options) // NOLINT(performance-unnecessary-value-param)
	{
		auto validated = validate_options(options);
		if (!validated)
			return std::move(validated.error());
		record = record.variants(options.variants);
		auto profile =
			record.requirements().facts.include(fact_kind::inheritance).include(fact_kind::symbol);
		profile =
			profile.precision(maximum(options.precision, precision_level::workspace_semantic));
		if (auto ensured = workspace.ensure(profile, options.scope, options.execution); !ensured)
			return std::move(ensured.error());
		const auto store = workspace.facts();
		auto symbols = store.symbols();
		if (!symbols)
			return std::move(symbols.error());
		auto selected = select_symbols(record, symbols.value());
		if (!selected)
			return std::move(selected.error());
		auto edges = store.inheritance();
		if (!edges)
			return std::move(edges.error());
		auto data = std::make_shared<detail::search_report_data<inheritance_edge>>();
		data->coverage = store.coverage(profile, options.scope);
		if (auto status = enforce_coverage(data->coverage, options); !status)
			return std::move(status.error());
		std::set<std::string> frontier = selected.value().ids;
		std::set<std::pair<std::string, std::string>> emitted;
		bool changed = true;
		while (changed)
		{
			changed = false;
			for (const auto& edge : edges.value())
				if (frontier.contains(std::string{edge.base.value()}) &&
					emitted
						.insert({std::string{edge.derived.value()}, std::string{edge.base.value()}})
						.second)
				{
					data->matches.push_back(edge);
					if (transitive)
						changed =
							frontier.insert(std::string{edge.derived.value()}).second || changed;
				}
			if (!transitive)
				break;
		}
		std::ranges::sort(data->matches,
						  [](const inheritance_edge& left, const inheritance_edge& right)
						  {
							  return std::tuple{left.derived.value(), left.base.value()} <
								  std::tuple{right.derived.value(), right.base.value()};
						  });
		data->unresolved_items = selected.value().unresolved_items;
		data->guarantee = data->coverage.complete() && data->unresolved_items.empty()
			? result_guarantee::exact_within_coverage
			: result_guarantee::best_effort;
		data->precision = maximum(options.precision, precision_level::workspace_semantic);
		data->explanation = record.explain();
		data->reasons = selected.value().reasons;
		data->accounting =
			accounting(edges.value().size(), data->matches.size(), !data->unresolved_items.empty());
		apply_result_limit(*data, options, validated.value());
		return detail::search_report_access::make<inheritance_edge>(std::move(data));
	}

	result<search_report<override_edge>>
	overrides(const workspace& workspace,
			  select::symbol_selector method,
			  const bool reverse,
			  search_options options) // NOLINT(performance-unnecessary-value-param)
	{
		auto validated = validate_options(options);
		if (!validated)
			return std::move(validated.error());
		method = method.variants(options.variants);
		auto profile = method.requirements()
						   .facts.include(fact_kind::override_relation)
						   .include(fact_kind::symbol);
		profile =
			profile.precision(maximum(options.precision, precision_level::workspace_semantic));
		if (auto ensured = workspace.ensure(profile, options.scope, options.execution); !ensured)
			return std::move(ensured.error());
		const auto store = workspace.facts();
		auto symbols = store.symbols();
		if (!symbols)
			return std::move(symbols.error());
		auto selected = select_symbols(method, symbols.value());
		if (!selected)
			return std::move(selected.error());
		auto edges = store.overrides();
		if (!edges)
			return std::move(edges.error());
		auto data = std::make_shared<detail::search_report_data<override_edge>>();
		data->coverage = store.coverage(profile, options.scope);
		if (auto status = enforce_coverage(data->coverage, options); !status)
			return std::move(status.error());
		for (const auto& edge : edges.value())
		{
			const auto& selected_id = reverse ? edge.overridden_method : edge.overriding_method;
			if (selected.value().ids.contains(std::string{selected_id.value()}))
				data->matches.push_back(edge);
		}
		std::ranges::sort(
			data->matches,
			[](const override_edge& left, const override_edge& right)
			{
				return std::tuple{left.overriding_method.value(), left.overridden_method.value()} <
					std::tuple{right.overriding_method.value(), right.overridden_method.value()};
			});
		data->unresolved_items = selected.value().unresolved_items;
		data->guarantee = data->coverage.complete() && data->unresolved_items.empty()
			? result_guarantee::exact_within_coverage
			: result_guarantee::best_effort;
		data->precision = maximum(options.precision, precision_level::workspace_semantic);
		data->explanation = method.explain();
		data->reasons = selected.value().reasons;
		data->accounting =
			accounting(edges.value().size(), data->matches.size(), !data->unresolved_items.empty());
		apply_result_limit(*data, options, validated.value());
		return detail::search_report_access::make<override_edge>(std::move(data));
	}
} // namespace cxxlens::search
