#pragma once

/**
 * @file core_findings.hpp
 * @brief Issue #43 Contract Candidate declarations for `finding` and `finding_set`.
 *
 * This is a non-installed declaration source. Issue #53 owns integration into the stable public
 * headers; this file intentionally has no production implementation.
 */

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/core/evidence.hpp>

namespace cxxlens
{
	/** @brief Finding severity. Severity is material payload but is excluded from identity. */
	enum class severity : std::uint8_t
	{
		note,
		info,
		warning,
		error,
		fatal,
	};

	/** @brief Identity and authoritative payload used by `finding::make`. */
	struct finding_input
	{
		std::string rule_or_recipe;
		std::string subject_semantic_id;
		source_span primary;
		std::optional<std::string> variant_signature;
		std::map<std::string, std::string> identity_parameters;
		severity level{severity::warning};
		confidence certainty{confidence::possible};
		result_guarantee guarantee{result_guarantee::best_effort};
		std::string message;
		evidence why;
		std::vector<unresolved> unresolved_items;
		coverage_report coverage;
		precision_level achieved_precision{precision_level::ast_structural};
		precision_level required_precision{precision_level::ast_structural};
	};

	/** @brief Independent registries used to validate a finding. */
	struct finding_validation_context
	{
		evidence_validation_context evidence_context;
		std::vector<std::string> capabilities;
	};

	/** @brief Renderer-neutral views over authoritative explanation inputs. */
	struct finding_explanation_input
	{
		finding_id id;
		std::string rule_or_recipe;
		std::span<const evidence_item> evidence_items;
		std::span<const unresolved> unresolved_items;
		std::span<const coverage_unit> coverage_units;
	};

	/** @brief Stable semantic result about user code. */
	class finding
	{
	  public:
		/** @brief Construct and independently validate a finding. */
		[[nodiscard]] static result<finding> make(
			finding_input input, const finding_validation_context& context);
		/** @brief Return the full stable ID; message and severity are not identity inputs. */
		[[nodiscard]] finding_id id() const;
		/** @brief Return the stable producer rule or recipe ID. */
		[[nodiscard]] std::string_view rule_or_recipe() const noexcept;
		/** @brief Return display/gate severity. */
		[[nodiscard]] severity level() const noexcept;
		/** @brief Return typed confidence without upgrading precision or guarantee. */
		[[nodiscard]] confidence certainty() const noexcept;
		/** @brief Return the validated result guarantee. */
		[[nodiscard]] result_guarantee guarantee() const noexcept;
		/** @brief Return the identity-relevant normalized primary location. */
		[[nodiscard]] const source_span& primary_location() const noexcept;
		/** @brief Return display prose, which is never a control-flow input. */
		[[nodiscard]] std::string_view message() const noexcept;
		/** @brief Return authoritative structured evidence. */
		[[nodiscard]] const evidence& why() const noexcept;
		/** @brief Return preserved uncertainty; empty and unresolved remain distinct. */
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		/** @brief Return authoritative coverage accounting. */
		[[nodiscard]] const coverage_report& coverage() const noexcept;
		/** @brief Return renderer-neutral explanation views; renderers own prose. */
		[[nodiscard]] finding_explanation_input explanation_input() const noexcept;
		/** @brief Revalidate identity, evidence, coverage, precision, and guarantee. */
		[[nodiscard]] result<void> validate(const finding_validation_context& context) const;
		/** @brief Return canonical material payload used for duplicate conflict detection. */
		[[nodiscard]] std::string semantic_representation() const;
		/** @brief Return canonical `cxxlens.finding.v1` JSON. */
		[[nodiscard]] std::string to_json() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	/** @brief Canonically ordered, conflict-checked finding collection. */
	class finding_set
	{
	  public:
		/** @brief Insert a unique row or reject a conflicting duplicate ID. */
		[[nodiscard]] result<void> add(finding value);
		/** @brief Return the unique row count. */
		[[nodiscard]] std::size_t size() const noexcept;
		/** @brief Return whether no authoritative finding rows exist. */
		[[nodiscard]] bool empty() const noexcept;
		/** @brief Return rows in canonical rule/source/subject/variant/full-ID order. */
		[[nodiscard]] std::span<const finding> all() const noexcept;
		/** @brief Return a pure inclusive confidence-threshold projection. */
		[[nodiscard]] finding_set minimum_confidence(confidence minimum) const;
		/** @brief Return a pure inclusive severity-threshold projection. */
		[[nodiscard]] finding_set minimum_severity(severity minimum) const;
		/** @brief Return canonical `cxxlens.finding-set.v1` JSON. */
		[[nodiscard]] std::string to_json() const;
		/** @brief Return Markdown projected only from authoritative rows. */
		[[nodiscard]] std::string to_markdown() const;
		/** @brief Return SARIF projected only from authoritative rows. */
		[[nodiscard]] std::string to_sarif() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};
} // namespace cxxlens
