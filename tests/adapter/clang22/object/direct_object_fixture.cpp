#include <cstdlib>
#include <iostream>
#include <map>

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Tooling/Tooling.h>

#include "direct_object_semantics.hpp"

namespace n = cxxlens::detail::clang22::object_semantics;
void require(bool value, const char* message)
{
	if (!value)
	{
		std::cerr << message << '\n';
		std::exit(1);
	}
}
class visitor : public clang::RecursiveASTVisitor<visitor>
{
	clang::ASTContext& context_;
	std::string current_;

  public:
	std::map<std::string, n::direct_type_access> reads;
	std::map<std::string, n::direct_downcast> casts;
	explicit visitor(clang::ASTContext& context) : context_(context) {}
	bool TraverseVarDecl(clang::VarDecl* declaration)
	{
		const auto prior = current_;
		current_ = declaration->getNameAsString();
		const auto success = clang::RecursiveASTVisitor<visitor>::TraverseVarDecl(declaration);
		current_ = prior;
		return success;
	}
	bool VisitExpr(clang::Expr* expression)
	{
		if (current_.empty())
			return true;
		if (const auto* cast = llvm::dyn_cast<clang::CastExpr>(expression))
		{
			if (cast->getCastKind() == clang::CK_BaseToDerived)
				casts[current_] = n::observe_direct_downcast(*cast, context_);
			if (cast->getCastKind() == clang::CK_LValueToRValue)
			{
				const auto fact = n::observe_direct_type_access(*cast, context_);
				if (fact.object || !reads.contains(current_))
					reads[current_] = fact;
			}
		}
		return true;
	}
};
int main()
{
	auto ast = clang::tooling::buildASTFromCodeWithArgs(
		R"cpp(
 namespace std {enum class byte:unsigned char {};}
 struct Base {int value;}; struct Derived:Base {};
 void local(int* unknown, Base* unknown_base) {
   int object=0;
   int& alias=object;
   auto pun_bad=*reinterpret_cast<float*>(&object);
   auto pun_signed_char=*reinterpret_cast<signed char*>(&object);
   auto pun_unsigned=*reinterpret_cast<unsigned*>(&object);
   auto pun_char=*reinterpret_cast<char*>(&object);
   auto pun_byte=*reinterpret_cast<std::byte*>(&object);
   auto pun_unknown=*reinterpret_cast<float*>(unknown);
   auto pun_reference=*reinterpret_cast<float*>(&alias);
   auto formation=reinterpret_cast<float*>(&object);
   Base exact_base;
   auto downcast_bad=static_cast<Derived*>(&exact_base);
   auto downcast_unknown=static_cast<Derived*>(unknown_base);
 }
 )cpp",
		{"-std=c++23", "-nostdinc", "-nostdinc++"},
		"/project/objects.cpp");
	require(static_cast<bool>(ast), "actual object AST absent");
	visitor actual(ast->getASTContext());
	actual.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
	for (const auto* name : {"pun_bad", "pun_signed_char"})
		require(actual.reads.at(name).object &&
					actual.reads.at(name).permitted_for_declared_type == false,
				"actual incompatible declared-type read was lost");
	for (const auto* name : {"pun_unsigned", "pun_char", "pun_byte"})
		require(actual.reads.at(name).object &&
					actual.reads.at(name).permitted_for_declared_type == true,
				"permitted representation read acquired type-access violation");
	for (const auto* name : {"pun_unknown", "pun_reference"})
		require(!actual.reads.at(name).object && !actual.reads.at(name).permitted_for_declared_type,
				"pointer/reference pointee identity was invented");
	require(!actual.reads.contains("formation"), "cast formation acquired a memory read");
	require(actual.casts.at("downcast_bad").object &&
				actual.casts.at("downcast_bad").compatible_with_declared_type == false,
			"declared Base compatibility was confused with Derived");
	require(!actual.casts.at("downcast_unknown").object &&
				!actual.casts.at("downcast_unknown").compatible_with_declared_type,
			"unknown pointer declared pointee identity was inferred");
	std::cout << "actual direct object/type/downcast roles:10 observations PASS\n";
}
