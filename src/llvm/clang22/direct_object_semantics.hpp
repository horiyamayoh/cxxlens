#pragma once

#include <optional>

#include <clang/AST/Type.h>

namespace clang
{
	class ASTContext;
	class CastExpr;
	class Expr;
	class VarDecl;
} // namespace clang

namespace cxxlens::detail::clang22::object_semantics
{
	/** Original declaration/type compatibility at an actual named-address use.
	 * A declaration's type does not prove point-of-use effective or dynamic type
	 * after lifetime end or storage reuse. These observations require a separate
	 * current-storage proof before becoming an object-access/downcast violation.
	 * This does not resolve pointer/reference values or infer a pointee. */
	struct direct_type_access
	{
		const clang::VarDecl* object{};
		const clang::Expr* actual_address{};
		clang::QualType declared_object_type, access_type;
		std::optional<bool> permitted_for_declared_type;
	};
	struct direct_downcast
	{
		const clang::VarDecl* object{};
		const clang::Expr* actual_address{};
		clang::QualType declared_object_type, target_type;
		std::optional<bool> compatible_with_declared_type;
	};
	[[nodiscard]] direct_type_access observe_direct_type_access(const clang::Expr& value_read,
																const clang::ASTContext&);
	[[nodiscard]] direct_downcast observe_direct_downcast(const clang::CastExpr&,
														  const clang::ASTContext&);
} // namespace cxxlens::detail::clang22::object_semantics
