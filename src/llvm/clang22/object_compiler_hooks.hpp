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
