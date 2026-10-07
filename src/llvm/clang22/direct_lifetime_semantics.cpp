#include "direct_lifetime_semantics.hpp"

#include <set>
#include <vector>

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <clang/Analysis/CFG.h>

namespace cxxlens::detail::clang22::object_semantics
{
	namespace
	{
		const clang::DeclRefExpr* direct_receiver(const clang::CXXMemberCallExpr& call)
		{
			const auto* object = call.getImplicitObjectArgument();
			return object ? llvm::dyn_cast<clang::DeclRefExpr>(object->IgnoreParenImpCasts())
						  : nullptr;
		}
	} // namespace
	direct_lifetime_state observe_direct_lifetime_state(const clang::FunctionDecl& owner,
														const clang::VarDecl& object,
														const clang::Expr& actual_use,
														const clang::CFG& graph,
														std::size_t maximum_work,
														std::size_t maximum_nodes)
	{
		std::size_t operations{};
		return observe_direct_lifetime_state(
			owner, object, actual_use, graph, maximum_work, maximum_nodes, operations);
	}

	direct_lifetime_state observe_direct_lifetime_state(const clang::FunctionDecl& owner,
														const clang::VarDecl& object,
														const clang::Expr& actual_use,
														const clang::CFG& graph,
														std::size_t maximum_work,
														std::size_t maximum_nodes,
														std::size_t& operations)
	{
		operations = 0U;
		auto& work = operations;
		direct_lifetime_state result;
		if (!owner.getASTContext().getLangOpts().CPlusPlus || !maximum_work || !maximum_nodes ||
			maximum_work > 8'000'000U || maximum_nodes > 100'000U || graph.size() > maximum_nodes ||
			owner.isInvalidDecl() || !owner.doesThisDeclarationHaveABody() ||
			owner.isDependentContext() || object.isInvalidDecl() ||
			object.getDeclContext() != &owner || !object.hasLocalStorage() ||
			object.isStaticLocal() || !object.hasInit() || object.getType()->isReferenceType() ||
			object.getType()->isDependentType() || object.getType()->isIncompleteType() ||
			object.getType().isVolatileQualified())
			return result;
		const auto* use = llvm::dyn_cast<clang::CXXMemberCallExpr>(&actual_use);
		const auto* method = use ? use->getMethodDecl() : nullptr;
		const auto* use_ref = use ? direct_receiver(*use) : nullptr;
		if (!method || llvm::isa<clang::CXXDestructorDecl>(method) || method->isStatic() ||
			!use_ref || use_ref->getDecl() != &object)
			return result;
		result.actual_member_call = true;
		result.actual_polymorphic_use = method->isVirtual();

		const auto* record = object.getType()->getAsCXXRecordDecl();
		const auto* construction =
			llvm::dyn_cast<clang::CXXConstructExpr>(object.getInit()->IgnoreParenImpCasts());
		if (!record || !record->hasDefinition() || record->hasAnyDependentBases() ||
			record->getNumBases() != 0U || !construction || construction->getNumArgs() != 0U ||
			!construction->getConstructor() || !construction->getConstructor()->isImplicit() ||
			!construction->getConstructor()->isDefaulted())
			return result;
		// A compiler-generated empty-record initializer cannot expose this object
		// through user field/base constructors. General initialization stays unknown.
		if (!record->field_empty())
			return result;

		const auto tick = [&]()
		{
			if (work == maximum_work)
				return false;
			++work;
			return true;
		};
		std::vector<const clang::Stmt*> pending{owner.getBody()};
		std::set<const clang::Stmt*> destructions;
		std::set<const clang::DeclRefExpr*> permitted_references{use_ref};
		bool found_declaration{}, found_use{};
		while (!pending.empty())
		{
			if (!tick())
				return result;
			const auto* statement = pending.back();
			pending.pop_back();
			found_use |= statement == use;
			if (const auto* declarations = llvm::dyn_cast<clang::DeclStmt>(statement))
				for (const auto* declaration : declarations->decls())
				{
					if (!tick())
						return result;
					found_declaration |= declaration == &object;
				}
			if (llvm::isa<clang::CXXNewExpr,
						  clang::CXXDeleteExpr,
						  clang::LambdaExpr,
						  clang::BlockExpr,
						  clang::GCCAsmStmt,
						  clang::MSAsmStmt,
						  clang::CoroutineBodyStmt,
						  clang::RecoveryExpr,
						  clang::IndirectGotoStmt>(statement))
				return result;
			if (const auto* call = llvm::dyn_cast<clang::CallExpr>(statement); call && call != use)
			{
				const auto* member = llvm::dyn_cast<clang::CXXMemberCallExpr>(call);
				const auto* selected = member ? member->getMethodDecl() : nullptr;
				const auto* reference = member ? direct_receiver(*member) : nullptr;
				if (!selected || !llvm::isa<clang::CXXDestructorDecl>(selected) ||
					selected->isNoReturn() || !reference || reference->getDecl() != &object)
					return result;
				if (destructions.size() == maximum_nodes ||
					permitted_references.size() == maximum_nodes)
					return result;
				destructions.insert(call);
				permitted_references.insert(reference);
			}
			if (const auto* ctor = llvm::dyn_cast<clang::CXXConstructExpr>(statement);
				ctor && ctor != construction)
				return result;
			if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(statement);
				reference && reference->getDecl() == &object)
				++result.original_references;
			for (const auto* child : statement->children())
			{
				if (!tick())
					return result;
				if (!child)
					continue;
				if (pending.size() == maximum_nodes)
					return result;
				pending.push_back(child);
			}
		}
		if (!found_declaration || !found_use ||
			result.original_references != permitted_references.size())
			return result;
		result.reference_census_complete = result.action_census_complete = true;
		result.original_destructions = destructions.size();

