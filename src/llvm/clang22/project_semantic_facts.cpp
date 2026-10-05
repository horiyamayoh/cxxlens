#include "project_semantic_facts.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

#include <cxxlens/relations/cc_abi_surface.hpp>
#include <cxxlens/relations/cc_body.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_operand.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_cfg_edge.hpp>
#include <cxxlens/relations/cc_cfg_node.hpp>
#include <cxxlens/relations/cc_declaration.hpp>
#include <cxxlens/relations/cc_declaration_inventory.hpp>
#include <cxxlens/relations/cc_entity_detail.hpp>
#include <cxxlens/relations/cc_entity_edge.hpp>
#include <cxxlens/relations/cc_flow_fact.hpp>
#include <cxxlens/relations/cc_flow_inventory.hpp>
#include <cxxlens/relations/cc_layout_fact.hpp>
#include <cxxlens/relations/cc_record_inventory.hpp>
#include <cxxlens/relations/cc_record_surface.hpp>
#include <cxxlens/relations/cc_syntax_node.hpp>
#include <cxxlens/relations/cc_type.hpp>
#include <cxxlens/relations/cc_type_component.hpp>
#include <cxxlens/relations/source_comment.hpp>
#include <cxxlens/relations/source_comment_inventory.hpp>
#include <cxxlens/relations/source_include.hpp>
#include <cxxlens/relations/source_include_inventory.hpp>
#include <cxxlens/relations/source_preprocessor_event.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <cxxlens/relations/source_token.hpp>
#include <cxxlens/relations/source_token_inventory.hpp>

#include "project_abi_observer.hpp"
#include "sdk/bounded_json_internal.hpp"
#include "sdk/source_identity_internal.hpp"
#include "source_closure_vfs.hpp"

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
#include <clang/AST/Attr.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Analysis/CFG.h>
#include <clang/Basic/Module.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/MacroInfo.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/PreprocessingRecord.h>
#include <clang/Lex/Preprocessor.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#endif

