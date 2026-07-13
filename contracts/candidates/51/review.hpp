#pragma once

/** @file review.hpp @brief Issue #51 non-installed review Contract Candidate. */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/core.hpp>
#include <cxxlens/core/failure.hpp>
#include <cxxlens/core/finding.hpp>
#include <cxxlens/source.hpp>
#include <cxxlens/workspace.hpp>

namespace cxxlens::rules
{
	class rule_pack;
}

namespace cxxlens::review
{
	enum class diff_side : std::uint8_t
	{
		before,
		after,
	};

	enum class file_change_kind : std::uint8_t
	{
		added,
		deleted,
		modified,
		renamed,
		copied,
		binary,
		submodule,
	};

	struct diff_options
	{
		std::string head_ref{"HEAD"};
		bool include_worktree{true};
		bool include_index{true};
		bool include_untracked{};
		std::size_t input_budget_bytes{16 * 1024 * 1024};
	};

	class diff_view
	{
	  public:
		[[nodiscard]] static result<diff_view> from_unified_diff(
			std::string_view input, path repository_root = {}, diff_options options = {});
		[[nodiscard]] static result<diff_view> from_git(
			path repository, std::string base_ref, diff_options options = {}, execution_context execution = {});
		[[nodiscard]] bool contains(const source_span& span, diff_side side = diff_side::after) const;
		[[nodiscard]] std::span<const path> changed_files() const noexcept;
		[[nodiscard]] std::string_view identity() const noexcept;
		[[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		[[nodiscard]] std::string to_json() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	enum class baseline_match_state : std::uint8_t
	{
		exact,
		equivalent,
		changed,
		new_finding,
		resolved,
		ambiguous,
	};

	struct baseline_options
	{
		std::size_t input_budget_bytes{16 * 1024 * 1024};
		bool allow_declared_migration{};
		bool trusted_input{};
	};

	class baseline
	{
	  public:
		[[nodiscard]] static result<baseline> load(path source, baseline_options options = {});
		[[nodiscard]] result<void> save(path destination, baseline_options options = {}) const;
		[[nodiscard]] baseline_match_state compare(const finding& value) const;
		[[nodiscard]] bool contains_equivalent(const finding& value) const;
		[[nodiscard]] std::string_view id() const noexcept;
		[[nodiscard]] semantic_version version() const noexcept;
		[[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		[[nodiscard]] std::string to_json() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	enum class gate_state : std::uint8_t
	{
		pass,
		warn,
		fail,
		indeterminate,
	};

	class gate_policy
	{
	  public:
		[[nodiscard]] static gate_policy no_new_errors();
		[[nodiscard]] static gate_policy no_high_confidence_security_findings();
		[[nodiscard]] gate_policy minimum_severity(severity value) const;
		[[nodiscard]] gate_policy minimum_confidence(confidence value) const;
		[[nodiscard]] gate_policy changed_lines_only(bool value = true) const;
		[[nodiscard]] gate_policy no_new_only(bool value = true) const;
		[[nodiscard]] gate_policy maximum_unresolved(std::size_t value) const;
		[[nodiscard]] gate_policy minimum_coverage(double value) const;
		[[nodiscard]] result<void> validate() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	class review_report
	{
	  public:
		[[nodiscard]] const finding_set& findings() const noexcept;
		[[nodiscard]] const coverage_report& coverage() const noexcept;
		[[nodiscard]] gate_state gate() const noexcept;
		[[nodiscard]] int exit_code() const noexcept;
		[[nodiscard]] std::string_view identity() const noexcept;
		[[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		[[nodiscard]] result_guarantee guarantee() const noexcept;
		[[nodiscard]] std::string to_json() const;
		[[nodiscard]] std::string to_sarif() const;
		[[nodiscard]] std::string to_markdown() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	class workflow
	{
	  public:
		[[nodiscard]] static workflow for_diff(diff_view value);
		[[nodiscard]] workflow add(rules::rule_pack value) const;
		[[nodiscard]] workflow add_findings(finding_set value) const;
		[[nodiscard]] workflow baseline_(baseline value) const;
		[[nodiscard]] workflow gate(gate_policy value) const;
		[[nodiscard]] workflow propose_fixes(bool value = true) const;
		[[nodiscard]] workflow affected_context(bool value = true) const;
		[[nodiscard]] result<review_report> run(const workspace& workspace, execution_context execution = {}) const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};
} // namespace cxxlens::review
