#include "owned_object_state.hpp"

#include <set>
#include <vector>

#include <clang/AST/DeclCXX.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <clang/Analysis/CFG.h>

namespace cxxlens::detail::clang22::object_semantics
{
	owned_object_state observe_owned_object_state(const clang::FunctionDecl& owner,
												  const clang::VarDecl& object,
												  const clang::Expr& actual_address,
												  const clang::CFG& original_cfg,
												  std::size_t maximum_work,
												  std::size_t maximum_pending_nodes)
	{
		std::size_t operations{};
		return observe_owned_object_state(owner,
										  object,
										  actual_address,
										  original_cfg,
										  maximum_work,
										  maximum_pending_nodes,
										  operations);
	}

	owned_object_state observe_owned_object_state(const clang::FunctionDecl& owner,
												  const clang::VarDecl& object,
												  const clang::Expr& actual_address,
												  const clang::CFG& original_cfg,
												  std::size_t maximum_work,
												  std::size_t maximum_pending_nodes,
												  std::size_t& operations)
	{
		operations = 0U;
		auto& work = operations;
		owned_object_state result;
		if (!maximum_work || !maximum_pending_nodes || !owner.hasBody() || owner.isInvalidDecl() ||
			owner.isDependentContext() || object.isInvalidDecl() || !object.hasLocalStorage() ||
			object.isStaticLocal() || !object.hasInit() || object.getType()->isReferenceType() ||
			object.getType()->isDependentType() || object.getType()->isIncompleteType() ||
			object.getType().isVolatileQualified() || object.getDeclContext() != &owner)
			return result;

		const auto* address = llvm::dyn_cast<clang::UnaryOperator>(&actual_address);
		const auto* reference = address && address->getOpcode() == clang::UO_AddrOf
			? llvm::dyn_cast<clang::DeclRefExpr>(address->getSubExpr()->IgnoreParenImpCasts())
			: nullptr;
		if (!reference || reference->getDecl() != &object)
			return result;
		result.initialized_automatic = true;

		const auto* record = object.getType()->getAsCXXRecordDecl();
		if (record &&
			(!record->hasDefinition() || !record->isTriviallyCopyable() ||
			 !record->hasTrivialDestructor()))
			return result;
		result.trivial_initialization = object.getType()->isScalarType() || record;
		if (!result.trivial_initialization)
			return result;

		std::vector<const clang::Stmt*> pending{owner.getBody()};
		bool found_declaration{}, found_address{}, only_reference{true};
		result.no_opaque_action = true;
		while (!pending.empty())
		{
			if (work == maximum_work)
				return result;
			++work;
			const auto* node = pending.back();
			pending.pop_back();
			if (const auto* declarations = llvm::dyn_cast<clang::DeclStmt>(node))
				for (const auto* declaration : declarations->decls())
				{
					if (work == maximum_work)
						return result;
					++work;
					found_declaration |= declaration == &object;
				}
			found_address |= node == address;
			if (const auto* use = llvm::dyn_cast<clang::DeclRefExpr>(node);
				use && use->getDecl() == &object)
			{
				++result.original_reference_occurrences;
				only_reference &= use == reference;
			}
			// Hidden user actions, capture, storage exposure and control transfers can
			// invalidate a declaration-only lifetime inference. Keep their point state
			// unavailable rather than trying to infer effects from call spelling.
			if (llvm::isa<clang::CallExpr,
						  clang::CXXNewExpr,
						  clang::CXXDeleteExpr,
						  clang::LambdaExpr,
						  clang::BlockExpr,
						  clang::GCCAsmStmt,
						  clang::MSAsmStmt,
						  clang::CoroutineBodyStmt,
						  clang::RecoveryExpr,
						  clang::IndirectGotoStmt>(node))
				result.no_opaque_action = false;
			if (const auto* construction = llvm::dyn_cast<clang::CXXConstructExpr>(node))
			{
				const bool original_object_initialization =
					node == object.getInit()->IgnoreParenImpCasts() &&
					construction->getConstructor() && construction->getConstructor()->isTrivial() &&
					construction->getNumArgs() == 0U;
				if (!original_object_initialization)
					result.no_opaque_action = false;
			}
			for (const auto* child : node->children())
			{
				if (work == maximum_work)
					return result;
				++work;
				if (!child)
					continue;
				if (pending.size() == maximum_pending_nodes)
					return result;
				pending.push_back(child);
			}
		}
		result.reference_census_complete = true;
		result.initialized_automatic &= found_declaration;
		result.single_address_use =
			found_address && only_reference && result.original_reference_occurrences == 1U;
		// The AST census alone cannot prove initialization before this point: a use
		// in the object's own initializer is a concrete counterexample. Traverse
		// original CFG paths with a two-state initialization bit. No source interval
		// or returned row ordering supplies this ownership/order association.
		using path_state = std::pair<const clang::CFGBlock*, bool>;
		std::vector<path_state> paths{{&original_cfg.getEntry(), false}};
		std::set<path_state> visited;
		bool cfg_point{}, uninitialized_point{}, cfg_declaration{};
		const auto tick = [&]
		{
			if (work == maximum_work)
				return false;
			++work;
			return true;
		};
		const auto contains_address = [&](const clang::Stmt* root, bool& contains)
		{
			std::vector<const clang::Stmt*> descendants{root};
			while (!descendants.empty())
			{
				if (!tick())
					return false;
				const auto* current = descendants.back();
				descendants.pop_back();
				if (!current)
					continue;
				contains |= current == address;
				for (const auto* child : current->children())
				{
					if (!tick())
						return false;
					if (!child)
						continue;
					if (descendants.size() == maximum_pending_nodes)
						return false;
					descendants.push_back(child);
				}
			}
			return true;
		};
		while (!paths.empty())
		{
			if (!tick())
				return result;
			const auto state = paths.back();
			paths.pop_back();
			if (visited.contains(state))
				continue;
			if (visited.size() == maximum_pending_nodes)
				return result;
			visited.insert(state);
			bool initialized = state.second;
			for (const auto& element : *state.first)
			{
				if (!tick())
					return result;
				const auto original = element.getAs<clang::CFGStmt>();
				if (!original)
					continue;
				const auto* statement = original->getStmt();
				bool at_address{};
				if (!contains_address(statement, at_address))
					return result;
				if (at_address)
				{
					cfg_point = true;
					uninitialized_point |= !initialized;
				}
				if (const auto* declarations = llvm::dyn_cast<clang::DeclStmt>(statement))
					for (const auto* declaration : declarations->decls())
					{
						if (!tick())
							return result;
						if (declaration == &object)
						{
							initialized = true;
							cfg_declaration = true;
						}
					}
			}
			for (const auto& edge : state.first->succs())
			{
				if (!tick())
					return result;
				const auto* target = edge.getReachableBlock();
				if (!target)
					continue;
				if (paths.size() == maximum_pending_nodes)
					return result;
				paths.push_back({target, initialized});
			}
		}
		result.original_cfg_complete = cfg_point && cfg_declaration;
		result.initialized_before_address = result.original_cfg_complete && !uninitialized_point;
		return result;
	}
} // namespace cxxlens::detail::clang22::object_semantics
