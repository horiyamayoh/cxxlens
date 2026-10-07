#include "original_source_feature_references.hpp"

#include <clang/AST/Decl.h>
#include <clang/AST/ExprCXX.h>

namespace cxxlens::detail::clang22
{
	original_source_feature_references
	observe_original_source_feature_references(const original_source_feature_view& feature) noexcept
	{
		original_source_feature_references result;
		if (feature.declaration)
		{
			if (const auto* declaration = llvm::dyn_cast<clang::ValueDecl>(feature.declaration))
			{
				result.type = declaration->getType();
				result.type_role = source_feature_type_role::declared;
			}
		}
		if (const auto* expression = llvm::dyn_cast_or_null<clang::Expr>(feature.statement))
		{
			result.type = expression->getType();
			result.type_role = source_feature_type_role::expression;
			if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression))
			{
				result.referenced_declaration = reference->getDecl();
				result.reference_kind = source_feature_reference_kind::declaration_reference;
			}
			else if (const auto* member = llvm::dyn_cast<clang::MemberExpr>(expression))
			{
				result.referenced_declaration = member->getMemberDecl();
				result.reference_kind = source_feature_reference_kind::member_reference;
			}
			else if (const auto* construction = llvm::dyn_cast<clang::CXXConstructExpr>(expression))
			{
				result.selected_callable = construction->getConstructor();
				result.reference_kind = source_feature_reference_kind::selected_constructor;
			}
			else if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expression))
			{
				result.selected_callable = call->getDirectCallee();
				result.reference_kind = result.selected_callable
					? source_feature_reference_kind::direct_callee
					: source_feature_reference_kind::indirect_call;
			}
		}
		if (!feature.type_location.isNull())
		{
			result.type = feature.type_location.getType();
			result.type_role = source_feature_type_role::located;
		}
		return result;
	}
} // namespace cxxlens::detail::clang22
