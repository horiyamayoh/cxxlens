#pragma once

/** @file qa.hpp @brief Stable QA contract declarations. */

#include <chrono>
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

namespace cxxlens::qa
{
	enum class sanitizer_kind : std::uint8_t
	{
		address,
		leak,
		undefined_behavior,
		thread,
		memory,
	};

	enum class coverage_mode : std::uint8_t
	{
		none,
		source_regions,
		branches,
		functions,
	};

	enum class step_requirement : std::uint8_t
	{
		required,
		optional,
	};

	class profile
	{
	  public:
		[[nodiscard]] static profile memory();
		[[nodiscard]] static profile concurrency();
		[[nodiscard]] static profile undefined_behavior();
		[[nodiscard]] static profile coverage();
		[[nodiscard]] profile sanitizer(sanitizer_kind value) const;
		[[nodiscard]] profile coverage_mode_(coverage_mode value) const;
		[[nodiscard]] profile
		build_target(std::string value,
					 step_requirement requirement = step_requirement::required) const;
		[[nodiscard]] profile
		test_argv(std::vector<std::string> value,
				  step_requirement requirement = step_requirement::required) const;
		[[nodiscard]] profile environment(std::string key, std::string value) const;
		[[nodiscard]] profile timeout(std::chrono::seconds value) const;
		[[nodiscard]] profile retry_limit(std::size_t value) const;
		[[nodiscard]] result<void> validate() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	struct process_policy
	{
		std::vector<path> executable_allowlist;
		path working_directory_root;
		std::vector<std::string> inherited_environment_allowlist;
		std::vector<std::string> redacted_environment;
		std::size_t stdout_limit_bytes{16 * 1024 * 1024};
		std::size_t stderr_limit_bytes{16 * 1024 * 1024};
		std::chrono::seconds timeout{300};
		std::size_t max_parallel_processes{1};
		bool network_allowed{};
		bool shell_allowed{};
	};

	enum class step_state : std::uint8_t
	{
		passed,
		failed,
		skipped,
		unavailable,
		unsupported,
		cancelled,
		timed_out,
		crashed,
		partial,
	};

	class dynamic_report
	{
	  public:
		[[nodiscard]] const finding_set& findings() const noexcept;
		[[nodiscard]] const coverage_report& coverage() const noexcept;
		[[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		[[nodiscard]] result_guarantee guarantee() const noexcept;
		[[nodiscard]] std::string_view build_identity() const noexcept;
		[[nodiscard]] std::string to_json() const;
		[[nodiscard]] std::string to_sarif() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	struct coverage_import_options
	{
		path profile_data;
		std::vector<path> binaries;
		path source_root;
		std::string expected_build_id;
		std::size_t input_budget_bytes{64 * 1024 * 1024};
		bool changed_lines_only{};
	};

	class coverage_data
	{
	  public:
		[[nodiscard]] const coverage_report& coverage() const noexcept;
		[[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		[[nodiscard]] std::string_view identity() const noexcept;
		[[nodiscard]] std::string to_json() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	class workflow
	{
	  public:
		[[nodiscard]] static workflow for_project(const workspace& workspace);
		[[nodiscard]] workflow use(profile value) const;
		[[nodiscard]] workflow process_policy_(process_policy value) const;
		[[nodiscard]] workflow import_coverage(bool value = true) const;
		[[nodiscard]] workflow associate_with(finding_set value) const;
		[[nodiscard]] workflow continue_on_optional_failure(bool value = true) const;
		[[nodiscard]] result<dynamic_report> run(execution_context execution = {}) const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	[[nodiscard]] result<coverage_data> import_source_coverage(const workspace& workspace,
															   coverage_import_options options,
															   execution_context execution = {});
} // namespace cxxlens::qa