namespace cxxlens::detail::clang22
{
	namespace
	{
		struct extraction_failure
		{
			sdk::error value;
		};
		template <class T>
		T take(sdk::result<T> value)
		{
			if (!value)
				throw extraction_failure{std::move(value.error())};
			return std::move(*value);
		}
		void check(sdk::result<void> value)
		{
			if (!value)
				throw extraction_failure{std::move(value.error())};
		}
		[[noreturn]] void fail(std::string field, std::string reason)
		{
			throw extraction_failure{
				{"application-analysis.cpp-facts-invalid", std::move(field), std::move(reason)}};
		}
		using fields = std::map<std::string, sdk::detached_cell, std::less<>>;
		sdk::detached_cell id(std::string type, std::string value)
		{
			return sdk::detached_cell::typed(std::move(type), std::move(value));
		}
		sdk::detached_cell symbol(std::string domain, std::string value)
		{
			return {{sdk::scalar_kind::open_symbol, std::move(domain), false},
					sdk::cell_state::present,
					sdk::scalar_value{std::move(value)},
					std::nullopt};
		}
		sdk::detached_cell flags(std::string domain, std::set<std::string, std::less<>> values)
		{
			std::vector<std::byte> encoded;
			for (const auto& value : values)
			{
				if (value.size() > std::numeric_limits<std::uint32_t>::max())
					fail("flags", "length");
				const auto size = static_cast<std::uint32_t>(value.size());
				for (unsigned shift = 0U; shift < 32U; shift += 8U)
					encoded.push_back(static_cast<std::byte>((size >> shift) & 255U));
				for (const auto c : value)
					encoded.push_back(static_cast<std::byte>(c));
			}
			return {{sdk::scalar_kind::set, "open_symbol<" + domain + ">", false},
					sdk::cell_state::present,
					sdk::scalar_value{std::move(encoded)},
					std::nullopt};
		}
		sdk::detached_row make_row(const sdk::relation_descriptor& descriptor, fields values)
		{
			sdk::detached_row row{descriptor.id, {}};
			for (const auto& column : descriptor.columns)
			{
				auto found = values.find(column.name);
				if (found != values.end())
				{
					found->second.type = column.type;
					row.cells.emplace(column.id, std::move(found->second));
				}
				else if (column.type.optional)
					row.cells.emplace(column.id, sdk::detached_cell::absent(column.type));
			}
			if (descriptor.domain_identity.result_column)
			{
				const auto& result = *descriptor.domain_identity.result_column;
				const auto column = take(descriptor.column(result));
				row.cells.insert_or_assign(
					result,
					id(column.type.parameter, take(sdk::derive_domain_identity(descriptor, row))));
			}
			check(sdk::validate_row(descriptor, row));
			if (descriptor.domain_identity.result_column)
				check(sdk::validate_domain_identity(descriptor, row));
			return row;
		}
		std::string row_id(const sdk::detached_row& row, std::string_view column)
		{
			return std::get<std::string>(
				*row.cells.at(row.descriptor_id + "." + std::string{column}).value);
		}
		sdk::detached_cell digest_value(std::string value)
		{
			return {{sdk::scalar_kind::digest, {}, false},
					sdk::cell_state::present,
					sdk::scalar_value{std::move(value)},
					std::nullopt};
		}

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
		std::string project_path(std::string_view path)
		{
			const auto root = source_closure_vfs::synthetic_root();
			if (path.starts_with("project://"))
				return std::string{path};
			if (path.starts_with(root) && path.size() > root.size() && path[root.size()] == '/')
				return "project://" + std::string{path.substr(root.size() + 1U)};
			return {};
		}
		bool written_lambda(const clang::NamedDecl* declaration)
		{
			const auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(declaration);
			return method && method->getParent()->isLambda() &&
				method == method->getParent()->getLambdaCallOperator();
		}
		clang::TemplateSpecializationKind specialization_kind(const clang::NamedDecl* declaration)
		{
			if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(declaration))
				return function->getTemplateSpecializationKind();
			if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(declaration))
				return record->getTemplateSpecializationKind();
			if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration))
				return variable->getTemplateSpecializationKind();
			return clang::TSK_Undeclared;
		}
		bool declaration_population_admitted(const clang::NamedDecl* declaration)
		{
			if (specialization_kind(declaration) == clang::TSK_ImplicitInstantiation)
				return false;
			for (auto* context = declaration->getDeclContext();
				 context && !context->isTranslationUnit();
				 context = context->getParent())
				if (const auto* owner =
						llvm::dyn_cast<clang::NamedDecl>(clang::Decl::castFromDeclContext(context)))
				{
					const auto kind = specialization_kind(owner);
					if (kind == clang::TSK_ImplicitInstantiation ||
						kind == clang::TSK_ExplicitInstantiationDeclaration ||
						kind == clang::TSK_ExplicitInstantiationDefinition)
						return false;
				}
			return true;
		}
		void declaration_categories(const clang::NamedDecl& declaration,
									std::set<std::string, std::less<>>& properties)
		{
			properties.emplace("finite_declarations_v1");
			if (declaration_population_admitted(&declaration))
				properties.emplace("declaration_population_admitted");
			if (written_lambda(&declaration))
				properties.emplace("written_lambda");
			bool primary = false;
			if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(&declaration))
				primary = record->getDescribedClassTemplate() != nullptr;
			if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(&declaration))
				primary = function->getDescribedFunctionTemplate() != nullptr;
			if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(&declaration))
				primary = variable->getDescribedVarTemplate() != nullptr;
			if (const auto* alias = llvm::dyn_cast<clang::TypeAliasDecl>(&declaration))
				primary = alias->getDescribedAliasTemplate() != nullptr;
			if (primary)
				properties.emplace("template_primary");
			if (llvm::isa<clang::ConceptDecl>(&declaration))
				properties.emplace("concept");
			const bool partial =
				llvm::isa<clang::ClassTemplatePartialSpecializationDecl,
						  clang::VarTemplatePartialSpecializationDecl>(&declaration);
			if (partial)
				properties.emplace("template_partial_specialization");
			switch (specialization_kind(&declaration))
			{
				case clang::TSK_ExplicitSpecialization:
					if (!partial)
						properties.emplace("template_full_specialization");
					break;
				case clang::TSK_ExplicitInstantiationDeclaration:
					properties.emplace("template_explicit_instantiation_declaration");
					break;
				case clang::TSK_ExplicitInstantiationDefinition:
					properties.emplace("template_explicit_instantiation_definition");
					break;
				case clang::TSK_ImplicitInstantiation:
					properties.emplace("template_implicit_instantiation");
					break;
				case clang::TSK_Undeclared:
					break;
			}
			if (!declaration_population_admitted(&declaration))
				properties.emplace("template_implicit_instantiation");
		}
		std::string usr(const clang::NamedDecl* declaration)
		{
			if (declaration == nullptr)
				return {};
			llvm::SmallString<256U> value;
			if (const auto* parameter = llvm::dyn_cast<clang::ParmVarDecl>(declaration))
				if (const auto* function =
						llvm::dyn_cast<clang::FunctionDecl>(parameter->getDeclContext()))
					if (!clang::index::generateUSRForDecl(function->getCanonicalDecl(), value) &&
						!value.empty())
					{
						if (value.size() > 65536U)
							fail("identity", "usr-byte-limit");
						return "clang-parameter-slot:" + value.str().str() +
							"#parameter:" + std::to_string(parameter->getFunctionScopeIndex());
					}
			value.clear();
			if (clang::index::generateUSRForDecl(declaration->getCanonicalDecl(), value) ||
				value.empty())
				return {};
			if (value.size() > 65536U)
				fail("identity", "usr-byte-limit");
			return "clang-usr:" + value.str().str();
		}
		std::string access(const clang::AccessSpecifier value)
		{
			switch (value)
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
			return "unknown";
		}
		std::string storage(clang::StorageClass value)
		{
			switch (value)
			{
				case clang::SC_None:
					return "none";
				case clang::SC_Extern:
					return "extern";
				case clang::SC_Static:
					return "static";
				case clang::SC_PrivateExtern:
					return "private_extern";
				case clang::SC_Auto:
					return "auto";
				case clang::SC_Register:
					return "register";
			}
			return "unknown";
		}
		class project_pp_callbacks final : public clang::PPCallbacks
		{
		  public:
			project_pp_callbacks(clang::Preprocessor& preprocessor,
								 const source_closure_snapshot& closure,
								 project_preprocessor_observations& output)
				: pp_{preprocessor}, closure_{closure}, output_{output}
			{
			}
			void FileChanged(clang::SourceLocation location,
							 FileChangeReason reason,
							 clang::SrcMgr::CharacteristicKind,
							 clang::FileID) override
			{
				if (reason != EnterFile)
					return;
				const auto logical = project_path(pp_.getSourceManager().getFilename(location));
				if (closure_.find_member(logical))
					output_.opened_files.insert(logical);
			}
			void MacroDefined(const clang::Token& token,
							  const clang::MacroDirective* directive) override
			{
				const auto* macro = directive ? directive->getMacroInfo() : nullptr;
				if (!macro)
					return;
				add({token.getLocation(), macro->getDefinitionEndLoc()},
					"macro_definition",
					name(token),
					text({token.getLocation(), macro->getDefinitionEndLoc()}),
					"observed");
			}
			void MacroUndefined(const clang::Token& token,
								const clang::MacroDefinition&,
								const clang::MacroDirective*) override
			{
				add({token.getLocation(), token.getLocation()},
					"macro_undefinition",
					name(token),
					{},
					"observed");
			}
			void MacroExpands(const clang::Token& token,
							  const clang::MacroDefinition&,
							  clang::SourceRange range,
							  const clang::MacroArgs*) override
			{
				add(range, "macro_expansion", name(token), text(range), "observed");
			}
			void Defined(const clang::Token& token,
						 const clang::MacroDefinition& definition,
						 clang::SourceRange range) override
			{
				add(range,
					"defined_test",
					name(token),
					{},
					definition.getMacroInfo() ? "true" : "false");
			}
			void If(clang::SourceLocation location,
					clang::SourceRange range,
					ConditionValueKind value) override
			{
				condition(location, range, "if", value);
			}
			void Elif(clang::SourceLocation location,
					  clang::SourceRange range,
					  ConditionValueKind value,
					  clang::SourceLocation) override
			{
				condition(location, range, "elif", value);
			}
			void Ifdef(clang::SourceLocation location,
					   const clang::Token& token,
					   const clang::MacroDefinition& definition) override
			{
				add({location, token.getLocation()},
					"ifdef",
					name(token),
					{},
					definition.getMacroInfo() ? "true" : "false");
			}
			void Ifndef(clang::SourceLocation location,
						const clang::Token& token,
						const clang::MacroDefinition& definition) override
			{
				add({location, token.getLocation()},
					"ifndef",
					name(token),
					{},
					definition.getMacroInfo() ? "false" : "true");
			}
			void Elifdef(clang::SourceLocation location,
						 const clang::Token& token,
						 const clang::MacroDefinition& definition) override
			{
				add({location, token.getLocation()},
					"elifdef",
					name(token),
					{},
					definition.getMacroInfo() ? "true" : "false");
			}
			void Elifdef(clang::SourceLocation location,
						 clang::SourceRange range,
						 clang::SourceLocation) override
			{
				add({location, range.getEnd()}, "elifdef", text(range), {}, "not_evaluated");
			}
			void Elifndef(clang::SourceLocation location,
						  const clang::Token& token,
						  const clang::MacroDefinition& definition) override
			{
				add({location, token.getLocation()},
					"elifndef",
					name(token),
					{},
					definition.getMacroInfo() ? "false" : "true");
			}
			void Elifndef(clang::SourceLocation location,
						  clang::SourceRange range,
						  clang::SourceLocation) override
			{
				add({location, range.getEnd()}, "elifndef", text(range), {}, "not_evaluated");
			}
			void Else(clang::SourceLocation location, clang::SourceLocation) override
			{
				add({location, location}, "else", {}, {}, "observed");
			}
			void Endif(clang::SourceLocation location, clang::SourceLocation) override
			{
				add({location, location}, "endif", {}, {}, "observed");
			}
			void SourceRangeSkipped(clang::SourceRange range, clang::SourceLocation) override
			{
				add(range, "skipped_range", {}, {}, "skipped");
			}
			void PragmaDirective(clang::SourceLocation location,
								 clang::PragmaIntroducerKind) override
			{
				auto& manager = pp_.getSourceManager();
				const auto first = manager.getExpansionLoc(location);
				const auto logical = project_path(manager.getFilename(first));
				const auto* member = closure_.find_member(logical);
				if (!member)
					return;
				const auto* blob = closure_.find_blob(member->content_digest);
				if (!blob || !blob->content)
					return;
				const auto begin = manager.getFileOffset(first);
				auto end = blob->content->find('\n', begin);
				if (end == std::string::npos)
					end = blob->content->size();
				add({first, first},
					"pragma",
					{},
					blob->content->substr(begin, end - begin),
					"observed");
			}

		  private:
			std::string name(const clang::Token& token) const
			{
				return token.getIdentifierInfo() ? token.getIdentifierInfo()->getName().str()
												 : pp_.getSpelling(token);
			}
			std::string text(clang::SourceRange range) const
			{
				const auto first = pp_.getSourceManager().getExpansionLoc(range.getBegin());
				if (!closure_.find_member(project_path(pp_.getSourceManager().getFilename(first))))
					return {};
				return clang::Lexer::getSourceText(clang::CharSourceRange::getTokenRange(range),
												   pp_.getSourceManager(),
												   pp_.getLangOpts())
					.str();
			}
			void condition(clang::SourceLocation location,
						   clang::SourceRange range,
						   std::string kind,
						   ConditionValueKind value)
			{
				add({location, range.getEnd()},
					std::move(kind),
					{},
					text(range),
					value == CVK_True		 ? "true"
						: value == CVK_False ? "false"
											 : "not_evaluated");
			}
			void add(clang::SourceRange range,
					 std::string kind,
					 std::string name_value,
					 std::string value,
					 std::string state)
			{
				if (output_.truncated)
					return;
				auto& manager = pp_.getSourceManager();
				const auto first = manager.getExpansionLoc(range.getBegin());
				const auto logical = project_path(manager.getFilename(first));
				const auto* member = closure_.find_member(logical);
				if (!member)
					return;
				const auto last = clang::Lexer::getLocForEndOfToken(
					manager.getExpansionLoc(range.getEnd()), 0, manager, pp_.getLangOpts());
				if (last.isInvalid() || !manager.isWrittenInSameFile(first, last))
				{
					output_.classification_incomplete.insert(logical);
					return;
				}
				const auto begin = manager.getFileOffset(first), end = manager.getFileOffset(last);
				if (end < begin || end > member->size_bytes)
				{
					output_.classification_incomplete.insert(logical);
					return;
				}
				const auto bytes =
					logical.size() + kind.size() + name_value.size() + value.size() + state.size();
				if (output_.events.size() >= 1000000U ||
					bytes > 64U * 1024U * 1024U - output_.retained_bytes)
				{
					output_.truncated = true;
					return;
				}
				output_.retained_bytes += bytes;
				output_.events.push_back({logical,
										  begin,
										  end,
										  std::move(kind),
										  std::move(name_value),
										  std::move(value),
										  std::move(state)});
			}
			clang::Preprocessor& pp_;
			const source_closure_snapshot& closure_;
			project_preprocessor_observations& output_;
		};

		class collector final : public clang::RecursiveASTVisitor<collector>
		{
			using base = clang::RecursiveASTVisitor<collector>;
			struct ast_enumeration
			{
				const clang::Stmt* body{};
				std::string function;
				std::uint64_t nodes{}, unmapped{}, local_variables{};
				std::uint64_t unmapped_members{};
				std::set<std::pair<std::string, std::string>> member_accesses;
				std::uint64_t unmapped_calls{};
				std::set<std::string, std::less<>> direct_call_sites, direct_call_targets;
				std::set<const clang::Expr*> call_admissions;
				std::set<std::string, std::less<>> call_sites;
				bool call_frontier{};
				bool active{}, written_initializer{};
			};

		  public:
			collector(provider::clang22::borrowed_translation_unit& unit,
					  const source_closure_snapshot& closure,
					  const provider_worker_v4_ast_observation_batch& observations,
					  const provider_worker_v4_normalized_output& normalized,
					  const std::function<void(std::string_view)>& progress,
					  const project_original_calls& original_calls)
				: unit_{unit}, closure_{closure}, observations_{observations}, progress_{progress},
				  abi_observer_{unit.ast(), unit.preprocessor(), unit.code_generation_options()},
				  original_calls_{original_calls}
			{
				for (const auto& batch : normalized.batches)
					for (const auto& row : batch.rows)
						if (row.descriptor_id == "cc.call_site.v1" ||
							row.descriptor_id == "cc.call_direct_target.v1")
						{
							retain_call_bytes(row.canonical_form().size() + 128U);
							auto& index = row.descriptor_id == "cc.call_site.v1"
								? original_sites_
								: original_targets_;
							index.emplace(row_id(row, "call"), row);
						}
				for (const auto& batch : normalized.batches)
					for (const auto& row : batch.rows)
						if (row.descriptor_id == "cc.entity.v1")
						{
							const auto& encoded = row.cells.at("cc.entity.v1.provider_local_key");
							if (!encoded.value)
								continue;
							const auto& bytes = std::get<std::vector<std::byte>>(*encoded.value);
							std::string key;
							for (const auto b : bytes)
								key.push_back(static_cast<char>(b));
							entities_.emplace(std::move(key), row_id(row, "entity"));
						}
				for (const auto& value : observations.observations)
					if (value.kind == provider_worker_v4_ast_observation_kind::entity &&
						value.primary_span)
						anchors_.emplace(
							std::pair{value.primary_span->file, value.primary_span->begin},
							value.semantic_key);
			}
			bool shouldVisitTemplateInstantiations() const
			{
				return true;
			}
			bool TraverseDecl(clang::Decl* declaration)
			{
				if (declaration == nullptr)
					return true;
				if (!llvm::isa<clang::TranslationUnitDecl>(declaration) &&
					!admitted(declaration->getLocation()))
					return true;
				const auto* function = llvm::dyn_cast<clang::FunctionDecl>(declaration);
				if (llvm::isa<clang::TemplateDecl,
							  clang::ClassTemplateSpecializationDecl,
							  clang::VarTemplateSpecializationDecl>(declaration) ||
					(function &&
					 function->getTemplatedKind() != clang::FunctionDecl::TK_NonTemplate))
					mark_template(declaration->getSourceRange());
				if (function != nullptr && !admitted(function->getLocation()))
					return true;
				const auto previous = current_function_;
				const auto* previous_default = inherited_default_;
				if (const auto* parameter = llvm::dyn_cast<clang::ParmVarDecl>(declaration);
					parameter && parameter->hasInheritedDefaultArg() &&
					!parameter->hasUnparsedDefaultArg())
					inherited_default_ = parameter->hasUninstantiatedDefaultArg()
						? parameter->getUninstantiatedDefaultArg()
						: parameter->getDefaultArg();
				if (function != nullptr)
				{
					current_function_ = entity(function);
					ast_enumeration enumeration;
					enumeration.body = function->getBody();
					enumeration.function = current_function_;
					ast_enumerations_.push_back(std::move(enumeration));
				}
				if (++depth_ > 4096U)
					fail("traversal", "depth-limit");
				const auto result = base::TraverseDecl(declaration);
				--depth_;
				if (function != nullptr)
				{
					const auto enumeration = std::move(ast_enumerations_.back());
					ast_enumerations_.pop_back();
					if (const auto detail = pending_function_details_.find(function);
						detail != pending_function_details_.end())
					{
						call_scope_fields(detail->second, enumeration);
						append(make_row(cc::relations::entity_detail::descriptor(),
										std::move(detail->second)));
						pending_function_details_.erase(detail);
					}
					if (const auto found = pending_bodies_.find(function);
						found != pending_bodies_.end())
					{
						if (declaration_population_admitted(function) &&
							(!function->isImplicit() || written_lambda(function)))
							call_scope_fields(found->second, enumeration);
						found->second.emplace(
							"ast_node_count",
							sdk::detached_cell::unsigned_integer(enumeration.nodes));
						found->second.emplace(
							"local_variable_count",
							sdk::detached_cell::unsigned_integer(enumeration.local_variables));
						found->second.emplace("member_access_count",
											  sdk::detached_cell::unsigned_integer(
												  enumeration.member_accesses.size()));
						found->second.emplace(
							"member_access_state",
							symbol("cc.member-access-state/1",
								   enumeration.unmapped_members == 0U ? "complete" : "partial"));
						found->second.emplace("member_access_profile",
											  symbol("cc.member-access-profile/1",
													 "clang22-explicit-member-access/1"));
						std::set<std::string, std::less<>> member_targets;
						for (const auto& [source, target] : enumeration.member_accesses)
							member_targets.insert(target);
						found->second.emplace("member_access_targets",
											  flags("cc_entity_id", std::move(member_targets)));
						found->second.emplace("direct_call_count",
											  sdk::detached_cell::unsigned_integer(
												  enumeration.direct_call_sites.size()));
						found->second.emplace(
							"direct_call_state",
							symbol("cc.direct-call-state/1",
								   enumeration.unmapped_calls == 0U ? "complete" : "partial"));
						found->second.emplace("direct_call_profile",
											  symbol("cc.direct-call-profile/1",
													 "clang22-written-syntactic-direct-calls/1"));
						found->second.emplace("direct_call_sites",
											  flags("cc_call_id", enumeration.direct_call_sites));
						found->second.emplace(
							"direct_call_targets",
							flags("cc_entity_id", enumeration.direct_call_targets));
						found->second.emplace(
							"ast_state",
							symbol("cc.body-ast-state/1",
								   enumeration.unmapped == 0U ? "complete" : "partial"));
						if (enumeration.unmapped != 0U)
							output_.unresolved.push_back({"body.ast-enumeration-frontier",
														  current_function_,
														  "source-spans-unavailable"});
						append(
							make_row(cc::relations::body::descriptor(), std::move(found->second)));
						pending_bodies_.erase(found);
					}
				}
				current_function_ = previous;
				inherited_default_ = previous_default;
				return result;
			}
			bool TraverseLambdaExpr(clang::LambdaExpr* expression)
			{
				if (!WalkUpFromLambdaExpr(expression))
					return false;
				for (unsigned i{}; i < expression->capture_size(); ++i)
					if (expression->capture_begin()[i].isExplicit() &&
						!TraverseLambdaCapture(expression,
											   expression->capture_begin() + i,
											   expression->capture_init_begin()[i]))
						return false;
				// Captures execute in the enclosing function; the written body has its own owner.
				return TraverseDecl(expression->getCallOperator());
			}
			bool TraverseDecltypeTypeLoc(clang::DecltypeTypeLoc type, bool traverse_qualifier)
			{
				++unevaluated_depth_;
				const bool success = clang::RecursiveASTVisitor<collector>::TraverseDecltypeTypeLoc(
					type, traverse_qualifier);
				--unevaluated_depth_;
				return success;
			}
			bool TraverseTypeOfExprTypeLoc(clang::TypeOfExprTypeLoc type, bool traverse_qualifier)
			{
				const bool unevaluated =
					!type.getUnderlyingExpr()->getType()->isVariablyModifiedType();
				if (unevaluated)
					++unevaluated_depth_;
				const bool success = base::TraverseTypeOfExprTypeLoc(type, traverse_qualifier);
				if (unevaluated)
					--unevaluated_depth_;
				return success;
			}
			bool TraverseDecltypeType(clang::DecltypeType* type, bool qualifier)
			{
				++unevaluated_depth_;
				const bool success = base::TraverseDecltypeType(type, qualifier);
				--unevaluated_depth_;
				return success;
			}
			bool TraverseTypeOfExprType(clang::TypeOfExprType* type, bool qualifier)
			{
				const bool unevaluated =
					!type->getUnderlyingExpr()->getType()->isVariablyModifiedType();
				if (unevaluated)
					++unevaluated_depth_;
				const bool success = base::TraverseTypeOfExprType(type, qualifier);
				if (unevaluated)
					--unevaluated_depth_;
				return success;
			}
			bool dataTraverseStmtPre(clang::Stmt* statement)
			{
				// An inherited default is the same written expression from another actual
				// declaration. Its redeclaration does not acquire a second syntax/call scope.
				if (statement == inherited_default_)
					return false;
				const auto* parent_statement = statements_.empty() ? nullptr : statements_.back();
				const bool body_root =
					!ast_enumerations_.empty() && ast_enumerations_.back().body == statement;
				if (body_root)
					ast_enumerations_.back().active = true;
				statements_.push_back(statement);
				if (++depth_ > 4096U)
					fail("traversal", "depth-limit");
				if (const auto* expression = llvm::dyn_cast<clang::Expr>(statement); expression &&
					(llvm::isa<clang::CallExpr>(expression) ||
					 llvm::isa<clang::CXXConstructExpr>(expression)))
				{
					const auto state = evaluation_context(*expression);
					const auto previous = call_evaluation_.find(expression);
					if (previous == call_evaluation_.end())
					{
						retain_call_bytes(128U + state.size());
						call_evaluation_.emplace(expression, state);
					}
					else if (previous->second != state)
						previous->second = "unknown";
				}
				const auto source = span(statement->getSourceRange(), "expression");
				if (!ast_enumerations_.empty() && ast_enumerations_.back().active &&
					ast_enumerations_.back().function == current_function_)
				{
					if (source)
						++ast_enumerations_.back().nodes;
					else
						++ast_enumerations_.back().unmapped;
				}
				std::string parent;
				if (source)
				{
					fields value = common();
					value.emplace("source", id("source_span_id", *source));
					value.emplace("kind",
								  symbol("cc.syntax-kind/1", statement->getStmtClassName()));
					value.emplace("ordinal",
								  sdk::detached_cell::unsigned_integer(syntax_ordinal_++));
					if (!current_function_.empty())
						value.emplace("function", id("cc_entity_id", current_function_));
					if (!body_root && !parents_.empty() && !parents_.back().empty())
						value.emplace("parent", id("syntax_node_id", parents_.back()));
					std::set<std::string, std::less<>> properties;
					// This marker makes absent category flags known false for this versioned
					// profile.
					properties.emplace("finite_categories_v1");
					// Independent membership of the original emitted call profile. It does
					// not depend on whether normalization returned a call row.
					properties.emplace("finite_call_admission_v1");
					if (const auto* expression = llvm::dyn_cast<clang::Expr>(statement);
						expression && admitted(expression->getExprLoc()) &&
						(llvm::isa<clang::CallExpr>(expression) ||
						 (llvm::isa<clang::CXXConstructExpr>(expression) &&
						  llvm::cast<clang::CXXConstructExpr>(expression)
							  ->getParenOrBraceRange()
							  .isValid())))
						properties.emplace("admitted_call_site");
					properties.emplace(llvm::isa<clang::Expr>(statement) ? "expression"
																		 : "statement");
					if (llvm::isa<clang::ImplicitCastExpr,
								  clang::ExprWithCleanups,
								  clang::MaterializeTemporaryExpr,
								  clang::CXXBindTemporaryExpr>(statement))
						properties.emplace("implicit");
					std::string role = "child";
					if (parent_statement == nullptr)
						role = "body";
					if (const auto* branch =
							llvm::dyn_cast_or_null<clang::IfStmt>(parent_statement))
					{
						if (branch->getCond() == statement)
							role = "condition";
						else if (branch->getThen() == statement)
							role = "then";
						else if (branch->getElse() == statement)
							role = "else";
						else
							role = "initializer";
					}
					else if (const auto* loop =
								 llvm::dyn_cast_or_null<clang::ForStmt>(parent_statement))
					{
						role = loop->getCond() == statement ? "condition"
							: loop->getBody() == statement	? "body"
							: loop->getInc() == statement	? "increment"
															: "initializer";
					}
					else if (const auto* while_loop =
								 llvm::dyn_cast_or_null<clang::WhileStmt>(parent_statement))
						role = while_loop->getCond() == statement ? "condition" : "body";
					else if (const auto* do_loop =
								 llvm::dyn_cast_or_null<clang::DoStmt>(parent_statement))
						role = do_loop->getCond() == statement ? "condition" : "body";
					else if (const auto* range_loop =
								 llvm::dyn_cast_or_null<clang::CXXForRangeStmt>(parent_statement))
						role = range_loop->getBody() == statement ? "body" : "initializer";
					else if (const auto* switch_branch =
								 llvm::dyn_cast_or_null<clang::SwitchStmt>(parent_statement))
						role = switch_branch->getCond() == statement ? "condition" : "body";
					else if (const auto* conditional =
								 llvm::dyn_cast_or_null<clang::ConditionalOperator>(
									 parent_statement))
						role = conditional->getCond() == statement	  ? "condition"
							: conditional->getTrueExpr() == statement ? "then"
																	  : "else";
					else if (const auto* operation =
								 llvm::dyn_cast_or_null<clang::BinaryOperator>(parent_statement))
						role = operation->getLHS() == statement ? "lhs" : "rhs";
					else if (llvm::isa_and_nonnull<clang::ReturnStmt>(parent_statement))
						role = "return_value";
					else if (const auto* call =
								 llvm::dyn_cast_or_null<clang::CallExpr>(parent_statement))
					{
						role = call->getCallee() == statement ? "callee" : "argument";
						for (unsigned index{}; index < call->getNumArgs(); ++index)
							if (call->getArg(index) == statement)
								properties.emplace("argument_" + std::to_string(index));
					}
					properties.emplace("role_" + role);
					const auto* label = llvm::dyn_cast_or_null<clang::LabelStmt>(parent_statement);
					const auto* switch_label =
						llvm::dyn_cast_or_null<clang::SwitchCase>(parent_statement);
					if (llvm::isa<clang::Expr>(statement) &&
						(llvm::isa_and_nonnull<clang::CompoundStmt>(parent_statement) ||
						 ((role == "then" || role == "else") &&
						  llvm::isa_and_nonnull<clang::IfStmt>(parent_statement)) ||
						 (role == "body" &&
						  llvm::isa_and_nonnull<clang::ForStmt,
												clang::WhileStmt,
												clang::DoStmt,
												clang::CXXForRangeStmt>(parent_statement)) ||
						 (label && label->getSubStmt() == statement) ||
						 (switch_label && switch_label->getSubStmt() == statement)))
						properties.emplace("statement");
					if (body_root)
					{
						properties.erase("role_" + role);
						properties.emplace("role_body");
					}
					if (const auto* expression = llvm::dyn_cast<clang::Expr>(statement))
					{
						if (!expression->getType().isNull())
							value.emplace(
								"type",
								sdk::detached_cell::utf8(
									expression->getType().getCanonicalType().getAsString()));
						if (expression->isTypeDependent() || expression->isValueDependent())
							properties.emplace("dependent");
						if (expression->isLValue())
							properties.emplace("lvalue");
					}
					if (const auto* operation = llvm::dyn_cast<clang::BinaryOperator>(statement))
					{
						value.emplace("opcode",
									  sdk::detached_cell::utf8(operation->getOpcodeStr().str()));
						if (operation->isAssignmentOp())
							properties.emplace("assignment");
						if (operation->isComparisonOp())
							properties.emplace("comparison");
					}
					else if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(statement))
						value.emplace(
							"opcode",
							sdk::detached_cell::utf8(
								clang::UnaryOperator::getOpcodeStr(unary->getOpcode()).str()));
					if (llvm::isa<clang::IntegerLiteral,
								  clang::FloatingLiteral,
								  clang::StringLiteral,
								  clang::CharacterLiteral,
								  clang::CXXBoolLiteralExpr>(statement))
					{
						const auto text = clang::Lexer::getSourceText(
							clang::CharSourceRange::getTokenRange(statement->getSourceRange()),
							unit_.source_manager(),
							unit_.ast().getLangOpts());
						value.emplace("value", sdk::detached_cell::utf8(text.str()));
						properties.emplace("literal");
					}
					if (const auto* handler = llvm::dyn_cast<clang::CXXCatchStmt>(statement))
					{
						if (const auto* exception = handler->getExceptionDecl())
						{
							const auto type = exception->getType().getCanonicalType();
							if (type.isNull() || type->isDependentType() ||
								(!type->isReferenceType() && type->isIncompleteType()))
								properties.emplace("catch_type_unknown");
							else if (!type->isReferenceType() && !type.isTrivialType(unit_.ast()))
								properties.emplace("catch_nontrivial_by_value");
						}
						else
							properties.emplace("catch_all");
						const auto* body =
							llvm::dyn_cast_or_null<clang::CompoundStmt>(handler->getHandlerBlock());
						if (body &&
							std::ranges::all_of(body->body(),
												[](const auto* child)
												{
													return llvm::isa<clang::NullStmt>(child);
												}))
							properties.emplace("catch_empty");
					}
					if (const auto* cast = llvm::dyn_cast<clang::CastExpr>(statement))
					{
						if (cast->getCastKind() == clang::CK_PointerToIntegral)
							properties.emplace("cast_pointer_to_integer");
						else if (cast->getCastKind() == clang::CK_IntegralToPointer)
							properties.emplace("cast_integer_to_pointer");
					}
					if (llvm::isa<clang::CoawaitExpr, clang::CoyieldExpr>(statement))
						properties.emplace("coroutine_suspend_site");
					if (const auto* call = llvm::dyn_cast<clang::CallExpr>(statement))
					{
						if (call->getType().isNull() || call->isTypeDependent() ||
							call->isValueDependent())
							properties.emplace("discarded_result_unknown");
						else if (!call->getType()->isVoidType())
						{
							const auto use = result_use(*call);
							if (use == "discarded")
								properties.emplace("discarded_nonvoid_result");
							else if (use == "unknown")
								properties.emplace("discarded_result_unknown");
						}
					}
					value.emplace("flags", flags("cc.syntax-flag/1", std::move(properties)));
					auto row = make_row(cc::relations::syntax_node::descriptor(), std::move(value));
					parent = row_id(row, "node");
					retain_call_bytes(parent.size() + 192U);
					syntax_nodes_[statement].insert(parent);
					append(std::move(row));
				}
				parents_.push_back(std::move(parent));
				return true;
			}
			bool dataTraverseStmtPost(clang::Stmt* statement)
			{
				if (!ast_enumerations_.empty() && ast_enumerations_.back().body == statement)
					ast_enumerations_.back().active = false;
				parents_.pop_back();
				statements_.pop_back();
				--depth_;
				return true;
			}
			bool VisitNamedDecl(clang::NamedDecl* declaration)
			{
				if (declaration == nullptr ||
					(declaration->isImplicit() && !written_lambda(declaration)) ||
					!admitted(declaration->getLocation()))
					return true;
				// A template wrapper and its templated declaration share a Clang USR.
				// The actual declaration supplies kind, type and member properties.
				if (llvm::isa<clang::ClassTemplateDecl,
							  clang::FunctionTemplateDecl,
							  clang::VarTemplateDecl,
							  clang::TypeAliasTemplateDecl>(declaration))
					return true;
				if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration); variable &&
					variable->isLocalVarDecl() && !ast_enumerations_.empty() &&
					ast_enumerations_.back().active &&
					ast_enumerations_.back().function == current_function_)
					++ast_enumerations_.back().local_variables;
				const bool population_admitted = declaration_population_admitted(declaration);
				if (population_admitted && observed_declarations_.insert(declaration).second)
				{
					retain_population_bytes(64U);
					if (observed_declarations_.size() > 1'000'000U)
						fail("declaration-inventory", "admission-count-limit");
				}
				const auto entity_id = entity(declaration);
				const auto source = span(declaration->getSourceRange(), "declaration");
				if (const auto* record = llvm::dyn_cast<clang::RecordDecl>(declaration); record &&
					record->isThisDeclarationADefinition() &&
					!observed_record_definitions_.contains(record->getCanonicalDecl()))
				{
					retain_inventory_bytes(64U);
					if (observed_record_definitions_.size() >= 1'000'000U)
						fail("record-inventory", "definition-count-limit");
					observed_record_definitions_.insert(record->getCanonicalDecl());
				}
				if (entity_id.empty() || !source)
					return true;
				fields value = common();
				value.emplace("entity", id("cc_entity_id", entity_id));
				value.emplace("source", id("source_span_id", *source));
				value.emplace("access", symbol("cc.access/1", access(declaration->getAccess())));
				value.emplace("linkage",
							  symbol("cc.linkage/1",
									 declaration->isExternallyVisible() ? "external" : "internal"));
				value.emplace(
					"language",
					symbol("cc.language/1", unit_.ast().getLangOpts().CPlusPlus ? "c++" : "c"));
				value.emplace("module_domain",
							  sdk::detached_cell::utf8(
								  declaration->getOwningModule()
									  ? declaration->getOwningModule()->getFullModuleName()
									  : "<none>"));
				std::set<std::string, std::less<>> properties;
				declaration_categories(*declaration, properties);
				std::string signature = usr(declaration);
				bool definition = true;
				std::uint64_t parameters{};
				clang::QualType type;
				if (const auto* typed = llvm::dyn_cast<clang::ValueDecl>(declaration))
					type = typed->getType();
				if (const auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(declaration))
					type = alias->getUnderlyingType();
				if (const auto* tag = llvm::dyn_cast<clang::TagDecl>(declaration))
				{
					type = unit_.ast().getCanonicalTagType(tag);
					definition = tag->isThisDeclarationADefinition();
				}
				if (const auto* record = llvm::dyn_cast<clang::RecordDecl>(declaration))
				{
					if (definition && record->isCompleteDefinition() && !record->isInvalidDecl())
					{
						properties.emplace("record_definition");
						if (const auto* cxx_record = llvm::dyn_cast<clang::CXXRecordDecl>(record))
						{
							if (cxx_record->isDependentContext())
								properties.emplace("abstractness_unknown");
							else if (cxx_record->isAbstract())
								properties.emplace("abstract");
						}
					}
					else
						properties.emplace("abstractness_unknown");
				}
				if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(declaration))
				{
					value.emplace("calling_convention",
								  sdk::detached_cell::utf8("clang22.cc/" +
														   std::to_string(static_cast<unsigned>(
															   function->getType()
																   ->castAs<clang::FunctionType>()
																   ->getCallConv()))));
					definition = function->isThisDeclarationADefinition();
					if (population_admitted)
					{
						properties.emplace("finite_function_body_v1");
						const bool written_body = function->doesThisDeclarationHaveABody() &&
							function->getBody() && !function->isDeleted() &&
							!function->isDefaulted();
						if (written_body)
							properties.emplace("body_written");
						else if (!function->hasSkippedBody() &&
								 (!function->doesThisDeclarationHaveABody() ||
								  function->isDeleted() || function->isDefaulted()))
							properties.emplace("body_absent");
					}
					parameters = function->getNumParams();
					if (function->isConstexpr())
						properties.emplace("constexpr");
					if (function->isImmediateFunction())
						properties.emplace("consteval");
					if (function->isInlineSpecified())
						properties.emplace("inline");
					if (function->isDeleted())
						properties.emplace("deleted");
					if (function->isDefaulted())
						properties.emplace("defaulted");
					if (function->isVariadic())
						properties.emplace("variadic");
					if (function->isExternC())
						properties.emplace("extern_c");
					if (function->getStorageClass() == clang::SC_Static)
						properties.emplace("static");
					if (const auto* proto = function->getType()->getAs<clang::FunctionProtoType>())
					{
						// canThrow()/isNothrow() require an evaluated exception specification.
						// Unused defaulted members and template declarations deliberately retain
						// lazy specifications; observing them must not instantiate user code.
						const auto specification = proto->getExceptionSpecType();
						if (!clang::isUnresolvedExceptionSpec(specification) &&
							specification != clang::EST_Unparsed &&
							specification != clang::EST_DependentNoexcept)
							properties.emplace("noexcept_classified");
						if (specification == clang::EST_DynamicNone ||
							specification == clang::EST_NoThrow ||
							specification == clang::EST_BasicNoexcept ||
							specification == clang::EST_NoexceptTrue)
							properties.emplace("noexcept");
						else if (clang::isUnresolvedExceptionSpec(specification) ||
								 specification == clang::EST_Unparsed ||
								 specification == clang::EST_DependentNoexcept)
						{
							properties.emplace("noexcept_unknown");
							output_.unresolved.push_back(
								{"declaration.exception-specification-frontier",
								 entity_id,
								 "instantiate-or-evaluate-exception-specification"});
						}
					}
					if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function))
					{
						if (method->isVirtual())
							properties.emplace("virtual");
						if (method->isPureVirtual())
							properties.emplace("pure_virtual");
						if (method->isStatic())
							properties.emplace("static");
						if (method->isConst())
							properties.emplace("const");
						for (const auto* overridden : method->overridden_methods())
							edge(entity_id, entity(overridden), *source, "overrides");
					}
				}
				else
					value.emplace("calling_convention", sdk::detached_cell::utf8("<none>"));
				if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration))
				{
					definition =
						variable->isThisDeclarationADefinition() != clang::VarDecl::DeclarationOnly;
					if (variable->getTLSKind() != clang::VarDecl::TLS_None)
						properties.emplace("thread_local");
					if (variable->hasGlobalStorage())
						properties.emplace("global_storage");
					if (variable->hasInit())
						properties.emplace("initialized");
					if (variable->isLocalVarDecl())
						properties.emplace("local_variable");
				}
				if (const auto* parameter = llvm::dyn_cast<clang::ParmVarDecl>(declaration))
				{
					if (parameter->hasDefaultArg())
						properties.emplace("default_argument");
					if (parameter->getType()->isLValueReferenceType() &&
						!parameter->getType()->getPointeeType().isConstQualified())
						properties.emplace("mutable_lvalue_reference");
				}
				if (const auto* field = llvm::dyn_cast<clang::FieldDecl>(declaration);
					field && field->isMutable())
					properties.emplace("mutable");
				if (!type.isNull())
				{
					value.emplace("canonical_type", id("cc_type_id", canonical_type(type)));
					signature = type.getCanonicalType().getAsString();
					if (type.isConstQualified())
						properties.emplace("const");
					if (type.isVolatileQualified())
						properties.emplace("volatile");
					if (type->isPointerType())
						properties.emplace("pointer");
					if (type->isReferenceType())
						properties.emplace("reference");
					auto referenced = type;
					while (referenced->isPointerType() || referenced->isReferenceType() ||
						   referenced->isArrayType())
					{
						if (referenced->isArrayType())
							referenced = unit_.ast().getBaseElementType(referenced);
						else
							referenced = referenced->getPointeeType();
					}
					if (const auto* record = referenced->getAs<clang::RecordType>())
						edge(entity_id, entity(record->getDecl()), *source, "uses_type");
				}
				for (const auto* attribute : declaration->attrs())
					if (const auto* spelling = attribute->getSpelling(); spelling != nullptr)
						properties.emplace("attribute_" + std::string{spelling});
				value.emplace("signature", sdk::detached_cell::utf8(std::move(signature)));
				value.emplace("is_definition", sdk::detached_cell::boolean(definition));
				value.emplace("flags", flags("cc.entity-flag/1", std::move(properties)));
				value.emplace("parameter_count", sdk::detached_cell::unsigned_integer(parameters));
				if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(declaration);
					function && population_admitted)
				{
					retain_call_bytes(512U);
					pending_function_details_.insert_or_assign(function, std::move(value));
				}
				else
					append(make_row(cc::relations::entity_detail::descriptor(), std::move(value)));
				auto declaration_row = fields{
					{"entity", id("cc_entity_id", entity_id)},
					{"source", id("source_span_id", *source)},
					{"kind", symbol("cc.declaration-kind/1", declaration->getDeclKindName())},
					{"storage", symbol("cc.storage-class/1", "none")},
					{"linkage",
					 symbol("cc.linkage/1",
							declaration->isExternallyVisible() ? "external" : "internal")},
					{"attributes", flags("cc.attribute/1", {})},
					{"is_implicit", sdk::detached_cell::boolean(declaration->isImplicit())},
					{"is_deleted", sdk::detached_cell::boolean(false)},
					{"is_defaulted", sdk::detached_cell::boolean(false)},
					{"is_friend",
					 sdk::detached_cell::boolean(declaration->getFriendObjectKind() !=
												 clang::Decl::FOK_None)},
					{"is_exported",
					 sdk::detached_cell::boolean(declaration->isInExportDeclContext())}};
				std::set<std::string, std::less<>> attributes;
				for (const auto* attribute : declaration->attrs())
					if (const auto* spelling = attribute->getSpelling(); spelling)
						attributes.emplace(spelling);
				declaration_row.insert_or_assign("attributes",
												 flags("cc.attribute/1", std::move(attributes)));
				if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(declaration))
				{
					declaration_row.insert_or_assign(
						"is_deleted", sdk::detached_cell::boolean(function->isDeleted()));
					declaration_row.insert_or_assign(
						"is_defaulted", sdk::detached_cell::boolean(function->isDefaulted()));
					declaration_row.insert_or_assign(
						"storage",
						symbol("cc.storage-class/1", storage(function->getStorageClass())));
				}
				if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration))
					declaration_row.insert_or_assign(
						"storage",
						symbol("cc.storage-class/1", storage(variable->getStorageClass())));
				auto original_declaration =
					make_row(cc::relations::declaration::descriptor(), std::move(declaration_row));
				if (population_admitted)
				{
					const auto declaration_id = row_id(original_declaration, "declaration");
					retain_population_bytes(declaration_id.size() * 2U + 128U);
					declaration_ids_.insert(declaration_id);
					if (unit_.source_manager().isInSystemHeader(
							unit_.source_manager().getExpansionLoc(declaration->getLocation())))
						system_declaration_ids_.insert(declaration_id);
				}
				append(std::move(original_declaration));
				if (!type.isNull() && !llvm::isa<clang::FunctionDecl>(declaration))
					layout(*declaration, type);
				const auto* owner = llvm::dyn_cast<clang::NamedDecl>(
					clang::Decl::castFromDeclContext(declaration->getDeclContext()));
				const auto owner_id = entity(owner);
				if (!owner_id.empty() && owner_id != entity_id)
					edge(owner_id, entity_id, *source, "owns");
				if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(declaration);
					record && record->isThisDeclarationADefinition())
					for (const auto& base_specifier : record->bases())
						if (const auto* target =
								base_specifier.getType()->getAs<clang::RecordType>())
							edge(entity_id, entity(target->getDecl()), *source, "inherits");
				if (const auto* record = llvm::dyn_cast<clang::RecordDecl>(declaration))
				{
					record_surface(*record, entity_id, *source);
					abi_surface(entity_id, *source, "record", take(abi_observer_.record(*record)));
				}
				else if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(declaration))
					abi_surface(
						entity_id, *source, "function", take(abi_observer_.function(*function)));
				return true;
			}
			bool VisitFunctionDecl(clang::FunctionDecl* function)
			{
				if (function == nullptr || (function->isImplicit() && !written_lambda(function)) ||
					!function->isThisDeclarationADefinition() || !function->hasBody() ||
					!admitted(function->getLocation()))
					return true;
				build_cfg(*function);
				return true;
			}
			bool VisitCXXConstructExpr(clang::CXXConstructExpr* expression)
			{
				if (expression && expression->getParenOrBraceRange().isValid())
					admit_call(expression);
				return true;
			}
			bool VisitCallExpr(clang::CallExpr* expression)
			{
				admit_call(expression);
				if (ast_enumerations_.empty() || current_function_.empty())
					return true;
				auto& enumeration = ast_enumerations_.back();
				if (enumeration.function != current_function_ ||
					(!enumeration.active && !enumeration.written_initializer))
					return true;
				const auto* callee = expression->getDirectCallee();
				if (!callee && !expression->isTypeDependent() && !expression->isValueDependent())
					return true;
				const auto target = entity(callee);
				const auto original = original_calls_.find(expression);
				bool bound{};
				if (!target.empty() && original != original_calls_.end())
					if (const auto actual_target = original_targets_.find(original->second);
						actual_target != original_targets_.end() &&
						row_id(actual_target->second, "target") == target)
					{
						bound = true;
						if (!enumeration.direct_call_sites.contains(original->second))
						{
							retain_direct_call_bytes(192U + original->second.size() +
													 target.size());
							enumeration.direct_call_sites.insert(original->second);
							enumeration.direct_call_targets.insert(target);
						}
					}
				if (!bound)
				{
					++enumeration.unmapped_calls;
					output_.unresolved.push_back({"body.direct-call-frontier",
												  current_function_,
												  callee ? "supply-direct-call-source-and-identity"
														 : "observe-call-specialization"});
				}
				return true;
			}
			bool VisitMemberExpr(clang::MemberExpr* expression)
			{
				if (!current_function_.empty())
				{
					if (llvm::isa<clang::FieldDecl>(expression->getMemberDecl()))
						observe_member_access(*expression->getMemberDecl(),
											  expression->getSourceRange());
					if (const auto source = span(expression->getSourceRange(), "expression"))
					{
						edge(current_function_,
							 entity(expression->getMemberDecl()),
							 *source,
							 "accesses_member");
						if (llvm::isa<clang::FieldDecl>(expression->getMemberDecl()))
							state_access(*expression, entity(expression->getMemberDecl()), *source);
					}
				}
				return true;
			}
			bool TraverseConstructorInitializer(clang::CXXCtorInitializer* initializer)
			{
				if (initializer && initializer->isMemberInitializer() && initializer->isWritten())
					if (const auto source = observe_member_access(*initializer->getMember(),
																  initializer->getSourceRange()))
						edge(current_function_,
							 entity(initializer->getMember()),
							 *source,
							 "accesses_member");
				const bool tracked =
					!ast_enumerations_.empty() && initializer && initializer->isWritten();
				const bool previous = tracked && ast_enumerations_.back().written_initializer;
				if (tracked)
					ast_enumerations_.back().written_initializer = true;
				const auto traversed = base::TraverseConstructorInitializer(initializer);
				if (tracked)
					ast_enumerations_.back().written_initializer = previous;
				return traversed;
			}
			std::optional<std::string> observe_member_access(const clang::NamedDecl& field,
															 clang::SourceRange range)
			{
				const auto source = span(range, "expression");
				if (!ast_enumerations_.empty() &&
					ast_enumerations_.back().function == current_function_)
				{
					const auto target = entity(&field);
					if (source && !target.empty())
						ast_enumerations_.back().member_accesses.emplace(*source, target);
					else
					{
						++ast_enumerations_.back().unmapped_members;
						output_.unresolved.push_back({"body.member-access-frontier",
													  current_function_,
													  "supply-field-identity-and-source-span"});
					}
				}
				return source;
			}
			bool VisitDeclRefExpr(clang::DeclRefExpr* expression)
			{
				if (current_function_.empty() || !llvm::isa<clang::VarDecl>(expression->getDecl()))
					return true;
				if (const auto source = span(expression->getSourceRange(), "expression"))
					state_access(*expression, entity(expression->getDecl()), *source);
				return true;
			}
			void preprocess(const project_preprocessor_observations& preprocessing)
			{
				auto* record = unit_.preprocessor().getPreprocessingRecord();
				if (record == nullptr)
					fail("preprocessor", "record-unavailable");
				for (auto iterator = record->begin(); iterator != record->end(); ++iterator)
				{
					const auto* observation = *iterator;
					include_census* inclusion_population{};
					if (const auto* inclusion =
							llvm::dyn_cast<clang::InclusionDirective>(observation))
						if (const auto* member = admitted(inclusion->getSourceRange().getBegin()))
						{
							inclusion_population = &include_populations_[member->logical_path];
							const auto first = unit_.source_manager().getExpansionLoc(
								inclusion->getSourceRange().getBegin());
							const auto last = unit_.source_manager().getExpansionLoc(
								inclusion->getSourceRange().getEnd());
							const auto included = inclusion->getFile();
							const auto key =
								std::to_string(unit_.source_manager().getFileOffset(first)) + ':' +
								std::to_string(last.isValid()
												   ? unit_.source_manager().getFileOffset(last)
												   : 0U) +
								':' + inclusion->getFileName().str() + ':' +
								(included ? included->getName().str() : "<unresolved>");
							retain_population_bytes(key.size() + 96U);
							inclusion_population->admissions.insert(key);
							inclusion_population->resolution_complete &= included.has_value();
						}
					const auto source = span(observation->getSourceRange(), "preprocessor");
					if (!source)
						continue;
					fields value = common();
					value.emplace("source", id("source_span_id", *source));
					if (const auto* inclusion =
							llvm::dyn_cast<clang::InclusionDirective>(observation))
					{
						const auto* member = admitted(inclusion->getSourceRange().getBegin());
						value.emplace("from_file", id("file_id", member->file_id));
						value.emplace("spelling",
									  sdk::detached_cell::utf8(inclusion->getFileName().str()));
						value.emplace("kind",
									  symbol("source.include-kind/1",
											 inclusion->wasInQuotes() ? "quoted" : "angled"));
						const auto included = inclusion->getFile();
						const auto* target = included
							? closure_.find_member(project_path(included->getName()))
							: nullptr;
						if (target)
							value.emplace("to_file", id("file_id", target->file_id));
						value.emplace("resolution",
									  symbol("source.include-resolution/1",
											 target			? "resolved"
												 : included ? "external"
															: "unresolved"));
						const auto file_id = included
							? unit_.source_manager().translateFile(*included)
							: clang::FileID{};
						value.emplace(
							"is_system",
							sdk::detached_cell::boolean(
								file_id.isValid() &&
								unit_.source_manager().isInSystemHeader(
									unit_.source_manager().getLocForStartOfFile(file_id))));
						auto include =
							make_row(source::relations::include::descriptor(), std::move(value));
						if (inclusion_population)
						{
							const auto include_id = row_id(include, "include");
							retain_population_bytes(include_id.size() + 96U);
							inclusion_population->ids.insert(include_id);
						}
						append(std::move(include));
					}
				}
				for (const auto& member : closure_.members)
				{
					const auto found = include_populations_.find(member.logical_path);
					const auto count =
						found == include_populations_.end() ? 0U : found->second.admissions.size();
					const auto ids = found == include_populations_.end()
						? std::set<std::string, std::less<>>{}
						: found->second.ids;
					const bool opened = preprocessing.opened_files.contains(member.logical_path);
					const bool complete = opened && count == ids.size();
					auto value = common();
					value.emplace("file", id("file_id", member.file_id));
					value.emplace("source_snapshot",
								  id("source_snapshot_id",
									 take(sdk::detail::derive_source_snapshot_id(
										 member.file_id, member.content_digest, "utf8"))));
					value.emplace(
						"profile",
						sdk::detached_cell::utf8("clang22-preprocessed-inclusion-directives/1"));
					value.emplace("enumeration_state",
								  symbol("source.include-inventory-state/1",
										 !opened		? "unavailable"
											 : complete ? "complete"
														: "partial"));
					value.emplace("include_count", sdk::detached_cell::unsigned_integer(count));
					value.emplace("includes", flags("include_id", ids));
					value.emplace("resolution_state",
								  symbol("source.include-resolution-state/1",
										 !opened ? "unavailable"
											 : found == include_populations_.end() ||
												 found->second.resolution_complete
											 ? "complete"
											 : "partial"));
					if (!complete)
						value.emplace("reason",
									  sdk::detached_cell::utf8(
										  !opened ? "source-not-preprocessed-in-unit"
												  : "include-source-identity-unavailable"));
					append(make_row(source::relations::include_inventory::descriptor(),
									std::move(value)));
				}
				std::uint64_t ordinal{};
				for (const auto& event : preprocessing.events)
				{
					const auto* member = closure_.find_member(event.logical_path);
					if (!member || event.end < event.begin || event.end > member->size_bytes)
						fail("preprocessor", "event-source-binding");
					const auto snapshot = take(sdk::detail::derive_source_snapshot_id(
						member->file_id, member->content_digest, "utf8"));
					auto source =
						make_row(source::relations::span::descriptor(),
								 {{"snapshot", id("source_snapshot_id", snapshot)},
								  {"file", id("file_id", member->file_id)},
								  {"begin", sdk::detached_cell::unsigned_integer(event.begin)},
								  {"end", sdk::detached_cell::unsigned_integer(event.end)},
								  {"role", symbol("source.range-role/1", "preprocessor")},
								  {"read_only", sdk::detached_cell::boolean(true)}});
					const auto source_id = row_id(source, "span");
					append(std::move(source));
					auto value = common();
					value.emplace("source", id("source_span_id", source_id));
					value.emplace("kind", symbol("source.pp-event-kind/1", event.kind));
					value.emplace("name", sdk::detached_cell::utf8(event.name));
					if (!event.value.empty())
						value.emplace("value", sdk::detached_cell::utf8(event.value));
					value.emplace("state", symbol("source.pp-state/1", event.state));
					value.emplace("ordinal", sdk::detached_cell::unsigned_integer(ordinal++));
					append(make_row(source::relations::preprocessor_event::descriptor(),
									std::move(value)));
				}
				if (preprocessing.truncated)
					output_.unresolved.push_back({"preprocessor.event-budget",
												  observations_.compile_unit,
												  "retained-prefix"});
			}
			void tokens(const project_preprocessor_observations& preprocessing)
			{
				for (const auto& logical : preprocessing.opened_files)
					if (const auto* member = closure_.find_member(logical))
					{
						retain_population_bytes(member->file_id.size() + 256U);
						parsed_files_.insert(member->file_id);
						parsed_snapshots_.insert(take(sdk::detail::derive_source_snapshot_id(
							member->file_id, member->content_digest, "utf8")));
					}
				using intervals = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
				std::map<std::string, intervals, std::less<>> skipped, macros;
				for (const auto& event : preprocessing.events)
					if (event.kind == "skipped_range")
						skipped[event.logical_path].emplace_back(event.begin, event.end);
					else if (event.kind == "macro_expansion")
						macros[event.logical_path].emplace_back(event.begin, event.end);
				const auto normalize = [](auto& ranges)
				{
					for (auto& [path, spans] : ranges)
					{
						(void)path;
						std::ranges::sort(spans);
						intervals merged;
						for (const auto& interval : spans)
							if (!merged.empty() && interval.first <= merged.back().second)
								merged.back().second =
									std::max(merged.back().second, interval.second);
							else
								merged.push_back(interval);
						spans = std::move(merged);
					}
				};
				normalize(skipped);
				normalize(macros);
				normalize(template_ranges_);
				const auto overlaps = [](const auto& ranges,
										 const std::string& path,
										 std::uint64_t begin,
										 std::uint64_t end)
				{
					const auto found = ranges.find(path);
					if (found == ranges.end())
						return false;
					const auto next =
						std::lower_bound(found->second.begin(),
										 found->second.end(),
										 end,
										 [](const auto& interval, std::uint64_t offset)
										 {
											 return interval.first < offset;
										 });
					return next != found->second.begin() && std::prev(next)->second > begin;
				};
				constexpr std::string_view profile = "clang22.tokens.lexical-pp-template.v1";
				std::map<std::string,
						 std::vector<const project_preprocessor_observations::token*>,
						 std::less<>>
					expanded;
				for (const auto& token : preprocessing.expanded_tokens)
					expanded[token.logical_path].push_back(&token);
				auto& manager = unit_.source_manager();
				clang::IdentifierTable identifiers{unit_.ast().getLangOpts()};
				for (const auto& member : closure_.members)
				{
					const auto* blob = closure_.find_blob(member.content_digest);
					if (!blob || !blob->content)
						fail("tokens", "frozen-content-required");
					const auto snapshot = take(sdk::detail::derive_source_snapshot_id(
						member.file_id, member.content_digest, "utf8"));
					const bool opened = preprocessing.opened_files.contains(member.logical_path);
					const bool activity_known = opened && !preprocessing.truncated &&
						!preprocessing.classification_incomplete.contains(member.logical_path);
					const bool templates_known =
						opened && !template_incomplete_.contains(member.logical_path);
					bool template_partial{};
					const auto emit = [&](std::string_view phase,
										  std::uint64_t begin,
										  std::uint64_t end,
										  std::uint64_t ordinal,
										  std::string kind,
										  std::string spelling,
										  std::optional<bool> macro,
										  std::optional<bool> active,
										  bool directive,
										  bool directive_start)
					{
						if (end < begin || end > member.size_bytes)
							fail("tokens", "range-outside-frozen-input");
						auto source = make_row(
							source::relations::span::descriptor(),
							{{"snapshot", id("source_snapshot_id", snapshot)},
							 {"file", id("file_id", member.file_id)},
							 {"begin", sdk::detached_cell::unsigned_integer(begin)},
							 {"end", sdk::detached_cell::unsigned_integer(end)},
							 {"role", symbol("source.range-role/1", std::string{phase} + "_token")},
							 {"read_only",
							  sdk::detached_cell::boolean(phase == "expanded" || directive)}});
						const auto span_id = row_id(source, "span");
						append(std::move(source));
						auto values = common();
						values.emplace("source", id("source_span_id", span_id));
						values.emplace("phase", symbol("source.token-phase/1", std::string{phase}));
						values.emplace("kind", symbol("source.token-kind/1", std::move(kind)));
						values.emplace("spelling", sdk::detached_cell::utf8(std::move(spelling)));
						values.emplace("ordinal", sdk::detached_cell::unsigned_integer(ordinal));
						values.emplace("profile", sdk::detached_cell::utf8(std::string{profile}));
						values.emplace("preprocessor", sdk::detached_cell::boolean(directive));
						values.emplace("directive_start",
									   sdk::detached_cell::boolean(directive_start));
						if (macro)
							values.emplace("macro", sdk::detached_cell::boolean(*macro));
						if (active)
							values.emplace("active", sdk::detached_cell::boolean(*active));
						if (templates_known && active && (*active || directive))
							values.emplace("template_context",
										   sdk::detached_cell::boolean(overlaps(
											   template_ranges_, member.logical_path, begin, end)));
						else
							template_partial = true;
						append(make_row(source::relations::token::descriptor(), std::move(values)));
					};
					const auto inventory = [&](std::string_view phase,
											   std::optional<std::uint64_t> count,
											   std::optional<std::uint64_t> directives,
											   std::string state,
											   std::string reason)
					{
						auto values = common();
						values.emplace("file", id("file_id", member.file_id));
						values.emplace("source_snapshot", id("source_snapshot_id", snapshot));
						values.emplace("phase", symbol("source.token-phase/1", std::string{phase}));
						values.emplace("profile", sdk::detached_cell::utf8(std::string{profile}));
						values.emplace("enumeration_state",
									   symbol("source.token-enumeration/1", state));
						values.emplace("active_state",
									   symbol("source.token-classification/1",
											  phase == "expanded"  ? state
												  : activity_known ? "complete"
																   : "unavailable"));
						values.emplace("template_state",
									   symbol("source.token-classification/1",
											  !templates_known		 ? "unavailable"
												  : template_partial ? "partial"
																	 : "complete"));
						if (count)
							values.emplace("token_count",
										   sdk::detached_cell::unsigned_integer(*count));
						if (directives)
							values.emplace("directive_count",
										   sdk::detached_cell::unsigned_integer(*directives));
						if (!reason.empty())
							values.emplace("reason", sdk::detached_cell::utf8(std::move(reason)));
						append(make_row(source::relations::token_inventory::descriptor(),
										std::move(values)));
					};
					const auto file_id = manager.createFileID(
						llvm::MemoryBuffer::getMemBufferCopy(*blob->content, member.logical_path),
						clang::SrcMgr::C_User);
					clang::Lexer lexer{file_id,
									   manager.getBufferOrFake(file_id),
									   manager,
									   unit_.ast().getLangOpts()};
					lexer.SetCommentRetentionState(true);
					std::uint64_t count{}, directives{}, comment_count{}, comment_bytes{};
					std::set<std::string, std::less<>> comment_ids;
					bool directive{};
					while (true)
					{
						clang::Token token;
						lexer.LexFromRawLexer(token);
						if (token.is(clang::tok::eof))
							break;
						if (token.is(clang::tok::comment))
						{
							const auto begin = manager.getFileOffset(token.getLocation());
							const auto end = static_cast<std::uint64_t>(begin) + token.getLength();
							if (end > member.size_bytes)
								fail("comments", "range-outside-frozen-input");
							auto source =
								make_row(source::relations::span::descriptor(),
										 {{"snapshot", id("source_snapshot_id", snapshot)},
										  {"file", id("file_id", member.file_id)},
										  {"begin", sdk::detached_cell::unsigned_integer(begin)},
										  {"end", sdk::detached_cell::unsigned_integer(end)},
										  {"role", symbol("source.range-role/1", "comment")},
										  {"read_only", sdk::detached_cell::boolean(true)}});
							const auto source_id = row_id(source, "span");
							append(std::move(source));
							auto value = common();
							value.emplace("source", id("source_span_id", source_id));
							const auto spelling = clang::Lexer::getSpelling(
								token, manager, unit_.ast().getLangOpts());
							value.emplace("kind",
										  symbol("source.comment-kind/1",
												 spelling.starts_with("//") ? "line" : "block"));
							value.emplace("byte_count",
										  sdk::detached_cell::unsigned_integer(end - begin));
							retain_population_bytes(end - begin);
							const auto raw =
								std::as_bytes(std::span{blob->content->data() + begin,
														static_cast<std::size_t>(end - begin)});
							value.emplace("spelling_bytes",
										  sdk::detached_cell::bytes(
											  std::vector<std::byte>{raw.begin(), raw.end()}));
							value.emplace("ordinal",
										  sdk::detached_cell::unsigned_integer(comment_count++));
							value.emplace(
								"profile",
								sdk::detached_cell::utf8("clang22-frozen-raw-comments/1"));
							auto comment = make_row(source::relations::comment::descriptor(),
													std::move(value));
							const auto comment_id = row_id(comment, "comment");
							retain_population_bytes(comment_id.size() + 96U);
							comment_ids.insert(comment_id);
							if (end - begin >
								std::numeric_limits<std::uint64_t>::max() - comment_bytes)
								fail("comments", "byte-count-overflow");
							comment_bytes += end - begin;
							append(std::move(comment));
							continue;
						}
						const auto begin = manager.getFileOffset(token.getLocation());
						const auto end = static_cast<std::uint64_t>(begin) + token.getLength();
						if (token.isAtStartOfLine())
						{
							directive = token.is(clang::tok::hash);
							if (directive)
								++directives;
						}
						const bool directive_start =
							token.isAtStartOfLine() && token.is(clang::tok::hash);
						const auto spelling =
							clang::Lexer::getSpelling(token, manager, unit_.ast().getLangOpts());
						const auto kind = token.is(clang::tok::raw_identifier)
							? identifiers.get(spelling).getTokenID()
							: token.getKind();
						std::optional<bool> active, macro;
						if (activity_known)
						{
							active =
								!directive && !overlaps(skipped, member.logical_path, begin, end);
							macro = overlaps(macros, member.logical_path, begin, end);
						}
						emit("raw",
							 begin,
							 end,
							 count++,
							 clang::tok::getTokenName(kind),
							 spelling,
							 macro,
							 active,
							 directive,
							 directive_start);
					}
					inventory("raw", count, directives, "complete", {});
					std::vector<std::byte> line_starts;
					const auto line = [&](std::uint64_t offset)
					{
						retain_population_bytes(8U);
						for (unsigned shift{}; shift < 64U; shift += 8U)
							line_starts.push_back(static_cast<std::byte>((offset >> shift) & 255U));
					};
					if (!blob->content->empty())
						line(0U);
					for (std::size_t offset{}; offset < blob->content->size(); ++offset)
						if ((*blob->content)[offset] == '\r' || (*blob->content)[offset] == '\n')
						{
							if ((*blob->content)[offset] == '\r' &&
								offset + 1U < blob->content->size() &&
								(*blob->content)[offset + 1U] == '\n')
								++offset;
							if (offset + 1U < blob->content->size())
								line(offset + 1U);
						}
					auto comments = common();
					comments.emplace("file", id("file_id", member.file_id));
					comments.emplace("source_snapshot", id("source_snapshot_id", snapshot));
					comments.emplace("profile",
									 sdk::detached_cell::utf8("clang22-frozen-raw-comments/1"));
					comments.emplace("enumeration_state",
									 symbol("source.comment-inventory-state/1", "complete"));
					comments.emplace("comment_count",
									 sdk::detached_cell::unsigned_integer(comment_count));
					comments.emplace("comments",
									 flags("source_comment_id", std::move(comment_ids)));
					comments.emplace("comment_bytes",
									 sdk::detached_cell::unsigned_integer(comment_bytes));
					comments.emplace("physical_line_starts",
									 sdk::detached_cell::bytes(std::move(line_starts)));
					comments.emplace(
						"physical_line_profile",
						sdk::detached_cell::utf8("source.physical-lines.crlf-lf-cr/1"));
					append(make_row(source::relations::comment_inventory::descriptor(),
									std::move(comments)));
					template_partial = false;
					count = 0U;
					for (const auto* token : expanded[member.logical_path])
						emit("expanded",
							 token->begin,
							 token->end,
							 count++,
							 token->kind,
							 token->spelling,
							 token->macro,
							 true,
							 false,
							 false);
					const auto state = !opened ? "unavailable"
						: preprocessing.tokens_truncated || preprocessing.tokens_unmapped
						? "partial"
						: "complete";
					inventory("expanded",
							  opened ? std::optional{count} : std::nullopt,
							  std::nullopt,
							  state,
							  !opened ? "source-not-preprocessed-in-unit"
								  : preprocessing.tokens_truncated ? "expanded-token-budget"
								  : preprocessing.tokens_unmapped  ? "expanded-token-source-mapping"
																   : "");
				}
			}
			project_semantic_facts finish()
			{
				finish_calls();
				auto inventory = common();
				inventory.emplace("profile",
								  symbol("cc.record-inventory-profile/1",
										 "clang22-explicit-admitted-record-definitions/1"));
				inventory.emplace(
					"definition_count",
					sdk::detached_cell::unsigned_integer(observed_record_definitions_.size()));
				const bool complete =
					observed_record_definitions_.size() == record_definition_surfaces_.size();
				inventory.emplace(
					"enumeration_state",
					symbol("cc.record-inventory-state/1", complete ? "complete" : "partial"));
				inventory.emplace(
					"definitions",
					flags("record_surface_id", std::move(record_definition_surfaces_)));
				inventory.emplace(
					"system_definitions",
					flags("record_surface_id", std::move(system_record_definition_surfaces_)));
				if (!complete)
				{
					inventory.emplace("reason",
									  sdk::detached_cell::utf8(
										  "record-definition-identity-or-source-unavailable"));
					output_.unresolved.push_back(
						{"record.inventory-frontier",
						 observations_.compile_unit,
						 "record-definition-identity-or-source-unavailable"});
				}
				append(
					make_row(cc::relations::record_inventory::descriptor(), std::move(inventory)));
				auto declarations = common();
				declarations.emplace(
					"profile",
					sdk::detached_cell::utf8("clang22-explicit-admitted-named-declarations/1"));
				declarations.emplace(
					"declaration_count",
					sdk::detached_cell::unsigned_integer(observed_declarations_.size()));
				const bool declarations_complete =
					observed_declarations_.size() == declaration_ids_.size();
				declarations.emplace("enumeration_state",
									 symbol("cc.declaration-inventory-state/1",
											declarations_complete ? "complete" : "partial"));
				declarations.emplace("declarations",
									 flags("cc_declaration_id", std::move(declaration_ids_)));
				declarations.emplace(
					"system_declarations",
					flags("cc_declaration_id", std::move(system_declaration_ids_)));
				declarations.emplace("parsed_files", flags("file_id", std::move(parsed_files_)));
				declarations.emplace("parsed_source_snapshots",
									 flags("source_snapshot_id", std::move(parsed_snapshots_)));
				declarations.emplace("file_state",
									 symbol("cc.declaration-file-state/1", "complete"));
				if (!declarations_complete)
				{
					declarations.emplace(
						"reason",
						sdk::detached_cell::utf8("declaration-identity-or-source-unavailable"));
					output_.unresolved.push_back(
						{"declaration.inventory-frontier",
						 observations_.compile_unit,
						 "supply-written-declaration-identity-and-source"});
				}
				append(make_row(cc::relations::declaration_inventory::descriptor(),
								std::move(declarations)));
				if (progress_)
					progress_("finishing " + std::to_string(rows_.size()) + " detached rows");
				output_.rows.reserve(rows_.size());
				for (auto& [key, row] : rows_)
				{
					(void)key;
					output_.rows.push_back(std::move(row));
				}
				return std::move(output_);
			}

		  private:
			void retain_call_bytes(std::size_t bytes)
			{
				if (bytes > 64U * 1024U * 1024U - call_bytes_)
					fail("call-operands", "retained-byte-limit");
				call_bytes_ += bytes;
			}
			void call_scope_fields(fields& value, const ast_enumeration& enumeration)
			{
				value.emplace(
					"call_site_count",
					sdk::detached_cell::unsigned_integer(enumeration.call_admissions.size()));
				value.emplace("call_site_ids", flags("cc_call_id", enumeration.call_sites));
				value.emplace(
					"call_site_state",
					symbol("cc.function-call-site-state/1",
						   enumeration.call_admissions.size() == enumeration.call_sites.size() &&
								   !enumeration.call_frontier
							   ? "complete"
							   : "partial"));
				value.emplace("call_site_profile",
							  sdk::detached_cell::utf8("clang22-function-emitted-call-sites/1"));
			}
			void admit_call(const clang::Expr* expression)
			{
				if (!expression || !admitted(expression->getExprLoc()) || ast_enumerations_.empty())
					return;
				auto& enumeration = ast_enumerations_.back();
				if (!enumeration.call_admissions.contains(expression))
				{
					if (enumeration.call_admissions.size() >= 100000U)
						fail("function-call-sites", "admission-count-limit");
					retain_call_bytes(64U);
					enumeration.call_admissions.insert(expression);
				}
				const auto found = original_calls_.find(expression);
				const auto original = found == original_calls_.end()
					? original_sites_.end()
					: original_sites_.find(found->second);
				if (original == original_sites_.end() || current_function_.empty() ||
					!original->second.cells.at("cc.call_site.v1.caller").value ||
					row_id(original->second, "caller") != current_function_)
				{
					enumeration.call_frontier = true;
					output_.unresolved.push_back(
						{"function.call-site-frontier",
						 observations_.compile_unit,
						 "supply-original-call-source-and-owner-identity"});
					return;
				}
				if (!enumeration.call_sites.contains(found->second))
				{
					retain_call_bytes(found->second.size() + 128U);
					enumeration.call_sites.insert(found->second);
				}
			}
			std::string evaluation_context(const clang::Expr& expression)
			{
				if (unevaluated_depth_ != 0U)
					return "unevaluated";
				if (expression.isTypeDependent() || expression.isValueDependent())
					return "dependent";
				for (const auto* ancestor : statements_)
				{
					if (++category_work_ > 32'000'000U)
						fail("call-operands", "context-operation-limit");
					if (llvm::isa<clang::CXXNoexceptExpr,
								  clang::TypeTraitExpr,
								  clang::ExpressionTraitExpr,
								  clang::RequiresExpr,
								  clang::OffsetOfExpr,
								  clang::CXXUuidofExpr>(ancestor))
						return "unevaluated";
					if (const auto* trait =
							llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(ancestor))
						if (trait->getKind() != clang::UETT_SizeOf ||
							!trait->getTypeOfArgument()->isVariableArrayType())
							return "unevaluated";
					if (const auto* typeid_expression =
							llvm::dyn_cast<clang::CXXTypeidExpr>(ancestor))
						if (!typeid_expression->isPotentiallyEvaluated())
							return "unevaluated";
					if (const auto* call = llvm::dyn_cast<clang::CallExpr>(ancestor))
						if (call->isUnevaluatedBuiltinCall(unit_.ast()))
							return "unevaluated";
				}
				return "potentially_evaluated";
			}
			void update_original(const sdk::relation_descriptor& descriptor,
								 const sdk::detached_row& original,
								 fields additions)
			{
				auto updated = original;
				for (auto& [name, cell] : additions)
				{
					const auto column = take(descriptor.column(descriptor.id + "." + name));
					cell.type = column.type;
					updated.cells.insert_or_assign(column.id, std::move(cell));
				}
				check(sdk::validate_row(descriptor, updated));
				if (descriptor.domain_identity.result_column)
					check(sdk::validate_domain_identity(descriptor, updated));
				retain_call_bytes(updated.canonical_form().size() + 128U);
				const auto key = descriptor.id + row_id(original, "call");
				const auto found = output_.call_updates.find(key);
				if (found != output_.call_updates.end() &&
					found->second.canonical_form() != updated.canonical_form())
					fail("call-operands", "conflicting-original-call-update");
				output_.call_updates.insert_or_assign(key, std::move(updated));
			}
			void call_target_signature(const clang::Expr& expression,
									   const std::string& call,
									   const clang::FunctionDecl* target)
			{
				const auto original = original_targets_.find(call);
				if (!target || original == original_targets_.end())
					return;
				target = target->getCanonicalDecl();
				const auto actual_type = canonical_type(target->getType());
				const auto& structure = type_structure_.at(actual_type);
				const auto actual_usr = usr(target);
				const auto expected_entity = entity(target);
				const bool complete = !actual_usr.empty() && structure.state == "complete" &&
					(expected_entity.empty() ||
					 expected_entity == row_id(original->second, "target"));
				fields additions{
					{"target_signature_state",
					 symbol("cc.target-signature-state/1", complete ? "complete" : "partial")},
					{"target_signature_profile",
					 sdk::detached_cell::utf8("clang22-original-target-signature/1")},
					{"target_canonical_type", id("cc_type_id", actual_type)},
					{"target_canonical_type_digest", digest_value(structure.digest)},
					{"target_canonical_type_profile",
					 sdk::detached_cell::utf8("clang22-structural-type/1")},
					{"target_structural_signature_digest",
					 digest_value(take(sdk::semantic_digest(
						 "cc.entity.structural-signature.v1",
						 target->getType().getCanonicalType().getAsString())))},
					{"target_language",
					 symbol("build.language/1", unit_.ast().getLangOpts().CPlusPlus ? "c++" : "c")},
					{"target_linkage",
					 symbol("cc.linkage/1",
							target->isExternallyVisible() ? "external" : "internal")},
					{"target_module_domain",
					 sdk::detached_cell::utf8(target->getOwningModule()
												  ? target->getOwningModule()->getFullModuleName()
												  : "<none>")}};
				if (!actual_usr.empty())
				{
					std::vector<std::byte> bytes;
					retain_call_bytes(actual_usr.size());
					bytes.reserve(actual_usr.size());
					for (const auto ch : actual_usr)
						bytes.push_back(static_cast<std::byte>(ch));
					additions.emplace("target_usr", sdk::detached_cell::bytes(std::move(bytes)));
				}
				else
					additions.emplace(
						"target_usr",
						sdk::detached_cell::unknown({sdk::scalar_kind::bytes, {}, true},
													"compiler-target-usr-unavailable"));
				if (!complete)
					output_.unresolved.push_back(
						{"call.target-signature-frontier",
						 call,
						 "supply-complete-original-target-identity-and-type"});
				(void)expression;
				update_original(cc::relations::call_direct_target::descriptor(),
								original->second,
								std::move(additions));
			}
			void call_operand(const clang::Expr& invocation,
							  const std::string& call,
							  const clang::Expr* input,
							  std::string kind,
							  std::uint64_t index,
							  std::optional<std::uint64_t> actual,
							  std::optional<std::uint64_t> formal,
							  std::optional<std::uint64_t> written,
							  std::string origin)
			{
				const bool default_argument = input && llvm::isa<clang::CXXDefaultArgExpr>(input);
				const bool default_initializer =
					input && llvm::isa<clang::CXXDefaultInitExpr>(input);
				const bool implicit =
					default_argument || default_initializer || origin == "implicit_receiver";
				if (default_argument)
				{
					origin = "default_argument";
					input = llvm::cast<clang::CXXDefaultArgExpr>(input)->getExpr();
				}
				else if (default_initializer)
				{
					origin = "default_initializer";
					input = llvm::cast<clang::CXXDefaultInitExpr>(input)->getExpr();
				}
				fields value = common();
				value.emplace("call", id("cc_call_id", call));
				value.emplace("kind", symbol("cc.call-operand-kind/1", std::move(kind)));
				value.emplace("index", sdk::detached_cell::unsigned_integer(index));
				value.emplace("origin", symbol("cc.call-operand-origin/1", std::move(origin)));
				value.emplace("implicit", sdk::detached_cell::boolean(implicit));
				value.emplace("default_argument", sdk::detached_cell::boolean(default_argument));
				if (actual)
					value.emplace("actual_argument_index",
								  sdk::detached_cell::unsigned_integer(*actual));
				if (formal)
					value.emplace("formal_parameter_index",
								  sdk::detached_cell::unsigned_integer(*formal));
				if (written)
					value.emplace("written_argument_index",
								  sdk::detached_cell::unsigned_integer(*written));
				bool complete = input != nullptr;
				std::string category = "unknown";
				if (input)
				{
					category = input->isTypeDependent() || input->isValueDependent() ? "dependent"
						: input->isLValue()											 ? "lvalue"
						: input->isXValue()											 ? "xvalue"
																					 : "prvalue";
					const auto source = span(input->getSourceRange(), "expression");
					if (source)
						value.emplace("source", id("source_span_id", *source));
					else
					{
						complete = false;
						value.emplace("source",
									  sdk::detached_cell::unknown(
										  {sdk::scalar_kind::typed_id, "source_span_id", true},
										  "operand-source-unavailable"));
					}
					const auto node = syntax_nodes_.find(input);
					if (node != syntax_nodes_.end() && node->second.size() == 1U)
						value.emplace("expression", id("syntax_node_id", *node->second.begin()));
					else
					{
						complete = false;
						value.emplace("expression",
									  sdk::detached_cell::unknown(
										  {sdk::scalar_kind::typed_id, "syntax_node_id", true},
										  "operand-original-expression-unavailable"));
					}
					if (!input->getType().isNull())
						value.emplace("type", id("cc_type_id", canonical_type(input->getType())));
					else
					{
						complete = false;
						value.emplace("type",
									  sdk::detached_cell::unknown(
										  {sdk::scalar_kind::typed_id, "cc_type_id", true},
										  "operand-type-unavailable"));
					}
					const auto* named = input->IgnoreParenImpCasts();
					const clang::NamedDecl* referenced = nullptr;
					if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(named))
						referenced = reference->getDecl();
					else if (const auto* member = llvm::dyn_cast<clang::MemberExpr>(named))
						referenced = member->getMemberDecl();
					if (referenced)
					{
						const auto target = entity(referenced);
						if (!target.empty())
							value.emplace("referenced_entity", id("cc_entity_id", target));
						else
						{
							complete = false;
							value.emplace("referenced_entity",
										  sdk::detached_cell::unknown(
											  {sdk::scalar_kind::typed_id, "cc_entity_id", true},
											  "operand-referenced-entity-unavailable"));
						}
					}
					else if (named->isTypeDependent() || named->isValueDependent())
					{
						complete = false;
						value.emplace("referenced_entity",
									  sdk::detached_cell::unknown(
										  {sdk::scalar_kind::typed_id, "cc_entity_id", true},
										  "dependent-operand-reference"));
					}
				}
				value.emplace("value_category", symbol("cc.value-category/1", std::move(category)));
				const auto context = call_evaluation_.find(&invocation);
				const auto evaluation =
					context == call_evaluation_.end() ? "unknown" : context->second;
				value.emplace("evaluation_state",
							  symbol("cc.expression-evaluation-state/1", evaluation));
				if (evaluation == "unknown")
					complete = false;
				value.emplace("observation_state",
							  symbol("cc.call-operand-observation-state/1",
									 complete ? "complete" : "partial"));
				if (!complete)
					value.emplace("reason",
								  sdk::detached_cell::utf8(
									  "operand-source-expression-type-or-reference-frontier"));
				append(make_row(cc::relations::call_operand::descriptor(), std::move(value)));
			}
			void finish_calls()
			{
				// Pointer order affects work order only. Every detached identity/row order uses
				// original canonical IDs and relation serialization, never an AST address.
				for (const auto& [expression, call] : original_calls_)
				{
					const auto original = original_sites_.find(call);
					if (original == original_sites_.end())
						continue;
					const auto* function_call = llvm::dyn_cast<clang::CallExpr>(expression);
					const auto* construction = llvm::dyn_cast<clang::CXXConstructExpr>(expression);
					if (!function_call && !construction)
						continue;
					const auto* target = function_call ? function_call->getDirectCallee()
													   : construction->getConstructor();
					const auto actual_count =
						function_call ? function_call->getNumArgs() : construction->getNumArgs();
					if (actual_count > 4096U)
						fail("call-operands", "per-call-slot-limit");
					const auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(target);
					const bool operator_object =
						method && llvm::isa<clang::CXXOperatorCallExpr>(expression);
					const bool first_receiver = method && !method->isStatic() &&
						(method->isExplicitObjectMemberFunction() || operator_object);
					const bool skipped_object = first_receiver || operator_object;
					const auto* member_call = llvm::dyn_cast<clang::CXXMemberCallExpr>(expression);
					const clang::Expr* receiver = first_receiver && actual_count
						? function_call->getArg(0)
						: member_call && (!method || !method->isStatic())
						? member_call->getImplicitObjectArgument()
						: nullptr;
					const bool receiver_expected =
						first_receiver || (member_call && (!method || !method->isStatic()));
					const auto arguments =
						actual_count - (skipped_object && actual_count ? 1U : 0U);
					const bool result = construction ||
						(!expression->getType().isNull() && !expression->getType()->isVoidType());
					fields population{
						{"argument_count", sdk::detached_cell::unsigned_integer(arguments)},
						{"operand_count",
						 sdk::detached_cell::unsigned_integer(
							 arguments + (receiver_expected ? 1U : 0U) + (result ? 1U : 0U))},
						{"operand_population_state",
						 symbol("cc.call-operand-population-state/1",
								skipped_object && actual_count == 0U ? "partial" : "complete")},
						{"operand_profile",
						 sdk::detached_cell::utf8("clang22-actual-call-operands/1")}};
					const auto node = syntax_nodes_.find(expression);
					if (node != syntax_nodes_.end() && node->second.size() == 1U)
						population.emplace("expression",
										   id("syntax_node_id", *node->second.begin()));
					else
						population.emplace("expression",
										   sdk::detached_cell::unknown(
											   {sdk::scalar_kind::typed_id, "syntax_node_id", true},
											   "call-original-expression-unavailable"));
					update_original(cc::relations::call_site::descriptor(),
									original->second,
									std::move(population));
					if (receiver_expected)
						call_operand(
							*expression,
							call,
							receiver,
							"receiver",
							0U,
							first_receiver ? std::optional<std::uint64_t>{0U} : std::nullopt,
							method && method->isExplicitObjectMemberFunction()
								? std::optional<std::uint64_t>{0U}
								: std::nullopt,
							first_receiver ? std::optional<std::uint64_t>{0U} : std::nullopt,
							first_receiver ? "written" : "implicit_receiver");
					std::uint64_t slot{}, written{};
					for (unsigned actual = skipped_object && actual_count ? 1U : 0U;
						 actual < actual_count;
						 ++actual)
					{
						const auto* argument = function_call ? function_call->getArg(actual)
															 : construction->getArg(actual);
						const bool injected =
							llvm::isa<clang::CXXDefaultArgExpr, clang::CXXDefaultInitExpr>(
								argument);
						const auto formal = actual -
							(skipped_object && method && !method->isExplicitObjectMemberFunction()
								 ? 1U
								 : 0U);
						call_operand(*expression,
									 call,
									 argument,
									 "argument",
									 slot++,
									 actual,
									 target && formal < target->getNumParams()
										 ? std::optional<std::uint64_t>{formal}
										 : std::nullopt,
									 injected ? std::nullopt
											  : std::optional<std::uint64_t>{written++},
									 "written");
					}
					if (result)
						call_operand(*expression,
									 call,
									 expression,
									 "result",
									 0U,
									 std::nullopt,
									 std::nullopt,
									 std::nullopt,
									 "result");
					call_target_signature(*expression, call, target);
				}
			}
			void retain_population_bytes(std::size_t bytes)
			{
				if (bytes > 64U * 1024U * 1024U - population_bytes_)
					fail("finite-populations", "retained-byte-limit");
				population_bytes_ += bytes;
			}
			void retain_direct_call_bytes(std::size_t bytes)
			{
				if (bytes > 64U * 1024U * 1024U - direct_call_bytes_)
					fail("body-direct-calls", "retained-byte-limit");
				direct_call_bytes_ += bytes;
			}
			void retain_inventory_bytes(std::size_t bytes)
			{
				if (bytes > 64U * 1024U * 1024U - record_inventory_bytes_)
					fail("record-inventory", "retained-byte-limit");
				record_inventory_bytes_ += bytes;
			}
			void mark_template(clang::SourceRange range)
			{
				auto& manager = unit_.source_manager();
				const auto expansion = manager.getExpansionRange(range);
				const auto begin = manager.getExpansionLoc(expansion.getBegin());
				const auto* member = admitted(begin);
				if (!member)
					return;
				auto end = manager.getExpansionLoc(expansion.getEnd());
				if (expansion.isTokenRange())
					end = clang::Lexer::getLocForEndOfToken(
						end, 0U, manager, unit_.ast().getLangOpts());
				if (end.isInvalid() || !manager.isWrittenInSameFile(begin, end) ||
					manager.getFileOffset(end) > member->size_bytes)
				{
					template_incomplete_.insert(member->logical_path);
					return;
				}
				template_ranges_[member->logical_path].emplace_back(manager.getFileOffset(begin),
																	manager.getFileOffset(end));
			}
			const source_closure_member* admitted(clang::SourceLocation location) const
			{
				if (location.isInvalid())
					return nullptr;
				return closure_.find_member(project_path(unit_.source_manager().getFilename(
					unit_.source_manager().getExpansionLoc(location))));
			}
			std::optional<std::string> span(clang::SourceRange range, std::string_view role)
			{
				auto& manager = unit_.source_manager();
				const auto expansion = manager.getExpansionRange(range);
				const auto begin = manager.getExpansionLoc(expansion.getBegin());
				const auto* member = admitted(begin);
				if (!member)
					return std::nullopt;
				auto end = manager.getExpansionLoc(expansion.getEnd());
				if (expansion.isTokenRange())
					end = clang::Lexer::getLocForEndOfToken(
						end, 0U, manager, unit_.ast().getLangOpts());
				if (end.isInvalid() || !manager.isWrittenInSameFile(begin, end))
					return std::nullopt;
				const auto first = manager.getFileOffset(begin), last = manager.getFileOffset(end);
				if (last < first || last > member->size_bytes)
					fail("source", "span-outside-frozen-input");
				const auto snapshot = take(sdk::detail::derive_source_snapshot_id(
					member->file_id, member->content_digest, "utf8"));
				auto row = make_row(source::relations::span::descriptor(),
									{{"snapshot", id("source_snapshot_id", snapshot)},
									 {"file", id("file_id", member->file_id)},
									 {"begin", sdk::detached_cell::unsigned_integer(first)},
									 {"end", sdk::detached_cell::unsigned_integer(last)},
									 {"role", symbol("source.range-role/1", std::string{role})},
									 {"read_only",
									  sdk::detached_cell::boolean(role == "preprocessor" ||
																  range.getBegin().isMacroID() ||
																  range.getEnd().isMacroID())}});
				const auto result = row_id(row, "span");
				append(std::move(row));
				return result;
			}
			std::string entity(const clang::NamedDecl* declaration) const
			{
				if (!declaration)
					return {};
				const auto* canonical = declaration->getCanonicalDecl();
				if (const auto found = entity_lookup_.find(canonical);
					found != entity_lookup_.end())
					return found->second;
				auto key = usr(declaration);
				if (key.empty())
				{
					const auto* member = admitted(declaration->getLocation());
					if (!member)
						return {};
					const auto location =
						unit_.source_manager().getExpansionLoc(declaration->getBeginLoc());
					const auto found = anchors_.find(
						{member->file_id, unit_.source_manager().getFileOffset(location)});
					if (found == anchors_.end())
						return {};
					key = found->second;
				}
				const auto found = entities_.find(key);
				const auto result = found == entities_.end() ? std::string{} : found->second;
				entity_lookup_.emplace(canonical, result);
				return result;
			}
			fields common() const
			{
				return {{"compile_unit", id("compile_unit_id", observations_.compile_unit)}};
			}
			std::string canonical_type(clang::QualType input)
			{
				if (input.isNull())
					fail("type", "missing-compiler-type");
				const auto type = input.getCanonicalType();
				if (const auto found = types_.find(type.getAsOpaquePtr()); found != types_.end())
					return found->second;
				if (++type_depth_ > 256U)
					fail("type", "recursion-limit");
				std::vector<std::tuple<std::string, std::string, std::string, std::string>>
					components;
				auto component = [&](std::string role, clang::QualType child)
				{
					components.emplace_back(std::move(role), canonical_type(child), "", "");
				};
				std::string constructor = type->getTypeClassName(),
							signature = "clang22-structural-type-v1\n";
				const clang::NamedDecl* nominal = nullptr;
				bool structural_complete = !type->isDependentType();
				if (const auto* record = type->getAs<clang::RecordType>())
				{
					constructor = "record";
					nominal = record->getDecl();
				}
				else if (const auto* enumeration = type->getAs<clang::EnumType>())
				{
					constructor = "enum";
					nominal = enumeration->getDecl();
				}
				else if (const auto* builtin =
							 llvm::dyn_cast<clang::BuiltinType>(type.getTypePtr()))
				{
					constructor = "builtin";
					signature += std::to_string(static_cast<unsigned>(builtin->getKind())) + "\n";
					if (!type->isVoidType() && !type->isIncompleteType() &&
						!type->isDependentType())
						signature += std::to_string(unit_.ast().getTypeSize(type)) + ":" +
							std::to_string(unit_.ast().getTypeAlign(type));
				}
				else if (const auto* pointer = type->getAs<clang::PointerType>())
				{
					constructor = "pointer";
					component("pointee", pointer->getPointeeType());
				}
				else if (const auto* reference = type->getAs<clang::ReferenceType>())
				{
					constructor =
						type->isLValueReferenceType() ? "lvalue_reference" : "rvalue_reference";
					component("pointee", reference->getPointeeType());
				}
				else if (const auto* array = unit_.ast().getAsArrayType(type))
				{
					constructor = "array";
					component("element", array->getElementType());
					if (const auto* sized = llvm::dyn_cast<clang::ConstantArrayType>(array))
					{
						llvm::SmallString<64> extent;
						sized->getSize().toString(extent, 10, false);
						components.emplace_back(
							"extent",
							"",
							take(sdk::semantic_digest("cc.array-extent.v1", extent.str())),
							extent.str().str());
					}
					else
					{
						if (!array->isIncompleteArrayType())
							structural_complete = false;
						signature +=
							array->isIncompleteArrayType() ? "incomplete" : "runtime-or-dependent";
					}
				}
				else if (const auto* function = type->getAs<clang::FunctionType>())
				{
					constructor = "function";
					component("return", function->getReturnType());
					signature +=
						std::to_string(static_cast<unsigned>(function->getCallConv())) + "\n";
					if (const auto* prototype = llvm::dyn_cast<clang::FunctionProtoType>(function))
					{
						if (clang::isUnresolvedExceptionSpec(prototype->getExceptionSpecType()) ||
							prototype->getExceptionSpecType() == clang::EST_Unparsed ||
							prototype->getExceptionSpecType() == clang::EST_DependentNoexcept)
							structural_complete = false;
						for (const auto parameter : prototype->param_types())
							component("parameter", parameter);
						signature += prototype->isVariadic() ? "variadic\n" : "fixed\n";
						signature += std::to_string(
										 static_cast<unsigned>(prototype->getExceptionSpecType())) +
							"\n";
						signature +=
							std::to_string(static_cast<unsigned>(prototype->getRefQualifier())) +
							"\n";
						signature += std::to_string(prototype->getMethodQuals().getAsOpaqueValue());
					}
					else
						signature += "no-prototype";
				}
				else if (const auto* member = type->getAs<clang::MemberPointerType>())
				{
					constructor = "member_pointer";
					component("pointee", member->getPointeeType());
					if (const auto* owner = member->getMostRecentCXXRecordDecl())
						component("owner", unit_.ast().getCanonicalTagType(owner));
					else if (const auto* owner_type = member->getQualifier().getAsType())
						component("owner", clang::QualType{owner_type, 0});
					else
					{
						structural_complete = false;
						signature += type.getAsString();
						output_.unresolved.push_back({"type.member-pointer-owner-frontier",
													  observations_.compile_unit,
													  "instantiate-dependent-owner"});
					}
				}
				else if (const auto* atomic = type->getAs<clang::AtomicType>())
				{
					constructor = "atomic";
					component("value", atomic->getValueType());
				}
				else if (const auto* complex = type->getAs<clang::ComplexType>())
				{
					constructor = "complex";
					component("element", complex->getElementType());
				}
				else if (const auto* vector = type->getAs<clang::VectorType>())
				{
					constructor = "vector";
					component("element", vector->getElementType());
					signature += std::to_string(vector->getNumElements());
				}
				else if (const auto* integer = type->getAs<clang::BitIntType>())
				{
					constructor = "bitint";
					signature += std::to_string(integer->getNumBits()) +
						(integer->isUnsigned() ? ":unsigned" : ":signed");
				}
				else
				{
					structural_complete = false;
					// Canonical spelling is supplemental identity only for types whose structure is
					// dependent or not represented by this analysis profile. It cannot assert
					// equality.
					signature += type.getAsString();
					output_.unresolved.push_back(
						{"type.structure-frontier", observations_.compile_unit, constructor});
				}
				const auto nominal_id = entity(nominal);
				if (nominal)
					signature += "\n" + usr(nominal);
				for (const auto& [role, child, value, preimage] : components)
				{
					(void)preimage;
					signature += "\n" + role + ":" + child + ":" + value;
				}
				if (type.hasAddressSpace())
					signature += "\naddress-space:" +
						std::to_string(static_cast<unsigned>(type.getAddressSpace()));
				std::set<std::string, std::less<>> qualifiers;
				if (type.isConstQualified())
					qualifiers.emplace("const");
				if (type.isVolatileQualified())
					qualifiers.emplace("volatile");
				if (type.isRestrictQualified())
					qualifiers.emplace("restrict");
				fields value{
					{"constructor", symbol("cc.type-constructor/1", constructor)},
					{"component_signature_digest",
					 digest_value(
						 take(sdk::semantic_digest("cc.clang22.type-components.v1", signature)))},
					{"qualifiers", flags("cc.type-qualifier/1", std::move(qualifiers))},
					{"dependent", sdk::detached_cell::boolean(type->isDependentType())},
					{"structure_profile", sdk::detached_cell::utf8("clang22-structural-type/1")},
					{"structure_preimage", sdk::detached_cell::utf8(signature)},
					{"structure_state",
					 symbol("cc.type-structure-state/1",
							structural_complete ? "complete" : "partial")},
					{"spelling", sdk::detached_cell::utf8(type.getAsString())}};
				if (!nominal_id.empty())
					value.emplace("nominal_entity", id("cc_entity_id", nominal_id));
				auto row = make_row(cc::relations::type::descriptor(), std::move(value));
				const auto result = row_id(row, "type");
				types_.emplace(type.getAsOpaquePtr(), result);
				const auto digest = row_id(row, "component_signature_digest");
				retain_call_bytes(result.size() + digest.size() + 256U);
				type_structure_.emplace(
					result, type_structure{digest, structural_complete ? "complete" : "partial"});
				append(std::move(row));
				std::uint64_t ordinal{};
				for (const auto& [role, child, parameter, preimage] : components)
				{
					fields part{{"owner_type", id("cc_type_id", result)},
								{"role", symbol("cc.type-component-role/1", role)},
								{"ordinal", sdk::detached_cell::unsigned_integer(ordinal++)}};
					if (!child.empty())
						part.emplace("component_type", id("cc_type_id", child));
					if (!parameter.empty())
					{
						part.emplace("value_digest", digest_value(parameter));
						part.emplace("value_preimage", sdk::detached_cell::utf8(preimage));
					}
					append(make_row(cc::relations::type_component::descriptor(), std::move(part)));
				}
				--type_depth_;
				return result;
			}
			struct type_reference_enumeration
			{
				std::set<const clang::Type*> visited;
				std::set<std::string, std::less<>> targets;
				std::size_t operations{}, target_bytes{};
				std::string state = "complete", reason;
				void incomplete(std::string code)
				{
					state = "partial";
					if (reason.empty() || code < reason)
						reason = std::move(code);
				}
				void tick(unsigned depth)
				{
					if (depth > 128U || ++operations > 200'000U)
						fail("record-type-references", "traversal-limit");
				}
			};
			void nominal_reference(const clang::NamedDecl* declaration,
								   type_reference_enumeration& result)
			{
				if (!declaration || declaration->getLocation().isInvalid())
				{
					result.incomplete("nominal-declaration-unavailable");
					return;
				}
				if (unit_.source_manager().isInSystemHeader(declaration->getLocation()))
					return;
				const auto target = entity(declaration);
				if (target.empty())
					result.incomplete("nominal-identity-unavailable");
				else if (!result.targets.contains(target))
				{
					constexpr auto maximum_bytes = 64U * 1024U * 1024U;
					if (result.targets.size() >= 200'000U ||
						target.size() > maximum_bytes - result.target_bytes)
						fail("record-type-references", "target-limit");
					result.target_bytes += target.size();
					result.targets.insert(target);
				}
			}
			void template_reference(const clang::TemplateArgument& argument,
									type_reference_enumeration& result,
									unsigned depth)
			{
				result.tick(depth);
				switch (argument.getKind())
				{
					case clang::TemplateArgument::Type:
						type_references(argument.getAsType(), result, depth + 1U);
						break;
					case clang::TemplateArgument::Declaration:
						type_references(argument.getParamTypeForDecl(), result, depth + 1U);
						break;
					case clang::TemplateArgument::NullPtr:
						type_references(argument.getNullPtrType(), result, depth + 1U);
						break;
					case clang::TemplateArgument::Integral:
						type_references(argument.getIntegralType(), result, depth + 1U);
						break;
					case clang::TemplateArgument::StructuralValue:
						type_references(argument.getStructuralValueType(), result, depth + 1U);
						break;
					case clang::TemplateArgument::Expression:
						type_references(argument.getAsExpr()->getType(), result, depth + 1U);
						break;
					case clang::TemplateArgument::Pack:
						for (const auto& element : argument.pack_elements())
							template_reference(element, result, depth + 1U);
						break;
					case clang::TemplateArgument::Template:
					case clang::TemplateArgument::TemplateExpansion:
					{
						const auto* declaration =
							argument.getAsTemplateOrTemplatePattern().getAsTemplateDecl();
						if (const auto* record =
								llvm::dyn_cast_or_null<clang::ClassTemplateDecl>(declaration))
							nominal_reference(record->getTemplatedDecl(), result);
						else
							result.incomplete("template-nominal-identity-unavailable");
						if (argument.getKind() == clang::TemplateArgument::TemplateExpansion)
							result.incomplete("dependent-template-expansion");
						break;
					}
					case clang::TemplateArgument::Null:
						result.incomplete("template-argument-unavailable");
						break;
				}
			}
			void type_references(clang::QualType input,
								 type_reference_enumeration& result,
								 unsigned depth = 0U)
			{
				result.tick(depth);
				if (input.isNull())
				{
					result.incomplete("compiler-type-unavailable");
					return;
				}
				const auto type = input.getCanonicalType();
				if (!result.visited.insert(type.getTypePtr()).second)
					return;
				if (type->isDependentType())
					result.incomplete("dependent-type-requires-specialization");
				const auto child = [&](clang::QualType value)
				{
					type_references(value, result, depth + 1U);
				};
				if (const auto* record = type->getAs<clang::RecordType>())
				{
					nominal_reference(record->getDecl(), result);
					if (const auto* specialization =
							llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
								record->getDecl()))
						for (const auto& argument : specialization->getTemplateArgs().asArray())
							template_reference(argument, result, depth + 1U);
				}
				else if (const auto* enumeration = type->getAs<clang::EnumType>())
					nominal_reference(enumeration->getDecl(), result);
				else if (const auto* pointer = type->getAs<clang::PointerType>())
					child(pointer->getPointeeType());
				else if (const auto* reference = type->getAs<clang::ReferenceType>())
					child(reference->getPointeeType());
				else if (const auto* array = unit_.ast().getAsArrayType(type))
					child(array->getElementType());
				else if (const auto* function = type->getAs<clang::FunctionType>())
				{
					child(function->getReturnType());
					if (const auto* prototype = llvm::dyn_cast<clang::FunctionProtoType>(function))
						for (const auto parameter : prototype->param_types())
							child(parameter);
				}
				else if (const auto* member = type->getAs<clang::MemberPointerType>())
				{
					child(member->getPointeeType());
					if (const auto* owner = member->getMostRecentCXXRecordDecl())
						child(unit_.ast().getCanonicalTagType(owner));
					else if (const auto* owner_type = member->getQualifier().getAsType())
						child(clang::QualType{owner_type, 0});
					else
						result.incomplete("member-pointer-owner-requires-specialization");
				}
				else if (const auto* atomic = type->getAs<clang::AtomicType>())
					child(atomic->getValueType());
				else if (const auto* complex = type->getAs<clang::ComplexType>())
					child(complex->getElementType());
				else if (const auto* vector = type->getAs<clang::VectorType>())
					child(vector->getElementType());
				else if (const auto* specialization =
							 type->getAs<clang::TemplateSpecializationType>())
				{
					const auto* declaration = specialization->getTemplateName().getAsTemplateDecl();
					if (const auto* template_record =
							llvm::dyn_cast_or_null<clang::ClassTemplateDecl>(declaration))
						nominal_reference(template_record->getTemplatedDecl(), result);
					else
						result.incomplete("template-nominal-identity-unavailable");
					for (const auto& argument : specialization->template_arguments())
						template_reference(argument, result, depth + 1U);
				}
				else if (!llvm::isa<clang::BuiltinType, clang::BitIntType>(type.getTypePtr()))
					result.incomplete("unsupported-type-constructor:" +
									  std::string{type->getTypeClassName()});
			}
			void record_surface(const clang::RecordDecl& record,
								std::string_view owner,
								std::string_view source)
			{
				auto value = common();
				value.emplace("entity", id("cc_entity_id", std::string{owner}));
				value.emplace("source", id("source_span_id", std::string{source}));
				value.emplace(
					"profile",
					symbol("cc.record-surface-profile/1", "clang22-explicit-record-surface/1"));
				const bool definition = record.isThisDeclarationADefinition() &&
					record.isCompleteDefinition() && !record.isInvalidDecl();
				value.emplace("is_definition", sdk::detached_cell::boolean(definition));
				std::set<std::string, std::less<>> methods, fields, bases;
				std::set<const clang::Decl*> counted_methods, counted_fields;
				std::uint64_t base_count{};
				std::string state = definition ? "complete" : "unknown";
				std::string reason = definition ? "" : "record-definition-unavailable";
				type_reference_enumeration references;
				if (!definition)
				{
					references.state = "unknown";
					references.reason = "record-definition-unavailable";
				}
				const auto incomplete = [&](std::string code)
				{
					state = "partial";
					if (reason.empty() || code < reason)
						reason = std::move(code);
				};
				const auto retain =
					[&](const clang::NamedDecl* member, std::set<std::string, std::less<>>& ids)
				{
					const auto member_id = entity(member);
					if (member_id.empty() || !admitted(member->getLocation()) ||
						!span(member->getSourceRange(), "declaration"))
						incomplete("record-member-identity-or-source-unavailable");
					else
						ids.insert(member_id);
				};
				if (definition)
				{
					for (const auto* declaration : record.decls())
					{
						if (declaration->isImplicit())
							continue;
						if (const auto* field = llvm::dyn_cast<clang::FieldDecl>(declaration))
						{
							type_references(field->getType(), references);
							if (counted_fields.insert(field->getCanonicalDecl()).second)
								retain(field, fields);
						}
						if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration);
							variable && variable->isStaticDataMember())
							type_references(variable->getType(), references);
						const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(declaration);
						if (const auto* function_template =
								llvm::dyn_cast<clang::FunctionTemplateDecl>(declaration))
							method = llvm::dyn_cast<clang::CXXMethodDecl>(
								function_template->getTemplatedDecl());
						if (method && !method->isImplicit() &&
							counted_methods.insert(method->getCanonicalDecl()).second)
						{
							retain(method, methods);
							type_references(method->getType(), references);
						}
					}
					if (const auto* cxx_record = llvm::dyn_cast<clang::CXXRecordDecl>(&record))
					{
						base_count = cxx_record->getNumBases();
						for (const auto& base : cxx_record->bases())
						{
							type_references(base.getType(), references);
							const auto* target = base.getType()->getAs<clang::RecordType>();
							const auto target_id =
								target ? entity(target->getDecl()) : std::string{};
							if (target_id.empty())
								incomplete("record-base-identity-unavailable");
							else
								bases.insert(target_id);
						}
					}
				}
				value.emplace("method_count",
							  sdk::detached_cell::unsigned_integer(counted_methods.size()));
				value.emplace("field_count",
							  sdk::detached_cell::unsigned_integer(counted_fields.size()));
				value.emplace("base_specifier_count",
							  sdk::detached_cell::unsigned_integer(base_count));
				// make_row binds the shared canonical string-set bytes to the typed-ID set columns.
				value.emplace("methods", flags("cc_entity_id", std::move(methods)));
				value.emplace("fields", flags("cc_entity_id", std::move(fields)));
				value.emplace("base_targets", flags("cc_entity_id", std::move(bases)));
				value.emplace("type_reference_count",
							  sdk::detached_cell::unsigned_integer(references.targets.size()));
				value.emplace("type_reference_state",
							  symbol("cc.record-type-reference-state/1", references.state));
				value.emplace("type_reference_profile",
							  symbol("cc.record-type-reference-profile/1",
									 "clang22-explicit-nonsystem-nominal-type-uses/1"));
				for (const auto& target : references.targets)
					edge(std::string{owner}, target, std::string{source}, "uses_type");
				value.emplace("type_reference_targets",
							  flags("cc_entity_id", std::move(references.targets)));
				if (!references.reason.empty())
				{
					value.emplace("type_reference_reason",
								  sdk::detached_cell::utf8(references.reason));
					output_.unresolved.push_back(
						{"record.type-reference-frontier", std::string{owner}, references.reason});
				}
				value.emplace("enumeration_state", symbol("cc.record-surface-state/1", state));
				if (!reason.empty())
				{
					value.emplace("reason", sdk::detached_cell::utf8(reason));
					output_.unresolved.push_back(
						{"record.surface-frontier", std::string{owner}, reason});
				}
				auto surface =
					make_row(cc::relations::record_surface::descriptor(), std::move(value));
				if (definition)
				{
					const auto id = row_id(surface, "surface");
					if (!record_definition_surfaces_.contains(id))
					{
						retain_inventory_bytes(id.size() + 64U);
						record_definition_surfaces_.insert(id);
					}
					if (unit_.source_manager().isInSystemHeader(
							unit_.source_manager().getExpansionLoc(record.getLocation())) &&
						!system_record_definition_surfaces_.contains(id))
					{
						retain_inventory_bytes(id.size() + 64U);
						system_record_definition_surfaces_.insert(id);
					}
				}
				append(std::move(surface));
			}
			void abi_surface(const std::string& owner,
							 const std::string& source,
							 std::string kind,
							 project_abi_observation observation)
			{
				auto value = common();
				value.emplace("entity", id("cc_entity_id", owner));
				value.emplace("source", id("source_span_id", source));
				value.emplace("kind", symbol("cc.abi-surface-kind/1", std::move(kind)));
				value.emplace(
					"profile",
					symbol("cc.abi-surface-profile/1", "clang22-storage-and-call-interface/1"));
				value.emplace("abi_state", symbol("cc.abi-surface-state/1", observation.abi_state));
				value.emplace("layout_state",
							  symbol("cc.abi-surface-state/1", observation.layout_state));
				if (observation.byte_size)
					value.emplace("byte_size",
								  sdk::detached_cell::unsigned_integer(*observation.byte_size));
				if (observation.byte_alignment)
					value.emplace(
						"byte_alignment",
						sdk::detached_cell::unsigned_integer(*observation.byte_alignment));
				const auto number = [](std::string& out, std::uint64_t n)
				{
					for (unsigned shift{}; shift < 64U; shift += 8U)
						out.push_back(static_cast<char>((n >> shift) & 255U));
				};
				const auto bytes = [](std::string_view text)
				{
					std::vector<std::byte> out;
					out.reserve(text.size());
					for (const auto byte : text)
						out.push_back(static_cast<std::byte>(byte));
					return sdk::detached_cell::bytes(std::move(out));
				};
				if (observation.byte_size)
				{
					std::string ranges;
					ranges.reserve(observation.occupied_ranges.size() * 16U);
					for (const auto& [begin, end] : observation.occupied_ranges)
					{
						number(ranges, begin);
						number(ranges, end);
					}
					value.emplace("occupied_ranges", bytes(ranges));
				}
				if (!observation.abi_context.empty())
					value.emplace("abi_context", digest_value(observation.abi_context));
				if (!observation.abi_signature.empty())
				{
					std::string payload;
					payload.reserve(16U + observation.abi_context.size() +
									observation.abi_signature.size());
					number(payload, observation.abi_context.size());
					payload += observation.abi_context;
					number(payload, observation.abi_signature.size());
					payload += observation.abi_signature;
					value.emplace("abi_signature", bytes(observation.abi_signature));
					value.emplace("abi_fingerprint",
								  digest_value(take(
									  sdk::semantic_digest("cc.clang22.abi-surface.v1", payload))));
				}
				if (!observation.reason.empty())
					value.emplace("reason", sdk::detached_cell::utf8(observation.reason));
				if (observation.abi_state != "complete" ||
					(observation.byte_size && observation.layout_state != "complete"))
					output_.unresolved.push_back(
						{"abi.surface-frontier", owner, observation.reason});
				append(make_row(cc::relations::abi_surface::descriptor(), std::move(value)));
			}
			void layout(const clang::NamedDecl& declaration, clang::QualType type)
			{
				const auto owner = entity(&declaration);
				if (owner.empty())
					return;
				auto value = common();
				value.emplace("entity", id("cc_entity_id", owner));
				value.emplace("kind", symbol("cc.layout-kind/1", "object"));
				value.emplace("canonical_type", id("cc_type_id", canonical_type(type)));
				const bool complete = !type->isIncompleteType() && !type->isDependentType() &&
					!type->isVoidType() && !type->isFunctionType() && !type->isUndeducedType();
				std::string abi = "clang22-object-layout-v1:" + canonical_type(type);
				if (complete)
				{
					const auto size = unit_.ast().getTypeSizeInChars(type).getQuantity();
					const auto alignment = unit_.ast().getTypeAlignInChars(type).getQuantity();
					if (size < 0 || alignment <= 0)
						fail("layout", "invalid-compiler-size");
					value.emplace(
						"byte_size",
						sdk::detached_cell::unsigned_integer(static_cast<std::uint64_t>(size)));
					value.emplace("byte_alignment",
								  sdk::detached_cell::unsigned_integer(
									  static_cast<std::uint64_t>(alignment)));
					abi += ":" + std::to_string(size) + ":" + std::to_string(alignment);
				}
				value.emplace(
					"guarantee",
					symbol("cc.layout-guarantee/1", complete ? "compiler_observed" : "unknown"));
				if (!complete)
					value.emplace("reason",
								  sdk::detached_cell::utf8(type->isDependentType()
															   ? "dependent-type"
															   : "incomplete-type"));
				if (complete)
					if (const auto* record = type->getAs<clang::RecordType>())
					{
						const auto* definition = record->getDecl()->getDefinition();
						if (definition)
						{
							const auto& actual = unit_.ast().getASTRecordLayout(definition);
							unsigned ordinal{};
							for (const auto* field : definition->fields())
							{
								auto member = common();
								member.emplace("entity", id("cc_entity_id", owner));
								member.emplace("kind", symbol("cc.layout-kind/1", "field"));
								const auto field_id = entity(field);
								if (field_id.empty())
								{
									++ordinal;
									continue;
								}
								member.emplace("member", id("cc_entity_id", field_id));
								member.emplace("canonical_type",
											   id("cc_type_id", canonical_type(field->getType())));
								const auto offset = actual.getFieldOffset(ordinal++);
								member.emplace("bit_offset",
											   sdk::detached_cell::unsigned_integer(offset));
								if (field->isBitField())
									member.emplace("bit_width",
												   sdk::detached_cell::unsigned_integer(
													   field->getBitWidthValue()));
								member.emplace(
									"guarantee",
									symbol("cc.layout-guarantee/1", "compiler_observed"));
								const auto signature = canonical_type(field->getType()) + ":" +
									std::to_string(offset) + ":" +
									(field->isBitField() ? std::to_string(field->getBitWidthValue())
														 : "object");
								member.emplace("abi_digest",
											   digest_value(take(sdk::semantic_digest(
												   "cc.clang22.field-layout.v1", signature))));
								abi += ":" + signature;
								append(make_row(cc::relations::layout_fact::descriptor(),
												std::move(member)));
							}
						}
					}
				value.emplace(
					"abi_digest",
					digest_value(take(sdk::semantic_digest("cc.clang22.object-layout.v1", abi))));
				append(make_row(cc::relations::layout_fact::descriptor(), std::move(value)));
			}
			std::string_view result_use(const clang::CallExpr& call)
			{
				// Inspect the actual traversal ancestors; declarations and unevaluated contexts
				// never acquire expression-statement semantics through a surrounding block.
				if (unevaluated_depth_ != 0U)
					return "consumed";
				for (const auto* ancestor : statements_)
				{
					if (++category_work_ > 32'000'000U)
						fail("syntax.categories", "operation-limit");
					if (llvm::isa<clang::UnaryExprOrTypeTraitExpr,
								  clang::CXXNoexceptExpr,
								  clang::TypeTraitExpr,
								  clang::RequiresExpr>(ancestor))
						return "consumed";
					if (const auto* typeid_expression =
							llvm::dyn_cast<clang::CXXTypeidExpr>(ancestor))
						if (!typeid_expression->isPotentiallyEvaluated())
							return "consumed";
				}
				const clang::Stmt* child = &call;
				for (auto position = statements_.rbegin() + 1; position != statements_.rend();
					 ++position)
				{
					if (++category_work_ > 32'000'000U)
						fail("syntax.categories", "operation-limit");
					const auto* parent = *position;
					if (llvm::isa<clang::UnaryExprOrTypeTraitExpr,
								  clang::CXXNoexceptExpr,
								  clang::TypeTraitExpr,
								  clang::RequiresExpr>(parent))
						return "consumed";
					if (const auto* cast = llvm::dyn_cast<clang::CastExpr>(parent))
					{
						if (cast->getType()->isVoidType())
							return "discarded";
						if (!llvm::isa<clang::ImplicitCastExpr>(cast))
							return "consumed";
					}
					else if (llvm::isa<clang::ParenExpr,
									   clang::ExprWithCleanups,
									   clang::MaterializeTemporaryExpr,
									   clang::CXXBindTemporaryExpr,
									   clang::ConstantExpr>(parent))
					{
						// Transparent wrappers retain the parent's use of this result.
					}
					else if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(parent))
					{
						if (binary->getOpcode() != clang::BO_Comma)
							return "consumed";
						if (binary->getLHS() == child)
							return "discarded";
					}
					else if (const auto* conditional =
								 llvm::dyn_cast<clang::AbstractConditionalOperator>(parent))
					{
						if (conditional->getCond() == child)
							return "consumed";
					}
					else if (const auto* loop = llvm::dyn_cast<clang::ForStmt>(parent))
						return loop->getInit() == child || loop->getInc() == child ||
								loop->getBody() == child
							? "discarded"
							: "consumed";
					else if (const auto* branch = llvm::dyn_cast<clang::IfStmt>(parent))
						return branch->getThen() == child || branch->getElse() == child
							? "discarded"
							: "consumed";
					else if (const auto* while_loop = llvm::dyn_cast<clang::WhileStmt>(parent))
						return while_loop->getBody() == child ? "discarded" : "consumed";
					else if (const auto* do_loop = llvm::dyn_cast<clang::DoStmt>(parent))
						return do_loop->getBody() == child ? "discarded" : "consumed";
					else if (const auto* range_loop =
								 llvm::dyn_cast<clang::CXXForRangeStmt>(parent))
						return range_loop->getBody() == child ? "discarded" : "consumed";
					else if (llvm::isa<clang::CompoundStmt>(parent))
						return "discarded";
					else if (!llvm::isa<clang::LabelStmt, clang::SwitchCase, clang::AttributedStmt>(
								 parent))
						return "consumed";
					child = parent;
				}
				return "unknown";
			}
			void append(sdk::detached_row row)
			{
				auto key = row.canonical_form();
				if (rows_.contains(key))
					return;
				const auto size = key.size();
				if (rows_.size() >= 1000000U || size > 128U * 1024U * 1024U - retained_bytes_)
					fail("resource", "fact-output-limit");
				retained_bytes_ += size;
				rows_.emplace(std::move(key), std::move(row));
			}
			void edge(const std::string& from,
					  const std::string& to,
					  const std::string& source,
					  std::string kind)
			{
				if (from.empty())
					return;
				auto value = common();
				value.emplace("source_entity", id("cc_entity_id", from));
				if (!to.empty())
					value.emplace("target_entity", id("cc_entity_id", to));
				value.emplace("source", id("source_span_id", source));
				value.emplace("kind", symbol("cc.entity-edge-kind/1", std::move(kind)));
				value.emplace(
					"resolution",
					symbol("cc.edge-resolution/1", to.empty() ? "unresolved" : "resolved"));
				append(make_row(cc::relations::entity_edge::descriptor(), std::move(value)));
			}
			void state_access(const clang::Expr& expression,
							  const std::string& target,
							  const std::string& source)
			{
				bool read = true, write = false, address = false;
				for (auto iterator = statements_.rbegin(); iterator != statements_.rend();
					 ++iterator)
				{
					if (*iterator == &expression ||
						llvm::isa<clang::ParenExpr, clang::ImplicitCastExpr>(*iterator))
						continue;
					if (const auto* operation = llvm::dyn_cast<clang::BinaryOperator>(*iterator);
						operation && operation->isAssignmentOp() &&
						operation->getLHS()->IgnoreParenImpCasts() == &expression)
					{
						write = true;
						read = operation->isCompoundAssignmentOp();
					}
					if (const auto* operation = llvm::dyn_cast<clang::UnaryOperator>(*iterator))
					{
						write = operation->isIncrementDecrementOp();
						address = operation->getOpcode() == clang::UO_AddrOf;
						if (address)
							read = false;
					}
					break;
				}
				if (read)
					edge(current_function_, target, source, "reads");
				if (write)
					edge(current_function_, target, source, "writes");
				if (address)
					edge(current_function_, target, source, "addresses");
			}
			class local_flow final : public clang::RecursiveASTVisitor<local_flow>
			{
				using visitor = clang::RecursiveASTVisitor<local_flow>;
				using json = sdk::detail::json_value;
				using string_set = std::set<std::string, std::less<>>;
				struct point
				{
					unsigned block;
					unsigned position;
				};
				struct event
				{
					point where;
					std::uint64_t order;
					std::string subject;
					std::string fact;
					bool definition;
					bool strong;
					std::optional<std::string> source;
				};

			  public:
				local_flow(collector& owner,
						   clang::FunctionDecl& function,
						   const clang::CFG& cfg,
						   const std::map<unsigned, std::string>& nodes,
						   std::string body,
						   std::string source)
					: owner_{owner}, function_{function}, cfg_{cfg}, nodes_{nodes},
					  function_id_{owner.entity(&function)}, body_id_{std::move(body)},
					  body_source_{std::move(source)}
				{
					for (const auto* block : cfg)
					{
						block_sizes_.emplace(block->getBlockID(), block->size());
						unsigned position{};
						for (const auto& element : *block)
						{
							if (const auto statement = element.getAs<clang::CFGStmt>())
								points_.emplace(statement->getStmt(),
												point{block->getBlockID(), position});
							++position;
						}
						if (const auto* terminator = block->getTerminatorStmt())
							points_.emplace(terminator, point{block->getBlockID(), position});
						for (const auto& adjacent : block->succs())
						{
							const auto* target = adjacent.getReachableBlock();
							if (!target)
								target = adjacent.getPossiblyUnreachableBlock();
							if (target)
							{
								successors_[block->getBlockID()].insert(target->getBlockID());
								predecessors_[target->getBlockID()].insert(block->getBlockID());
							}
							else
								frontier("cfg-open-edge", nodes.at(block->getBlockID()));
						}
					}
				}
				void run()
				{
					for (const auto* block : cfg_)
					{
						unsigned position{};
						for (const auto& element : *block)
						{
							const point where{block->getBlockID(), position++};
							if (const auto lifetime = element.getAs<clang::CFGLifetimeEnds>())
								emit("lifetime_end",
									 where,
									 owner_.entity(lifetime->getVarDecl()),
									 owner_.span(lifetime->getTriggerStmt()
													 ? lifetime->getTriggerStmt()->getSourceRange()
													 : lifetime->getVarDecl()->getSourceRange(),
												 "statement"),
									 {},
									 {},
									 "compiler_observed");
							if (const auto destructor = element.getAs<clang::CFGAutomaticObjDtor>())
								emit(
									"automatic_destructor",
									where,
									owner_.entity(destructor->getVarDecl()),
									owner_.span(destructor->getTriggerStmt()
													? destructor->getTriggerStmt()->getSourceRange()
													: destructor->getVarDecl()->getSourceRange(),
												"statement"),
									{},
									{},
									"compiler_observed");
							if (const auto loop = element.getAs<clang::CFGLoopExit>())
								emit(
									"loop_exit",
									where,
									function_id_,
									owner_.span(loop->getLoopStmt()->getSourceRange(), "statement"),
									{},
									{},
									"compiler_observed");
						}
					}
					for (const auto* parameter : function_.parameters())
						definition(*parameter, {cfg_.getEntry().getBlockID(), 0}, nullptr, true);
					if (!TraverseStmt(function_.getBody()))
						fail("flow", "traversal-stopped");
					std::ranges::sort(events_,
									  [](const event& a, const event& b)
									  {
										  return std::tie(
													 a.where.block, a.where.position, a.order) <
											  std::tie(b.where.block, b.where.position, b.order);
									  });
					for (const auto& value : events_)
						events_by_block_[value.where.block].push_back(&value);
					fixed_point();
					inventory();
				}
				bool TraverseLambdaExpr(clang::LambdaExpr* expression)
				{
					// The call operator is a distinct body; captures execute in the enclosing body.
					for (auto* initializer : expression->capture_inits())
						if (initializer && !TraverseStmt(initializer))
							return false;
					return true;
				}
				bool TraverseDecl(clang::Decl* declaration)
				{
					if (llvm::isa_and_nonnull<clang::FunctionDecl, clang::RecordDecl>(declaration))
						return true;
					return visitor::TraverseDecl(declaration);
				}
				bool TraverseDecltypeType(clang::DecltypeType*, bool)
				{
					return true;
				}
				bool TraverseDecltypeTypeLoc(clang::DecltypeTypeLoc, bool)
				{
					return true;
				}
				bool TraverseTypeOfExprType(clang::TypeOfExprType* type, bool qualifier)
				{
					return !type->getUnderlyingExpr()->getType()->isVariablyModifiedType() ||
						visitor::TraverseTypeOfExprType(type, qualifier);
				}
				bool TraverseTypeOfExprTypeLoc(clang::TypeOfExprTypeLoc type, bool qualifier)
				{
					return !type.getUnderlyingExpr()->getType()->isVariablyModifiedType() ||
						visitor::TraverseTypeOfExprTypeLoc(type, qualifier);
				}
				bool TraverseIfStmt(clang::IfStmt* statement)
				{
					if (!statement->isConstexpr())
						return visitor::TraverseIfStmt(statement);
					if (!TraverseStmt(statement->getInit()) ||
						!TraverseStmt(statement->getConditionVariableDeclStmt()) ||
						!TraverseStmt(statement->getCond()))
						return false;
					const auto selected = statement->getNondiscardedCase(owner_.unit_.ast());
					if (!selected)
					{
						frontier("dependent-constexpr-branch", function_id_);
						return true;
					}
					return TraverseStmt(*selected);
				}
				bool TraverseChooseExpr(clang::ChooseExpr* expression)
				{
					if (expression->isConditionDependent())
					{
						frontier("dependent-selected-expression", function_id_);
						return true;
					}
					return TraverseStmt(expression->getChosenSubExpr());
				}
				bool TraverseGenericSelectionExpr(clang::GenericSelectionExpr* expression)
				{
					if (expression->isResultDependent())
					{
						frontier("dependent-selected-expression", function_id_);
						return true;
					}
					return TraverseStmt(expression->getResultExpr());
				}
				bool dataTraverseStmtPre(clang::Stmt* statement)
				{
					// Follow compiler evaluation semantics before collecting events. An
					// ancestor CFG location must never make an unevaluated child a use.
					if (llvm::isa<clang::CXXNoexceptExpr,
								  clang::TypeTraitExpr,
								  clang::ExpressionTraitExpr,
								  clang::RequiresExpr,
								  clang::OffsetOfExpr,
								  clang::CXXUuidofExpr>(statement))
						return false;
					if (const auto* trait =
							llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(statement))
						if (trait->getKind() != clang::UETT_SizeOf ||
							!trait->getTypeOfArgument()->isVariableArrayType())
							return false;
					if (const auto* typeid_expression =
							llvm::dyn_cast<clang::CXXTypeidExpr>(statement))
						if (!typeid_expression->isPotentiallyEvaluated())
							return false;
					if (const auto* call = llvm::dyn_cast<clang::CallExpr>(statement))
						if (call->isUnevaluatedBuiltinCall(owner_.unit_.ast()))
							return false;
					stack_.push_back(statement);
					if (stack_.size() > 4096U)
						fail("flow", "depth-limit");
					if (const auto* assignment = llvm::dyn_cast<clang::BinaryOperator>(statement);
						assignment && assignment->isAssignmentOp())
						if (const auto* left = assignment->getLHS()->IgnoreParenImpCasts();
							!assignment->isCompoundAssignmentOp())
							writes_only_.insert(left);
					return true;
				}
				bool dataTraverseStmtPost(clang::Stmt* statement)
				{
					if (const auto* declarations = llvm::dyn_cast<clang::DeclStmt>(statement))
						for (const auto* declaration : declarations->decls())
							if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration))
								if (const auto where = location(statement))
									definition(*variable,
											   *where,
											   variable->getInit(),
											   variable->hasInit());
					if (const auto* operation = llvm::dyn_cast<clang::BinaryOperator>(statement);
						operation && operation->isAssignmentOp())
						write(operation->getLHS(), operation->getRHS(), statement);
					if (const auto* operation = llvm::dyn_cast<clang::UnaryOperator>(statement);
						operation && operation->isIncrementDecrementOp())
						write(operation->getSubExpr(), nullptr, statement);
					stack_.pop_back();
					return true;
				}
				bool VisitDeclRefExpr(clang::DeclRefExpr* expression)
				{
					if (llvm::isa<clang::VarDecl>(expression->getDecl()))
						use(expression, expression->getDecl());
					return true;
				}
				bool VisitVarDecl(clang::VarDecl* declaration)
				{
					observe_subject(*declaration);
					return true;
				}
				bool VisitMemberExpr(clang::MemberExpr* expression)
				{
					if (llvm::isa<clang::FieldDecl>(expression->getMemberDecl()))
						use(expression, expression->getMemberDecl());
					return true;
				}
				bool VisitCallExpr(clang::CallExpr* expression)
				{
					if (const auto where = location(expression))
					{
						const auto* callee = expression->getDirectCallee();
						const auto target = owner_.entity(callee);
						const auto source = owner_.span(expression->getSourceRange(), "expression");
						emit("call", *where, function_id_, source, target, {}, "observed");
						if (!callee || !callee->hasBody())
							frontier("callee-summary-missing",
									 source.value_or(nodes_.at(where->block)));
						for (const auto* argument : expression->arguments())
							if (argument->getType()->isPointerType() ||
								argument->getType()->isReferenceType())
								frontier("call-memory-effect",
										 source.value_or(nodes_.at(where->block)));
					}
					return true;
				}
				bool VisitUnaryOperator(clang::UnaryOperator* expression)
				{
					if (expression->getOpcode() == clang::UO_Deref)
						if (const auto where = location(expression))
						{
							emit("dereference",
								 *where,
								 subject(expression->getSubExpr()),
								 owner_.span(expression->getSourceRange(), "expression"),
								 {},
								 {},
								 "observed");
							frontier("indirect-memory", nodes_.at(where->block));
						}
					return true;
				}

			  private:
				std::string subject(const clang::Expr* expression) const
				{
					if (!expression)
						return {};
					expression = expression->IgnoreParenImpCasts();
					if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression))
						return owner_.entity(reference->getDecl());
					if (const auto* member = llvm::dyn_cast<clang::MemberExpr>(expression))
						return owner_.entity(member->getMemberDecl());
					return {};
				}
				std::optional<point> location(const clang::Stmt* statement) const
				{
					if (const auto found = points_.find(statement); found != points_.end())
						return found->second;
					for (auto iterator = stack_.rbegin(); iterator != stack_.rend(); ++iterator)
					{
						// A VLA sizeof may evaluate a fresh type bound, but the operand of
						// sizeof(existing_array) is not a storage read. Only actual descendant
						// CFG expressions justify an event across this evaluation boundary.
						if (llvm::isa<clang::UnaryExprOrTypeTraitExpr>(*iterator))
							break;
						if (const auto found = points_.find(*iterator); found != points_.end())
							return found->second;
					}
					return std::nullopt;
				}
				bool within_sizeof_operand() const
				{
					return std::ranges::any_of(
						stack_,
						[](const auto* statement)
						{
							return llvm::isa<clang::UnaryExprOrTypeTraitExpr>(statement);
						});
				}
				static std::string encoded(const string_set& values)
				{
					json::array_type array;
					for (const auto& value : values)
						array.push_back(take(json::string(value)));
					return sdk::detail::canonical_json(json::array(std::move(array)));
				}
				std::string emit(std::string kind,
								 point where,
								 const std::string& subject_id,
								 const std::optional<std::string>& source,
								 const std::string& target,
								 std::string value,
								 std::string guarantee,
								 const string_set* definitions = nullptr,
								 const string_set* live = nullptr,
								 const std::string& use_fact = {},
								 const std::string& event_fact = {})
				{
					++admitted_facts_;
					if (!source &&
						(kind == "definition" || kind == "use" || kind == "call" ||
						 kind == "dereference" || kind == "constant" || kind == "null" ||
						 kind == "points_to"))
					{
						++unmapped_facts_;
						frontier("fact-source-missing", function_id_);
					}
					if (subject_id.empty())
					{
						++unmapped_facts_;
						frontier("subject-identity-missing", function_id_);
						return {};
					}
					auto row = owner_.common();
					row.emplace("function", id("cc_entity_id", function_id_));
					const bool reaching = kind.starts_with("reaching_"),
							   liveness = kind.starts_with("live_");
					row.emplace("kind", symbol("cc.flow-kind/1", std::move(kind)));
					row.emplace("node", id("cfg_node_id", nodes_.at(where.block)));
					row.emplace("subject", id("cc_entity_id", subject_id));
					if (source)
						row.emplace("source", id("source_span_id", *source));
					if (!target.empty())
						row.emplace("target", id("cc_entity_id", target));
					if (!value.empty())
						row.emplace("value", sdk::detached_cell::utf8(std::move(value)));
					row.emplace("ordinal", sdk::detached_cell::unsigned_integer(ordinal_++));
					row.emplace("program_point",
								sdk::detached_cell::unsigned_integer(where.position));
					if (definitions)
						row.emplace("definition_facts", flags("flow_fact_id", *definitions));
					if (live)
						row.emplace("live_subjects", flags("cc_entity_id", *live));
					if (!use_fact.empty())
						row.emplace("use_fact", id("flow_fact_id", use_fact));
					if (!event_fact.empty())
						row.emplace("event_fact", id("flow_fact_id", event_fact));
					row.emplace("guarantee", symbol("cc.flow-guarantee/1", std::move(guarantee)));
					auto fact = make_row(cc::relations::flow_fact::descriptor(), std::move(row));
					const auto result = row_id(fact, "fact");
					owner_.retain_population_bytes(result.size() * 3U + 256U);
					fact_ids_.insert(result);
					if (reaching)
						reaching_points_.insert(result);
					if (liveness)
						liveness_points_.insert(result);
					owner_.append(std::move(fact));
					return result;
				}
				void frontier(std::string reason, const std::string& subject_id)
				{
					if (frontiers_.emplace(reason, subject_id).second)
					{
						auto block = cfg_.getEntry().getBlockID();
						for (const auto& [candidate, node] : nodes_)
							if (node == subject_id)
							{
								block = candidate;
								break;
							}
						emit("frontier", {block, 0}, function_id_, {}, {}, reason, "unknown");
						owner_.output_.unresolved.push_back(
							{"flow." + reason, subject_id, "clang22-local-may-flow-v1"});
					}
				}
				void use(const clang::Expr* expression, const clang::NamedDecl* declaration)
				{
					if (writes_only_.contains(expression))
						return;
					const auto where = location(expression);
					if (!where)
					{
						if (within_sizeof_operand())
							return;
						++unmapped_facts_;
						frontier("use-position-missing", function_id_);
						return;
					}
					observe_subject(*declaration);
					const auto variable = owner_.entity(declaration);
					const auto source = owner_.span(expression->getSourceRange(), "expression");
					const auto fact = emit("use", *where, variable, source, {}, {}, "observed");
					if (!fact.empty())
						events_.push_back({*where, order_++, variable, fact, false, false, source});
				}
				void definition(const clang::NamedDecl& declaration,
								point where,
								const clang::Expr* initializer,
								bool initialized)
				{
					observe_subject(declaration);
					const auto variable = owner_.entity(&declaration);
					const auto source = owner_.span(initializer ? initializer->getSourceRange()
																: declaration.getSourceRange(),
													"expression");
					const auto fact = emit("definition",
										   where,
										   variable,
										   source,
										   {},
										   initialized ? "initialized" : "uninitialized",
										   "observed");
					if (fact.empty())
						return;
					const auto* local = llvm::dyn_cast<clang::VarDecl>(&declaration);
					const bool strong = local && local->isLocalVarDeclOrParm() &&
						!local->getType()->isReferenceType();
					events_.push_back({where, order_++, variable, fact, true, strong, source});
					definition_subjects_.emplace(fact, variable);
					if (initializer)
					{
						clang::Expr::EvalResult evaluation;
						if (!initializer->isValueDependent() &&
							initializer->EvaluateAsInt(evaluation, owner_.unit_.ast()))
						{
							llvm::SmallString<128> number;
							evaluation.Val.getInt().toString(number, 10);
							emit("constant",
								 where,
								 variable,
								 source,
								 {},
								 number.str().str(),
								 "compiler_observed");
						}
						if (initializer->isNullPointerConstant(
								owner_.unit_.ast(), clang::Expr::NPC_ValueDependentIsNotNull) !=
							clang::Expr::NPCK_NotNull)
							emit("null", where, variable, source, {}, "null", "compiler_observed");
						if (const auto* address = llvm::dyn_cast<clang::UnaryOperator>(
								initializer->IgnoreParenImpCasts());
							address && address->getOpcode() == clang::UO_AddrOf)
							emit("points_to",
								 where,
								 variable,
								 source,
								 subject(address->getSubExpr()),
								 {},
								 "may");
					}
				}
				void write(const clang::Expr* expression,
						   const clang::Expr* initializer,
						   const clang::Stmt* statement)
				{
					const auto where = location(statement);
					if (!where)
					{
						if (within_sizeof_operand())
							return;
						++unmapped_facts_;
						frontier("write-position-missing", function_id_);
						return;
					}
					expression = expression->IgnoreParenImpCasts();
					if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression))
						definition(*reference->getDecl(), *where, initializer, true);
					else if (const auto* member = llvm::dyn_cast<clang::MemberExpr>(expression))
						definition(*member->getMemberDecl(), *where, initializer, true);
					else
						frontier("indirect-write", nodes_.at(where->block));
				}
				void transfer(string_set& state, const event& value) const
				{
					if (!value.definition)
						return;
					if (value.strong)
						std::erase_if(state,
									  [&](const auto& existing)
									  {
										  return definition_subjects_.at(existing) == value.subject;
									  });
					state.insert(value.fact);
				}
				void fixed_point()
				{
					std::map<unsigned, string_set> incoming, outgoing, live_in, live_out, gen, kill;
					for (const auto& [block, events] : events_by_block_)
						for (const auto* value : events)
							if (value->definition)
							{
								if (value->strong)
									kill[block].insert(value->subject);
							}
							else if (!kill[block].contains(value->subject))
								gen[block].insert(value->subject);
					std::deque<unsigned> queue;
					std::set<unsigned> scheduled;
					for (const auto& [block, node] : nodes_)
					{
						(void)node;
						queue.push_back(block);
						scheduled.insert(block);
					}
					std::size_t steps{};
					bool converged = true;
					while (!queue.empty())
					{
						if (++steps > 1000000U)
						{
							converged = false;
							break;
						}
						const auto block = queue.front();
						queue.pop_front();
						scheduled.erase(block);
						string_set state;
						for (const auto predecessor : predecessors_[block])
							state.insert(outgoing[predecessor].begin(),
										 outgoing[predecessor].end());
						incoming[block] = state;
						for (const auto* value : events_by_block_[block])
							transfer(state, *value);
						if (state.size() > 100000U)
						{
							converged = false;
							break;
						}
						if (state != outgoing[block])
						{
							outgoing[block] = std::move(state);
							for (const auto successor : successors_[block])
								if (scheduled.insert(successor).second)
									queue.push_back(successor);
						}
					}
					if (!converged)
						frontier("fixed-point-budget", function_id_);
					reaching_complete_ = converged;
					const auto reaching_guarantee = converged ? "may" : "unknown";
					for (const auto& [block, node] : nodes_)
					{
						(void)node;
						emit("reaching_in",
							 {block, 0},
							 function_id_,
							 {},
							 {},
							 encoded(incoming[block]),
							 reaching_guarantee,
							 &incoming[block]);
						emit("reaching_out",
							 {block, block_sizes_.at(block)},
							 function_id_,
							 {},
							 {},
							 encoded(outgoing[block]),
							 reaching_guarantee,
							 &outgoing[block]);
						auto state = incoming[block];
						for (const auto* value : events_by_block_[block])
						{
							emit("reaching_before",
								 value->where,
								 function_id_,
								 value->source,
								 {},
								 encoded(state),
								 reaching_guarantee,
								 &state,
								 nullptr,
								 {},
								 value->fact);
							if (value->definition)
								transfer(state, *value);
							else
							{
								string_set reaching;
								for (const auto& definition_id : state)
									if (definition_subjects_.at(definition_id) == value->subject)
										reaching.insert(definition_id);
								emit("def_use",
									 value->where,
									 value->subject,
									 value->source,
									 {},
									 encoded(reaching),
									 reaching_guarantee,
									 &reaching,
									 nullptr,
									 value->fact);
								if (reaching.empty())
									frontier("definition-frontier", value->subject);
							}
							emit("reaching_after",
								 value->where,
								 function_id_,
								 value->source,
								 {},
								 encoded(state),
								 reaching_guarantee,
								 &state,
								 nullptr,
								 {},
								 value->fact);
						}
					}
					queue.clear();
					scheduled.clear();
					steps = 0;
					converged = true;
					for (auto iterator = nodes_.rbegin(); iterator != nodes_.rend(); ++iterator)
					{
						queue.push_back(iterator->first);
						scheduled.insert(iterator->first);
					}
					while (!queue.empty())
					{
						if (++steps > 1000000U)
						{
							converged = false;
							break;
						}
						const auto block = queue.front();
						queue.pop_front();
						scheduled.erase(block);
						string_set after;
						for (const auto successor : successors_[block])
							after.insert(live_in[successor].begin(), live_in[successor].end());
						auto before = gen[block];
						for (const auto& variable : after)
							if (!kill[block].contains(variable))
								before.insert(variable);
						live_out[block] = std::move(after);
						if (before != live_in[block])
						{
							live_in[block] = std::move(before);
							for (const auto predecessor : predecessors_[block])
								if (scheduled.insert(predecessor).second)
									queue.push_back(predecessor);
						}
					}
					if (!converged)
						frontier("liveness-budget", function_id_);
					liveness_complete_ = converged;
					for (const auto& [block, node] : nodes_)
					{
						(void)node;
						emit("live_in",
							 {block, 0},
							 function_id_,
							 {},
							 {},
							 encoded(live_in[block]),
							 converged ? "may" : "unknown",
							 nullptr,
							 &live_in[block]);
						emit("live_out",
							 {block, block_sizes_.at(block)},
							 function_id_,
							 {},
							 {},
							 encoded(live_out[block]),
							 converged ? "may" : "unknown",
							 nullptr,
							 &live_out[block]);
						auto live = live_out[block];
						for (auto event = events_by_block_[block].rbegin();
							 event != events_by_block_[block].rend();
							 ++event)
						{
							const auto& value = **event;
							emit("live_after",
								 value.where,
								 function_id_,
								 value.source,
								 {},
								 encoded(live),
								 converged ? "may" : "unknown",
								 nullptr,
								 &live,
								 {},
								 value.fact);
							if (value.definition)
							{
								if (value.strong)
									live.erase(value.subject);
							}
							else
								live.insert(value.subject);
							emit("live_before",
								 value.where,
								 function_id_,
								 value.source,
								 {},
								 encoded(live),
								 converged ? "may" : "unknown",
								 nullptr,
								 &live,
								 {},
								 value.fact);
						}
					}
				}
				void observe_subject(const clang::NamedDecl& declaration)
				{
					const auto* value = llvm::dyn_cast<clang::ValueDecl>(&declaration);
					if (!value)
						return;
					const auto entity = owner_.entity(&declaration);
					if (entity.empty() || value->getType().isNull() ||
						value->getType()->isDependentType())
					{
						subjects_complete_ = false;
						return;
					}
					owner_.retain_population_bytes(entity.size() * 2U + 128U);
					if (value->getType()->isPointerType())
						pointer_subjects_.insert(entity);
					if (value->getType()->isIntegralOrEnumerationType())
						integer_subjects_.insert(entity);
				}
				void inventory()
				{
					auto value = owner_.common();
					value.emplace("function", id("cc_entity_id", function_id_));
					value.emplace("body", id("body_id", body_id_));
					value.emplace("source", id("source_span_id", body_source_));
					value.emplace("profile",
								  sdk::detached_cell::utf8("clang22-local-may-flow-facts/1"));
					const bool complete =
						unmapped_facts_ == 0U && admitted_facts_ == fact_ids_.size();
					value.emplace(
						"enumeration_state",
						symbol("cc.flow-inventory-state/1", complete ? "complete" : "partial"));
					value.emplace("fact_count",
								  sdk::detached_cell::unsigned_integer(admitted_facts_));
					value.emplace("facts", flags("flow_fact_id", fact_ids_));
					value.emplace("program_point_profile",
								  sdk::detached_cell::utf8(
									  "clang22-cfg-boundaries-and-written-local-events/1"));
					value.emplace("program_point_count",
								  sdk::detached_cell::unsigned_integer(nodes_.size() * 2U +
																	   events_.size() * 2U));
					value.emplace("reaching_points", flags("flow_fact_id", reaching_points_));
					value.emplace("liveness_points", flags("flow_fact_id", liveness_points_));
					value.emplace("reaching_state",
								  symbol("cc.flow-facet-state/1",
										 reaching_complete_ ? "complete" : "partial"));
					value.emplace("liveness_state",
								  symbol("cc.flow-facet-state/1",
										 liveness_complete_ ? "complete" : "partial"));
					value.emplace("pointer_subjects", flags("cc_entity_id", pointer_subjects_));
					value.emplace("integer_subjects", flags("cc_entity_id", integer_subjects_));
					value.emplace("subject_state",
								  symbol("cc.flow-facet-state/1",
										 subjects_complete_ ? "complete" : "partial"));
					value.emplace("points_to_state",
								  symbol("cc.flow-facet-state/1",
										 pointer_subjects_.empty() && subjects_complete_
											 ? "complete"
											 : "partial"));
					value.emplace("null_state",
								  symbol("cc.flow-facet-state/1",
										 pointer_subjects_.empty() && subjects_complete_
											 ? "complete"
											 : "partial"));
					value.emplace("range_state",
								  symbol("cc.flow-facet-state/1",
										 integer_subjects_.empty() && subjects_complete_
											 ? "complete"
											 : "partial"));
					value.emplace("summary_state", symbol("cc.flow-facet-state/1", "unavailable"));
					if (!complete)
						value.emplace("reason",
									  sdk::detached_cell::utf8(
										  "flow-subject-source-or-position-unavailable"));
					owner_.append(
						make_row(cc::relations::flow_inventory::descriptor(), std::move(value)));
				}
				collector& owner_;
				clang::FunctionDecl& function_;
				const clang::CFG& cfg_;
				const std::map<unsigned, std::string>& nodes_;
				std::string function_id_;
				std::string body_id_, body_source_;
				string_set fact_ids_, reaching_points_, liveness_points_, pointer_subjects_,
					integer_subjects_;
				std::uint64_t admitted_facts_{}, unmapped_facts_{};
				bool reaching_complete_{}, liveness_complete_{}, subjects_complete_{true};
				std::map<const clang::Stmt*, point> points_;
				std::vector<const clang::Stmt*> stack_;
				std::set<const clang::Expr*> writes_only_;
				std::vector<event> events_;
				std::map<unsigned, std::vector<const event*>> events_by_block_;
				std::map<std::string, std::string, std::less<>> definition_subjects_;
				std::map<unsigned, std::set<unsigned>> successors_, predecessors_;
				std::map<unsigned, unsigned> block_sizes_;
				std::set<std::pair<std::string, std::string>> frontiers_;
				std::uint64_t ordinal_{}, order_{};
			};
			void build_cfg(clang::FunctionDecl& function)
			{
				if (progress_)
					progress_("cfg/flow " + function.getQualifiedNameAsString());
				const auto context =
					"cxxlens: detaching CFG/flow for " + function.getQualifiedNameAsString();
				llvm::PrettyStackTraceString phase{context.c_str()};
				const auto function_id = entity(&function);
				const auto source = span(function.getBody()->getSourceRange(), "body");
				if (function_id.empty() || !source)
					return;
				const auto text = clang::Lexer::getSourceText(
					clang::CharSourceRange::getTokenRange(function.getBody()->getSourceRange()),
					unit_.source_manager(),
					unit_.ast().getLangOpts());
				const auto content =
					sdk::content_digest(std::as_bytes(std::span{text.data(), text.size()}));
				auto value = common();
				value.emplace("function", id("cc_entity_id", function_id));
				value.emplace("source", id("source_span_id", *source));
				value.emplace("analysis_profile",
							  symbol("cc.analysis-profile/1", "clang22-cfg-eh-lifetime-v1"));
				value.emplace("content",
							  sdk::detached_cell{{sdk::scalar_kind::digest, {}, false},
												 sdk::cell_state::present,
												 sdk::scalar_value{content},
												 std::nullopt});
				clang::CFG::BuildOptions options;
				options.PruneTriviallyFalseEdges = false;
				options.AddEHEdges = true;
				options.AddImplicitDtors = true;
				options.AddLifetime = true;
				options.AddLoopExit = true;
				options.AddTemporaryDtors = true;
				options.AddScopes = true;
				options.AddCXXNewAllocator = true;
				options.AddInitializers = true;
				options.AddRichCXXConstructors = true;
				std::unique_ptr<clang::CFG> cfg;
				if (!function.isDependentContext())
					cfg =
						clang::CFG::buildCFG(&function, function.getBody(), &unit_.ast(), options);
				const auto eligibility = cfg		? "closed"
					: function.isDependentContext() ? "dependent"
													: "unavailable";
				value.emplace("eligibility", symbol("cc.body-eligibility/1", eligibility));
				if (!cfg)
					value.emplace("reason",
								  sdk::detached_cell::utf8(function.isDependentContext()
															   ? "instantiate-required"
															   : "cfg-construction-failed"));
				std::uint64_t edges{};
				if (cfg)
					for (const auto* block : *cfg)
						edges += block->succ_size();
				value.emplace("node_count",
							  sdk::detached_cell::unsigned_integer(cfg ? cfg->size() : 0U));
				value.emplace("edge_count", sdk::detached_cell::unsigned_integer(edges));
				auto body = make_row(cc::relations::body::descriptor(), value);
				const auto body_id = row_id(body, "body");
				if (!cfg)
				{
					pending_bodies_.insert_or_assign(&function, std::move(value));
					return;
				}
				std::map<unsigned, std::string> nodes;
				for (const auto* block : *cfg)
				{
					auto node = common();
					node.emplace("function", id("cc_entity_id", function_id));
					node.emplace("body", id("body_id", body_id));
					node.emplace("ordinal",
								 sdk::detached_cell::unsigned_integer(block->getBlockID()));
					node.emplace("kind",
								 symbol("cc.cfg-node-kind/1",
										block == &cfg->getEntry()	   ? "entry"
											: block == &cfg->getExit() ? "exit"
																	   : "block"));
					node.emplace("element_count",
								 sdk::detached_cell::unsigned_integer(block->size()));
					node.emplace(
						"statement_count",
						sdk::detached_cell::unsigned_integer(
							static_cast<std::uint64_t>(std::ranges::count_if(
								*block,
								[](const auto& element)
								{
									return element.template getAs<clang::CFGStmt>().has_value();
								}))));
					if (const auto* terminator = block->getTerminatorStmt())
						if (auto where = span(terminator->getSourceRange(), "statement"))
							node.emplace("source", id("source_span_id", *where));
					auto row = make_row(cc::relations::cfg_node::descriptor(), std::move(node));
					nodes.emplace(block->getBlockID(), row_id(row, "node"));
					append(std::move(row));
				}
				value.emplace("entry", id("cfg_node_id", nodes.at(cfg->getEntry().getBlockID())));
				value.emplace("exit", id("cfg_node_id", nodes.at(cfg->getExit().getBlockID())));
				pending_bodies_.insert_or_assign(&function, std::move(value));
				for (const auto* block : *cfg)
				{
					std::uint64_t ordinal{};
					for (const auto& successor : block->succs())
					{
						auto branch = common();
						branch.emplace("function", id("cc_entity_id", function_id));
						branch.emplace("from", id("cfg_node_id", nodes.at(block->getBlockID())));
						const auto* target = successor.getReachableBlock();
						if (!target)
							target = successor.getPossiblyUnreachableBlock();
						if (target)
							branch.emplace("to", id("cfg_node_id", nodes.at(target->getBlockID())));
						std::string kind = "normal";
						const auto* terminator = block->getTerminatorStmt();
						if (!target)
							kind = "frontier";
						else if (block->getTerminator().isValid() &&
								 block->getTerminator().getKind() ==
									 clang::CFGTerminator::TemporaryDtorsBranch)
							kind = "cleanup";
						else if (block->getTerminator().isValid() &&
								 block->getTerminator().getKind() ==
									 clang::CFGTerminator::VirtualBaseBranch)
							kind = "virtual_base";
						else if (llvm::isa_and_nonnull<clang::SwitchStmt>(terminator))
							kind = llvm::isa_and_nonnull<clang::DefaultStmt>(target->getLabel())
								? "default"
								: "case";
						else if (llvm::isa_and_nonnull<clang::GotoStmt, clang::IndirectGotoStmt>(
									 terminator))
							kind = "goto";
						else if (llvm::isa_and_nonnull<clang::BreakStmt>(terminator))
							kind = "break";
						else if (llvm::isa_and_nonnull<clang::ContinueStmt>(terminator))
							kind = "continue";
						else if (llvm::isa_and_nonnull<clang::CXXTryStmt>(terminator))
							kind = "exception_dispatch";
						else if (llvm::isa_and_nonnull<clang::CXXThrowExpr>(terminator))
							kind = "exception";
						else if (block->getTerminatorCondition() != nullptr &&
								 block->succ_size() == 2U)
							kind = ordinal == 0U ? "true" : "false";
						branch.emplace("kind", symbol("cc.cfg-edge-kind/1", std::move(kind)));
						branch.emplace("ordinal", sdk::detached_cell::unsigned_integer(ordinal++));
						append(make_row(cc::relations::cfg_edge::descriptor(), std::move(branch)));
					}
				}
				local_flow{*this, function, *cfg, nodes, body_id, *source}.run();
				if (progress_)
					progress_("cfg/flow finished " + function.getQualifiedNameAsString());
			}
			provider::clang22::borrowed_translation_unit& unit_;
			const source_closure_snapshot& closure_;
			const provider_worker_v4_ast_observation_batch& observations_;
			std::map<std::string, std::string, std::less<>> entities_;
			mutable std::map<const clang::Decl*, std::string> entity_lookup_;
			std::map<std::pair<std::string, std::uint64_t>, std::string> anchors_;
			project_semantic_facts output_;
			std::map<std::string, sdk::detached_row, std::less<>> rows_;
			const std::function<void(std::string_view)>& progress_;
			project_abi_observer abi_observer_;
			const project_original_calls& original_calls_;
			std::map<std::string, sdk::detached_row, std::less<>> original_sites_,
				original_targets_;
			std::map<const clang::FunctionDecl*, fields> pending_function_details_;
			std::map<const clang::Stmt*, std::set<std::string, std::less<>>> syntax_nodes_;
			std::map<const clang::Expr*, std::string> call_evaluation_;
			struct type_structure
			{
				std::string digest, state;
			};
			std::map<std::string, type_structure, std::less<>> type_structure_;
			std::size_t call_bytes_{};
			std::string current_function_;
			const clang::Stmt* inherited_default_{};
			std::vector<ast_enumeration> ast_enumerations_;
			std::map<const clang::FunctionDecl*, fields> pending_bodies_;
			std::vector<std::string> parents_;
			std::vector<clang::Stmt*> statements_;
			std::size_t category_work_{};
			std::size_t unevaluated_depth_{};
			std::size_t depth_{};
			std::size_t retained_bytes_{};
			std::size_t record_inventory_bytes_{};
			std::size_t population_bytes_{};
			std::set<const clang::NamedDecl*> observed_declarations_;
			std::set<std::string, std::less<>> declaration_ids_, system_declaration_ids_,
				parsed_files_, parsed_snapshots_;
			struct include_census
			{
				std::set<std::string, std::less<>> admissions, ids;
				bool resolution_complete{true};
			};
			std::map<std::string, include_census, std::less<>> include_populations_;
			std::size_t direct_call_bytes_{};
			std::set<const clang::Decl*> observed_record_definitions_;
			std::set<std::string, std::less<>> record_definition_surfaces_,
				system_record_definition_surfaces_;
			std::uint64_t syntax_ordinal_{};
			std::map<const void*, std::string> types_;
			std::size_t type_depth_{};
			std::map<std::string, std::vector<std::pair<std::uint64_t, std::uint64_t>>, std::less<>>
				template_ranges_;
			std::set<std::string, std::less<>> template_incomplete_;
		};
