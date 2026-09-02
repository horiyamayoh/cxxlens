#include "worker_parser.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclGroup.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/FileManager.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>

namespace cxxlens::detail::clang23_gcc_replay
{
	namespace
	{
		constexpr std::string_view logical_root{"project://"};
		constexpr std::string_view synthetic_root{"/__cxxlens_gcc_replay__"};

		[[nodiscard]] sdk::error failure(std::string field, std::string detail)
		{
			return {
				"application-analysis.replay-parse-failed", std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::string native_path(const std::string_view logical)
		{
			return std::string{synthetic_root} + "/" +
				std::string{logical.substr(logical_root.size())};
		}

		[[nodiscard]] sdk::result<std::string> compiler_argument(const std::string_view argument)
		{
			if (argument.starts_with(logical_root))
				return native_path(argument);
			constexpr std::array<std::string_view, 4U> joined_path_options{
				"-idirafter", "-isystem", "-iquote", "-I"};
			for (const auto option : joined_path_options)
				if (argument.starts_with(option) &&
					argument.substr(option.size()).starts_with(logical_root))
					return std::string{option} + native_path(argument.substr(option.size()));
			if (argument.contains(logical_root))
				return sdk::unexpected(failure("effective_argv", "unbound-logical-path"));
			return std::string{argument};
		}

		struct observation_state
		{
			std::optional<observation_batch> observations;
			std::optional<sdk::error> error;
		};

		class counting_consumer final : public clang::ASTConsumer
		{
		  public:
			counting_consumer(std::size_t& declarations,
							  observation_state& observations,
							  observer_limits limits)
				: declarations_{declarations}, observations_{observations}, limits_{limits}
			{
			}

			bool HandleTopLevelDecl(const clang::DeclGroupRef group) override
			{
				declarations_ +=
					static_cast<std::size_t>(std::distance(group.begin(), group.end()));
				return true;
			}

			void HandleTranslationUnit(clang::ASTContext& context) override
			{
				auto observed = observe_translation_unit(context, limits_);
				if (observed)
					observations_.observations = std::move(*observed);
				else
					observations_.error = std::move(observed.error());
			}

		  private:
			std::size_t& declarations_;
			observation_state& observations_;
			observer_limits limits_;
		};

		class counting_action final : public clang::ASTFrontendAction
		{
		  public:
			counting_action(std::size_t& declarations,
							observation_state& observations,
							observer_limits limits)
				: declarations_{declarations}, observations_{observations}, limits_{limits}
			{
			}

		  protected:
			std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&,
																  llvm::StringRef) override
			{
				return std::make_unique<counting_consumer>(declarations_, observations_, limits_);
			}

		  private:
			std::size_t& declarations_;
			observation_state& observations_;
			observer_limits limits_;
		};

		class counting_diagnostics final : public clang::DiagnosticConsumer
		{
		  public:
			counting_diagnostics(std::size_t& warnings, std::size_t& errors)
				: warnings_{warnings}, errors_{errors}
			{
			}

			void HandleDiagnostic(const clang::DiagnosticsEngine::Level level,
								  const clang::Diagnostic&) override
			{
				if (level == clang::DiagnosticsEngine::Warning)
					++warnings_;
				else if (level == clang::DiagnosticsEngine::Error ||
						 level == clang::DiagnosticsEngine::Fatal)
					++errors_;
			}

		  private:
			std::size_t& warnings_;
			std::size_t& errors_;
		};
	} // namespace

	sdk::result<parse_result>
	parse_replay_input(const sdk::detail::validated_gcc_replay_input& input,
					   const observer_limits limits)
	{
		try
		{
			if (auto valid = limits.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			const auto& value = input.value();
			const auto main = std::ranges::find(value.source_members,
												std::string_view{"main"},
												&sdk::detail::decoded_capture_source_member::role);
			if (main == value.source_members.end() ||
				std::ranges::count(value.effective_arguments, main->logical_path) != 1)
				return sdk::unexpected(failure("effective_argv", "main-source-binding"));

			auto filesystem = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
			for (const auto& member : value.source_members)
			{
				const auto path = native_path(member.logical_path);
				const auto content = llvm::StringRef{
					reinterpret_cast<const char*>(member.content.data()), member.content.size()};
				if (!filesystem->addFile(
						path, 0, llvm::MemoryBuffer::getMemBufferCopy(content, path)))
					return sdk::unexpected(failure("source_members", "vfs-mount"));
			}
			if (const auto error = filesystem->setCurrentWorkingDirectory(synthetic_root))
				return sdk::unexpected(failure("source_members", error.message()));

			std::vector<std::string> arguments;
			arguments.reserve(value.effective_arguments.size());
			for (const auto& argument : value.effective_arguments)
			{
				auto rewritten = compiler_argument(argument);
				if (!rewritten)
					return sdk::unexpected(std::move(rewritten.error()));
				arguments.push_back(std::move(*rewritten));
			}

			clang::FileSystemOptions filesystem_options;
			auto file_manager =
				llvm::makeIntrusiveRefCnt<clang::FileManager>(filesystem_options, filesystem);
			parse_result result;
			observation_state observations;
			counting_diagnostics diagnostics{result.warning_count, result.error_count};
			auto action =
				std::make_unique<counting_action>(result.declaration_count, observations, limits);
			clang::tooling::ToolInvocation invocation{
				std::move(arguments), std::move(action), file_manager.get()};
			invocation.setDiagnosticConsumer(&diagnostics);
			const auto succeeded = invocation.run();
			result.terminal = succeeded && result.error_count == 0U ? parse_terminal::parsed
																	: parse_terminal::rejected;
			if (observations.error)
				return sdk::unexpected(std::move(*observations.error));
			if (result.terminal == parse_terminal::parsed)
			{
				if (!observations.observations)
					return sdk::unexpected(failure("translation_unit", "observation-missing"));
				result.observations = std::move(*observations.observations);
			}
			return result;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("memory", "allocation"));
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(failure("memory", "length"));
		}
	}
} // namespace cxxlens::detail::clang23_gcc_replay
