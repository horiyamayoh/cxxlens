#pragma once

/** @file project_semantic_facts.hpp @brief Private compiler-to-public C++ relation extraction. */

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/provider/clang22.hpp>
#include <cxxlens/sdk/relation.hpp>

#include "provider_worker_v4_ast_observer.hpp"
#include "provider_worker_v4_output_normalizer.hpp"

namespace cxxlens::detail::clang22
{
	struct project_template_observations;
	/** Value-only preprocessing observations; emitted while the compiler evaluates directives. */
	struct project_preprocessor_event
	{
		std::string logical_path;
		std::uint64_t begin{}, end{};
		std::string kind, name, value, state;
		std::uint32_t location{}, definition_location{}, parent_location{};
		std::optional<std::size_t> parent_observation, closing_observation;
		std::optional<bool> function_like, variadic, argument_empty;
		std::optional<std::uint64_t> parameter_count, replacement_count, parameter_index,
			argument_index, substitution_count, evaluating_substitution_count;
		std::string parameter_name;
		std::vector<std::uint32_t> argument_locations;
		std::optional<bool> active;
		bool macro_complete{}, expansion_context_complete{};
		std::uint64_t context_depth{};
	};
	struct project_preprocessor_observations
	{
		struct token
		{
			std::string logical_path, kind, spelling;
			std::uint64_t begin{}, end{};
			bool macro{};
		};
		std::vector<project_preprocessor_event> events;
		std::size_t retained_bytes{};
		std::map<std::string, std::uint64_t, std::less<>> event_admissions;
		bool truncated{};
		std::vector<token> expanded_tokens;
		std::set<std::string, std::less<>> opened_files, classification_incomplete;
		std::size_t token_bytes{};
		bool tokens_truncated{}, tokens_unmapped{};
	};
	/** The native job owns callbacks; closure and detached output live until the job returns. */
	void install_project_preprocessor_observer(clang::Preprocessor& preprocessor,
											   const source_closure_snapshot& closure,
											   project_preprocessor_observations& output);
	struct project_semantic_facts
	{
		std::vector<sdk::detached_row> rows;
		std::vector<sdk::provider::unresolved_item> unresolved;
		// Additive fields of existing original call rows, replaced by original row identity.
		std::map<std::string, sdk::detached_row, std::less<>> call_updates;
	};
	using project_original_calls = std::map<const clang::Expr*, std::string>;

	/** Detach declarations, semantic edges, syntax, PP, CFG and local flow before AST release. */
	[[nodiscard]] sdk::result<project_semantic_facts>
	observe_project_semantics(provider::clang22::borrowed_translation_unit& unit,
							  const source_closure_snapshot& closure,
							  const provider_worker_v4_ast_observation_batch& observations,
							  const provider_worker_v4_normalized_output& normalized,
							  const project_preprocessor_observations& preprocessing,
							  const std::function<void(std::string_view)>& progress = {},
							  const project_original_calls& original_calls = {},
							  const std::string& project_id = {},
							  const project_template_observations* templates = nullptr);
} // namespace cxxlens::detail::clang22
