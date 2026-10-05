#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Tooling/Tooling.h>

#include "llvm/clang22/original_builtin_kind.hpp"

using cxxlens::detail::clang22::observe_original_builtin_kind;
using cxxlens::detail::clang22::observe_original_integer_representation;

static void require(bool value, const char* reason)
{
	if (!value)
	{
		std::cerr << reason << '\n';
		std::exit(1);
	}
}

struct actual_builtin_observer : clang::RecursiveASTVisitor<actual_builtin_observer>
{
	const clang::ASTContext& context;
	std::map<std::string, cxxlens::detail::clang22::original_builtin_kind_observation> variables;
	std::map<std::string, cxxlens::detail::clang22::original_integer_representation>
		representations;
	unsigned actual_boolean_call{}, integral_boolean_conversions{}, actual_arithmetic{},
		actual_narrowing{};
	explicit actual_builtin_observer(const clang::ASTContext& value) : context(value) {}
	bool VisitVarDecl(clang::VarDecl* declaration)
	{
		variables.emplace(declaration->getNameAsString(),
						  observe_original_builtin_kind(declaration->getType()));
		representations.emplace(
			declaration->getNameAsString(),
			observe_original_integer_representation(context, declaration->getType()));
		return true;
	}
	bool VisitBinaryOperator(clang::BinaryOperator* expression)
	{
		if (expression->getOpcode() == clang::BO_Add)
		{
			const auto result =
				observe_original_integer_representation(context, expression->getType());
			require(result.state == "complete" && result.bit_width == 32U &&
						result.signed_value == true,
					"actual arithmetic result representation unavailable");
			++actual_arithmetic;
		}
		return true;
	}
	bool VisitCastExpr(clang::CastExpr* expression)
	{
		if (expression->getCastKind() == clang::CK_IntegralCast)
		{
			const auto result =
				observe_original_integer_representation(context, expression->getType());
			const auto source = observe_original_integer_representation(
				context, expression->getSubExpr()->getType());
			require(result.state == "complete" && result.bit_width == 8U &&
						result.signed_value == false && source.bit_width == 32U &&
						source.signed_value == true,
					"actual cast source/destination representations were conflated");
			++actual_narrowing;
		}
		return true;
	}
	bool VisitCallExpr(clang::CallExpr* expression)
	{
		const auto observation = observe_original_builtin_kind(expression->getType());
		actual_boolean_call += observation.kind == "Bool" && observation.state == "complete";
		return true;
	}
	bool VisitImplicitCastExpr(clang::ImplicitCastExpr* expression)
	{
		if (expression->getCastKind() == clang::CK_IntegralToBoolean)
		{
			const auto destination = observe_original_builtin_kind(expression->getType());
			const auto source = observe_original_builtin_kind(expression->getSubExpr()->getType());
			require(destination.kind == "Bool" && destination.state == "complete" &&
						source.kind == "Int",
					"actual conversion must retain Bool destination separately from original "
					"integer source");
			++integral_boolean_conversions;
		}
		return true;
	}
};

int main()
{
	auto ast = clang::tooling::buildASTFromCodeWithArgs(R"cpp(
		typedef bool UserAlias;
		bool actual_bool();
		bool plain_boolean;
		const UserAlias aliased_boolean = false;
		unsigned char one_byte_integer;
		int ordinary_integer;
		bool* pointer_to_boolean;
		_BitInt(9) bit_integer;
		unsigned _BitInt(9) unsigned_bit_integer;
		enum Fixed : unsigned short;
		Fixed fixed_enum;
		enum Complete : int { first = 0, second = 1 };
		Complete complete_enum;
		double floating_value;
		struct BoolLike { explicit operator bool() const; } record_value;
		void condition(int value) { if (actual_bool()) {} if (value) {} int sum = value + 1; unsigned char narrowed = (unsigned char)sum; }
	)cpp",
														{"-std=c++23", "-nostdinc", "-nostdinc++"},
														"/project/builtin.cpp");
	require(static_cast<bool>(ast), "actual compiler AST unavailable");
	actual_builtin_observer observer{ast->getASTContext()};
	observer.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
	for (const auto name : {"plain_boolean", "aliased_boolean"})
		require(observer.variables.at(name).kind == "Bool" &&
					observer.variables.at(name).state == "complete",
				"actual canonical Bool/qualified typedef classification lost");
	require(observer.variables.at("one_byte_integer").kind == "UChar" &&
				observer.variables.at("ordinary_integer").kind == "Int",
			"small integer must not be inferred as Bool from width");
	require(observer.variables.at("pointer_to_boolean").kind.empty() &&
				observer.variables.at("record_value").kind.empty(),
			"pointer/record or conversion operator must not classify as original builtin Bool");
	const auto& context = ast->getASTContext();
	require(observe_original_builtin_kind(context.DependentTy).state == "unknown",
			"dependent builtin must remain unknown");
	require(observe_original_builtin_kind(context.OverloadTy).state == "partial",
			"compiler placeholder must remain partial");
	require(observe_original_builtin_kind({}).state == "unavailable",
			"missing compiler type must remain unavailable");
	require(observe_original_builtin_kind(context.VoidTy).kind == "Void",
			"actual Void discriminator missing");
	require(observer.actual_boolean_call == 1U && observer.integral_boolean_conversions == 1U,
			"actual Bool call and integer boolean conversion were conflated");
	require(observer.representations.at("bit_integer").bit_width == 9U &&
				observer.representations.at("bit_integer").signed_value == true &&
				observer.representations.at("unsigned_bit_integer").bit_width == 9U &&
				observer.representations.at("unsigned_bit_integer").signed_value == false,
			"_BitInt integer precision must exclude object padding");
	require(observer.representations.at("fixed_enum").bit_width == 16U &&
				observer.representations.at("fixed_enum").signed_value == false &&
				!observer.representations.at("fixed_enum").underlying_type.isNull(),
			"actual fixed enum underlying representation missing");
	require(observer.representations.at("complete_enum").bit_width == 32U &&
				observer.representations.at("complete_enum").signed_value == true,
			"actual enum representation missing");
	for (const auto name : {"floating_value", "pointer_to_boolean", "record_value"})
		require(observer.representations.at(name).state == "not_applicable" &&
					!observer.representations.at(name).bit_width,
				"noninteger type acquired guessed integer geometry");
	require(
		observe_original_integer_representation(context, context.DependentTy).state == "unknown" &&
			observe_original_integer_representation(context, context.OverloadTy).state == "partial",
		"dependent/placeholder integer representation frontier lost");
	require(observer.actual_arithmetic == 1U && observer.actual_narrowing == 1U,
			"actual expression/cast representation oracle incomplete");
	std::cout << "original builtin kind and integer representation actual compiler oracle PASS\n";
}