#endif
	} // namespace

	void install_project_preprocessor_observer(clang::Preprocessor& preprocessor,
											   const source_closure_snapshot& closure,
											   project_preprocessor_observations& output)
	{
#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
		preprocessor.addPPCallbacks(
			std::make_unique<project_pp_callbacks>(preprocessor, closure, output));
		preprocessor.setTokenWatcher(
			[&preprocessor, &closure, &output](const clang::Token& token)
			{
				if (token.isAnnotation() || token.is(clang::tok::eof) ||
					token.is(clang::tok::eod) || token.is(clang::tok::comment) ||
					output.tokens_truncated)
					return;
				auto& manager = preprocessor.getSourceManager();
				if (token.getLocation().isInvalid())
				{
					output.tokens_unmapped = true;
					return;
				}
				const auto range = manager.getExpansionRange(token.getLocation());
				const auto begin = manager.getExpansionLoc(range.getBegin());
				const auto logical = project_path(manager.getFilename(begin));
				const auto* member = closure.find_member(logical);
				if (!member)
					return;
				auto end = manager.getExpansionLoc(range.getEnd());
				if (range.isTokenRange())
					end = clang::Lexer::getLocForEndOfToken(
						end, 0U, manager, preprocessor.getLangOpts());
				if (end.isInvalid() || !manager.isWrittenInSameFile(begin, end) ||
					manager.getFileOffset(end) > member->size_bytes)
				{
					output.tokens_unmapped = true;
					return;
				}
				constexpr auto maximum_bytes = 64U * 1024U * 1024U;
				if (output.expanded_tokens.size() >= 250000U ||
					token.getLength() > maximum_bytes - output.token_bytes)
				{
					output.tokens_truncated = true;
					return;
				}
				bool invalid{};
				auto spelling = preprocessor.getSpelling(token, &invalid);
				if (invalid)
				{
					output.tokens_unmapped = true;
					return;
				}
				const std::string kind = clang::tok::getTokenName(token.getKind());
				const auto bytes = logical.size() + kind.size() + spelling.size();
				if (bytes > maximum_bytes - output.token_bytes)
				{
					output.tokens_truncated = true;
					return;
				}
				output.token_bytes += bytes;
				output.expanded_tokens.push_back({logical,
												  kind,
												  std::move(spelling),
												  manager.getFileOffset(begin),
												  manager.getFileOffset(end),
												  token.getLocation().isMacroID()});
			});
#else
		(void)preprocessor;
		(void)closure;
		(void)output;
#endif
	}

	sdk::result<project_semantic_facts>
	observe_project_semantics(provider::clang22::borrowed_translation_unit& unit,
							  const source_closure_snapshot& closure,
							  const provider_worker_v4_ast_observation_batch& observations,
							  const provider_worker_v4_normalized_output& normalized,
							  const project_preprocessor_observations& preprocessing,
							  const std::function<void(std::string_view)>& progress,
							  const project_original_calls& original_calls)
	{
#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
		try
		{
			llvm::PrettyStackTraceString phase{"cxxlens: detaching project C++ facts"};
			static std::once_flag diagnostics;
			std::call_once(diagnostics,
						   []
						   {
							   llvm::EnablePrettyStackTrace();
							   llvm::sys::PrintStackTraceOnErrorSignal("cxxlens-clang22-analyze");
						   });
			check(closure.validate());
			check(observations.validate());
			check(normalized.validate());
			collector visitor{unit, closure, observations, normalized, progress, original_calls};
			if (progress)
				progress("detaching preprocessor facts");
			visitor.preprocess(preprocessing);
			if (progress)
				progress("detaching declaration, syntax, CFG and flow facts");
			if (!visitor.TraverseDecl(unit.ast().getTranslationUnitDecl()))
				fail("traversal", "stopped");
			if (progress)
				progress("detaching raw and expanded source tokens");
			visitor.tokens(preprocessing);
			return visitor.finish();
		}
		catch (const extraction_failure& error)
		{
			return sdk::unexpected(error.value);
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				sdk::error{"application-analysis.resource-exhausted", "cpp-facts", "allocation"});
		}
#else
		(void)unit;
		(void)closure;
		(void)observations;
		(void)normalized;
		(void)preprocessing;
		(void)progress;
		(void)original_calls;
		return sdk::unexpected(
			sdk::error{"native.unsupported-clang-major", "cpp-facts", "clang-major-22"});
#endif
	}
} // namespace cxxlens::detail::clang22
