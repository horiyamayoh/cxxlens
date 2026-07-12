#include "symbol_type_extractor.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../core/canonical_encoding.hpp"
#include "../../runtime/hash_port.hpp"
#include "call_relation_extractor.hpp"
#include "source_map_adapter.hpp"

#ifndef CXXLENS_HAS_CLANG22
#define CXXLENS_HAS_CLANG22 0
#endif

#if CXXLENS_HAS_CLANG22
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
#include <clang/AST/TypeLoc.h>
#include <clang/Basic/Linkage.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/APSInt.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringExtras.h>
#endif

namespace cxxlens::detail::clang22
{
	namespace
	{
		[[nodiscard]] error semantic_error(std::string reason)
		{
			error failure;
			failure.code.value = "extractor.invalid-observation";
			failure.message = "Semantic observation extraction failed";
			failure.scope = failure_scope::compile_unit;
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

#if CXXLENS_HAS_CLANG22
		[[nodiscard]] std::string bool_text(const bool value)
		{
			return value ? "true" : "false";
		}

		[[nodiscard]] std::string observation_key(const facts::observation_record& value)
		{
			const auto found = value.payload.find("semantic_key");
			return std::to_string(static_cast<std::uint16_t>(value.kind)) + ":" +
				(found == value.payload.end() ? std::string{} : found->second);
		}

		[[nodiscard]] std::string source_key(const std::optional<source_span>& source)
		{
			if (!source)
				return "source:unavailable";
			return "source:" + std::string{source->primary.begin.file.value()} + ":" +
				std::to_string(source->primary.begin.byte_offset) + ":" +
				std::to_string(source->primary.end.byte_offset);
		}

		[[nodiscard]] std::string symbol_kind_name(const clang::NamedDecl& declaration)
		{
			if (llvm::isa<clang::NamespaceDecl>(declaration))
				return "namespace";
			if (const auto* record = llvm::dyn_cast<clang::RecordDecl>(&declaration))
			{
				if (record->isUnion())
					return "union";
				if (record->isStruct())
					return "struct";
				return "class";
			}
			if (llvm::isa<clang::CXXConstructorDecl>(declaration))
				return "constructor";
			if (llvm::isa<clang::CXXDestructorDecl>(declaration))
				return "destructor";
			if (llvm::isa<clang::CXXMethodDecl>(declaration))
				return "method";
			if (llvm::isa<clang::FunctionDecl>(declaration))
				return "function";
			if (llvm::isa<clang::FieldDecl>(declaration))
				return "field";
			if (llvm::isa<clang::ParmVarDecl>(declaration))
				return "parameter";
			if (llvm::isa<clang::VarDecl>(declaration))
				return "variable";
			if (llvm::isa<clang::EnumDecl>(declaration))
				return "enum";
			if (llvm::isa<clang::EnumConstantDecl>(declaration))
				return "enum_constant";
			if (llvm::isa<clang::TypedefDecl>(declaration))
				return "typedef";
			if (llvm::isa<clang::TypeAliasDecl>(declaration))
				return "type_alias";
			if (llvm::isa<clang::TemplateDecl>(declaration))
				return "template";
			if (llvm::isa<clang::ConceptDecl>(declaration))
				return "concept";
			return "unknown";
		}

		[[nodiscard]] std::string linkage_name(const clang::NamedDecl& declaration)
		{
			switch (declaration.getFormalLinkage())
			{
				case clang::Linkage::None:
				case clang::Linkage::VisibleNone:
					return "none";
				case clang::Linkage::Internal:
					return "internal";
				case clang::Linkage::UniqueExternal:
					return "unique_external";
				case clang::Linkage::Module:
					return "module";
				case clang::Linkage::External:
					return "external";
				case clang::Linkage::Invalid:
					return "unknown";
			}
			return "unknown";
		}

		[[nodiscard]] bool is_definition(const clang::NamedDecl& declaration)
		{
			if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(&declaration))
				return function->isThisDeclarationADefinition();
			if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(&declaration))
				return variable->isThisDeclarationADefinition() == clang::VarDecl::Definition;
			if (const auto* tag = llvm::dyn_cast<clang::TagDecl>(&declaration))
				return tag->isThisDeclarationADefinition();
			return false;
		}

