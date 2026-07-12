#include "preprocessor_extractor.hpp"

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

#include "source_map_adapter.hpp"

#ifndef CXXLENS_HAS_CLANG22
#define CXXLENS_HAS_CLANG22 0
#endif

#if CXXLENS_HAS_CLANG22
#include <clang/Basic/FileEntry.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/MacroArgs.h>
#include <clang/Lex/MacroInfo.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/Token.h>
#include <llvm/ADT/StringRef.h>
#endif

namespace cxxlens::detail::clang22
{
	namespace
	{
		[[nodiscard]] error extractor_error(std::string reason)
		{
			error failure;
			failure.code.value = "extractor.invalid-observation";
			failure.message = "Preprocessor extraction failed";
			failure.scope = failure_scope::compile_unit;
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

#if CXXLENS_HAS_CLANG22
		constexpr std::string_view extractor_version = "1.0.0";

		[[nodiscard]] std::string fact_kind_name(const fact_kind kind)
		{
			switch (kind)
			{
				case fact_kind::file:
					return "file";
				case fact_kind::include_relation:
					return "include_relation";
				case fact_kind::macro_definition:
					return "macro_definition";
				case fact_kind::macro_expansion:
					return "macro_expansion";
				default:
					return "custom";
			}
		}

		[[nodiscard]] std::string bool_text(const bool value)
		{
			return value ? "true" : "false";
		}

		[[nodiscard]] std::string
		condition_value(const clang::PPCallbacks::ConditionValueKind value)
		{
			switch (value)
			{
				case clang::PPCallbacks::CVK_NotEvaluated:
					return "not_evaluated";
				case clang::PPCallbacks::CVK_False:
					return "false";
				case clang::PPCallbacks::CVK_True:
					return "true";
			}
			return "not_evaluated";
		}

		class extraction_session final : public preprocessor_extraction_session
		{
		  public:
			extraction_session(clang::CompilerInstance& compiler,
							   const compile_unit& unit,
							   const std::vector<frontend::virtual_source_file>& virtual_files)
				: compiler_{compiler}, unit_{unit}, sources_{compiler, unit, virtual_files}
			{
				for (std::size_t index = 0U; index < unit_.command().arguments.size(); ++index)
				{
					const auto& argument = unit_.command().arguments[index];
					std::string definition;
					if (argument == "-D" && index + 1U < unit_.command().arguments.size())
						definition = unit_.command().arguments[++index];
					else if (argument.starts_with("-D") && argument.size() > 2U)
						definition = argument.substr(2U);
					if (!definition.empty())
						command_macros_.insert(definition.substr(0U, definition.find('=')));
				}
			}

			void initialize()
			{
				const auto start = compiler_.getSourceManager().getLocForStartOfFile(
					compiler_.getSourceManager().getMainFileID());
				emit_file(start, clang::SrcMgr::C_User);
			}

			[[nodiscard]] result<std::vector<facts::observation_record>> take() override
			{
				std::ranges::sort(observations_, {}, &extraction_session::observation_key);
				const auto duplicate =
					std::ranges::unique(observations_, {}, &extraction_session::observation_key);
				observations_.erase(duplicate.begin(), duplicate.end());
				for (const auto& observation : observations_)
					if (auto checked = facts::validate(observation); !checked)
						return checked.error();
				return std::move(observations_);
			}

			void file_changed(const clang::SourceLocation location,
							  const clang::PPCallbacks::FileChangeReason reason,
							  const clang::SrcMgr::CharacteristicKind type)
			{
				if (reason == clang::PPCallbacks::EnterFile)
					emit_file(location, type);
			}

			void include(const clang::SourceLocation hash_location,
						 const llvm::StringRef spelling,
						 const bool angled,
						 const clang::CharSourceRange filename_range,
						 const clang::OptionalFileEntryRef resolved,
						 const clang::SrcMgr::CharacteristicKind type)
			{
				const clang::SourceRange directive{hash_location, filename_range.getEnd()};
				auto source = sources_.direct_span(directive);
				std::map<std::string, std::string> payload{
					{"conditional.context", conditional_context()},
					{"conditional.depth", std::to_string(conditionals_.size())},
					{"include.angled", bool_text(angled)},
					{"include.resolved",
					 resolved ? resolved_path(resolved->getName()) : "unresolved"},
					{"include.spelling", spelling.str()},
					{"include.system", bool_text(type != clang::SrcMgr::C_User)},
					{"semantic_key", include_key(hash_location, spelling)},
					{"variant", std::string{unit_.variant_id().value()}},
				};
				emit(fact_kind::include_relation,
					 source ? std::optional{std::move(source.value())} : std::nullopt,
					 std::move(payload));
			}

			void macro_defined(const clang::Token& name_token,
							   const clang::MacroDirective* directive)
			{
				const auto name = token_name(name_token);
				const auto* info = directive == nullptr ? nullptr : directive->getMacroInfo();
				std::optional<source_span> source;
				std::string body;
				std::string parameters;
				const bool builtin_location = info == nullptr ||
					info->getDefinitionLoc().isInvalid() ||
					compiler_.getSourceManager().isWrittenInBuiltinFile(info->getDefinitionLoc()) ||
					compiler_.getSourceManager().isWrittenInCommandLineFile(
						info->getDefinitionLoc());
				if (builtin_location && !command_macros_.contains(name))
					return;
				if (info != nullptr && info->getDefinitionLoc().isValid() &&
					compiler_.getSourceManager().isInSystemHeader(info->getDefinitionLoc()))
					return;
				const bool builtin =
					builtin_location || (info != nullptr && info->isBuiltinMacro());
				if (info != nullptr)
				{
					if (!builtin_location && info->getDefinitionLoc().isValid() &&
						info->getDefinitionEndLoc().isValid())
					{
						auto normalized = sources_.direct_span(
							{info->getDefinitionLoc(), info->getDefinitionEndLoc()});
						if (normalized)
							source = std::move(normalized.value());
					}
					body = macro_body(*info);
					for (const auto* parameter : info->params())
					{
						if (!parameters.empty())
							parameters += ',';
						parameters += parameter == nullptr ? "" : parameter->getName().str();
					}
				}
				std::map<std::string, std::string> payload{
					{"conditional.context", conditional_context()},
					{"macro.action", "define"},
					{"macro.body", body},
					{"macro.builtin", bool_text(builtin)},
					{"macro.name", name},
					{"macro.parameters", parameters},
					{"semantic_key",
					 builtin_location ? "macro:definition:" + name +
							 ":command-line:" + std::string{unit_.variant_id().value()}
									  : macro_key("definition", name, name_token.getLocation())},
					{"variant", std::string{unit_.variant_id().value()}},
				};
				emit(fact_kind::macro_definition, std::move(source), std::move(payload));
			}

			void macro_undefined(const clang::Token& name_token)
			{
				const auto name = token_name(name_token);
				auto source =
					sources_.direct_span({name_token.getLocation(), name_token.getEndLoc()});
				std::map<std::string, std::string> payload{
					{"conditional.context", conditional_context()},
					{"macro.action", "undef"},
					{"macro.name", name},
					{"semantic_key", macro_key("undef", name, name_token.getLocation())},
					{"variant", std::string{unit_.variant_id().value()}},
				};
				emit(fact_kind::macro_definition,
					 source ? std::optional{std::move(source.value())} : std::nullopt,
					 std::move(payload));
			}

			void macro_expands(const clang::Token& name_token,
							   const clang::MacroDefinition& definition,
							   const clang::SourceRange invocation,
							   const clang::MacroArgs* arguments)
			{
				const auto name = token_name(name_token);
				const auto* info = definition.getMacroInfo();
				const bool scratch = info != nullptr && info->getDefinitionLoc().isValid() &&
					compiler_.getSourceManager().isWrittenInScratchSpace(info->getDefinitionLoc());
				const bool builtin = info == nullptr || info->isBuiltinMacro() ||
					info->getDefinitionLoc().isInvalid() ||
					compiler_.getSourceManager().isWrittenInBuiltinFile(info->getDefinitionLoc()) ||
					compiler_.getSourceManager().isWrittenInCommandLineFile(
						info->getDefinitionLoc());
				std::optional<clang::SourceRange> definition_range;
				if (info != nullptr && !scratch && info->getDefinitionLoc().isValid() &&
					info->getDefinitionEndLoc().isValid())
					definition_range.emplace(info->getDefinitionLoc(), info->getDefinitionEndLoc());
				auto expansion =
					sources_.macro_span(invocation,
										invocation,
										definition_range ? &*definition_range : nullptr,
										name,
										source_origin::macro_expansion);
				std::map<std::string, std::string> payload{
					{"conditional.context", conditional_context()},
					{"conditional.depth", std::to_string(conditionals_.size())},
					{"macro.builtin", bool_text(builtin)},
					{"macro.definition_location",
					 info == nullptr || info->getDefinitionLoc().isInvalid() ? "invalid"
						 : scratch											 ? "scratch"
						 : builtin											 ? "builtin"
																			 : "file"},
					{"macro.name", name},
					{"macro.role", "expansion"},
					{"semantic_key", macro_key("expansion", name, invocation.getBegin())},
					{"variant", std::string{unit_.variant_id().value()}},
				};
				emit(fact_kind::macro_expansion,
					 expansion ? std::optional{std::move(expansion.value())} : std::nullopt,
					 std::move(payload));

				if (info != nullptr && !info->tokens().empty())
				{
					const auto body_range = clang::SourceRange{info->tokens().front().getLocation(),
															   info->tokens().back().getEndLoc()};
					auto body = sources_.macro_span(
						invocation, invocation, &body_range, name, source_origin::macro_body);
					std::map<std::string, std::string> body_payload{
						{"conditional.context", conditional_context()},
						{"macro.name", name},
						{"macro.role", "body"},
						{"macro.text", macro_body(*info)},
						{"semantic_key", macro_key("body", name, invocation.getBegin())},
						{"variant", std::string{unit_.variant_id().value()}},
					};
					emit(fact_kind::macro_expansion,
						 body ? std::optional{std::move(body.value())} : std::nullopt,
						 std::move(body_payload));
				}

				if (arguments == nullptr)
					return;
				for (std::uint32_t index = 0U; index < arguments->getNumMacroArguments(); ++index)
				{
					const auto* tokens = arguments->getUnexpArgument(index);
					const auto length = clang::MacroArgs::getArgLength(tokens);
					const auto argument_range = length == 0U
						? invocation
						: clang::SourceRange{tokens[0].getLocation(),
											 tokens[length - 1U].getEndLoc()};
					auto argument =
						sources_.macro_span(argument_range,
											invocation,
											definition_range ? &*definition_range : nullptr,
											name,
											source_origin::macro_argument,
											index);
					std::string text;
					for (std::uint32_t token_index = 0U; token_index < length; ++token_index)
					{
						if (!text.empty())
							text += ' ';
						text += compiler_.getPreprocessor().getSpelling(tokens[token_index]);
					}
					std::map<std::string, std::string> argument_payload{
						{"conditional.context", conditional_context()},
						{"macro.argument_index", std::to_string(index)},
						{"macro.name", name},
						{"macro.role", "argument"},
						{"macro.text", text},
						{"semantic_key",
						 macro_key(
							 "argument-" + std::to_string(index), name, invocation.getBegin())},
						{"variant", std::string{unit_.variant_id().value()}},
					};
					emit(fact_kind::macro_expansion,
						 argument ? std::optional{std::move(argument.value())} : std::nullopt,
						 std::move(argument_payload));
				}
			}

			void push_condition(std::string value)
			{
				conditionals_.push_back(std::move(value));
			}

			void replace_condition(std::string value)
			{
				if (conditionals_.empty())
					conditionals_.push_back(std::move(value));
				else
					conditionals_.back() = std::move(value);
			}

			void pop_condition()
			{
				if (!conditionals_.empty())
					conditionals_.pop_back();
			}

			[[nodiscard]] std::string condition_text(const clang::SourceRange range) const
			{
				return sources_.source_text(range);
			}

		  private:
			[[nodiscard]] static std::string observation_key(const facts::observation_record& value)
			{
				const auto semantic = value.payload.find("semantic_key");
				return std::to_string(static_cast<std::uint16_t>(value.kind)) + ":" +
					(semantic == value.payload.end() ? std::string{} : semantic->second);
			}

			[[nodiscard]] std::string token_name(const clang::Token& token) const
			{
				if (const auto* identifier = token.getIdentifierInfo(); identifier != nullptr)
					return identifier->getName().str();
				return compiler_.getPreprocessor().getSpelling(token);
			}

			[[nodiscard]] std::string macro_body(const clang::MacroInfo& info) const
			{
				std::string output;
				for (const auto& token : info.tokens())
				{
					if (!output.empty())
						output += ' ';
					output += compiler_.getPreprocessor().getSpelling(token);
				}
				return output;
			}

			[[nodiscard]] std::string conditional_context() const
			{
				if (conditionals_.empty())
					return "unconditional";
				std::string output;
				for (const auto& value : conditionals_)
				{
					if (!output.empty())
						output += " > ";
					output += value;
				}
				return output;
			}

			[[nodiscard]] std::string resolved_path(const llvm::StringRef input) const
			{
				const path filename{input.str()};
				const auto normalized = filename.lexically_normal();
				const auto relative =
					normalized.lexically_relative(unit_.command().directory.lexically_normal());
				if (!relative.empty() && !relative.native().starts_with("..") &&
					!relative.is_absolute())
					return relative.generic_string();
				return "external/" + normalized.filename().generic_string();
			}

			[[nodiscard]] std::string location_key(const clang::SourceLocation location) const
			{
				auto file = sources_.file_for(location);
				if (!file)
					return "invalid";
				const auto direct = sources_.direct_span({location, location});
				return std::string{file.value().id.value()} + ":" +
					(direct ? std::to_string(direct.value().primary.begin.byte_offset) : "invalid");
			}

			[[nodiscard]] std::string include_key(const clang::SourceLocation location,
												  const llvm::StringRef spelling) const
			{
				return "include:" + location_key(location) + ":" + spelling.str() + ":" +
					std::string{unit_.variant_id().value()};
			}

			[[nodiscard]] std::string macro_key(const std::string& role,
												const std::string& name,
												const clang::SourceLocation location) const
			{
				return "macro:" + role + ":" + name + ":" + location_key(location) + ":" +
					std::string{unit_.variant_id().value()};
			}

			void emit_file(const clang::SourceLocation location,
						   const clang::SrcMgr::CharacteristicKind type)
			{
				const auto& manager = compiler_.getSourceManager();
				const auto file_location = manager.getFileLoc(location);
				if (file_location.isInvalid() || manager.isWrittenInBuiltinFile(location) ||
					manager.getFilename(file_location).empty())
					return;
				auto normalized = sources_.file_for(location);
				if (!normalized)
					return;
				const auto key = std::string{normalized.value().id.value()};
				if (!emitted_files_.insert(key).second)
					return;
				std::map<std::string, std::string> payload{
					{"file.builtin", bool_text(normalized.value().builtin)},
					{"file.generated", bool_text(normalized.value().generated)},
					{"file.path", normalized.value().semantic_path},
					{"file.size", std::to_string(normalized.value().size)},
					{"file.system",
					 bool_text(normalized.value().system || type != clang::SrcMgr::C_User)},
					{"semantic_key", "file:" + key},
					{"variant", std::string{unit_.variant_id().value()}},
				};
				emit(fact_kind::file, normalized.value().whole_file, std::move(payload));
			}

			void emit(const fact_kind kind,
					  std::optional<source_span> source,
					  std::map<std::string, std::string> payload)
			{
				facts::observation_record observation;
				observation.adapter_id = "clang22.frontend";
				observation.adapter_version = std::string{extractor_version};
				observation.llvm_major = 22U;
				observation.compile_unit = unit_.id();
				observation.variant = unit_.variant_id();
				observation.kind = kind;
				observation.source = std::move(source);
				observation.payload_version = 1U;
				payload.emplace("extractor.id", "source-preprocessor");
				payload.emplace("extractor.version", extractor_version);
				observation.payload = std::move(payload);
				const auto semantic = observation.payload.at("semantic_key");
				observation.coverage_contributions.push_back(
					{fact_kind_name(kind), semantic, coverage_state::covered, std::nullopt});
				observations_.push_back(std::move(observation));
			}

			clang::CompilerInstance& compiler_;
			const compile_unit& unit_;
			source_map_adapter sources_;
			std::vector<std::string> conditionals_;
			std::set<std::string> command_macros_;
			std::set<std::string> emitted_files_;
			std::vector<facts::observation_record> observations_;
		};

		class callbacks final : public clang::PPCallbacks
		{
		  public:
			explicit callbacks(extraction_session& session) : session_{session} {}

			void FileChanged(clang::SourceLocation location,
							 clang::PPCallbacks::FileChangeReason reason,
							 clang::SrcMgr::CharacteristicKind type,
							 clang::FileID) override
			{
				session_.file_changed(location, reason, type);
			}

			void InclusionDirective(clang::SourceLocation hash_location,
									const clang::Token&,
									llvm::StringRef filename,
									bool angled,
									clang::CharSourceRange filename_range,
									clang::OptionalFileEntryRef file,
									llvm::StringRef,
									llvm::StringRef,
									const clang::Module*,
									bool,
									clang::SrcMgr::CharacteristicKind type) override
			{
				session_.include(hash_location, filename, angled, filename_range, file, type);
			}

			void MacroDefined(const clang::Token& token,
							  const clang::MacroDirective* directive) override
			{
				session_.macro_defined(token, directive);
			}

			void MacroUndefined(const clang::Token& token,
								const clang::MacroDefinition&,
								const clang::MacroDirective*) override
			{
				session_.macro_undefined(token);
			}

			void MacroExpands(const clang::Token& token,
							  const clang::MacroDefinition& definition,
							  clang::SourceRange range,
							  const clang::MacroArgs* arguments) override
			{
				session_.macro_expands(token, definition, range, arguments);
			}

			void If(clang::SourceLocation,
					clang::SourceRange condition,
					ConditionValueKind value) override
			{
				session_.push_condition("if(" + session_.condition_text(condition) +
										")=" + condition_value(value));
			}

			void Elif(clang::SourceLocation,
					  clang::SourceRange condition,
					  ConditionValueKind value,
					  clang::SourceLocation) override
			{
				session_.replace_condition("elif(" + session_.condition_text(condition) +
										   ")=" + condition_value(value));
			}

			void Ifdef(clang::SourceLocation,
					   const clang::Token& token,
					   const clang::MacroDefinition&) override
			{
				session_.push_condition("ifdef(" + token.getIdentifierInfo()->getName().str() +
										")");
			}

			void Ifndef(clang::SourceLocation,
						const clang::Token& token,
						const clang::MacroDefinition&) override
			{
				session_.push_condition("ifndef(" + token.getIdentifierInfo()->getName().str() +
										")");
			}

			void Else(clang::SourceLocation, clang::SourceLocation) override
			{
				session_.replace_condition("else");
			}

			void Endif(clang::SourceLocation, clang::SourceLocation) override
			{
				session_.pop_condition();
			}

		  private:
			extraction_session& session_;
		};
#endif

		class unavailable_session final : public preprocessor_extraction_session
		{
		  public:
			[[nodiscard]] result<std::vector<facts::observation_record>> take() override
			{
				return extractor_error("clang22-not-linked");
			}
		};
	} // namespace

	result<std::unique_ptr<preprocessor_extraction_session>>
	attach_preprocessor_extractor(clang::CompilerInstance& compiler,
								  const compile_unit& unit,
								  const std::vector<frontend::virtual_source_file>& virtual_files)
	{
#if !CXXLENS_HAS_CLANG22
		(void)compiler;
		(void)unit;
		(void)virtual_files;
		return std::unique_ptr<preprocessor_extraction_session>{
			std::make_unique<unavailable_session>()};
#else
		auto session = std::make_unique<extraction_session>(compiler, unit, virtual_files);
		session->initialize();
		compiler.getPreprocessor().addPPCallbacks(std::make_unique<callbacks>(*session));
		return std::unique_ptr<preprocessor_extraction_session>{std::move(session)};
#endif
	}
} // namespace cxxlens::detail::clang22
