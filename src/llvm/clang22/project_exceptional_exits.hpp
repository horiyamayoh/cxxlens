#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/provider/clang22.hpp>

namespace clang
{
	class Decl;
	class FunctionDecl;
	class Stmt;
} // namespace clang

namespace cxxlens::detail::clang22
{
	struct exceptional_exit_limits
	{
		std::size_t maximum_operations{8'000'000U};
		std::size_t maximum_scopes{100'000U};
		std::size_t maximum_occurrences{1'000'000U};
		std::size_t maximum_retained_bytes{64U * 1024U * 1024U};
		std::function<bool()> cancelled;
	};
	struct exceptional_source
	{
		std::uint32_t begin{}, end{};
		bool declaration{};
	};
	struct exceptional_scope_binding
	{
		std::string_view detail, function, definition_source, body;
	};
	/** Exact original compiler-pointer lookups, borrowed only during this analysis job. */
	struct exceptional_compiler_bindings
	{
		std::function<exceptional_scope_binding(const clang::FunctionDecl*)> scope;
		std::function<std::string_view(const clang::Stmt*)> expression;
		std::function<std::string_view(const clang::Decl*)> target;
	};
	struct exceptional_occurrence
	{
		std::string role, eligibility, target_usr, expression_kind, expression, target;
		exceptional_source source;
		std::optional<std::size_t> original_expression_ordinal;
		std::optional<std::uint64_t> block_ordinal, instruction_ordinal, successor_ordinal;
		std::optional<bool> is_invoke, does_not_throw, does_not_return;
		std::optional<unsigned> intrinsic_id, compiler_route, emitter_methods;
		bool binding_complete{true};
	};
	struct exceptional_lowering_variant
	{
		std::string kind, symbol;
		unsigned index{};
		std::vector<exceptional_occurrence> occurrences;
		bool complete{};
		std::string reason;
	};
	struct exceptional_physical_scope
	{
		std::string owner_usr, detail, function, definition_source, body_id;
		exceptional_source declaration, body;
		std::vector<exceptional_lowering_variant> variants;
		bool complete{}, admitted{}, activation_complete{true};
		std::string reason;
	};
	/** Detached observations only; all compiler pointer correspondences expire in the job. */
	struct project_exceptional_exit_observations
	{
		std::vector<exceptional_physical_scope> scopes;
		bool hooks_installed{}, frozen{};
		std::size_t operations{}, retained_bytes{};
	};
	/** Lower independently admitted already parsed definitions with original compiler settings. */
	[[nodiscard]] sdk::result<project_exceptional_exit_observations>
	observe_project_exceptional_exits(provider::clang22::borrowed_translation_unit&,
									  exceptional_exit_limits = {},
									  exceptional_compiler_bindings = {});
} // namespace cxxlens::detail::clang22
