#include "direct_object_semantics.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/ExprCXX.h>

namespace cxxlens::detail::clang22::object_semantics
{
	namespace
	{
		const clang::VarDecl* addressed_object(const clang::Expr& expression,
											   const clang::Expr*& actual_address)
		{
			const auto* current = expression.IgnoreParens();
			while (const auto* transparent = llvm::dyn_cast<clang::ImplicitCastExpr>(current))
			{
				if (transparent->getCastKind() != clang::CK_NoOp)
					return nullptr;
				current = transparent->getSubExpr()->IgnoreParens();
			}
			const auto* address = llvm::dyn_cast<clang::UnaryOperator>(current);
			if (!address || address->getOpcode() != clang::UO_AddrOf)
				return nullptr;
			const auto* reference =
				llvm::dyn_cast<clang::DeclRefExpr>(address->getSubExpr()->IgnoreParenImpCasts());
			const auto* object =
				reference ? llvm::dyn_cast<clang::VarDecl>(reference->getDecl()) : nullptr;
			if (!object || object->isInvalidDecl() || object->getType()->isReferenceType() ||
				object->getType()->isDependentType() || object->getType()->isIncompleteType())
				return nullptr;
			actual_address = address;
			return object;
		}
		bool character_inspection(clang::QualType type)
		{
			if (const auto* builtin = type->getAs<clang::BuiltinType>())
				return builtin->getKind() == clang::BuiltinType::Char_S ||
					builtin->getKind() == clang::BuiltinType::Char_U ||
					builtin->getKind() == clang::BuiltinType::UChar;
			const auto* enumeration = type->getAs<clang::EnumType>();
			return enumeration && enumeration->isStdByteType();
		}
	} // namespace

	direct_type_access observe_direct_type_access(const clang::Expr& value_read,
												  const clang::ASTContext& context)
	{
		direct_type_access result;
		if (!context.getLangOpts().CPlusPlus)
			return result;
		const auto* load = llvm::dyn_cast<clang::ImplicitCastExpr>(&value_read);
		if (!load || load->getCastKind() != clang::CK_LValueToRValue)
			return result;
		const auto* dereference =
			llvm::dyn_cast<clang::UnaryOperator>(load->getSubExpr()->IgnoreParenImpCasts());
		if (!dereference || dereference->getOpcode() != clang::UO_Deref)
			return result;
		const auto* cast =
			llvm::dyn_cast<clang::CastExpr>(dereference->getSubExpr()->IgnoreParens());
		if (!cast || cast->getCastKind() != clang::CK_BitCast || !cast->getType()->isPointerType())
			return result;
		result.object = addressed_object(*cast->getSubExpr(), result.actual_address);
		if (!result.object)
			return result;
		result.declared_object_type = result.object->getType().getCanonicalType();
		result.access_type = load->getType().getCanonicalType();
		if (context.hasSameUnqualifiedType(result.declared_object_type, result.access_type) ||
			character_inspection(result.access_type))
		{
			result.permitted_for_declared_type = true;
			return result;
		}
		if (result.declared_object_type->isIntegerType() &&
			!result.declared_object_type->isBooleanType() && result.access_type->isIntegerType() &&
			!result.access_type->isBooleanType())
		{
			const auto equivalent = result.declared_object_type->isSignedIntegerType()
				? context.getCorrespondingUnsignedType(result.declared_object_type)
				: context.getCorrespondingSignedType(result.declared_object_type);
			if (context.hasSameUnqualifiedType(equivalent, result.access_type))
			{
				result.permitted_for_declared_type = true;
				return result;
			}
		}
		// This narrow proof covers only complete arithmetic objects and actual
		// arithmetic value reads. Aggregate-subobject and pointer paths stay unknown.
		if (result.declared_object_type->isArithmeticType() &&
			result.access_type->isArithmeticType())
			result.permitted_for_declared_type = false;
		return result;
	}

	direct_downcast observe_direct_downcast(const clang::CastExpr& cast,
											const clang::ASTContext& context)
	{
		direct_downcast result;
		if (!context.getLangOpts().CPlusPlus || cast.getCastKind() != clang::CK_BaseToDerived ||
			cast.isTypeDependent() || !cast.getType()->isPointerType())
			return result;
		result.object = addressed_object(*cast.getSubExpr(), result.actual_address);
		if (!result.object)
			return result;
		result.declared_object_type = result.object->getType().getCanonicalType();
		result.target_type = cast.getType()->getPointeeType().getCanonicalType();
		const auto* actual = result.declared_object_type->getAsCXXRecordDecl();
		const auto* target = result.target_type->getAsCXXRecordDecl();
		if (!actual || !target || !actual->hasDefinition() || !target->hasDefinition() ||
			actual->hasAnyDependentBases() || target->hasAnyDependentBases())
			return result;
		actual = actual->getDefinition();
		target = target->getDefinition();
		result.compatible_with_declared_type =
			actual->getCanonicalDecl() == target->getCanonicalDecl() ||
			actual->isDerivedFrom(target);
		return result;
	}
} // namespace cxxlens::detail::clang22::object_semantics
