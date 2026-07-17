#include "provider_worker.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <limits>
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
		constexpr std::string_view task_magic = "cxxlens.clang22.task.v2";
		const sdk::semantic_version provider_version{1U, 0U, 0U};
		constexpr std::string_view provider_semantic_contract{
			"sha256:1111111111111111111111111111111111111111111111111111111111111111"};
		constexpr std::uint32_t maximum_string_bytes = 64U * 1024U * 1024U;
		constexpr std::uint32_t maximum_arguments = 1024U;

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

		[[nodiscard, maybe_unused]] std::string
		origin_canonical(const provider::clang22::detached_source_origin& origin)
		{
			std::ostringstream output;
			output << origin.kind.size() << ':' << origin.kind << origin.logical_path.size() << ':'
				   << origin.logical_path << ':' << origin.begin << ':' << origin.end << ':'
				   << (origin.read_only ? '1' : '0');
			return output.str();
		}

		[[nodiscard]] std::string origin_chain_canonical(const std::vector<std::string>& origins)
		{
			std::ostringstream output;
			for (const auto& origin : origins)
				output << origin.size() << ':' << origin;
			return output.str();
		}

		[[nodiscard]] sdk::relation_descriptor observation_descriptor(const std::string& name)
		{
			sdk::relation_descriptor descriptor;
			descriptor.id = name + ".v1";
			descriptor.name = name;
			descriptor.version = {1U, 0U, 0U};
			descriptor.semantic_major = 1U;
			descriptor.semantics = name + "/1";
			descriptor.owner_namespace = "cxxlens.provider.clang22";
			const auto prefix = descriptor.id + ".";
			descriptor.columns = {
				{prefix + "observation",
				 "observation",
				 {sdk::scalar_kind::typed_id, "clang22_observation_id", false},
				 true,
				 sdk::column_role::claim_key},
				{prefix + "compile_unit",
				 "compile_unit",
				 {sdk::scalar_kind::typed_id, "compile_unit_id", false},
				 true,
				 sdk::column_role::authoritative_payload},
				{prefix + "semantic_key",
				 "semantic_key",
				 {sdk::scalar_kind::bytes, {}, false},
				 true,
				 sdk::column_role::authoritative_payload},
				{prefix + "payload_digest",
				 "payload_digest",
				 {sdk::scalar_kind::digest, {}, false},
				 true,
				 sdk::column_role::authoritative_payload},
				{prefix + "source",
				 "source",
				 {sdk::scalar_kind::typed_id, "source_span_id", true},
				 false,
				 sdk::column_role::authoritative_payload},
				{prefix + "source_origin_chain",
				 "source_origin_chain",
				 {sdk::scalar_kind::bytes, {}, true},
				 false,
				 sdk::column_role::authoritative_payload},
				{prefix + "exact_equivalence",
				 "exact_equivalence",
				 {sdk::scalar_kind::boolean, {}, false},
				 true,
				 sdk::column_role::authoritative_payload},
				{prefix + "limitation",
				 "limitation",
				 {sdk::scalar_kind::utf8_string, {}, true},
				 false,
				 sdk::column_role::authoritative_payload},
			};
			descriptor.key_columns = {prefix + "observation"};
			descriptor.merge = sdk::merge_mode::functional_assertion;
			descriptor.conflict_columns = {
				prefix + "compile_unit",
				prefix + "semantic_key",
				prefix + "payload_digest",
				prefix + "source",
				prefix + "source_origin_chain",
				prefix + "exact_equivalence",
				prefix + "limitation",
			};
			descriptor.descriptor_digest = *sdk::semantic_digest(
				"cxxlens.relation-descriptor-binding.v2",
				descriptor.contract_digest + "\n" + descriptor.canonical_form());
			return descriptor;
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		observation_row(const sdk::relation_descriptor& descriptor,
						const detached_observation& observation,
						const bool exact,
						const std::string_view limitation)
		{
			sdk::row_builder builder{descriptor};
			const auto projection = observation.canonical_form();
			const auto prefix = descriptor.id + ".";
			for (auto result : {
					 builder.set({descriptor.id,
								  prefix + "observation",
								  {sdk::scalar_kind::typed_id, "clang22_observation_id", false}},
								 sdk::detached_cell::typed(
									 "clang22_observation_id",
									 *sdk::semantic_digest("clang22.observation.v1", projection))),
					 builder.set(
						 {descriptor.id,
						  prefix + "compile_unit",
						  {sdk::scalar_kind::typed_id, "compile_unit_id", false}},
						 sdk::detached_cell::typed("compile_unit_id", observation.compile_unit)),
					 builder.set({descriptor.id,
								  prefix + "semantic_key",
								  {sdk::scalar_kind::bytes, {}, false}},
								 sdk::detached_cell::bytes(bytes(observation.semantic_key))),
					 builder.set({descriptor.id,
								  prefix + "payload_digest",
								  {sdk::scalar_kind::digest, {}, false}},
								 symbol_cell(sdk::scalar_kind::digest,
											 {},
											 *sdk::semantic_digest("clang22.observation-payload.v1",
																   projection))),
					 builder.set({descriptor.id,
								  prefix + "exact_equivalence",
								  {sdk::scalar_kind::boolean, {}, false}},
								 sdk::detached_cell::boolean(exact)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			if (observation.source_span_id)
			{
				auto result =
					builder.set({descriptor.id,
								 prefix + "source",
								 {sdk::scalar_kind::typed_id, "source_span_id", true}},
								optional_typed("source_span_id", *observation.source_span_id));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			if (!observation.source_origin_chain.empty())
			{
				auto result = builder.set(
					{descriptor.id,
					 prefix + "source_origin_chain",
					 {sdk::scalar_kind::bytes, {}, true}},
					optional_bytes(bytes(origin_chain_canonical(observation.source_origin_chain))));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			if (!limitation.empty())
			{
				auto result = builder.set({descriptor.id,
										   prefix + "limitation",
										   {sdk::scalar_kind::utf8_string, {}, true}},
										  optional_utf8(std::string{limitation}));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			return std::move(builder).finish();
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
			if (observation.source_span_id)
			{
				auto result = builder.set<relation::anchor>(
					optional_typed("source_span_id", *observation.source_span_id));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
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
					 builder.set<relation::source>(
						 sdk::detached_cell::typed("source_span_id", *observation.source_span_id)),
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
			if (const auto anchor = call.payload.find("call.direct_callee_anchor");
				anchor != call.payload.end() && !anchor->second.empty())
				output.source_span_id = anchor->second;
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
					 observation.source_span_id ? std::string_view{*observation.source_span_id}
												: std::string_view{},
					 entity_field(observation, "call.kind"),
					 entity_field(observation, "call.caller"),
				 })
				output << value.size() << ':' << value;
			return output.str();
		}

		class binary_writer
		{
		  public:
			void string(const std::string_view value)
			{
				integer(static_cast<std::uint32_t>(value.size()));
				const auto data = std::as_bytes(std::span{value});
				output_.insert(output_.end(), data.begin(), data.end());
			}

			void integer(const std::uint32_t value)
			{
				for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
					output_.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
			}

			[[nodiscard]] std::vector<std::byte> finish() &&
			{
				return std::move(output_);
			}

		  private:
			std::vector<std::byte> output_;
		};

		class binary_reader
		{
		  public:
			explicit binary_reader(const std::span<const std::byte> input) : input_{input} {}

			[[nodiscard]] sdk::result<std::uint32_t> integer()
			{
				if (remaining() < 4U)
					return sdk::unexpected(
						provider_error("provider.frontend-request-invalid", "length"));
				std::uint32_t output{};
				for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
					output |= std::to_integer<std::uint32_t>(input_[offset_++]) << shift;
				return output;
			}

			[[nodiscard]] sdk::result<std::string> string()
			{
				auto length = integer();
				if (!length)
					return sdk::unexpected(std::move(length.error()));
				if (*length > maximum_string_bytes || remaining() < *length)
					return sdk::unexpected(
						provider_error("provider.frontend-request-invalid", "string"));
				const auto* data = reinterpret_cast<const char*>(input_.data() + offset_);
				std::string output{data, *length};
				offset_ += *length;
				return output;
			}

			[[nodiscard]] bool empty() const noexcept
			{
				return offset_ == input_.size();
			}

		  private:
			[[nodiscard]] std::size_t remaining() const noexcept
			{
				return input_.size() - offset_;
			}
			std::span<const std::byte> input_;
			std::size_t offset_{};
		};

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
					entity.source_span_id = std::move(source->id);
					for (const auto& origin : source->origin_chain)
						entity.source_origin_chain.push_back(origin_canonical(origin));
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
					if (unit_->source_manager().isWrittenInMainFile(
							identity_declaration->getLocation()))
					{
						auto callee_source = provider::clang22::normalize_source(
							*unit_,
							identity_declaration->getSourceRange(),
							{source_snapshot_, file_, "declaration"});
						if (callee_source)
							call.payload.emplace("call.direct_callee_anchor",
												 std::move(callee_source->id));
					}
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
					return true;
				}
				call.source_span_id = std::move(source->id);
				for (const auto& origin : source->origin_chain)
					call.source_origin_chain.push_back(origin_canonical(origin));
				call.semantic_key =
					*sdk::semantic_digest("clang22.call.v1",
										  current_function_ + "\n" + *call.source_span_id + "\n" +
											  call.payload["call.direct_callee"]);
				insert(std::move(call));
				return true;
			}

		  private:
			void insert(detached_observation observation)
			{
				const auto key = std::to_string(static_cast<unsigned>(observation.kind)) + "\n" +
					observation.semantic_key + "\n" +
					observation.source_span_id.value_or(std::string{});
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
		};
#endif

		[[nodiscard]] sdk::result<observation_batch>
		extract(const clang22_task_input& input, const std::string_view toolchain_digest)
		{
			observation_batch output;
			output.unit = input.compile_unit;
			output.variant = input.variant;
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

		class canonical_provider final : public sdk::provider::portable_provider
		{
		  public:
			canonical_provider(clang22_task_input request, std::string toolchain_digest)
				: request_{std::move(request)}, toolchain_digest_{std::move(toolchain_digest)}
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
				return provider_semantic_contract;
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

				for (const auto& [descriptor, rows, group] : {
						 std::tuple{entity_observation_descriptor(),
									&normalized->entity_observations,
									std::string_view{"observation"}},
						 std::tuple{type_observation_descriptor(),
									&normalized->type_observations,
									std::string_view{"observation"}},
						 std::tuple{call_observation_descriptor(),
									&normalized->call_observations,
									std::string_view{"observation"}},
						 std::tuple{cc::relations::entity::descriptor(),
									&normalized->entities,
									std::string_view{"canonical"}},
						 std::tuple{cc::relations::call_site::descriptor(),
									&normalized->call_sites,
									std::string_view{"canonical"}},
						 std::tuple{cc::relations::call_direct_target::descriptor(),
									&normalized->direct_targets,
									std::string_view{"canonical"}},
					 })
					if (auto emitted = emit(descriptor, *rows, group); !emitted)
						return emitted;

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
		};
	} // namespace

	sdk::result<void> detached_observation::validate() const
	{
		if (compile_unit.empty() || semantic_key.empty())
			return sdk::unexpected(provider_error("provider.observation-invalid", "identity"));
		if (source_span_id && source_span_id->empty())
			return sdk::unexpected(
				provider_error("provider.observation-invalid", "source_span_id"));
		for (const auto& origin : source_origin_chain)
			if (origin.empty() || origin.find('\0') != std::string::npos)
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
		output << static_cast<unsigned>(kind) << '\n'
			   << compile_unit << '\n'
			   << semantic_key << '\n';
		for (const auto& [key, value] : payload)
			output << key.size() << ':' << key << value.size() << ':' << value;
		output << '\n'
			   << source_span_id.value_or(std::string{}) << '\n'
			   << origin_chain_canonical(source_origin_chain);
		return output.str();
	}

	sdk::result<void> observation_batch::validate() const
	{
		if (unit.empty() || variant.empty())
			return sdk::unexpected(provider_error("provider.batch-invalid", "identity"));
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

	sdk::result<void> clang22_task_input::validate() const
	{
		if (compile_unit.empty() || variant.empty())
			return sdk::unexpected(provider_error("provider.frontend-request-invalid", "identity"));
		return provider::clang22::translation_unit_input{
			source_snapshot, file, logical_path, source, arguments}
			.validate();
	}

	sdk::result<std::vector<std::byte>> encode_task_input(const clang22_task_input& input)
	{
		if (auto valid = input.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (input.source.size() > maximum_string_bytes)
			return sdk::unexpected(
				provider_error("provider.frontend-request-invalid", "source-size"));
		binary_writer writer;
		writer.string(task_magic);
		writer.string(input.compile_unit);
		writer.string(input.variant);
		writer.string(input.source_snapshot);
		writer.string(input.file);
		writer.string(input.logical_path);
		writer.string(input.source);
		writer.integer(static_cast<std::uint32_t>(input.arguments.size()));
		for (const auto& argument : input.arguments)
			writer.string(argument);
		return std::move(writer).finish();
	}

	sdk::result<clang22_task_input> decode_task_input(const std::span<const std::byte> input)
	{
		binary_reader reader{input};
		auto magic = reader.string();
		auto compile_unit = reader.string();
		auto variant = reader.string();
		auto source_snapshot = reader.string();
		auto file = reader.string();
		auto logical_path = reader.string();
		auto source = reader.string();
		auto count = reader.integer();
		if (!magic || !compile_unit || !variant || !source_snapshot || !file || !logical_path ||
			!source || !count || *magic != task_magic || *count > maximum_arguments)
			return sdk::unexpected(provider_error("provider.frontend-request-invalid", "payload"));
		clang22_task_input output{
			std::move(*compile_unit),
			std::move(*variant),
			std::move(*source_snapshot),
			std::move(*file),
			std::move(*logical_path),
			std::move(*source),
			{},
		};
		output.arguments.reserve(*count);
		for (std::uint32_t index = 0U; index < *count; ++index)
		{
			auto argument = reader.string();
			if (!argument)
				return sdk::unexpected(std::move(argument.error()));
			output.arguments.push_back(std::move(*argument));
		}
		if (!reader.empty())
			return sdk::unexpected(
				provider_error("provider.frontend-request-invalid", "trailing-bytes"));
		if (auto valid = output.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return output;
	}

	sdk::relation_descriptor entity_observation_descriptor()
	{
		return observation_descriptor("frontend.clang22.entity_observation");
	}

	sdk::relation_descriptor type_observation_descriptor()
	{
		return observation_descriptor("frontend.clang22.type_observation");
	}

	sdk::relation_descriptor call_observation_descriptor()
	{
		return observation_descriptor("frontend.clang22.call_observation");
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

		std::map<std::string, std::string, std::less<>> entity_ids;
		for (const auto* observation : ordered_observations)
			if (observation->kind == observation_kind::entity)
			{
				auto local = observation_row(entity_observation_descriptor(),
											 *observation,
											 output.exact_equivalence,
											 limitation);
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
			auto local = observation_row(
				type_observation_descriptor(), *observation, output.exact_equivalence, limitation);
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
			auto local = observation_row(
				call_observation_descriptor(), observation, output.exact_equivalence, limitation);
			if (!local)
				return sdk::unexpected(std::move(local.error()));
			output.call_observations.push_back(std::move(*local));
			if (!observation.source_span_id)
			{
				output.unresolved.push_back(
					{"provider.source-unavailable", observation.semantic_key, "cc.call_site"});
				continue;
			}
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
		auto expected_task_id = environment("CXXLENS_PROVIDER_TASK_ID");
		auto expected_task_digest = environment("CXXLENS_PROVIDER_TASK_INPUT_DIGEST");
		auto expected_invocation = environment("CXXLENS_PROVIDER_NORMALIZED_INVOCATION_DIGEST");
		auto expected_toolchain = environment("CXXLENS_PROVIDER_TOOLCHAIN_DIGEST");
		auto expected_environment = environment("CXXLENS_PROVIDER_ENVIRONMENT_DIGEST");
		auto expected_major = environment("CXXLENS_PROVIDER_PROTOCOL_MAJOR");
		auto expected_minor = environment("CXXLENS_PROVIDER_PROTOCOL_MINOR");
		if (!expected_manifest || !expected_task_id || !expected_task_digest ||
			!expected_invocation || !expected_toolchain || !expected_environment ||
			!expected_major || !expected_minor)
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
		const auto source_digest = sdk::content_digest(std::as_bytes(std::span{request->source}));
		auto catalog = sdk::project_catalog::make(".",
												  environment_digest,
												  {{request->compile_unit,
													task_control.normalized_invocation_digest,
													source_digest,
													environment_digest}});
		if (!catalog)
		{
			send_frontend_failure("project");
			return EXIT_SUCCESS;
		}
		std::vector outputs{entity_observation_descriptor(),
							type_observation_descriptor(),
							call_observation_descriptor(),
							cc::relations::entity::descriptor(),
							cc::relations::call_site::descriptor(),
							cc::relations::call_direct_target::descriptor()};
		auto session = sdk::provider::provider_session{std::string{provider_id},
													   provider_version,
													   std::string{provider_semantic_contract},
													   outputs,
													   {},
													   {"cc.clang22-canonical-1"},
													   "observation",
													   "assertion"};
		auto task = sdk::provider::task::make(std::move(session),
											  std::move(*catalog),
											  std::move(outputs),
											  "condition:all",
											  "cc.clang22-canonical-1",
											  {"canonical", "observation"});
		if (!task || task->task_id != task_id)
		{
			send_frontend_failure("task-id");
			return EXIT_SUCCESS;
		}
		canonical_provider provider{std::move(*request), toolchain_digest};
		(void)sdk::provider::run_worker(provider, *task, writer);
		return EXIT_SUCCESS;
	}
} // namespace cxxlens::detail::clang22