		[[nodiscard]] std::string access_name(const clang::AccessSpecifier access)
		{
			switch (access)
			{
				case clang::AS_public:
					return "public";
				case clang::AS_protected:
					return "protected";
				case clang::AS_private:
					return "private";
				case clang::AS_none:
					return "none";
			}
			return "none";
		}
#endif
	} // namespace

#if CXXLENS_HAS_CLANG22
	struct semantic_identity_adapter::impl
	{
		clang::CompilerInstance& compiler;
		const compile_unit& unit;
		source_map_adapter sources;
		runtime::fnv1a_hash_adapter hashes;
		identity::collision_registry collisions;
		std::map<const clang::NamedDecl*, detached_symbol_identity> symbols;
		std::map<std::string, detached_type_identity> types;

		impl(clang::CompilerInstance& compiler_value,
			 const compile_unit& unit_value,
			 const std::vector<frontend::virtual_source_file>& virtual_files)
			: compiler{compiler_value}, unit{unit_value},
			  sources{compiler_value, unit_value, virtual_files}
		{
		}

		[[nodiscard]] result<std::string>
		make_id(const std::string& prefix,
				const std::string& domain,
				const std::vector<std::pair<std::string, std::string>>& fields)
		{
			identity::canonical_encoder encoder{domain, {1U, 1U}};
			for (const auto& [name, value] : fields)
				encoder.string_field(name, value);
			auto payload = encoder.finish();
			if (!payload)
				return semantic_error("canonical-identity-encoding-failed");
			identity::identity_service identities{hashes};
			auto id = identities.make_id(prefix, payload.value(), collisions);
			if (!id)
				return semantic_error("canonical-identity-hash-failed");
			return id.value();
		}

		[[nodiscard]] std::string normalize_usr(std::string value) const
		{
			const auto root = unit.command().directory.lexically_normal().generic_string();
			for (auto found = value.find(root); found != std::string::npos;
				 found = value.find(root, found))
				value.replace(found, root.size(), "project-root");
			return value;
		}

		[[nodiscard]] result<std::string>
		declaration_source_key(const clang::NamedDecl& declaration)
		{
			if (declaration.getLocation().isInvalid())
				return std::string{"source:implicit"};
			auto normalized = sources.direct_span(declaration.getSourceRange());
			if (!normalized)
				return std::string{"source:unavailable"};
			return source_key(normalized.value());
		}

		[[nodiscard]] result<detached_symbol_identity> build_symbol(const clang::NamedDecl& input)
		{
			const auto* canonical = llvm::cast<clang::NamedDecl>(input.getCanonicalDecl());
			if (const auto found = symbols.find(canonical); found != symbols.end())
				return found->second;

			llvm::SmallString<256> usr_buffer;
			std::optional<std::string> usr;
			if (!clang::index::generateUSRForDecl(canonical, usr_buffer) && !usr_buffer.empty())
				usr = normalize_usr(usr_buffer.str().str());

			std::string owner = "translation-unit";
			if (const auto* owner_decl =
					llvm::dyn_cast<clang::NamedDecl>(canonical->getDeclContext()))
			{
				auto owner_identity = build_symbol(*owner_decl);
				if (!owner_identity)
					return owner_identity.error();
				owner = std::string{owner_identity.value().id.value()};
			}

			std::string signature =
				symbol_kind_name(*canonical) + ":" + canonical->getNameAsString();
			if (const auto* value = llvm::dyn_cast<clang::ValueDecl>(canonical))
			{
				const auto type = value->getType().getCanonicalType();
				signature += ":type-class=" + std::to_string(type->getTypeClass());
			}
			auto location_key = declaration_source_key(*canonical);
			if (!location_key)
				return location_key.error();
			signature += ":" + location_key.value();

			std::vector<std::pair<std::string, std::string>> fields;
			if (usr)
				fields.emplace_back("usr", *usr);
			else
			{
				fields.emplace_back("semantic_owner", owner);
				fields.emplace_back("declaration_kind", symbol_kind_name(*canonical));
				fields.emplace_back("signature_structure", signature);
			}
			auto generated = make_id("symbol", "cxxlens.semantic-symbol.v1", fields);
			if (!generated)
				return generated.error();

			detached_symbol_identity output;
			output.id = symbol_id{generated.value()};
			output.name.display_qualified_name = canonical->getQualifiedNameAsString();
			output.name.usr = usr;
			if (!usr)
			{
				output.name.semantic_owner = owner;
				output.name.declaration_kind = symbol_kind_name(*canonical);
				output.name.signature_structure = signature;
			}
			output.kind = symbol_kind_name(*canonical);
			output.linkage = linkage_name(*canonical);
			symbols.emplace(canonical, output);
			return output;
		}

		struct type_structure
		{
			std::string text;
			std::optional<symbol_id> declaration;
			std::vector<type_id> components;
			std::string kind;
			bool builtin{};
			bool pointer{};
			bool reference{};
			bool dependent{};
		};

		[[nodiscard]] result<type_id>
		component_id(const std::string& structure,
					 const std::string& domain = "cxxlens.semantic-type.v1")
		{
			auto generated = make_id("type", domain, {{"canonical_structure", structure}});
			if (!generated)
				return generated.error();
			return type_id{generated.value()};
		}

		[[nodiscard]] result<type_structure> structure(clang::QualType input,
													   const std::uint32_t depth)
		{
			if (input.isNull() || depth > 64U)
				return semantic_error("invalid-or-recursive-type");
			const auto qualifiers = input.getLocalQualifiers();
			const auto canonical = input.getCanonicalType();
			const auto* raw = canonical.getTypePtrOrNull();
			if (raw == nullptr)
				return semantic_error("null-canonical-type");
			type_structure output;
			output.dependent = raw->isDependentType();
			const auto prefix = std::string{"q("} + (qualifiers.hasConst() ? "c" : "-") +
				(qualifiers.hasVolatile() ? "v" : "-") + (qualifiers.hasRestrict() ? "r" : "-") +
				"):";

			auto append_child = [&](clang::QualType child) -> result<std::string>
			{
				auto nested = structure(child, depth + 1U);
				if (!nested)
					return nested.error();
				auto id = component_id(nested.value().text);
				if (!id)
					return id.error();
				output.components.push_back(id.value());
				return nested.value().text;
			};

			if (const auto* builtin = llvm::dyn_cast<clang::BuiltinType>(raw))
			{
				output.kind = "builtin";
				output.builtin = true;
				output.text = prefix + "builtin(" + std::to_string(builtin->getKind()) + ")";
			}
			else if (const auto* pointer = llvm::dyn_cast<clang::PointerType>(raw))
			{
				auto pointee = append_child(pointer->getPointeeType());
				if (!pointee)
					return pointee.error();
				output.kind = "pointer";
				output.pointer = true;
				output.text = prefix + "pointer(" + pointee.value() + ")";
			}
			else if (const auto* reference = llvm::dyn_cast<clang::ReferenceType>(raw))
			{
				auto referred = append_child(reference->getPointeeType());
				if (!referred)
					return referred.error();
				output.kind = llvm::isa<clang::LValueReferenceType>(reference) ? "lvalue_reference"
																			   : "rvalue_reference";
				output.reference = true;
				output.text = prefix + output.kind + "(" + referred.value() + ")";
			}
			else if (const auto* array = llvm::dyn_cast<clang::ArrayType>(raw))
			{
				auto element = append_child(array->getElementType());
				if (!element)
					return element.error();
				std::string extent = "unknown";
				if (const auto* constant = llvm::dyn_cast<clang::ConstantArrayType>(array))
					extent = llvm::toString(constant->getSize(), 10U, false);
				output.kind = "array";
				output.text = prefix + "array(" + extent + ";" + element.value() + ")";
			}
			else if (const auto* function = llvm::dyn_cast<clang::FunctionProtoType>(raw))
			{
				auto result_type = append_child(function->getReturnType());
				if (!result_type)
					return result_type.error();
				output.kind = "function";
				output.text = prefix + "function(result=" + result_type.value() + ";params=[";
				for (const auto parameter : function->param_types())
				{
					auto nested = append_child(parameter);
					if (!nested)
						return nested.error();
					output.text += nested.value() + ";";
				}
				output.text += "];variadic=" + bool_text(function->isVariadic()) +
					";ref=" + std::to_string(function->getRefQualifier()) +
					";exception=" + std::to_string(function->getExceptionSpecType()) +
					";calling-convention=" + std::to_string(function->getCallConv()) + ")";
			}
			else if (const auto* record = raw->getAs<clang::RecordType>())
			{
				auto declaration = build_symbol(*record->getDecl());
				if (!declaration)
					return declaration.error();
				output.kind = "record";
				output.declaration = declaration.value().id;
				output.text = prefix + "record(" + std::string{output.declaration->value()} + ")";
			}
			else if (const auto* enumeration = raw->getAs<clang::EnumType>())
			{
				auto declaration = build_symbol(*enumeration->getDecl());
				if (!declaration)
					return declaration.error();
				output.kind = "enum";
				output.declaration = declaration.value().id;
				output.text = prefix + "enum(" + std::string{output.declaration->value()} + ")";
			}
			else if (const auto* parameter = llvm::dyn_cast<clang::TemplateTypeParmType>(raw))
			{
				output.kind = "template_parameter";
				output.text = prefix +
					"template-parameter(depth=" + std::to_string(parameter->getDepth()) +
					";index=" + std::to_string(parameter->getIndex()) +
					";pack=" + bool_text(parameter->isParameterPack()) + ")";
			}
			else
			{
				output.kind = output.dependent ? "dependent" : "unknown";
				output.text = prefix + "type-class(" + std::to_string(raw->getTypeClass()) +
					";dependent=" + bool_text(output.dependent) + ")";
			}

			if (!output.builtin && !output.declaration && output.components.empty())
			{
				auto atom = component_id(output.text, "cxxlens.semantic-type-atom.v1");
				if (!atom)
					return atom.error();
				output.components.push_back(atom.value());
			}
			std::ranges::sort(output.components,
							  {},
							  [](const type_id& value)
							  {
								  return value.value();
							  });
			const auto duplicate = std::ranges::unique(output.components);
			output.components.erase(duplicate.begin(), duplicate.end());
			return output;
		}

		[[nodiscard]] result<detached_type_identity> build_type(const clang::QualType& input)
		{
			auto built = structure(input, 0U);
			if (!built)
				return built.error();
			if (const auto found = types.find(built.value().text); found != types.end())
				return found->second;
			auto generated = component_id(built.value().text);
			if (!generated)
				return generated.error();
			detached_type_identity output;
			output.id = generated.value();
			output.type.display_spelling = input.getAsString();
			output.type.canonical_structure = built.value().text;
			output.type.declaration = built.value().declaration;
			output.type.components = built.value().components;
			output.type.builtin = built.value().builtin;
			output.kind = built.value().kind;
			output.is_const = input.isConstQualified();
			output.is_volatile = input.isVolatileQualified();
			output.is_pointer = built.value().pointer;
			output.is_reference = built.value().reference;
			output.dependent = built.value().dependent;
			types.emplace(built.value().text, output);
			return output;
		}
	};
