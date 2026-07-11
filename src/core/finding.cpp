#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <cxxlens/core/finding.hpp>

#include "../runtime/hash_port.hpp"
#include "canonical_encoding.hpp"
#include "json_projections.hpp"

namespace cxxlens
{
	namespace
	{
		using detail::identity::canonical_encoder;
		using detail::identity::collision_registry;
		using detail::identity::encoding_versions;
		using detail::identity::identity_error_code;
		using detail::identity::identity_service;

		[[nodiscard]] error finding_error(std::string code, std::string field, std::string reason)
		{
			error failure;
			failure.code.value = std::move(code);
			failure.message = "finding contract validation failed";
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

		[[nodiscard]] bool valid_semantic_id(const std::string_view value)
		{
			const auto separator = value.rfind('_');
			if (separator == std::string_view::npos || separator == 0U ||
				value.size() - separator - 1U != 64U)
				return false;
			for (std::size_t index = separator + 1U; index < value.size(); ++index)
			{
				const char character = value.at(index);
				if ((character < '0' || character > '9') && (character < 'a' || character > 'f'))
					return false;
			}
			return true;
		}

		[[nodiscard]] std::string framed(const std::string_view value)
		{
			return std::to_string(value.size()) + ':' + std::string{value};
		}

		[[nodiscard]] std::string severity_name(const severity value)
		{
			constexpr std::array<std::string_view, 5U> names{
				"note", "info", "warning", "error", "fatal"};
			return std::string{names.at(static_cast<std::size_t>(value))};
		}

		[[nodiscard]] std::string confidence_name(const confidence value)
		{
			constexpr std::array<std::string_view, 5U> names{
				"speculative", "possible", "probable", "high", "certain"};
			return std::string{names.at(static_cast<std::size_t>(value))};
		}

		[[nodiscard]] std::string guarantee_name(const result_guarantee value)
		{
			constexpr std::array<std::string_view, 5U> names{"exact_within_coverage",
															 "sound_over_approximation",
															 "sound_under_approximation",
															 "best_effort",
															 "heuristic"};
			return std::string{names.at(static_cast<std::size_t>(value))};
		}

		[[nodiscard]] result<finding_id> make_finding_id(const finding_input& input)
		{
			canonical_encoder encoder{"cxxlens.finding-id.v1", encoding_versions{1U, 1U}};
			encoder.string_field("rule_or_recipe", input.rule_or_recipe)
				.string_field("subject", input.subject_semantic_id)
				.string_field("source_file", std::string{input.primary.primary.begin.file.value()})
				.unsigned_field("source_begin", input.primary.primary.begin.byte_offset)
				.unsigned_field("source_end", input.primary.primary.end.byte_offset)
				.optional_string("variant", input.variant_signature);
			std::vector<std::pair<std::string, std::string>> parameters{
				input.identity_parameters.begin(), input.identity_parameters.end()};
			encoder.string_map("parameters", std::move(parameters));
			const auto payload = encoder.finish();
			if (!payload)
				return finding_error(
					"core.invalid-argument", "identity", "canonical-input-rejected");

			static detail::runtime::fnv1a_hash_adapter hashes;
			static collision_registry collisions;
			static std::mutex collision_mutex;
			const identity_service identities{hashes};
			std::lock_guard lock{collision_mutex};
			const auto generated = identities.make_id("finding", payload.value(), collisions);
			if (!generated)
			{
				const auto code = generated.error().code == identity_error_code::collision
					? "facts.identity-collision"
					: "core.internal-invariant-violation";
				return finding_error(code, "id", "identity-generation-failed");
			}
			return finding_id{generated.value()};
		}

		void normalize_unresolved(std::vector<unresolved>& items)
		{
			std::ranges::sort(items,
							  {},
							  [](const unresolved& item)
							  {
								  return item.semantic_representation();
							  });
			items.erase(std::unique(items.begin(),
									items.end(),
									[](const unresolved& left, const unresolved& right)
									{
										return left.semantic_representation() ==
											right.semantic_representation();
									}),
						items.end());
		}

	} // namespace

	result<void> diagnostic::validate() const
	{
		if (!valid_namespaced(id))
			return finding_error("core.schema-validation-failed", "id", "invalid-diagnostic-id");
		if (static_cast<std::uint8_t>(level) > static_cast<std::uint8_t>(severity::fatal))
			return finding_error("core.schema-validation-failed", "level", "unknown-severity");
		if (primary && primary->validate())
			return finding_error("core.schema-validation-failed", "primary", "invalid-source-span");
		std::string previous;
		for (const auto& span : related)
		{
			if (span.validate())
				return finding_error(
					"core.schema-validation-failed", "related", "invalid-source-span");
			const auto key = span.to_canonical_json();
			if (!previous.empty() && previous >= key)
				return finding_error(
					"core.schema-validation-failed", "related", "noncanonical-order");
			previous = key;
		}
		return {};
	}

	std::string diagnostic::to_json() const
	{
		return detail::json::write(detail::json::diagnostic_value(*this)).value();
	}

