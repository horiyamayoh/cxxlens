#include "provider_worker_v4_ast_observer.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <cxxlens/provider/clang22.hpp>

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/raw_ostream.h>
#endif

namespace cxxlens::detail::clang22
{
	namespace
	{
		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool valid_text(const std::string_view value) noexcept
		{
			return !value.empty() && value.find('\0') == std::string_view::npos &&
				sdk::validate_utf8_text(value);
		}

		void append_text(std::ostringstream& output, const std::string_view value)
		{
			output << value.size() << ':' << value;
		}

		[[nodiscard]] std::string
		span_canonical(const std::optional<materialization::observation_v2_primary_span>& span)
		{
			std::ostringstream output;
			if (!span)
			{
				append_text(output, "absent");
				return output.str();
			}
			append_text(output, "present");
			append_text(output, span->span_id);
			append_text(output, span->snapshot);
			append_text(output, span->file);
			append_text(output, std::to_string(span->begin));
			append_text(output, std::to_string(span->end));
			append_text(output, span->role);
			append_text(output, span->read_only ? "1" : "0");
			return output.str();
		}

		[[nodiscard]] std::string
		origins_canonical(const std::vector<materialization::observation_v2_origin>& origins)
		{
			std::ostringstream output;
			append_text(output, std::to_string(origins.size()));
			for (const auto& origin : origins)
			{
				append_text(output, origin.kind);
				append_text(output, origin.logical_path);
				append_text(output, std::to_string(origin.begin));
				append_text(output, std::to_string(origin.end));
				append_text(output, origin.read_only ? "1" : "0");
			}
			return output.str();
		}

