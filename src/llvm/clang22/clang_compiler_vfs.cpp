#include "clang_compiler_vfs.hpp"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/PCHContainerOperations.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>

#include <algorithm>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace cxxlens::detail::clang22::source_closure
{
	namespace
	{
		[[nodiscard]] clang_run_error make_error(
			std::string code, std::string path = {}, std::string detail = {})
		{
			return {std::move(code), std::move(path), std::move(detail)};
		}

		[[nodiscard]] std::expected<std::string, clang_run_error>
		map_bound_path(const std::string_view logical_path)
		{
			constexpr std::string_view project{"project://"};
			constexpr std::string_view generated{"generated://"};
			if (logical_path == project)
				return std::string{"/__cxxlens/project"};
			if (logical_path == generated)
				return std::string{"/__cxxlens/generated"};
			if (logical_path.starts_with(project))
				return std::string{"/__cxxlens/project/"} +
					std::string{logical_path.substr(project.size())};
			if (logical_path.starts_with(generated))
				return std::string{"/__cxxlens/generated/"} +
					std::string{logical_path.substr(generated.size())};
			return std::unexpected(make_error(
				"source-closure.clang-working-directory-unbound", std::string{logical_path}));
		}

		class callback_consumer final : public clang::ASTConsumer
		{
		  public:
			callback_consumer(clang_translation_unit_callback& callback,
				std::expected<void, clang_run_error>& outcome,
				std::string main_logical_path)
				: callback_{&callback}, outcome_{&outcome},
				  main_logical_path_{std::move(main_logical_path)}
			{
			}

			void HandleTranslationUnit(clang::ASTContext& context) override
			{
				if (context.getDiagnostics().hasErrorOccurred())
				{
					*outcome_ = std::unexpected(make_error(
						"source-closure.clang-parse-failed", main_logical_path_,
						"diagnostic-error"));
					return;
				}
				*outcome_ = (*callback_)(context, context.getSourceManager());
			}

		  private:
			clang_translation_unit_callback* callback_;
			std::expected<void, clang_run_error>* outcome_;
			std::string main_logical_path_;
		};

		class callback_action final : public clang::ASTFrontendAction
		{
		  public:
			callback_action(clang_translation_unit_callback& callback,
				std::expected<void, clang_run_error>& outcome,
				std::string main_logical_path)
				: callback_{&callback}, outcome_{&outcome},
				  main_logical_path_{std::move(main_logical_path)}
			{
			}

			std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
				clang::CompilerInstance&, llvm::StringRef) override
			{
				return std::make_unique<callback_consumer>(
					*callback_, *outcome_, main_logical_path_);
			}

		  private:
			clang_translation_unit_callback* callback_;
			std::expected<void, clang_run_error>* outcome_;
			std::string main_logical_path_;
		};

		class callback_factory final : public clang::tooling::FrontendActionFactory
		{
		  public:
			callback_factory(clang_translation_unit_callback& callback,
				std::expected<void, clang_run_error>& outcome,
				std::string main_logical_path)
				: callback_{&callback}, outcome_{&outcome},
				  main_logical_path_{std::move(main_logical_path)}
			{
			}

			std::unique_ptr<clang::FrontendAction> create() override
			{
				return std::make_unique<callback_action>(
					*callback_, *outcome_, main_logical_path_);
			}

		  private:
			clang_translation_unit_callback* callback_;
			std::expected<void, clang_run_error>* outcome_;
			std::string main_logical_path_;
		};
	} // namespace

	std::expected<void, clang_run_error>
	run_with_compiler_vfs(std::shared_ptr<const validated_snapshot> snapshot,
		const std::span<const std::string> arguments,
		const std::string_view logical_working_directory,
		clang_translation_unit_callback callback)
	{
		if (!snapshot || !callback || arguments.size() < 2U)
			return std::unexpected(make_error("source-closure.clang-input-invalid"));
		auto logical_vfs = read_only_compiler_vfs::create(snapshot);
		if (!logical_vfs)
			return std::unexpected(make_error(
				logical_vfs.error().code, logical_vfs.error().path, logical_vfs.error().role));

		const auto main = std::ranges::find_if(snapshot->files, [](const file& item)
		{
			return item.role == file_role::main_source;
		});
		if (main == snapshot->files.end() || arguments.back() != main->logical_path)
			return std::unexpected(make_error(
				"source-closure.clang-main-binding", arguments.back()));
		auto main_path = logical_vfs->map_logical_path(main->logical_path);
		if (!main_path)
			return std::unexpected(make_error(
				main_path.error().code, main_path.error().path, main_path.error().role));
		auto working_directory = map_bound_path(logical_working_directory);
		if (!working_directory)
			return std::unexpected(std::move(working_directory.error()));
		auto rewritten = logical_vfs->rewrite_invocation(arguments);
		if (!rewritten)
			return std::unexpected(make_error(
				rewritten.error().code, rewritten.error().path, rewritten.error().role));

		auto memory = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
		for (const auto& item : snapshot->files)
		{
			auto opened = logical_vfs->open_logical(item.logical_path);
			if (!opened)
				return std::unexpected(make_error(
					opened.error().code, opened.error().path, opened.error().role));
			const auto bytes = opened->content;
			const auto text = llvm::StringRef{
				reinterpret_cast<const char*>(bytes.data()), bytes.size()};
			auto buffer = llvm::MemoryBuffer::getMemBufferCopy(text, opened->compiler_path);
			if (!memory->addFile(opened->compiler_path, 0, std::move(buffer)))
				return std::unexpected(make_error(
					"source-closure.clang-vfs-add-failed", opened->logical_path));
		}
		if (const auto current = memory->setCurrentWorkingDirectory(*working_directory); current)
			return std::unexpected(make_error(
				"source-closure.clang-working-directory-invalid", *working_directory,
				current.message()));

		std::vector<std::string> compiler_arguments{
			rewritten->begin() + 1U, rewritten->end() - 1U};
		clang::tooling::FixedCompilationDatabase database{
			*working_directory, std::move(compiler_arguments)};
		clang::tooling::ClangTool tool{database, {*main_path},
			std::make_shared<clang::PCHContainerOperations>(), memory};
		std::expected<void, clang_run_error> callback_outcome{};
		callback_factory factory{callback, callback_outcome, main->logical_path};
		const auto status = tool.run(&factory);
		if (!callback_outcome)
			return callback_outcome;
		if (status != 0)
			return std::unexpected(make_error(
				"source-closure.clang-parse-failed", main->logical_path, std::to_string(status)));
		return {};
	}
} // namespace cxxlens::detail::clang22::source_closure
