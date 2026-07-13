#pragma once

/** @file transform.hpp @brief Stable transform contract declarations. */

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

namespace cxxlens::transform
{
	enum class edit_kind : std::uint8_t
	{
		insert,
		replace,
		erase,
		create_file,
		delete_file,
	};

	enum class macro_edit_policy : std::uint8_t
	{
		reject,
		allow_argument_spelling,
		allow_definition_with_explicit_opt_in,
	};

	enum class generated_code_policy : std::uint8_t
	{
		read_only,
		allow_declared_generated_root,
	};

	enum class format_policy : std::uint8_t
	{
		none,
		changed_ranges,
		whole_changed_file,
	};

	enum class reparse_policy : std::uint8_t
	{
		changed_main_files,
		affected_variants,
		all_variants,
	};

	enum class apply_mode : std::uint8_t
	{
		dry_run,
		write_files,
	};

	enum class conflict_kind : std::uint8_t
	{
		overlap,
		insert_order,
		file_lifecycle,
		variant_divergence,
		macro_origin,
	};

	enum class transaction_state : std::uint8_t
	{
		dry_run_validated,
		prepared,
		committed,
		rolled_back,
		rollback_failed,
	};

	enum class file_apply_state : std::uint8_t
	{
		unchanged,
		prepared,
		committed,
		rolled_back,
		recovery_required,
	};

	struct edit_precondition
	{
		std::string expected_source_digest;
		std::string expected_file_identity;
		std::string catalog_version;
		std::string fact_snapshot_id;
		bool directly_spelled_required{true};
		std::vector<build_variant_id> verified_variants;
	};

	struct edit
	{
		std::string id;
		edit_kind kind{edit_kind::replace};
		path file;
		std::uint64_t begin{};
		std::uint64_t end{};
		std::string replacement;
		edit_precondition precondition;
		std::string atomic_group;
		evidence why;
	};

	struct edit_conflict
	{
		conflict_kind kind{conflict_kind::overlap};
		std::vector<std::string> edit_ids;
		std::vector<build_variant_id> variants;
		std::string stable_code;
	};

	struct plan_options
	{
		analysis_scope scope{analysis_scope::all()};
		macro_edit_policy macro_policy{macro_edit_policy::reject};
		generated_code_policy generated{generated_code_policy::read_only};
		format_policy formatting{format_policy::changed_ranges};
		reparse_policy reparse{reparse_policy::affected_variants};
		bool reject_new_diagnostics{true};
		execution_context execution;
	};

	struct preview_options
	{
		std::size_t context_lines{3};
		std::size_t output_budget_bytes{4 * 1024 * 1024};
	};

	struct preview_result
	{
		std::string unified_diff;
		bool truncated{};
		std::size_t omitted_files{};
		std::vector<unresolved> unresolved_items;
	};

	struct apply_options
	{
		apply_mode mode{apply_mode::dry_run};
		execution_context execution;
	};

	struct file_apply_result
	{
		path file;
		file_apply_state state{file_apply_state::unchanged};
		std::string before_digest;
		std::string after_digest;
		std::optional<path> recovery_artifact;
		std::vector<diagnostic> diagnostics;
	};

	class apply_result
	{
	  public:
		[[nodiscard]] transaction_state state() const noexcept;
		[[nodiscard]] bool committed() const noexcept;
		[[nodiscard]] std::string_view transaction_id() const noexcept;
		[[nodiscard]] std::span<const file_apply_result> files() const noexcept;
		[[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		[[nodiscard]] const coverage_report& coverage() const noexcept;
		[[nodiscard]] std::string to_json() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	class edit_plan
	{
	  public:
		[[nodiscard]] plan_id id() const noexcept;
		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] std::span<const edit> edits() const noexcept;
		[[nodiscard]] std::span<const edit_conflict> conflicts() const noexcept;
		[[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		[[nodiscard]] const coverage_report& coverage() const noexcept;
		[[nodiscard]] result<preview_result>
		preview_unified_diff(preview_options options = {}) const;
		[[nodiscard]] std::string to_json() const;
		[[nodiscard]] result<apply_result> apply(workspace& workspace,
												 apply_options options = {}) const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	class replacement_template
	{
	  public:
		[[nodiscard]] static replacement_template text(std::string value);
		[[nodiscard]] replacement_template argument(std::size_t index,
													std::string placeholder) const;
		[[nodiscard]] replacement_template preserve_comments(bool value = true) const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	class codemod
	{
	  public:
		[[nodiscard]] std::string_view id() const noexcept;
		[[nodiscard]] result<edit_plan> plan(workspace& workspace, plan_options options = {}) const;
		[[nodiscard]] std::string explain() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	[[nodiscard]] codemod replace_function_call(std::string old_qualified,
												std::string new_qualified);
	[[nodiscard]] codemod
	replace_method_call(std::string receiver_type, std::string old_method, std::string new_method);
	[[nodiscard]] codemod rewrite_calls(select::call_selector selector,
										replacement_template replacement);
	[[nodiscard]] codemod rename_symbol(select::symbol_selector selector, std::string new_name);
	[[nodiscard]] codemod add_include_where_needed(std::string header, select::symbol_selector use);
	[[nodiscard]] codemod remove_unused_includes();
} // namespace cxxlens::transform
