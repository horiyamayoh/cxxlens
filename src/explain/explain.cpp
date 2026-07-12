#include <algorithm>
#include <array>
#include <cstddef>
#include <sstream>
#include <string_view>
#include <utility>

#include <cxxlens/explain.hpp>

#include "../core/canonical_json.hpp"
#include "../core/json_projections.hpp"
#include "../query/query_executor.hpp"

namespace cxxlens::explain
{
	namespace
	{
		using json_value = detail::json::json_value;

		[[nodiscard]] json_value strings(const std::vector<std::string>& values)
		{
			json_value::array output;
			for (const auto& value : values)
				output.emplace_back(value);
			return output;
		}

		[[nodiscard]] json_value property_value(const std::map<std::string, std::string>& values)
		{
			json_value::object output;
			for (const auto& [key, value] : values)
				output.emplace_back(key, value);
			return output;
		}

		[[nodiscard]] json_value unresolved_rows(const std::vector<unresolved>& values)
		{
			json_value::array output;
			for (const auto& value : values)
				output.emplace_back(detail::json::unresolved_value(value));
			return output;
		}

		[[nodiscard]] evidence evidence_set(const std::vector<evidence_item>& values)
		{
			evidence output;
			for (const auto& value : values)
				output.add(value);
			return output;
		}

		[[nodiscard]] std::string_view outcome_name(const detail::query::predicate_outcome value)
		{
			constexpr std::array names{"matched", "rejected", "unresolved"};
			return names.at(static_cast<std::size_t>(value));
		}

		[[nodiscard]] std::string fact_names(const fact_profile& profile)
		{
			return profile.to_json();
		}

		void add_heading(std::ostringstream& output,
						 const std::string_view title,
						 const std::vector<std::string>& rows)
		{
			output << "\n## " << title << "\n\n";
			if (rows.empty())
				output << "None.\n";
			else
				for (const auto& row : rows)
					output << "- " << row << "\n";
		}
	} // namespace

	std::string explanation::to_markdown() const
	{
		std::ostringstream output;
		output << "# " << (title.empty() ? "cxxlens explanation" : title) << "\n\n"
			   << summary << "\n";
		add_heading(output, "Steps", steps);
		output << "\n## Evidence\n\n";
		if (evidence.empty())
			output << "None.\n";
		else
			for (const auto& item : evidence)
				output << "- `" << static_cast<std::uint16_t>(item.kind) << "`: " << item.summary
					   << "\n";
		output << "\n## Unresolved\n\n";
		if (unresolved_items.empty())
			output << "None.\n";
		else
			for (const auto& item : unresolved_items)
				output << "- `" << item.stable_code << "`: " << item.summary << "\n";
		add_heading(output, "Suggested actions", suggested_actions);
		output << "\n## Properties\n\n";
		if (properties.empty())
			output << "None.\n";
		else
			for (const auto& [key, value] : properties)
				output << "- `" << key << "`: `" << value << "`\n";
		return output.str();
	}

	std::string explanation::to_json() const
	{
		const auto why = evidence_set(evidence);
		json_value document{
			detail::json::envelope({"cxxlens.explanation.v1"},
								   {{"evidence", detail::json::evidence_value(why)},
									{"properties", property_value(this->properties)},
									{"steps", strings(steps)},
									{"suggested_actions", strings(suggested_actions)},
									{"summary", summary},
									{"title", title},
									{"unresolved", unresolved_rows(unresolved_items)}})};
		return detail::json::write(document).value();
	}

	explanation selector(const select::semantic_selector& value, const detail_level level)
	{
		explanation output;
		output.title = "Selector explanation";
		output.summary = value.explain();
		const auto requirements = value.requirements();
		output.steps = {"Normalize and type-check the selector.",
						"Provision the exact required fact profile.",
						"Compile and execute the deterministic query plan."};
		output.properties.emplace("selector.json", value.to_json());
		output.properties.emplace("requirements.facts", fact_names(requirements.facts));
		output.properties.emplace(
			"requirements.precision",
			std::to_string(static_cast<std::uint8_t>(requirements.minimum_precision)));
		output.properties.emplace("detail", std::to_string(static_cast<std::uint8_t>(level)));
		if (level == detail_level::agent)
			output.suggested_actions = {
				"Use <cxxlens/search.hpp> for execution.",
				"Treat names as resolver inputs, never semantic identity.",
				"Check coverage, guarantee, and unresolved rows before consuming matches."};
		return output;
	}

	explanation finding(const cxxlens::finding& value, const detail_level level)
	{
		explanation output;
		output.title = "Finding explanation";
		output.summary = std::string{value.message()};
		output.evidence.assign(value.why().items().begin(), value.why().items().end());
		output.unresolved_items.assign(value.unresolved_items().begin(),
									   value.unresolved_items().end());
		output.properties.emplace("finding.id", std::string{value.id().value()});
		output.properties.emplace("detail", std::to_string(static_cast<std::uint8_t>(level)));
		return output;
	}

