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
#include <cxxlens/relations/cc_operation.hpp>
#include <cxxlens/relations/cc_record_inventory.hpp>
#include <cxxlens/relations/cc_record_surface.hpp>
#include <cxxlens/relations/cc_syntax_node.hpp>
#include <cxxlens/relations/cc_target_resolution_slot.hpp>
#include <cxxlens/relations/cc_template_inventory.hpp>
#include <cxxlens/relations/cc_type.hpp>
#include <cxxlens/relations/cc_type_component.hpp>
#include <cxxlens/relations/source_comment.hpp>
#include <cxxlens/relations/source_comment_inventory.hpp>
#include <cxxlens/relations/source_include.hpp>
#include <cxxlens/relations/source_include_inventory.hpp>
#include <cxxlens/relations/source_preprocessor_event.hpp>
#include <cxxlens/relations/source_preprocessor_inventory.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <cxxlens/relations/source_token.hpp>
#include <cxxlens/relations/source_token_inventory.hpp>

#include "project_abi_observer.hpp"
#include "project_template_event_rows.hpp"
#include "project_template_observer.hpp"
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
#include <clang/Basic/TargetInfo.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/MacroArgs.h>
#include <clang/Lex/MacroInfo.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/PreprocessingRecord.h>
#include <clang/Lex/Preprocessor.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>

