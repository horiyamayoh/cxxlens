#pragma once

#include <cstdint>

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>

namespace clang
{
	class Sema;
} // namespace clang
namespace cxxlens_object_semantics_hook
{
	enum class atomic_operation_kind
	{
		unknown,
		init,
		load,
		store,
		rmw,
		exchange,
		compare_exchange,
		test_and_set,
		clear
	};
	enum class atomic_order_class
	{
		unknown,
		relaxed,
		consume,
		acquire,
		release,
		acq_rel,
		seq_cst
	};
	enum class atomic_validation
	{
		unknown,
		valid,
		invalid,
		not_applicable
	};
	struct atomic_order_view
	{
		atomic_order_class kind{atomic_order_class::unknown};
		atomic_validation validation{atomic_validation::unknown};
	};
	/** Compact results from the actual BuildAtomicExpr ICE/order predicates.
	 * Extraction never re-evaluates the expression or interprets a raw integer. */
	struct atomic_expression_view
	{
		atomic_operation_kind operation{atomic_operation_kind::unknown};
		atomic_order_view success, failure;
		bool scoped{};
	};
	[[nodiscard]] std::uint32_t atomic_routes() noexcept;
	void atomic_expression(clang::Sema&, const clang::AtomicExpr*, const atomic_expression_view&);
	/** Functional routes exported by the actual linked original SemaChecking TU:
	 * root lifetime, access identity, argument context, and final access pair. */
	[[nodiscard]] std::uint32_t sequence_routes() noexcept;
	/** Called only inside the original parser/Sema job. Implementations detach
	 * original compiler identity/source/axis values immediately; no pointer is
	 * stored outside this parser job. Stock/DSO compilers expose no hook census. */
	void sequence_root(clang::Sema&, const clang::Expr*, bool begin);
	/** Original checker access and argument traversal. The final diagnostic pair
	 * alone is insufficient: stock22 warns across C++17 indeterminately sequenced
	 * ordinary arguments as well. Preserve the exact argument occurrence, language
	 * classification and access ownership before interpreting that pair. */
	std::uint64_t sequence_access(clang::Sema&, const clang::Expr*);
	void sequence_argument(clang::Sema&,
						   const clang::Expr* invocation,
						   const clang::Expr* argument,
						   unsigned index,
						   bool indeterminately_sequenced,
						   bool begin);
	void unsequenced(clang::Sema&,
					 const clang::NamedDecl*,
					 const clang::Expr* modification,
					 const clang::Expr* other,
					 bool other_modifies,
					 std::uint64_t modification_context,
					 std::uint64_t other_context);
} // namespace cxxlens_object_semantics_hook
