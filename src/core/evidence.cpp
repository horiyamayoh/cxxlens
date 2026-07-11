#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/core/evidence.hpp>

namespace cxxlens
{
	namespace
	{
		[[nodiscard]] error contract_error(std::string field, std::string reason)
		{
			error failure;
			failure.code.value = "core.schema-validation-failed";
			failure.message = "evidence or coverage contract validation failed";
			failure.attributes.emplace("field", std::move(field));
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		[[nodiscard]] bool valid_namespaced(const std::string_view value)
		{
			if (value.empty() || value.front() == '.' || value.back() == '.' ||
				value.find('.') == std::string_view::npos)
				return false;
			bool dot = false;
			for (const char character : value)
			{
				const auto byte = static_cast<unsigned char>(character);
				if (character == '.')
				{
					if (dot)
						return false;
					dot = true;
					continue;
				}
				dot = false;
				if (std::islower(byte) == 0 && std::isdigit(byte) == 0 && character != '-' &&
					character != '_')
					return false;
			}
			return true;
		}

		template <class T>
		[[nodiscard]] bool sorted_unique(const std::vector<T>& values)
		{
			if constexpr (requires(const T& value) { value.value(); })
			{
				return std::adjacent_find(values.begin(),
										  values.end(),
										  [](const T& left, const T& right)
										  {
											  return left.value() >= right.value();
										  }) == values.end();
			}
			else
			{
				return std::adjacent_find(values.begin(), values.end(), std::greater_equal{}) ==
					values.end();
			}
		}

		[[nodiscard]] std::string framed(const std::string_view value)
		{
			return std::to_string(value.size()) + ':' + std::string{value};
		}

		[[nodiscard]] std::string json_escape(const std::string_view input)
		{
			std::string output;
			for (const char character : input)
			{
				switch (character)
				{
					case '\\':
						output += "\\\\";
						break;
					case '"':
						output += "\\\"";
						break;
					case '\n':
						output += "\\n";
						break;
					case '\r':
						output += "\\r";
						break;
					case '\t':
						output += "\\t";
						break;
					default:
						output += character;
						break;
				}
			}
			return output;
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
			return index < names.size() ? std::string{names.at(index)} : std::string{};
		}

		[[nodiscard]] std::string evidence_key(const evidence_item& item)
		{
			std::string output = "kind=" + std::to_string(static_cast<std::uint16_t>(item.kind));
			if (item.source)
				output += "|source=" + framed(item.source->to_canonical_json());
			for (const auto& fact : item.supporting_facts)
				output += "|fact=" + framed(fact.value());
			for (const auto& [key, value] : item.attributes)
				output += "|attribute=" + framed(key) + framed(value);
			return output;
		}

		void normalize(evidence_item& item)
		{
			std::ranges::sort(item.supporting_facts,
							  {},
							  [](const fact_id& id)
							  {
								  return id.value();
							  });
			item.supporting_facts.erase(
				std::unique(item.supporting_facts.begin(), item.supporting_facts.end()),
				item.supporting_facts.end());
		}

		[[nodiscard]] bool request_less(const coverage_request& left, const coverage_request& right)
		{
			return std::tie(left.kind, left.id) < std::tie(right.kind, right.id);
		}

		[[nodiscard]] bool unit_less(const coverage_unit& left, const coverage_unit& right)
		{
			return std::tie(left.kind, left.id, left.state, left.reason) <
				std::tie(right.kind, right.id, right.state, right.reason);
		}

		[[nodiscard]] bool same_key(const coverage_request& request, const coverage_unit& unit)
		{
			return request.kind == unit.kind && request.id == unit.id;
		}

		[[nodiscard]] std::string coverage_state_name(const coverage_state state)
		{
			constexpr std::array<std::string_view, 5U> names{
				"covered", "excluded", "failed", "unresolved", "not_applicable"};
			const auto index = static_cast<std::size_t>(state);
			return index < names.size() ? std::string{names.at(index)} : std::string{};
		}

		[[nodiscard]] bool valid_request(const coverage_request& request)
		{
			return valid_namespaced("unit." + request.kind) && !request.id.empty() &&
				request.id.front() != '/';
		}

		[[nodiscard]] bool has_approximation(const evidence& why)
		{
			return std::ranges::any_of(why.items(),
									   [](const evidence_item& item)
									   {
										   return item.kind == evidence_kind::approximation;
									   });
		}
	} // namespace