		[[nodiscard]] sdk::result<void>
		validate_span(const materialization::observation_v2_primary_span& span)
		{
			if (!valid_text(span.span_id) || !valid_text(span.snapshot) || !valid_text(span.file) ||
				!valid_text(span.role) || span.end < span.begin)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-observation-invalid", "primary-span"));
			auto expected = sdk::source_span_identity(
				span.snapshot, span.file, span.begin, span.end, span.role);
			if (!expected || *expected != span.span_id)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-observation-invalid", "primary-span-identity"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_origin(const materialization::observation_v2_origin& origin)
		{
			if (!valid_text(origin.kind) || !valid_text(origin.logical_path) || origin.begin < 0 ||
				origin.end < origin.begin || !origin.read_only)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-observation-invalid", "origin"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_task_metadata(const source_closure_task_v4_decoded& metadata,
							   const std::string_view compile_unit,
							   const source_closure_member*& main)
		{
			if (!valid_text(compile_unit))
				return sdk::unexpected(
					failure("provider-worker-v4.ast-input-invalid", "compile-unit"));
			if (auto valid = metadata.input.closure.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			auto identity = derive_source_closure_task_v4_identity(metadata.input);
			if (!identity || *identity != metadata.identity)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-input-invalid", "task-identity"));
			if (!valid_text(metadata.identity.task_id) ||
				!valid_text(metadata.identity.task_v4_digest))
				return sdk::unexpected(failure("provider-worker-v4.ast-input-invalid", "identity"));
			main = metadata.input.closure.find_member(metadata.input.main_logical_path);
			if (main == nullptr || main->role != source_closure_role::main ||
				main->logical_path != metadata.input.main_logical_path)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-input-invalid", "main-member"));
			return {};
		}

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
		[[nodiscard]] std::optional<std::string>
		source_anchor(provider::clang22::borrowed_translation_unit& unit,
					  const clang::SourceLocation location,
					  const std::string_view source_snapshot,
					  const std::string_view file)
		{
			auto& source = unit.source_manager();
			const auto spelling = source.getSpellingLoc(location);
			if (spelling.isInvalid() || !source.isWrittenInMainFile(spelling))
				return std::nullopt;
			bool invalid{};
			const auto buffer = source.getBufferData(source.getFileID(spelling), &invalid);
			if (invalid)
				return std::nullopt;
			const auto content =
				sdk::content_digest(std::as_bytes(std::span{buffer.data(), buffer.size()}));
			std::ostringstream projection;
			append_text(projection, source_snapshot);
			append_text(projection, file);
			append_text(projection, content);
			append_text(projection, std::to_string(source.getFileOffset(spelling)));
			auto anchor =
				sdk::semantic_digest("clang22.declaration-source-anchor.v1", projection.str());
			return anchor ? std::optional<std::string>{std::move(*anchor)} : std::nullopt;
		}

		[[nodiscard]] std::string declaration_kind(const clang::FunctionDecl& declaration)
		{
			if (llvm::isa<clang::CXXConstructorDecl>(declaration))
				return "constructor";
			if (llvm::isa<clang::CXXDestructorDecl>(declaration))
				return "destructor";
			if (llvm::isa<clang::CXXMethodDecl>(declaration))
				return "method";
			return "function";
		}

		[[nodiscard]] std::string
		declaration_context_identity(provider::clang22::borrowed_translation_unit& unit,
									 const clang::DeclContext& context,
									 const std::string_view source_snapshot,
									 const std::string_view file)
		{
			std::ostringstream output;
			const auto* declaration = clang::Decl::castFromDeclContext(&context);
			if (declaration == nullptr)
				return {};
			append_text(output, declaration->getDeclKindName());
			if (const auto* named = llvm::dyn_cast<clang::NamedDecl>(declaration))
			{
				const auto* canonical = llvm::cast<clang::NamedDecl>(named->getCanonicalDecl());
				append_text(output, canonical->getQualifiedNameAsString());
				if (auto anchor =
						source_anchor(unit, canonical->getLocation(), source_snapshot, file))
					append_text(output, *anchor);
			}
			return output.str();
		}

		[[nodiscard]] std::string template_identity(const clang::FunctionDecl& declaration)
		{
			std::string output =
				std::to_string(static_cast<unsigned>(declaration.getTemplatedKind()));
			if (const auto* arguments = declaration.getTemplateSpecializationArgs())
			{
				clang::PrintingPolicy policy{declaration.getASTContext().getLangOpts()};
				llvm::raw_string_ostream stream{output};
				for (const auto& argument : arguments->asArray())
				{
					stream << ':';
					argument.print(policy, stream, true);
				}
			}
			return output;
		}

		[[nodiscard]] std::string
		constraint_identity(provider::clang22::borrowed_translation_unit& unit,
							const clang::FunctionDecl& declaration)
		{
			const auto& requires_clause = declaration.getTrailingRequiresClause();
			const auto* constraint = requires_clause.ConstraintExpr;
			if (constraint == nullptr)
				return {};
			return clang::Lexer::getSourceText(
					   clang::CharSourceRange::getTokenRange(constraint->getSourceRange()),
					   unit.source_manager(),
					   unit.ast().getLangOpts())
				.str();
		}

		[[nodiscard]] sdk::result<std::pair<std::string, std::string>>
		declaration_identity_for(provider::clang22::borrowed_translation_unit& unit,
								 const clang::FunctionDecl& declaration,
								 const std::string_view toolchain_digest,
								 const std::string_view source_snapshot,
								 const std::string_view file)
		{
			const auto* canonical = declaration.getCanonicalDecl();
			const auto* anchor_declaration = canonical;
			if (!unit.source_manager().isWrittenInMainFile(canonical->getLocation()))
				if (const auto* definition = declaration.getDefinition(); definition != nullptr &&
					unit.source_manager().isWrittenInMainFile(definition->getLocation()))
					anchor_declaration = definition;
			llvm::SmallString<256> storage;
			if (!clang::index::generateUSRForDecl(canonical, storage) && !storage.empty())
				return std::pair<std::string, std::string>{"clang-usr:" + storage.str().str(),
														   "exact-usr"};
			const auto anchor =
				source_anchor(unit, anchor_declaration->getLocation(), source_snapshot, file);
			if (toolchain_digest.empty() || !anchor)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-identity-unresolved", "fallback"));
			std::ostringstream projection;
			append_text(projection, "clang22.declaration-fallback.v2");
			append_text(projection, toolchain_digest);
			append_text(projection, declaration_kind(*canonical));
			append_text(projection, canonical->getQualifiedNameAsString());
			append_text(projection, canonical->getType().getCanonicalType().getAsString());
			append_text(projection, template_identity(*canonical));
			append_text(projection, constraint_identity(unit, *canonical));
			append_text(projection,
						declaration_context_identity(
							unit, *canonical->getDeclContext(), source_snapshot, file));
			append_text(projection, *anchor);
			auto digest = sdk::semantic_digest("clang22.declaration-fallback.v2", projection.str());
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			return std::pair<std::string, std::string>{"clang-fallback:" + *digest,
													   "structural-fallback"};
		}

		[[nodiscard]] std::string call_kind(const clang::CallExpr& expression)
		{
			const auto* direct = expression.getDirectCallee();
			if (direct == nullptr)
			{
				if (expression.isTypeDependent() || expression.isValueDependent())
					return "dependent";
				if (llvm::isa<clang::CXXMemberCallExpr>(expression))
					return "indirect_member_pointer";
				return "indirect_function";
			}
			const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(direct);
			if (method == nullptr)
				return direct->isOverloadedOperator() ? "operator" : "direct_function";
			const auto* member =
				llvm::dyn_cast<clang::MemberExpr>(expression.getCallee()->IgnoreParenImpCasts());
			if (method->isVirtual() && (member == nullptr || !member->hasQualifier()))
				return "virtual_member";
			return direct->isOverloadedOperator() ? "operator" : "direct_member";
		}

		class visitor final : public clang::RecursiveASTVisitor<visitor>
		{
		  public:
			visitor(provider::clang22::borrowed_translation_unit& unit,
					provider_worker_v4_ast_observation_batch& output,
					std::string source_snapshot,
					std::string source_file,
					std::string toolchain_digest)
				: unit_{&unit}, output_{&output}, source_snapshot_{std::move(source_snapshot)},
				  source_file_{std::move(source_file)},
				  toolchain_digest_{std::move(toolchain_digest)}
			{
			}

			bool TraverseFunctionDecl(clang::FunctionDecl* declaration)
			{
				if (declaration == nullptr)
					return true;
				auto previous = current_function_;
				auto identity = declaration_identity_for(
					*unit_, *declaration, toolchain_digest_, source_snapshot_, source_file_);
				current_function_ = identity ? identity->first : std::string{};
				const auto traversed =
					clang::RecursiveASTVisitor<visitor>::TraverseFunctionDecl(declaration);
				current_function_ = std::move(previous);
				return traversed;
			}

			bool VisitFunctionDecl(clang::FunctionDecl* declaration)
			{
				if (declaration == nullptr || declaration->isImplicit() ||
					!unit_->source_manager().isWrittenInMainFile(declaration->getLocation()))
					return true;
				auto identity = declaration_identity_for(
					*unit_, *declaration, toolchain_digest_, source_snapshot_, source_file_);
				if (!identity)
				{
					fail(identity.error().code);
					return true;
				}
				provider_worker_v4_ast_observation entity;
				entity.kind = provider_worker_v4_ast_observation_kind::entity;
				entity.compile_unit = output_->compile_unit;
				entity.semantic_key = identity->first;
				entity.payload.emplace("symbol.identity_confidence", identity->second);
				entity.payload.emplace("symbol.kind", declaration_kind(*declaration));
				entity.payload.emplace("symbol.qualified_name",
									   declaration->getQualifiedNameAsString());
				entity.payload.emplace("symbol.signature",
									   declaration->getType().getCanonicalType().getAsString());
				entity.payload.emplace("symbol.is_definition",
									   declaration->isThisDeclarationADefinition() ? "true"
																				   : "false");
				entity.payload.emplace("symbol.is_canonical_declaration",
									   declaration == declaration->getCanonicalDecl() ? "true"
																					  : "false");
				entity.exact_equivalence = identity->second == "exact-usr";
				if (!entity.exact_equivalence)
					entity.limitation = "identity-confidence:structural-fallback";
				attach_source(entity, declaration->getSourceRange(), "declaration");
				insert(std::move(entity));

				provider_worker_v4_ast_observation type;
				type.kind = provider_worker_v4_ast_observation_kind::type;
				type.compile_unit = output_->compile_unit;
				type.payload.emplace("type.canonical",
									 declaration->getType().getCanonicalType().getAsString());
				type.semantic_key =
					*sdk::semantic_digest("clang22.type.v1", type.payload.at("type.canonical"));
				insert(std::move(type));
				return true;
			}

			bool VisitCallExpr(clang::CallExpr* expression)
			{
				if (expression == nullptr ||
					!unit_->source_manager().isWrittenInMainFile(expression->getExprLoc()))
					return true;
				provider_worker_v4_ast_observation call;
				call.kind = provider_worker_v4_ast_observation_kind::call;
				call.compile_unit = output_->compile_unit;
				call.payload.emplace("call.kind", call_kind(*expression));
				if (!current_function_.empty())
					call.payload.emplace("call.caller", current_function_);
				if (const auto* callee = expression->getDirectCallee(); callee != nullptr)
				{
					const auto* declaration = callee->getDefinition();
					if (declaration == nullptr)
						declaration = callee->getCanonicalDecl();
					auto identity = declaration_identity_for(
						*unit_, *declaration, toolchain_digest_, source_snapshot_, source_file_);
					if (identity)
					{
						call.payload.emplace("call.direct_callee", identity->first);
						call.payload.emplace("call.direct_callee_identity_confidence",
											 identity->second);
						call.exact_equivalence = identity->second == "exact-usr";
						if (!call.exact_equivalence)
							call.limitation = "identity-confidence:structural-fallback";
					}
					else
					{
						fail(identity.error().code);
						call.payload.emplace("call.unresolved_reason",
											 "callee-identity-unavailable");
					}
					call.payload.emplace("call.direct_callee_kind", declaration_kind(*declaration));
					call.payload.emplace("call.direct_callee_signature",
										 declaration->getType().getCanonicalType().getAsString());
					call.payload.emplace("call.direct_callee_qualified_name",
										 declaration->getQualifiedNameAsString());
				}
				else if (call.payload.at("call.kind") == "dependent")
					call.payload.emplace("call.unresolved_reason", "dependent-callee");
				else if (call.payload.at("call.kind") == "indirect_member_pointer")
					call.payload.emplace("call.unresolved_reason",
										 "member-pointer-target-not-modeled");
				else
					call.payload.emplace("call.unresolved_reason",
										 "function-pointer-target-not-modeled");
				attach_source(call, expression->getSourceRange(), "expression");
				if (call.primary_span)
				{
					std::ostringstream identity;
					append_text(identity, current_function_);
					append_text(identity, call.primary_span->span_id);
					append_text(identity,
								call.payload.contains("call.direct_callee")
									? call.payload.at("call.direct_callee")
									: std::string_view{});
					call.semantic_key = *sdk::semantic_digest("clang22.call.v1", identity.str());
				}
				else
				{
					std::ostringstream identity;
					append_text(identity, current_function_);
					append_text(identity, std::to_string(unavailable_call_index_++));
					for (const auto& [key, value] : call.payload)
					{
						append_text(identity, key);
						append_text(identity, value);
					}
					call.semantic_key =
						*sdk::semantic_digest("clang22.call-source-unavailable.v1", identity.str());
				}
				insert(std::move(call));
				return true;
			}

		  private:
			void fail(const std::string_view code)
			{
				++output_->failed_count;
				output_->diagnostics.emplace_back(code);
			}

			void attach_source(provider_worker_v4_ast_observation& observation,
							   const clang::SourceRange& range,
							   const std::string_view role)
			{
				auto source = provider::clang22::normalize_source(
					*unit_, range, {source_snapshot_, source_file_, std::string{role}});
				if (!source)
				{
					fail(source.error().code);
					observation.exact_equivalence = false;
					observation.limitation = "source-span-unavailable:" + source.error().code;
					return;
				}
				observation.primary_span = materialization::observation_v2_primary_span{
					source->id,
					source->source_snapshot,
					source->file,
					source->begin,
					source->end,
					std::string{role},
					source->read_only,
				};
				for (const auto& origin : source->origin_chain)
					observation.origins.push_back({origin.kind,
												   origin.logical_path,
												   static_cast<std::int64_t>(origin.begin),
												   static_cast<std::int64_t>(origin.end),
												   origin.read_only});
			}

			void insert(provider_worker_v4_ast_observation observation)
			{
				const auto key = observation.canonical_form();
				if (seen_.insert(key).second)
					output_->observations.push_back(std::move(observation));
			}

			provider::clang22::borrowed_translation_unit* unit_;
			provider_worker_v4_ast_observation_batch* output_;
			std::string source_snapshot_;
			std::string source_file_;
			std::string toolchain_digest_;
			std::string current_function_;
			std::set<std::string, std::less<>> seen_;
			std::uint64_t unavailable_call_index_{};
		};
#endif
	} // namespace

	sdk::result<void> provider_worker_v4_ast_observation::validate() const
	{
		if (!valid_text(compile_unit) || !valid_text(semantic_key))
			return sdk::unexpected(
				failure("provider-worker-v4.ast-observation-invalid", "identity"));
		if (kind == provider_worker_v4_ast_observation_kind::type &&
			(primary_span || !origins.empty()))
			return sdk::unexpected(
				failure("provider-worker-v4.ast-observation-invalid", "type-source-authority"));
		if (primary_span)
		{
			if (auto valid = validate_span(*primary_span); !valid)
				return valid;
		}
		for (const auto& origin : origins)
			if (auto valid = validate_origin(origin); !valid)
				return valid;
		if (limitation && (!valid_text(*limitation) || exact_equivalence))
			return sdk::unexpected(
				failure("provider-worker-v4.ast-observation-invalid", "limitation"));
		if (!limitation && !exact_equivalence)
			return sdk::unexpected(
				failure("provider-worker-v4.ast-observation-invalid", "exact-equivalence"));
		for (const auto& [key, value] : payload)
			if (!valid_text(key) || value.find('\0') != std::string::npos ||
				!sdk::validate_utf8_text(value))
				return sdk::unexpected(
					failure("provider-worker-v4.ast-observation-invalid", "payload"));
		return {};
	}

	std::string provider_worker_v4_ast_observation::canonical_form() const
	{
		std::ostringstream output;
		append_text(output, "cxxlens.clang22.task-v4.ast-observation.v1");
		append_text(output, std::to_string(static_cast<unsigned>(kind)));
		append_text(output, compile_unit);
		append_text(output, semantic_key);
		append_text(output, std::to_string(payload.size()));
		for (const auto& [key, value] : payload)
		{
			append_text(output, key);
			append_text(output, value);
		}
		append_text(output, span_canonical(primary_span));
		append_text(output, origins_canonical(origins));
		append_text(output, exact_equivalence ? "exact" : "limited");
		append_text(output, limitation ? std::string_view{*limitation} : std::string_view{});
		return output.str();
	}

	sdk::result<void> provider_worker_v4_ast_observation_batch::validate() const
	{
		if (!valid_text(task_id) || !valid_text(task_v4_digest) || !valid_text(compile_unit) ||
			!valid_text(source_snapshot) || !valid_text(source_file))
			return sdk::unexpected(failure("provider-worker-v4.ast-batch-invalid", "identity"));
		std::string previous;
		for (const auto& observation : observations)
		{
			if (auto valid = observation.validate(); !valid)
				return valid;
			if (observation.compile_unit != compile_unit)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-batch-invalid", "compile-unit"));
			const auto canonical = observation.canonical_form();
			if (!previous.empty() && previous >= canonical)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-batch-invalid", "observation-order"));
			previous = canonical;
		}
		for (const auto& diagnostic : diagnostics)
			if (!valid_text(diagnostic))
				return sdk::unexpected(
					failure("provider-worker-v4.ast-batch-invalid", "diagnostic"));
		return {};
	}

	sdk::result<provider_worker_v4_ast_observation_batch>
	observe_provider_worker_v4_ast(provider::clang22::borrowed_translation_unit& unit,
								   const source_closure_task_v4_decoded& metadata,
								   std::string compile_unit)
	{
		const source_closure_member* main{};
		if (auto valid = validate_task_metadata(metadata, compile_unit, main); !valid)
			return sdk::unexpected(std::move(valid.error()));
		provider_worker_v4_ast_observation_batch output{
			metadata.identity.task_id,
			metadata.identity.task_v4_digest,
			std::move(compile_unit),
			metadata.input.closure.snapshot_id,
			main->file_id,
			{},
			{},
			0U,
			{},
		};
		// The source file identity is explicit in the closure metadata; the source manager is used
		// only to normalize ranges originating in this already-mounted translation unit.
		output.source_file = main->file_id;
		output.source_snapshot = metadata.input.closure.snapshot_id;

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
		visitor extractor{unit,
						  output,
						  output.source_snapshot,
						  output.source_file,
						  metadata.input.toolchain_digest};
		if (!extractor.TraverseDecl(unit.ast().getTranslationUnitDecl()))
			return sdk::unexpected(
				failure("provider-worker-v4.ast-traversal-failed", "translation-unit"));
#else
		(void)unit;
		return sdk::unexpected(
			failure("native.unsupported-clang-major", "translation-unit", "clang-major-22"));
#endif

		std::ranges::sort(output.observations,
						  {},
						  [](const provider_worker_v4_ast_observation& value)
						  {
							  return value.canonical_form();
						  });
		const materialization::observation_v2_task_authority authority{
			output.compile_unit, output.source_snapshot, output.source_file, main->size_bytes};
		output.rows.reserve(output.observations.size());
		for (const auto& observation : output.observations)
		{
			materialization::native_observation_v2 native;
			switch (observation.kind)
			{
				case provider_worker_v4_ast_observation_kind::entity:
					native.kind = materialization::observation_v2_kind::entity;
					break;
				case provider_worker_v4_ast_observation_kind::type:
					native.kind = materialization::observation_v2_kind::type;
					break;
				case provider_worker_v4_ast_observation_kind::call:
					native.kind = materialization::observation_v2_kind::call;
					break;
			}
			native.final_relation_compile_unit_id = observation.compile_unit;
			native.semantic_key = observation.semantic_key;
			for (const auto& [key, value] : observation.payload)
				native.payload.push_back({key, value});
			native.primary_span = observation.primary_span;
			native.origin_chain = observation.origins;
			native.exact_equivalence = observation.exact_equivalence;
			native.limitation = observation.limitation;
			auto row = materialization::make_observation_v2_row(native, authority);
			if (!row)
				return sdk::unexpected(std::move(row.error()));
			output.rows.push_back(std::move(*row));
		}
		if (auto valid = output.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return output;
	}
} // namespace cxxlens::detail::clang22
