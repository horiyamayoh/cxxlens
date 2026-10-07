#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Analysis/CFG.h>
#include <clang/Tooling/Tooling.h>

#include "direct_object_semantics.hpp"
#include "owned_object_state.hpp"

namespace n = cxxlens::detail::clang22::object_semantics;
void require(bool value, const char* reason)
{
	if (!value)
	{
		std::cerr << reason << '\n';
		std::exit(1);
	}
}
class visitor : public clang::RecursiveASTVisitor<visitor>
{
	clang::ASTContext& context_;
	const clang::FunctionDecl* owner_{};
	std::unique_ptr<clang::CFG> cfg_;

  public:
	std::map<std::string, n::owned_object_state> states;
	std::map<std::string, bool> compatibility;
	explicit visitor(clang::ASTContext& context) : context_(context) {}
	bool TraverseFunctionDecl(clang::FunctionDecl* owner)
	{
		const auto* prior = owner_;
		owner_ = owner;
		auto prior_cfg = std::move(cfg_);
		if (owner->doesThisDeclarationHaveABody())
		{
			clang::CFG::BuildOptions options;
			options.setAllAlwaysAdd();
			cfg_ = clang::CFG::buildCFG(owner, owner->getBody(), &context_, options);
			require(static_cast<bool>(cfg_), "original owned-object CFG absent");
		}
		const auto ok = clang::RecursiveASTVisitor<visitor>::TraverseFunctionDecl(owner);
		owner_ = prior;
		cfg_ = std::move(prior_cfg);
		return ok;
	}
	bool VisitCastExpr(clang::CastExpr* expression)
	{
		if (!owner_ || !owner_->hasBody() || !cfg_)
			return true;
		const auto retain = [&](const auto& fact)
		{
			if (!fact.object || !fact.actual_address)
				return;
			const auto name = owner_->getNameAsString();
			states[name] =
				n::observe_owned_object_state(*owner_, *fact.object, *fact.actual_address, *cfg_);
			std::size_t operations = 999U;
			const auto measured = n::observe_owned_object_state(
				*owner_, *fact.object, *fact.actual_address, *cfg_, 100'000U, 100'000U, operations);
			require(measured.live_unreplaced() == states[name].live_unreplaced() &&
						measured.original_reference_occurrences ==
							states[name].original_reference_occurrences &&
						operations <= 100'000U,
					"measured traversal changed the original pointwise proof");
			if (measured.live_unreplaced())
			{
				require(operations > 1U, "nonempty original traversal reported no work");
				const auto required = operations;
				require(n::observe_owned_object_state(*owner_,
													  *fact.object,
													  *fact.actual_address,
													  *cfg_,
													  required,
													  100'000U,
													  operations)
								.live_unreplaced() &&
							operations == required,
						"exact measured work does not admit the original proof");
				require(!n::observe_owned_object_state(*owner_,
													   *fact.object,
													   *fact.actual_address,
													   *cfg_,
													   required - 1U,
													   100'000U,
													   operations)
								.live_unreplaced() &&
							operations == required - 1U,
						"incomplete measured traversal acquired a pointwise proof");
			}
			operations = 999U;
			require(
				!n::observe_owned_object_state(
					 *owner_, *fact.object, *fact.actual_address, *cfg_, 0U, 100'000U, operations)
						.live_unreplaced() &&
					operations == 0U,
				"unstarted traversal retained a stale work counter");
			require(!n::observe_owned_object_state(
						 *owner_, *fact.object, *fact.actual_address, *cfg_, 1U)
						 .live_unreplaced(),
					"exhausted original body census acquired current storage");
		};
		if (expression->getCastKind() == clang::CK_LValueToRValue)
		{
			const auto fact = n::observe_direct_type_access(*expression, context_);
			if (fact.object)
			{
				retain(fact);
				compatibility[owner_->getNameAsString()] =
					fact.permitted_for_declared_type.value_or(true);
			}
		}
		else if (expression->getCastKind() == clang::CK_BaseToDerived)
		{
			const auto fact = n::observe_direct_downcast(*expression, context_);
			if (fact.object)
			{
				retain(fact);
				compatibility[owner_->getNameAsString()] =
					fact.compatible_with_declared_type.value_or(true);
			}
		}
		return true;
	}
};
int main()
{
	auto ast = clang::tooling::buildASTFromCodeWithArgs(R"cpp(
  void opaque();
  struct Base{int value;}; struct Derived:Base{};
  float known_pun(){int value=0;return *reinterpret_cast<float*>(&value);}
  Derived* known_cast(){Base value;return static_cast<Derived*>(&value);}
  float exposed(){int value=0;auto address=&value;return *reinterpret_cast<float*>(&value);}
  float unknown_action(){int value=0;opaque();return *reinterpret_cast<float*>(&value);}
  float uninitialized(){int value;return *reinterpret_cast<float*>(&value);}
  float ended(){int value=0;using Scalar=int;value.~Scalar();return *reinterpret_cast<float*>(&value);}
  float captured(){int value=0;auto closure=[&value]{return value;};return *reinterpret_cast<float*>(&value);}
  float unknown_pointer(int* p){return *reinterpret_cast<float*>(p);}
  void during_initialization(){int value=*reinterpret_cast<float*>(&value);}
  )cpp",
														{"-std=c++23", "-nostdinc", "-nostdinc++"},
														"/project/storage.cpp");
	require(static_cast<bool>(ast), "original storage AST absent");
	visitor actual(ast->getASTContext());
	actual.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
	for (const auto* name : {"known_pun", "known_cast"})
		require(actual.states.contains(name) && actual.states.at(name).live_unreplaced() &&
					!actual.compatibility.at(name),
				"known automatic current storage proof absent");
	for (const auto* name : {"exposed",
							 "unknown_action",
							 "uninitialized",
							 "ended",
							 "captured",
							 "during_initialization"})
		require(actual.states.contains(name) && !actual.states.at(name).live_unreplaced(),
				"declaration type alone acquired current storage proof");
	require(!actual.states.contains("unknown_pointer"), "unknown pointer acquired owned storage");
	require(actual.states.at("during_initialization").reference_census_complete &&
				actual.states.at("during_initialization").single_address_use &&
				actual.states.at("during_initialization").original_cfg_complete &&
				!actual.states.at("during_initialization").initialized_before_address,
			"actual initializer access did not retain independent CFG point order");
	std::cout << "original current-storage census:9 observations PASS\n";
}
