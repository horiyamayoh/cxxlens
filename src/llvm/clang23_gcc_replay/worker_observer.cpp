#include "worker_observer.hpp"

#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/Linkage.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <clang/UnifiedSymbolResolution/USRGeneration.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>

namespace cxxlens::detail::clang23_gcc_replay
{
	namespace
	{
		constexpr std::string_view synthetic_prefix{"/__cxxlens_gcc_replay__/"};
		constexpr std::string_view logical_prefix{"project://"};
		constexpr std::size_t maximum_clang_text_bytes{1024U * 1024U};

		[[nodiscard]] sdk::error failure(std::string field, std::string detail)
		{
			return {"application-analysis.replay-observation-failed",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::error resource_failure(std::string detail)
		{
			return {"application-analysis.replay-observation-resource-limit",
					"translation_unit",
					std::move(detail)};
		}

		class budget final
		{
		  public:
			explicit budget(observer_limits limits) : limits_{limits} {}

			[[nodiscard]] sdk::result<void> traverse()
			{
				if (traversal_entries_ >= limits_.maximum_traversal_entries)
					return sdk::unexpected(resource_failure("traversal-entries"));
				++traversal_entries_;
				return {};
			}

			[[nodiscard]] sdk::result<void> observe(const std::size_t logical_bytes)
			{
				if (observations_ >= limits_.maximum_observations)
					return sdk::unexpected(resource_failure("observations"));
				if (logical_bytes > limits_.maximum_logical_bytes - logical_bytes_)
					return sdk::unexpected(resource_failure("logical-bytes"));
				++observations_;
				logical_bytes_ += logical_bytes;
				return {};
			}

			[[nodiscard]] sdk::result<void> enter_depth()
			{
				if (traversal_depth_ >= limits_.maximum_traversal_depth)
					return sdk::unexpected(resource_failure("traversal-depth"));
				++traversal_depth_;
				return {};
			}

			void leave_depth() noexcept
			{
				if (traversal_depth_ != 0U)
					--traversal_depth_;
			}

			[[nodiscard]] std::size_t traversal_entries() const noexcept
			{
				return traversal_entries_;
			}

		  private:
			observer_limits limits_;
			std::size_t observations_{};
			std::size_t traversal_entries_{};
			std::size_t traversal_depth_{};
			std::size_t logical_bytes_{};
		};

		[[nodiscard]] std::optional<observed_source_span>
		span_for(const clang::SourceManager& sources,
				 const clang::LangOptions& language,
				 const clang::SourceRange range)
		{
			if (range.isInvalid())
				return std::nullopt;
			const bool macro = range.getBegin().isMacroID() || range.getEnd().isMacroID();
			const auto begin = sources.getSpellingLoc(range.getBegin());
			const auto last = sources.getSpellingLoc(range.getEnd());
			if (begin.isInvalid() || last.isInvalid() ||
				sources.getFileID(begin) != sources.getFileID(last))
				return std::nullopt;
			const auto filename = sources.getFilename(begin);
			if (!filename.starts_with(synthetic_prefix))
				return std::nullopt;
			const auto after = clang::Lexer::getLocForEndOfToken(last, 0U, sources, language);
			if (after.isInvalid() || sources.getFileID(after) != sources.getFileID(begin))
				return std::nullopt;
			observed_source_span output;
			output.logical_path =
				std::string{logical_prefix} + filename.drop_front(synthetic_prefix.size()).str();
			output.begin = sources.getFileOffset(begin);
			output.end = sources.getFileOffset(after);
			output.macro_spelling = macro;
			if (output.end < output.begin)
				return std::nullopt;
			return output;
		}

		[[nodiscard]] std::optional<std::string> provider_local_key(const clang::NamedDecl& value)
		{
			llvm::SmallString<256U> storage;
			if (clang::index::generateUSRForDecl(value.getCanonicalDecl(), storage) ||
				storage.empty() || storage.size() > maximum_clang_text_bytes)
				return std::nullopt;
			return std::string{"clang23-usr:"} + storage.str().str();
		}

		[[nodiscard]] std::string declaration_kind(const clang::FunctionDecl& value)
		{
			if (llvm::isa<clang::CXXConstructorDecl>(value))
				return "constructor";
			if (llvm::isa<clang::CXXDestructorDecl>(value))
				return "destructor";
			if (llvm::isa<clang::CXXConversionDecl>(value))
				return "conversion-function";
			if (llvm::isa<clang::CXXMethodDecl>(value))
				return "method";
			return "function";
		}

		[[nodiscard]] std::string storage_class(const clang::FunctionDecl& value)
		{
			switch (value.getStorageClass())
			{
				case clang::SC_None:
					return "none";
				case clang::SC_Extern:
					return "extern";
				case clang::SC_Static:
					return "static";
				case clang::SC_PrivateExtern:
					return "private-extern";
				case clang::SC_Auto:
					return "auto";
				case clang::SC_Register:
					return "register";
			}
			return "unknown";
		}

		[[nodiscard]] std::string linkage(const clang::FunctionDecl& value)
		{
			switch (value.getFormalLinkage())
			{
				case clang::Linkage::Invalid:
					return "invalid";
				case clang::Linkage::None:
					return "none";
				case clang::Linkage::Internal:
					return "internal";
				case clang::Linkage::UniqueExternal:
					return "unique-external";
				case clang::Linkage::VisibleNone:
					return "visible-none";
				case clang::Linkage::Module:
					return "module";
				case clang::Linkage::External:
					return "external";
			}
			return "invalid";
		}

		[[nodiscard]] std::string call_kind(const clang::CallExpr& value)
		{
			const auto* callee = value.getDirectCallee();
			if (callee == nullptr)
				return "unresolved";
			const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(callee);
			if (method == nullptr)
				return callee->isOverloadedOperator() ? "operator" : "direct_function";
			return callee->isOverloadedOperator() ? "operator" : "direct_member";
		}

		class visitor final : public clang::RecursiveASTVisitor<visitor>
		{
			using base = clang::RecursiveASTVisitor<visitor>;

		  public:
			visitor(clang::ASTContext& context, observation_batch& output, budget& bounds)
				: context_{context}, output_{output}, bounds_{bounds}
			{
			}

			bool VisitDecl(clang::Decl*)
			{
				return accept(bounds_.traverse());
			}

			bool TraverseDecl(clang::Decl* value)
			{
				if (value == nullptr)
					return true;
				return with_depth(
					[&]()
					{
						return base::TraverseDecl(value);
					});
			}

			bool TraverseType(clang::QualType value, const bool qualifiers = true)
			{
				if (value.isNull())
					return true;
				return with_depth(
					[&]()
					{
						return base::TraverseType(value, qualifiers);
					});
			}

			bool TraverseTypeLoc(clang::TypeLoc value, const bool qualifiers = true)
			{
				if (value.isNull())
					return true;
				return with_depth(
					[&]()
					{
						return base::TraverseTypeLoc(value, qualifiers);
					});
			}

			bool VisitStmt(clang::Stmt*)
			{
				return accept(bounds_.traverse());
			}

			bool VisitType(clang::Type*)
			{
				return accept(bounds_.traverse());
			}

			bool TraverseFunctionDecl(clang::FunctionDecl* value)
			{
				if (value == nullptr)
					return true;
				auto previous = std::move(current_function_);
				if (auto key = provider_local_key(*value))
					current_function_ = std::move(*key);
				else
					current_function_.clear();
				const auto result = base::TraverseFunctionDecl(value);
				current_function_ = std::move(previous);
				return result;
			}

			bool VisitFunctionDecl(clang::FunctionDecl* value)
			{
				if (value == nullptr || value->isImplicit())
					return true;
				auto source = span_for(
					context_.getSourceManager(), context_.getLangOpts(), value->getSourceRange());
				if (!source)
					return true;
				auto key = provider_local_key(*value);
				if (!key)
					return limitation("function-usr-unavailable:" + source->logical_path);

				clang::PrintingPolicy policy{context_.getLangOpts()};
				const auto type = value->getType().getCanonicalType().getAsString(policy);
				const auto name = value->getQualifiedNameAsString();
				if (type.size() > maximum_clang_text_bytes ||
					name.size() > maximum_clang_text_bytes)
					return reject(resource_failure("clang-text"));

				observed_entity entity{*key,
									   declaration_kind(*value),
									   name,
									   type,
									   *source,
									   value->isThisDeclarationADefinition()};
				if (!accept(bounds_.observe(entity.provider_local_key.size() + entity.kind.size() +
											entity.qualified_name.size() +
											entity.canonical_type.size() +
											entity.source.logical_path.size())))
					return false;
				output_.entities.push_back(std::move(entity));

				observed_declaration declaration{*key,
												 declaration_kind(*value),
												 storage_class(*value),
												 linkage(*value),
												 *source,
												 value->isDeleted(),
												 value->isExplicitlyDefaulted()};
				if (!accept(bounds_.observe(declaration.entity_provider_local_key.size() +
											declaration.kind.size() + declaration.storage.size() +
											declaration.linkage.size() +
											declaration.source.logical_path.size())))
					return false;
				output_.declarations.push_back(std::move(declaration));

				auto type_key = sdk::semantic_digest(
					"clang23.gcc-replay.function-type-observation.v1", *key + "\n" + type);
				if (!type_key)
					return reject(std::move(type_key.error()));
				observed_type observed{std::move(*type_key),
									   *key,
									   "function",
									   type,
									   value->getType()->isDependentType()};
				if (!accept(bounds_.observe(observed.provider_local_key.size() +
											observed.owning_entity_provider_local_key.size() +
											observed.constructor.size() +
											observed.canonical_spelling.size())))
					return false;
				output_.types.push_back(std::move(observed));
				return true;
			}

			bool VisitCallExpr(clang::CallExpr* value)
			{
				if (value == nullptr || value->getDirectCallee() == nullptr)
					return true;
				auto source = span_for(
					context_.getSourceManager(), context_.getLangOpts(), value->getSourceRange());
				if (!source)
					return true;
				auto target = provider_local_key(*value->getDirectCallee());
				if (!target)
					return limitation("direct-callee-usr-unavailable:" + source->logical_path);
				observed_direct_call call;
				if (!current_function_.empty())
					call.caller_provider_local_key = current_function_;
				call.target_provider_local_key = std::move(*target);
				call.kind = call_kind(*value);
				call.source = std::move(*source);
				const auto caller_bytes =
					call.caller_provider_local_key ? call.caller_provider_local_key->size() : 0U;
				if (!accept(bounds_.observe(caller_bytes + call.target_provider_local_key.size() +
											call.kind.size() + call.source.logical_path.size())))
					return false;
				output_.direct_calls.push_back(std::move(call));
				return true;
			}

			[[nodiscard]] std::optional<sdk::error> error() &&
			{
				return std::move(error_);
			}

		  private:
			template <class callable>
			bool with_depth(callable&& operation)
			{
				if (!accept(bounds_.enter_depth()))
					return false;
				const auto result = std::forward<callable>(operation)();
				bounds_.leave_depth();
				return result;
			}

			bool accept(sdk::result<void> result)
			{
				if (result)
					return true;
				return reject(std::move(result.error()));
			}

			bool reject(sdk::error value)
			{
				if (!error_)
					error_ = std::move(value);
				return false;
			}

			bool limitation(std::string value)
			{
				if (!accept(bounds_.observe(value.size())))
					return false;
				output_.limitations.push_back(std::move(value));
				return true;
			}

			clang::ASTContext& context_;
			observation_batch& output_;
			budget& bounds_;
			std::string current_function_;
			std::optional<sdk::error> error_;
		};
	} // namespace

	sdk::result<void> observer_limits::validate() const
	{
		if (maximum_observations == 0U ||
			maximum_observations > observer_product_maximum_observations)
			return sdk::unexpected(failure("maximum_observations", "outside-product-bound"));
		if (maximum_traversal_entries == 0U ||
			maximum_traversal_entries > observer_product_maximum_traversal_entries)
			return sdk::unexpected(failure("maximum_traversal_entries", "outside-product-bound"));
		if (maximum_traversal_depth == 0U ||
			maximum_traversal_depth > observer_product_maximum_traversal_depth)
			return sdk::unexpected(failure("maximum_traversal_depth", "outside-product-bound"));
		if (maximum_logical_bytes == 0U ||
			maximum_logical_bytes > observer_product_maximum_logical_bytes)
			return sdk::unexpected(failure("maximum_logical_bytes", "outside-product-bound"));
		return {};
	}

	sdk::result<observation_batch> observe_translation_unit(clang::ASTContext& context,
															const observer_limits limits)
	{
		try
		{
			if (auto valid = limits.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			observation_batch output;
			budget bounds{limits};
			visitor observer{context, output, bounds};
			if (!observer.TraverseDecl(context.getTranslationUnitDecl()))
			{
				auto error = std::move(observer).error();
				return sdk::unexpected(error ? std::move(*error)
											 : failure("translation_unit", "traversal-aborted"));
			}
			output.traversal_entries = bounds.traversal_entries();
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(resource_failure("allocation"));
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(resource_failure("length"));
		}
	}
} // namespace cxxlens::detail::clang23_gcc_replay
