#include "value_domain_semantics.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <llvm/ADT/SmallString.h>

namespace cxxlens::detail::clang22::object_semantics
{
	namespace
	{
		std::string decimal(const llvm::APInt& value, bool is_signed)
		{
			llvm::SmallString<64> encoded;
			value.toString(encoded, 10U, is_signed);
			return std::string(encoded);
		}
		std::optional<llvm::APSInt> original_integer(const clang::Expr& expression,
													 const clang::ASTContext& context,
													 std::size_t maximum_bits)
		{
			const auto type = expression.getType();
			if (!maximum_bits || expression.isValueDependent() || expression.isTypeDependent() ||
				type.isVolatileQualified() || !type->isIntegralOrEnumerationType() ||
				context.getIntWidth(type) > maximum_bits)
				return std::nullopt;
			clang::Expr::EvalResult result;
			if (!expression.EvaluateAsInt(result, context, clang::Expr::SE_NoSideEffects) ||
				result.HasSideEffects || result.HasUndefinedBehavior || !result.Val.isInt() ||
				result.Val.getInt().getBitWidth() > maximum_bits)
				return std::nullopt;
			return result.Val.getInt();
		}
	} // namespace

	enum_value observe_enum_value(const clang::CastExpr& cast,
								  const clang::ASTContext& context,
								  std::size_t maximum_integer_bits)
	{
		enum_value result;
		if (!context.getLangOpts().CPlusPlus || cast.isTypeDependent() ||
			cast.getCastKind() != clang::CK_IntegralCast || !cast.getType()->isEnumeralType())
			return result;
		const auto* declaration = cast.getType()->getAsEnumDecl();
		if (!declaration || declaration->isInvalidDecl())
			return result;
		result.fixed_underlying = declaration->isFixed();
		result.enumeration_binding = state::complete;
		if (declaration->isFixed())
			return result;
		declaration = declaration->getDefinition();
		if (!declaration)
		{
			result.enumeration_binding = state::unavailable;
			return result;
		}
		llvm::APInt upper, lower;
		declaration->getValueRange(upper, lower);
		if (!maximum_integer_bits || upper.getBitWidth() > maximum_integer_bits ||
			lower.getBitWidth() > maximum_integer_bits)
		{
			result.enumeration_binding = state::unavailable;
			return result;
		}
		--upper;
		const bool signed_domain = declaration->getNumNegativeBits() != 0U;
		result.compiler_value_domain =
			integer_range{decimal(lower, signed_domain), decimal(upper, signed_domain)};
		if (const auto value = original_integer(*cast.getSubExpr(), context, maximum_integer_bits))
		{
			const auto encoded = decimal(*value, value->isSigned());
			result.original_value = integer_range{encoded, encoded};
		}
		return result;
	}

	bool_representation observe_bool_representation(const clang::CastExpr& cast,
													const clang::ASTContext& context,
													std::size_t maximum_integer_bits)
	{
		bool_representation result;
		if (cast.isTypeDependent() || !cast.getType()->isBooleanType())
			return result;
		result.canonical_bool = true;
		if (cast.getCastKind() == clang::CK_IntegralToBoolean)
		{
			result.operation = bool_representation::kind::integral_conversion;
			return result;
		}
		if (!llvm::isa<clang::BuiltinBitCastExpr>(cast))
			return result;
		result.operation = bool_representation::kind::bit_cast;
		const auto source_type = cast.getSubExpr()->getType();
		if (source_type->isDependentType() || !source_type->isIntegerType() ||
			source_type->isBooleanType() || source_type.isVolatileQualified() ||
			context.getTypeSize(source_type) != context.getTypeSize(cast.getType()) ||
			context.getIntWidth(source_type) != context.getTypeSize(source_type) ||
			context.getIntWidth(cast.getType()) != 1U)
			return result;
		if (const auto value = original_integer(*cast.getSubExpr(), context, maximum_integer_bits))
		{
			// An integral bit_cast copies the observed unsigned object bit pattern;
			// it does not perform integral-to-bool value conversion.
			const auto encoded = decimal(*value, false);
			result.original_representation = integer_range{encoded, encoded};
			result.representation_binding = state::complete;
		}
		return result;
	}
} // namespace cxxlens::detail::clang22::object_semantics