#else
	struct semantic_identity_adapter::impl
	{
	};
#endif

	semantic_identity_adapter::semantic_identity_adapter(
		clang::CompilerInstance& compiler,
		const compile_unit& unit,
		const std::vector<frontend::virtual_source_file>& virtual_files)
#if CXXLENS_HAS_CLANG22
		: impl_{std::make_unique<impl>(compiler, unit, virtual_files)}
#else
		: impl_{std::make_unique<impl>()}
#endif
	{
		(void)compiler;
		(void)unit;
		(void)virtual_files;
	}

	semantic_identity_adapter::~semantic_identity_adapter() = default;
	semantic_identity_adapter::semantic_identity_adapter(semantic_identity_adapter&&) noexcept =
		default;
	semantic_identity_adapter&
	semantic_identity_adapter::operator=(semantic_identity_adapter&&) noexcept = default;

	result<detached_symbol_identity>
	semantic_identity_adapter::symbol(const clang::NamedDecl& declaration)
	{
#if CXXLENS_HAS_CLANG22
		return impl_->build_symbol(declaration);
#else
		(void)declaration;
		return semantic_error("clang22-not-linked");
#endif
	}

	result<detached_type_identity> semantic_identity_adapter::type(const clang::QualType& value)
	{
#if CXXLENS_HAS_CLANG22
		return impl_->build_type(value);
#else
		(void)value;
		return semantic_error("clang22-not-linked");
#endif
	}

	result<source_span> semantic_identity_adapter::source(const clang::SourceRange& range) const
	{
#if !CXXLENS_HAS_CLANG22
		(void)range;
		return semantic_error("clang22-not-linked");
#else
		if (range.isInvalid())
			return semantic_error("invalid-semantic-source");
		const auto begin = range.getBegin();
		if (!begin.isMacroID())
			return impl_->sources.direct_span(range);
		const auto& manager = impl_->compiler.getSourceManager();
		const auto invocation = manager.getImmediateExpansionRange(begin).getAsRange();
		const auto macro_name =
			clang::Lexer::getImmediateMacroName(begin, manager, impl_->compiler.getLangOpts())
				.str();
		clang::SourceLocation argument_start;
		const auto origin = manager.isMacroArgExpansion(begin, &argument_start)
			? source_origin::macro_argument
			: source_origin::macro_body;
		return impl_->sources.macro_span(
			range, invocation, nullptr, macro_name.empty() ? "unknown-macro" : macro_name, origin);
#endif
	}

	facts::observation_record
	semantic_identity_adapter::observation(const fact_kind kind,
										   std::string semantic_key,
										   std::optional<source_span> source_value) const
	{
		facts::observation_record output;
		output.adapter_id = "clang22.frontend";
		output.adapter_version = "1.0.0";
		output.llvm_major = 22U;
#if CXXLENS_HAS_CLANG22
		output.compile_unit = impl_->unit.id();
		output.variant = impl_->unit.variant_id();
#endif
		output.kind = kind;
		output.source = std::move(source_value);
		output.payload_version = 1U;
		output.payload.emplace("extractor.id", "clang22.semantic");
		output.payload.emplace("extractor.version", "1.0.0");
		output.payload.emplace("semantic_key", std::move(semantic_key));
#if CXXLENS_HAS_CLANG22
		output.payload.emplace("variant", std::string{impl_->unit.variant_id().value()});
#endif
		return output;
	}

	void semantic_identity_adapter::mark(facts::observation_record& observation,
										 const coverage_state state,
										 std::optional<std::string> reason) const
	{
		observation.coverage_contributions.push_back({"semantic-observation",
													  observation.payload.at("semantic_key"),
													  state,
													  std::move(reason)});
	}

	clang::CompilerInstance& semantic_identity_adapter::compiler() const noexcept
	{
#if CXXLENS_HAS_CLANG22
		return impl_->compiler;
#else
		std::terminate();
#endif
	}

	const compile_unit& semantic_identity_adapter::unit() const noexcept
	{
#if CXXLENS_HAS_CLANG22
		return impl_->unit;
#else
		std::terminate();
#endif
	}

