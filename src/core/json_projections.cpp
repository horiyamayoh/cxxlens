#include "json_projections.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <cxxlens/core/finding.hpp>

namespace cxxlens::detail::json
{
	namespace
	{
		[[nodiscard]] std::string failure_scope_name(const failure_scope scope)
		{
			constexpr std::array<std::string_view, 6U> names{
				"operation", "file", "compile_unit", "build_variant", "symbol", "workspace"};
			const auto index = static_cast<std::size_t>(scope);
			return index < names.size() ? std::string{names.at(index)} : std::string{"unknown"};
		}

		[[nodiscard]] std::string unresolved_kind_name(const unresolved_kind kind)
		{
			constexpr std::array<std::string_view, 24U> names{
				"missing_compile_command",
				"inferred_compile_command",
				"malformed_compile_command",
				"parse_failed",
				"incomplete_ast",
				"ambiguous_symbol",
				"dependent_type",
				"unresolved_overload",
				"unsupported_language_extension",
				"missing_module_bmi",
				"macro_origin_ambiguous",
				"macro_edit_unsafe",
				"generated_code_read_only",
				"function_pointer_target_unknown",
				"callback_target_unknown",
				"virtual_dynamic_type_unknown",
				"open_world_virtual_target",
				"alias_analysis_required",
				"dataflow_budget_exceeded",
				"path_sensitive_budget_exceeded",
				"capability_unavailable",
				"build_variant_disagreement",
				"stale_source",
				"custom",
			};
			const auto index = static_cast<std::size_t>(kind);
			return index < names.size() ? std::string{names.at(index)} : std::string{"unknown"};
		}

		[[nodiscard]] std::string evidence_kind_name(const evidence_kind kind)
		{
			constexpr std::array<std::string_view, 15U> names{
				"source",
				"ast_binding",
				"canonical_symbol",
				"canonical_type",
				"inheritance_relation",
				"override_relation",
				"call_resolution",
				"control_flow",
				"dataflow_path",
				"api_model",
				"dynamic_observation",
				"build_context",
				"approximation",
				"exclusion",
				"custom",
			};
			const auto index = static_cast<std::size_t>(kind);
			return index < names.size() ? std::string{names.at(index)} : std::string{"unknown"};
		}

		[[nodiscard]] std::string coverage_state_name(const coverage_state state)
		{
			constexpr std::array<std::string_view, 5U> names{
				"covered", "excluded", "failed", "unresolved", "not_applicable"};
			const auto index = static_cast<std::size_t>(state);
			return index < names.size() ? std::string{names.at(index)} : std::string{"unknown"};
		}

		[[nodiscard]] std::string severity_name(const severity level)
		{
			constexpr std::array<std::string_view, 5U> names{
				"note", "info", "warning", "error", "fatal"};
			const auto index = static_cast<std::size_t>(level);
			return index < names.size() ? std::string{names.at(index)} : std::string{"unknown"};
		}

		[[nodiscard]] json_value string_array(const std::vector<std::string>& values)
		{
			json_value::array result;
			for (const auto& value : values)
				result.emplace_back(value);
			return result;
		}

		[[nodiscard]] json_value string_map(const std::map<std::string, std::string>& values)
		{
			json_value::object result;
			for (const auto& [key, value] : values)
				result.emplace_back(key, value);
			return result;
		}
	} // namespace

	json_value error_value(const error& failure)
	{
		json_value::array locations;
		for (const auto& location : failure.locations)
			locations.emplace_back(source_span_value(location));
		json_value::array causes;
		for (const auto& cause : failure.causes)
			causes.emplace_back(error_value(cause));
		json_value::object fields{
			{"code", failure.code.value},
			{"message", failure.message},
			{"scope", failure_scope_name(failure.scope)},
			{"locations", std::move(locations)},
			{"causes", std::move(causes)},
			{"suggested_actions", string_array(failure.suggested_actions)},
			{"attributes", string_map(failure.attributes)},
			{"retryable", failure.retryable},
		};
		return envelope({"cxxlens.error.v1"}, std::move(fields));
	}

	json_value unresolved_value(const unresolved& item)
	{
		json_value::array related;
		for (const auto& span : item.related)
			related.emplace_back(source_span_value(span));
		json_value::object fields{
			{"kind", unresolved_kind_name(item.kind)},
			{"stable_code", item.stable_code},
			{"summary", item.summary},
			{"scope", failure_scope_name(item.scope)},
			{"related", std::move(related)},
			{"missing_inputs", string_array(item.missing_inputs)},
			{"suggested_actions", string_array(item.suggested_actions)},
			{"required_precision",
			 item.required_precision
				 ? json_value{static_cast<std::uint64_t>(*item.required_precision)}
				 : json_value{}},
			{"required_capability",
			 item.required_capability ? json_value{*item.required_capability} : json_value{}},
			{"attributes", string_map(item.attributes)},
		};
		return envelope({"cxxlens.unresolved.v1"}, std::move(fields));
	}

	json_value evidence_value(const evidence& why)
	{
		json_value::array rows;
		for (const auto& item : why.items())
		{
			json_value::array facts;
			for (const auto& fact : item.supporting_facts)
				facts.emplace_back(std::string{fact.value()});
			rows.emplace_back(json_value::object{
				{"kind", evidence_kind_name(item.kind)},
				{"summary", item.summary},
				{"source", item.source ? source_span_value(*item.source) : json_value{}},
				{"supporting_facts", std::move(facts)},
				{"attributes", string_map(item.attributes)},
			});
		}
		return envelope({"cxxlens.evidence.v1"}, {{"items", std::move(rows)}});
	}

	json_value coverage_value(const coverage_report& coverage)
	{
		json_value::array requests;
		for (const auto& request : coverage.requested())
			requests.emplace_back(json_value::object{{"kind", request.kind}, {"id", request.id}});
		json_value::array units;
		for (const auto& unit : coverage.units())
		{
			units.emplace_back(json_value::object{
				{"kind", unit.kind},
				{"id", unit.id},
				{"state", coverage_state_name(unit.state)},
				{"reason", unit.reason ? json_value{*unit.reason} : json_value{}},
			});
		}
		json_value::object summary;
		for (const auto state : {coverage_state::covered,
								 coverage_state::excluded,
								 coverage_state::failed,
								 coverage_state::unresolved,
								 coverage_state::not_applicable})
			summary.emplace_back(coverage_state_name(state),
								 static_cast<std::uint64_t>(coverage.count(state)));
		return envelope({"cxxlens.coverage.v1"},
						{{"requested", std::move(requests)},
						 {"units", std::move(units)},
						 {"summary", std::move(summary)},
						 {"complete", coverage.complete()}});
	}

	json_value diagnostic_value(const diagnostic& observation)
	{
		json_value::array related;
		for (const auto& span : observation.related)
			related.emplace_back(source_span_value(span));
		return envelope(
			{"cxxlens.diagnostic.v1"},
			{{"id", observation.id},
			 {"message", observation.message},
			 {"severity", severity_name(observation.level)},
			 {"primary",
			  observation.primary ? source_span_value(*observation.primary) : json_value{}},
			 {"related", std::move(related)},
			 {"compiler_option",
			  observation.compiler_option ? json_value{*observation.compiler_option}
										  : json_value{}}});
	}
} // namespace cxxlens::detail::json
