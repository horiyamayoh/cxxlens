#include <algorithm>
#include <array>
#include <cstddef>
#include <sstream>
#include <string_view>
#include <type_traits>

#include "../core/canonical_json.hpp"
#include "../core/json_projections.hpp"
#include "search_report_access.hpp"

namespace cxxlens
{
	namespace
	{
		using json_value = detail::json::json_value;

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

		[[nodiscard]] std::string_view precision_name(const precision_level value)
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

		[[nodiscard]] std::string_view outcome_name(const detail::query::predicate_outcome value)
		{
			constexpr std::array names{"matched", "rejected", "unresolved"};
			return names.at(static_cast<std::size_t>(value));
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

		[[nodiscard]] std::string_view dispatch_name(const dispatch_kind value)
		{
			constexpr std::array names{"direct_exact",
									   "static_member_target",
									   "virtual_candidate_set",
									   "indirect_candidate_set",
									   "modeled_candidate_set",
									   "unresolved"};
			return names.at(static_cast<std::size_t>(value));
		}

		[[nodiscard]] std::string_view access_name(const access_kind value)
		{
			constexpr std::array names{"none", "public", "protected", "private"};
			return names.at(static_cast<std::size_t>(value));
		}

		[[nodiscard]] json_value symbol_array(const std::span<const symbol_id> values)
		{
			json_value::array output;
			for (const auto& value : values)
				output.emplace_back(std::string{value.value()});
			return output;
		}

		[[nodiscard]] json_value unresolved_array(const std::span<const unresolved> values)
		{
			json_value::array output;
			for (const auto& value : values)
				output.emplace_back(detail::json::unresolved_value(value));
			return output;
		}

		[[nodiscard]] json_value
		reason_array(const std::vector<detail::query::reason_count>& values)
		{
			json_value::array output;
			for (const auto& value : values)
				output.emplace_back(
					json_value::object{{"count", value.count},
									   {"outcome", std::string{outcome_name(value.outcome)}},
									   {"reason_code", value.reason_code}});
			return output;
		}

		[[nodiscard]] json_value accounting_value(const detail::query::candidate_accounting& value)
		{
			return json_value::object{{"considered", value.considered},
									  {"matched", value.matched},
									  {"rejected", value.rejected},
									  {"unresolved", value.unresolved}};
		}

		[[nodiscard]] json_value call_value(const call_site& call,
											const detail::query::raw_call_match* context)
		{
			json_value::array variants;
			if (context != nullptr)
				for (const auto& variant : context->per_variant)
					variants.emplace_back(
						json_value::object{{"candidates", symbol_array(variant.candidates)},
										   {"variant", std::string{variant.variant.value()}}});
			const auto receiver = call.receiver_static_type();
			const auto caller = call.caller();
			const auto target = call.direct_callee();
			return json_value::object{
				{"call_id", std::string{call.id().value()}},
				{"caller", caller ? json_value{std::string{caller->value()}} : json_value{}},
				{"certainty", std::string{confidence_name(call.certainty())}},
				{"dispatch", std::string{dispatch_name(call.dispatch())}},
				{"evidence", detail::json::evidence_value(call.why())},
				{"guarantee", std::string{guarantee_name(call.guarantee())}},
				{"kind", std::string{call_kind_name(call.kind())}},
				{"location", detail::json::source_span_value(call.location())},
				{"per_variant", std::move(variants)},
				{"possible_targets", symbol_array(call.possible_callees())},
				{"receiver",
				 receiver ? json_value{json_value::object{
								{"canonical", std::string{receiver->canonical_spelling()}},
								{"id", std::string{receiver->id().value()}},
								{"spelling", std::string{receiver->spelling()}}}}
						  : json_value{}},
				{"static_target", target ? json_value{std::string{target->value()}} : json_value{}},
				{"unresolved",
				 context != nullptr ? unresolved_array(context->unresolved_items)
									: json_value{json_value::array{}}}};
		}

		[[nodiscard]] json_value inheritance_value(const inheritance_edge& edge)
		{
			return json_value::object{{"access", std::string{access_name(edge.access)}},
									  {"base", std::string{edge.base.value()}},
									  {"derived", std::string{edge.derived.value()}},
									  {"evidence", detail::json::evidence_value(edge.why)},
									  {"source", detail::json::source_span_value(edge.source)},
									  {"virtual", edge.is_virtual}};
		}

		[[nodiscard]] json_value override_value(const override_edge& edge)
		{
			return json_value::object{{"evidence", detail::json::evidence_value(edge.why)},
									  {"overridden", std::string{edge.overridden_method.value()}},
									  {"overriding", std::string{edge.overriding_method.value()}},
									  {"source", detail::json::source_span_value(edge.source)}};
		}

		template <class T>
		[[nodiscard]] std::string_view result_kind()
		{
			if constexpr (std::is_same_v<T, call_site>)
				return "call";
			if constexpr (std::is_same_v<T, inheritance_edge>)
				return "inheritance";
			return "override";
		}

		template <class T>
		[[nodiscard]] const detail::search_report_data<T>&
		data_or_empty(const std::shared_ptr<const detail::search_report_data<T>>& data)
		{
			static const detail::search_report_data<T> empty;
			return data ? *data : empty;
		}
	} // namespace

