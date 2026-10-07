#pragma once

#include <cstddef>

namespace clang
{
	class Expr;
	class FunctionDecl;
	class VarDecl;
	class CFG;
} // namespace clang

namespace cxxlens::detail::clang22::object_semantics
{
	/** A deliberately narrow actual AST proof of an initialized automatic object's
	 * unreplaced lifetime at one builtin address use. Each condition is independent
	 * of declared-type compatibility. The complete body is traversed; returned
	 * declaration/reference rows do not define this census. This never resolves
	 * pointer aliases or uses byte interval containment as ownership. */
	struct owned_object_state
	{
		bool reference_census_complete{};
		std::size_t original_reference_occurrences{};
		bool initialized_automatic{}, trivial_initialization{}, single_address_use{};
		bool no_opaque_action{};
		bool original_cfg_complete{}, initialized_before_address{};

		[[nodiscard]] bool live_unreplaced() const noexcept
		{
			return reference_census_complete && original_reference_occurrences == 1U &&
				initialized_automatic && trivial_initialization && single_address_use &&
				no_opaque_action && original_cfg_complete && initialized_before_address;
		}
	};
	[[nodiscard]] owned_object_state
	observe_owned_object_state(const clang::FunctionDecl&,
							   const clang::VarDecl&,
							   const clang::Expr& actual_address,
							   const clang::CFG& original_cfg,
							   std::size_t maximum_work = 100'000U,
							   std::size_t maximum_pending_nodes = 100'000U);
	/** Measured form of the same pointwise proof. operations is reset on entry
	 * and retains actual traversal work on every complete or partial return.
	 * The caller supplies its remaining work and settles this count once. */
	[[nodiscard]] owned_object_state observe_owned_object_state(const clang::FunctionDecl&,
																const clang::VarDecl&,
																const clang::Expr&,
																const clang::CFG&,
																std::size_t maximum_work,
																std::size_t maximum_pending_nodes,
																std::size_t& operations);

} // namespace cxxlens::detail::clang22::object_semantics
