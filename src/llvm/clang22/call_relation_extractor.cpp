#include "call_relation_extractor.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "symbol_type_extractor.hpp"

#ifndef CXXLENS_HAS_CLANG22
#define CXXLENS_HAS_CLANG22 0
#endif

#if CXXLENS_HAS_CLANG22
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/ParentMapContext.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/Type.h>
#include <llvm/Support/Casting.h>
#endif

namespace cxxlens::detail::clang22
{
	namespace
	{
		[[nodiscard]] error relation_error(std::string reason)
		{
			error failure;
			failure.code.value = "extractor.invalid-observation";
			failure.message = "Call and relation observation extraction failed";
			failure.scope = failure_scope::compile_unit;
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

#if CXXLENS_HAS_CLANG22
		[[nodiscard]] std::string bool_text(const bool value)
		{
			return value ? "true" : "false";
		}

		[[nodiscard]] std::string source_key(const std::optional<source_span>& source)
		{
			if (!source)
				return "source:unavailable";
			return "source:" + std::string{source->primary.begin.file.value()} + ":" +
				std::to_string(source->primary.begin.byte_offset) + ":" +
				std::to_string(source->primary.end.byte_offset);
		}

		[[nodiscard]] std::string join_ids(const std::vector<symbol_id>& values)
		{
			std::string output;
			for (const auto& value : values)
			{
				if (!output.empty())
					output.push_back(',');
				output += value.value();
			}
			return output;
		}

		[[nodiscard]] std::string reference_role(clang::ASTContext& context,
												 const clang::Expr& expression)
		{
			clang::DynTypedNode current = clang::DynTypedNode::create(expression);
			for (std::uint32_t depth = 0U; depth < 6U; ++depth)
			{
				const auto parents = context.getParents(current);
				if (parents.empty())
					break;
				const auto& parent = parents[0];
				if (const auto* unary = parent.get<clang::UnaryOperator>();
					unary != nullptr && unary->getOpcode() == clang::UO_AddrOf)
					return "address_taken";
				if (const auto* binary = parent.get<clang::BinaryOperator>(); binary != nullptr)
				{
					if (binary->isAssignmentOp() &&
						binary->getLHS()->IgnoreParenImpCasts() == expression.IgnoreParenImpCasts())
						return binary->getOpcode() == clang::BO_Assign ? "write" : "read_write";
				}
				if (parent.get<clang::CallExpr>() != nullptr)
					return "call";
				current = parent;
			}
			return "read";
		}

		[[nodiscard]] std::string operator_name(const clang::OverloadedOperatorKind value)
		{
			return clang::getOperatorSpelling(value);
		}

		class relation_visitor final : public clang::RecursiveASTVisitor<relation_visitor>
		{
		  public:
			relation_visitor(clang::ASTContext& context, semantic_identity_adapter& identities)
				: context_{context}, identities_{identities}
			{
			}

			bool TraverseFunctionDecl(clang::FunctionDecl* declaration)
			{
				const auto* previous = caller_;
				caller_ = declaration;
				const auto result =
					clang::RecursiveASTVisitor<relation_visitor>::TraverseFunctionDecl(declaration);
				caller_ = previous;
				return result;
			}

			bool VisitDeclRefExpr(clang::DeclRefExpr* expression)
			{
				if (expression == nullptr || expression->getDecl() == nullptr)
					return true;
				return emit_reference(*expression, *expression->getDecl(), "decl_ref");
			}

			bool VisitMemberExpr(clang::MemberExpr* expression)
			{
				if (expression == nullptr || expression->getMemberDecl() == nullptr)
					return true;
				return emit_reference(*expression, *expression->getMemberDecl(), "member_ref");
			}

			bool VisitCallExpr(clang::CallExpr* expression)
			{
				if (expression == nullptr)
					return true;
				std::string kind = "unknown";
				std::string dispatch = "unresolved";
				std::optional<detached_symbol_identity> target;
				std::optional<detached_type_identity> receiver;
				std::vector<symbol_id> candidates;
				std::optional<std::string> unresolved_reason;

				const auto* direct = expression->getDirectCallee();
				if (direct != nullptr)
				{
					auto identity = identities_.symbol(*direct);
					if (!identity)
						return fail(std::move(identity.error()));
					target = std::move(identity.value());
				}

				if (const auto* member = llvm::dyn_cast<clang::CXXMemberCallExpr>(expression))
				{
					const auto* method = member->getMethodDecl();
					if (method != nullptr)
					{
						kind = llvm::isa<clang::CXXDestructorDecl>(method)
							? "destructor"
							: (method->isVirtual() ? "virtual_member" : "member");
						dispatch = "static_member_target";
					}
					else
						unresolved_reason = "unresolved-member-target";
					if (const auto* object = member->getImplicitObjectArgument(); object != nullptr)
					{
						auto type = identities_.type(object->getType());
						if (!type)
							return fail(std::move(type.error()));
						receiver = std::move(type.value());
					}
				}
				else if (const auto* operation =
							 llvm::dyn_cast<clang::CXXOperatorCallExpr>(expression))
				{
					kind = "overloaded_operator";
					dispatch = target ? "direct_exact" : "unresolved";
					operator_spelling_ = operator_name(operation->getOperator());
				}
				else if (target)
				{
					kind = "direct_function";
					dispatch = "direct_exact";
				}
				else
				{
					const auto callee_type = expression->getCallee()->getType();
					kind = callee_type->isFunctionPointerType() || callee_type->isBlockPointerType()
						? "function_pointer"
						: "unknown";
					unresolved_reason =
						expression->isTypeDependent() || expression->isValueDependent()
						? "dependent-call-target"
						: "indirect-call-target";
					const auto* callee = expression->getCallee()->IgnoreParenImpCasts();
					if (const auto* lookup = llvm::dyn_cast<clang::UnresolvedLookupExpr>(callee))
					{
						for (const auto declaration : lookup->decls())
						{
							if (const auto* named = llvm::dyn_cast<clang::NamedDecl>(declaration))
							{
								auto candidate = identities_.symbol(*named);
								if (!candidate)
									return fail(std::move(candidate.error()));
								candidates.push_back(candidate.value().id);
							}
						}
					}
				}
				return emit_call(*expression,
								 kind,
								 dispatch,
								 std::move(target),
								 std::move(receiver),
								 std::move(candidates),
								 std::move(unresolved_reason));
			}

			bool VisitCXXConstructExpr(clang::CXXConstructExpr* expression)
			{
				if (expression == nullptr || expression->getConstructor() == nullptr)
					return true;
				auto target = identities_.symbol(*expression->getConstructor());
				if (!target)
					return fail(std::move(target.error()));
				return emit_call(*expression,
								 "constructor",
								 "direct_exact",
								 std::optional{std::move(target.value())},
								 std::nullopt,
								 {},
								 std::nullopt);
			}

			bool VisitBinaryOperator(clang::BinaryOperator* expression)
			{
				if (expression == nullptr)
					return true;
				operator_spelling_ = expression->getOpcodeStr().str();
				return emit_call(*expression,
								 "builtin_operator",
								 "unresolved",
								 std::nullopt,
								 std::nullopt,
								 {},
								 std::optional<std::string>{"language-builtin-operator"});
			}

			bool VisitUnaryOperator(clang::UnaryOperator* expression)
			{
				if (expression == nullptr)
					return true;
				operator_spelling_ =
					clang::UnaryOperator::getOpcodeStr(expression->getOpcode()).str();
				return emit_call(*expression,
								 "builtin_operator",
								 "unresolved",
								 std::nullopt,
								 std::nullopt,
								 {},
								 std::optional<std::string>{"language-builtin-operator"});
			}

			[[nodiscard]] std::optional<error> take_error()
			{
				return std::move(error_);
			}

			[[nodiscard]] std::vector<facts::observation_record> take()
			{
				return std::move(observations_);
			}

		  private:
			template <class Expression>
			bool emit_reference(const Expression& expression,
								const clang::NamedDecl& target_declaration,
								std::string expression_kind)
			{
				auto target = identities_.symbol(target_declaration);
				if (!target)
					return fail(std::move(target.error()));
				auto source = identities_.source(expression.getSourceRange());
				const auto normalized = source ? std::optional{source.value()} : std::nullopt;
				const auto role = reference_role(context_, expression);
				auto observation =
					identities_.observation(fact_kind::reference,
											"reference:" + std::string{target.value().id.value()} +
												":" + role + ":" + source_key(normalized),
											normalized);
				observation.name = target.value().name;
				observation.payload.emplace("reference.target",
											std::string{target.value().id.value()});
				observation.payload.emplace("reference.role", role);
				observation.payload.emplace("reference.expression_kind",
											std::move(expression_kind));
				observation.payload.emplace(
					"reference.from_macro",
					bool_text(normalized && normalized->origin != source_origin::directly_spelled));
				if (caller_ != nullptr)
				{
					auto caller = identities_.symbol(*caller_);
					if (!caller)
						return fail(std::move(caller.error()));
					observation.payload.emplace("reference.owner",
												std::string{caller.value().id.value()});
				}
				identities_.mark(observation,
								 normalized ? coverage_state::covered : coverage_state::unresolved,
								 normalized ? std::nullopt
											: std::optional<std::string>{"source-unavailable"});
				observations_.push_back(std::move(observation));
				return true;
			}

			template <class Expression>
			bool emit_call(const Expression& expression,
						   std::string kind, // NOLINT(bugprone-easily-swappable-parameters)
						   std::string dispatch,
						   std::optional<detached_symbol_identity> target,
						   std::optional<detached_type_identity> receiver,
						   std::vector<symbol_id> candidates,
						   std::optional<std::string> unresolved_reason)
			{
				auto source = identities_.source(expression.getSourceRange());
				const auto normalized = source ? std::optional{source.value()} : std::nullopt;
				std::ranges::sort(candidates,
								  {},
								  [](const symbol_id& value)
								  {
									  return value.value();
								  });
				const auto duplicate = std::ranges::unique(candidates);
				candidates.erase(duplicate.begin(), duplicate.end());
				auto observation = identities_.observation(
					fact_kind::call, "call:" + kind + ":" + source_key(normalized), normalized);
				observation.payload.emplace("call.kind", kind);
				observation.payload.emplace("call.dispatch", dispatch);
				observation.payload.emplace(
					"call.direct_callee", target ? std::string{target->id.value()} : "unresolved");
				observation.payload.emplace(
					"call.static_target", target ? std::string{target->id.value()} : "unresolved");
				observation.payload.emplace("call.possible_callees", join_ids(candidates));
				observation.payload.emplace("call.receiver_static_type",
											receiver ? std::string{receiver->id.value()} : "none");
				observation.payload.emplace(
					"call.dependent",
					bool_text(expression.isTypeDependent() || expression.isValueDependent()));
				observation.payload.emplace(
					"call.implicit",
					bool_text(normalized &&
							  normalized->origin == source_origin::implicit_compiler_node));
				observation.payload.emplace(
					"call.from_macro",
					bool_text(normalized && normalized->origin != source_origin::directly_spelled));
				if (!operator_spelling_.empty())
				{
					observation.payload.emplace("call.operator", std::move(operator_spelling_));
					operator_spelling_.clear();
				}
				if (caller_ != nullptr)
				{
					auto caller = identities_.symbol(*caller_);
					if (!caller)
						return fail(std::move(caller.error()));
					observation.payload.emplace("call.caller",
												std::string{caller.value().id.value()});
				}
				if (target)
					observation.name = target->name;
				if (receiver)
					observation.type = receiver->type;
				if (unresolved_reason)
					observation.payload.emplace("call.unresolved_reason", *unresolved_reason);
				const auto covered = target.has_value() || kind == "builtin_operator";
				identities_.mark(observation,
								 covered ? coverage_state::covered : coverage_state::unresolved,
								 covered ? std::nullopt : unresolved_reason);
				observations_.push_back(std::move(observation));
				return true;
			}

			bool fail(error failure)
			{
				if (!error_)
					error_ = std::move(failure);
				return false;
			}

			clang::ASTContext& context_;
			semantic_identity_adapter& identities_;
			const clang::FunctionDecl* caller_{};
			std::vector<facts::observation_record> observations_;
			std::optional<error> error_;
			std::string operator_spelling_;
		};
#endif
	} // namespace

	result<std::vector<facts::observation_record>>
	extract_call_relation_observations(clang::ASTContext& context,
									   semantic_identity_adapter& identities)
	{
#if CXXLENS_HAS_CLANG22
		relation_visitor visitor{context, identities};
		if (!visitor.TraverseDecl(context.getTranslationUnitDecl()))
		{
			if (auto failure = visitor.take_error())
				return std::move(*failure);
			return relation_error("call-relation-traversal-failed");
		}
		return visitor.take();
#else
		(void)context;
		(void)identities;
		return relation_error("clang22-not-linked");
#endif
	}
} // namespace cxxlens::detail::clang22
