#include "project_exceptional_exits.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <utility>

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
#include <clang/AST/DeclCXX.h>
#include <clang/AST/EvaluatedExprVisitor.h>
#include <clang/AST/GlobalDecl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/CodeGenOptions.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/HeaderSearch.h>
#include <clang/Lex/Preprocessor.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/TargetParser/Triple.h>
#endif

namespace cxxlens::detail::clang22
{
#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
	namespace
	{
		struct bound_call
		{
			const llvm::CallBase* instruction{};
			const clang::Decl* target{};
			const clang::Stmt* expression{};
			unsigned methods{};
			bool generic{};
		};
		struct original_carrier
		{
			const llvm::BasicBlock* block{};
			const clang::Stmt* expression{};
			unsigned route{};
		};
		struct original_builtin
		{
			const clang::Stmt* expression{};
			const clang::Decl* target{};
			unsigned id{};
			std::vector<const llvm::CallBase*> previous;
		};
		struct original_frame
		{
			std::size_t scope{std::numeric_limits<std::size_t>::max()}, variant{};
			llvm::Function* function{};
			const clang::Decl* owner{};
			std::vector<std::pair<const clang::Stmt*, unsigned>> expressions;
			std::vector<bound_call> calls;
			std::vector<original_carrier> carriers;
			std::vector<original_builtin> builtins;
			std::size_t admissions{}, emissions{};
		};
		struct scope_binding
		{
			const clang::FunctionDecl* declaration{};
			std::vector<const clang::CXXThrowExpr*> throws;
		};
		struct recorder
		{
			provider::clang22::borrowed_translation_unit& unit;
			const exceptional_exit_limits& limits;
			const exceptional_compiler_bindings& compiler_bindings;
			project_exceptional_exit_observations output;
			std::vector<scope_binding> bindings;
			std::map<void*, original_frame> frames;
			std::optional<sdk::error> failure;
			bool allocation_failure{}, callback_failure{};
			std::size_t members{};
			bool work(std::size_t n = 1U)
			{
				if (failure || allocation_failure || callback_failure)
					return false;
				if (limits.cancelled && limits.cancelled())
					return fail("native.exceptional-exit-cancelled", "operations", "cancelled");
				if (n > limits.maximum_operations -
						std::min(output.operations, limits.maximum_operations))
					return fail("native.exceptional-exit-budget", "operations", "limit-exceeded");
				output.operations += n;
				return true;
			}
			bool fail(std::string code, std::string field, std::string reason)
			{
				if (!failure)
					failure = sdk::error{std::move(code), std::move(field), std::move(reason)};
				return false;
			}
			bool retain(std::size_t n)
			{
				if (!work())
					return false;
				if (n > limits.maximum_retained_bytes -
						std::min(output.retained_bytes, limits.maximum_retained_bytes))
					return fail(
						"native.exceptional-exit-budget", "retained-bytes", "limit-exceeded");
				output.retained_bytes += n;
				return true;
			}
			std::string usr(const clang::Decl* declaration)
			{
				if (!declaration || !work())
					return {};
				llvm::SmallString<256> bytes;
				if (clang::index::generateUSRForDecl(declaration, bytes))
					return {};
				if (!retain(bytes.size() + sizeof(std::string)))
					return {};
				return std::string(bytes);
			}
			void bind_expression(exceptional_occurrence& value, const clang::Stmt* expression)
			{
				if (!expression || !compiler_bindings.expression || !work())
					return;
				const auto original = compiler_bindings.expression(expression);
				if (retain(original.size()))
					value.expression = original;
			}
			void bind_target(exceptional_occurrence& value, const clang::Decl* target)
			{
				if (!target || !compiler_bindings.target || !work())
					return;
				const auto original = compiler_bindings.target(target);
				if (retain(original.size()))
					value.target = original;
			}
			static exceptional_source source(const clang::Stmt* statement)
			{
				return statement ? exceptional_source{statement->getBeginLoc().getRawEncoding(),
													  statement->getEndLoc().getRawEncoding(),
													  false}
								 : exceptional_source{};
			}
			bool occurrence(exceptional_lowering_variant& variant, exceptional_occurrence value)
			{
				if (members >= limits.maximum_occurrences)
					return fail("native.exceptional-exit-budget", "occurrences", "limit-exceeded");
				if (!retain(sizeof(value) + 256U + value.role.size() + value.eligibility.size() +
							value.target_usr.size() + value.expression_kind.size()))
					return false;
				++members;
				variant.occurrences.push_back(std::move(value));
				return true;
			}
			original_frame* frame(void* context)
			{
				if (!work())
					return nullptr;
				const auto found = frames.find(context);
				if (found == frames.end() ||
					found->second.scope == std::numeric_limits<std::size_t>::max())
					return nullptr;
				return &found->second;
			}
			bound_call* call(original_frame& current, const llvm::CallBase* instruction)
			{
				for (auto& value : current.calls)
				{
					if (!work())
						return nullptr;
					if (value.instruction == instruction)
						return &value;
				}
				if (!retain(sizeof(bound_call) * 2U + 64U))
					return nullptr;
				current.calls.push_back(
					{instruction,
					 nullptr,
					 current.expressions.empty() ? nullptr : current.expressions.back().first,
					 0U,
					 false});
				return &current.calls.back();
			}
			void begin(void* context, const clang::Decl* declaration, llvm::Function* function)
			{
				if (!work() || !declaration || !function)
					return;
				if (frames.contains(context))
				{
					fail("native.exceptional-exit-invalid", "lowering", "duplicate-function-begin");
					return;
				}
				original_frame current;
				current.function = function;
				current.owner = declaration;
				for (std::size_t i{}; i < bindings.size(); ++i)
				{
					if (!work())
						return;
					if (bindings[i].declaration == declaration)
					{
						current.scope = i;
						break;
					}
				}
				if (current.scope != std::numeric_limits<std::size_t>::max())
				{
					auto& scope = output.scopes[current.scope];
					if (!retain(sizeof(exceptional_lowering_variant) * 2U +
								function->getName().size() + 128U))
						return;
					current.variant = scope.variants.size();
					scope.variants.push_back(
						{"function", std::string(function->getName()), 0U, {}, false, {}});
				}
				if (retain(sizeof(original_frame) + 256U))
					frames.emplace(context, std::move(current));
			}
			void finish(void* context, const clang::Decl* declaration, llvm::Function* function)
			{
				if (!work())
					return;
				const auto found = frames.find(context);
				if (found == frames.end())
				{
					fail("native.exceptional-exit-invalid",
						 "lowering",
						 "function-end-without-begin");
					return;
				}
				auto& current = found->second;
				if (current.scope == std::numeric_limits<std::size_t>::max())
				{
					frames.erase(found);
					return;
				}
				if (current.owner != declaration || current.function != function ||
					!current.expressions.empty() || !current.builtins.empty())
				{
					fail("native.exceptional-exit-invalid",
						 "lowering",
						 "original-function-correspondence");
					return;
				}
				auto& scope = output.scopes[current.scope];
				auto& variant = scope.variants[current.variant];
				bool complete = current.admissions == current.emissions;
				for (std::size_t i{}; i < bindings[current.scope].throws.size(); ++i)
				{
					const auto* expression = bindings[current.scope].throws[i];
					exceptional_occurrence value;
					value.role = "written_throw";
					value.eligibility = "eligible";
					value.source = source(expression);
					value.original_expression_ordinal = i;
					value.expression_kind = expression->getStmtClassName();
					bind_expression(value, expression);
					if (!occurrence(variant, std::move(value)))
						return;
				}
				std::size_t block_ordinal{};
				for (const auto& block : *function)
				{
					if (!work())
						return;
					bool termination = false;
					for (const auto& carrier : current.carriers)
					{
						if (!work())
							return;
						if (carrier.block != &block || carrier.route < 2U || carrier.route > 4U)
							continue;
						termination = true;
						exceptional_occurrence value;
						value.role = "termination";
						value.eligibility = "eligible";
						value.block_ordinal = block_ordinal;
						value.compiler_route = carrier.route;
						if (!occurrence(variant, std::move(value)))
							return;
						break; // One actual block, even when several compiler views refer to it.
					}
					std::size_t instruction_ordinal{};
					for (const auto& instruction : block)
					{
						if (!work())
							return;
						if (const auto* original = llvm::dyn_cast<llvm::CallBase>(&instruction))
						{
							if (!work(current.calls.size()))
								return;
							const auto bound = std::ranges::find(
								current.calls, original, &bound_call::instruction);
							exceptional_occurrence value;
							value.block_ordinal = block_ordinal;
							value.instruction_ordinal = instruction_ordinal;
							value.is_invoke = llvm::isa<llvm::InvokeInst>(original);
							value.does_not_throw = original->doesNotThrow();
							value.does_not_return = original->doesNotReturn();
							value.intrinsic_id = static_cast<unsigned>(original->getIntrinsicID());
							if (bound == current.calls.end())
							{
								// These original compiler intrinsics cannot be an exceptional
								// exit. Their LLVM identity and completed instruction position
								// are sufficient; an AST source binding remains independent.
								const auto id = original->getIntrinsicID();
								const bool nonexceptional_intrinsic =
									(id == llvm::Intrinsic::lifetime_start ||
									 id == llvm::Intrinsic::lifetime_end ||
									 id == llvm::Intrinsic::donothing ||
									 id == llvm::Intrinsic::memcpy ||
									 id == llvm::Intrinsic::memmove ||
									 id == llvm::Intrinsic::memset) &&
									original->doesNotThrow() && !original->doesNotReturn();
								complete &= nonexceptional_intrinsic;
								value.role = "ordinary_instruction";
								value.eligibility =
									nonexceptional_intrinsic ? "excluded" : "unknown";
								value.binding_complete = false;
							}
							else
							{
								value.emitter_methods = bound->methods | (bound->generic ? 1U : 0U);
								value.target_usr = usr(bound->target);
								bind_target(value, bound->target);
								bind_expression(value, bound->expression);
								value.source = source(bound->expression);
								if (bound->expression)
									value.expression_kind = bound->expression->getStmtClassName();
								// The Itanium throw/rethrow emitter selects the actual
								// noreturn runtime-call route. A generic copy/constructor
								// call under that AST context remains its own call site.
								const bool throw_helper = !bound->generic && !bound->target &&
									(bound->methods & (1U << 3U)) != 0U && bound->expression &&
									llvm::isa<clang::CXXThrowExpr>(bound->expression);
								const bool escaping = !original->doesNotThrow() &&
									!llvm::isa<llvm::InvokeInst>(original);
								value.role = termination || throw_helper ? "lowering_helper"
									: escaping							 ? "escaping_call"
																		 : "ordinary_instruction";
								value.eligibility = !termination && !throw_helper && escaping
									? "eligible"
									: "excluded";
								if ((bound->methods & (1U << 5U)) != 0U && !termination)
								{
									// A compiler termination instruction whose block route was not
									// admitted.
									complete = false;
									value.eligibility = "unknown";
									value.binding_complete = false;
								}
							}
							if (!occurrence(variant, std::move(value)))
								return;
						}
						if (llvm::isa<llvm::ResumeInst>(instruction))
						{
							exceptional_occurrence value;
							value.role = "unhandled_resume";
							value.eligibility = "eligible";
							value.block_ordinal = block_ordinal;
							value.instruction_ordinal = instruction_ordinal;
							if (!occurrence(variant, std::move(value)))
								return;
						}
						if (llvm::isa<llvm::CleanupReturnInst,
									  llvm::CatchSwitchInst,
									  llvm::CatchReturnInst>(instruction))
							complete = false; // Other EH target routes need their actual carrier
											  // admission.
						++instruction_ordinal;
					}
					++block_ordinal;
				}
				variant.complete = complete;
				if (!complete)
					variant.reason = "original-lowering-call-or-EH-route-unbound";
				frames.erase(found);
			}
		};
		thread_local recorder* active_recorder{};
		template <typename F>
		void callback(F&& action) noexcept
		{
			if (!active_recorder || active_recorder->failure ||
				active_recorder->allocation_failure || active_recorder->callback_failure)
				return;
			try
			{
				action(*active_recorder);
			}
			catch (const std::bad_alloc&)
			{
				active_recorder->allocation_failure = true;
			}
			catch (...)
			{
				active_recorder->callback_failure = true;
			}
		}
		struct evaluated_admission : clang::EvaluatedExprVisitor<evaluated_admission>
		{
			recorder& owner;
			std::size_t scope;
			evaluated_admission(recorder& value, std::size_t index)
				: EvaluatedExprVisitor(value.unit.ast()), owner(value), scope(index)
			{
			}
			bool shouldVisitDiscardedStmt() const
			{
				return false;
			}
			void VisitStmt(clang::Stmt* statement)
			{
				if (owner.work())
					EvaluatedExprVisitor::VisitStmt(statement);
			}
			void VisitCXXThrowExpr(clang::CXXThrowExpr* expression)
			{
				if (owner.retain(sizeof(expression) * 2U + 32U))
					owner.bindings[scope].throws.push_back(expression);
				VisitStmt(expression);
			}
			void VisitCXXDefaultArgExpr(clang::CXXDefaultArgExpr*)
			{
				owner.output.scopes[scope].activation_complete = false;
			}
			void VisitCXXDefaultInitExpr(clang::CXXDefaultInitExpr*)
			{
				owner.output.scopes[scope].activation_complete = false;
			}
			void VisitCoroutineBodyStmt(clang::CoroutineBodyStmt*)
			{
				owner.output.scopes[scope].activation_complete = false;
			}
		};
		struct written_admission : clang::RecursiveASTVisitor<written_admission>
		{
			recorder& owner;
			explicit written_admission(recorder& value) : owner(value) {}
			bool TraverseDecl(clang::Decl* declaration)
			{
				return owner.work() && RecursiveASTVisitor::TraverseDecl(declaration);
			}
			bool VisitLambdaExpr(clang::LambdaExpr* expression)
			{
				return admit(expression->getCallOperator(), true);
			}
			bool VisitFunctionDecl(clang::FunctionDecl* declaration)
			{
				return admit(declaration, false);
			}
			bool admit(clang::FunctionDecl* declaration, bool written_lambda)
			{
				if (!declaration || (declaration->isImplicit() && !written_lambda) ||
					declaration->isDefaulted() || declaration->isDeleted() ||
					!declaration->doesThisDeclarationHaveABody())
					return true;
				for (const auto& binding : owner.bindings)
				{
					if (!owner.work())
						return false;
					if (binding.declaration == declaration)
						return true;
				}
				if (owner.bindings.size() >= owner.limits.maximum_scopes)
					return owner.fail("native.exceptional-exit-budget", "scopes", "limit-exceeded");
				if (!owner.retain(sizeof(exceptional_physical_scope) * 2U +
								  sizeof(scope_binding) * 2U + 512U))
					return false;
				exceptional_physical_scope value;
				value.owner_usr = owner.usr(declaration);
				if (owner.compiler_bindings.scope)
				{
					const auto original = owner.compiler_bindings.scope(declaration);
					if (!owner.retain(original.detail.size() + original.function.size() +
									  original.definition_source.size() + original.body.size()))
						return false;
					value.detail = original.detail;
					value.function = original.function;
					value.definition_source = original.definition_source;
					value.body_id = original.body;
				}
				value.declaration = {declaration->getBeginLoc().getRawEncoding(),
									 declaration->getEndLoc().getRawEncoding(),
									 true};
				value.body = recorder::source(declaration->getBody());
				value.admitted = !declaration->isDependentContext() &&
					!declaration->getType()->isDependentType();
				if (!value.admitted)
					value.reason = "dependent-written-definition";
				const auto index = owner.output.scopes.size();
				owner.output.scopes.push_back(std::move(value));
				owner.bindings.push_back({declaration, {}});
				evaluated_admission evaluated(owner, index);
				evaluated.Visit(declaration->getBody());
				if (const auto* constructor =
						llvm::dyn_cast<clang::CXXConstructorDecl>(declaration))
					for (const auto* initializer : constructor->inits())
					{
						if (!owner.work())
							return false;
						if (initializer->getInit())
							evaluated.Visit(initializer->getInit());
					}
				return !owner.failure;
			}
		};
	} // namespace
#endif
	sdk::result<project_exceptional_exit_observations>
	observe_project_exceptional_exits(provider::clang22::borrowed_translation_unit& unit,
									  exceptional_exit_limits limits,
									  exceptional_compiler_bindings compiler_bindings)
	{
#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
		try
		{
			recorder state{unit, limits, compiler_bindings, {}, {}, {}, {}, false, false, 0U};
			written_admission admission(state);
			admission.TraverseDecl(unit.ast().getTranslationUnitDecl());
			if (state.allocation_failure)
				return sdk::unexpected(sdk::error{
					"native.exceptional-exit-resource-exhausted", "callback", "allocation"});
			if (state.callback_failure)
				return sdk::unexpected(
					sdk::error{"native.exceptional-exit-invalid", "callback", "exception"});
			if (state.failure)
				return sdk::unexpected(*state.failure);
#if defined(CXXLENS_CLANG_EXCEPTIONAL_EVENTS) && CXXLENS_CLANG_EXCEPTIONAL_EVENTS
			if (active_recorder)
				return sdk::unexpected(
					sdk::error{"native.exceptional-exit-invalid", "scope", "nested-analysis"});
			const auto& triple = unit.ast().getTargetInfo().getTriple();
			const bool supported = triple.isOSLinux() &&
				(triple.getArch() == llvm::Triple::x86_64 ||
				 triple.getArch() == llvm::Triple::aarch64);
			if (supported)
			{
				struct guard
				{
					~guard()
					{
						active_recorder = nullptr;
					}
				} reset;
				active_recorder = &state;
				state.output.hooks_installed = true;
				llvm::LLVMContext llvm_context;
				auto& preprocessing = unit.preprocessor();
				auto generator = clang::CreateLLVMCodeGen(
					unit.ast().getDiagnostics(),
					"cxxlens-original-exceptional-analysis",
					unit.source_manager().getFileManager().getVirtualFileSystemPtr(),
					preprocessing.getHeaderSearchInfo().getHeaderSearchOpts(),
					preprocessing.getPreprocessorOpts(),
					unit.code_generation_options(),
					llvm_context);
				if (!generator)
					return sdk::unexpected(sdk::error{
						"native.exceptional-exit-invalid", "lowering", "generator-unavailable"});
				generator->Initialize(unit.ast());
				for (auto* declaration : unit.ast().getTranslationUnitDecl()->decls())
				{
					if (!state.work())
						break;
					if (!generator->HandleTopLevelDecl(clang::DeclGroupRef(declaration)))
					{
						state.fail("native.exceptional-exit-invalid",
								   "lowering",
								   "original-declaration-rejected");
						break;
					}
				}
				for (std::size_t i{}; i < state.bindings.size() && !state.failure; ++i)
				{
					if (!state.work())
						break;
					if (!state.output.scopes[i].admitted)
						continue;
					const auto* declaration = state.bindings[i].declaration;
					if (const auto* constructor =
							llvm::dyn_cast<clang::CXXConstructorDecl>(declaration))
						generator->GetAddrOfGlobal(
							clang::GlobalDecl(constructor, clang::Ctor_Complete), false);
					else if (const auto* destructor =
								 llvm::dyn_cast<clang::CXXDestructorDecl>(declaration))
						generator->GetAddrOfGlobal(
							clang::GlobalDecl(destructor, clang::Dtor_Complete), false);
					else
						generator->GetAddrOfGlobal(clang::GlobalDecl(declaration), false);
				}
				if (!state.failure)
					generator->HandleTranslationUnit(unit.ast());
				if (!state.failure && !state.frames.empty())
					state.fail("native.exceptional-exit-invalid",
							   "lowering",
							   "unfinished-original-function");
			}
#endif
			if (state.allocation_failure)
				return sdk::unexpected(sdk::error{
					"native.exceptional-exit-resource-exhausted", "callback", "allocation"});
			if (state.callback_failure)
				return sdk::unexpected(
					sdk::error{"native.exceptional-exit-invalid", "callback", "exception"});
			if (state.failure)
				return sdk::unexpected(*state.failure);
			for (auto& scope : state.output.scopes)
			{
				scope.complete = state.output.hooks_installed && scope.admitted &&
					scope.activation_complete && !scope.variants.empty() &&
					std::ranges::all_of(scope.variants, &exceptional_lowering_variant::complete);
				if (!scope.complete && scope.reason.empty())
					scope.reason = !state.output.hooks_installed
						? "compiler-lowering-hooks-unsupported"
						: !scope.activation_complete
						? "original-default-or-coroutine-activation-unrepresented"
						: scope.variants.empty() ? "admitted-written-definition-not-lowered"
												 : "original-lowering-member-binding-unavailable";
			}
			state.output.frozen = true;
			return std::move(state.output);
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				sdk::error{"native.exceptional-exit-resource-exhausted", "analysis", "allocation"});
		}
		catch (...)
		{
			return sdk::unexpected(
				sdk::error{"native.exceptional-exit-invalid", "analysis", "exception"});
		}
#else
		(void)unit;
		(void)limits;
		(void)compiler_bindings;
		return sdk::unexpected(
			sdk::error{"native.exceptional-exit-unsupported", "analysis", "compiler-unavailable"});
#endif
	}
} // namespace cxxlens::detail::clang22

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
using namespace cxxlens::detail::clang22;
extern "C" void cxxlens_eh_function(void* context,
									const clang::Decl* declaration,
									llvm::Function* function,
									bool begin) noexcept
{
	callback(
		[&](recorder& value)
		{
			if (begin)
				value.begin(context, declaration, function);
			else
				value.finish(context, declaration, function);
		});
}
extern "C" void cxxlens_eh_variant(void* context, const clang::GlobalDecl* declaration) noexcept
{
	callback(
		[&](recorder& value)
		{
			auto* current = value.frame(context);
			if (!current || !declaration)
				return;
			auto& variant = value.output.scopes[current->scope].variants[current->variant];
			if (declaration->getDecl() != current->owner)
			{
				value.fail("native.exceptional-exit-invalid", "variant", "foreign-owner");
				return;
			}
			if (llvm::isa<clang::CXXConstructorDecl>(current->owner))
			{
				variant.kind = "constructor";
				variant.index = static_cast<unsigned>(declaration->getCtorType());
			}
			else if (llvm::isa<clang::CXXDestructorDecl>(current->owner))
			{
				variant.kind = "destructor";
				variant.index = static_cast<unsigned>(declaration->getDtorType());
			}
			else if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(current->owner))
			{
				variant.kind = function->isReferenceableKernel() ? "kernel" : "function";
				variant.index = function->isReferenceableKernel()
					? static_cast<unsigned>(declaration->getKernelReferenceKind())
					: declaration->getMultiVersionIndex();
			}
			else
				variant.kind = "block";
		});
}
extern "C" void cxxlens_eh_expression(void* context,
									  const clang::Decl* declaration,
									  const clang::Stmt* expression,
									  unsigned kind,
									  bool begin) noexcept
{
	callback(
		[&](recorder& value)
		{
			auto* current = value.frame(context);
			if (!current)
				return;
			if (current->owner != declaration || !expression)
			{
				value.fail("native.exceptional-exit-invalid", "expression", "foreign-owner");
				return;
			}
			if (begin)
			{
				if (current->expressions.size() >= 256U)
				{
					value.fail(
						"native.exceptional-exit-budget", "expression-depth", "limit-exceeded");
					return;
				}
				if (value.retain(64U))
					current->expressions.emplace_back(expression, kind);
			}
			else
			{
				if (current->expressions.empty() ||
					current->expressions.back() != std::pair{expression, kind})
				{
					value.fail("native.exceptional-exit-invalid",
							   "expression",
							   "unbalanced-original-context");
					return;
				}
				current->expressions.pop_back();
			}
		});
}
extern "C" void
cxxlens_eh_call_admit(void* context, const clang::Decl* declaration, const clang::Decl*) noexcept
{
	callback(
		[&](recorder& value)
		{
			auto* current = value.frame(context);
			if (!current)
				return;
			if (current->owner != declaration)
				value.fail("native.exceptional-exit-invalid", "call", "foreign-owner");
			else
				++current->admissions;
		});
}
extern "C" void cxxlens_eh_call_emit(void* context,
									 const clang::Decl* declaration,
									 const clang::Decl* target,
									 llvm::CallBase* instruction,
									 bool) noexcept
{
	callback(
		[&](recorder& value)
		{
			auto* current = value.frame(context);
			if (!current)
				return;
			if (current->owner != declaration || !instruction)
			{
				value.fail(
					"native.exceptional-exit-invalid", "call", "foreign-owner-or-instruction");
				return;
			}
			if (auto* call = value.call(*current, instruction))
			{
				call->generic = true;
				call->target = target;
				++current->emissions;
			}
		});
}
extern "C" void cxxlens_eh_runtime_emit(void* context,
										const clang::Decl* declaration,
										llvm::CallBase* instruction,
										unsigned method) noexcept
{
	callback(
		[&](recorder& value)
		{
			auto* current = value.frame(context);
			if (!current)
				return;
			if (current->owner != declaration || !instruction || method == 0U || method > 6U)
			{
				value.fail(
					"native.exceptional-exit-invalid", "runtime", "original-binding-unavailable");
				return;
			}
			if (auto* call = value.call(*current, instruction))
				call->methods |= 1U << method;
		});
}
extern "C" void cxxlens_codegen_eh_observe(void* context,
										   const clang::Decl* declaration,
										   const clang::Stmt* expression,
										   llvm::BasicBlock* block,
										   unsigned kind) noexcept
{
	callback(
		[&](recorder& value)
		{
			auto* current = value.frame(context);
			if (!current)
				return;
			if (current->owner != declaration || !block || kind == 0U || kind > 5U)
			{
				value.fail(
					"native.exceptional-exit-invalid", "EH-route", "original-binding-unavailable");
				return;
			}
			if (value.retain(sizeof(original_carrier) * 2U + 32U))
				current->carriers.push_back({block, expression, kind});
		});
}
extern "C" void cxxlens_eh_builtin(void* context,
								   const clang::Decl* declaration,
								   const clang::Decl* target,
								   const clang::Stmt* expression,
								   unsigned builtin,
								   bool begin) noexcept
{
	callback(
		[&](recorder& value)
		{
			auto* current = value.frame(context);
			if (!current)
				return;
			if (current->owner != declaration || !expression)
			{
				value.fail(
					"native.exceptional-exit-invalid", "builtin", "original-binding-unavailable");
				return;
			}
			if (begin)
			{
				if (current->builtins.size() >= 256U ||
					!value.retain(sizeof(original_builtin) * 2U + 64U))
				{
					if (!value.failure)
						value.fail(
							"native.exceptional-exit-budget", "builtin-depth", "limit-exceeded");
					return;
				}
				original_builtin frame{expression, target, builtin, {}};
				for (const auto& block : *current->function)
					for (const auto& instruction : block)
					{
						if (!value.work())
							return;
						if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
						{
							if (!value.retain(sizeof(call) * 2U + 32U))
								return;
							frame.previous.push_back(call);
						}
					}
				current->builtins.push_back(std::move(frame));
			}
			else
			{
				if (current->builtins.empty() ||
					current->builtins.back().expression != expression ||
					current->builtins.back().target != target ||
					current->builtins.back().id != builtin)
				{
					value.fail("native.exceptional-exit-invalid",
							   "builtin",
							   "unbalanced-original-context");
					return;
				}
				auto prior = std::move(current->builtins.back());
				current->builtins.pop_back();
				for (const auto& block : *current->function)
					for (const auto& instruction : block)
					{
						if (!value.work())
							return;
						const auto* original = llvm::dyn_cast<llvm::CallBase>(&instruction);
						if (!original)
							continue;
						if (!value.work(prior.previous.size()))
							return;
						if (std::ranges::find(prior.previous, original) != prior.previous.end())
							continue;
						if (auto* call = value.call(*current, original))
						{
							if (!call->generic && call->methods == 0U)
							{
								call->target = target;
								call->expression = expression;
							}
						}
					}
			}
		});
}
#endif