	evidence& evidence::add(evidence_item item)
	{
		normalize(item);
		const auto key = evidence_key(item);
		const auto position = std::ranges::lower_bound(items_,
													   key,
													   {},
													   [](const evidence_item& existing)
													   {
														   return evidence_key(existing);
													   });
		if (position != items_.end() && evidence_key(*position) == key)
		{
			if (item.summary < position->summary)
				position->summary = std::move(item.summary);
			return *this;
		}
		items_.insert(position, std::move(item));
		return *this;
	}

	evidence& evidence::merge(const evidence& other)
	{
		for (const auto& item : other.items_)
			add(item);
		return *this;
	}

	std::span<const evidence_item> evidence::items() const noexcept
	{
		return items_;
	}

	result<void> evidence::validate(const evidence_validation_context& context) const
	{
		if (!sorted_unique(context.snapshot_facts))
			return contract_error("snapshot_facts", "must-be-sorted-unique");
		if (!sorted_unique(context.custom_factories))
			return contract_error("custom_factories", "must-be-sorted-unique");
		std::string previous;
		for (const auto& item : items_)
		{
			const auto kind_name = evidence_kind_name(item.kind);
			if (kind_name.empty())
				return contract_error("kind", "unknown-evidence-kind");
			const auto key = evidence_key(item);
			if (!previous.empty() && previous >= key)
				return contract_error("items", "noncanonical-or-duplicate");
			previous = key;
			if (item.source && item.source->validate())
				return contract_error("source", "invalid-source-span");
			if (item.kind == evidence_kind::source && !item.source)
				return contract_error("source", "required-for-source-kind");
			if (!sorted_unique(item.supporting_facts))
				return contract_error("supporting_facts", "must-be-sorted-unique");
			for (const auto& fact : item.supporting_facts)
			{
				if (!fact.valid())
					return contract_error("supporting_facts", "invalid-fact-id");
				if (!std::binary_search(context.snapshot_facts.begin(),
										context.snapshot_facts.end(),
										fact,
										[](const fact_id& left, const fact_id& right)
										{
											return left.value() < right.value();
										}))
					return contract_error("supporting_facts", "fact-not-in-snapshot");
			}
			for (const auto& [attribute, value] : item.attributes)
			{
				if (!valid_namespaced("attribute." + attribute) || value.empty())
					return contract_error("attributes", "invalid-attribute");
			}
			if (item.kind == evidence_kind::api_model && !item.attributes.contains("model.version"))
				return contract_error("attributes", "model-version-required");
			if (item.kind == evidence_kind::build_context &&
				!item.attributes.contains("build.variant"))
				return contract_error("attributes", "build-variant-required");
			if (item.kind == evidence_kind::approximation &&
				!item.attributes.contains("approximation.kind"))
				return contract_error("attributes", "approximation-kind-required");
			if (item.kind == evidence_kind::exclusion &&
				!item.attributes.contains("exclusion.reason"))
				return contract_error("attributes", "exclusion-reason-required");
			if (item.kind == evidence_kind::custom)
			{
				const auto factory = item.attributes.find("factory.id");
				if (factory == item.attributes.end() ||
					!std::binary_search(context.custom_factories.begin(),
										context.custom_factories.end(),
										factory->second))
					return contract_error("attributes.factory.id", "unknown-custom-factory");
			}
			if (!item.source && item.supporting_facts.empty() && item.attributes.empty())
				return contract_error("item", "prose-only-evidence");
		}
		return {};
	}

	std::string evidence::semantic_representation() const
	{
		std::string output = "evidence.v1";
		for (const auto& item : items_)
			output += "|item=" + framed(evidence_key(item));
		return output;
	}