#if CXXLENS_HAS_CLANG22
	namespace
	{
		class symbol_type_visitor final : public clang::RecursiveASTVisitor<symbol_type_visitor>
		{
		  public:
			symbol_type_visitor(clang::ASTContext& context, semantic_identity_adapter& identities)
				: context_{context}, identities_{identities}
			{
			}

			bool VisitNamedDecl(clang::NamedDecl* declaration)
			{
				if (declaration == nullptr || declaration->getLocation().isInvalid())
					return true;
				auto identity = identities_.symbol(*declaration);
				if (!identity)
					return fail(std::move(identity.error()));
				auto source = identities_.source(declaration->getSourceRange());
				const auto normalized = source ? std::optional{source.value()} : std::nullopt;
				const auto occurrence = source_key(normalized);

				auto symbol =
					identities_.observation(fact_kind::symbol,
											"symbol:" + std::string{identity.value().id.value()},
											normalized);
				symbol.name = identity.value().name;
				symbol.payload.emplace("symbol.id", std::string{identity.value().id.value()});
				symbol.payload.emplace("symbol.kind", identity.value().kind);
				symbol.payload.emplace("symbol.linkage", identity.value().linkage);
				symbol.payload.emplace("symbol.name", declaration->getNameAsString());
				symbol.payload.emplace("symbol.qualified_name",
									   declaration->getQualifiedNameAsString());
				symbol.payload.emplace("symbol.implicit", bool_text(declaration->isImplicit()));
				symbol.payload.emplace("symbol.definition", bool_text(is_definition(*declaration)));
				identities_.mark(symbol);
				observations_.push_back(std::move(symbol));

				auto occurrence_observation = identities_.observation(
					is_definition(*declaration) ? fact_kind::definition : fact_kind::declaration,
					(is_definition(*declaration) ? "definition:" : "declaration:") +
						std::string{identity.value().id.value()} + ":" + occurrence,
					normalized);
				occurrence_observation.name = identity.value().name;
				occurrence_observation.payload.emplace("symbol.id",
													   std::string{identity.value().id.value()});
				occurrence_observation.payload.emplace(
					"declaration.canonical",
					declaration->getCanonicalDecl() == declaration ? "true" : "false");
				identities_.mark(occurrence_observation,
								 normalized ? coverage_state::covered : coverage_state::unresolved,
								 normalized ? std::nullopt
											: std::optional<std::string>{"source-unavailable"});
				observations_.push_back(std::move(occurrence_observation));

				if (const auto* value = llvm::dyn_cast<clang::ValueDecl>(declaration))
					return emit_type(value->getType(), normalized);
				if (const auto* type_declaration = llvm::dyn_cast<clang::TypeDecl>(declaration))
					return emit_type(context_.getTypeDeclType(type_declaration), normalized);
				return true;
			}

			bool VisitTypeLoc(clang::TypeLoc location)
			{
				if (location.isNull())
					return true;
				auto source = identities_.source(location.getSourceRange());
				return emit_type(location.getType(),
								 source ? std::optional{source.value()} : std::nullopt);
			}

			bool VisitCXXRecordDecl(clang::CXXRecordDecl* declaration)
			{
				if (declaration == nullptr || !declaration->isThisDeclarationADefinition())
					return true;
				auto derived = identities_.symbol(*declaration);
				if (!derived)
					return fail(std::move(derived.error()));
				for (const auto& base : declaration->bases())
				{
					auto base_type = identities_.type(base.getType());
					if (!base_type)
						return fail(std::move(base_type.error()));
					std::optional<detached_symbol_identity> base_symbol;
					if (const auto* record = base.getType()->getAsCXXRecordDecl())
					{
						auto identity = identities_.symbol(*record);
						if (!identity)
							return fail(std::move(identity.error()));
						base_symbol = std::move(identity.value());
					}
					const auto range = base.getTypeSourceInfo() == nullptr
						? clang::SourceRange{base.getBeginLoc(), base.getEndLoc()}
						: base.getTypeSourceInfo()->getTypeLoc().getSourceRange();
					auto source = identities_.source(range);
					const auto normalized = source ? std::optional{source.value()} : std::nullopt;
					const auto target = base_symbol ? std::string{base_symbol->id.value()}
													: std::string{base_type.value().id.value()};
					auto observation = identities_.observation(
						fact_kind::inheritance,
						"inheritance:" + std::string{derived.value().id.value()} + ":" + target +
							":" + source_key(normalized),
						normalized);
					observation.payload.emplace("inheritance.derived",
												std::string{derived.value().id.value()});
					observation.payload.emplace("inheritance.base_type",
												std::string{base_type.value().id.value()});
					observation.payload.emplace("inheritance.base",
												base_symbol ? std::string{base_symbol->id.value()}
															: "unresolved");
					observation.payload.emplace("inheritance.access",
												access_name(base.getAccessSpecifier()));
					observation.payload.emplace("inheritance.virtual", bool_text(base.isVirtual()));
					observation.payload.emplace("inheritance.dependent",
												bool_text(base.getType()->isDependentType()));
					observation.payload.emplace("inheritance.direct", "true");
					identities_.mark(
						observation,
						base_symbol ? coverage_state::covered : coverage_state::unresolved,
						base_symbol ? std::nullopt
									: std::optional<std::string>{"dependent-or-unresolved-base"});
					observations_.push_back(std::move(observation));
				}
				return true;
			}

			bool VisitCXXMethodDecl(clang::CXXMethodDecl* declaration)
			{
				if (declaration == nullptr)
					return true;
				auto overriding = identities_.symbol(*declaration);
				if (!overriding)
					return fail(std::move(overriding.error()));
				for (const auto* overridden : declaration->overridden_methods())
				{
					auto base = identities_.symbol(*overridden);
					if (!base)
						return fail(std::move(base.error()));
					auto source = identities_.source(declaration->getSourceRange());
					const auto normalized = source ? std::optional{source.value()} : std::nullopt;
					auto observation = identities_.observation(
						fact_kind::override_relation,
						"override:" + std::string{overriding.value().id.value()} + ":" +
							std::string{base.value().id.value()} + ":" + source_key(normalized),
						normalized);
					observation.payload.emplace("override.overriding",
												std::string{overriding.value().id.value()});
					observation.payload.emplace("override.overridden",
												std::string{base.value().id.value()});
					observation.payload.emplace("override.direct", "true");
					identities_.mark(observation);
					observations_.push_back(std::move(observation));
				}
				return true;
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
			bool emit_type(clang::QualType value, std::optional<source_span> source)
			{
				if (value.isNull())
					return true;
				auto identity = identities_.type(value);
				if (!identity)
					return fail(std::move(identity.error()));
				auto observation =
					identities_.observation(fact_kind::type,
											"type:" + std::string{identity.value().id.value()},
											std::move(source));
				observation.type = identity.value().type;
				observation.payload.emplace("type.id", std::string{identity.value().id.value()});
				observation.payload.emplace("type.kind", identity.value().kind);
				observation.payload.emplace("type.const", bool_text(identity.value().is_const));
				observation.payload.emplace("type.volatile",
											bool_text(identity.value().is_volatile));
				observation.payload.emplace("type.indirection",
											bool_text(identity.value().is_pointer));
				observation.payload.emplace("type.reference",
											bool_text(identity.value().is_reference));
				observation.payload.emplace("type.dependent",
											bool_text(identity.value().dependent));
				identities_.mark(observation);
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
			std::vector<facts::observation_record> observations_;
			std::optional<error> error_;
		};

		class extraction_session final : public semantic_extraction_session
		{
		  public:
			extraction_session(clang::CompilerInstance& compiler,
							   const compile_unit& unit,
							   const std::vector<frontend::virtual_source_file>& virtual_files)
				: identities_{compiler, unit, virtual_files}
			{
			}

			result<void> consume(clang::ASTContext& context) override
			{
				if (consumed_)
					return semantic_error("semantic-session-consumed-twice");
				consumed_ = true;
				symbol_type_visitor visitor{context, identities_};
				if (!visitor.TraverseDecl(context.getTranslationUnitDecl()))
				{
					if (auto failure = visitor.take_error())
						return std::move(*failure);
					return semantic_error("symbol-type-traversal-failed");
				}
				observations_ = visitor.take();
				auto relations = extract_call_relation_observations(context, identities_);
				if (!relations)
					return std::move(relations.error());
				observations_.insert(observations_.end(),
									 std::make_move_iterator(relations.value().begin()),
									 std::make_move_iterator(relations.value().end()));
				return {};
			}

			result<std::vector<facts::observation_record>> take() override
			{
				if (!consumed_)
					return semantic_error("semantic-session-not-consumed");
				std::ranges::sort(observations_, {}, observation_key);
				const auto duplicate = std::ranges::unique(observations_, {}, observation_key);
				observations_.erase(duplicate.begin(), duplicate.end());
				for (const auto& observation : observations_)
					if (auto checked = facts::validate(observation); !checked)
						return std::move(checked.error());
				return std::move(observations_);
			}

		  private:
			semantic_identity_adapter identities_;
			std::vector<facts::observation_record> observations_;
			bool consumed_{};
		};
	} // namespace
#endif

	class unavailable_session final : public semantic_extraction_session
	{
	  public:
		result<void> consume(clang::ASTContext&) override
		{
			return semantic_error("clang22-not-linked");
		}
		result<std::vector<facts::observation_record>> take() override
		{
			return semantic_error("clang22-not-linked");
		}
	};

	result<std::unique_ptr<semantic_extraction_session>>
	make_semantic_extractor(clang::CompilerInstance& compiler,
							const compile_unit& unit,
							const std::vector<frontend::virtual_source_file>& virtual_files)
	{
#if CXXLENS_HAS_CLANG22
		return std::unique_ptr<semantic_extraction_session>{
			std::make_unique<extraction_session>(compiler, unit, virtual_files)};
#else
		(void)compiler;
		(void)unit;
		(void)virtual_files;
		return std::unique_ptr<semantic_extraction_session>{
			std::make_unique<unavailable_session>()};
#endif
	}
} // namespace cxxlens::detail::clang22