		enum class phase
		{
			not_started,
			live,
			ended
		};
		using path_state = std::pair<const clang::CFGBlock*, phase>;
		std::vector<path_state> paths{{&graph.getEntry(), phase::not_started}};
		std::set<path_state> visited;
		std::set<phase> point_states;
		bool cfg_declaration{};
		const auto occurrence = [&](const clang::Stmt* root, bool& at_use, bool& at_destruction)
		{
			std::vector<const clang::Stmt*> nodes{root};
			while (!nodes.empty())
			{
				if (!tick())
					return false;
				const auto* current = nodes.back();
				nodes.pop_back();
				if (!current)
					continue;
				at_use |= current == use;
				at_destruction |= destructions.contains(current);
				for (const auto* child : current->children())
				{
					if (!tick())
						return false;
					if (!child)
						continue;
					if (nodes.size() == maximum_nodes)
						return false;
					nodes.push_back(child);
				}
			}
			return true;
		};
		while (!paths.empty())
		{
			if (!tick())
				return result;
			const auto current = paths.back();
			paths.pop_back();
			if (visited.contains(current))
				continue;
			if (visited.size() == maximum_nodes)
				return result;
			visited.insert(current);
			auto state = current.second;
			for (const auto& element : *current.first)
			{
				if (!tick())
					return result;
				if (const auto original = element.getAs<clang::CFGStmt>())
				{
					const auto* statement = original->getStmt();
					bool at_use{}, at_destruction{};
					if (!occurrence(statement, at_use, at_destruction))
						return result;
					// A compound expression containing both points needs its actual
					// sequencing decomposition; parent containment is not an order proof.
					if (at_use && at_destruction)
						return result;
					if (at_use)
						point_states.insert(state);
					if (at_destruction)
						state = phase::ended;
					if (const auto* declarations = llvm::dyn_cast<clang::DeclStmt>(statement))
						for (const auto* declaration : declarations->decls())
						{
							if (!tick())
								return result;
							if (declaration == &object)
							{
								state = phase::live;
								cfg_declaration = true;
							}
						}
				}
				else if (const auto cleanup = element.getAs<clang::CFGAutomaticObjDtor>();
						 cleanup && cleanup->getVarDecl() == &object)
					state = phase::ended;
			}
			for (const auto& edge : current.first->succs())
			{
				if (!tick())
					return result;
				const auto* next = edge.getReachableBlock();
				if (!next)
					continue;
				if (paths.size() == maximum_nodes)
					return result;
				paths.push_back({next, state});
			}
		}
		result.cfg_point_complete = cfg_declaration && !point_states.empty();
		if (result.cfg_point_complete && point_states.size() == 1U)
			result.phase = *point_states.begin() == phase::live ? "live"
				: *point_states.begin() == phase::ended			? "ended"
																: "not_started";
		return result;
	}
} // namespace cxxlens::detail::clang22::object_semantics
