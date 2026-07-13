#pragma once

/** @file rules.hpp @brief Stable rules and reporting contract declarations. */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/select.hpp>
#include <cxxlens/workspace.hpp>

namespace cxxlens
{
	namespace transform
	{
		class codemod;
		class edit_plan;
	} // namespace transform
	namespace generate
	{
		class generation_plan;
	} // namespace generate
	namespace review
	{
		class review_report;
	} // namespace review
} // namespace cxxlens

namespace cxxlens::rules
{
	struct rule_metadata
	{
		std::string id;
		semantic_version version;
		std::string title;
		std::string description;
		std::string category;
		std::vector<std::string> tags;
		severity default_severity{severity::warning};
		confidence default_confidence{confidence::probable};
		std::optional<std::string> help_uri;
	};

	struct diagnostic_template
	{
		std::string text;
		std::vector<std::string> placeholders;
	};

	enum class rule_failure_policy : std::uint8_t
	{
		fail_fast,
		continue_partial,
	};

	enum class suppression_origin : std::uint8_t
	{
		inline_source,
		external_configuration,
		baseline,
	};

	struct suppression_entry
	{
		std::string id;
		suppression_origin origin{suppression_origin::external_configuration};
		std::optional<path> file;
		std::optional<source_span> range;
		std::optional<symbol_id> symbol;
		std::optional<std::string> rule_id;
		std::optional<std::string> category;
		std::optional<std::string> tag;
		std::optional<severity> minimum_severity;
		std::optional<std::string> baseline_finding_id;
		std::string justification;
		std::optional<std::string> owner;
		std::optional<std::chrono::sys_seconds> expires_at;
	};

	class rule;
	class rule_builder
	{
	  public:
		[[nodiscard]] rule_builder metadata(rule_metadata value) const;
		[[nodiscard]] rule_builder when(select::semantic_selector value) const;
		[[nodiscard]] rule_builder unless(select::semantic_selector value) const;
		[[nodiscard]] rule_builder scope(analysis_scope value) const;
		[[nodiscard]] rule_builder severity_level(severity value) const;
		[[nodiscard]] rule_builder confidence_at_least(confidence value) const;
		[[nodiscard]] rule_builder diagnose(diagnostic_template value) const;
		[[nodiscard]] rule_builder note(diagnostic_template value) const;
		[[nodiscard]] rule_builder fix(transform::codemod value) const;
		[[nodiscard]] rule_builder explain(std::string markdown) const;
		[[nodiscard]] result<rule> build() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	class suppression_policy
	{
	  public:
		[[nodiscard]] static suppression_policy none();
		[[nodiscard]] suppression_policy inline_nolint(bool value = true) const;
		[[nodiscard]] suppression_policy file(path value, std::string justification) const;
		[[nodiscard]] suppression_policy range(source_span value, std::string justification) const;
		[[nodiscard]] suppression_policy symbol(symbol_id value, std::string justification) const;
		[[nodiscard]] suppression_policy rule(std::string value, std::string justification) const;
		[[nodiscard]] suppression_policy category(std::string value,
												  std::string justification) const;
		[[nodiscard]] suppression_policy tag(std::string value, std::string justification) const;
		[[nodiscard]] suppression_policy minimum_severity(severity value) const;
		[[nodiscard]] suppression_policy baseline(std::vector<std::string> finding_ids) const;
		[[nodiscard]] suppression_policy external(std::vector<suppression_entry> entries) const;
		[[nodiscard]] suppression_policy require_justification(bool value = true) const;
		[[nodiscard]] suppression_policy reject_expired(bool value = true) const;
		[[nodiscard]] std::string to_json() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	struct rule_run_options
	{
		analysis_scope scope{analysis_scope::all()};
		suppression_policy suppressions{suppression_policy::none()};
		rule_failure_policy failure_policy{rule_failure_policy::continue_partial};
		std::optional<confidence> minimum_confidence;
		execution_context execution;
	};

	class rule
	{
	  public:
		[[nodiscard]] std::string_view id() const noexcept;
		[[nodiscard]] const rule_metadata& metadata() const noexcept;
		[[nodiscard]] select::selector_requirements requirements() const;
		[[nodiscard]] result<finding_set> run(const workspace& workspace,
											  rule_run_options options = {}) const;
		[[nodiscard]] std::string to_json() const;
		[[nodiscard]] std::string explain() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
		friend class rule_builder;
	};

	class rule_pack
	{
	  public:
		[[nodiscard]] rule_pack add(rule value) const;
		[[nodiscard]] rule_pack add(rule_pack value) const;
		[[nodiscard]] rule_pack enable(std::string pattern) const;
		[[nodiscard]] rule_pack disable(std::string pattern) const;
		[[nodiscard]] rule_pack suppressions(suppression_policy value) const;
		[[nodiscard]] rule_pack failure_policy(rule_failure_policy value) const;
		[[nodiscard]] result<finding_set> run(const workspace& workspace,
											  rule_run_options options = {}) const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	[[nodiscard]] rule_builder make_rule(std::string id, semantic_version version = {1, 0, 0, {}});
} // namespace cxxlens::rules

namespace cxxlens::report
{
	enum class format : std::uint8_t
	{
		json,
		markdown,
		sarif,
		unified_diff,
	};

	enum class path_presentation : std::uint8_t
	{
		project_relative,
		redacted_token,
		basename_only,
	};

	struct options
	{
		format output{format::markdown};
		path_presentation paths{path_presentation::project_relative};
		std::optional<semantic_version> schema_version;
		bool include_source_excerpt{true};
		bool include_evidence{true};
		bool include_unresolved{true};
		std::size_t source_context_lines{2};
		std::size_t output_budget_bytes{4 * 1024 * 1024};
	};

	struct rendered_report
	{
		std::string bytes;
		std::string media_type;
		std::string schema_id;
		bool truncated{};
		std::vector<unresolved> unresolved_items;
		result_guarantee guarantee{result_guarantee::best_effort};
	};

	[[nodiscard]] result<rendered_report> render(const finding_set& value, options settings = {});
	[[nodiscard]] result<rendered_report> render(const transform::edit_plan& value,
												 options settings = {});
	[[nodiscard]] result<rendered_report> render(const generate::generation_plan& value,
												 options settings = {});
	[[nodiscard]] result<rendered_report> render(const review::review_report& value,
												 options settings = {});
} // namespace cxxlens::report
