#include "function_exit_semantics.hpp"

#include <set>

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/StmtCXX.h>
#include <clang/Analysis/CFG.h>
namespace cxxlens::detail::clang22::object_semantics
{
	exit_projection observe_function_exits(const clang::FunctionDecl& function,
										   const clang::CFG& graph,
										   std::size_t maximum_blocks,
										   std::size_t maximum_work)
	{
		exit_projection result;
		if (!function.getASTContext().getLangOpts().CPlusPlus || !maximum_blocks || !maximum_work ||
			graph.size() > maximum_blocks || function.isInvalidDecl() ||
			!function.doesThisDeclarationHaveABody())
			return result;
		using R = missing_return::return_kind;
		using E = missing_return::exit_kind;
		R returns = R::unknown;
		if (llvm::isa<clang::CXXConstructorDecl>(function))
			returns = R::constructor;
		else if (llvm::isa<clang::CXXDestructorDecl>(function))
			returns = R::destructor;
		else if (function.getReturnType()->isDependentType())
			returns = R::unknown;
		else if (function.getReturnType()->isVoidType())
			returns = R::void_value;
		else if (function.getReturnType()->isReferenceType())
			returns = R::reference;
		else
			returns = R::value;
		const auto* coroutine = llvm::dyn_cast<clang::CoroutineBodyStmt>(function.getBody());
		std::set<const clang::CFGBlock*> reached;
		std::vector<const clang::CFGBlock*> pending{&graph.getEntry()};
		std::size_t work{};
		auto tick = [&]()
		{
			return work++ < maximum_work;
		};
		while (!pending.empty())
		{
			if (!tick())
				return result;
			const auto* block = pending.back();
			pending.pop_back();
			if (reached.contains(block))
				continue;
			if (reached.size() >= maximum_blocks)
				return result;
			reached.insert(block);
			for (const auto& successor : block->succs())
			{
				if (!tick())
					return result;
				const auto* target = successor.getReachableBlock();
				if (target && !reached.contains(target))
				{
					if (pending.size() >= maximum_blocks)
						return result;
					pending.push_back(target);
				}
			}
		}
		result.enumeration_complete = true;
		for (const auto& predecessor : graph.getExit().preds())
		{
			if (!tick())
			{
				result.enumeration_complete = false;
				return result;
			}
			const auto* block = predecessor.getReachableBlock();
			if (!block || !reached.contains(block))
				continue;
			if (result.members.size() >= maximum_blocks)
			{
				result.enumeration_complete = false;
				return result;
			}
			exit_member member;
			member.predecessor = block->getBlockID();
			member.exit = graph.getExit().getBlockID();
			auto& fact = member.facts;
			fact.returns = returns;
			fact.is_main = function.isMain();
			fact.is_coroutine = coroutine != nullptr;
			if (coroutine)
				fact.coroutine_has_return_void = coroutine->getFallthroughHandler() != nullptr;
			fact.exit_binding = state::complete;
			if (block->hasNoReturnElement())
				fact.exits = E::no_return;
			else if (const auto* terminator = block->getTerminatorStmt();
					 llvm::isa_and_nonnull<clang::CXXThrowExpr>(terminator))
				fact.exits = E::exceptional;
			else
			{
				const clang::Stmt* last{};
				bool cleanup = false;
				for (const auto& element : *block)
				{
					if (!tick())
					{
						result.enumeration_complete = false;
						return result;
					}
					if (const auto stmt = element.getAs<clang::CFGStmt>())
						last = stmt->getStmt();
					else
						cleanup = true;
				}
				if (llvm::isa_and_nonnull<clang::ReturnStmt>(last))
					fact.exits = E::return_value;
				else if (llvm::isa_and_nonnull<clang::CXXThrowExpr>(last))
					fact.exits = E::exceptional;
				else if (cleanup || coroutine || returns == R::unknown)
				{
					fact.exits = E::unknown;
					fact.exit_binding = state::unavailable;
				}
				else
					fact.exits = E::fallthrough;
			}
			result.members.push_back(std::move(member));
		}
		return result;
	}
} // namespace cxxlens::detail::clang22::object_semantics
