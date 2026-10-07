#include <cstdlib>
#include <iostream>
#include <map>

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Analysis/CFG.h>
#include <clang/Tooling/Tooling.h>

#include "direct_lifetime_semantics.hpp"
namespace n = cxxlens::detail::clang22::object_semantics;
namespace
{
	void require(bool v, const char* message)
	{
		if (!v)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}
	class visitor : public clang::RecursiveASTVisitor<visitor>
	{
		clang::ASTContext& context;
		clang::FunctionDecl* owner{};
		clang::VarDecl* object{};
		clang::CXXMemberCallExpr* point{};

	  public:
		std::map<std::string, n::direct_lifetime_state> facts;
		explicit visitor(clang::ASTContext& c) : context(c) {}
		bool TraverseFunctionDecl(clang::FunctionDecl* function)
		{
			if (!function || !function->doesThisDeclarationHaveABody() ||
				function->isCXXClassMember())
				return true;
			owner = function;
			object = nullptr;
			point = nullptr;
			const auto result = clang::RecursiveASTVisitor<visitor>::TraverseFunctionDecl(function);
			if (object && point)
			{
				clang::CFG::BuildOptions options;
				options.AddImplicitDtors = true;
				options.AddCXXDefaultInitExprInCtors = true;
				auto cfg = clang::CFG::buildCFG(function, function->getBody(), &context, options);
				require(static_cast<bool>(cfg), "actual CFG missing");
				facts[function->getNameAsString()] =
					n::observe_direct_lifetime_state(*function, *object, *point, *cfg);
				std::size_t operations = 999U;
				const auto measured = n::observe_direct_lifetime_state(
					*function, *object, *point, *cfg, 1'000'000U, 100'000U, operations);
				const auto& original = facts.at(function->getNameAsString());
				require(measured.complete() == original.complete() &&
							measured.phase == original.phase && operations <= 1'000'000U,
						"measured lifetime traversal changed the original point state");
				if (measured.complete())
				{
					require(operations > 1U, "original lifetime traversal reported no work");
					const auto required = operations;
					require(n::observe_direct_lifetime_state(
								*function, *object, *point, *cfg, required, 100'000U, operations)
									.complete() &&
								operations == required,
							"exact measured lifetime work does not admit the original point");
					require(
						!n::observe_direct_lifetime_state(
							 *function, *object, *point, *cfg, required - 1U, 100'000U, operations)
								.complete() &&
							operations == required - 1U,
						"incomplete measured lifetime traversal acquired a point state");
				}
				operations = 999U;
				require(!n::observe_direct_lifetime_state(
							 *function, *object, *point, *cfg, 0U, 100'000U, operations)
								.complete() &&
							operations == 0U,
						"unstarted lifetime traversal retained a stale work counter");
				const auto bounded =
					n::observe_direct_lifetime_state(*function, *object, *point, *cfg, 1);
				require(!bounded.complete(), "limited CFG census manufactured lifetime truth");
			}
			owner = nullptr;
			return result;
		}
		bool VisitVarDecl(clang::VarDecl* declaration)
		{
			if (owner && declaration->getDeclContext() == owner &&
				declaration->getName() == "value")
				object = declaration;
			return true;
		}
		bool VisitCXXMemberCallExpr(clang::CXXMemberCallExpr* call)
		{
			if (owner && call->getMethodDecl() &&
				call->getMethodDecl()->getNameAsString() == "read")
				point = call;
			return true;
		}
	};
} // namespace
int main()
{
	auto ast = clang::tooling::buildASTFromCodeWithArgs(R"cpp(
  struct Object { virtual int read(){return 1;} virtual ~Object(){} };
  void external();
  void expose(Object*);
  void ended(){Object value;value.~Object();value.read();}
  void live(){Object value;value.read();}
  void uncertain(bool branch){Object value;if(branch)value.~Object();value.read();}
  void opaque(){Object value;external();value.~Object();value.read();}
  void address(){Object value;expose(&value);value.~Object();value.read();}
  void combined(){Object value;(value.~Object(),value.read());}
  void dead(){Object value;value.~Object();if(false)value.read();}
  struct Own { Own(){external();} virtual int read(){return 1;} ~Own(){} };
  void custom(){Own value;value.~Own();value.read();}
  struct Nonvirtual { int read(){return 1;} ~Nonvirtual(){} };
  void nonvirtual(){Nonvirtual value;value.~Nonvirtual();value.read();}
  )cpp",
														{"-std=c++23", "-nostdinc", "-nostdinc++"},
														"/project/lifetime.cpp");
	require(static_cast<bool>(ast), "actual lifetime AST missing");
	visitor v(ast->getASTContext());
	v.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
	for (const auto& [name, f] : v.facts)
		std::cerr << name << " complete=" << f.complete()
				  << " phase=" << f.phase.value_or("unknown") << " refs=" << f.original_references
				  << " dtors=" << f.original_destructions << '\n';
	require(v.facts.at("ended").complete() && v.facts.at("ended").phase == "ended" &&
				v.facts.at("ended").actual_polymorphic_use,
			"actual ended virtual use absent");
	require(v.facts.at("live").complete() && v.facts.at("live").phase == "live",
			"actual live object became ended");
	require(v.facts.at("nonvirtual").complete() && v.facts.at("nonvirtual").phase == "ended" &&
				!v.facts.at("nonvirtual").actual_polymorphic_use,
			"ordinary ended member use became vptr proof");
	for (auto name : {"uncertain", "opaque", "address", "combined", "dead", "custom"})
		require(!v.facts.at(name).complete(), "unsupported lifetime domain falsely complete");
	std::cout << "9 original direct lifetime cases PASS\n";
}