	result<finding> finding::make(finding_input input, const finding_validation_context& context)
	{
		finding value;
		value.rule_or_recipe_ = input.rule_or_recipe;
		value.subject_semantic_id_ = input.subject_semantic_id;
		value.primary_ = input.primary;
		value.variant_signature_ = input.variant_signature;
		value.identity_parameters_ = input.identity_parameters;
		value.level_ = input.level;
		value.certainty_ = input.certainty;
		value.guarantee_ = input.guarantee;
		value.message_ = input.message;
		value.why_ = input.why;
		normalize_unresolved(input.unresolved_items);
		value.unresolved_items_ = std::move(input.unresolved_items);
		value.coverage_ = input.coverage;
		value.achieved_precision_ = input.achieved_precision;
		value.required_precision_ = input.required_precision;
		const auto generated_id = make_finding_id(input);
		if (!generated_id)
			return generated_id.error();
		value.id_ = generated_id.value();
		if (auto validation = value.validate(context); !validation)
			return std::move(validation.error());
		return value;
	}

	finding_id finding::id() const
	{
		return id_;
	}

	std::string_view finding::rule_or_recipe() const noexcept
	{
		return rule_or_recipe_;
	}

	severity finding::level() const noexcept
	{
		return level_;
	}

	confidence finding::certainty() const noexcept
	{
		return certainty_;
	}

	result_guarantee finding::guarantee() const noexcept
	{
		return guarantee_;
	}

	const source_span& finding::primary_location() const noexcept
	{
		return primary_;
	}

	std::string_view finding::message() const noexcept
	{
		return message_;
	}

	const evidence& finding::why() const noexcept
	{
		return why_;
	}

	std::span<const unresolved> finding::unresolved_items() const noexcept
	{
		return unresolved_items_;
	}

	const coverage_report& finding::coverage() const noexcept
	{
		return coverage_;
	}

	finding_explanation_input finding::explanation_input() const noexcept
	{
		return {id_, rule_or_recipe_, why_.items(), unresolved_items_, coverage_.units()};
	}

	result<void> finding::validate(const finding_validation_context& context) const
	{
		if (!valid_namespaced(rule_or_recipe_))
			return finding_error("core.schema-validation-failed", "rule_or_recipe", "invalid-code");
		if (!valid_semantic_id(subject_semantic_id_))
			return finding_error("core.schema-validation-failed", "subject", "invalid-semantic-id");
		if (primary_.validate())
			return finding_error("core.schema-validation-failed", "primary", "invalid-source-span");
		if (variant_signature_ &&
			(variant_signature_->empty() || variant_signature_->front() == '/'))
			return finding_error("core.schema-validation-failed", "variant", "invalid-signature");
		for (const auto& [key, value] : identity_parameters_)
		{
			if (!valid_namespaced("parameter." + key) || value.empty())
				return finding_error(
					"core.schema-validation-failed", "parameters", "invalid-parameter");
		}
		if (static_cast<std::uint8_t>(level_) > static_cast<std::uint8_t>(severity::fatal))
			return finding_error("core.schema-validation-failed", "level", "unknown-severity");
		if (static_cast<std::uint8_t>(certainty_) > static_cast<std::uint8_t>(confidence::certain))
			return finding_error(
				"core.schema-validation-failed", "confidence", "unknown-confidence");
		if (auto validation = why_.validate(context.evidence_context); !validation)
			return validation;
		std::string previous;
		for (const auto& item : unresolved_items_)
		{
			if (auto validation = item.validate(context.capabilities); !validation)
				return validation;
			const auto key = item.semantic_representation();
			if (!previous.empty() && previous >= key)
				return finding_error(
					"core.schema-validation-failed", "unresolved", "noncanonical-order");
			previous = key;
		}
		if (!unresolved_items_.empty() && guarantee_ == result_guarantee::exact_within_coverage)
			return finding_error(
				"core.schema-validation-failed", "guarantee", "exact-finding-has-unresolved");
		if (auto validation = validate_result_contract(
				guarantee_, achieved_precision_, required_precision_, coverage_, why_);
			!validation)
			return validation;
		finding_input identity_input;
		identity_input.rule_or_recipe = rule_or_recipe_;
		identity_input.subject_semantic_id = subject_semantic_id_;
		identity_input.primary = primary_;
		identity_input.variant_signature = variant_signature_;
		identity_input.identity_parameters = identity_parameters_;
		const auto expected = make_finding_id(identity_input);
		if (!expected || expected.value() != id_)
			return finding_error("core.internal-invariant-violation", "id", "identity-mismatch");
		return {};
	}

	std::string finding::semantic_representation() const
	{
		std::string output = "finding-payload.v1|id=" + framed(id_.value()) +
			"|message=" + framed(message_) +
			"|severity=" + std::to_string(static_cast<std::uint8_t>(level_)) +
			"|confidence=" + std::to_string(static_cast<std::uint8_t>(certainty_)) +
			"|guarantee=" + std::to_string(static_cast<std::uint8_t>(guarantee_)) +
			"|evidence=" + framed(why_.semantic_representation()) +
			"|coverage=" + framed(coverage_.to_json());
		for (const auto& item : unresolved_items_)
			output += "|unresolved=" + framed(item.semantic_representation());
		return output;
	}

