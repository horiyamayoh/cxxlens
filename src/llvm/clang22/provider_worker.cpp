#include "provider_worker.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <map>
#include <memory>
#include <ostream>
#include <ranges>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

#include <cxxlens/provider/clang22.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/sdk/provider.hpp>

#if CXXLENS_HAS_CLANG22
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
		using sdk::provider::message_type;

		constexpr std::string_view provider_id = "cxxlens.clang22.reference";
		const sdk::semantic_version provider_version{1U, 0U, 0U};
		struct output_plan_authority
		{
			provider_output_slot slot;
			std::string_view descriptor_id;
			std::string_view dependency_group;
		};
		constexpr std::array exact_output_plan{
			output_plan_authority{
				provider_output_slot::call_direct_target, "cc.call_direct_target.v1", "canonical"},
			output_plan_authority{provider_output_slot::call_site, "cc.call_site.v1", "canonical"},
			output_plan_authority{provider_output_slot::entity, "cc.entity.v1", "canonical"},
			output_plan_authority{provider_output_slot::call_observation,
								  "frontend.clang22.call_observation.v2",
								  "observation"},
			output_plan_authority{provider_output_slot::entity_observation,
								  "frontend.clang22.entity_observation.v2",
								  "observation"},
			output_plan_authority{provider_output_slot::type_observation,
								  "frontend.clang22.type_observation.v2",
								  "observation"},
		};

		[[nodiscard]] sdk::error
		provider_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
		{
			const auto input = std::as_bytes(std::span{value});
			return {input.begin(), input.end()};
		}

		[[nodiscard]] sdk::detached_cell symbol_cell(const sdk::scalar_kind kind,
													 std::string parameter,
													 std::string value,
													 const bool optional = false)
		{
			return {{kind, std::move(parameter), optional},
					sdk::cell_state::present,
					sdk::scalar_value{std::move(value)},
					std::nullopt};
		}

		[[nodiscard]] sdk::detached_cell optional_typed(std::string parameter, std::string value)
		{
			auto output = sdk::detached_cell::typed(std::move(parameter), std::move(value));
			output.type.optional = true;
			return output;
		}

		[[nodiscard]] sdk::detached_cell optional_utf8(std::string value)
		{
			auto output = sdk::detached_cell::utf8(std::move(value));
			output.type.optional = true;
			return output;
		}

		[[nodiscard]] sdk::detached_cell optional_bytes(std::vector<std::byte> value)
		{
			auto output = sdk::detached_cell::bytes(std::move(value));
			output.type.optional = true;
			return output;
		}

		[[nodiscard]] std::string limitation_text(const std::vector<std::string>& limitations)
		{
			std::ostringstream output;
			for (std::size_t index = 0U; index < limitations.size(); ++index)
			{
				if (index != 0U)
					output << ',';
				output << limitations[index];
			}
			return output.str();
		}

		void append_canonical_text(std::ostringstream& output, const std::string_view value)
		{
			output << value.size() << ':' << value;
		}

		[[nodiscard]] std::string primary_span_canonical(
			const std::optional<materialization::observation_v2_primary_span>& primary_span)
		{
			std::ostringstream output;
			if (!primary_span)
			{
				append_canonical_text(output, "absent");
				return output.str();
			}
			append_canonical_text(output, "present");
			append_canonical_text(output, primary_span->span_id);
			append_canonical_text(output, primary_span->snapshot);
			append_canonical_text(output, primary_span->file);
			append_canonical_text(output, std::to_string(primary_span->begin));
			append_canonical_text(output, std::to_string(primary_span->end));
			append_canonical_text(output, primary_span->role);
			append_canonical_text(output, primary_span->read_only ? "1" : "0");
			return output.str();
		}

		[[nodiscard]] std::string
		origin_chain_canonical(const std::vector<materialization::observation_v2_origin>& origins)
		{
			std::ostringstream output;
			append_canonical_text(output, std::to_string(origins.size()));
			for (const auto& origin : origins)
			{
				append_canonical_text(output, origin.kind);
				append_canonical_text(output, origin.logical_path);
				append_canonical_text(output, std::to_string(origin.begin));
				append_canonical_text(output, std::to_string(origin.end));
				append_canonical_text(output, origin.read_only ? "1" : "0");
			}
			return output.str();
		}

		[[nodiscard]] std::optional<std::string_view>
		source_span_id(const detached_observation& observation)
		{
			if (!observation.primary_span)
				return std::nullopt;
			return observation.primary_span->span_id;
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		observation_v2_row(const detached_observation& observation,
						   const materialization::observation_v2_task_authority& authority,
						   const bool exact,
						   const std::string_view limitation)
		{
			materialization::native_observation_v2 record;
			switch (observation.kind)
			{
				case observation_kind::entity:
					record.kind = materialization::observation_v2_kind::entity;
					break;
				case observation_kind::type:
					record.kind = materialization::observation_v2_kind::type;
					break;
				case observation_kind::call:
					record.kind = materialization::observation_v2_kind::call;
					break;
			}
			record.final_relation_compile_unit_id = observation.compile_unit;
			record.semantic_key = observation.semantic_key;
			for (const auto& [key, value] : observation.payload)
				record.payload.push_back({key, value});
			record.primary_span = observation.primary_span;
			record.origin_chain = observation.origins;
			record.exact_equivalence = exact;
			if (!limitation.empty())
				record.limitation = std::string{limitation};
			return materialization::make_observation_v2_row(record, authority);
		}

		[[nodiscard]] sdk::result<sdk::detached_row> entity_row(
			const detached_observation& observation, const std::string& toolchain, const bool exact)
		{
			using relation = cc::relations::entity;
			relation::builder builder;
			const auto kind = observation.payload.contains("symbol.kind")
				? observation.payload.at("symbol.kind")
				: "unknown";
			const auto signature = observation.payload.contains("symbol.signature")
				? observation.payload.at("symbol.signature")
				: observation.semantic_key;
			for (auto result : {
					 builder.set<relation::entity_column>(
						 sdk::detached_cell::typed("cc_entity_id", "pending")),
					 builder.set<relation::canonicalization>(
						 symbol_cell(sdk::scalar_kind::closed_symbol,
									 "cc.canonicalization-state/1",
									 exact ? "canonicalized" : "provider_local")),
					 builder.set<relation::kind>(
						 symbol_cell(sdk::scalar_kind::open_symbol, "cc.entity-kind/1", kind)),
					 builder.set<relation::structural_signature_digest>(symbol_cell(
						 sdk::scalar_kind::digest,
						 {},
						 *sdk::semantic_digest("cc.entity.structural-signature.v1", signature))),
					 builder.set<relation::toolchain>(
						 optional_typed("toolchain_context_id", toolchain)),
					 builder.set<relation::provider_local_key>(
						 optional_bytes(bytes(observation.semantic_key))),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			const auto qualified_name = observation.payload.find("symbol.qualified_name");
			if (qualified_name != observation.payload.end() && !qualified_name->second.empty())
			{
				auto result =
					builder.set<relation::qualified_name>(optional_utf8(qualified_name->second));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			auto row = std::move(builder).finish();
			if (!row)
				return sdk::unexpected(std::move(row.error()));
			auto identity = sdk::derive_domain_identity(relation::descriptor(), *row);
			if (!identity)
				return sdk::unexpected(std::move(identity.error()));
			row->cells.at("cc.entity.v1.entity") =
				sdk::detached_cell::typed("cc_entity_id", std::move(*identity));
			if (auto valid = sdk::validate_domain_identity(relation::descriptor(), *row); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return row;
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		call_site_row(const detached_observation& observation,
					  const std::map<std::string, std::string, std::less<>>& entities,
					  const std::uint64_t ordinal)
		{
			using relation = cc::relations::call_site;
			relation::builder builder;
			const auto caller = observation.payload.find("call.caller");
			const auto kind = observation.payload.contains("call.kind")
				? observation.payload.at("call.kind")
				: "unknown";
			for (auto result : {
					 builder.set<relation::call>(
						 sdk::detached_cell::typed("cc_call_id", "pending")),
					 builder.set<relation::compile_unit>(
						 sdk::detached_cell::typed("compile_unit_id", observation.compile_unit)),
					 builder.set<relation::kind>(
						 symbol_cell(sdk::scalar_kind::open_symbol, "cc.call-kind/1", kind)),
					 builder.set<relation::source>(sdk::detached_cell::typed(
						 "source_span_id", observation.primary_span->span_id)),
					 builder.set<relation::ordinal>(sdk::detached_cell::unsigned_integer(ordinal)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			if (caller != observation.payload.end())
			{
				const auto found = entities.find(caller->second);
				if (found != entities.end())
				{
					auto result = builder.set<relation::caller>(
						optional_typed("cc_entity_id", found->second));
					if (!result)
						return sdk::unexpected(std::move(result.error()));
				}
			}
			auto row = std::move(builder).finish();
			if (!row)
				return sdk::unexpected(std::move(row.error()));
			auto identity = sdk::derive_domain_identity(relation::descriptor(), *row);
			if (!identity)
				return sdk::unexpected(std::move(identity.error()));
			row->cells.at("cc.call_site.v1.call") =
				sdk::detached_cell::typed("cc_call_id", std::move(*identity));
			if (auto valid = sdk::validate_domain_identity(relation::descriptor(), *row); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return row;
		}

		[[nodiscard]] sdk::result<sdk::detached_row> direct_target_row(const std::string& call,
																	   const std::string& target)
		{
			using relation = cc::relations::call_direct_target;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::call>(sdk::detached_cell::typed("cc_call_id", call)),
					 builder.set<relation::target>(
						 sdk::detached_cell::typed("cc_entity_id", target)),
					 builder.set<relation::resolution>(symbol_cell(sdk::scalar_kind::open_symbol,
																   "cc.direct-target-resolution/1",
																   "syntactic_direct")),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			return std::move(builder).finish();
		}

		[[nodiscard]] sdk::result<std::string> row_string(const sdk::detached_row& row,
														  const std::string_view column)
		{
			const auto found = row.cells.find(column);
			if (found == row.cells.end() || !found->second.value)
				return sdk::unexpected(
					provider_error("provider.canonical-row-invalid", std::string{column}));
			const auto* value = std::get_if<std::string>(&*found->second.value);
			if (value == nullptr)
				return sdk::unexpected(provider_error(
					"provider.canonical-row-invalid", std::string{column}, "not-string"));
			return *value;
		}

		[[nodiscard]] sdk::result<detached_observation>
		direct_callee_observation(const detached_observation& call)
		{
			const auto key = call.payload.find("call.direct_callee");
			const auto kind = call.payload.find("call.direct_callee_kind");
			const auto signature = call.payload.find("call.direct_callee_signature");
			if (key == call.payload.end() || key->second.empty() || kind == call.payload.end() ||
				kind->second.empty() || signature == call.payload.end() ||
				signature->second.empty())
				return sdk::unexpected(provider_error(
					"provider.direct-target-unresolved", call.semantic_key, "projection"));
			detached_observation output;
			output.kind = observation_kind::entity;
			output.compile_unit = call.compile_unit;
			output.semantic_key = key->second;
			output.payload.emplace("symbol.kind", kind->second);
			output.payload.emplace("symbol.signature", signature->second);
			if (const auto confidence = call.payload.find("call.direct_callee_identity_confidence");
				confidence != call.payload.end() && !confidence->second.empty())
				output.payload.emplace("symbol.identity_confidence", confidence->second);
			if (const auto name = call.payload.find("call.direct_callee_qualified_name");
				name != call.payload.end() && !name->second.empty())
				output.payload.emplace("symbol.qualified_name", name->second);
			return output;
		}

		[[nodiscard]] std::string_view entity_field(const detached_observation& observation,
													const std::string_view field)
		{
			const auto found = observation.payload.find(field);
			return found == observation.payload.end() ? std::string_view{} : found->second;
		}

		[[nodiscard]] unsigned entity_preference(const detached_observation& observation)
		{
			if (entity_field(observation, "symbol.is_definition") == "true")
				return 0U;
			if (entity_field(observation, "symbol.is_canonical_declaration") == "true")
				return 1U;
			return 2U;
		}

		[[nodiscard]] bool compatible_redeclaration(const detached_observation& left,
													const detached_observation& right)
		{
			return entity_field(left, "symbol.kind") == entity_field(right, "symbol.kind") &&
				entity_field(left, "symbol.signature") == entity_field(right, "symbol.signature");
		}

		[[nodiscard]] bool call_kind_requires_direct_target(const std::string_view kind)
		{
			return kind == "direct_function" || kind == "direct_member" ||
				kind == "virtual_member" || kind == "operator";
		}

		[[nodiscard]] bool call_kind_forbids_direct_target(const std::string_view kind)
		{
			return kind == "indirect_function" || kind == "indirect_member_pointer" ||
				kind == "dependent" || kind == "unresolved";
		}

		[[nodiscard]] std::string call_occurrence_class(const detached_observation& observation)
		{
			std::ostringstream output;
			for (const auto value : {
					 std::string_view{observation.compile_unit},
					 source_span_id(observation).value_or(std::string_view{}),
					 entity_field(observation, "call.kind"),
					 entity_field(observation, "call.caller"),
				 })
				output << value.size() << ':' << value;
			return output.str();
		}

		[[nodiscard]] bool canonical_digest(const std::string_view value)
		{
			const auto hex = value.starts_with("sha256:")  ? value.substr(7U)
				: value.starts_with("semantic-v2:sha256:") ? value.substr(19U)
														   : std::string_view{};
			return hex.size() == 64U &&
				std::ranges::all_of(hex,
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		class stream_sink final : public sdk::provider::frame_sink
		{
		  public:
			explicit stream_sink(std::ostream& output) : output_{&output} {}

			sdk::result<void> write(const std::span<const std::byte> data) override
			{
				output_->write(reinterpret_cast<const char*>(data.data()),
							   static_cast<std::streamsize>(data.size()));
				return output_->good()
					? sdk::result<void>{}
					: sdk::unexpected(provider_error("provider.worker-write", "stdout"));
			}

		  private:
			std::ostream* output_;
		};

#if CXXLENS_HAS_CLANG22
		[[nodiscard]] std::string declaration_kind(const clang::FunctionDecl& declaration);

		[[nodiscard]] std::optional<std::string>
		fallback_source_anchor(provider::clang22::borrowed_translation_unit& unit,
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
			std::ostringstream projection;
			projection << source_snapshot.size() << ':' << source_snapshot << file.size() << ':'
					   << file << ':'
					   << sdk::content_digest(
							  std::as_bytes(std::span{buffer.data(), buffer.size()}))
					   << ':' << source.getFileOffset(spelling);
			auto anchor =
				sdk::semantic_digest("clang22.declaration-source-anchor.v1", projection.str());
			return anchor ? std::optional<std::string>{std::move(*anchor)} : std::nullopt;
		}

		[[nodiscard]] std::string
		declaration_context_identity(provider::clang22::borrowed_translation_unit& unit,
									 const clang::DeclContext& context,
									 const std::string_view source_snapshot,
									 const std::string_view file)
		{
			std::ostringstream output;
			const auto* context_declaration = clang::Decl::castFromDeclContext(&context);
			output << context_declaration->getDeclKindName();
			if (const auto* declaration = llvm::dyn_cast<clang::NamedDecl>(context_declaration))
			{
				const auto* canonical =
					llvm::cast<clang::NamedDecl>(declaration->getCanonicalDecl());
				output << ':' << canonical->getQualifiedNameAsString();
				if (auto anchor = fallback_source_anchor(
						unit, canonical->getLocation(), source_snapshot, file))
					output << ':' << *anchor;
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
			const auto& associated_constraint = declaration.getTrailingRequiresClause();
			const auto* constraint = associated_constraint.ConstraintExpr;
			if (constraint == nullptr)
				return {};
			const auto text = clang::Lexer::getSourceText(
				clang::CharSourceRange::getTokenRange(constraint->getSourceRange()),
				unit.source_manager(),
				unit.ast().getLangOpts());
			return text.str();
		}

		[[nodiscard]] sdk::result<declaration_identity>
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
			std::optional<std::string> usr;
			if (!clang::index::generateUSRForDecl(canonical, storage) && !storage.empty())
				usr = storage.str().str();
			const auto anchor = fallback_source_anchor(
				unit, anchor_declaration->getLocation(), source_snapshot, file);
			return make_declaration_identity(
				{std::move(usr),
				 std::string{toolchain_digest},
				 declaration_kind(*canonical),
				 canonical->getQualifiedNameAsString(),
				 canonical->getType().getCanonicalType().getAsString(),
				 template_identity(*canonical),
				 constraint_identity(unit, *canonical),
				 declaration_context_identity(
					 unit, *canonical->getDeclContext(), source_snapshot, file),
				 anchor.value_or(std::string{})});
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

		class observation_visitor final : public clang::RecursiveASTVisitor<observation_visitor>
		{
		  public:
			observation_visitor(provider::clang22::borrowed_translation_unit& unit,
								observation_batch& output,
								std::string source_snapshot,
								std::string file,
								std::string toolchain_digest)
				: unit_{&unit}, output_{&output}, source_snapshot_{std::move(source_snapshot)},
				  file_{std::move(file)}, toolchain_digest_{std::move(toolchain_digest)}
			{
			}

			bool TraverseFunctionDecl(clang::FunctionDecl* declaration)
			{
				if (declaration == nullptr)
					return true;
				auto previous = current_function_;
				auto identity = declaration_identity_for(
					*unit_, *declaration, toolchain_digest_, source_snapshot_, file_);
				current_function_ = identity ? identity->semantic_key : std::string{};
				const auto traversed =
					clang::RecursiveASTVisitor<observation_visitor>::TraverseFunctionDecl(
						declaration);
				current_function_ = std::move(previous);
				return traversed;
			}

			bool VisitFunctionDecl(clang::FunctionDecl* declaration)
			{
				if (declaration == nullptr || declaration->isImplicit() ||
					!unit_->source_manager().isWrittenInMainFile(declaration->getLocation()))
					return true;
				auto identity = declaration_identity_for(
					*unit_, *declaration, toolchain_digest_, source_snapshot_, file_);
				if (!identity)
				{
					++output_->failed_count;
					output_->diagnostics.push_back(identity.error().code);
					return true;
				}
				detached_observation entity;
				entity.kind = observation_kind::entity;
				entity.compile_unit = output_->unit;
				entity.semantic_key = identity->semantic_key;
				entity.payload.emplace("symbol.identity_confidence", identity->confidence);
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
				auto source =
					provider::clang22::normalize_source(*unit_,
														declaration->getSourceRange(),
														{source_snapshot_, file_, "declaration"});
				if (source)
				{
					entity.primary_span = materialization::observation_v2_primary_span{
						source->id,
						source->source_snapshot,
						source->file,
						source->begin,
						source->end,
						source->role,
						source->read_only,
					};
					for (const auto& origin : source->origin_chain)
					{
						entity.origins.push_back({origin.kind,
												  origin.logical_path,
												  static_cast<std::int64_t>(origin.begin),
												  static_cast<std::int64_t>(origin.end),
												  origin.read_only});
					}
				}
				insert(std::move(entity));

				detached_observation type;
				type.kind = observation_kind::type;
				type.compile_unit = output_->unit;
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
				detached_observation call;
				call.kind = observation_kind::call;
				call.compile_unit = output_->unit;
				call.payload.emplace("call.kind", call_kind(*expression));
				if (!current_function_.empty())
					call.payload.emplace("call.caller", current_function_);
				if (const auto* callee = expression->getDirectCallee(); callee != nullptr)
				{
					const auto* identity_declaration = callee->getDefinition();
					if (identity_declaration == nullptr)
						identity_declaration = callee->getCanonicalDecl();
					auto callee_identity = declaration_identity_for(
						*unit_, *identity_declaration, toolchain_digest_, source_snapshot_, file_);
					if (!callee_identity)
					{
						++output_->failed_count;
						output_->diagnostics.push_back(callee_identity.error().code);
						call.payload.emplace("call.unresolved_reason",
											 "callee-identity-unavailable");
					}
					else
					{
						call.payload.emplace("call.direct_callee", callee_identity->semantic_key);
						call.payload.emplace("call.direct_callee_identity_confidence",
											 callee_identity->confidence);
					}
					call.payload.emplace("call.direct_callee_kind",
										 declaration_kind(*identity_declaration));
					call.payload.emplace(
						"call.direct_callee_signature",
						identity_declaration->getType().getCanonicalType().getAsString());
					call.payload.emplace("call.direct_callee_qualified_name",
										 identity_declaration->getQualifiedNameAsString());
				}
				else if (call.payload.at("call.kind") == "dependent")
					call.payload.emplace("call.unresolved_reason", "dependent-callee");
				else if (call.payload.at("call.kind") == "indirect_member_pointer")
					call.payload.emplace("call.unresolved_reason",
										 "member-pointer-target-not-modeled");
				else
					call.payload.emplace("call.unresolved_reason",
										 "function-pointer-target-not-modeled");
				auto source = provider::clang22::normalize_source(
					*unit_, expression->getSourceRange(), {source_snapshot_, file_, "expression"});
				if (!source)
				{
					++output_->failed_count;
					output_->diagnostics.push_back(source.error().code);
					std::ostringstream unavailable_key;
					append_canonical_text(unavailable_key,
										  "cxxlens.clang22.call-source-unavailable.v1");
					append_canonical_text(unavailable_key, output_->unit);
					append_canonical_text(unavailable_key, current_function_);
					append_canonical_text(unavailable_key,
										  std::to_string(source_unavailable_call_index_++));
					for (const auto& [key, value] : call.payload)
					{
						append_canonical_text(unavailable_key, key);
						append_canonical_text(unavailable_key, value);
					}
					call.semantic_key = *sdk::semantic_digest(
						"cxxlens.clang22.call-source-unavailable.v1", unavailable_key.str());
					insert(std::move(call));
					return true;
				}
				call.primary_span = materialization::observation_v2_primary_span{
					source->id,
					source->source_snapshot,
					source->file,
					source->begin,
					source->end,
					source->role,
					source->read_only,
				};
				for (const auto& origin : source->origin_chain)
				{
					call.origins.push_back({origin.kind,
											origin.logical_path,
											static_cast<std::int64_t>(origin.begin),
											static_cast<std::int64_t>(origin.end),
											origin.read_only});
				}
				call.semantic_key =
					*sdk::semantic_digest("clang22.call.v1",
										  current_function_ + "\n" + call.primary_span->span_id +
											  "\n" + call.payload["call.direct_callee"]);
				insert(std::move(call));
				return true;
			}

		  private:
			void insert(detached_observation observation)
			{
				const auto key = observation_dedup_key(observation);
				if (seen_.emplace(key, output_->observations.size()).second)
					output_->observations.push_back(std::move(observation));
			}

			provider::clang22::borrowed_translation_unit* unit_;
			observation_batch* output_;
			std::string source_snapshot_;
			std::string file_;
			std::string toolchain_digest_;
			std::string current_function_;
			std::map<std::string, std::size_t, std::less<>> seen_;
			std::uint64_t source_unavailable_call_index_{};
		};
#endif

		[[nodiscard]] sdk::result<observation_batch>
		extract(const clang22_task_input& input, const std::string_view toolchain_digest)
		{
			observation_batch output;
			output.unit = input.compile_unit;
			output.variant = input.variant;
			output.materialization_authority = materialization::observation_v2_task_authority{
				input.compile_unit, input.source_snapshot, input.file, input.source_size_bytes};
			provider::clang22::translation_unit_input native_input{input.source_snapshot,
																   input.file,
																   input.logical_path,
																   input.source,
																   input.arguments};
			auto outcome = provider::clang22::with_translation_unit(
				native_input,
				[&output, &input, toolchain_digest](
					provider::clang22::borrowed_translation_unit& unit) -> sdk::result<void>
				{
#if CXXLENS_HAS_CLANG22
					observation_visitor visitor{unit,
												output,
												input.source_snapshot,
												input.file,
												std::string{toolchain_digest}};
					if (!visitor.TraverseDecl(unit.ast().getTranslationUnitDecl()))
						return sdk::unexpected(
							provider_error("provider.native-traversal-failed", output.unit));
					return {};
#else
					(void)unit;
					(void)input;
					(void)toolchain_digest;
					return sdk::unexpected(provider_error(
						"native.unsupported-clang-major", output.unit, "clang-major-22"));
#endif
				});
			if (!outcome)
				return sdk::unexpected(std::move(outcome.error()));
			std::ranges::sort(output.observations,
							  {},
							  [](const detached_observation& observation)
							  {
								  return observation.canonical_form();
							  });
			if (auto valid = output.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return output;
		}

		[[nodiscard]] sdk::result<sdk::relation_descriptor>
		output_descriptor(const provider_output_slot slot)
		{
			switch (slot)
			{
				case provider_output_slot::call_direct_target:
					return cc::relations::call_direct_target::descriptor();
				case provider_output_slot::call_site:
					return cc::relations::call_site::descriptor();
				case provider_output_slot::entity:
					return cc::relations::entity::descriptor();
				case provider_output_slot::call_observation:
					return call_observation_descriptor();
				case provider_output_slot::entity_observation:
					return entity_observation_descriptor();
				case provider_output_slot::type_observation:
					return type_observation_descriptor();
			}
			return sdk::unexpected(provider_error("provider.output-plan-invalid", "slot"));
		}

		class canonical_provider final : public sdk::provider::portable_provider
		{
		  public:
			canonical_provider(clang22_task_input request,
							   std::string toolchain_digest,
							   std::string semantic_contract_digest)
				: request_{std::move(request)}, toolchain_digest_{std::move(toolchain_digest)},
				  semantic_contract_digest_{std::move(semantic_contract_digest)}
			{
			}

			[[nodiscard]] std::string_view id() const noexcept override
			{
				return provider_id;
			}

			[[nodiscard]] sdk::semantic_version version() const noexcept override
			{
				return provider_version;
			}

			[[nodiscard]] std::string_view semantic_contract_digest() const noexcept override
			{
				return semantic_contract_digest_;
			}

			sdk::result<void> run(const sdk::provider::task& task,
								  sdk::provider::context& context) override
			{
				std::vector<std::string> limitations;
				const auto invocation_exact =
					invocation_has_exact_equivalence(request_.arguments, limitations);
				auto batch = extract(request_, toolchain_digest_);
				if (!batch)
					return sdk::unexpected(std::move(batch.error()));
				auto normalized = canonicalize_provider_batch(
					*batch, toolchain_digest_, invocation_exact, std::move(limitations));
				if (!normalized)
					return sdk::unexpected(std::move(normalized.error()));

				const auto emit = [&](const sdk::relation_descriptor& descriptor,
									  const std::vector<sdk::detached_row>& rows,
									  const std::string_view group) -> sdk::result<void>
				{
					auto sink = context.relation(descriptor);
					if (auto opened = sink.begin(
							std::string{group}, "clang22-atomic", descriptor.id + "-batch");
						!opened)
						return opened;
					for (const auto& row : rows)
						if (auto pushed = sink.push(row); !pushed)
							return pushed;
					return sink.end();
				};

				const auto plan = provider_output_plan();
				if (auto valid = validate_provider_output_plan(plan); !valid)
					return valid;
				const auto rows =
					[&](const provider_output_slot slot) -> const std::vector<sdk::detached_row>*
				{
					switch (slot)
					{
						case provider_output_slot::call_direct_target:
							return &normalized->direct_targets;
						case provider_output_slot::call_site:
							return &normalized->call_sites;
						case provider_output_slot::entity:
							return &normalized->entities;
						case provider_output_slot::call_observation:
							return &normalized->call_observations;
						case provider_output_slot::entity_observation:
							return &normalized->entity_observations;
						case provider_output_slot::type_observation:
							return &normalized->type_observations;
					}
					return nullptr;
				};
				for (const auto& binding : plan)
				{
					auto descriptor = output_descriptor(binding.slot);
					const auto* output_rows = rows(binding.slot);
					if (!descriptor || descriptor->id != binding.descriptor_id ||
						output_rows == nullptr)
						return sdk::unexpected(
							provider_error("provider.output-plan-invalid", "descriptor-binding"));
					if (auto emitted = emit(*descriptor, *output_rows, binding.dependency_group);
						!emitted)
						return emitted;
				}

				for (auto item : normalized->unresolved)
					context.unresolved().add(std::move(item));
				const auto coverage_state =
					normalized->exact_equivalence ? "covered" : "unresolved";
				const auto reason = limitation_text(normalized->equivalence_limitations);
				for (const auto kind : {
						 "frontend.clang22.observation",
						 "cc.entity",
						 "cc.call-extraction",
					 })
				{
					context.coverage().request(kind, task.task_id);
					if (auto classified = context.coverage().classify(
							{kind, task.task_id, coverage_state, reason});
						!classified)
						return classified;
				}
				context.evidence().add(
					{"provider.clang22.execution",
					 task.task_id,
					 std::string{provider_id},
					 normalized->exact_equivalence ? "exact" : "provider-local"});
				return {};
			}

		  private:
			clang22_task_input request_;
			std::string toolchain_digest_;
			std::string semantic_contract_digest_;
		};
	} // namespace

	sdk::result<void> detached_observation::validate() const
	{
		if (compile_unit.empty() || semantic_key.empty())
			return sdk::unexpected(provider_error("provider.observation-invalid", "identity"));
		if (kind == observation_kind::type && (primary_span || !origins.empty()))
			return sdk::unexpected(
				provider_error("provider.observation-invalid", "type_source_authority"));
		if (primary_span)
		{
			if (primary_span->span_id.empty() || primary_span->snapshot.empty() ||
				primary_span->file.empty() || primary_span->role.empty() ||
				primary_span->begin > primary_span->end ||
				!sdk::validate_utf8_text(primary_span->span_id) ||
				!sdk::validate_utf8_text(primary_span->snapshot) ||
				!sdk::validate_utf8_text(primary_span->file) ||
				!sdk::validate_utf8_text(primary_span->role))
				return sdk::unexpected(
					provider_error("provider.observation-invalid", "primary_span"));
			auto expected = sdk::source_span_identity(primary_span->snapshot,
													  primary_span->file,
													  primary_span->begin,
													  primary_span->end,
													  primary_span->role);
			if (!expected || *expected != primary_span->span_id)
				return sdk::unexpected(
					provider_error("provider.observation-invalid", "primary_span_binding"));
		}
		for (const auto& origin : origins)
			if (origin.kind.empty() || origin.logical_path.empty() ||
				!sdk::validate_utf8_text(origin.kind) ||
				!sdk::validate_utf8_text(origin.logical_path) || origin.begin < 0 ||
				origin.end < origin.begin || !origin.read_only)
				return sdk::unexpected(
					provider_error("provider.observation-invalid", "source_origin_chain"));
		for (const auto& [key, value] : payload)
		{
			if (key.empty() || key.find('\0') != std::string::npos ||
				value.find('\0') != std::string::npos)
				return sdk::unexpected(provider_error("provider.observation-invalid", "payload"));
			if ((key == "symbol.identity_confidence" ||
				 key == "call.direct_callee_identity_confidence") &&
				value != "exact-usr" && value != "structural-fallback")
				return sdk::unexpected(
					provider_error("provider.observation-invalid", "identity-confidence"));
		}
		const auto confidence_matches =
			[](const std::string_view key, const std::string_view confidence)
		{
			return (confidence == "exact-usr" && key.starts_with("clang-usr:")) ||
				(confidence == "structural-fallback" && key.starts_with("clang-fallback:"));
		};
		if (const auto confidence = payload.find("symbol.identity_confidence");
			confidence != payload.end() && !confidence_matches(semantic_key, confidence->second))
			return sdk::unexpected(
				provider_error("provider.observation-invalid", "identity-confidence-binding"));
		if (const auto confidence = payload.find("call.direct_callee_identity_confidence");
			confidence != payload.end())
		{
			const auto target = payload.find("call.direct_callee");
			if (target == payload.end() || !confidence_matches(target->second, confidence->second))
				return sdk::unexpected(
					provider_error("provider.observation-invalid", "identity-confidence-binding"));
		}
		return {};
	}

	std::string detached_observation::canonical_form() const
	{
		std::ostringstream output;
		append_canonical_text(output, "cxxlens.clang22.observation-native-order.v2");
		append_canonical_text(output, std::to_string(static_cast<unsigned>(kind)));
		append_canonical_text(output, compile_unit);
		append_canonical_text(output, semantic_key);
		append_canonical_text(output, std::to_string(payload.size()));
		for (const auto& [key, value] : payload)
		{
			append_canonical_text(output, key);
			append_canonical_text(output, value);
		}
		append_canonical_text(output, primary_span_canonical(primary_span));
		append_canonical_text(output, origin_chain_canonical(origins));
		return output.str();
	}

	std::string observation_dedup_key(const detached_observation& observation)
	{
		return observation.canonical_form();
	}

	sdk::result<void> observation_batch::validate() const
	{
		if (unit.empty() || variant.empty())
			return sdk::unexpected(provider_error("provider.batch-invalid", "identity"));
		if (materialization_authority &&
			materialization_authority->final_relation_compile_unit_id != unit)
			return sdk::unexpected(
				provider_error("provider.batch-invalid", "materialization_authority"));
		for (const auto& observation : observations)
		{
			if (auto valid = observation.validate(); !valid)
				return valid;
			if (observation.compile_unit != unit)
				return sdk::unexpected(provider_error("provider.batch-invalid", "compile_unit"));
		}
		for (const auto& diagnostic : diagnostics)
			if (diagnostic.empty())
				return sdk::unexpected(provider_error("provider.batch-invalid", "diagnostic"));
		return {};
	}

	std::vector<provider_output_binding> provider_output_plan()
	{
		std::vector<provider_output_binding> output;
		output.reserve(exact_output_plan.size());
		for (const auto& binding : exact_output_plan)
			output.push_back({binding.slot,
							  std::string{binding.descriptor_id},
							  std::string{binding.dependency_group}});
		return output;
	}

	sdk::result<void>
	validate_provider_output_plan(const std::span<const provider_output_binding> plan)
	{
		if (plan.size() != exact_output_plan.size())
			return sdk::unexpected(provider_error(
				"provider.output-plan-invalid", "descriptor-count", "missing-or-extra"));
		for (std::size_t left{}; left < plan.size(); ++left)
			for (std::size_t right = left + 1U; right < plan.size(); ++right)
				if (plan[left].slot == plan[right].slot ||
					plan[left].descriptor_id == plan[right].descriptor_id)
					return sdk::unexpected(
						provider_error("provider.output-plan-invalid", "descriptor", "duplicate"));
		for (std::size_t index{}; index < plan.size(); ++index)
		{
			const auto& actual = plan[index];
			const auto& expected = exact_output_plan[index];
			if (actual.slot != expected.slot || actual.descriptor_id != expected.descriptor_id)
				return sdk::unexpected(provider_error(
					"provider.output-plan-invalid", "descriptor", "membership-or-order"));
			if (actual.dependency_group != expected.dependency_group)
				return sdk::unexpected(
					provider_error("provider.output-plan-invalid", "dependency-group", "order"));
		}
		return {};
	}

	sdk::relation_descriptor entity_observation_descriptor()
	{
		return materialization::entity_observation_v2_descriptor();
	}

	sdk::relation_descriptor type_observation_descriptor()
	{
		return materialization::type_observation_v2_descriptor();
	}

	sdk::relation_descriptor call_observation_descriptor()
	{
		return materialization::call_observation_v2_descriptor();
	}

	bool invocation_has_exact_equivalence(const std::span<const std::string> arguments,
										  std::vector<std::string>& limitations)
	{
		static constexpr std::array<std::string_view, 5U> unsupported_semantic_prefixes{
			"-fabi-version",
			"-fno-gnu-unique",
			"-fconcepts-diagnostics-depth",
			"-Wno-psabi",
			"-mabi=",
		};
		for (const auto& argument : arguments)
			for (const auto prefix : unsupported_semantic_prefixes)
				if (argument.starts_with(prefix))
					limitations.push_back("ignored-or-gcc-specific-option:" + argument);
		std::ranges::sort(limitations);
		limitations.erase(std::ranges::unique(limitations).begin(), limitations.end());
		return limitations.empty();
	}

	sdk::result<declaration_identity>
	make_declaration_identity(const declaration_identity_input& input)
	{
		if (input.usr && !input.usr->empty())
			return declaration_identity{"clang-usr:" + *input.usr, "exact-usr"};
		if (input.toolchain_digest.size() != 71U ||
			!input.toolchain_digest.starts_with("sha256:") || input.declaration_kind.empty() ||
			input.canonical_signature.empty() || input.declaration_context.empty() ||
			input.canonical_source_anchor.empty())
			return sdk::unexpected(provider_error(
				"provider.declaration-identity-unresolved", "fallback", "incomplete-projection"));
		std::ostringstream projection;
		const auto append = [&](const std::string_view name, const std::string_view value)
		{
			projection << name.size() << ':' << name << value.size() << ':' << value;
		};
		append("schema", "clang22.declaration-fallback.v2");
		append("toolchain_digest", input.toolchain_digest);
		append("declaration_kind", input.declaration_kind);
		append("qualified_name", input.qualified_name);
		append("canonical_signature", input.canonical_signature);
		append("template_identity", input.template_identity);
		append("constraint_identity", input.constraint_identity);
		append("declaration_context", input.declaration_context);
		append("canonical_source_anchor", input.canonical_source_anchor);
		return declaration_identity{
			"clang-fallback:" +
				*sdk::semantic_digest("clang22.declaration-fallback.v2", projection.str()),
			"structural-fallback"};
	}

	sdk::result<canonicalized_provider_batch>
	canonicalize_provider_batch(const observation_batch& batch,
								const std::string& toolchain_digest,
								const bool invocation_exact,
								std::vector<std::string> invocation_limitations)
	{
		if (auto valid = batch.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (!toolchain_digest.starts_with("sha256:"))
			return sdk::unexpected(
				provider_error("provider.toolchain-digest-invalid", "toolchain"));
		for (const auto& diagnostic : batch.diagnostics)
			invocation_limitations.push_back("frontend-diagnostic:" + diagnostic);
		for (const auto& observation : batch.observations)
			for (const auto field : {std::string_view{"symbol.identity_confidence"},
									 std::string_view{"call.direct_callee_identity_confidence"}})
				if (const auto confidence = observation.payload.find(field);
					confidence != observation.payload.end() && confidence->second != "exact-usr")
					invocation_limitations.push_back("identity-confidence:" + confidence->second +
													 ":" + observation.semantic_key);
		std::ranges::sort(invocation_limitations);
		invocation_limitations.erase(std::ranges::unique(invocation_limitations).begin(),
									 invocation_limitations.end());

		canonicalized_provider_batch output;
		output.equivalence_limitations = std::move(invocation_limitations);
		const auto toolchain = *sdk::semantic_digest("toolchain-context", toolchain_digest);
		std::vector<const detached_observation*> ordered_observations;
		ordered_observations.reserve(batch.observations.size());
		for (const auto& observation : batch.observations)
			ordered_observations.push_back(&observation);
		std::ranges::sort(ordered_observations,
						  [](const detached_observation* left, const detached_observation* right)
						  {
							  return left->canonical_form() < right->canonical_form();
						  });
		for (const auto* observation : ordered_observations)
			if (observation->kind != observation_kind::type && !observation->primary_span)
				output.unresolved.push_back(
					{"provider.source-unavailable",
					 observation->semantic_key,
					 observation->kind == observation_kind::entity ? "cc.entity" : "cc.call_site"});
		std::map<std::string, std::vector<const detached_observation*>, std::less<>> entity_groups;
		for (const auto& observation : batch.observations)
			if (observation.kind == observation_kind::entity)
				entity_groups[observation.semantic_key].push_back(&observation);
		std::vector<const detached_observation*> selected_entities;
		selected_entities.reserve(entity_groups.size());
		for (auto& [semantic_key, group] : entity_groups)
		{
			std::ranges::sort(
				group,
				[](const detached_observation* left, const detached_observation* right)
				{
					return std::tuple{entity_preference(*left), left->canonical_form()} <
						std::tuple{entity_preference(*right), right->canonical_form()};
				});
			const auto* selected = group.front();
			selected_entities.push_back(selected);
			if (std::ranges::any_of(group,
									[&](const detached_observation* candidate)
									{
										return !compatible_redeclaration(*selected, *candidate);
									}))
			{
				output.equivalence_limitations.push_back("incompatible-redeclaration:" +
														 semantic_key);
				output.unresolved.push_back(
					{"provider.entity-redeclaration-incompatible", semantic_key, "cc.entity"});
			}
		}
		for (const auto& observation : batch.observations)
		{
			if (observation.kind != observation_kind::type && !observation.primary_span)
				output.equivalence_limitations.push_back(
					"source-authority-unavailable:" +
					std::string{observation.kind == observation_kind::entity ? "entity:"
																			 : "call:"} +
					observation.semantic_key);
			if (observation.kind != observation_kind::call)
				continue;
			const auto kind = entity_field(observation, "call.kind");
			const auto target = entity_field(observation, "call.direct_callee");
			if ((target.empty() && call_kind_requires_direct_target(kind)) ||
				(!target.empty() && call_kind_forbids_direct_target(kind)))
				output.equivalence_limitations.push_back("call-kind-target-inconsistent:" +
														 observation.semantic_key);
		}
		std::ranges::sort(output.equivalence_limitations);
		output.equivalence_limitations.erase(
			std::ranges::unique(output.equivalence_limitations).begin(),
			output.equivalence_limitations.end());
		output.exact_equivalence =
			invocation_exact && output.equivalence_limitations.empty() && batch.failed_count == 0U;
		const auto limitation = limitation_text(output.equivalence_limitations);
		const auto observation_authority = batch.materialization_authority.value_or(
			materialization::observation_v2_task_authority{batch.unit, {}, {}, 0U});

		std::map<std::string, std::string, std::less<>> entity_ids;
		for (const auto* observation : ordered_observations)
			if (observation->kind == observation_kind::entity)
			{
				auto local = observation_v2_row(
					*observation, observation_authority, output.exact_equivalence, limitation);
				if (!local)
					return sdk::unexpected(std::move(local.error()));
				output.entity_observations.push_back(std::move(*local));
			}
		for (const auto* observation : selected_entities)
		{
			auto canonical = entity_row(*observation, toolchain, output.exact_equivalence);
			if (!canonical)
				return sdk::unexpected(std::move(canonical.error()));
			auto entity = row_string(*canonical, "cc.entity.v1.entity");
			if (!entity)
				return sdk::unexpected(std::move(entity.error()));
			entity_ids.emplace(observation->semantic_key, std::move(*entity));
			output.entities.push_back(std::move(*canonical));
		}

		for (const auto* observation : ordered_observations)
		{
			if (observation->kind != observation_kind::type)
				continue;
			auto local = observation_v2_row(
				*observation, observation_authority, output.exact_equivalence, limitation);
			if (!local)
				return sdk::unexpected(std::move(local.error()));
			output.type_observations.push_back(std::move(*local));
		}
		std::vector<const detached_observation*> calls;
		for (const auto& observation : batch.observations)
			if (observation.kind == observation_kind::call)
				calls.push_back(&observation);
		std::ranges::sort(
			calls,
			[](const detached_observation* left, const detached_observation* right)
			{
				return std::tuple{call_occurrence_class(*left), left->canonical_form()} <
					std::tuple{call_occurrence_class(*right), right->canonical_form()};
			});
		std::string previous_class;
		std::uint64_t call_ordinal{};
		for (const auto* observation_pointer : calls)
		{
			const auto& observation = *observation_pointer;
			const auto occurrence_class = call_occurrence_class(observation);
			if (occurrence_class != previous_class)
			{
				previous_class = occurrence_class;
				call_ordinal = 0U;
			}
			auto local = observation_v2_row(
				observation, observation_authority, output.exact_equivalence, limitation);
			if (!local)
				return sdk::unexpected(std::move(local.error()));
			output.call_observations.push_back(std::move(*local));
			if (!observation.primary_span)
				continue;
			auto site = call_site_row(observation, entity_ids, call_ordinal++);
			if (!site)
				return sdk::unexpected(std::move(site.error()));
			auto call = row_string(*site, "cc.call_site.v1.call");
			if (!call)
				return sdk::unexpected(std::move(call.error()));
			output.call_sites.push_back(std::move(*site));
			const auto target = observation.payload.find("call.direct_callee");
			if (target == observation.payload.end() || target->second.empty())
			{
				const auto reason = observation.payload.contains("call.unresolved_reason")
					? observation.payload.at("call.unresolved_reason")
					: "no-direct-callee";
				const auto kind = entity_field(observation, "call.kind");
				const auto code = call_kind_requires_direct_target(kind)
					? "provider.call-kind-target-inconsistent"
					: (kind.starts_with("indirect_") ? "provider.indirect-target-unresolved"
													 : "provider.call-target-unresolved");
				output.unresolved.push_back({code, *call, reason});
				continue;
			}
			if (call_kind_forbids_direct_target(entity_field(observation, "call.kind")))
			{
				output.unresolved.push_back(
					{"provider.call-kind-target-inconsistent", *call, "unexpected-direct-callee"});
				continue;
			}
			auto target_entity = entity_ids.find(target->second);
			std::string target_id;
			if (target_entity != entity_ids.end())
				target_id = target_entity->second;
			else
			{
				auto target_observation = direct_callee_observation(observation);
				if (!target_observation)
				{
					output.unresolved.push_back(
						{"provider.direct-target-unresolved", *call, "projection"});
					continue;
				}
				auto target_row =
					entity_row(*target_observation, toolchain, output.exact_equivalence);
				if (!target_row)
					return sdk::unexpected(std::move(target_row.error()));
				auto projected = row_string(*target_row, "cc.entity.v1.entity");
				if (!projected)
					return sdk::unexpected(std::move(projected.error()));
				target_id = std::move(*projected);
			}
			auto direct = direct_target_row(*call, target_id);
			if (!direct)
				return sdk::unexpected(std::move(direct.error()));
			output.direct_targets.push_back(std::move(*direct));
		}
		return output;
	}

	int run_provider_worker(const std::span<const std::byte> input, std::ostream& output)
	{
		const auto environment = [](const char* name) -> std::optional<std::string>
		{
			const auto* value = std::getenv(name);
			return value == nullptr ? std::nullopt : std::optional<std::string>{value};
		};
		auto expected_manifest = environment("CXXLENS_PROVIDER_MANIFEST");
		auto expected_provider_id = environment("CXXLENS_PROVIDER_ID");
		auto expected_semantic_contract = environment("CXXLENS_PROVIDER_SEMANTIC_CONTRACT_DIGEST");
		auto expected_task_id = environment("CXXLENS_PROVIDER_TASK_ID");
		auto expected_task_digest = environment("CXXLENS_PROVIDER_TASK_INPUT_DIGEST");
		auto expected_invocation = environment("CXXLENS_PROVIDER_NORMALIZED_INVOCATION_DIGEST");
		auto expected_toolchain = environment("CXXLENS_PROVIDER_TOOLCHAIN_DIGEST");
		auto expected_environment = environment("CXXLENS_PROVIDER_ENVIRONMENT_DIGEST");
		auto expected_major = environment("CXXLENS_PROVIDER_PROTOCOL_MAJOR");
		auto expected_minor = environment("CXXLENS_PROVIDER_PROTOCOL_MINOR");
		if (!expected_manifest || !expected_provider_id || !expected_semantic_contract ||
			*expected_provider_id != provider_id ||
			!canonical_digest(*expected_semantic_contract) || !expected_task_id ||
			!expected_task_digest || !expected_invocation || !expected_toolchain ||
			!expected_environment || !expected_major || !expected_minor)
			return EXIT_FAILURE;
		sdk::provider::protocol_limits input_limits;
		const auto parse_version = [](const std::string_view text, std::uint16_t& output)
		{
			const auto [end, error] =
				std::from_chars(text.data(), text.data() + text.size(), output);
			return error == std::errc{} && end == text.data() + text.size();
		};
		if (!parse_version(*expected_major, input_limits.protocol_major) ||
			!parse_version(*expected_minor, input_limits.maximum_minor))
			return EXIT_FAILURE;
		input_limits.minimum_minor = input_limits.maximum_minor;
		auto frames = sdk::provider::decode_frame_stream(input, input_limits);
		if (!frames)
			return EXIT_FAILURE;
		auto validated = sdk::provider::validate_host_transcript(*frames,
																 {*expected_manifest,
																  {*expected_task_id,
																   *expected_task_digest,
																   *expected_invocation,
																   *expected_toolchain,
																   *expected_environment},
																  input_limits});
		if (!validated)
			return EXIT_FAILURE;

		stream_sink sink{output};
		sdk::provider::protocol_writer writer{sink};
		writer.grant_credit(validated->credit);
		if (!writer.send(message_type::hello, frames->at(0U).control))
			return EXIT_FAILURE;
		if (!writer.send(message_type::schema_negotiate, frames->at(1U).control))
			return EXIT_FAILURE;
		const auto& task_control = validated->task;
		const std::string task_id{task_control.task_id};
		const auto send_frontend_failure = [&](const std::string_view field)
		{
			auto control = sdk::provider::encode_task_failed_metadata(
				{"provider.frontend-request-invalid", task_id, std::string{field}});
			if (control)
				(void)writer.send(message_type::task_failed, *control);
		};

		auto request = decode_task_input(validated->payload);
		if (!request)
		{
			send_frontend_failure("payload");
			return EXIT_SUCCESS;
		}
		const std::string toolchain_digest{task_control.toolchain_digest};
		const std::string environment_digest{task_control.environment_digest};
		if (request->normalized_invocation_digest != task_control.normalized_invocation_digest ||
			request->toolchain_digest != toolchain_digest ||
			request->environment_digest != environment_digest)
		{
			send_frontend_failure("task-binding");
			return EXIT_SUCCESS;
		}
		const auto output_plan = provider_output_plan();
		if (auto valid = validate_provider_output_plan(output_plan); !valid)
		{
			send_frontend_failure("output-plan");
			return EXIT_SUCCESS;
		}
		std::vector<sdk::relation_descriptor> outputs;
		outputs.reserve(output_plan.size());
		for (const auto& binding : output_plan)
		{
			auto descriptor = output_descriptor(binding.slot);
			if (!descriptor || descriptor->id != binding.descriptor_id)
			{
				send_frontend_failure("output-plan");
				return EXIT_SUCCESS;
			}
			outputs.push_back(std::move(*descriptor));
		}
		auto task =
			reconstruct_provider_task(*request, std::move(outputs), *expected_semantic_contract);
		if (!task || task->task_id != task_id)
		{
			send_frontend_failure("task-id");
			return EXIT_SUCCESS;
		}
		auto execution = sdk::provider::execution_context{};
		execution.budget = request->budget;
		canonical_provider provider{
			std::move(*request), toolchain_digest, *expected_semantic_contract};
		(void)sdk::provider::run_worker(provider, *task, writer, std::move(execution));
		return EXIT_SUCCESS;
	}
} // namespace cxxlens::detail::clang22