	explanation coverage(const coverage_report& value, const detail_level level)
	{
		explanation output;
		output.title = "Coverage explanation";
		output.summary =
			value.complete() ? "Requested coverage is complete." : "Requested coverage is partial.";
		output.properties.emplace("requested", std::to_string(value.requested().size()));
		output.properties.emplace("covered", std::to_string(value.count(coverage_state::covered)));
		output.properties.emplace("failed", std::to_string(value.count(coverage_state::failed)));
		output.properties.emplace("unresolved",
								  std::to_string(value.count(coverage_state::unresolved)));
		output.properties.emplace("detail", std::to_string(static_cast<std::uint8_t>(level)));
		for (const auto& unit : value.units())
			if (unit.state == coverage_state::failed || unit.state == coverage_state::unresolved)
			{
				unresolved item;
				item.kind = unresolved_kind::capability_unavailable;
				item.stable_code = unit.reason.value_or("facts.coverage-incomplete");
				item.summary = "coverage unit is not covered";
				item.missing_inputs.push_back(unit.kind + ":" + unit.id);
				output.unresolved_items.push_back(std::move(item));
			}
		return output;
	}

	result<explanation>
	why_not_matched(const workspace& workspace,
					// NOLINTNEXTLINE(performance-unnecessary-value-param)
					select::semantic_selector selector_value,
					analysis_scope scope,
					const detail_level level,
					execution_context execution) // NOLINT(performance-unnecessary-value-param)
	{
		detail::query::execution_options options;
		options.scope = std::move(scope);
		options.execution = std::move(execution);
		auto result = detail::query::provision_compile_execute(workspace, selector_value, options);
		if (!result)
			return std::move(result.error());
		explanation output;
		output.title = "Why candidates did not match";
		output.summary = "Predicate rejection, unresolved prerequisites, and returned matches are "
						 "accounted independently.";
		output.properties.emplace("considered",
								  std::to_string(result.value().trace.accounting.considered));
		output.properties.emplace("matched",
								  std::to_string(result.value().trace.accounting.matched));
		output.properties.emplace("rejected",
								  std::to_string(result.value().trace.accounting.rejected));
		output.properties.emplace("unresolved",
								  std::to_string(result.value().trace.accounting.unresolved));
		for (const auto& reason : result.value().trace.reasons)
			output.properties.emplace("reason." + reason.reason_code + "." +
										  std::string{outcome_name(reason.outcome)},
									  std::to_string(reason.count));
		const std::size_t example_limit = level == detail_level::summary ? 2U : 8U;
		for (std::size_t index{}; index < std::min(example_limit, result.value().rejected.size());
			 ++index)
			output.steps.push_back("Rejected call fact `" +
								   std::string{result.value().rejected.at(index).value()} + "`.");
		for (const auto& match : result.value().matches)
			for (const auto& item : match.why.items())
				output.evidence.push_back(item);
		output.unresolved_items = result.value().unresolved_items;
		if (!result.value().coverage.complete())
			output.suggested_actions.emplace_back(
				"Resolve failed or unresolved coverage units and retry.");
		if (!output.unresolved_items.empty())
			output.suggested_actions.emplace_back(
				"Inspect stable unresolved codes before treating absence as a negative match.");
		return output;
	}

	std::string agent_task_card::to_markdown() const
	{
		std::ostringstream output;
		output << "# cxxlens agent task\n\n" << goal << "\n";
		add_heading(output, "Required headers", required_headers);
		add_heading(output, "API calls", api_calls);
		add_heading(output, "Preconditions", preconditions);
		add_heading(output, "Expected outputs", expected_outputs);
		add_heading(output, "Failure modes", failure_modes);
		add_heading(output, "Verification", verification_steps);
		return output.str();
	}

	std::string agent_task_card::to_json() const
	{
		json_value document{
			detail::json::envelope({"cxxlens.agent-task-card.v1"},
								   {{"api_calls", strings(api_calls)},
									{"expected_outputs", strings(expected_outputs)},
									{"failure_modes", strings(failure_modes)},
									{"goal", goal},
									{"preconditions", strings(preconditions)},
									{"required_headers", strings(required_headers)},
									{"verification_steps", strings(verification_steps)}})};
		return detail::json::write(document).value();
	}

	agent_task_card for_selector(const select::semantic_selector& value)
	{
		agent_task_card output;
		output.goal = "Execute the normalized selector and preserve semantic uncertainty.";
		output.required_headers = {"<cxxlens/search.hpp>"};
		output.api_calls = {"cxxlens::search::calls",
							"cxxlens::search_report::matches",
							"cxxlens::search_report::coverage",
							"cxxlens::search_report::unresolved_items"};
		output.preconditions = {"Open a workspace with exact compile commands.",
								"Provision selector-required facts without silent fallback.",
								"Selector: " + value.to_json()};
		output.expected_outputs = {"Canonical matches",
								   "Structured evidence",
								   "Coverage",
								   "Guarantee",
								   "Unresolved prerequisites"};
		output.failure_modes = {"search.required-facts-unavailable",
								"search.refinement-failed",
								"core.cancelled",
								"core.budget-exhausted"};
		output.verification_steps = {"Validate the report schema.",
									 "Check matched/rejected/unresolved accounting.",
									 "Compare jobs 1/2/8 and warm/cold canonical JSON."};
		return output;
	}
} // namespace cxxlens::explain