	std::string finding::to_json() const
	{
		using detail::json::json_value;
		json_value::object parameters;
		for (const auto& [key, value] : identity_parameters_)
			parameters.emplace_back(key, value);
		json_value::array unresolved_rows;
		for (const auto& item : unresolved_items_)
			unresolved_rows.emplace_back(detail::json::unresolved_value(item));
		json_value::object fields{
			{"id", std::string{id_.value()}},
			{"rule_or_recipe", rule_or_recipe_},
			{"subject_semantic_id", subject_semantic_id_},
			{"primary", detail::json::source_span_value(primary_)},
			{"variant_signature",
			 variant_signature_ ? json_value{*variant_signature_} : json_value{}},
			{"identity_parameters", std::move(parameters)},
			{"severity", severity_name(level_)},
			{"confidence", confidence_name(certainty_)},
			{"guarantee", guarantee_name(guarantee_)},
			{"message", message_},
			{"evidence", detail::json::evidence_value(why_)},
			{"unresolved", std::move(unresolved_rows)},
			{"coverage", detail::json::coverage_value(coverage_)},
			{"achieved_precision", static_cast<std::uint64_t>(achieved_precision_)},
			{"required_precision", static_cast<std::uint64_t>(required_precision_)},
		};
		return detail::json::write(
				   json_value{detail::json::envelope(
					   detail::json::document_versions{"cxxlens.finding.v1"}, std::move(fields))})
			.value();
	}

	result<void> finding_set::add(finding value)
	{
		if (!value.id_.valid())
			return finding_error("core.schema-validation-failed", "finding.id", "invalid-id");
		for (const auto& existing : rows_)
		{
			if (existing.id_ == value.id_)
			{
				if (existing.semantic_representation() == value.semantic_representation())
					return {};
				return finding_error(
					"core.internal-invariant-violation", "finding.id", "conflicting-duplicate");
			}
		}
		const auto less = [](const finding& left, const finding& right)
		{
			return std::tuple{left.rule_or_recipe_,
							  left.primary_.primary.begin.file.value(),
							  left.primary_.primary.begin.byte_offset,
							  left.primary_.primary.end.byte_offset,
							  left.subject_semantic_id_,
							  left.variant_signature_,
							  left.id_.value()} <
				std::tuple{right.rule_or_recipe_,
						   right.primary_.primary.begin.file.value(),
						   right.primary_.primary.begin.byte_offset,
						   right.primary_.primary.end.byte_offset,
						   right.subject_semantic_id_,
						   right.variant_signature_,
						   right.id_.value()};
		};
		const auto position = std::ranges::lower_bound(rows_, value, less);
		rows_.insert(position, std::move(value));
		return {};
	}

	std::size_t finding_set::size() const noexcept
	{
		return rows_.size();
	}

	bool finding_set::empty() const noexcept
	{
		return rows_.empty();
	}

	std::span<const finding> finding_set::all() const noexcept
	{
		return rows_;
	}

	finding_set finding_set::minimum_confidence(const confidence minimum) const
	{
		finding_set output;
		for (const auto& item : rows_)
		{
			if (item.certainty_ >= minimum)
				output.rows_.push_back(item);
		}
		return output;
	}

	finding_set finding_set::minimum_severity(const severity minimum) const
	{
		finding_set output;
		for (const auto& item : rows_)
		{
			if (item.level_ >= minimum)
				output.rows_.push_back(item);
		}
		return output;
	}

	std::string finding_set::to_json() const
	{
		using detail::json::json_value;
		json_value::array findings;
		for (const auto& item : rows_)
		{
			json_value::object parameters;
			for (const auto& [key, value] : item.identity_parameters_)
				parameters.emplace_back(key, value);
			json_value::array unresolved_rows;
			for (const auto& unresolved_item : item.unresolved_items_)
				unresolved_rows.emplace_back(detail::json::unresolved_value(unresolved_item));
			findings.emplace_back(json_value::object{
				{"id", std::string{item.id_.value()}},
				{"rule_or_recipe", item.rule_or_recipe_},
				{"subject_semantic_id", item.subject_semantic_id_},
				{"primary", detail::json::source_span_value(item.primary_)},
				{"variant_signature",
				 item.variant_signature_ ? json_value{*item.variant_signature_} : json_value{}},
				{"identity_parameters", std::move(parameters)},
				{"severity", severity_name(item.level_)},
				{"confidence", confidence_name(item.certainty_)},
				{"guarantee", guarantee_name(item.guarantee_)},
				{"message", item.message_},
				{"evidence", detail::json::evidence_value(item.why_)},
				{"unresolved", std::move(unresolved_rows)},
				{"coverage", detail::json::coverage_value(item.coverage_)},
				{"achieved_precision", static_cast<std::uint64_t>(item.achieved_precision_)},
				{"required_precision", static_cast<std::uint64_t>(item.required_precision_)},
			});
		}
		return detail::json::write(json_value{detail::json::envelope(
									   detail::json::document_versions{"cxxlens.finding-set.v1"},
									   {{"findings", std::move(findings)}})})
			.value();
	}
} // namespace cxxlens
