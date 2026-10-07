#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace clang
{
	class CFG;
	class Expr;
	class FunctionDecl;
	class VarDecl;
} // namespace clang

namespace cxxlens::detail::clang22::object_semantics
{
	/** Pointwise original lifetime proof for a directly named automatic record.
	 * Complete requires the full actual reference/action census and original CFG
	 * ordering. This does not infer a pointee, dynamic representation, an alias
	 * closure, or a function-wide absence of violations. */
	struct direct_lifetime_state
	{
		bool reference_census_complete{}, action_census_complete{}, cfg_point_complete{};
		std::optional<std::string> phase;
		bool actual_member_call{}, actual_polymorphic_use{};
		std::size_t original_references{}, original_destructions{};

		[[nodiscard]] bool complete() const noexcept
		{
			return reference_census_complete && action_census_complete && cfg_point_complete &&
				phase.has_value();
		}
	};
	[[nodiscard]] direct_lifetime_state
	observe_direct_lifetime_state(const clang::FunctionDecl&,
								  const clang::VarDecl&,
								  const clang::Expr& actual_use,
								  const clang::CFG&,
								  std::size_t maximum_work = 1'000'000U,
								  std::size_t maximum_nodes = 100'000U);
	/** Measured form of the same pointwise proof. operations is reset on entry
	 * and retains actual traversal work on every complete or partial return.
	 * The caller supplies its remaining work and settles this count once. */
	[[nodiscard]] direct_lifetime_state observe_direct_lifetime_state(const clang::FunctionDecl&,
																	  const clang::VarDecl&,
																	  const clang::Expr&,
																	  const clang::CFG&,
																	  std::size_t maximum_work,
																	  std::size_t maximum_nodes,
																	  std::size_t& operations);

} // namespace cxxlens::detail::clang22::object_semantics
