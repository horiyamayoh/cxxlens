#include "worker_observer.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
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
		constexpr std::size_t maximum_macro_origin_depth{128U};
		constexpr std::size_t maximum_macro_origins{256U};

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

		[[nodiscard]] sdk::error type_unavailable(std::string detail)
		{
			return {"application-analysis.replay-type-structure-unavailable",
					"function_type",
					std::move(detail)};
		}

		[[nodiscard]] sdk::result<std::string_view> type_symbol(const std::string_view value)
		{
			return sdk::result<std::string_view>{value};
		}

		[[nodiscard]] sdk::result<std::vector<std::string>>
		qualifier_symbols(clang::Qualifiers qualifiers)
		{
			std::vector<std::string> output;
			if (qualifiers.hasConst())
				output.emplace_back("const");
			if (qualifiers.hasRestrict())
				output.emplace_back("restrict");
			if (qualifiers.hasVolatile())
				output.emplace_back("volatile");
			qualifiers.removeCVRQualifiers();
			if (!qualifiers.empty())
				return sdk::unexpected(type_unavailable("unsupported-qualifier"));
			return output;
		}

		[[nodiscard]] sdk::result<std::string_view> builtin_constructor(const clang::QualType value)
		{
			const auto canonical = value.getCanonicalType().getUnqualifiedType();
			const auto* builtin = llvm::dyn_cast<clang::BuiltinType>(canonical.getTypePtr());
			if (builtin == nullptr)
				return sdk::unexpected(type_unavailable("non-builtin-component"));
			switch (builtin->getKind())
			{
				case clang::BuiltinType::Void:
					return type_symbol("builtin.void");
				case clang::BuiltinType::Bool:
					return type_symbol("builtin.bool");
				case clang::BuiltinType::Char_U:
					return type_symbol("builtin.char-unsigned");
				case clang::BuiltinType::Char_S:
					return type_symbol("builtin.char-signed");
				case clang::BuiltinType::UChar:
					return type_symbol("builtin.unsigned-char");
				case clang::BuiltinType::SChar:
					return type_symbol("builtin.signed-char");
				case clang::BuiltinType::WChar_U:
					return type_symbol("builtin.wchar-unsigned");
				case clang::BuiltinType::WChar_S:
					return type_symbol("builtin.wchar-signed");
				case clang::BuiltinType::Char8:
					return type_symbol("builtin.char8");
				case clang::BuiltinType::Char16:
					return type_symbol("builtin.char16");
				case clang::BuiltinType::Char32:
					return type_symbol("builtin.char32");
				case clang::BuiltinType::UShort:
					return type_symbol("builtin.unsigned-short");
				case clang::BuiltinType::Short:
					return type_symbol("builtin.signed-short");
				case clang::BuiltinType::UInt:
					return type_symbol("builtin.unsigned-int");
				case clang::BuiltinType::Int:
					return type_symbol("builtin.signed-int");
				case clang::BuiltinType::ULong:
					return type_symbol("builtin.unsigned-long");
				case clang::BuiltinType::Long:
					return type_symbol("builtin.signed-long");
				case clang::BuiltinType::ULongLong:
					return type_symbol("builtin.unsigned-long-long");
				case clang::BuiltinType::LongLong:
					return type_symbol("builtin.signed-long-long");
				case clang::BuiltinType::UInt128:
					return type_symbol("builtin.unsigned-int128");
				case clang::BuiltinType::Int128:
					return type_symbol("builtin.signed-int128");
				case clang::BuiltinType::Half:
					return type_symbol("builtin.half");
				case clang::BuiltinType::Float:
					return type_symbol("builtin.float");
				case clang::BuiltinType::Double:
					return type_symbol("builtin.double");
				case clang::BuiltinType::LongDouble:
					return type_symbol("builtin.long-double");
				case clang::BuiltinType::Float16:
					return type_symbol("builtin.float16");
				case clang::BuiltinType::BFloat16:
					return type_symbol("builtin.bfloat16");
				case clang::BuiltinType::Float128:
					return type_symbol("builtin.float128");
				case clang::BuiltinType::Ibm128:
					return type_symbol("builtin.ibm128");
				case clang::BuiltinType::NullPtr:
					return type_symbol("builtin.nullptr");
				default:
					return sdk::unexpected(type_unavailable("unsupported-builtin-component"));
			}
		}

		[[nodiscard]] sdk::result<std::string_view>
		calling_convention(const clang::CallingConv value)
		{
			switch (value)
			{
				case clang::CC_C:
					return type_symbol("c");
				case clang::CC_X86StdCall:
					return type_symbol("x86-stdcall");
				case clang::CC_X86FastCall:
					return type_symbol("x86-fastcall");
				case clang::CC_X86ThisCall:
					return type_symbol("x86-thiscall");
				case clang::CC_X86VectorCall:
					return type_symbol("x86-vectorcall");
				case clang::CC_Win64:
					return type_symbol("win64");
				case clang::CC_X86_64SysV:
					return type_symbol("x86-64-sysv");
				default:
					return sdk::unexpected(type_unavailable("unsupported-calling-convention"));
			}
		}

		[[nodiscard]] sdk::result<std::string_view>
		exception_specification(const clang::ExceptionSpecificationType value)
		{
			switch (value)
			{
				case clang::EST_None:
					return type_symbol("none");
				case clang::EST_DynamicNone:
					return type_symbol("dynamic-none");
				case clang::EST_NoThrow:
					return type_symbol("ms-nothrow");
				case clang::EST_BasicNoexcept:
					return type_symbol("noexcept");
				case clang::EST_NoexceptFalse:
					return type_symbol("noexcept-false");
				case clang::EST_NoexceptTrue:
					return type_symbol("noexcept-true");
				default:
					return sdk::unexpected(type_unavailable("unsupported-exception-specification"));
			}
		}

		[[nodiscard]] std::string ref_qualifier(const clang::RefQualifierKind value)
		{
			switch (value)
			{
				case clang::RQ_None:
					return "none";
				case clang::RQ_LValue:
					return "lvalue";
				case clang::RQ_RValue:
					return "rvalue";
			}
			return "invalid";
		}

		[[nodiscard]] sdk::result<observed_type::function_structure>
		function_type_structure(const clang::FunctionDecl& value)
		{
			const auto* prototype = value.getType()->getAs<clang::FunctionProtoType>();
			if (prototype == nullptr)
				return sdk::unexpected(type_unavailable("unprototyped-function"));
			if (value.getType()->isDependentType())
				return sdk::unexpected(type_unavailable("dependent-function"));
			auto qualifiers = qualifier_symbols(prototype->getMethodQuals());
			auto convention = calling_convention(prototype->getCallConv());
			auto exceptions = exception_specification(prototype->getExceptionSpecType());
			if (!qualifiers || !convention || !exceptions)
				return sdk::unexpected(!qualifiers		 ? std::move(qualifiers.error())
										   : !convention ? std::move(convention.error())
														 : std::move(exceptions.error()));

			observed_type::function_structure output;
			output.qualifiers = std::move(*qualifiers);
			output.calling_convention = *convention;
			output.exception_specification = *exceptions;
			output.ref_qualifier = ref_qualifier(prototype->getRefQualifier());
			output.variadic = prototype->isVariadic();
			const auto append_component = [&](const std::string_view role,
											  const std::uint64_t ordinal,
											  const clang::QualType type) -> sdk::result<void>
			{
				auto component_qualifiers =
					qualifier_symbols(type.getCanonicalType().getQualifiers());
				auto constructor = builtin_constructor(type);
				if (!component_qualifiers || !constructor)
					return sdk::unexpected(!component_qualifiers
											   ? std::move(component_qualifiers.error())
											   : std::move(constructor.error()));
				output.components.push_back({std::string{role},
											 ordinal,
											 std::string{*constructor},
											 std::move(*component_qualifiers)});
				return {};
			};
			if (auto result = append_component("result", 0U, prototype->getReturnType()); !result)
				return sdk::unexpected(std::move(result.error()));
			std::uint64_t ordinal{};
			for (const auto parameter : prototype->param_types())
			{
				if (auto result = append_component("parameter", ordinal++, parameter); !result)
					return sdk::unexpected(std::move(result.error()));
			}
			return output;
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
			output.role = "spelling";
			if (output.end < output.begin)
				return std::nullopt;
			return output;
		}

		struct call_source_attachment
		{
			observed_source_span primary;
			std::vector<observed_source_origin> origins;
		};

		[[nodiscard]] sdk::result<std::string>
		logical_path_for(const clang::SourceManager& sources, const clang::SourceLocation location)
		{
			if (location.isInvalid())
				return sdk::unexpected(failure("call.source", "invalid-location"));
			const auto filename = sources.getFilename(location);
			if (!filename.starts_with(synthetic_prefix))
				return sdk::unexpected(failure("call.source", "outside-replay-vfs"));
			if (filename.size() - synthetic_prefix.size() > maximum_clang_text_bytes)
				return sdk::unexpected(resource_failure("clang-text"));
			return std::string{logical_prefix} + filename.drop_front(synthetic_prefix.size()).str();
		}

		[[nodiscard]] sdk::result<call_source_attachment>
		call_source_for(const clang::SourceManager& sources,
						const clang::LangOptions& language,
						const clang::SourceRange range)
		{
			if (range.isInvalid())
				return sdk::unexpected(failure("call.source", "invalid-range"));
			const auto expansion = sources.getExpansionRange(range);
			const auto begin = sources.getExpansionLoc(expansion.getBegin());
			auto end = sources.getExpansionLoc(expansion.getEnd());
			if (expansion.isTokenRange())
				end = clang::Lexer::getLocForEndOfToken(end, 0U, sources, language);
			if (begin.isInvalid() || end.isInvalid() || !sources.isWrittenInSameFile(begin, end))
				return sdk::unexpected(failure("call.source", "expansion-range"));
			auto primary_path = logical_path_for(sources, begin);
			if (!primary_path)
				return sdk::unexpected(std::move(primary_path.error()));
			call_source_attachment output;
			output.primary = {std::move(*primary_path),
							  sources.getFileOffset(begin),
							  sources.getFileOffset(end),
							  "expansion"};
			if (output.primary.end < output.primary.begin)
				return sdk::unexpected(failure("call.source", "expansion-offset"));

			std::size_t origin_bytes{};
			auto origin_begin = range.getBegin();
			auto origin_end = range.getEnd();
			for (std::size_t depth{}; depth < maximum_macro_origin_depth &&
				 (origin_begin.isMacroID() || origin_end.isMacroID());
				 ++depth)
			{
				const auto spelling_begin = sources.getSpellingLoc(origin_begin);
				const auto spelling_end_token = sources.getSpellingLoc(origin_end);
				const auto spelling_end =
					clang::Lexer::getLocForEndOfToken(spelling_end_token, 0U, sources, language);
				if (spelling_begin.isInvalid() || spelling_end.isInvalid())
					return sdk::unexpected(failure("call.origin", "range"));
				auto append = [&](std::string kind,
								  const clang::SourceLocation origin_start,
								  const clang::SourceLocation origin_finish) -> sdk::result<void>
				{
					if (output.origins.size() >= maximum_macro_origins)
						return sdk::unexpected(resource_failure("macro-origins"));
					auto path = logical_path_for(sources, origin_start);
					if (!path)
						return sdk::unexpected(std::move(path.error()));
					if (kind.size() > observer_product_maximum_logical_bytes - origin_bytes ||
						path->size() >
							observer_product_maximum_logical_bytes - origin_bytes - kind.size())
						return sdk::unexpected(resource_failure("macro-origin-bytes"));
					origin_bytes += kind.size() + path->size();
					const auto origin_start_offset = sources.getFileOffset(origin_start);
					const auto origin_finish_offset = sources.getFileOffset(origin_finish);
					if (origin_finish_offset < origin_start_offset)
						return sdk::unexpected(failure("call.origin", "offset"));
					output.origins.push_back({std::move(kind),
											  std::move(*path),
											  origin_start_offset,
											  origin_finish_offset,
											  true});
					return {};
				};
				if (sources.isWrittenInSameFile(spelling_begin, spelling_end))
				{
					if (auto appended = append("macro-spelling", spelling_begin, spelling_end);
						!appended)
						return sdk::unexpected(std::move(appended.error()));
				}
				else
				{
					const auto begin_end =
						clang::Lexer::getLocForEndOfToken(spelling_begin, 0U, sources, language);
					if (begin_end.isInvalid())
						return sdk::unexpected(failure("call.origin", "begin-token"));
					if (auto appended = append("macro-spelling-begin", spelling_begin, begin_end);
						!appended)
						return sdk::unexpected(std::move(appended.error()));
					if (auto appended =
							append("macro-spelling-end", spelling_end_token, spelling_end);
						!appended)
						return sdk::unexpected(std::move(appended.error()));
				}
				const auto next_begin = origin_begin.isMacroID()
					? sources.getImmediateExpansionRange(origin_begin).getBegin()
					: origin_begin;
				const auto next_end = origin_end.isMacroID()
					? sources.getImmediateExpansionRange(origin_end).getEnd()
					: origin_end;
				if (next_begin == origin_begin && next_end == origin_end)
					break;
				origin_begin = next_begin;
				origin_end = next_end;
			}
			if (origin_begin.isMacroID() || origin_end.isMacroID())
				return sdk::unexpected(resource_failure("macro-origin-depth"));
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

		[[nodiscard]] sdk::result<std::vector<std::string>>
		declaration_attributes(const clang::FunctionDecl& value)
		{
			std::vector<std::string> output;
			output.reserve(value.getAttrs().size());
			for (const auto* attribute : value.attrs())
			{
				if (attribute == nullptr)
					continue;
				const auto* spelling = attribute->getSpelling();
				if (spelling == nullptr || *spelling == '\0')
					return sdk::unexpected(
						failure("declaration.attribute", "spelling-unavailable"));
				std::string detached{spelling};
				if (detached.size() > maximum_clang_text_bytes)
					return sdk::unexpected(resource_failure("clang-text"));
				output.push_back(std::move(detached));
			}
			std::ranges::sort(output);
			output.erase(std::ranges::unique(output).begin(), output.end());
			return output;
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

				auto attributes = declaration_attributes(*value);
				if (!attributes)
					return reject(std::move(attributes.error()));
				observed_declaration declaration{*key,
												 declaration_kind(*value),
												 storage_class(*value),
												 linkage(*value),
												 std::move(*attributes),
												 *source,
												 value->isImplicit(),
												 value->isDeleted(),
												 value->isExplicitlyDefaulted(),
												 value->getFriendObjectKind() !=
													 clang::Decl::FOK_None,
												 value->isInExportDeclContext()};
				std::size_t attribute_bytes{};
				for (const auto& attribute : declaration.attributes)
				{
					if (attribute.size() >
						std::numeric_limits<std::size_t>::max() - attribute_bytes)
						return reject(resource_failure("logical-bytes"));
					attribute_bytes += attribute.size();
				}
				if (!accept(bounds_.observe(declaration.entity_provider_local_key.size() +
											declaration.kind.size() + declaration.storage.size() +
											declaration.linkage.size() + attribute_bytes +
											declaration.source.logical_path.size())))
					return false;
				output_.declarations.push_back(std::move(declaration));

				auto type_key =
					sdk::semantic_digest("clang23.gcc-replay.function-type-observation.v2", *key);
				if (!type_key)
					return reject(std::move(type_key.error()));
				observed_type observed{std::move(*type_key),
									   *key,
									   "function",
									   type,
									   value->getType()->isDependentType(),
									   std::nullopt,
									   std::nullopt};
				auto structure = function_type_structure(*value);
				if (structure)
					observed.structure = std::move(*structure);
				else
					observed.unavailable_reason = std::move(structure.error().detail);
				std::size_t structural_bytes{};
				const auto add_structural_bytes = [&](const std::size_t bytes)
				{
					if (bytes > std::numeric_limits<std::size_t>::max() - structural_bytes)
						return false;
					structural_bytes += bytes;
					return true;
				};
				if (observed.structure)
				{
					for (const auto& qualifier : observed.structure->qualifiers)
						if (!add_structural_bytes(qualifier.size()))
							return reject(resource_failure("logical-bytes"));
					for (const auto& component : observed.structure->components)
					{
						if (!add_structural_bytes(component.role.size()) ||
							!add_structural_bytes(component.constructor.size()))
							return reject(resource_failure("logical-bytes"));
						for (const auto& qualifier : component.qualifiers)
							if (!add_structural_bytes(qualifier.size()))
								return reject(resource_failure("logical-bytes"));
					}
					if (!add_structural_bytes(observed.structure->calling_convention.size()) ||
						!add_structural_bytes(observed.structure->exception_specification.size()) ||
						!add_structural_bytes(observed.structure->ref_qualifier.size()))
						return reject(resource_failure("logical-bytes"));
				}
				else if (!add_structural_bytes(observed.unavailable_reason->size()))
					return reject(resource_failure("logical-bytes"));
				if (!accept(bounds_.observe(observed.provider_local_key.size() +
											observed.owning_entity_provider_local_key.size() +
											observed.constructor.size() +
											observed.canonical_spelling.size() + structural_bytes)))
					return false;
				output_.types.push_back(std::move(observed));
				return true;
			}

			bool VisitCallExpr(clang::CallExpr* value)
			{
				if (value == nullptr || value->getDirectCallee() == nullptr)
					return true;
				auto source = call_source_for(
					context_.getSourceManager(), context_.getLangOpts(), value->getSourceRange());
				if (!source)
					return source.error().code ==
							"application-analysis.replay-observation-resource-limit"
						? reject(std::move(source.error()))
						: limitation("call-source-unavailable:" + source.error().detail);
				auto target = provider_local_key(*value->getDirectCallee());
				if (!target)
					return limitation("direct-callee-usr-unavailable:" +
									  source->primary.logical_path);
				observed_direct_call call;
				if (!current_function_.empty())
					call.caller_provider_local_key = current_function_;
				call.target_provider_local_key = std::move(*target);
				call.kind = call_kind(*value);
				call.source = std::move(source->primary);
				call.origins = std::move(source->origins);
				const auto caller_bytes =
					call.caller_provider_local_key ? call.caller_provider_local_key->size() : 0U;
				if (!accept(bounds_.observe(caller_bytes + call.target_provider_local_key.size() +
											call.kind.size() + call.source.logical_path.size())))
					return false;
				for (const auto& origin : call.origins)
					if (!accept(bounds_.observe(origin.kind.size() + origin.logical_path.size())))
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
