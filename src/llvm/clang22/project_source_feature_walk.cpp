#include "project_source_feature_walk.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>

namespace cxxlens::detail::clang22
{
	namespace
	{
		std::string_view attribute_kind_name(clang::attr::Kind kind) noexcept
		{
			switch (kind)
			{
#define ATTR(NAME) \
	case clang::attr::NAME: \
		return #NAME;
#include <clang/Basic/AttrList.inc>
			}
			return {};
		}

		class source_feature_walk final : public clang::RecursiveASTVisitor<source_feature_walk>
		{
			using base = clang::RecursiveASTVisitor<source_feature_walk>;

		  public:
			source_feature_walk(source_feature_observer observer,
								void* observer_context,
								source_feature_walk_limits limits)
				: observer_{observer}, observer_context_{observer_context}, limits_{limits}
			{
			}

			bool shouldVisitImplicitCode() const
			{
				return true;
			}
			bool shouldVisitTemplateInstantiations() const
			{
				return true;
			}

			bool TraverseDecl(clang::Decl* declaration)
			{
				if (!declaration)
					return true;
				if (!enter())
					return false;
				const auto* previous = declaration_context_;
				declaration_context_ = declaration;
				const bool success = base::TraverseDecl(declaration);
				declaration_context_ = previous;
				--depth_;
				return success && result_.frontier.empty();
			}

			bool TraverseType(clang::QualType type, bool qualifier = true)
			{
				if (type.isNull())
					return true;
				if (!enter())
					return false;
				const bool success = base::TraverseType(type, qualifier);
				--depth_;
				return success && result_.frontier.empty();
			}

			bool TraverseTypeLoc(clang::TypeLoc type, bool qualifier = true)
			{
				if (type.isNull())
					return true;
				if (!enter())
					return false;
				original_source_feature_view feature{
					source_feature_class::type_location,
					static_cast<std::uint32_t>(type.getTypeLocClass()),
					type.getTypeLocClass() == clang::TypeLoc::Qualified
						? std::string_view{"Qualified"}
						: std::string_view{type.getTypePtr()->getTypeClassName()},
					type.getSourceRange(),
					declaration_context_,
					nullptr,
					nullptr,
					nullptr,
					type};
				const bool success = admit(feature) && base::TraverseTypeLoc(type, qualifier);
				--depth_;
				return success && result_.frontier.empty();
			}

			bool TraverseStmt(clang::Stmt* statement, DataRecursionQueue* queue = nullptr)
			{
				if (!statement)
					return true;
				if (!charge())
					return false;
				if (!queue && limits_.maximum_pending_statements == 0U)
					return stop("statement-queue-limit");
				if (queue && queue->size() >= limits_.maximum_pending_statements)
					return stop("statement-queue-limit");
				return base::TraverseStmt(statement, queue) && result_.frontier.empty();
			}

			bool VisitDecl(clang::Decl* declaration)
			{
				// The translation unit is a traversal container, not a source feature.
				if (llvm::isa<clang::TranslationUnitDecl>(declaration))
					return true;
				if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(declaration);
					function && (function->hasSkippedBody() || function->isLateTemplateParsed()))
					++result_.deferred_constructs;
				if (const auto* parameter = llvm::dyn_cast<clang::ParmVarDecl>(declaration);
					parameter && parameter->hasUnparsedDefaultArg())
					++result_.deferred_constructs;
				return admit({source_feature_class::declaration,
							  static_cast<std::uint32_t>(declaration->getKind()),
							  declaration->getDeclKindName(),
							  declaration->getSourceRange(),
							  declaration_context_,
							  declaration});
			}

			bool VisitStmt(clang::Stmt* statement)
			{
				return admit({source_feature_class::statement,
							  static_cast<std::uint32_t>(statement->getStmtClass()),
							  statement->getStmtClassName(),
							  statement->getSourceRange(),
							  declaration_context_,
							  nullptr,
							  statement});
			}

			bool VisitAttr(clang::Attr* attribute)
			{
				return admit({source_feature_class::attribute,
							  static_cast<std::uint32_t>(attribute->getKind()),
							  attribute_kind_name(attribute->getKind()),
							  attribute->getRange(),
							  declaration_context_,
							  nullptr,
							  nullptr,
							  attribute});
			}

			source_feature_walk_result run(clang::ASTContext& context)
			{
				if (!observer_)
				{
					stop("observer-unavailable");
					return result_;
				}
				const bool walked = TraverseDecl(context.getTranslationUnitDecl());
				result_.completed = walked && result_.frontier.empty() &&
					result_.deferred_constructs == 0U &&
					!context.getDiagnostics().hasErrorOccurred();
				if (!result_.completed && result_.frontier.empty())
					result_.frontier = result_.deferred_constructs
						? "deferred-original-construct"
						: "original-parse-or-traversal-frontier";
				return result_;
			}

		  private:
			bool stop(std::string_view reason)
			{
				if (result_.frontier.empty())
					result_.frontier = reason;
				return false;
			}
			bool charge()
			{
				if (!result_.frontier.empty())
					return false;
				if (limits_.cancelled && limits_.cancelled(limits_.cancellation_context))
					return stop("cancelled");
				if (result_.operations >= limits_.maximum_operations)
					return stop("work-limit");
				if (limits_.try_charge_operation &&
					!limits_.try_charge_operation(limits_.operation_context))
					return stop("enclosing-work-limit");
				++result_.operations;
				return true;
			}
			bool enter()
			{
				if (!charge())
					return false;
				if (depth_ >= limits_.maximum_depth)
					return stop("depth-limit");
				++depth_;
				return true;
			}
			bool admit(original_source_feature_view feature)
			{
				if (!charge())
					return false;
				if (result_.members >= limits_.maximum_members)
					return stop("member-limit");
				feature.ordinal = result_.members++;
				return observer_(observer_context_, feature) || stop("observer-stopped");
			}

			source_feature_observer observer_;
			void* observer_context_;
			source_feature_walk_limits limits_;
			source_feature_walk_result result_;
			const clang::Decl* declaration_context_{};
			std::size_t depth_{};
		};
	} // namespace

	source_feature_walk_result walk_original_source_features(clang::ASTContext& context,
															 source_feature_observer observer,
															 void* observer_context,
															 source_feature_walk_limits limits)
	{
		return source_feature_walk{observer, observer_context, limits}.run(context);
	}
} // namespace cxxlens::detail::clang22