	std::string evidence::to_json() const
	{
		std::ostringstream output;
		output << R"({"schema":"cxxlens.evidence.v1","items":[)";
		for (std::size_t index = 0U; index < items_.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			const auto& item = items_.at(index);
			output << R"({"kind":")" << evidence_kind_name(item.kind) << R"(","summary":")"
				   << json_escape(item.summary) << R"(","source":)";
			if (item.source)
				output << item.source->to_canonical_json();
			else
				output << "null";
			output << R"(,"supporting_facts":[)";
			for (std::size_t fact_index = 0U; fact_index < item.supporting_facts.size();
				 ++fact_index)
			{
				if (fact_index != 0U)
					output << ',';
				output << '"' << item.supporting_facts.at(fact_index).value() << '"';
			}
			output << R"(],"attributes":{)";
			std::size_t attribute_index = 0U;
			for (const auto& [key, value] : item.attributes)
			{
				if (attribute_index++ != 0U)
					output << ',';
				output << '"' << json_escape(key) << R"(":")" << json_escape(value) << '"';
			}
			output << "}}";
		}
		output << "]}";
		return output.str();
	}

	std::string evidence::to_markdown() const
	{
		std::ostringstream output;
		for (const auto& item : items_)
			output << "- `" << evidence_kind_name(item.kind) << "`: " << item.summary << '\n';
		return output.str();
	}

	coverage_report& coverage_report::request(coverage_request request)
	{
		requested_.insert(std::ranges::upper_bound(requested_, request, request_less),
						  std::move(request));
		return *this;
	}

	coverage_report& coverage_report::classify(coverage_unit unit)
	{
		units_.insert(std::ranges::upper_bound(units_, unit, unit_less), std::move(unit));
		return *this;
	}

	coverage_report& coverage_report::merge(const coverage_report& other)
	{
		for (const auto& request_value : other.requested_)
			request(request_value);
		for (const auto& unit : other.units_)
			classify(unit);
		return *this;
	}

	result<void> coverage_report::validate() const
	{
		for (std::size_t index = 0U; index < requested_.size(); ++index)
		{
			const auto& request_value = requested_.at(index);
			if (!valid_request(request_value))
				return contract_error("requested", "invalid-unit-key");
			if (index != 0U && !request_less(requested_.at(index - 1U), request_value))
				return contract_error("requested", "duplicate-or-noncanonical");
		}
		for (std::size_t index = 0U; index < units_.size(); ++index)
		{
			const auto& unit = units_.at(index);
			if (!valid_request({unit.kind, unit.id}))
				return contract_error("units", "invalid-unit-key");
			if (coverage_state_name(unit.state).empty())
				return contract_error("state", "unknown-coverage-state");
			if (unit.state == coverage_state::covered && unit.reason)
				return contract_error("reason", "covered-must-not-have-reason");
			if (unit.state != coverage_state::covered &&
				(!unit.reason || !valid_namespaced(*unit.reason)))
				return contract_error("reason", "stable-reason-required");
			if (index != 0U && units_.at(index - 1U).kind == unit.kind &&
				units_.at(index - 1U).id == unit.id)
				return contract_error("units", "multiple-terminal-states");
			if (!std::ranges::any_of(requested_,
									 [&unit](const coverage_request& request_value)
									 {
										 return same_key(request_value, unit);
									 }))
				return contract_error("units", "unrequested-unit");
		}
		if (requested_.size() != units_.size())
			return contract_error("units", "missing-terminal-state");
		return {};
	}

	bool coverage_report::complete() const
	{
		return validate().has_value() && count(coverage_state::failed) == 0U &&
			count(coverage_state::unresolved) == 0U;
	}

	std::span<const coverage_request> coverage_report::requested() const noexcept
	{
		return requested_;
	}

	std::span<const coverage_unit> coverage_report::units() const noexcept
	{
		return units_;
	}

	std::size_t coverage_report::count(const coverage_state state) const noexcept
	{
		return static_cast<std::size_t>(std::ranges::count_if(units_,
															  [state](const coverage_unit& unit)
															  {
																  return unit.state == state;
															  }));
	}

	std::vector<compile_unit_id> coverage_report::covered_compile_units() const
	{
		std::vector<compile_unit_id> output;
		for (const auto& unit : units_)
		{
			if (unit.kind == "compile-unit" && unit.state == coverage_state::covered)
				output.emplace_back(unit.id);
		}
		return output;
	}

	std::vector<compile_unit_id> coverage_report::failed_compile_units() const
	{
		std::vector<compile_unit_id> output;
		for (const auto& unit : units_)
		{
			if (unit.kind == "compile-unit" && unit.state == coverage_state::failed)
				output.emplace_back(unit.id);
		}
		return output;
	}

	std::string coverage_report::to_json() const
	{
		std::ostringstream output;
		output << R"({"schema":"cxxlens.coverage.v1","requested":[)";
		for (std::size_t index = 0U; index < requested_.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			const auto& request_value = requested_.at(index);
			output << R"({"kind":")" << json_escape(request_value.kind) << R"(","id":")"
				   << json_escape(request_value.id) << R"("})";
		}
		output << R"(],"units":[)";
		for (std::size_t index = 0U; index < units_.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			const auto& unit = units_.at(index);
			output << R"({"kind":")" << json_escape(unit.kind) << R"(","id":")"
				   << json_escape(unit.id) << R"(","state":")" << coverage_state_name(unit.state)
				   << R"(","reason":)";
			if (unit.reason)
				output << '"' << json_escape(*unit.reason) << '"';
			else
				output << "null";
			output << '}';
		}
		output << R"(],"summary":{)";
		constexpr std::array<coverage_state, 5U> states{
			coverage_state::covered,
			coverage_state::excluded,
			coverage_state::failed,
			coverage_state::unresolved,
			coverage_state::not_applicable,
		};
		for (std::size_t index = 0U; index < states.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			output << '"' << coverage_state_name(states.at(index)) << R"(":)"
				   << count(states.at(index));
		}
		output << R"(},"complete":)" << (complete() ? "true" : "false") << '}';
		return output.str();
	}

	std::string coverage_report::to_markdown() const
	{
		std::ostringstream output;
		output << "| Kind | ID | State | Reason |\n|---|---|---|---|\n";
		for (const auto& unit : units_)
			output << "| " << unit.kind << " | " << unit.id << " | "
				   << coverage_state_name(unit.state) << " | " << unit.reason.value_or("")
				   << " |\n";
		return output.str();
	}

	result<void> validate_result_contract(const result_guarantee guarantee,
										  const precision_level achieved,
										  const precision_level required,
										  const coverage_report& coverage,
										  const evidence& why)
	{
		if (static_cast<std::uint8_t>(guarantee) >
			static_cast<std::uint8_t>(result_guarantee::heuristic))
			return contract_error("guarantee", "unknown-guarantee");
		if (static_cast<std::uint8_t>(achieved) >
				static_cast<std::uint8_t>(precision_level::dynamic_observation) ||
			static_cast<std::uint8_t>(required) >
				static_cast<std::uint8_t>(precision_level::dynamic_observation))
			return contract_error("precision", "unknown-precision");
		if (auto validation = coverage.validate(); !validation)
			return validation;
		if (!coverage.requested().empty() && why.items().empty())
			return contract_error("evidence", "material-result-requires-evidence");
		if (guarantee == result_guarantee::exact_within_coverage)
		{
			if (!coverage.complete())
				return contract_error("guarantee", "exact-requires-complete-coverage");
			if (achieved < required)
				return contract_error("guarantee", "exact-requires-precision");
		}
		if ((guarantee == result_guarantee::sound_over_approximation ||
			 guarantee == result_guarantee::sound_under_approximation) &&
			!has_approximation(why))
			return contract_error("guarantee", "sound-approximation-requires-evidence");
		return {};
	}
} // namespace cxxlens