	template <class T>
	search_report<T>::search_report(std::shared_ptr<const detail::search_report_data<T>> data)
		: data_{std::move(data)}
	{
	}

	template <class T>
	std::span<const T> search_report<T>::matches() const noexcept
	{
		return data_ ? std::span<const T>{data_->matches} : std::span<const T>{};
	}

	template <class T>
	const coverage_report& search_report<T>::coverage() const noexcept
	{
		return data_or_empty(data_).coverage;
	}

	template <class T>
	std::span<const unresolved> search_report<T>::unresolved_items() const noexcept
	{
		return data_ ? std::span<const unresolved>{data_->unresolved_items}
					 : std::span<const unresolved>{};
	}

	template <class T>
	result_guarantee search_report<T>::guarantee() const noexcept
	{
		return data_or_empty(data_).guarantee;
	}

	template <class T>
	precision_level search_report<T>::precision() const noexcept
	{
		return data_or_empty(data_).precision;
	}

	template <class T>
	std::string search_report<T>::query_explanation() const
	{
		return data_or_empty(data_).explanation;
	}

	template <class T>
	std::string search_report<T>::to_json() const
	{
		const auto& data = data_or_empty(data_);
		json_value::array matches;
		for (std::size_t index{}; index < data.matches.size(); ++index)
		{
			if constexpr (std::is_same_v<T, call_site>)
				matches.emplace_back(call_value(
					data.matches.at(index),
					index < data.call_rows.size() ? &data.call_rows.at(index) : nullptr));
			else if constexpr (std::is_same_v<T, inheritance_edge>)
				matches.emplace_back(inheritance_value(data.matches.at(index)));
			else
				matches.emplace_back(override_value(data.matches.at(index)));
		}
		json_value document{
			detail::json::envelope({"cxxlens.search-report.v1"},
								   {{"accounting", accounting_value(data.accounting)},
									{"coverage", detail::json::coverage_value(data.coverage)},
									{"guarantee", std::string{guarantee_name(data.guarantee)}},
									{"matches", std::move(matches)},
									{"precision", std::string{precision_name(data.precision)}},
									{"query_explanation", data.explanation},
									{"reasons", reason_array(data.reasons)},
									{"result_kind", std::string{result_kind<T>()}},
									{"unresolved", unresolved_array(data.unresolved_items)}})};
		return detail::json::write(document).value();
	}

	template <class T>
	std::string search_report<T>::to_markdown() const
	{
		const auto& data = data_or_empty(data_);
		std::ostringstream output;
		output << "# cxxlens search report\n\n"
			   << "- Result kind: `" << result_kind<T>() << "`\n"
			   << "- Matches: " << data.matches.size() << "\n"
			   << "- Guarantee: `" << guarantee_name(data.guarantee) << "`\n"
			   << "- Precision: `" << precision_name(data.precision) << "`\n"
			   << "- Coverage complete: " << (data.coverage.complete() ? "yes" : "no") << "\n"
			   << "- Coverage requested/covered/failed/unresolved: "
			   << data.coverage.requested().size() << "/"
			   << data.coverage.count(coverage_state::covered) << "/"
			   << data.coverage.count(coverage_state::failed) << "/"
			   << data.coverage.count(coverage_state::unresolved) << "\n\n";
		if constexpr (std::is_same_v<T, call_site>)
		{
			output << "| Call | Static target | Possible targets | Origin | Guarantee |\n"
				   << "|---|---|---:|---|---|\n";
			for (const auto& call : data.matches)
			{
				const auto target = call.direct_callee();
				output << "| `" << call.id().value() << "` | `"
					   << (target ? target->value() : "unresolved") << "` | "
					   << call.possible_callees().size() << " | `"
					   << static_cast<std::uint16_t>(call.location().origin) << "` | `"
					   << guarantee_name(call.guarantee()) << "` |\n";
			}
		}
		else if constexpr (std::is_same_v<T, inheritance_edge>)
		{
			output << "| Derived | Base | Access | Virtual |\n|---|---|---|---|\n";
			for (const auto& edge : data.matches)
				output << "| `" << edge.derived.value() << "` | `" << edge.base.value() << "` | `"
					   << access_name(edge.access) << "` | " << (edge.is_virtual ? "yes" : "no")
					   << " |\n";
		}
		else
		{
			output << "| Overriding | Overridden |\n|---|---|\n";
			for (const auto& edge : data.matches)
				output << "| `" << edge.overriding_method.value() << "` | `"
					   << edge.overridden_method.value() << "` |\n";
		}
		output << "\n## Predicate accounting\n\n";
		for (const auto& reason : data.reasons)
			output << "- `" << reason.reason_code << "` / `" << outcome_name(reason.outcome)
				   << "`: " << reason.count << "\n";
		output << "\n## Unresolved\n\n";
		if (data.unresolved_items.empty())
			output << "None.\n";
		else if (!data.expand_unresolved)
			output << data.unresolved_items.size()
				   << " unresolved item(s); enable `include_unresolved` for details.\n";
		else
			for (const auto& value : data.unresolved_items)
				output << "- `" << value.stable_code << "`: " << value.summary << "\n";
		output << "\n## Query explanation\n\n" << data.explanation << "\n";
		return output.str();
	}

	template class search_report<call_site>;
	template class search_report<inheritance_edge>;
	template class search_report<override_edge>;
} // namespace cxxlens