#include "original_builtin_kind.hpp"
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
		sdk::detached_cell id(std::string type, std::string value, bool optional = false)
		{
			auto result = sdk::detached_cell::typed(std::move(type), std::move(value));
			result.type.optional = optional;
			return result;
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
				const auto event = add({token.getLocation(), macro->getDefinitionEndLoc()},
									   "macro_definition",
									   name(token),
									   text({token.getLocation(), macro->getDefinitionEndLoc()}),
									   "observed");
				if (event)
					macro_facets(*event, *macro);
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
							  const clang::MacroDefinition& definition,
							  clang::SourceRange range,
							  const clang::MacroArgs* arguments) override
			{
				const auto* macro = definition.getMacroInfo();
				observe_candidate(token, range);
				const auto event =
					add(range, "macro_expansion", name(token), text(range), "observed");
				if (!event || !macro)
					return;
				macro_facets(*event, *macro);
				auto& manager = pp_.getSourceManager();
				auto& observed = output_.events[*event];
				observed.parent_location = token.getLocation().isMacroID()
					? manager.getImmediateMacroCallerLoc(token.getLocation()).getRawEncoding()
					: 0U;
				observed.expansion_context_complete = !token.getLocation().isMacroID();
				if (!arguments)
					return;
				for (unsigned index{}; index < arguments->getNumMacroArguments(); ++index)
				{
					const auto* argument = arguments->getUnexpArgument(index);
					unsigned count{};
					while (!argument[count].is(clang::tok::eof))
					{
						if (count >= 1000000U)
						{
							output_.truncated = true;
							return;
						}
						++count;
					}
					const auto child =
						add(count ? clang::SourceRange{argument[0].getLocation(),
													   argument[count - 1U].getLocation()}
								  : range,
							"macro_argument",
							{},
							{},
							"observed");
					if (!child)
						continue;
					auto& value = output_.events[*child];
					value.parent_observation = *event;
					value.argument_index = index;
					value.parameter_index = index;
					value.argument_empty = count == 0U;
					if (index < macro->getNumParams())
						value.parameter_name = macro->params()[index]->getName().str();
					std::uint64_t uses{}, evaluating{};
					const auto replacement = macro->tokens();
					if (replacement.size() >
						64000000U / std::max<unsigned>(1U, arguments->getNumMacroArguments()))
					{
						output_.truncated = true;
						return;
					}
					for (std::size_t r{}; r < replacement.size(); ++r)
					{
						if (!replacement[r].getIdentifierInfo() || index >= macro->getNumParams() ||
							replacement[r].getIdentifierInfo() != macro->params()[index])
							continue;
						++uses;
						if (!(r &&
							  (replacement[r - 1U].is(clang::tok::hash) ||
							   replacement[r - 1U].is(clang::tok::hashhash))) &&
							!(r + 1U < replacement.size() &&
							  replacement[r + 1U].is(clang::tok::hashhash)))
							++evaluating;
					}
					value.substitution_count = uses;
					if (evaluating == 0U || count == 0U)
						value.evaluating_substitution_count = 0U;
					if (count * sizeof(std::uint32_t) >
						64U * 1024U * 1024U - output_.retained_bytes)
					{
						output_.truncated = true;
						return;
					}
					output_.retained_bytes += count * sizeof(std::uint32_t);
					value.argument_locations.reserve(count);
					for (unsigned t{}; t < count; ++t)
						value.argument_locations.push_back(
							argument[t].getLocation().getRawEncoding());
					for (unsigned t{}; t < count; ++t)
						observe_candidate(argument[t],
										  {argument[t].getLocation(), argument[t].getLocation()});
				}
			}
			void Defined(const clang::Token& token,
						 const clang::MacroDefinition& definition,
						 clang::SourceRange range) override
			{
				nonexpanding_defined_operands_.insert(token.getLocation().getRawEncoding());
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
				const auto event = add({location, token.getLocation()},
									   "ifdef",
									   name(token),
									   {},
									   definition.getMacroInfo() ? "true" : "false");
				if (event)
					record_condition(*event, definition.getMacroInfo() != nullptr, false, true);
			}
			void Ifndef(clang::SourceLocation location,
						const clang::Token& token,
						const clang::MacroDefinition& definition) override
			{
				const auto event = add({location, token.getLocation()},
									   "ifndef",
									   name(token),
									   {},
									   definition.getMacroInfo() ? "false" : "true");
				if (event)
					record_condition(*event, definition.getMacroInfo() == nullptr, false, true);
			}
			void Elifdef(clang::SourceLocation location,
						 const clang::Token& token,
						 const clang::MacroDefinition& definition) override
			{
				const auto event = add({location, token.getLocation()},
									   "elifdef",
									   name(token),
									   {},
									   definition.getMacroInfo() ? "true" : "false");
				if (event)
					record_condition(*event, definition.getMacroInfo() != nullptr, false, false);
			}
			void Elifdef(clang::SourceLocation location,
						 clang::SourceRange range,
						 clang::SourceLocation) override
			{
				const auto event =
					add({location, range.getEnd()}, "elifdef", text(range), {}, "not_evaluated");
				if (event)
					record_condition(*event, false, false, false);
			}
			void Elifndef(clang::SourceLocation location,
						  const clang::Token& token,
						  const clang::MacroDefinition& definition) override
			{
				const auto event = add({location, token.getLocation()},
									   "elifndef",
									   name(token),
									   {},
									   definition.getMacroInfo() ? "false" : "true");
				if (event)
					record_condition(*event, definition.getMacroInfo() == nullptr, false, false);
			}
			void Elifndef(clang::SourceLocation location,
						  clang::SourceRange range,
						  clang::SourceLocation) override
			{
				const auto event =
					add({location, range.getEnd()}, "elifndef", text(range), {}, "not_evaluated");
				if (event)
					record_condition(*event, false, false, false);
			}
			void Else(clang::SourceLocation location, clang::SourceLocation) override
			{
				const auto event = add({location, location}, "else", {}, {}, "observed");
				if (event)
					record_condition(*event, false, true, false);
			}
			void Endif(clang::SourceLocation location, clang::SourceLocation) override
			{
				const auto event = add({location, location}, "endif", {}, {}, "observed");
				if (!event)
					return;
				const auto file = output_.events[*event].logical_path;
				auto& stack = conditions_[file];
				if (stack.empty())
				{
					output_.classification_incomplete.insert(file);
					return;
				}
				const auto frame = stack.back();
				stack.pop_back();
				output_.events[*event].parent_observation = frame.opening;
				output_.events[*event].active = frame.parent_active;
				for (const auto branch : frame.branches)
					output_.events[branch].closing_observation = *event;
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

			void observe_candidate(const clang::Token& token, clang::SourceRange range)
			{
				if (output_.truncated)
					return;
				auto semantic_token = token;
				if (semantic_token.is(clang::tok::raw_identifier))
					pp_.LookUpIdentifierInfo(semantic_token);
				auto* identifier = semantic_token.getIdentifierInfo();
				if (!identifier)
					return;
				const auto* macro = pp_.getMacroDefinition(identifier).getMacroInfo();
				if (!macro ||
					nonexpanding_defined_operands_.contains(token.getLocation().getRawEncoding()))
					return;
				if (output_.retained_bytes > 64U * 1024U * 1024U - 64U)
				{
					output_.truncated = true;
					return;
				}
				output_.retained_bytes += 64U;
				if (!candidate_tokens_.insert(token.getLocation().getRawEncoding()).second)
					return;
				const auto event = add(range, "macro_candidate", name(token), {}, "observed");
				if (event)
					macro_facets(*event, *macro);
			}

		  private:
			struct condition_frame
			{
				std::size_t opening, branch;
				bool parent_active, branch_active, taken;
				std::vector<std::size_t> branches;
			};
			std::map<std::string, std::vector<condition_frame>, std::less<>> conditions_;
			std::set<std::uint32_t> candidate_tokens_, nonexpanding_defined_operands_;
			void record_condition(std::size_t index, bool selected, bool otherwise, bool opening)
			{
				auto& event = output_.events[index];
				auto& stack = conditions_[event.logical_path];
				if (opening)
				{
					if (stack.size() >= 4096U)
					{
						output_.truncated = true;
						return;
					}
					const bool parent = stack.empty() || stack.back().branch_active;
					event.active = parent && selected;
					if (!stack.empty())
						event.parent_observation = stack.back().branch;
					output_.retained_bytes += sizeof(condition_frame) + sizeof(std::size_t);
					if (output_.retained_bytes > 64U * 1024U * 1024U)
					{
						output_.truncated = true;
						return;
					}
					stack.push_back({index, index, parent, *event.active, *event.active, {index}});
				}
				else
				{
					if (stack.empty())
					{
						output_.classification_incomplete.insert(event.logical_path);
						event.active.reset();
						return;
					}
					auto& frame = stack.back();
					event.active = frame.parent_active && (otherwise ? !frame.taken : selected);
					event.parent_observation = frame.opening;
					frame.branch = index;
					frame.branch_active = *event.active;
					frame.taken |= *event.active;
					output_.retained_bytes += sizeof(std::size_t);
					if (output_.retained_bytes > 64U * 1024U * 1024U)
					{
						output_.truncated = true;
						return;
					}
					frame.branches.push_back(index);
				}
			}

			void macro_facets(std::size_t event, const clang::MacroInfo& macro)
			{
				auto& value = output_.events[event];
				value.function_like = macro.isFunctionLike();
				value.variadic = macro.isVariadic();
				value.parameter_count = macro.getNumParams();
				value.replacement_count = macro.getNumTokens();
				value.definition_location = macro.getDefinitionLoc().getRawEncoding();
				value.macro_complete = true;
			}
			std::string name(const clang::Token& token) const
			{
				if (token.is(clang::tok::raw_identifier))
					return token.getRawIdentifier().str();
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
				auto& manager = pp_.getSourceManager();
				auto cursor = manager.getExpansionLoc(range.getBegin());
				const auto end = manager.getExpansionLoc(range.getEnd());
				if (cursor.isValid() && end.isValid() && manager.isWrittenInSameFile(cursor, end))
				{
					std::size_t steps{};
					while (manager.getFileOffset(cursor) <= manager.getFileOffset(end))
					{
						if (++steps > 1000000U)
						{
							output_.truncated = true;
							break;
						}
						clang::Token original;
						if (clang::Lexer::getRawToken(
								cursor, original, manager, pp_.getLangOpts(), true) ||
							original.is(clang::tok::eof) || original.is(clang::tok::eod))
							break;
						if (original.is(clang::tok::raw_identifier))
						{
							pp_.LookUpIdentifierInfo(original);
							observe_candidate(original,
											  {original.getLocation(), original.getLocation()});
						}
						const auto next = clang::Lexer::getLocForEndOfToken(
							original.getLocation(), 0, manager, pp_.getLangOpts());
						if (next.isInvalid() ||
							manager.getFileOffset(next) <= manager.getFileOffset(cursor))
							break;
						cursor = next;
					}
				}
				const auto event = add({location, range.getEnd()},
									   std::move(kind),
									   {},
									   text(range),
									   value == CVK_True		? "true"
										   : value == CVK_False ? "false"
																: "not_evaluated");
				if (event)
					record_condition(
						*event, value == CVK_True, false, output_.events[*event].kind == "if");
			}
			std::optional<std::size_t> add(clang::SourceRange range,
										   std::string kind,
										   std::string name_value,
										   std::string value,
										   std::string state)
			{
				if (output_.truncated)
					return std::nullopt;
				auto& manager = pp_.getSourceManager();
				const auto first = manager.getExpansionLoc(range.getBegin());
				const auto logical = project_path(manager.getFilename(first));
				const auto* member = closure_.find_member(logical);
				if (!member)
					return std::nullopt;
				++output_.event_admissions[logical];
				const auto last = clang::Lexer::getLocForEndOfToken(
					manager.getExpansionLoc(range.getEnd()), 0, manager, pp_.getLangOpts());
				if (last.isInvalid() || !manager.isWrittenInSameFile(first, last))
				{
					output_.classification_incomplete.insert(logical);
					return std::nullopt;
				}
				const auto begin = manager.getFileOffset(first), end = manager.getFileOffset(last);
				if (end < begin || end > member->size_bytes)
				{
					output_.classification_incomplete.insert(logical);
					return std::nullopt;
				}
				const auto bytes = sizeof(project_preprocessor_event) + logical.size() +
					kind.size() + name_value.size() + value.size() + state.size();
				if (output_.events.size() >= 1000000U ||
					bytes > 64U * 1024U * 1024U - output_.retained_bytes)
				{
					output_.truncated = true;
					return std::nullopt;
				}
				output_.retained_bytes += bytes;
				const auto index = output_.events.size();
				project_preprocessor_event observed;
				observed.logical_path = logical;
				observed.begin = begin;
				observed.end = end;
				observed.kind = std::move(kind);
				observed.name = std::move(name_value);
				observed.value = std::move(value);
				observed.state = std::move(state);
				observed.location = range.getBegin().getRawEncoding();
				const auto context = conditions_.find(logical);
				observed.active = context == conditions_.end() || context->second.empty()
					? true
					: context->second.back().branch_active;
				if (context != conditions_.end())
				{
					observed.context_depth = context->second.size();
					if (!context->second.empty())
						observed.parent_observation = context->second.back().branch;
				}
				output_.events.push_back(std::move(observed));
				return index;
			}
			clang::Preprocessor& pp_;
			const source_closure_snapshot& closure_;
			project_preprocessor_observations& output_;
		};

		class collector final : public clang::RecursiveASTVisitor<collector>
		{
			using base = clang::RecursiveASTVisitor<collector>;
			struct raw_pp_token
			{
				std::string id, spelling;
				clang::tok::TokenKind kind;
				std::uint64_t begin{}, end{};
				bool identifier{}, leading_space{};
			};
			struct pp_pending
			{
				std::string file;
				fields value;
			};
			struct raw_conditional
			{
				std::size_t opening{}, branch{};
				std::vector<std::size_t> branches;
				bool saw_else{};
			};
			struct operation_observation
			{
				const clang::Expr* expression{};
				const clang::ValueDecl* object{};
				const clang::Expr* object_expression{};
				const clang::FunctionDecl* target{};
				const clang::CXXBaseSpecifier* base{};
				clang::QualType type;
				clang::SourceRange source;
				std::string kind, origin, evaluation, outcome{"ordinary"}, type_use_kind;
				std::string node, body, element_kind;
				std::optional<std::uint64_t> element_index;
				bool implicit_cfg{};
				std::optional<std::uint64_t> subject_index;
				std::optional<bool> parameter_has_default_argument;
			};
			struct ast_enumeration
			{
				const clang::Stmt* body{};
				const clang::FunctionDecl* declaration{};
				std::string function;
				std::vector<operation_observation> operations;
				std::set<std::string, std::less<>> operation_ids;
				bool operation_frontier{};
				std::uint64_t nodes{}, unmapped{}, local_variables{};
				std::uint64_t unmapped_members{};
				std::set<std::pair<std::string, std::string>> member_accesses;
				std::uint64_t unmapped_calls{};
				std::set<std::string, std::less<>> direct_call_sites, direct_call_targets;
				std::set<const clang::Expr*> call_admissions;
				std::set<std::string, std::less<>> call_sites;
				bool call_frontier{};
				std::set<const clang::Stmt*> literal_admissions;
				std::set<std::string, std::less<>> literal_ids;
				bool literal_frontier{};
				std::uint64_t automatic_storage_count{};
				std::set<std::string, std::less<>> automatic_storage_ids;
				bool automatic_storage_frontier{};
				bool active{}, written_initializer{};
			};

		  public:
			collector(provider::clang22::borrowed_translation_unit& unit,
					  const source_closure_snapshot& closure,
					  const provider_worker_v4_ast_observation_batch& observations,
					  const provider_worker_v4_normalized_output& normalized,
					  const std::function<void(std::string_view)>& progress,
					  const project_original_calls& original_calls,
					  const std::string& project_id,
					  const project_template_observations* templates,
					  const project_template_event_observations* template_events)
				: unit_{unit}, closure_{closure}, observations_{observations}, progress_{progress},
				  abi_observer_{unit.ast(), unit.preprocessor(), unit.code_generation_options()},
				  original_calls_{original_calls}, project_id_{project_id}, templates_{templates},
				  template_events_{template_events}
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
					enumeration.declaration = function;
					enumeration.body = function->doesThisDeclarationHaveABody() &&
							!function->isDefaulted() && !function->isDeleted()
						? function->getBody()
						: nullptr;
					if (function->isDefaulted())
						inherited_default_ = function->getBody();
					enumeration.function = current_function_;
					ast_enumerations_.push_back(std::move(enumeration));
				}
				if (++depth_ > 4096U)
					fail("traversal", "depth-limit");
				retain_population_bytes(32U);
				declaration_stack_.push_back(declaration);
				const auto result = base::TraverseDecl(declaration);
				declaration_stack_.pop_back();
				--depth_;
				if (function != nullptr)
				{
					finish_operations(ast_enumerations_.back());
					finish_literal_scope(ast_enumerations_.back());
					const auto enumeration = std::move(ast_enumerations_.back());
					ast_enumerations_.pop_back();
					if (const auto detail = pending_function_details_.find(function);
						detail != pending_function_details_.end())
					{
						call_scope_fields(detail->second, enumeration);
						operation_scope_fields(detail->second, enumeration);
						literal_scope_fields(detail->second, enumeration);
						automatic_storage_scope_fields(detail->second, enumeration);
						append(make_row(cc::relations::entity_detail::descriptor(),
										std::move(detail->second)));
						pending_function_details_.erase(detail);
					}
					if (const auto found = pending_bodies_.find(function);
						found != pending_bodies_.end())
					{
						if (declaration_population_admitted(function) &&
							(!function->isImplicit() || written_lambda(function)))
						{
							call_scope_fields(found->second, enumeration);
							operation_scope_fields(found->second, enumeration);
							literal_scope_fields(found->second, enumeration);
							automatic_storage_scope_fields(found->second, enumeration);
						}
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
				if (const auto* expression = llvm::dyn_cast<clang::Expr>(statement);
					expression && !ast_operation_kinds(statement).empty())
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
				observe_ast_operations(statement);
				if (magic_literal(statement) && !ast_enumerations_.empty() &&
					(ast_enumerations_.back().active ||
					 ast_enumerations_.back().written_initializer) &&
					!ast_enumerations_.back().literal_admissions.contains(statement))
				{
					retain_population_bytes(64U);
					ast_enumerations_.back().literal_admissions.insert(statement);
				}
				if (llvm::isa<clang::DependentScopeDeclRefExpr,
							  clang::CXXDependentScopeMemberExpr,
							  clang::UnresolvedMemberExpr,
							  clang::UnresolvedLookupExpr>(statement))
					target_slot(statement,
								statement->getStmtClassName(),
								statement->getSourceRange(),
								"state_access",
								"accesses",
								0U,
								nullptr,
								"unknown",
								"dependent-reference-kind-and-target",
								nullptr,
								statement);
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
					properties.emplace("finite_operation_admission_v1");
					for (const auto& kind : ast_operation_kinds(statement))
						properties.emplace("operation_" + kind);
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
						{
							value.emplace("canonical_type",
										  id("cc_type_id", canonical_type(expression->getType())));
							value.emplace(
								"type",
								sdk::detached_cell::utf8(
									expression->getType().getCanonicalType().getAsString()));
						}
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
						value.emplace("cast_kind",
									  sdk::detached_cell::utf8(cast->getCastKindName()));
						if (cast->getCastKind() == clang::CK_NullToPointer)
							value.emplace("constant_null", sdk::detached_cell::boolean(true));
						if (!cast->getSubExpr()->getType().isNull())
							value.emplace(
								"operand_type",
								id("cc_type_id", canonical_type(cast->getSubExpr()->getType())));
						value.emplace("value_preservation",
									  symbol("cc.conversion-value-preservation/1",
											 conversion_preservation(*cast)));
						value.emplace("conversion_state",
									  symbol("cc.flow-binding-state/1", "unknown"));
						value.emplace(
							"conversion_profile",
							sdk::detached_cell::utf8("clang22-original-conversion-semantics/1"));
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
					literal_fields(value, *statement);
					auto row = make_row(cc::relations::syntax_node::descriptor(), std::move(value));
					parent = row_id(row, "node");
					retain_call_bytes(parent.size() + 192U);
					syntax_nodes_[statement].insert(parent);
					if (const auto* expression = llvm::dyn_cast<clang::Expr>(statement))
						observe_macro_argument_expression(*expression, parent, parent_statement);
					retain_population_bytes(row.canonical_form().size() + 128U);
					pending_syntax_bindings_.push_back({std::move(row), statement});
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
				declaration_target_slots(*declaration);
				if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(declaration);
					method && method->isVirtual() &&
					!virtual_methods_.contains(method->getCanonicalDecl()))
				{
					retain_population_bytes(64U);
					virtual_methods_.insert(method->getCanonicalDecl());
				}
				const auto entity_id = entity(declaration);
				const auto source = span(declaration->getSourceRange(), "declaration");
				// Parameter slots are compiler subjects even when they have no written name
				// or normalized entity. Keep their original type/source/index in the census.
				observe_declaration_type_use(*declaration);
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
				declaration_type_use_flags(*declaration, properties);
				if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration))
				{
					properties.emplace("finite_variable_storage_v1");
					properties.emplace(llvm::isa<clang::ParmVarDecl>(variable) ? "storage_parameter"
										   : variable->getTLSKind() != clang::VarDecl::TLS_None
										   ? "storage_thread"
										   : variable->hasLocalStorage() ? "storage_automatic"
																		 : "storage_static");
					if (variable->getType()->isReferenceType())
						properties.emplace("storage_non_object");
				}
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
					if (function->isMain())
						properties.emplace("main_entry");
					if (!unit_.ast().getLangOpts().Freestanding)
						properties.emplace("hosted_environment");
					value.emplace(
						"capture_count",
						sdk::detached_cell::unsigned_integer(function_capture_count(*function)));
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
					properties.emplace("finite_variable_storage_v1");
					if (variable->getType().isVolatileQualified())
						properties.emplace("volatile_qualified");
					if (variable->getDeclContext()->isFileContext())
						properties.emplace("file_context_storage");
					switch (variable->getStorageDuration())
					{
						case clang::SD_Automatic:
							properties.emplace("automatic_storage");
							break;
						case clang::SD_Thread:
							properties.emplace("thread_storage");
							break;
						case clang::SD_Static:
							properties.emplace("static_storage");
							break;
						case clang::SD_FullExpression:
							properties.emplace("full_expression_storage");
							break;
						case clang::SD_Dynamic:
							properties.emplace("dynamic_storage");
							break;
					}
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
				identifier_fields(declaration_row, *declaration);
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
				retain_operation_bytes(row_id(original_declaration, "declaration").size() + 96U);
				original_declarations_.emplace(declaration,
											   row_id(original_declaration, "declaration"));
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
					abi_surface(
						entity_id, *source, "record", take(abi_observer_.record(*record)), record);
				}
				else if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(declaration))
					abi_surface(
						entity_id, *source, "function", take(abi_observer_.function(*function)));
				return true;
			}
			bool VisitFunctionDecl(clang::FunctionDecl* function)
			{
				if (function == nullptr || (function->isImplicit() && !written_lambda(function)) ||
					!function->isThisDeclarationADefinition() ||
					!function->doesThisDeclarationHaveABody() || function->isDefaulted() ||
					function->isDeleted() || !admitted(function->getLocation()))
					return true;
				build_cfg(*function);
				return true;
			}
			bool VisitCXXConstructExpr(clang::CXXConstructExpr* expression)
			{
				if (expression && admitted(expression->getExprLoc()))
				{
					target_slot(expression,
								expression->getStmtClassName(),
								expression->getSourceRange(),
								"callable",
								"constructs",
								0U,
								expression->getConstructor(),
								"eligible",
								{},
								nullptr,
								expression);
					if (expression->getParenOrBraceRange().isValid())
						admit_call(expression);
				}
				return true;
			}
			bool VisitCallExpr(clang::CallExpr* expression)
			{
				if (expression && admitted(expression->getExprLoc()))
					target_slot(expression,
								expression->getStmtClassName(),
								expression->getSourceRange(),
								"callable",
								"calls",
								0U,
								expression->getDirectCallee(),
								"eligible",
								{},
								nullptr,
								expression);
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
				if (expression && admitted(expression->getExprLoc()))
				{
					target_slot(expression,
								expression->getStmtClassName(),
								expression->getSourceRange(),
								"state_access",
								"accesses_member",
								0U,
								expression->getMemberDecl(),
								"eligible",
								{},
								nullptr,
								expression);
					if (llvm::isa<clang::VarDecl, clang::FieldDecl>(expression->getMemberDecl()))
						storage_target_slots(*expression, expression->getMemberDecl());
				}
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
					target_slot(initializer,
								"CXXCtorInitializer",
								initializer->getSourceRange(),
								"state_access",
								"accesses_member",
								0U,
								initializer->getMember());
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
				if (expression && admitted(expression->getExprLoc()) &&
					llvm::isa<clang::VarDecl>(expression->getDecl()))
					storage_target_slots(*expression, expression->getDecl());
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
					std::optional<std::size_t> inclusion_slot;
					if (const auto* inclusion =
							llvm::dyn_cast<clang::InclusionDirective>(observation))
						if (const auto* member = admitted(inclusion->getSourceRange().getBegin()))
						{
							const auto include_slot = target_slot(inclusion,
																  "InclusionDirective",
																  inclusion->getSourceRange(),
																  "include",
																  "includes",
																  0U,
																  nullptr);
							inclusion_slot = include_slot;
							auto& slot = pending_target_slots_[include_slot].value;
							const auto actual_file = inclusion->getFile();
							const auto* resolved_file = actual_file
								? closure_.find_member(project_path(actual_file->getName()))
								: nullptr;
							if (resolved_file)
							{
								slot.emplace("target_file", id("file_id", resolved_file->file_id));
								slot.insert_or_assign(
									"resolution",
									symbol("cc.target-slot-resolution/1", "resolved"));
								slot.erase("reason");
							}
							else if (!actual_file)
							{
								slot.insert_or_assign(
									"resolution",
									symbol("cc.target-slot-resolution/1", "unresolved"));
								slot.insert_or_assign(
									"reason",
									sdk::detached_cell::utf8("compiler-include-resolution-failed"));
							}
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
						if (inclusion_slot)
							pending_target_slots_[*inclusion_slot].value.emplace(
								"include", id("include_id", row_id(include, "include")));
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
					value.emplace("phase", symbol("source.pp-phase/1", "evaluated"));
					value.emplace("profile",
								  sdk::detached_cell::utf8("clang22-evaluated-preprocessor/1"));
					if (event.function_like)
						value.emplace("function_like",
									  sdk::detached_cell::boolean(*event.function_like));
					if (event.variadic)
						value.emplace("variadic", sdk::detached_cell::boolean(*event.variadic));
					for (const auto& [key, number] : std::initializer_list<
							 std::pair<std::string_view, std::optional<std::uint64_t>>>{
							 {"parameter_count", event.parameter_count},
							 {"replacement_count", event.replacement_count},
							 {"parameter_index", event.parameter_index},
							 {"argument_index", event.argument_index},
							 {"substitution_count", event.substitution_count},
							 {"evaluating_substitution_count",
							  event.evaluating_substitution_count}})
						if (number)
							value.emplace(std::string{key},
										  sdk::detached_cell::unsigned_integer(*number));
					if (!event.parameter_name.empty())
						value.emplace("parameter_symbol",
									  id("pp_symbol_id", pp_symbol(event.parameter_name)));
					if (event.argument_empty)
					{
						value.emplace("argument_empty",
									  sdk::detached_cell::boolean(*event.argument_empty));
						if (!*event.argument_empty)
							value.emplace("argument_source", id("source_span_id", source_id));
					}
					if (event.active)
						value.emplace(
							"activity",
							symbol("source.pp-activity/1", *event.active ? "active" : "inactive"));
					if (event.macro_complete)
						value.emplace("macro_state", symbol("source.pp-facet-state/1", "complete"));
					retain_population_bytes(source_id.size() + 1536U);
					pending_evaluated_pp_.push_back({event.logical_path, std::move(value)});
				}
				raw_pp_ordinal_ = ordinal;
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
						auto emitted =
							make_row(source::relations::token::descriptor(), std::move(values));
						const auto original_token = row_id(emitted, "token");
						append(std::move(emitted));
						return original_token;
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
					std::vector<raw_pp_token> directive_tokens;
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
							if (!directive_tokens.empty())
							{
								raw_directive(member, *blob->content, directive_tokens);
								directive_tokens.clear();
							}
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
						const auto original_token = emit("raw",
														 begin,
														 end,
														 count++,
														 clang::tok::getTokenName(kind),
														 spelling,
														 macro,
														 active,
														 directive,
														 directive_start);
						retain_population_bytes(original_token.size() + member.logical_path.size() +
												128U);
						raw_token_locations_.emplace(
							std::make_pair(member.logical_path, static_cast<std::uint64_t>(begin)),
							original_token);
						if (directive)
						{
							retain_population_bytes(original_token.size() + spelling.size() + 192U);
							directive_tokens.push_back({original_token,
														spelling,
														kind,
														begin,
														end,
														token.is(clang::tok::raw_identifier),
														token.hasLeadingSpace()});
						}
					}
					if (!directive_tokens.empty())
						raw_directive(member, *blob->content, directive_tokens);
					close_raw_conditionals(member);
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
				finish_syntax_bindings();
				finish_cfg_conditions();
				finish_operation_dispatch();
				finish_flow_expressions();
				finish_calls();
				if (templates_ != nullptr)
				{
					project_template_bindings bindings;
					bindings.compile_unit = observations_.compile_unit;
					bindings.entity = [&](std::string_view usr)
					{
						// Template observations retain raw compiler USR bytes. The
						// normalized entity carrier uses the existing provider key frame.
						retain_population_bytes(usr.size() + 10U);
						const auto found = entities_.find("clang-usr:" + std::string{usr});
						return found == entities_.end() ? std::string{} : found->second;
					};
					bindings.source = [&](template_native_source input)
					{
						const auto range = clang::SourceRange(
							clang::SourceLocation::getFromRawEncoding(input.begin),
							clang::SourceLocation::getFromRawEncoding(input.end));
						return span(range,
									input.role == template_native_source_role::declaration
										? "declaration"
										: "expression")
							.value_or("");
					};
					auto template_rows = take(detach_project_templates(*templates_, bindings));
					if (template_events_ != nullptr)
					{
						template_event_bindings event_bindings;
						event_bindings.compile_unit = bindings.compile_unit;
						event_bindings.entity = bindings.entity;
						event_bindings.source = [&](template_event_source input)
						{
							return bindings.source(
								{input.begin,
								 input.end,
								 input.role == template_event_source_role::declaration
									 ? template_native_source_role::declaration
									 : template_native_source_role::expression});
						};
						auto events =
							take(detach_project_template_events(*template_events_, event_bindings));
						const auto inventory =
							std::ranges::find(template_rows,
											  cc::relations::template_inventory::descriptor().id,
											  &sdk::detached_row::descriptor_id);
						if (inventory == template_rows.end())
							fail("template-events", "original-template-inventory-unavailable");
						for (auto& [column, value] : events.inventory_facets)
						{
							const auto existing = inventory->cells.find(column);
							if (existing != inventory->cells.end() &&
								existing->second.state != sdk::cell_state::absent)
								fail("template-events", "duplicate-original-event-facet");
							inventory->cells.insert_or_assign(column, std::move(value));
						}
						check(sdk::validate_row(cc::relations::template_inventory::descriptor(),
												*inventory));
						check(sdk::validate_domain_identity(
							cc::relations::template_inventory::descriptor(), *inventory));
						for (auto& row : events.rows)
							append(std::move(row));
					}
					for (auto& row : template_rows)
						append(std::move(row));
				}
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
				finish_target_slots(declarations);
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
			std::string
			pp_source(const source_closure_member& member, std::uint64_t begin, std::uint64_t end)
			{
				const auto snapshot = take(sdk::detail::derive_source_snapshot_id(
					member.file_id, member.content_digest, "utf8"));
				auto row = make_row(source::relations::span::descriptor(),
									{{"snapshot", id("source_snapshot_id", snapshot)},
									 {"file", id("file_id", member.file_id)},
									 {"begin", sdk::detached_cell::unsigned_integer(begin)},
									 {"end", sdk::detached_cell::unsigned_integer(end)},
									 {"role", symbol("source.range-role/1", "preprocessor")},
									 {"read_only", sdk::detached_cell::boolean(true)}});
				const auto result = row_id(row, "span");
				append(std::move(row));
				return result;
			}
			std::string pp_symbol(std::string_view identifier)
			{
				if (project_id_.empty())
					return {};
				return take(sdk::semantic_digest(
					"source.clang22.pp-symbol.v1",
					std::to_string(project_id_.size()) + ":" + project_id_ +
						std::to_string(identifier.size()) + ":" + std::string{identifier}));
			}
			std::string pp_event_id(const fields& value)
			{
				return row_id(make_row(source::relations::preprocessor_event::descriptor(), value),
							  "event");
			}
			fields raw_pp_fields(const source_closure_member& member,
								 std::string kind,
								 std::string name,
								 std::uint64_t begin,
								 std::uint64_t end)
			{
				++raw_pp_admissions_[member.logical_path];
				auto value = common();
				const auto original_source = pp_source(member, begin, end);
				retain_population_bytes(original_source.size() + 128U);
				raw_event_bounds_.emplace(original_source, std::make_pair(begin, end));
				value.emplace("source", id("source_span_id", original_source));
				value.emplace("kind", symbol("source.pp-event-kind/1", std::move(kind)));
				value.emplace("name", sdk::detached_cell::utf8(std::move(name)));
				value.emplace("state", symbol("source.pp-state/1", "observed"));
				value.emplace("ordinal", sdk::detached_cell::unsigned_integer(raw_pp_ordinal_++));
				value.emplace("phase", symbol("source.pp-phase/1", "raw"));
				value.emplace("profile",
							  sdk::detached_cell::utf8("clang22-frozen-raw-directives/1"));
				return value;
			}
			void raw_macro(const source_closure_member& member,
						   std::size_t primary,
						   const std::vector<raw_pp_token>& tokens)
			{
				bool complete = tokens.size() >= 3U && tokens[2].identifier && !project_id_.empty();
				if (!complete)
				{
					raw_macro_partial_[member.logical_path] = true;
					pending_raw_pp_[primary].value.emplace(
						"macro_state", symbol("source.pp-facet-state/1", "partial"));
					return;
				}
				const auto parent = pp_event_id(pending_raw_pp_[primary].value);
				const auto child_begin = pending_raw_pp_.size();
				const bool function_like = tokens.size() > 3U &&
					tokens[3].kind == clang::tok::l_paren && !tokens[3].leading_space;
				bool variadic{};
				std::size_t replacement = 3U;
				struct parameter
				{
					std::string name;
					std::vector<std::string> tokens;
					std::uint64_t begin{}, end{};
				};
				std::vector<parameter> parameters;
				if (function_like)
				{
					replacement = 4U;
					bool need_parameter = true;
					while (replacement < tokens.size() &&
						   tokens[replacement].kind != clang::tok::r_paren)
					{
						if (!need_parameter || parameters.size() >= 100000U)
						{
							complete = false;
							break;
						}
						const auto& token = tokens[replacement++];
						parameter p;
						p.begin = token.begin;
						p.end = token.end;
						p.tokens.push_back(token.id);
						if (token.kind == clang::tok::ellipsis)
						{
							p.name = "__VA_ARGS__";
							variadic = true;
						}
						else if (token.identifier)
						{
							p.name = token.spelling;
							if (replacement < tokens.size() &&
								tokens[replacement].kind == clang::tok::ellipsis)
							{
								variadic = true;
								p.tokens.push_back(tokens[replacement].id);
								p.end = tokens[replacement++].end;
							}
						}
						else
						{
							complete = false;
							break;
						}
						retain_population_bytes(p.name.size() + 192U);
						parameters.push_back(std::move(p));
						need_parameter = false;
						if (replacement < tokens.size() &&
							tokens[replacement].kind == clang::tok::comma && !variadic)
						{
							++replacement;
							need_parameter = true;
						}
					}
					if (replacement >= tokens.size() ||
						tokens[replacement].kind != clang::tok::r_paren ||
						(need_parameter && !parameters.empty()))
						complete = false;
					else
						++replacement;
				}
				std::set<std::string, std::less<>> stringify, paste;
				std::map<std::string, std::size_t, std::less<>> parameter_indices;
				for (std::size_t i{}; i < parameters.size(); ++i)
				{
					retain_population_bytes(parameters[i].name.size() + 128U);
					if (!parameter_indices.emplace(parameters[i].name, i).second)
						complete = false;
				}
				std::vector<std::uint64_t> uses(parameters.size()),
					expression_uses(parameters.size());
				for (std::size_t index = replacement; index < tokens.size(); ++index)
				{
					const auto& token = tokens[index];
					const bool hash = token.kind == clang::tok::hash,
							   hashhash = token.kind == clang::tok::hashhash;
					if (hashhash)
					{
						paste.insert(token.id);
						if (index == replacement || index + 1U == tokens.size())
							complete = false;
					}
					if (hash && function_like)
					{
						const bool parameter_next = index + 1U < tokens.size() &&
							tokens[index + 1U].identifier &&
							parameter_indices.contains(tokens[index + 1U].spelling);
						if (function_like && parameter_next)
							stringify.insert(token.id);
						else
							complete = false;
					}
					if (hashhash || stringify.contains(token.id))
					{
						auto child = raw_pp_fields(
							member, "macro_replacement_operator", {}, token.begin, token.end);
						child.emplace("macro_state", symbol("source.pp-facet-state/1", "complete"));
						child.emplace("raw_event", id("preprocessor_event_id", parent));
						child.emplace("parent_event", id("preprocessor_event_id", parent));
						child.emplace("raw_token_ids", flags("source_token_id", {token.id}));
						child.emplace("paste_token_ids",
									  flags("source_token_id",
											hashhash ? std::set<std::string, std::less<>>{token.id}
													 : std::set<std::string, std::less<>>{}));
						child.emplace("stringify_token_ids",
									  flags("source_token_id",
											hashhash
												? std::set<std::string, std::less<>>{}
												: std::set<std::string, std::less<>>{token.id}));
						retain_population_bytes(768U);
						pending_raw_pp_.push_back({member.logical_path, std::move(child)});
					}
					const auto found = token.identifier ? parameter_indices.find(token.spelling)
														: parameter_indices.end();
					if (found != parameter_indices.end())
					{
						const auto parameter_index = found->second;
						++uses[parameter_index];
						if (!(index > replacement &&
							  (tokens[index - 1U].kind == clang::tok::hash ||
							   tokens[index - 1U].kind == clang::tok::hashhash)) &&
							!(index + 1U < tokens.size() &&
							  tokens[index + 1U].kind == clang::tok::hashhash))
							++expression_uses[parameter_index];
					}
				}
				for (std::size_t index{}; index < parameters.size(); ++index)
				{
					const auto& p = parameters[index];
					auto child = raw_pp_fields(member, "macro_parameter", p.name, p.begin, p.end);
					child.emplace("macro_state", symbol("source.pp-facet-state/1", "complete"));
					child.emplace("raw_event", id("preprocessor_event_id", parent));
					child.emplace("parent_event", id("preprocessor_event_id", parent));
					child.emplace("parameter_index", sdk::detached_cell::unsigned_integer(index));
					child.emplace("parameter_symbol", id("pp_symbol_id", pp_symbol(p.name)));
					child.emplace("raw_token_ids",
								  flags("source_token_id", {p.tokens.begin(), p.tokens.end()}));
					child.emplace("substitution_count",
								  sdk::detached_cell::unsigned_integer(uses[index]));
					if (expression_uses[index] == 0U)
						child.emplace("evaluating_substitution_count",
									  sdk::detached_cell::unsigned_integer(0U));
					retain_population_bytes(1024U);
					pending_raw_pp_.push_back({member.logical_path, std::move(child)});
				}
				auto& value = pending_raw_pp_[primary].value;
				value.emplace("function_like", sdk::detached_cell::boolean(function_like));
				value.emplace("variadic", sdk::detached_cell::boolean(variadic));
				value.emplace("parameter_count",
							  sdk::detached_cell::unsigned_integer(parameters.size()));
				value.emplace("replacement_count",
							  sdk::detached_cell::unsigned_integer(
								  tokens.size() - std::min(replacement, tokens.size())));
				value.emplace("stringify_token_ids",
							  flags("source_token_id", std::move(stringify)));
				value.emplace("paste_token_ids", flags("source_token_id", std::move(paste)));
				value.emplace("macro_state",
							  symbol("source.pp-facet-state/1", complete ? "complete" : "partial"));
				if (!complete)
				{
					raw_macro_partial_[member.logical_path] = true;
					for (std::size_t child = child_begin; child < pending_raw_pp_.size(); ++child)
						pending_raw_pp_[child].value.insert_or_assign(
							"macro_state", symbol("source.pp-facet-state/1", "partial"));
				}
			}
			void raw_directive(const source_closure_member& member,
							   const std::string& content,
							   const std::vector<raw_pp_token>& tokens)
			{
				const auto keyword = tokens.size() > 1U ? tokens[1].spelling : std::string{};
				static const std::set<std::string, std::less<>> recognized{"define",
																		   "undef",
																		   "include",
																		   "if",
																		   "ifdef",
																		   "ifndef",
																		   "elif",
																		   "elifdef",
																		   "elifndef",
																		   "else",
																		   "endif",
																		   "pragma",
																		   "line",
																		   "error",
																		   "warning"};
				if (keyword == "pragma" || keyword == "line" || keyword == "error" ||
					keyword == "warning" || keyword == "unknown")
					candidate_partial_[member.logical_path] = true;
				const auto kind = keyword.empty()  ? "raw_null"
					: recognized.contains(keyword) ? "raw_" + keyword
												   : "raw_unknown";
				const auto begin = tokens.front().begin, end = tokens.back().end;
				auto value = raw_pp_fields(member,
										   kind,
										   (kind == "raw_define" || kind == "raw_undef") &&
												   tokens.size() > 2U
											   ? tokens[2].spelling
											   : std::string{},
										   begin,
										   end);
				std::set<std::string, std::less<>> token_ids, condition_ids, features;
				for (std::size_t index{}; index < tokens.size(); ++index)
				{
					token_ids.insert(tokens[index].id);
					if (index > 1U &&
						(keyword == "if" || keyword == "ifdef" || keyword == "ifndef" ||
						 keyword == "elif" || keyword == "elifdef" || keyword == "elifndef"))
					{
						condition_ids.insert(tokens[index].id);
						if (tokens[index].identifier && tokens[index].spelling != "defined" &&
							!project_id_.empty())
							features.insert(pp_symbol(tokens[index].spelling));
					}
				}
				value.emplace("raw_token_ids", flags("source_token_id", std::move(token_ids)));
				value.emplace("condition_tokens",
							  flags("source_token_id", std::move(condition_ids)));
				value.emplace("feature_symbols", flags("pp_symbol_id", std::move(features)));
				if (kind == "raw_pragma")
				{
					std::string pragma = "unknown", state = "partial";
					if (tokens.size() > 2U && tokens[2].identifier)
					{
						const auto& head = tokens[2].spelling;
						if (head == "GCC" || head == "clang")
						{
							if (tokens.size() > 3U && tokens[3].identifier &&
								(tokens[3].spelling == "diagnostic" ||
								 tokens[3].spelling == "visibility"))
								pragma = tokens[3].spelling;
						}
						else if (head == "pack" || head == "once" || head == "push_macro" ||
								 head == "pop_macro" || head == "diagnostic" ||
								 head == "visibility" || head == "weak" || head == "message" ||
								 head == "omp")
							pragma = head;
						else if (tokens.size() == 3U)
							pragma = "other";
						if (pragma != "unknown")
							state = "complete";
					}
					value.emplace("pragma_kind", symbol("source.pragma-kind/1", pragma));
					value.emplace("pragma_state", symbol("source.pp-observation-state/1", state));
					value.emplace("pragma_profile",
								  sdk::detached_cell::utf8("clang22-original-raw-pragma-kind/1"));
				}
				value.emplace("depth",
							  sdk::detached_cell::unsigned_integer(raw_conditionals_.size()));
				value.emplace("structure_state", symbol("source.pp-facet-state/1", "complete"));
				value.emplace("region_begin", sdk::detached_cell::unsigned_integer(begin));
				value.emplace("region_end", sdk::detached_cell::unsigned_integer(end));
				const auto index = pending_raw_pp_.size();
				retain_population_bytes(2048U + tokens.size() * 128U);
				pending_raw_pp_.push_back({member.logical_path, std::move(value)});
				for (const auto& token : tokens)
				{
					retain_population_bytes(member.logical_path.size() + 128U);
					raw_directive_locations_.emplace(
						std::make_pair(member.logical_path, token.begin), index);
				}
				const auto current = pp_event_id(pending_raw_pp_[index].value);
				if (!raw_conditionals_.empty() && keyword != "elif" && keyword != "elifdef" &&
					keyword != "elifndef" && keyword != "else" && keyword != "endif")
					pending_raw_pp_[index].value.emplace(
						"branch_event",
						id("preprocessor_event_id",
						   pp_event_id(pending_raw_pp_[raw_conditionals_.back().branch].value)));
				const auto field = [&](std::size_t i, std::string name, sdk::detached_cell cell)
				{
					pending_raw_pp_[i].value.insert_or_assign(std::move(name), std::move(cell));
				};
				const auto body_begin = [&]
				{
					auto cursor = static_cast<std::size_t>(end);
					while (cursor < content.size() && content[cursor] != '\n' &&
						   content[cursor] != '\r')
						++cursor;
					if (cursor < content.size())
					{
						if (content[cursor] == '\r' && cursor + 1U < content.size() &&
							content[cursor + 1U] == '\n')
							++cursor;
						++cursor;
					}
					return cursor;
				};
				if (keyword == "if" || keyword == "ifdef" || keyword == "ifndef")
				{
					if (!raw_conditionals_.empty())
						field(index,
							  "parent_event",
							  id("preprocessor_event_id",
								 pp_event_id(
									 pending_raw_pp_[raw_conditionals_.back().branch].value)));
					field(index, "branch_event", id("preprocessor_event_id", current));
					field(
						index, "region_begin", sdk::detached_cell::unsigned_integer(body_begin()));
					field(index,
						  "region_end",
						  sdk::detached_cell::unsigned_integer(member.size_bytes));
					field(index,
						  "depth",
						  sdk::detached_cell::unsigned_integer(raw_conditionals_.size() + 1U));
					if (raw_conditionals_.size() >= 4096U)
						fail("preprocessor", "conditional-depth-limit");
					retain_population_bytes(192U);
					raw_conditionals_.push_back({index, index, {index}, false});
				}
				else if (keyword == "elif" || keyword == "elifdef" || keyword == "elifndef" ||
						 keyword == "else" || keyword == "endif")
				{
					if (raw_conditionals_.empty())
					{
						raw_structure_partial_[member.logical_path] = true;
						field(
							index, "structure_state", symbol("source.pp-facet-state/1", "partial"));
					}
					else
					{
						auto& frame = raw_conditionals_.back();
						field(frame.branch,
							  "region_end",
							  sdk::detached_cell::unsigned_integer(begin));
						field(index,
							  "depth",
							  sdk::detached_cell::unsigned_integer(raw_conditionals_.size()));
						field(index,
							  "parent_event",
							  id("preprocessor_event_id",
								 pp_event_id(pending_raw_pp_[frame.opening].value)));
						if (keyword == "endif")
						{
							for (auto branch : frame.branches)
								field(
									branch, "closing_event", id("preprocessor_event_id", current));
							raw_conditionals_.pop_back();
						}
						else
						{
							if (frame.saw_else)
							{
								raw_structure_partial_[member.logical_path] = true;
								field(index,
									  "structure_state",
									  symbol("source.pp-facet-state/1", "partial"));
							}
							frame.saw_else |= keyword == "else";
							field(index, "branch_event", id("preprocessor_event_id", current));
							field(index,
								  "region_begin",
								  sdk::detached_cell::unsigned_integer(body_begin()));
							field(index,
								  "region_end",
								  sdk::detached_cell::unsigned_integer(member.size_bytes));
							frame.branch = index;
							frame.branches.push_back(index);
						}
					}
				}
				if (kind == "raw_define")
					raw_macro(member, index, tokens);
			}
			void close_raw_conditionals(const source_closure_member& member)
			{
				if (raw_conditionals_.empty())
					return;
				raw_structure_partial_[member.logical_path] = true;
				for (const auto& frame : raw_conditionals_)
					for (auto branch : frame.branches)
						pending_raw_pp_[branch].value.insert_or_assign(
							"structure_state", symbol("source.pp-facet-state/1", "partial"));
				raw_conditionals_.clear();
			}

		  public:
			void finish_preprocessor(const project_preprocessor_observations& observations)
			{
				bind_preprocessor_facets(observations);
				bind_raw_activity(observations);
				std::map<std::pair<std::string, std::string>, std::set<std::string, std::less<>>>
					populations;
				for (auto* phase : {&pending_raw_pp_, &pending_evaluated_pp_})
					for (auto& event : *phase)
					{
						const auto event_phase =
							std::get<std::string>(*event.value.at("phase").value);
						auto row = make_row(source::relations::preprocessor_event::descriptor(),
											std::move(event.value));
						const auto original = row_id(row, "event");
						retain_population_bytes(original.size() + 192U);
						populations[{event.file, event_phase}].insert(original);
						append(std::move(row));
					}
				for (const auto& member : closure_.members)
					for (const auto phase : {"raw", "evaluated"})
					{
						const bool raw = std::string_view{phase} == "raw",
								   opened = observations.opened_files.contains(member.logical_path);
						const bool complete = raw ||
							(opened && !observations.truncated &&
							 !observations.classification_incomplete.contains(member.logical_path));
						auto value = common();
						value.emplace("file", id("file_id", member.file_id));
						value.emplace("source_snapshot",
									  id("source_snapshot_id",
										 take(sdk::detail::derive_source_snapshot_id(
											 member.file_id, member.content_digest, "utf8"))));
						value.emplace("phase", symbol("source.pp-phase/1", phase));
						value.emplace(
							"profile",
							sdk::detached_cell::utf8(raw ? "clang22-frozen-raw-directives/1"
														 : "clang22-evaluated-preprocessor/1"));
						const auto& events = populations[{member.logical_path, phase}];
						value.emplace(
							"event_count",
							sdk::detached_cell::unsigned_integer(
								raw ? raw_pp_admissions_[member.logical_path]
									: observations.event_admissions.contains(member.logical_path)
									? observations.event_admissions.at(member.logical_path)
									: 0U));
						value.emplace("event_ids", flags("preprocessor_event_id", events));
						value.emplace("enumeration_state",
									  symbol("source.pp-enumeration-state/1",
											 complete	   ? "complete"
												 : !opened ? "unavailable"
														   : "partial"));
						value.emplace(
							"structure_state",
							symbol("source.pp-facet-state/1",
								   raw ? raw_structure_partial_[member.logical_path] ? "partial"
																					 : "complete"
									   : !opened									 ? "unavailable"
									   : raw_structure_partial_[member.logical_path] ? "partial"
																					 : "complete"));
						value.emplace("activity_state",
									  symbol("source.pp-facet-state/1",
											 !opened ? "unavailable"
												 : raw_activity_partial_[member.logical_path]
												 ? "partial"
												 : "complete"));
						value.emplace("macro_state",
									  symbol("source.pp-facet-state/1",
											 raw ? raw_macro_partial_[member.logical_path]
													 ? "partial"
													 : "complete"
												 : !opened ? "unavailable"
												 : evaluated_macro_partial_[member.logical_path]
												 ? "partial"
												 : "complete"));
						value.emplace("candidate_state",
									  symbol("source.pp-facet-state/1",
											 raw ? "unavailable"
												 : !opened ? "unavailable"
												 : candidate_partial_[member.logical_path] ||
													 observations.tokens_truncated ||
													 observations.tokens_unmapped
												 ? "partial"
												 : "complete"));
						value.emplace("expansion_state",
									  symbol("source.pp-facet-state/1",
											 raw ? "unavailable"
												 : !opened ? "unavailable"
												 : evaluated_expansion_partial_[member.logical_path]
												 ? "partial"
												 : "complete"));
						value.emplace("argument_effect_state",
									  symbol("source.pp-facet-state/1",
											 raw ? "unavailable"
												 : !opened ? "unavailable"
												 : evaluated_effect_partial_[member.logical_path]
												 ? "partial"
												 : "complete"));
						if (!raw)
							value.emplace(
								"reason",
								sdk::detached_cell::utf8(!opened ? "source-not-preprocessed-in-unit"
																 : "evaluated-typed-macro-and-"
																   "context-observation-frontier"));
						append(make_row(source::relations::preprocessor_inventory::descriptor(),
										std::move(value)));
					}
			}

		  private:
			std::optional<std::pair<std::string, std::uint64_t>>
			pp_location(std::uint32_t original) const
			{
				if (!original)
					return std::nullopt;
				const auto location = unit_.source_manager().getSpellingLoc(
					clang::SourceLocation::getFromRawEncoding(original));
				if (location.isInvalid())
					return std::nullopt;
				const auto logical = project_path(unit_.source_manager().getFilename(location));
				if (!closure_.find_member(logical))
					return std::nullopt;
				return std::make_pair(logical, unit_.source_manager().getFileOffset(location));
			}
			std::optional<std::pair<std::uint32_t, std::uint32_t>>
			argument_origin(const clang::Expr& expression) const
			{
				auto& manager = unit_.source_manager();
				const auto begin = expression.getBeginLoc(), end = expression.getEndLoc();
				if (!begin.isMacroID() || !end.isMacroID() || !manager.isMacroArgExpansion(begin) ||
					!manager.isMacroArgExpansion(end))
					return std::nullopt;
				return std::make_pair(manager.getSpellingLoc(begin).getRawEncoding(),
									  manager.getSpellingLoc(end).getRawEncoding());
			}
			void observe_macro_argument_expression(const clang::Expr& expression,
												   const std::string& node,
												   const clang::Stmt* parent)
			{
				const auto origin = argument_origin(expression);
				if (!origin)
					return;
				if (const auto* enclosing = llvm::dyn_cast_or_null<clang::Expr>(parent);
					enclosing && argument_origin(*enclosing) == origin)
					return;
				retain_population_bytes(node.size() + current_function_.size() + 256U);
				macro_argument_expressions_[*origin].push_back(
					{node,
					 current_function_,
					 evaluation_context(expression),
					 expression.HasSideEffects(unit_.ast())});
			}
			void bind_preprocessor_facets(const project_preprocessor_observations& observations)
			{
				std::vector<std::string> original_ids;
				retain_population_bytes(pending_evaluated_pp_.size() * 128U);
				original_ids.reserve(pending_evaluated_pp_.size());
				for (const auto& event : pending_evaluated_pp_)
					original_ids.push_back(pp_event_id(event.value));
				std::map<std::uint32_t, std::vector<std::size_t>> expansions;
				std::set<std::uint32_t> actual_argument_locations;
				for (const auto& event : observations.events)
					for (const auto location : event.argument_locations)
					{
						retain_population_bytes(64U);
						actual_argument_locations.insert(location);
					}
				for (std::size_t index{}; index < observations.events.size(); ++index)
					if (observations.events[index].kind == "macro_expansion")
					{
						retain_population_bytes(128U);
						expansions[observations.events[index].location].push_back(index);
					}
				for (std::size_t index{}; index < observations.events.size(); ++index)
				{
					const auto& event = observations.events[index];
					auto& value = pending_evaluated_pp_[index].value;
					const auto location = pp_location(event.location);
					const auto raw = location ? raw_directive_locations_.find(*location)
											  : raw_directive_locations_.end();
					if (raw != raw_directive_locations_.end())
					{
						const auto& original = pending_raw_pp_[raw->second].value;
						if (event.active)
						{
							retain_population_bytes(64U);
							raw_activity_observations_[raw->second].insert(*event.active);
						}
						value.emplace("raw_event",
									  id("preprocessor_event_id", pp_event_id(original)));
						for (const auto field : {"raw_token_ids",
												 "stringify_token_ids",
												 "paste_token_ids",
												 "condition_tokens",
												 "feature_symbols"})
							if (const auto found = original.find(field); found != original.end())
								value.emplace(field, found->second);
						for (const auto field :
							 {"region_begin", "region_end", "depth", "structure_state"})
							if (const auto found = original.find(field); found != original.end())
								value.emplace(field, found->second);
					}
					else if (event.kind == "macro_definition")
					{
						value.insert_or_assign("macro_state",
											   symbol("source.pp-facet-state/1", "partial"));
						evaluated_macro_partial_[event.logical_path] = true;
					}
					if (event.definition_location)
					{
						const auto origin = pp_location(event.definition_location);
						const auto definition = origin ? raw_directive_locations_.find(*origin)
													   : raw_directive_locations_.end();
						if (definition != raw_directive_locations_.end())
							value.emplace(
								"definition_event",
								id("preprocessor_event_id",
								   pp_event_id(pending_raw_pp_[definition->second].value)));
						else
						{
							evaluated_macro_partial_[event.logical_path] = true;
						}
					}
					if (event.closing_observation)
						value.emplace("closing_event",
									  id("preprocessor_event_id",
										 original_ids.at(*event.closing_observation)));
					if (event.kind == "if" || event.kind == "ifdef" || event.kind == "ifndef" ||
						event.kind == "elif" || event.kind == "elifdef" ||
						event.kind == "elifndef" || event.kind == "else")
						value.emplace("branch_event",
									  id("preprocessor_event_id", original_ids[index]));
					if (event.parent_observation)
						value.emplace("parent_event",
									  id("preprocessor_event_id",
										 original_ids.at(*event.parent_observation)));
					if (event.kind == "macro_expansion")
					{
						if (event.parent_location)
						{
							const auto parent = expansions.find(event.parent_location);
							if (parent != expansions.end() && parent->second.size() == 1U &&
								parent->second.front() != index)
							{
								value.emplace("parent_expansion",
											  id("preprocessor_event_id",
												 original_ids[parent->second.front()]));
								// Actual SourceManager ancestry, not callback order or source
								// containment.
								std::uint64_t depth = 1U;
								auto cursor = event.parent_location;
								std::set<std::uint32_t> visited;
								while (cursor && depth < 4096U && !visited.contains(cursor))
								{
									retain_population_bytes(64U);
									visited.insert(cursor);
									const auto p = expansions.find(cursor);
									if (p == expansions.end() || p->second.size() != 1U)
										break;
									cursor = observations.events[p->second.front()].parent_location;
									if (cursor)
										++depth;
								}
								if (!cursor)
									value.emplace("expansion_depth",
												  sdk::detached_cell::unsigned_integer(depth));
								else
									evaluated_expansion_partial_[event.logical_path] = true;
							}
							else
								evaluated_expansion_partial_[event.logical_path] = true;
						}
						else
						{
							// File-location arguments can be pre-expanded inside a containing
							// invocation. Only retain depth zero when no actual argument token
							// binds this invocation.
							const bool argument_nested =
								actual_argument_locations.contains(event.location);
							if (!argument_nested)
								value.emplace("expansion_depth",
											  sdk::detached_cell::unsigned_integer(0U));
							else
								evaluated_expansion_partial_[event.logical_path] = true;
						}
					}
					if (event.kind != "macro_argument")
						continue;
					if (event.argument_empty == true)
					{
						value.emplace("argument_may_have_side_effects",
									  sdk::detached_cell::boolean(false));
						value.emplace("effect_state",
									  symbol("source.pp-effect-state/1", "complete"));
						continue;
					}
					std::set<std::string, std::less<>> tokens;
					for (const auto original : event.argument_locations)
					{
						const auto l = pp_location(original);
						const auto t =
							l ? raw_token_locations_.find(*l) : raw_token_locations_.end();
						if (t != raw_token_locations_.end())
							tokens.insert(t->second);
					}
					value.emplace("raw_token_ids", flags("source_token_id", std::move(tokens)));
					const auto key = event.argument_locations.empty()
						? std::pair<std::uint32_t, std::uint32_t>{}
						: std::make_pair(
							  unit_.source_manager()
								  .getSpellingLoc(clang::SourceLocation::getFromRawEncoding(
									  event.argument_locations.front()))
								  .getRawEncoding(),
							  unit_.source_manager()
								  .getSpellingLoc(clang::SourceLocation::getFromRawEncoding(
									  event.argument_locations.back()))
								  .getRawEncoding());
					const auto facts = macro_argument_expressions_.find(key);
					if (facts != macro_argument_expressions_.end() && !facts->second.empty())
					{
						const auto& first = facts->second.front();
						bool consistent = true;
						std::uint64_t evaluated{};
						for (const auto& fact : facts->second)
						{
							consistent &= fact.function == first.function &&
								fact.side_effects == first.side_effects &&
								fact.evaluation != "unknown" && fact.evaluation != "dependent";
							if (fact.evaluation == "potentially_evaluated")
								++evaluated;
						}
						if (consistent)
						{
							value.emplace("effect_expression", id("syntax_node_id", first.node));
							if (!first.function.empty())
								value.emplace("effect_function",
											  id("cc_entity_id", first.function));
							value.emplace("argument_may_have_side_effects",
										  sdk::detached_cell::boolean(first.side_effects));
							value.insert_or_assign("evaluating_substitution_count",
												   sdk::detached_cell::unsigned_integer(evaluated));
							value.emplace("effect_state",
										  symbol("source.pp-effect-state/1", "complete"));
						}
						else
							value.emplace("effect_state",
										  symbol("source.pp-effect-state/1", "partial"));
					}
					else
						value.emplace("effect_state",
									  symbol("source.pp-effect-state/1", "partial"));
					if (std::get<std::string>(*value.at("effect_state").value) != "complete")
						evaluated_effect_partial_[event.logical_path] = true;
				}
			}

			void bind_raw_activity(const project_preprocessor_observations& observations)
			{
				// Index original physical skipped intervals once, rather than scanning all
				// callback observations for every raw directive and auxiliary member.
				std::map<std::string, std::map<std::uint64_t, std::uint64_t>, std::less<>>
					skipped_ranges;
				for (const auto& event : observations.events)
				{
					if (event.kind != "skipped_range")
						continue;
					retain_population_bytes(event.logical_path.size() + 160U);
					auto& ranges = skipped_ranges[event.logical_path];
					auto [range, inserted] = ranges.try_emplace(event.begin, event.end);
					if (!inserted)
						range->second = std::max(range->second, event.end);
				}
				for (auto& [file, ranges] : skipped_ranges)
				{
					std::uint64_t maximum_end{};
					for (auto& [begin, end] : ranges)
					{
						maximum_end = std::max(maximum_end, end);
						end = maximum_end;
					}
				}
				std::map<std::string, std::size_t, std::less<>> originals;
				for (std::size_t index{}; index < pending_raw_pp_.size(); ++index)
				{
					retain_population_bytes(128U);
					originals.emplace(pp_event_id(pending_raw_pp_[index].value), index);
				}
				for (std::size_t index{}; index < pending_raw_pp_.size(); ++index)
				{
					auto& event = pending_raw_pp_[index];
					auto& value = event.value;
					if (!observations.opened_files.contains(event.file))
					{
						value.emplace("activity", symbol("source.pp-activity/1", "unknown"));
						continue;
					}
					std::optional<bool> active;
					const auto actual = raw_activity_observations_.find(index);
					if (actual != raw_activity_observations_.end() && actual->second.size() == 1U)
						active = *actual->second.begin();
					else if (actual != raw_activity_observations_.end())
						raw_activity_partial_[event.file] = true;
					if (!active && actual == raw_activity_observations_.end())
					{
						const auto source = value.at("source");
						const auto id_value = std::get<std::string>(*source.value);
						// The raw event source was emitted from compiler lexer offsets; skipped
						// ranges describe physical preprocessing activity, not AST object identity.
						const auto bounds = raw_event_bounds_.find(id_value);
						if (bounds != raw_event_bounds_.end())
						{
							if (const auto file = skipped_ranges.find(event.file);
								file != skipped_ranges.end())
							{
								const auto next = file->second.upper_bound(bounds->second.first);
								if (next != file->second.begin() &&
									bounds->second.first < std::prev(next)->second)
								{
									active = false;
								}
							}
						}
						auto branch = value.find("branch_event");
						if (branch == value.end())
							branch = value.find("raw_event");
						if (!active && branch != value.end())
						{
							const auto parent =
								originals.find(std::get<std::string>(*branch->second.value));
							if (parent != originals.end() && parent->second < index)
							{
								const auto activity =
									pending_raw_pp_[parent->second].value.find("activity");
								if (activity != pending_raw_pp_[parent->second].value.end())
								{
									const auto state =
										std::get<std::string>(*activity->second.value);
									if (state == "active")
										active = true;
									else if (state == "inactive")
										active = false;
								}
							}
						}
						if (!active && branch == value.end())
							active = true;
					}
					if (!active)
						raw_activity_partial_[event.file] = true;
					value.emplace("activity",
								  symbol("source.pp-activity/1",
										 active ? *active ? "active" : "inactive" : "unknown"));
				}
			}

			static std::vector<std::string> ast_operation_kinds(const clang::Stmt* statement)
			{
				std::vector<std::string> kinds;
				if (llvm::isa<clang::CallExpr>(statement))
					kinds.emplace_back("invocation");
				if (llvm::isa<clang::CXXConstructExpr>(statement))
					kinds.emplace_back("construction");
				if (const auto* allocation = llvm::dyn_cast<clang::CXXNewExpr>(statement))
				{
					kinds.emplace_back("allocation");
					if (allocation->getOperatorDelete())
						kinds.emplace_back("initialization_failure_deallocation");
				}
				if (llvm::isa<clang::CXXDeleteExpr>(statement))
					kinds.emplace_back("deallocation");
				if (const auto* cast = llvm::dyn_cast<clang::ImplicitCastExpr>(statement);
					cast && cast->getCastKind() == clang::CK_LValueToRValue)
					kinds.emplace_back("load");
				if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(statement);
					binary && binary->isAssignmentOp())
					kinds.emplace_back(binary->isCompoundAssignmentOp() ? "update" : "store");
				if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(statement);
					unary && unary->isIncrementDecrementOp())
					kinds.emplace_back("update");
				if (llvm::isa<clang::AtomicExpr>(statement))
					kinds.emplace_back("atomic");
				if (llvm::isa<clang::CXXThrowExpr>(statement))
					kinds.emplace_back("throw_expression");
				if (llvm::isa<clang::CoawaitExpr, clang::CoyieldExpr>(statement))
					kinds.emplace_back("coroutine_suspend");
				if (llvm::isa<clang::CXXBindTemporaryExpr>(statement))
					kinds.emplace_back("type_use");
				if (const auto* temporary =
						llvm::dyn_cast<clang::MaterializeTemporaryExpr>(statement);
					temporary &&
					!llvm::isa<clang::CXXBindTemporaryExpr>(
						temporary->getSubExpr()->IgnoreParenImpCasts()))
					kinds.emplace_back("type_use");
				return kinds;
			}
			void retain_operation_bytes(std::size_t bytes)
			{
				if (bytes > 64U * 1024U * 1024U - operation_bytes_)
					fail("compiler-actions", "retained-byte-limit");
				operation_bytes_ += bytes;
			}
			void admit_operation(operation_observation value)
			{
				if (ast_enumerations_.empty())
					return;
				auto& scope = ast_enumerations_.back();
				if (!declaration_population_admitted(scope.declaration) ||
					(scope.declaration->isImplicit() && !written_lambda(scope.declaration)))
					return;
				if (scope.operations.size() >= 1'000'000U)
					fail("compiler-actions", "admission-count-limit");
				retain_operation_bytes(sizeof(operation_observation) + value.kind.size() +
									   value.origin.size() + value.node.size() + value.body.size() +
									   256U);
				scope.operations.push_back(std::move(value));
			}
			void observe_ast_operations(const clang::Stmt* statement)
			{
				if (!ast_enumerations_.empty() && llvm::isa<clang::AsmStmt>(statement))
				{
					ast_enumerations_.back().operation_frontier = true;
					output_.unresolved.push_back({"operation.inline-assembly-frontier",
												  current_function_,
												  "supply-compiler-assembly-effects"});
				}
				const auto* expression = llvm::dyn_cast<clang::Expr>(statement);
				if (!expression || !admitted(expression->getExprLoc()))
					return;
				for (auto kind : ast_operation_kinds(statement))
				{
					operation_observation value;
					value.expression = expression;
					value.source = expression->getSourceRange();
					value.kind = std::move(kind);
					value.origin = "ast";
					value.evaluation = evaluation_context(*expression);
					if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expression))
						value.target = call->getDirectCallee();
					else if (const auto* construct =
								 llvm::dyn_cast<clang::CXXConstructExpr>(expression))
						value.target = construct->getConstructor();
					else if (const auto* allocation = llvm::dyn_cast<clang::CXXNewExpr>(expression))
					{
						value.target = value.kind == "allocation" ? allocation->getOperatorNew()
																  : allocation->getOperatorDelete();
						if (value.kind == "initialization_failure_deallocation")
							value.outcome = "initialization_failure";
					}
					else if (const auto* deletion =
								 llvm::dyn_cast<clang::CXXDeleteExpr>(expression))
					{
						value.target = deletion->getOperatorDelete();
						value.object_expression = deletion->getArgument();
					}
					else if (const auto* cast = llvm::dyn_cast<clang::ImplicitCastExpr>(expression))
						value.object_expression = cast->getSubExpr();
					else if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression))
						value.object_expression = binary->getLHS();
					else if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression))
						value.object_expression = unary->getSubExpr();
					else if (const auto* atomic = llvm::dyn_cast<clang::AtomicExpr>(expression))
						value.object_expression = atomic->getPtr();
					if (value.kind == "type_use")
					{
						value.type = expression->getType();
						value.type_use_kind = "temporary";
						value.object_expression = expression;
					}
					admit_operation(std::move(value));
				}
			}
			void declaration_type_use_flags(const clang::NamedDecl& declaration,
											std::set<std::string, std::less<>>& flags_value)
			{
				flags_value.emplace("finite_type_use_admission_v1");
				if (llvm::isa<clang::FunctionDecl>(declaration))
					flags_value.emplace("type_use_return_type");
				else if (llvm::isa<clang::ParmVarDecl>(declaration))
					flags_value.emplace("type_use_parameter");
				else if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(&declaration);
						 variable && variable->isLocalVarDecl())
					flags_value.emplace("type_use_local");
			}
			void observe_declaration_type_use(const clang::NamedDecl& declaration)
			{
				operation_observation value;
				value.origin = "declaration";
				value.kind = "type_use";
				value.evaluation = "declarative";
				value.source = declaration.getSourceRange();
				if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(&declaration))
				{
					value.type = function->getReturnType();
					value.type_use_kind = "return_type";
					value.subject_index = 0U;
				}
				else if (const auto* parameter = llvm::dyn_cast<clang::ParmVarDecl>(&declaration))
				{
					value.object = parameter;
					value.type = parameter->getType();
					value.type_use_kind = "parameter";
					value.subject_index = parameter->getFunctionScopeIndex();
					value.parameter_has_default_argument = parameter->hasDefaultArg();
				}
				else if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(&declaration);
						 variable && variable->isLocalVarDecl())
				{
					value.object = variable;
					value.type = variable->getType();
					value.type_use_kind = "local";
					if (variable->hasLocalStorage() && !variable->getType()->isReferenceType() &&
						!ast_enumerations_.empty())
						++ast_enumerations_.back().automatic_storage_count;
				}
				else
					return;
				admit_operation(std::move(value));
				if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(&declaration))
					observe_capture_type_uses(*function);
			}
			static std::uint64_t function_capture_count(const clang::FunctionDecl& function)
			{
				const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
				return method && written_lambda(method) ? method->getParent()->capture_size() : 0U;
			}
			void observe_capture_type_uses(const clang::FunctionDecl& function)
			{
				if (!written_lambda(&function))
					return;
				const auto* method = llvm::cast<clang::CXXMethodDecl>(&function);
				const auto count = function_capture_count(function);
				if (count > 100000U)
					fail("compiler-actions", "capture-count-limit");
				retain_operation_bytes(static_cast<std::size_t>(count) * 128U);
				llvm::DenseMap<const clang::ValueDecl*, clang::FieldDecl*> fields;
				clang::FieldDecl* this_capture{};
				method->getParent()->getCaptureFields(fields, this_capture);
				std::uint64_t index{};
				for (const auto& capture : method->getParent()->captures())
				{
					operation_observation value;
					value.kind = "type_use";
					value.origin = "declaration";
					value.evaluation = "declarative";
					value.type_use_kind = "capture";
					value.subject_index = index++;
					value.source = {capture.getLocation(), capture.getLocation()};
					if (capture.capturesVariable())
					{
						value.object = capture.getCapturedVar();
						value.type = value.object->getType();
					}
					else if (capture.capturesThis() && this_capture)
						value.type = this_capture->getType();
					admit_operation(std::move(value));
				}
			}
			static bool implicit_cfg_operation(const clang::CFGElement& value)
			{
				return value.getAs<clang::CFGImplicitDtor>().has_value() ||
					value.getAs<clang::CFGCleanupFunction>().has_value() ||
					value.getAs<clang::CFGNewAllocator>().has_value() ||
					value.getAs<clang::CFGConstructor>().has_value();
			}
			static std::uint64_t cfg_operation_admission_count(const clang::CFGBlock& block)
			{
				return static_cast<std::uint64_t>(
					std::ranges::count_if(block,
										  [](const auto& value)
										  {
											  return implicit_cfg_operation(value);
										  }));
			}
			void observe_cfg_operations(const clang::CFG& cfg,
										const std::map<unsigned, std::string>& nodes,
										const std::string& body)
			{
				for (const auto* block : cfg)
				{
					std::uint64_t position{};
					for (const auto& element : *block)
					{
						operation_observation value;
						value.origin = "cfg";
						value.evaluation = "potentially_evaluated";
						value.node = nodes.at(block->getBlockID());
						value.body = body;
						value.element_index = position++;
						value.element_kind = element.getAs<clang::CFGConstructor>() ? "constructor"
							: element.getAs<clang::CFGNewAllocator>() ? "new_allocator"
																	  : "statement";
						value.implicit_cfg = implicit_cfg_operation(element);
						if (const auto allocator_element = element.getAs<clang::CFGNewAllocator>())
						{
							value.kind = "allocation";
							value.expression = allocator_element->getAllocatorExpr();
							value.target = allocator_element->getAllocatorExpr()->getOperatorNew();
						}
						else if (const auto destruction = element.getAs<clang::CFGImplicitDtor>())
						{
							value.kind = "destruction";
							value.target = destruction->getDestructorDecl(unit_.ast());
							if (const auto automatic = element.getAs<clang::CFGAutomaticObjDtor>())
							{
								value.object = automatic->getVarDecl();
								value.source = value.object->getSourceRange();
								value.element_kind = "automatic_object_dtor";
							}
							else if (const auto deletion = element.getAs<clang::CFGDeleteDtor>())
							{
								value.expression = deletion->getDeleteExpr();
								value.object_expression = deletion->getDeleteExpr()->getArgument();
								value.element_kind = "delete_dtor";
							}
							else if (const auto member = element.getAs<clang::CFGMemberDtor>())
							{
								value.object = member->getFieldDecl();
								value.source = value.object->getSourceRange();
								value.element_kind = "member_dtor";
							}
							else if (const auto base = element.getAs<clang::CFGBaseDtor>())
							{
								value.base = base->getBaseSpecifier();
								value.type = value.base->getType();
								value.source = value.base->getSourceRange();
								value.element_kind = "base_dtor";
							}
							else if (const auto temporary =
										 element.getAs<clang::CFGTemporaryDtor>())
							{
								value.expression = temporary->getBindTemporaryExpr();
								value.object_expression = value.expression;
								value.element_kind = "temporary_dtor";
							}
						}
						else if (const auto cleanup = element.getAs<clang::CFGCleanupFunction>())
						{
							value.kind = "cleanup_function";
							value.target = cleanup->getFunctionDecl();
							value.object = cleanup->getVarDecl();
							value.source = value.object->getSourceRange();
							value.element_kind = "cleanup_function";
						}
						else if (const auto statement = element.getAs<clang::CFGStmt>())
						{
							value.expression = llvm::dyn_cast<clang::Expr>(statement->getStmt());
							if (!value.expression)
								continue;
							const auto kinds = ast_operation_kinds(value.expression);
							if (kinds.empty())
								continue;
							// Each original CFG view retains its actual element. The AST view owns
							// conditional cleanup and declarative type roles, which are not another
							// CFG call.
							value.kind = kinds.front();
							if (const auto* call =
									llvm::dyn_cast<clang::CallExpr>(value.expression))
								value.target = call->getDirectCallee();
							else if (const auto* construct =
										 llvm::dyn_cast<clang::CXXConstructExpr>(value.expression))
								value.target = construct->getConstructor();
							else if (const auto* allocation =
										 llvm::dyn_cast<clang::CXXNewExpr>(value.expression))
								value.target = allocation->getOperatorNew();
							else if (const auto* deletion =
										 llvm::dyn_cast<clang::CXXDeleteExpr>(value.expression))
							{
								value.target = deletion->getOperatorDelete();
								value.object_expression = deletion->getArgument();
							}
							else if (const auto* cast =
										 llvm::dyn_cast<clang::ImplicitCastExpr>(value.expression))
								value.object_expression = cast->getSubExpr();
							else if (const auto* binary =
										 llvm::dyn_cast<clang::BinaryOperator>(value.expression))
								value.object_expression = binary->getLHS();
							else if (const auto* unary =
										 llvm::dyn_cast<clang::UnaryOperator>(value.expression))
								value.object_expression = unary->getSubExpr();
							if (value.kind == "type_use")
							{
								value.type = value.expression->getType();
								value.type_use_kind = "temporary";
								value.object_expression = value.expression;
							}
						}
						else
							continue;
						if (value.expression)
							value.source = value.expression->getSourceRange();
						admit_operation(std::move(value));
					}
				}
			}
			std::optional<std::string> original_syntax(const clang::Stmt* expression)
			{
				if (!expression)
					return std::nullopt;
				const auto found = syntax_nodes_.find(expression);
				if (found == syntax_nodes_.end() || found->second.size() != 1U)
					return std::nullopt;
				return *found->second.begin();
			}
			void operation_target_fields(fields& value, const clang::FunctionDecl* target)
			{
				if (!target)
					return;
				target = target->getCanonicalDecl();
				const auto actual_type = canonical_type(target->getType());
				const auto& structure = type_structure_.at(actual_type);
				const auto actual_usr = usr(target);
				const auto actual_entity = entity(target);
				if (!actual_entity.empty())
					value.emplace("target", id("cc_entity_id", actual_entity));
				const auto kind = llvm::isa<clang::CXXConstructorDecl>(target) ? "constructor"
					: llvm::isa<clang::CXXDestructorDecl>(target)			   ? "destructor"
					: llvm::isa<clang::CXXConversionDecl>(target)			   ? "conversion"
					: llvm::isa<clang::CXXMethodDecl>(target)				   ? "method"
																			   : "function";
				value.emplace("target_kind", symbol("cc.entity-kind/1", kind));
				value.emplace("target_signature_state",
							  symbol("cc.target-signature-state/1",
									 !actual_usr.empty() && structure.state == "complete"
										 ? "complete"
										 : "partial"));
				value.emplace("target_signature_profile",
							  sdk::detached_cell::utf8("clang22-original-target-signature/1"));
				if (!actual_usr.empty())
				{
					retain_operation_bytes(actual_usr.size());
					std::vector<std::byte> bytes;
					bytes.reserve(actual_usr.size());
					for (auto ch : actual_usr)
						bytes.push_back(static_cast<std::byte>(ch));
					value.emplace("target_usr", sdk::detached_cell::bytes(std::move(bytes)));
				}
				value.emplace("target_canonical_type", id("cc_type_id", actual_type));
				value.emplace("target_canonical_type_digest", digest_value(structure.digest));
				value.emplace("target_canonical_type_profile",
							  sdk::detached_cell::utf8("clang22-structural-type/1"));
				value.emplace("target_structural_signature_digest",
							  digest_value(take(sdk::semantic_digest(
								  "cc.entity.structural-signature.v1",
								  target->getType().getCanonicalType().getAsString()))));
				value.emplace(
					"target_language",
					symbol("build.language/1", unit_.ast().getLangOpts().CPlusPlus ? "c++" : "c"));
				value.emplace("target_linkage",
							  symbol("cc.linkage/1",
									 target->isExternallyVisible() ? "external" : "internal"));
				value.emplace(
					"target_module_domain",
					sdk::detached_cell::utf8(target->getOwningModule()
												 ? target->getOwningModule()->getFullModuleName()
												 : "<none>"));
			}
			static bool magic_literal(const clang::Stmt* statement)
			{
				return llvm::isa_and_nonnull<clang::IntegerLiteral,
											 clang::FloatingLiteral,
											 clang::StringLiteral>(statement);
			}
			void identifier_fields(fields& value, const clang::NamedDecl& declaration)
			{
				const auto* context = declaration.getDeclContext();
				const auto scope = context->isTranslationUnit() ? "translation_unit"
					: context->isNamespace()					? "namespace"
					: context->isRecord()						? "record"
					: context->isFunctionOrMethod()				? "function"
																: "unknown";
				const auto* function = llvm::dyn_cast<clang::FunctionDecl>(&declaration);
				if (!function && context->isFunctionOrMethod())
					function = llvm::dyn_cast<clang::FunctionDecl>(
						clang::Decl::castFromDeclContext(context));
				if (const auto function_id = entity(function); !function_id.empty())
					value.emplace("identifier_function", id("cc_entity_id", function_id));
				std::string kind;
				if (llvm::isa<clang::CXXConstructorDecl>(declaration))
					kind = "constructor";
				else if (llvm::isa<clang::CXXDestructorDecl>(declaration))
					kind = "destructor";
				else if (llvm::isa<clang::CXXConversionDecl>(declaration))
					kind = "conversion";
				else if (declaration.getDeclName().getNameKind() ==
						 clang::DeclarationName::CXXOperatorName)
					kind = "operator";
				else if (declaration.getIdentifier())
					kind = "identifier";
				else if (declaration.getDeclName().isEmpty())
					kind = "anonymous";
				else
					kind = "unknown";
				std::string state = scope == std::string_view{"unknown"} || kind == "unknown"
					? "unsupported"
					: "complete";
				if (kind == "identifier")
				{
					const auto location = declaration.getLocation();
					const auto source = span({location, location}, "declaration");
					if (source)
					{
						value.emplace(
							"identifier_name",
							sdk::detached_cell::utf8(declaration.getIdentifier()->getName().str()));
						value.emplace("identifier_source", id("source_span_id", *source));
					}
					else
						state = "partial";
				}
				if (function && entity(function).empty())
					state = "partial";
				value.emplace("identifier_scope", symbol("cc.identifier-scope/1", scope));
				value.emplace("identifier_kind", symbol("cc.identifier-kind/1", std::move(kind)));
				value.emplace("identifier_state",
							  symbol("cc.identifier-state/1", std::move(state)));
				value.emplace(
					"identifier_profile",
					sdk::detached_cell::utf8("clang22-original-declaration-identifier/1"));
			}
			void literal_fields(fields& value, const clang::Stmt& statement)
			{
				if (const auto* integer = llvm::dyn_cast<clang::IntegerLiteral>(&statement))
				{
					if (integer->getValue().getBitWidth() > 4096U)
						fail("syntax-constant", "integer-width-limit");
					const bool signed_value = integer->getType()->isSignedIntegerType();
					llvm::SmallString<128> spelling;
					integer->getValue().toString(spelling, 10, signed_value);
					value.emplace("integer_value", sdk::detached_cell::utf8(spelling.str().str()));
					value.emplace(
						"integer_bit_width",
						sdk::detached_cell::unsigned_integer(integer->getValue().getBitWidth()));
					value.emplace("integer_signed", sdk::detached_cell::boolean(signed_value));
				}
				if (llvm::isa<clang::CXXNullPtrLiteralExpr, clang::GNUNullExpr>(statement))
					value.emplace("constant_null", sdk::detached_cell::boolean(true));
				if (const auto* string = llvm::dyn_cast<clang::StringLiteral>(&statement))
				{
					const auto bytes = string->getBytes();
					retain_population_bytes(bytes.size() + 512U);
					std::vector<std::byte> decoded;
					decoded.reserve(bytes.size());
					for (const char byte : bytes)
						decoded.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
					const std::string encoding = string->isOrdinary() ? "ordinary"
						: string->isUTF8()							  ? "utf8"
						: string->isUTF16()							  ? "utf16"
						: string->isUTF32()							  ? "utf32"
						: string->isWide()							  ? "wide"
																	  : "unknown";
					value.emplace("literal_value_bytes",
								  sdk::detached_cell::bytes(std::move(decoded)));
					value.emplace(
						"literal_value_profile",
						sdk::detached_cell::utf8("clang22-original-string-literal-value/1"));
					value.emplace("literal_value_state",
								  symbol("cc.syntax-observation-state/1",
										 encoding == "unknown" ? "partial" : "complete"));
					value.emplace("literal_encoding",
								  symbol("cc.string-literal-encoding/1", encoding));
					value.emplace(
						"literal_element_width_bits",
						sdk::detached_cell::unsigned_integer(string->getCharByteWidth() * 8U));
				}
				if (!magic_literal(&statement))
					return;
				const clang::NamedDecl* owner{};
				std::string context = "ordinary";
				for (auto current = declaration_stack_.rbegin();
					 current != declaration_stack_.rend();
					 ++current)
				{
					if (llvm::isa<clang::FunctionDecl>(*current))
						break;
					const clang::Expr* initializer{};
					if (const auto* constant = llvm::dyn_cast<clang::EnumConstantDecl>(*current))
					{
						initializer = constant->getInitExpr();
						if (initializer &&
							std::ranges::find(statements_, initializer) != statements_.end())
						{
							owner = constant;
							context = "enum_initializer";
							break;
						}
					}
					else if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(*current))
					{
						initializer = variable->getInit();
						if (initializer &&
							std::ranges::find(statements_, initializer) != statements_.end())
						{
							owner = variable;
							if (variable->getType().isConstQualified() || variable->isConstexpr())
								context = "const_initializer";
							break;
						}
					}
				}
				std::string state = "complete";
				if (owner)
				{
					const auto declaration = original_declarations_.find(owner);
					if (declaration == original_declarations_.end())
						state = "partial";
					else
						value.emplace("literal_declaration",
									  id("cc_declaration_id", declaration->second));
				}
				value.emplace("literal_context",
							  symbol("cc.literal-context/1", std::move(context)));
				value.emplace("literal_context_state",
							  symbol("cc.identifier-state/1", std::move(state)));
				value.emplace("literal_context_profile",
							  sdk::detached_cell::utf8("clang22-original-literal-context/1"));
			}
			void finish_literal_scope(ast_enumeration& scope)
			{
				for (const auto* literal : scope.literal_admissions)
				{
					const auto found = syntax_nodes_.find(literal);
					if (found == syntax_nodes_.end() || found->second.size() != 1U)
						scope.literal_frontier = true;
					else
					{
						retain_population_bytes(128U);
						scope.literal_ids.insert(*found->second.begin());
					}
				}
				if (const auto activated = activation_contexts_.find(scope.declaration);
					activated != activation_contexts_.end())
					for (const auto& [expression, contexts] : activated->second)
						if (magic_literal(expression) &&
							!scope.literal_admissions.contains(expression))
							scope.literal_frontier = true;
			}
			void literal_scope_fields(fields& value, const ast_enumeration& scope)
			{
				value.emplace(
					"literal_count",
					sdk::detached_cell::unsigned_integer(scope.literal_admissions.size()));
				value.emplace("literal_ids", flags("syntax_node_id", scope.literal_ids));
				value.emplace("literal_state",
							  symbol("cc.literal-enumeration-state/1",
									 !scope.body ? "unavailable"
										 : scope.literal_frontier || !scope.declaration ||
											 scope.declaration->hasSkippedBody() ||
											 unit_.ast().getDiagnostics().hasErrorOccurred()
										 ? "partial"
										 : "complete"));
				value.emplace("literal_profile",
							  sdk::detached_cell::utf8("clang22-function-literal-occurrences/1"));
			}
			std::string conversion_preservation(const clang::CastExpr& cast)
			{
				if (cast.isTypeDependent() || cast.isValueDependent())
					return "unknown";
				if (cast.getCastKind() == clang::CK_NoOp ||
					cast.getCastKind() == clang::CK_LValueToRValue)
					return "preserves";
				if (cast.getCastKind() == clang::CK_IntegralCast)
				{
					const auto from = cast.getSubExpr()->getType(), to = cast.getType();
					if (from->isEnumeralType() || to->isEnumeralType() || !from->isIntegerType() ||
						!to->isIntegerType())
						return "unknown";
					const auto from_width = unit_.ast().getIntWidth(from),
							   to_width = unit_.ast().getIntWidth(to);
					const bool from_signed = from->isSignedIntegerType(),
							   to_signed = to->isSignedIntegerType();
					if ((from_signed == to_signed && to_width >= from_width) ||
						(!from_signed && to_signed && to_width > from_width))
						return "preserves";
					return "may_change";
				}
				return "unknown";
			}
			void finish_syntax_bindings()
			{
				for (auto& binding : pending_syntax_bindings_)
				{
					const auto& descriptor = cc::relations::syntax_node::descriptor();
					const auto bind = [&](std::string_view name, const clang::Stmt* expression)
					{
						if (!expression)
							return false;
						if (const auto node = original_syntax(expression))
						{
							binding.row.cells.insert_or_assign(descriptor.id + "." +
																   std::string{name},
															   id("syntax_node_id", *node, true));
							return true;
						}
						return false;
					};
					if (const auto* cast = llvm::dyn_cast<clang::CastExpr>(binding.statement))
					{
						auto state =
							symbol("cc.flow-binding-state/1",
								   bind("operand", cast->getSubExpr()) ? "complete" : "unknown");
						state.type.optional = true;
						binding.row.cells.insert_or_assign(descriptor.id + ".conversion_state",
														   std::move(state));
					}
					else if (const auto* unary =
								 llvm::dyn_cast<clang::UnaryOperator>(binding.statement))
						bind("operand", unary->getSubExpr());
					if (const auto* binary =
							llvm::dyn_cast<clang::BinaryOperator>(binding.statement))
					{
						bind("left_operand", binary->getLHS());
						bind("right_operand", binary->getRHS());
					}
					if (const auto* index =
							llvm::dyn_cast<clang::ArraySubscriptExpr>(binding.statement))
					{
						bind("base_expression", index->getBase());
						bind("index_expression", index->getIdx());
					}
					std::set<const clang::Stmt*> children;
					std::set<std::string, std::less<>> child_ids;
					bool complete = true;
					for (const auto* child : binding.statement->children())
					{
						if (!child || children.contains(child))
							continue;
						retain_population_bytes(256U);
						children.insert(child);
						if (const auto node = original_syntax(child))
							child_ids.insert(*node);
						else
							complete = false;
					}
					fields additions{
						{"child_count", sdk::detached_cell::unsigned_integer(children.size())},
						{"child_ids", flags("syntax_node_id", std::move(child_ids))},
						{"child_state",
						 symbol("cc.literal-enumeration-state/1",
								complete ? "complete" : "partial")},
						{"child_profile",
						 sdk::detached_cell::utf8("clang22-original-statement-children/1")}};
					for (auto& [name, cell] : additions)
					{
						cell.type = take(descriptor.column(descriptor.id + "." + name)).type;
						binding.row.cells.insert_or_assign(descriptor.id + "." + name,
														   std::move(cell));
					}
					check(sdk::validate_row(descriptor, binding.row));
					append(std::move(binding.row));
				}
				pending_syntax_bindings_.clear();
			}
			void finish_cfg_conditions()
			{
				for (auto& binding : pending_cfg_terminators_)
				{
					const auto& descriptor = cc::relations::cfg_node::descriptor();
					const auto expression = original_syntax(binding.condition);
					if (expression)
						binding.row.cells.insert_or_assign(descriptor.id + ".terminator",
														   id("syntax_node_id", *expression, true));
					auto state = symbol("cc.flow-binding-state/1",
										!binding.condition || expression ? "complete" : "unknown");
					state.type.optional = true;
					binding.row.cells.insert_or_assign(descriptor.id + ".terminator_state",
													   std::move(state));
					check(sdk::validate_row(descriptor, binding.row));
					append(std::move(binding.row));
				}
				pending_cfg_terminators_.clear();
				for (auto& binding : pending_cfg_conditions_)
				{
					const auto& descriptor = cc::relations::cfg_edge::descriptor();
					const auto condition = original_syntax(binding.condition);
					if (condition)
						binding.row.cells.insert_or_assign(descriptor.id + ".condition",
														   id("syntax_node_id", *condition, true));
					auto state = symbol("cc.flow-binding-state/1",
										!binding.condition || condition ? "complete" : "unknown");
					state.type.optional = true;
					binding.row.cells.insert_or_assign(descriptor.id + ".condition_state",
													   std::move(state));
					if (const auto outcome = original_syntax(binding.outcome))
						binding.row.cells.insert_or_assign(descriptor.id + ".outcome_expression",
														   id("syntax_node_id", *outcome, true));
					check(sdk::validate_row(descriptor, binding.row));
					append(std::move(binding.row));
				}
				pending_cfg_conditions_.clear();
			}
			void automatic_storage_fields(fields& value,
										  const clang::VarDecl& object,
										  const std::string& function)
			{
				const auto type = object.getType();
				const auto duration = object.getTLSKind() != clang::VarDecl::TLS_None ? "thread"
					: object.hasLocalStorage()										  ? "automatic"
																					  : "static";
				std::string state = "complete";
				if (type.isNull() || object.isInvalidDecl() || type->isDependentType() ||
					type->isIncompleteType() || type->isVariablyModifiedType() ||
					type->isSizelessType())
					state = "partial";
				else if (type->isReferenceType())
					state = "unsupported";
				else
				{
					const auto size = unit_.ast().getTypeSizeInChars(type).getQuantity();
					const auto alignment = unit_.ast().getDeclAlign(&object).getQuantity();
					if (size < 0 || alignment <= 0)
						state = "partial";
					else
					{
						value.emplace(
							"storage_size_bytes",
							sdk::detached_cell::unsigned_integer(static_cast<std::uint64_t>(size)));
						value.emplace("storage_alignment_bytes",
									  sdk::detached_cell::unsigned_integer(
										  static_cast<std::uint64_t>(alignment)));
					}
				}
				value.emplace("storage_duration", symbol("cc.storage-duration/1", duration));
				value.emplace("storage_state", symbol("cc.identifier-state/1", std::move(state)));
				value.emplace(
					"storage_profile",
					sdk::detached_cell::utf8("clang22-declared-automatic-object-layout/1"));
				value.emplace(
					"storage_target_triple",
					sdk::detached_cell::utf8(unit_.ast().getTargetInfo().getTriple().str()));
				if (const auto context = original_abi_contexts_.find(function);
					context != original_abi_contexts_.end())
					value.emplace("storage_abi_context", digest_value(context->second));
			}
			void automatic_storage_scope_fields(fields& value, const ast_enumeration& scope)
			{
				value.emplace("automatic_storage_count",
							  sdk::detached_cell::unsigned_integer(scope.automatic_storage_count));
				value.emplace("automatic_storage_ids",
							  flags("operation_id", scope.automatic_storage_ids));
				value.emplace("automatic_storage_state",
							  symbol("cc.operation-enumeration-state/1",
									 !scope.body ? "unavailable"
										 : scope.automatic_storage_frontier ||
											 scope.declaration->hasSkippedBody() ||
											 scope.automatic_storage_ids.size() !=
												 scope.automatic_storage_count ||
											 unit_.ast().getDiagnostics().hasErrorOccurred()
										 ? "partial"
										 : "complete"));
				value.emplace(
					"automatic_storage_profile",
					sdk::detached_cell::utf8("clang22-written-automatic-local-storage/1"));
			}
			void operation_scope_fields(fields& value, const ast_enumeration& scope)
			{
				value.emplace("operation_count",
							  sdk::detached_cell::unsigned_integer(scope.operations.size()));
				value.emplace("operation_ids", flags("operation_id", scope.operation_ids));
				value.emplace("operation_state",
							  symbol("cc.operation-enumeration-state/1",
									 scope.operation_ids.size() == scope.operations.size() &&
											 !scope.operation_frontier
										 ? "complete"
										 : "partial"));
				value.emplace("operation_profile",
							  sdk::detached_cell::utf8("clang22-function-compiler-actions/1"));
			}
			void finish_operations(ast_enumeration& scope)
			{
				std::uint64_t target_index{};
				std::map<std::uint64_t, std::size_t> operation_target_slots;
				for (const auto& observation : scope.operations)
				{
					const bool callable_stage = observation.kind == "invocation" ||
						observation.kind == "construction" || observation.kind == "allocation" ||
						observation.kind == "deallocation" ||
						observation.kind == "initialization_failure_deallocation" ||
						observation.kind == "destruction" || observation.kind == "cleanup_function";
					if (callable_stage &&
						(!observation.expression ||
						 !llvm::isa<clang::CallExpr, clang::CXXConstructExpr>(
							 observation.expression)))
					{
						retain_population_bytes(128U);
						const auto slot = target_slot(
							observation.expression
								? static_cast<const void*>(observation.expression)
								: static_cast<const void*>(scope.declaration),
							observation.expression ? observation.expression->getStmtClassName()
												   : "CFGCallableStage",
							observation.source,
							"callable",
							observation.kind,
							observation.expression ? 0U : target_index,
							observation.target,
							"eligible",
							{},
							scope.declaration,
							observation.expression);
						operation_target_slots.emplace(target_index, slot);
					}
					++target_index;
				}

				if (!declaration_population_admitted(scope.declaration) ||
					(scope.declaration->isImplicit() && !written_lambda(scope.declaration)))
					return;
				const auto declaration = original_declarations_.find(scope.declaration);
				if (declaration == original_declarations_.end())
				{
					scope.operation_frontier = !scope.operations.empty();
					scope.automatic_storage_frontier = scope.automatic_storage_count != 0U;
					target_slot_domain_frontier_ = true;
					return;
				}
				std::string body;
				if (scope.declaration->isDefaulted() || scope.declaration->hasSkippedBody() ||
					(scope.body && !pending_bodies_.contains(scope.declaration)))
				{
					scope.operation_frontier = true;
					target_slot_domain_frontier_ = true;
				}
				if (const auto found = pending_bodies_.find(scope.declaration);
					found != pending_bodies_.end())
				{
					body =
						row_id(make_row(cc::relations::body::descriptor(), found->second), "body");
					const auto& eligibility = found->second.at("eligibility");
					if (std::get<std::string>(*eligibility.value) != "closed")
					{
						scope.operation_frontier = true;
						target_slot_domain_frontier_ = true;
					}
				}
				if (const auto activated = activation_contexts_.find(scope.declaration);
					activated != activation_contexts_.end())
				{
					std::set<std::pair<const clang::Stmt*, std::string>> observed;
					for (const auto& operation : scope.operations)
						if (operation.expression)
						{
							retain_population_bytes(operation.kind.size() + 96U);
							observed.emplace(operation.expression, operation.kind);
						}
					bool omitted{};
					for (const auto& [expression, contexts] : activated->second)
						for (const auto& kind : ast_operation_kinds(expression))
							if (!observed.contains({expression, kind}))
								omitted = true;
					if (omitted)
					{
						target_slot_domain_frontier_ = true;
						scope.operation_frontier = true;
						output_.unresolved.push_back(
							{"operation.default-activation-frontier",
							 scope.function,
							 "supply-original-default-activation-occurrences-and-CFG-bindings"});
					}
				}
				std::uint64_t ordinal{};
				for (const auto& observation : scope.operations)
				{
					auto value = common();
					value.emplace("scope_declaration",
								  id("cc_declaration_id", declaration->second));
					if (!scope.function.empty())
						value.emplace("function", id("cc_entity_id", scope.function));
					if (!body.empty())
						value.emplace("body", id("body_id", body));
					value.emplace("kind", symbol("cc.operation-kind/1", observation.kind));
					value.emplace("origin", symbol("cc.operation-origin/1", observation.origin));
					value.emplace("profile",
								  sdk::detached_cell::utf8("clang22-function-compiler-actions/1"));
					value.emplace("ordinal", sdk::detached_cell::unsigned_integer(ordinal++));
					auto evaluation = observation.evaluation;
					if (observation.origin == "cfg" && observation.expression)
					{
						// Always-add CFG elements can retain an unevaluated original expression.
						// Reuse its actual AST context instead of treating CFG membership as
						// evidence of evaluation.
						const auto context = call_evaluation_.find(observation.expression);
						if (context != call_evaluation_.end())
							evaluation = context->second;
						else if (!observation.implicit_cfg)
							evaluation = "unknown";
					}
					value.emplace("evaluation",
								  symbol("cc.operation-evaluation/1", std::move(evaluation)));
					value.emplace("outcome", symbol("cc.operation-outcome/1", observation.outcome));
					bool complete = true;
					const auto source = span(observation.source, "expression");
					if (source)
						value.emplace("source", id("source_span_id", *source));
					else
						complete = false;
					std::string subject;
					if (observation.expression)
					{
						const auto owner = activation_contexts_.find(scope.declaration);
						const auto actual =
							owner == activation_contexts_.end() ? nullptr : &owner->second;
						const auto activated = actual
							? actual->find(observation.expression)
							: std::map<const clang::Stmt*,
									   std::set<std::pair<std::string, const clang::ValueDecl*>>>::
								  const_iterator{};
						if (actual && activated != actual->end())
						{
							if (activated->second.size() == 1U)
							{
								const auto& context = *activated->second.begin();
								value.emplace(
									"expression_context",
									symbol("cc.operation-expression-context/1", context.first));
								if (const auto original =
										original_declarations_.find(context.second);
									original != original_declarations_.end())
									value.emplace("context_declaration",
												  id("cc_declaration_id", original->second));
							}
							else
								value.emplace(
									"expression_context",
									symbol("cc.operation-expression-context/1", "unknown"));
						}
						else
							value.emplace(
								"expression_context",
								symbol("cc.operation-expression-context/1", "written_scope"));
					}
					if (observation.expression)
					{
						if (const auto expression = original_syntax(observation.expression))
						{
							value.emplace("expression", id("syntax_node_id", *expression));
							subject = *expression;
						}
						else
							complete = false;
						if (const auto call = original_calls_.find(observation.expression);
							call != original_calls_.end() && original_sites_.contains(call->second))
							value.emplace("call", id("cc_call_id", call->second));
					}
					if (!observation.node.empty())
					{
						value.emplace("node", id("cfg_node_id", observation.node));
						value.emplace(
							"element_index",
							sdk::detached_cell::unsigned_integer(*observation.element_index));
						value.emplace("element_kind",
									  symbol("cc.cfg-element-kind/1", observation.element_kind));
					}
					if (!observation.type_use_kind.empty())
						value.emplace("type_use_kind",
									  symbol("cc.type-use-kind/1", observation.type_use_kind));
					if (observation.subject_index)
						value.emplace(
							"subject_index",
							sdk::detached_cell::unsigned_integer(*observation.subject_index));
					if (observation.parameter_has_default_argument)
						value.emplace("parameter_has_default_argument",
									  sdk::detached_cell::boolean(
										  *observation.parameter_has_default_argument));
					const clang::ValueDecl* object = observation.object;
					if (!object && observation.object_expression)
					{
						const auto* base = observation.object_expression->IgnoreParenImpCasts();
						if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(base))
							object = reference->getDecl();
						// A member expression is an actual field subject but does not identify its
						// receiver.
						else if (const auto* member = llvm::dyn_cast<clang::MemberExpr>(base))
							object = llvm::dyn_cast<clang::ValueDecl>(member->getMemberDecl());
					}
					bool object_complete = false;
					if (object)
					{
						if (const auto actual = original_declarations_.find(object);
							actual != original_declarations_.end())
						{
							value.emplace("object_declaration",
										  id("cc_declaration_id", actual->second));
							if (!observation.expression)
								subject = actual->second;
							object_complete = !llvm::isa<clang::FieldDecl>(object);
						}
						const auto actual_entity = entity(object);
						if (!actual_entity.empty())
							value.emplace("object_entity", id("cc_entity_id", actual_entity));
						if (const auto actual_source =
								span(object->getSourceRange(), "declaration"))
							value.emplace("object_source", id("source_span_id", *actual_source));
						value.emplace("object_type",
									  id("cc_type_id", canonical_type(object->getType())));
					}
					else if (!observation.type.isNull())
					{
						value.emplace("object_type",
									  id("cc_type_id", canonical_type(observation.type)));
						if (observation.type_use_kind == "return_type")
						{
							subject = declaration->second;
							object_complete = true;
						}
						else if (!subject.empty() && observation.type_use_kind == "temporary")
							object_complete = true;
					}
					if (observation.object_expression)
					{
						if (const auto actual = original_syntax(observation.object_expression))
						{
							value.emplace("object_expression", id("syntax_node_id", *actual));
							if (observation.type_use_kind == "temporary")
								object_complete = true;
						}
						if (const auto actual_source =
								span(observation.object_expression->getSourceRange(), "expression"))
							value.insert_or_assign("object_source",
												   id("source_span_id", *actual_source));
						value.insert_or_assign(
							"object_type",
							id("cc_type_id",
							   canonical_type(observation.object_expression->getType())));
					}
					if (observation.type_use_kind == "capture" && observation.subject_index)
					{
						subject =
							"actual-capture-slot:" + std::to_string(*observation.subject_index);
						if (!observation.type.isNull())
							value.insert_or_assign(
								"object_type", id("cc_type_id", canonical_type(observation.type)));
					}
					if (observation.type_use_kind == "parameter" && observation.subject_index)
						subject =
							"actual-parameter-slot:" + std::to_string(*observation.subject_index);
					if (observation.base)
					{
						// The actual base descriptor identifies this subobject. Equal byte
						// positions never group unrelated expression occurrences.
						subject = canonical_type(observation.base->getType());
						object_complete = false;
					}
					if (object || observation.object_expression || !observation.type.isNull())
						value.emplace("object_state",
									  symbol("cc.operation-state/1",
											 object_complete ? "complete" : "partial"));
					operation_target_fields(value, observation.target);
					const auto* automatic_object =
						observation.kind == "type_use" && observation.type_use_kind == "local"
						? llvm::dyn_cast_or_null<clang::VarDecl>(observation.object)
						: nullptr;
					const bool admitted_storage = automatic_object &&
						automatic_object->hasLocalStorage() &&
						!automatic_object->getType()->isReferenceType();
					if (automatic_object)
						automatic_storage_fields(value, *automatic_object, scope.function);
					if (!subject.empty())
					{
						std::string preimage;
						for (const auto& component : {declaration->second,
													  observation.kind,
													  observation.type_use_kind,
													  subject})
							preimage += std::to_string(component.size()) + ":" + component;
						if (observation.kind == "destruction" ||
							observation.kind == "cleanup_function")
							preimage += std::to_string(observation.element_kind.size()) + ":" +
								observation.element_kind;
						value.emplace("site",
									  id("operation_site_id",
										 take(sdk::semantic_digest("cc.clang22.operation-site.v1",
																   preimage))));
						value.emplace("site_state",
									  symbol("cc.operation-site-state/1", "complete"));
					}
					else
					{
						value.emplace("site_state", symbol("cc.operation-site-state/1", "partial"));
						complete = false;
					}
					value.emplace(
						"observation_state",
						symbol("cc.operation-state/1", complete ? "complete" : "partial"));
					if (!complete)
					{
						value.emplace("reason",
									  sdk::detached_cell::utf8(
										  "original-action-source-or-identity-frontier"));
						output_.unresolved.push_back(
							{"operation.original-witness-frontier",
							 scope.function,
							 "supply-original-syntax-declaration-and-source-witnesses"});
					}
					auto row = make_row(cc::relations::operation::descriptor(), std::move(value));
					const auto operation = row_id(row, "operation");
					if (const auto slot = operation_target_slots.find(ordinal - 1U);
						slot != operation_target_slots.end())
					{
						auto& original = pending_target_slots_[slot->second];
						if (!original.multiple_operation_views)
						{
							const auto candidate = id("operation_id", operation);
							const auto old = original.value.find("operation");
							if (old != original.value.end() &&
								old->second.canonical_form() != candidate.canonical_form())
							{
								original.multiple_operation_views = true;
								original.value.erase(old);
							}
							else
								original.value.emplace("operation", candidate);
						}
					}
					retain_operation_bytes(operation.size() + row.canonical_form().size() + 128U);
					scope.operation_ids.insert(operation);
					if (admitted_storage)
					{
						retain_population_bytes(operation.size() + 128U);
						scope.automatic_storage_ids.insert(operation);
					}
					retain_population_bytes(row.canonical_form().size() + 128U);
					pending_operation_dispatch_.push_back({std::move(row),
														   observation.expression,
														   observation.target,
														   observation.kind});
				}
			}

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
			bool overrides_method(const clang::CXXMethodDecl* candidate,
								  const clang::CXXMethodDecl* original,
								  std::set<const clang::CXXMethodDecl*>& visited,
								  unsigned depth)
			{
				if (++dispatch_work_ > 2'000'000U || depth > 128U)
					fail("dispatch", "override-graph-work-limit");
				candidate = candidate->getCanonicalDecl();
				if (candidate == original)
					return true;
				if (visited.contains(candidate))
					return false;
				retain_population_bytes(64U);
				visited.insert(candidate);
				for (const auto* parent : candidate->overridden_methods())
					if (overrides_method(parent, original, visited, depth + 1U))
						return true;
				return false;
			}
			fields static_candidates(const clang::CXXMethodDecl& input)
			{
				const auto* method = input.getCanonicalDecl();
				if (const auto found = static_candidates_.find(method);
					found != static_candidates_.end())
				{
					retain_population_bytes(found->second.second);
					return found->second.first;
				}
				std::set<std::string, std::less<>> targets;
				std::uint64_t count = 1U;
				if (const auto original = entity(method); !original.empty())
				{
					retain_population_bytes(original.size() + 128U);
					targets.insert(original);
				}
				for (const auto* candidate : virtual_methods_)
				{
					if (candidate == method)
						continue;
					std::set<const clang::CXXMethodDecl*> visited;
					if (!overrides_method(candidate, method, visited, 0U))
						continue;
					++count;
					if (const auto original = entity(candidate); !original.empty())
					{
						retain_population_bytes(original.size() + 128U);
						targets.insert(original);
					}
				}
				const bool complete =
					count == targets.size() && !unit_.ast().getDiagnostics().hasErrorOccurred();
				std::size_t encoded_bytes{};
				for (const auto& target : targets)
				{
					if (++dispatch_work_ > 2'000'000U ||
						target.size() + 4U > 64U * 1024U * 1024U - encoded_bytes)
						fail("dispatch", "candidate-payload-limit");
					encoded_bytes += target.size() + 4U;
				}
				// Charge the encoded set and the retained cache copy before either
				// allocation. Repeated call-site copies are charged on cache lookup.
				retain_population_bytes(encoded_bytes + 1024U);
				fields values{{"candidate_presence", symbol("cc.candidate-presence/1", "present")},
							  {"candidate_count", sdk::detached_cell::unsigned_integer(count)},
							  {"candidate_targets", flags("cc_entity_id", std::move(targets))},
							  {"candidate_state",
							   symbol("cc.target-slot-enumeration-state/1",
									  complete ? "complete" : "partial")},
							  {"candidate_profile",
							   sdk::detached_cell::utf8(
								   "clang22-materialized-static-override-candidates/1")}};
				const auto copy_bytes = encoded_bytes + 1024U;
				retain_population_bytes(copy_bytes);
				static_candidates_.emplace(method, std::pair{values, copy_bytes});
				return values;
			}
			fields dispatch_fields(const clang::Expr* expression,
								   const clang::FunctionDecl* target,
								   std::string_view action)
			{
				std::string kind = target ? "direct" : "not_applicable", state = "complete";
				const auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(target);
				const auto* call = llvm::dyn_cast_or_null<clang::CallExpr>(expression);
				if (call)
				{
					if (call->isTypeDependent() || call->isValueDependent())
					{
						kind = "dependent";
						state = "partial";
					}
					else if (!target)
					{
						kind = "indirect";
						if (llvm::isa<clang::CXXMemberCallExpr>(call))
							state = "partial"; // A member-pointer invocation does not reveal its
											   // virtual bit.
					}
					else if (method && method->isVirtual())
					{
						const auto* member = llvm::dyn_cast<clang::MemberExpr>(
							call->getCallee()->IgnoreParenImpCasts());
						const auto* member_call = llvm::dyn_cast<clang::CXXMemberCallExpr>(call);
						const auto* operator_call =
							llvm::dyn_cast<clang::CXXOperatorCallExpr>(call);
						const auto* receiver = member_call
							? member_call->getImplicitObjectArgument()
							: operator_call && operator_call->getNumArgs()
							? operator_call->getArg(0)
							: nullptr;
						if (member && !member->performsVirtualDispatch(unit_.ast().getLangOpts()))
							kind = "direct";
						else if (receiver &&
								 method->getDevirtualizedMethod(
									 const_cast<clang::Expr*>(receiver),
									 unit_.ast().getLangOpts().AppleKext))
							kind = "direct";
						else if (receiver)
							kind = "virtual";
						else
						{
							kind = "unknown";
							state = "partial";
						}
					}
				}
				else if (method && method->isVirtual() && action == "destruction" &&
						 llvm::isa_and_nonnull<clang::CXXDeleteExpr>(expression))
				{
					kind = "virtual";
					if (llvm::cast<clang::CXXDeleteExpr>(expression)->isArrayForm())
					{
						kind = "unknown";
						state = "partial";
					}
				}
				fields values{
					{"dispatch_kind", symbol("cc.dispatch-kind/1", kind)},
					{"dispatch_state", symbol("cc.target-slot-observation-state/1", state)},
					{"dispatch_profile",
					 sdk::detached_cell::utf8("clang22-original-call-dispatch/1")}};
				if (kind == "virtual" && method)
					values.merge(static_candidates(*method));
				else
				{
					values.emplace("candidate_presence",
								   symbol("cc.candidate-presence/1",
										  state == "complete" ? "absent" : "unknown"));
					if (state == "complete")
					{
						values.emplace("candidate_count", sdk::detached_cell::unsigned_integer(0U));
						values.emplace("candidate_targets", flags("cc_entity_id", {}));
						values.emplace("candidate_state",
									   symbol("cc.target-slot-enumeration-state/1", "complete"));
						values.emplace("candidate_profile",
									   sdk::detached_cell::utf8(
										   "clang22-materialized-static-override-candidates/1"));
					}
				}
				return values;
			}
			void finish_operation_dispatch()
			{
				const auto& descriptor = cc::relations::operation::descriptor();
				for (auto& binding : pending_operation_dispatch_)
				{
					for (auto& [name, cell] :
						 dispatch_fields(binding.expression, binding.target, binding.kind))
					{
						cell.type = take(descriptor.column(descriptor.id + "." + name)).type;
						binding.row.cells.insert_or_assign(descriptor.id + "." + name,
														   std::move(cell));
					}
					check(sdk::validate_row(descriptor, binding.row));
					append(std::move(binding.row));
				}
				pending_operation_dispatch_.clear();
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
					for (auto& [name, field] : dispatch_fields(expression, target, "call"))
						population.emplace(std::move(name), std::move(field));
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
					// Callee syntax can carry BoundMember/Overload and other compiler
					// placeholders. Those are not object types; asking Clang for their
					// layout reaches an unreachable branch rather than an unknown size.
					if (builtin->isPlaceholderType() || type->isSizelessType())
					{
						structural_complete = false;
						output_.unresolved.push_back({"type.structure-frontier",
													  observations_.compile_unit,
													  "non-object-or-sizeless-builtin"});
					}
					else if (!type->isVoidType() && !type->isIncompleteType() &&
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
				if (const auto builtin = observe_original_builtin_kind(type); !builtin.kind.empty())
				{
					value.emplace("builtin_kind",
								  symbol("cc.builtin-kind/1", std::string{builtin.kind}));
					value.emplace("builtin_profile",
								  sdk::detached_cell::utf8("clang22-original-builtin-type/1"));
					value.emplace(
						"builtin_state",
						symbol("cc.builtin-observation-state/1", std::string{builtin.state}));
				}
				const auto integer = observe_original_integer_representation(unit_.ast(), type);
				value.emplace(
					"integer_profile",
					sdk::detached_cell::utf8("clang22-original-integer-representation/1"));
				value.emplace(
					"integer_state",
					symbol("cc.integer-representation-state/1", std::string{integer.state}));
				if (integer.bit_width && integer.signed_value)
				{
					value.emplace("integer_bit_width",
								  sdk::detached_cell::unsigned_integer(*integer.bit_width));
					value.emplace("integer_signed",
								  sdk::detached_cell::boolean(*integer.signed_value));
				}
				if (!integer.underlying_type.isNull())
					value.emplace("integer_underlying_type",
								  id("cc_type_id", canonical_type(integer.underlying_type)));
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
							 project_abi_observation observation,
							 const clang::RecordDecl* record = nullptr)
			{
				if (kind == "function" && !observation.abi_context.empty())
				{
					if (const auto existing = original_abi_contexts_.find(owner);
						existing != original_abi_contexts_.end() &&
						existing->second != observation.abi_context)
						fail("automatic-storage", "conflicting-original-ABI-context");
					if (!original_abi_contexts_.contains(owner))
						retain_population_bytes(owner.size() + observation.abi_context.size() +
												128U);
					original_abi_contexts_.insert_or_assign(owner, observation.abi_context);
				}
				auto value = common();
				value.emplace("entity", id("cc_entity_id", owner));
				value.emplace("source", id("source_span_id", source));
				value.emplace("kind", symbol("cc.abi-surface-kind/1", std::move(kind)));
				value.emplace(
					"profile",
					symbol("cc.abi-surface-profile/1", "clang22-storage-and-call-interface/1"));
				const auto& target = unit_.ast().getTargetInfo();
				value.emplace("target_data_model_state",
							  symbol("cc.abi-observation-state/1", "complete"));
				value.emplace("target_data_model_profile",
							  sdk::detached_cell::utf8("clang22-original-target-data-model/1"));
				value.emplace("long_width_bits",
							  sdk::detached_cell::unsigned_integer(target.getLongWidth()));
				value.emplace("pointer_width_bits",
							  sdk::detached_cell::unsigned_integer(
								  target.getPointerWidth(clang::LangAS::Default)));
				value.emplace("wchar_width_bits",
							  sdk::detached_cell::unsigned_integer(target.getWCharWidth()));
				value.emplace("plain_char_signed",
							  sdk::detached_cell::boolean(unit_.ast().getLangOpts().CharIsSigned));
				value.emplace(
					"byte_order",
					symbol("cc.target-byte-order/1", target.isBigEndian() ? "big" : "little"));
				if (record && record->isCompleteDefinition())
				{
					const bool packed = record->hasAttr<clang::PackedAttr>();
					const auto* alignment = record->getAttr<clang::MaxFieldAlignmentAttr>();
					const auto maximum_alignment = alignment ? alignment->getAlignment() : 0U;
					value.emplace("packing_state",
								  symbol("cc.abi-observation-state/1",
										 unit_.ast().getDiagnostics().hasErrorOccurred()
											 ? "partial"
											 : "complete"));
					value.emplace(
						"packing_profile",
						sdk::detached_cell::utf8("clang22-original-record-packing-attributes/1"));
					value.emplace("packed_attribute", sdk::detached_cell::boolean(packed));
					value.emplace("maximum_field_alignment_bits",
								  sdk::detached_cell::unsigned_integer(maximum_alignment));
					value.emplace("packing_applied",
								  sdk::detached_cell::boolean(packed || maximum_alignment != 0U));
				}
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
			void finish_flow_expressions()
			{
				for (auto& binding : pending_flow_expressions_)
				{
					bool complete = true;
					for (const auto& [column, statement] :
						 std::initializer_list<std::pair<std::string_view, const clang::Stmt*>>{
							 {"expression", binding.expression},
							 {"value_expression", binding.value}})
					{
						if (!statement)
							continue;
						const auto syntax = syntax_nodes_.find(statement);
						if (syntax == syntax_nodes_.end() || syntax->second.size() != 1U)
						{
							complete = false;
							continue;
						}
						const auto key = "cc.flow_fact.v1." + std::string{column};
						auto cell = id("syntax_node_id", *syntax->second.begin());
						cell.type = binding.row.cells.at(key).type;
						binding.row.cells.insert_or_assign(key, std::move(cell));
					}
					auto state =
						symbol("cc.flow-binding-state/1", complete ? "complete" : "partial");
					state.type = binding.row.cells.at("cc.flow_fact.v1.expression_state").type;
					binding.row.cells.insert_or_assign("cc.flow_fact.v1.expression_state",
													   std::move(state));
					check(sdk::validate_row(cc::relations::flow_fact::descriptor(), binding.row));
					check(sdk::validate_domain_identity(cc::relations::flow_fact::descriptor(),
														binding.row));
					append(std::move(binding.row));
				}
				pending_flow_expressions_.clear();
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
				if (evaluation_context(expression) != "potentially_evaluated")
					return;
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

			struct target_slot_observation
			{
				fields value;
				const clang::Decl* declaration{};
				const clang::Stmt* expression{};
				bool multiple_operation_views{};
			};
			std::uint64_t target_subject(const void* subject)
			{
				if (const auto found = target_subjects_.find(subject);
					found != target_subjects_.end())
					return found->second;
				retain_population_bytes(96U);
				if (target_subjects_.size() >= 1'000'000U)
					fail("target-slots", "subject-limit");
				const auto ordinal = target_subjects_.size();
				target_subjects_.emplace(subject, ordinal);
				return ordinal;
			}
			std::size_t target_slot(const void* subject,
									std::string subject_kind,
									clang::SourceRange range,
									std::string domain,
									std::string relation_kind,
									std::uint64_t index,
									const clang::NamedDecl* target,
									std::string eligibility = "eligible",
									std::string reason = {},
									const clang::Decl* declaration = nullptr,
									const clang::Stmt* expression = nullptr)
			{
				const auto ordinal = target_subject(subject);
				const auto key = std::tuple{ordinal, domain, relation_kind, index};
				retain_population_bytes(1024U + reason.size() + subject_kind.size());
				if (target_slot_admissions_.size() >= 1'000'000U)
					fail("target-slots", "slot-limit");
				auto value = common();
				value.emplace("subject_kind", sdk::detached_cell::utf8(std::move(subject_kind)));
				value.emplace("subject_ordinal", sdk::detached_cell::unsigned_integer(ordinal));
				value.emplace("domain", symbol("cc.target-slot-domain/1", std::move(domain)));
				value.emplace("relation_kind", sdk::detached_cell::utf8(std::move(relation_kind)));
				value.emplace("slot_index", sdk::detached_cell::unsigned_integer(index));
				value.emplace("eligibility", symbol("cc.target-slot-eligibility/1", eligibility));
				const auto source = span(range, "expression");
				if (source)
					value.emplace("source", id("source_span_id", *source));
				const auto original_location =
					unit_.source_manager().getExpansionLoc(range.getBegin());
				if (original_location.isValid() &&
					unit_.source_manager().getFileID(original_location).isValid())
					value.emplace("is_system",
								  sdk::detached_cell::boolean(
									  unit_.source_manager().isInSystemHeader(original_location)));
				if (!current_function_.empty())
					value.emplace("owner", id("cc_entity_id", current_function_));
				const auto target_entity = entity(target);
				if (!target_entity.empty())
					value.emplace("target_entity", id("cc_entity_id", target_entity));
				const auto resolution = eligibility == "excluded"		  ? "not_applicable"
					: eligibility == "eligible" && !target_entity.empty() ? "resolved"
																		  : "unknown";
				value.emplace("resolution", symbol("cc.target-slot-resolution/1", resolution));
				value.emplace(
					"observation_state",
					symbol("cc.target-slot-observation-state/1", source ? "complete" : "partial"));
				value.emplace(
					"profile",
					sdk::detached_cell::utf8("clang22-original-consumer-target-relations/1"));
				if (!source && reason.empty())
					reason = "original-subject-source-unavailable";
				if (resolution == std::string_view{"unknown"} && reason.empty())
					reason = target ? "normalized-target-identity-unavailable"
									: "compiler-target-dependent-or-unavailable";
				if (!reason.empty())
					value.emplace("reason", sdk::detached_cell::utf8(std::move(reason)));
				if (const auto found = target_slot_admissions_.find(key);
					found != target_slot_admissions_.end())
				{
					auto& original = pending_target_slots_[found->second];
					bool conflicting = (original.declaration && declaration &&
										original.declaration != declaration) ||
						(original.expression && expression && original.expression != expression);
					for (const auto name : {"subject_kind",
											"source",
											"owner",
											"target_entity",
											"eligibility",
											"is_system"})
					{
						const auto old = original.value.find(name);
						const auto candidate = value.find(name);
						if (old != original.value.end() && candidate != value.end() &&
							old->second.state == sdk::cell_state::present &&
							candidate->second.state == sdk::cell_state::present &&
							old->second.canonical_form() != candidate->second.canonical_form())
						{
							conflicting = true;
							old->second = sdk::detached_cell::unknown(
								old->second.type, "contradictory-original-subject-view");
						}
					}
					if (conflicting)
					{
						original.value.insert_or_assign(
							"observation_state",
							symbol("cc.target-slot-observation-state/1", "conflicting"));
						original.value.insert_or_assign(
							"resolution", symbol("cc.target-slot-resolution/1", "unknown"));
						original.value.insert_or_assign(
							"reason",
							sdk::detached_cell::utf8("contradictory-original-subject-view"));
					}
					return found->second;
				}
				const auto retained_index = pending_target_slots_.size();
				target_slot_admissions_.emplace(key, retained_index);
				pending_target_slots_.push_back({std::move(value), declaration, expression, false});
				return retained_index;
			}
			void declaration_target_slots(const clang::NamedDecl& declaration)
			{
				// Base types and the declaration's own type belong to the same original
				// nominal-type slot sequence. Resetting it for the own type collides with
				// the first base when both are Record types.
				std::uint64_t type_index{};
				const auto* owner = llvm::dyn_cast_or_null<clang::NamedDecl>(
					clang::Decl::castFromDeclContext(declaration.getDeclContext()));
				const auto ownership_slot =
					target_slot(&declaration,
								declaration.getDeclKindName(),
								declaration.getSourceRange(),
								"ownership",
								"owns",
								0U,
								owner ? &declaration : nullptr,
								owner ? "eligible" : "excluded",
								owner ? "" : "compiler-top-level-unnamed-context",
								&declaration);
				if (owner)
				{
					const auto actual_owner = entity(owner);
					if (!actual_owner.empty())
						pending_target_slots_[ownership_slot].value.insert_or_assign(
							"owner", id("cc_entity_id", actual_owner));
					else
					{
						pending_target_slots_[ownership_slot].value.erase("owner");
						pending_target_slots_[ownership_slot].value.insert_or_assign(
							"observation_state",
							symbol("cc.target-slot-observation-state/1", "partial"));
						pending_target_slots_[ownership_slot].value.insert_or_assign(
							"reason",
							sdk::detached_cell::utf8("original-owner-identity-unavailable"));
					}
				}
				if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(&declaration);
					record && record->isThisDeclarationADefinition())
				{
					std::uint64_t index{};
					std::set<const clang::Type*> ancestors;
					for (const auto& base : record->bases())
					{
						const auto* actual = base.getType()->getAs<clang::RecordType>();
						target_type_slots(declaration, base.getType(), type_index, ancestors, 0U);
						target_slot(&declaration,
									"CXXBaseSpecifier",
									base.getSourceRange(),
									"inheritance",
									"inherits",
									index++,
									actual ? actual->getDecl() : nullptr,
									"eligible",
									{},
									&declaration);
					}
				}
				if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(&declaration))
				{
					std::uint64_t index{};
					for (const auto* overridden : method->overridden_methods())
						target_slot(&declaration,
									"CXXMethodOverride",
									declaration.getSourceRange(),
									"override",
									"overrides",
									index++,
									overridden,
									"eligible",
									{},
									&declaration);
					if (method->getParent()->isDependentContext())
						target_slot(&declaration,
									"DependentMethodOverride",
									declaration.getSourceRange(),
									"override",
									"overrides",
									index,
									nullptr,
									"unknown",
									"dependent-override-enumeration",
									&declaration);
				}
				clang::QualType type;
				if (const auto* value = llvm::dyn_cast<clang::ValueDecl>(&declaration))
					type = value->getType();
				else if (const auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(&declaration))
					type = alias->getUnderlyingType();
				else if (const auto* tag = llvm::dyn_cast<clang::TagDecl>(&declaration))
					type = unit_.ast().getCanonicalTagType(tag);
				std::set<const clang::Type*> ancestors;
				if (!type.isNull())
					target_type_slots(declaration, type, type_index, ancestors, 0U);
			}
			void target_type_slots(const clang::NamedDecl& declaration,
								   clang::QualType input,
								   std::uint64_t& index,
								   std::set<const clang::Type*>& ancestors,
								   unsigned depth)
			{
				if (++target_type_work_ > 2'000'000U || depth > 128U)
					fail("target-slots", "type-traversal-limit");
				if (input.isNull())
				{
					target_slot(&declaration,
								"QualType",
								declaration.getSourceRange(),
								"nominal_type",
								"uses_type",
								index++,
								nullptr,
								"unknown",
								"compiler-type-unavailable",
								&declaration);
					return;
				}
				const auto type = input.getCanonicalType();
				if (ancestors.contains(type.getTypePtr()))
				{
					target_slot(&declaration,
								"QualType",
								declaration.getSourceRange(),
								"nominal_type",
								"uses_type",
								index++,
								nullptr,
								"unknown",
								"recursive-type-position",
								&declaration);
					return;
				}
				retain_population_bytes(64U);
				ancestors.insert(type.getTypePtr());
				const auto child = [&](clang::QualType next)
				{
					target_type_slots(declaration, next, index, ancestors, depth + 1U);
				};
				const auto emit = [&](const clang::NamedDecl* target,
									  std::string eligibility,
									  std::string reason = {})
				{
					target_slot(&declaration,
								type->getTypeClassName(),
								declaration.getSourceRange(),
								"nominal_type",
								"uses_type",
								index++,
								target,
								std::move(eligibility),
								std::move(reason),
								&declaration);
				};
				if (const auto* record = type->getAs<clang::RecordType>())
				{
					emit(record->getDecl(), "eligible");
					if (const auto* specialization =
							llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
								record->getDecl()))
						for (const auto& argument : specialization->getTemplateArgs().asArray())
							target_template_slots(
								declaration, argument, index, ancestors, depth + 1U);
				}
				else if (const auto* enumeration = type->getAs<clang::EnumType>())
					emit(enumeration->getDecl(), "eligible");
				else if (const auto* pointer = type->getAs<clang::PointerType>())
				{
					emit(nullptr, "excluded", "compiler-pointer-wrapper");
					child(pointer->getPointeeType());
				}
				else if (const auto* reference = type->getAs<clang::ReferenceType>())
				{
					emit(nullptr, "excluded", "compiler-reference-wrapper");
					child(reference->getPointeeType());
				}
				else if (const auto* array = unit_.ast().getAsArrayType(type))
				{
					emit(nullptr, "excluded", "compiler-array-wrapper");
					child(array->getElementType());
				}
				else if (const auto* function = type->getAs<clang::FunctionType>())
				{
					emit(nullptr, "excluded", "compiler-function-wrapper");
					child(function->getReturnType());
					if (const auto* proto = llvm::dyn_cast<clang::FunctionProtoType>(function))
						for (const auto parameter : proto->param_types())
							child(parameter);
				}
				else if (const auto* member = type->getAs<clang::MemberPointerType>())
				{
					emit(nullptr, "excluded", "compiler-member-pointer-wrapper");
					child(member->getPointeeType());
					if (const auto* owner = member->getMostRecentCXXRecordDecl())
						child(unit_.ast().getCanonicalTagType(owner));
					else
						emit(nullptr, "unknown", "dependent-member-pointer-owner");
				}
				else if (const auto* atomic = type->getAs<clang::AtomicType>())
				{
					emit(nullptr, "excluded", "compiler-atomic-wrapper");
					child(atomic->getValueType());
				}
				else if (const auto* complex = type->getAs<clang::ComplexType>())
				{
					emit(nullptr, "excluded", "compiler-complex-wrapper");
					child(complex->getElementType());
				}
				else if (const auto* vector = type->getAs<clang::VectorType>())
				{
					emit(nullptr, "excluded", "compiler-vector-wrapper");
					child(vector->getElementType());
				}
				else if (llvm::isa<clang::BuiltinType, clang::BitIntType>(type.getTypePtr()))
					emit(nullptr, "excluded", "compiler-non-nominal-builtin");
				else if (const auto* specialization =
							 type->getAs<clang::TemplateSpecializationType>())
				{
					const auto* templ = llvm::dyn_cast_or_null<clang::ClassTemplateDecl>(
						specialization->getTemplateName().getAsTemplateDecl());
					emit(templ ? templ->getTemplatedDecl() : nullptr,
						 templ ? "eligible" : "unknown",
						 "dependent-template-specialization");
					for (const auto& argument : specialization->template_arguments())
						target_template_slots(declaration, argument, index, ancestors, depth + 1U);
				}
				else
					emit(nullptr,
						 "unknown",
						 type->isDependentType() ? "dependent-type-eligibility"
												 : "unsupported-type-constructor");
				ancestors.erase(type.getTypePtr());
			}
			void target_template_slots(const clang::NamedDecl& declaration,
									   const clang::TemplateArgument& argument,
									   std::uint64_t& index,
									   std::set<const clang::Type*>& ancestors,
									   unsigned depth)
			{
				if (++target_type_work_ > 2'000'000U || depth > 128U)
					fail("target-slots", "template-traversal-limit");
				const auto emit =
					[&](const clang::NamedDecl* target, std::string eligibility, std::string reason)
				{
					target_slot(&declaration,
								"TemplateArgument",
								declaration.getSourceRange(),
								"nominal_type",
								"template_argument",
								index++,
								target,
								std::move(eligibility),
								std::move(reason),
								&declaration);
				};
				const auto child = [&](clang::QualType type)
				{
					target_type_slots(declaration, type, index, ancestors, depth + 1U);
				};
				switch (argument.getKind())
				{
					case clang::TemplateArgument::Type:
						child(argument.getAsType());
						break;
					case clang::TemplateArgument::Declaration:
						emit(argument.getAsDecl(),
							 "eligible",
							 "actual-declaration-template-argument");
						child(argument.getParamTypeForDecl());
						break;
					case clang::TemplateArgument::Integral:
						emit(nullptr, "excluded", "compiler-integral-template-argument");
						child(argument.getIntegralType());
						break;
					case clang::TemplateArgument::NullPtr:
						emit(nullptr, "excluded", "compiler-null-template-argument");
						child(argument.getNullPtrType());
						break;
					case clang::TemplateArgument::StructuralValue:
						emit(nullptr, "excluded", "compiler-structural-value-template-argument");
						child(argument.getStructuralValueType());
						break;
					case clang::TemplateArgument::Expression:
						emit(nullptr, "excluded", "compiler-expression-template-argument");
						child(argument.getAsExpr()->getType());
						break;
					case clang::TemplateArgument::Pack:
						for (const auto& item : argument.pack_elements())
							target_template_slots(declaration, item, index, ancestors, depth + 1U);
						break;
					case clang::TemplateArgument::Template:
					case clang::TemplateArgument::TemplateExpansion:
					{
						const auto* templ =
							argument.getAsTemplateOrTemplatePattern().getAsTemplateDecl();
						const auto* record =
							llvm::dyn_cast_or_null<clang::ClassTemplateDecl>(templ);
						emit(record ? record->getTemplatedDecl() : nullptr,
							 record		 ? "eligible"
								 : templ ? "excluded"
										 : "unknown",
							 "compiler-template-name-argument");
						if (argument.getKind() == clang::TemplateArgument::TemplateExpansion)
							emit(nullptr, "unknown", "dependent-template-expansion");
						break;
					}
					case clang::TemplateArgument::Null:
						emit(nullptr, "unknown", "template-argument-unavailable");
						break;
				}
			}
			void storage_target_slots(const clang::Expr& expression, const clang::NamedDecl* target)
			{
				const auto evaluation = evaluation_context(expression);
				if (evaluation != "potentially_evaluated")
				{
					target_slot(&expression,
								expression.getStmtClassName(),
								expression.getSourceRange(),
								"state_access",
								"accesses",
								0U,
								target,
								evaluation == "unevaluated" ? "excluded" : "unknown",
								"compiler-evaluation-context",
								nullptr,
								&expression);
					return;
				}
				bool read = true, write = false, address = false;
				for (auto iterator = statements_.rbegin(); iterator != statements_.rend();
					 ++iterator)
				{
					if (*iterator == &expression ||
						llvm::isa<clang::ParenExpr, clang::ImplicitCastExpr>(*iterator))
						continue;
					if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(*iterator);
						binary && binary->isAssignmentOp() &&
						binary->getLHS()->IgnoreParenImpCasts() == &expression)
					{
						write = true;
						read = binary->isCompoundAssignmentOp();
					}
					if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(*iterator))
					{
						write = unary->isIncrementDecrementOp();
						address = unary->getOpcode() == clang::UO_AddrOf;
						if (address)
							read = false;
					}
					break;
				}
				if (read)
					target_slot(&expression,
								expression.getStmtClassName(),
								expression.getSourceRange(),
								"state_access",
								"reads",
								0U,
								target,
								"eligible",
								{},
								nullptr,
								&expression);
				if (write)
					target_slot(&expression,
								expression.getStmtClassName(),
								expression.getSourceRange(),
								"state_access",
								"writes",
								0U,
								target,
								"eligible",
								{},
								nullptr,
								&expression);
				if (address)
					target_slot(&expression,
								expression.getStmtClassName(),
								expression.getSourceRange(),
								"state_access",
								"addresses",
								0U,
								target,
								"eligible",
								{},
								nullptr,
								&expression);
			}
			void finish_target_slots(fields& inventory)
			{
				std::set<std::string, std::less<>> ids;
				for (auto& item : pending_target_slots_)
				{
					if (item.declaration)
						if (const auto found = original_declarations_.find(item.declaration);
							found != original_declarations_.end())
							item.value.emplace("declaration",
											   id("cc_declaration_id", found->second));
					if (item.expression)
					{
						if (const auto found = syntax_nodes_.find(item.expression);
							found != syntax_nodes_.end() && found->second.size() == 1U)
							item.value.emplace("expression",
											   id("syntax_node_id", *found->second.begin()));
						if (const auto* expr = llvm::dyn_cast<clang::Expr>(item.expression))
							if (const auto call = original_calls_.find(expr);
								call != original_calls_.end())
								item.value.emplace("call", id("cc_call_id", call->second));
					}
					auto row = make_row(cc::relations::target_resolution_slot::descriptor(),
										std::move(item.value));
					retain_population_bytes(128U);
					ids.insert(row_id(row, "slot"));
					append(std::move(row));
				}
				inventory.emplace(
					"target_slot_count",
					sdk::detached_cell::unsigned_integer(target_slot_admissions_.size()));
				inventory.emplace("target_slot_ids", flags("target_resolution_slot_id", ids));
				const bool complete = ids.size() == target_slot_admissions_.size() &&
					!unit_.ast().getDiagnostics().hasErrorOccurred() &&
					!target_slot_domain_frontier_;
				inventory.emplace("target_slot_state",
								  symbol("cc.target-slot-enumeration-state/1",
										 complete ? "complete" : "partial"));
				inventory.emplace(
					"target_slot_profile",
					sdk::detached_cell::utf8("clang22-original-consumer-target-relations/1"));
				if (!complete)
					output_.unresolved.push_back(
						{"target-slot.inventory-frontier",
						 observations_.compile_unit,
						 "complete-original-parser-and-all-target-domains"});
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
							std::string kind = "other";
							if (element.getAs<clang::CFGConstructor>())
								kind = "constructor";
							else if (element.getAs<clang::CFGNewAllocator>())
								kind = "new_allocator";
							else if (element.getAs<clang::CFGAutomaticObjDtor>())
								kind = "automatic_object_dtor";
							else if (element.getAs<clang::CFGDeleteDtor>())
								kind = "delete_dtor";
							else if (element.getAs<clang::CFGMemberDtor>())
								kind = "member_dtor";
							else if (element.getAs<clang::CFGBaseDtor>())
								kind = "base_dtor";
							else if (element.getAs<clang::CFGTemporaryDtor>())
								kind = "temporary_dtor";
							else if (element.getAs<clang::CFGCleanupFunction>())
								kind = "cleanup_function";
							else if (element.getAs<clang::CFGLifetimeEnds>())
								kind = "lifetime_end";
							else if (element.getAs<clang::CFGLoopExit>())
								kind = "loop_exit";
							else if (element.getAs<clang::CFGStmt>())
								kind = "statement";
							owner_.retain_population_bytes(kind.size() + 96U);
							element_kinds_.emplace(std::make_pair(block->getBlockID(), position),
												   std::move(kind));
							if (const auto statement = element.getAs<clang::CFGStmt>())
								points_.emplace(statement->getStmt(),
												point{block->getBlockID(), position});
							++position;
						}
						if (const auto* terminator = block->getTerminatorStmt())
						{
							points_.emplace(terminator, point{block->getBlockID(), position});
							element_kinds_.emplace(std::make_pair(block->getBlockID(), position),
												   "terminator");
						}
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
							implicit_element_active_ = true;
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
					implicit_element_active_ = false;
					for (const auto* parameter : function_.parameters())
						definition(*parameter,
								   {cfg_.getEntry().getBlockID(), 0},
								   nullptr,
								   true,
								   "parameter_entry");
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
											   variable->hasInit(),
											   variable->hasInit() ? "initialized_declaration"
																   : "uninitialized_declaration");
					if (const auto* operation = llvm::dyn_cast<clang::BinaryOperator>(statement);
						operation && operation->isAssignmentOp())
						write(operation->getLHS(),
							  operation->getRHS(),
							  statement,
							  operation->isCompoundAssignmentOp() ? "compound_assignment"
																  : "assignment");
					if (const auto* operation = llvm::dyn_cast<clang::UnaryOperator>(statement);
						operation && operation->isIncrementDecrementOp())
						write(operation->getSubExpr(), nullptr, statement, "increment_decrement");
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
					row.emplace("kind", symbol("cc.flow-kind/1", kind));
					if (kind == "definition" && !binding_definition_role_.empty())
					{
						row.emplace(
							"definition_role",
							symbol("cc.definition-role/1", std::string{binding_definition_role_}));
						row.emplace(
							"definition_profile",
							sdk::detached_cell::utf8("clang22-original-definition-roles/1"));
					}
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
					const bool event =
						!reaching && !liveness && kind != "frontier" && kind != "def_use";
					const auto* actual_expression = binding_expression_ ? binding_expression_
						: stack_.empty()								? nullptr
																		: stack_.back();
					const auto element = element_kinds_.find({where.block, where.position});
					if (event)
					{
						row.emplace("cfg_binding_profile",
									sdk::detached_cell::utf8("clang22-flow-cfg-elements/1"));
						const bool mapped = element != element_kinds_.end() &&
							(actual_expression || implicit_element_active_);
						row.emplace(
							"cfg_binding_state",
							symbol("cc.flow-binding-state/1", mapped ? "complete" : "unavailable"));
						if (mapped)
						{
							row.emplace("element_index",
										sdk::detached_cell::unsigned_integer(where.position));
							row.emplace("element_kind",
										symbol("cc.cfg-element-kind/1", element->second));
						}
						row.emplace(
							"expression_profile",
							sdk::detached_cell::utf8("clang22-flow-original-expressions/1"));
						row.emplace("expression_state",
									symbol("cc.flow-binding-state/1",
										   actual_expression ? "partial" : "unavailable"));
					}
					row.emplace("guarantee", symbol("cc.flow-guarantee/1", std::move(guarantee)));
					auto fact = make_row(cc::relations::flow_fact::descriptor(), std::move(row));
					const auto result = row_id(fact, "fact");
					owner_.retain_population_bytes(result.size() * 3U + 256U);
					fact_ids_.insert(result);
					if (reaching)
						reaching_points_.insert(result);
					if (liveness)
						liveness_points_.insert(result);
					if (event && actual_expression)
					{
						owner_.retain_population_bytes(fact.canonical_form().size() + 128U);
						owner_.pending_flow_expressions_.push_back(
							{std::move(fact), actual_expression, binding_value_});
					}
					else
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
								bool initialized,
								std::string_view role,
								const clang::Stmt* defining = nullptr)
				{
					const auto* old_expression = binding_expression_;
					const auto* old_value = binding_value_;
					const auto old_role = binding_definition_role_;
					binding_definition_role_ = role;
					binding_expression_ = defining ? defining : initializer;
					binding_value_ = initializer;
					struct restore
					{
						local_flow& owner;
						const clang::Stmt* expression;
						const clang::Expr* value;
						std::string_view role;
						~restore()
						{
							owner.binding_expression_ = expression;
							owner.binding_value_ = value;
							owner.binding_definition_role_ = role;
						}
					} guard{*this, old_expression, old_value, old_role};
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
						   const clang::Stmt* statement,
						   std::string_view role)
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
						definition(
							*reference->getDecl(), *where, initializer, true, role, statement);
					else if (const auto* member = llvm::dyn_cast<clang::MemberExpr>(expression))
						definition(
							*member->getMemberDecl(), *where, initializer, true, role, statement);
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
				std::map<std::pair<unsigned, unsigned>, std::string> element_kinds_;
				const clang::Stmt* binding_expression_{};
				const clang::Expr* binding_value_{};
				std::string_view binding_definition_role_{};
				bool implicit_element_active_{};
				std::set<std::pair<std::string, std::string>> frontiers_;
				std::uint64_t ordinal_{}, order_{};
			};
			void collect_activation_contexts(const clang::FunctionDecl& function)
			{
				using context = std::optional<std::pair<std::string, const clang::ValueDecl*>>;
				std::vector<std::pair<const clang::Stmt*, context>> pending;
				retain_population_bytes(128U);
				pending.push_back({function.getBody(), std::nullopt});
				if (const auto* constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(&function))
					for (const auto* initializer : constructor->inits())
					{
						retain_population_bytes(128U);
						pending.push_back({initializer->getInit(), std::nullopt});
					}
				std::size_t work{};
				while (!pending.empty())
				{
					if (++work > 1000000U)
						fail("resource", "default-context-work-limit");
					const auto [statement, current] = pending.back();
					pending.pop_back();
					if (!statement)
						continue;
					if (current)
					{
						retain_population_bytes(current->first.size() + 192U);
						activation_contexts_[&function][statement].insert(*current);
					}
					if (const auto* argument = llvm::dyn_cast<clang::CXXDefaultArgExpr>(statement))
					{
						retain_population_bytes(128U);
						pending.push_back({argument->getExpr(),
										   std::pair{std::string{"default_argument"},
													 static_cast<const clang::ValueDecl*>(
														 argument->getParam())}});
						continue;
					}
					if (const auto* initializer =
							llvm::dyn_cast<clang::CXXDefaultInitExpr>(statement))
					{
						retain_population_bytes(128U);
						pending.push_back({initializer->getExpr(),
										   std::pair{std::string{"default_initializer"},
													 static_cast<const clang::ValueDecl*>(
														 initializer->getField())}});
						continue;
					}
					if (const auto* lambda = llvm::dyn_cast<clang::LambdaExpr>(statement))
					{
						for (const auto* capture : lambda->capture_inits())
						{
							retain_population_bytes(128U);
							pending.push_back({capture, current});
						}
						continue;
					}
					for (const auto* child : statement->children())
					{
						retain_population_bytes(128U);
						pending.push_back({child, current});
					}
				}
			}

			static bool function_cannot_throw(const clang::FunctionDecl* function)
			{
				if (!function)
					return false;
				if (function->hasAttr<clang::NoThrowAttr>())
					return true;
				const auto* type = function->getType()->getAs<clang::FunctionProtoType>();
				if (!type)
					return false;
				const auto specification = type->getExceptionSpecType();
				return specification == clang::EST_DynamicNone ||
					specification == clang::EST_NoThrow ||
					specification == clang::EST_BasicNoexcept ||
					specification == clang::EST_NoexceptTrue;
			}
			struct cfg_outcome_observation
			{
				bool may_throw{}, has_throw{};
				const clang::Stmt* expression{};
			};
			cfg_outcome_observation cfg_outcome(const clang::CFGBlock& block)
			{
				cfg_outcome_observation outcome;
				outcome.may_throw = block.hasNoReturnElement();
				for (const auto& element : block)
				{
					if (++cfg_facet_work_ > 2'000'000U)
						fail("cfg-facets", "element-work-limit");
					if (const auto destructor = element.getAs<clang::CFGImplicitDtor>())
						outcome.may_throw |=
							!function_cannot_throw(destructor->getDestructorDecl(unit_.ast()));
					else if (const auto cleanup = element.getAs<clang::CFGCleanupFunction>())
						outcome.may_throw |= !function_cannot_throw(cleanup->getFunctionDecl());
					else if (element.getAs<clang::CFGNewAllocator>() ||
							 element.getAs<clang::CFGInitializer>())
						outcome.may_throw = true;
					else if (const auto statement = element.getAs<clang::CFGStmt>())
					{
						const auto* original = statement->getStmt();
						if (llvm::isa<clang::CXXThrowExpr>(original))
						{
							outcome.has_throw = true;
							outcome.expression = original;
						}
						else if (llvm::isa<clang::ReturnStmt>(original) && !outcome.has_throw)
							outcome.expression = original;
						else if (const auto* call = llvm::dyn_cast<clang::CallExpr>(original))
							outcome.may_throw |= !function_cannot_throw(call->getDirectCallee());
						else if (const auto* construction =
									 llvm::dyn_cast<clang::CXXConstructExpr>(original))
							outcome.may_throw |=
								!function_cannot_throw(construction->getConstructor());
						else if (const auto* allocation =
									 llvm::dyn_cast<clang::CXXNewExpr>(original))
							outcome.may_throw |=
								!function_cannot_throw(allocation->getOperatorNew());
						else if (llvm::isa<clang::CXXDeleteExpr,
										   clang::AsmStmt,
										   clang::CXXDefaultArgExpr,
										   clang::CXXDefaultInitExpr,
										   clang::CoawaitExpr,
										   clang::CoyieldExpr,
										   clang::ObjCMessageExpr>(original))
							outcome.may_throw = true;
					}
				}
				return outcome;
			}
			void build_cfg(clang::FunctionDecl& function)
			{
				collect_activation_contexts(function);
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
				options.AddCXXDefaultInitExprInCtors = true;
				options.AddCXXDefaultInitExprInAggregates = true;
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
					const auto implicit_count = cfg_operation_admission_count(*block);
					node.emplace("implicit_operation_count",
								 sdk::detached_cell::unsigned_integer(implicit_count));
					node.emplace("implicit_operation_state",
								 symbol("cc.operation-enumeration-state/1", "complete"));
					node.emplace("implicit_operation_profile",
								 sdk::detached_cell::utf8("clang22-function-compiler-actions/1"));
					node.emplace("terminator_state", symbol("cc.flow-binding-state/1", "unknown"));
					node.emplace("terminator_profile",
								 sdk::detached_cell::utf8("clang22-original-cfg-terminator/1"));
					auto row = make_row(cc::relations::cfg_node::descriptor(), std::move(node));
					nodes.emplace(block->getBlockID(), row_id(row, "node"));
					retain_population_bytes(row.canonical_form().size() + 128U);
					pending_cfg_terminators_.push_back(
						{std::move(row), block->getTerminatorStmt(), nullptr});
				}
				value.emplace("entry", id("cfg_node_id", nodes.at(cfg->getEntry().getBlockID())));
				value.emplace("exit", id("cfg_node_id", nodes.at(cfg->getExit().getBlockID())));
				pending_bodies_.insert_or_assign(&function, std::move(value));
				for (const auto* block : *cfg)
				{
					const auto original_outcome = cfg_outcome(*block);
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
						const bool exceptional = kind == "exception" ||
							kind == "exception_dispatch" || original_outcome.has_throw;
						const bool classified =
							target && (exceptional || !original_outcome.may_throw);
						const auto outcome = !classified ? "unknown"
							: exceptional				 ? "exceptional"
							: original_outcome.expression &&
								llvm::isa<clang::ReturnStmt>(original_outcome.expression) &&
								target == &cfg->getExit()
							? "normal_return"
							: "normal";
						branch.emplace("outcome", symbol("cc.cfg-edge-outcome/1", outcome));
						branch.emplace(
							"outcome_state",
							symbol("cc.flow-binding-state/1", classified ? "complete" : "unknown"));
						branch.emplace("outcome_profile",
									   sdk::detached_cell::utf8("clang22-original-cfg-outcome/1"));
						branch.emplace("kind", symbol("cc.cfg-edge-kind/1", std::move(kind)));
						branch.emplace("ordinal", sdk::detached_cell::unsigned_integer(ordinal++));
						branch.emplace("condition_state",
									   symbol("cc.flow-binding-state/1", "unknown"));
						branch.emplace(
							"condition_profile",
							sdk::detached_cell::utf8("clang22-original-cfg-branch-condition/1"));
						auto original_edge =
							make_row(cc::relations::cfg_edge::descriptor(), std::move(branch));
						retain_population_bytes(original_edge.canonical_form().size() + 128U);
						pending_cfg_conditions_.push_back({std::move(original_edge),
														   block->getTerminatorCondition(),
														   original_outcome.expression});
					}
				}
				observe_cfg_operations(*cfg, nodes, body_id);
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
			std::string project_id_;
			std::map<std::string, sdk::detached_row, std::less<>> original_sites_,
				original_targets_;
			std::map<const clang::FunctionDecl*, fields> pending_function_details_;
			std::map<std::string, std::string, std::less<>> original_abi_contexts_;
			std::set<const clang::CXXMethodDecl*> virtual_methods_;
			std::map<const clang::CXXMethodDecl*, std::pair<fields, std::size_t>>
				static_candidates_;
			std::size_t dispatch_work_{};
			struct operation_dispatch_binding
			{
				sdk::detached_row row;
				const clang::Expr* expression;
				const clang::FunctionDecl* target;
				std::string kind;
			};
			std::vector<operation_dispatch_binding> pending_operation_dispatch_;
			std::map<const clang::FunctionDecl*,
					 std::map<const clang::Stmt*,
							  std::set<std::pair<std::string, const clang::ValueDecl*>>>>
				activation_contexts_;
			std::map<const clang::Decl*, std::string> original_declarations_;
			std::size_t operation_bytes_{};
			std::vector<pp_pending> pending_raw_pp_, pending_evaluated_pp_;
			std::vector<raw_conditional> raw_conditionals_;
			std::map<std::string, bool, std::less<>> raw_structure_partial_, raw_macro_partial_,
				evaluated_macro_partial_, evaluated_expansion_partial_, evaluated_effect_partial_;
			std::map<std::pair<std::string, std::uint64_t>, std::string> raw_token_locations_;
			std::map<std::string, std::pair<std::uint64_t, std::uint64_t>, std::less<>>
				raw_event_bounds_;
			std::map<std::size_t, std::set<bool>> raw_activity_observations_;
			std::map<std::string, bool, std::less<>> raw_activity_partial_, candidate_partial_;
			std::map<std::pair<std::string, std::uint64_t>, std::size_t> raw_directive_locations_;
			struct macro_argument_expression
			{
				std::string node, function, evaluation;
				bool side_effects;
			};
			std::map<std::pair<std::uint32_t, std::uint32_t>,
					 std::vector<macro_argument_expression>>
				macro_argument_expressions_;
			std::uint64_t raw_pp_ordinal_{};
			std::map<std::string, std::uint64_t, std::less<>> raw_pp_admissions_;
			std::map<const clang::Stmt*, std::set<std::string, std::less<>>> syntax_nodes_;
			struct syntax_binding
			{
				sdk::detached_row row;
				const clang::Stmt* statement;
			};
			std::vector<syntax_binding> pending_syntax_bindings_;
			struct cfg_condition_binding
			{
				sdk::detached_row row;
				const clang::Stmt* condition;
				const clang::Stmt* outcome;
			};
			std::vector<cfg_condition_binding> pending_cfg_conditions_;
			std::vector<cfg_condition_binding> pending_cfg_terminators_;
			std::size_t cfg_facet_work_{};
			struct flow_expression_binding
			{
				sdk::detached_row row;
				const clang::Stmt* expression;
				const clang::Expr* value;
			};
			std::vector<flow_expression_binding> pending_flow_expressions_;
			std::map<const clang::Expr*, std::string> call_evaluation_;
			struct type_structure
			{
				std::string digest, state;
			};
			std::map<std::string, type_structure, std::less<>> type_structure_;
			std::size_t call_bytes_{};
			const project_template_observations* templates_{};
			const project_template_event_observations* template_events_{};
			std::string current_function_;
			const clang::Stmt* inherited_default_{};
			std::vector<ast_enumeration> ast_enumerations_;
			std::map<const clang::FunctionDecl*, fields> pending_bodies_;
			std::vector<std::string> parents_;
			std::vector<clang::Stmt*> statements_;
			std::vector<const clang::Decl*> declaration_stack_;
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
			std::map<const void*, std::uint64_t> target_subjects_;
			std::map<std::tuple<std::uint64_t, std::string, std::string, std::uint64_t>,
					 std::size_t>
				target_slot_admissions_;
			std::vector<target_slot_observation> pending_target_slots_;
			std::size_t target_type_work_{};
			bool target_slot_domain_frontier_{};
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
		auto callbacks = std::make_unique<project_pp_callbacks>(preprocessor, closure, output);
		auto* observer = callbacks.get();
		preprocessor.addPPCallbacks(std::move(callbacks));
		preprocessor.setTokenWatcher(
			[&preprocessor, &closure, &output, observer](const clang::Token& token)
			{
				if (token.isAnnotation() || token.is(clang::tok::eof) ||
					token.is(clang::tok::eod) || token.is(clang::tok::comment) ||
					output.tokens_truncated)
					return;
				observer->observe_candidate(token, {token.getLocation(), token.getLocation()});
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
							  const project_original_calls& original_calls,
							  const std::string& project_id,
							  const project_template_observations* templates,
							  const project_template_event_observations* template_events)
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
			collector visitor{unit,
							  closure,
							  observations,
							  normalized,
							  progress,
							  original_calls,
							  project_id,
							  templates,
							  template_events};
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
			visitor.finish_preprocessor(preprocessing);
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
		(void)project_id;
		(void)templates;
		(void)template_events;
		return sdk::unexpected(
			sdk::error{"native.unsupported-clang-major", "cpp-facts", "clang-major-22"});
#endif
	}
} // namespace cxxlens::detail::clang22
