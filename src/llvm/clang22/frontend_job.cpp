#include "frontend_job.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../runtime/time_port.hpp"
#include "../common/borrowed_lifetime.hpp"
#include "preprocessor_extractor.hpp"

#ifndef CXXLENS_HAS_CLANG22
#define CXXLENS_HAS_CLANG22 0
#endif

#if CXXLENS_HAS_CLANG22
#include <clang/AST/ASTConsumer.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Basic/Version.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Serialization/PCHContainerOperations.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>
#endif

#if !CXXLENS_HAS_CLANG22
namespace cxxlens
{
	void interop::borrowed_clang_tu::require_live() const noexcept
	{
		std::terminate();
	}
	clang::CompilerInstance& interop::borrowed_clang_tu::compiler() const noexcept
	{
		std::terminate();
	}
	clang::ASTContext& interop::borrowed_clang_tu::ast_context() const noexcept
	{
		std::terminate();
	}
	clang::SourceManager& interop::borrowed_clang_tu::source_manager() const noexcept
	{
		std::terminate();
	}
	clang::Preprocessor& interop::borrowed_clang_tu::preprocessor() const noexcept
	{
		std::terminate();
	}
	const clang::LangOptions& interop::borrowed_clang_tu::language_options() const noexcept
	{
		std::terminate();
	}
	const compile_unit& interop::borrowed_clang_tu::unit() const noexcept
	{
		std::terminate();
	}
} // namespace cxxlens
#endif

namespace cxxlens
{
	namespace
	{
		[[nodiscard]] error frontend_error(std::string code, std::string reason)
		{
			error failure;
			failure.code.value = std::move(code);
			failure.message = "Clang frontend job failed";
			failure.scope = failure_scope::compile_unit;
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		[[nodiscard]] result<void> injected_failure(const detail::frontend::frontend_fault fault)
		{
			switch (fault)
			{
				case detail::frontend::frontend_fault::none:
					return {};
				case detail::frontend::frontend_fault::timeout:
					return frontend_error("parse.timeout", "injected-timeout");
				case detail::frontend::frontend_fault::cancellation:
					return frontend_error("core.cancelled", "injected-cancellation");
				case detail::frontend::frontend_fault::crash:
					return frontend_error("parse.crashed", "injected-crash");
			}
			return frontend_error("core.internal-invariant-violation", "unknown-fault");
		}
	} // namespace

#if CXXLENS_HAS_CLANG22
	struct interop::borrowed_clang_tu::state
	{
		clang::CompilerInstance* compiler{};
		const compile_unit* unit{};
		detail::frontend::borrowed_lifetime_token* lifetime{};
	};

	struct detail::clang22::borrowed_tu_access
	{
		[[nodiscard]] static result<void> invoke(clang::CompilerInstance& compiler,
												 const compile_unit& unit,
												 interop::clang_tu_callback& callback)
		{
			detail::frontend::borrowed_lifetime_token lifetime;
			interop::borrowed_clang_tu::state state{&compiler, &unit, &lifetime};
			interop::borrowed_clang_tu view{&state};
			try
			{
				auto outcome = callback(view);
				lifetime.retire();
				return outcome;
			}
			catch (const std::exception&)
			{
				lifetime.retire();
				return frontend_error("parse.frontend-failed", "callback-exception");
			}
			catch (...)
			{
				lifetime.retire();
				return frontend_error("parse.frontend-failed", "callback-foreign-exception");
			}
		}
	};

	void interop::borrowed_clang_tu::require_live() const noexcept
	{
		if (state_ == nullptr || state_->compiler == nullptr || state_->unit == nullptr ||
			state_->lifetime == nullptr || !state_->lifetime->active_on_owner())
		{
			std::terminate();
		}
	}

	clang::CompilerInstance& interop::borrowed_clang_tu::compiler() const noexcept
	{
		require_live();
		return *state_->compiler;
	}
	clang::ASTContext& interop::borrowed_clang_tu::ast_context() const noexcept
	{
		return compiler().getASTContext();
	}
	clang::SourceManager& interop::borrowed_clang_tu::source_manager() const noexcept
	{
		return compiler().getSourceManager();
	}
	clang::Preprocessor& interop::borrowed_clang_tu::preprocessor() const noexcept
	{
		return compiler().getPreprocessor();
	}
	const clang::LangOptions& interop::borrowed_clang_tu::language_options() const noexcept
	{
		return compiler().getLangOpts();
	}
	const compile_unit& interop::borrowed_clang_tu::unit() const noexcept
	{
		require_live();
		return *state_->unit;
	}
#endif

	namespace detail::clang22
	{
		namespace
		{
			constexpr std::string_view adapter_version = "1.0.0";
			constexpr std::array<std::string_view, 12U> component_names{
				"LLVMOption",
				"LLVMSupport",
				"clangAST",
				"clangBasic",
				"clangDriver",
				"clangFrontend",
				"clangFrontendTool",
				"clangLex",
				"clangOptions",
				"clangSerialization",
				"clangTooling",
				"clangToolingCore",
			};

			[[nodiscard]] std::vector<std::string> components()
			{
				std::vector<std::string> output;
				output.reserve(component_names.size());
				for (const auto value : component_names)
					output.emplace_back(value);
				return output;
			}

			[[nodiscard]] result<frontend::observation_batch>
			preflight(frontend::parse_task& task, const execution_context& context)
			{
				if (auto injected = injected_failure(task.injected_fault); !injected)
					return std::move(injected.error());
				if (context.cancellation.stop_requested())
					return frontend_error("core.cancelled", "stop-requested");
				runtime::real_time_adapter clock;
				if (context.deadline && clock.steady_now() >= *context.deadline)
					return frontend_error("core.deadline-exceeded", "deadline-before-parse");
				if (!task.unit.id().valid() || !task.unit.variant_id().valid() ||
					task.unit.command().arguments.empty())
					return frontend_error("parse.invocation-build-failed", "invalid-compile-unit");
				return frontend::observation_batch{};
			}

#if CXXLENS_HAS_CLANG22
			[[nodiscard]] frontend::diagnostic_severity
			severity(const clang::DiagnosticsEngine::Level level)
			{
				switch (level)
				{
					case clang::DiagnosticsEngine::Ignored:
					case clang::DiagnosticsEngine::Remark:
					case clang::DiagnosticsEngine::Note:
						return frontend::diagnostic_severity::note;
					case clang::DiagnosticsEngine::Warning:
						return frontend::diagnostic_severity::warning;
					case clang::DiagnosticsEngine::Error:
						return frontend::diagnostic_severity::error;
					case clang::DiagnosticsEngine::Fatal:
						return frontend::diagnostic_severity::fatal;
				}
				return frontend::diagnostic_severity::fatal;
			}

			[[nodiscard]] std::string sanitize(std::string_view input)
			{
				std::string output;
				output.reserve(input.size());
				for (const auto value : input)
				{
					const auto byte = static_cast<unsigned char>(value);
					if (byte >= 0x20U && byte != 0x7FU)
						output.push_back(value);
				}
				return output;
			}

			class diagnostic_bridge final : public clang::DiagnosticConsumer
			{
			  public:
				explicit diagnostic_bridge(path root) : root_{std::move(root)} {}

				void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
									  const clang::Diagnostic& info) override
				{
					clang::DiagnosticConsumer::HandleDiagnostic(level, info);
					llvm::SmallString<256> rendered;
					info.FormatDiagnostic(rendered);
					frontend::normalized_diagnostic row;
					row.id = "clang.diagnostic." + std::to_string(info.getID());
					row.severity = severity(level);
					row.message = sanitize(std::string_view{rendered.data(), rendered.size()});
					if (info.hasSourceManager() && info.getLocation().isValid())
					{
						const auto presumed =
							info.getSourceManager().getPresumedLoc(info.getLocation());
						if (presumed.isValid())
						{
							path file{presumed.getFilename()};
							const auto relative = file.lexically_normal().lexically_relative(root_);
							row.file = relative.empty() || relative.native().starts_with("..")
								? file.filename().generic_string()
								: relative.generic_string();
							row.line = presumed.getLine();
							row.column = presumed.getColumn();
						}
					}
					rows_.push_back(std::move(row));
				}

				[[nodiscard]] std::vector<frontend::normalized_diagnostic> take()
				{
					std::ranges::sort(rows_);
					const auto duplicate = std::ranges::unique(rows_);
					rows_.erase(duplicate.begin(), duplicate.end());
					return std::move(rows_);
				}

			  private:
				path root_;
				std::vector<frontend::normalized_diagnostic> rows_;
			};

			class borrowed_consumer final : public clang::ASTConsumer
			{
			  public:
				borrowed_consumer(clang::CompilerInstance& compiler,
								  const compile_unit& unit,
								  interop::clang_tu_callback* callback,
								  std::optional<error>& callback_error,
								  bool& callback_invoked)
					: compiler_{compiler}, unit_{unit}, callback_{callback},
					  callback_error_{callback_error}, callback_invoked_{callback_invoked}
				{
				}

				void HandleTranslationUnit(clang::ASTContext&) override
				{
					if (callback_ == nullptr || !*callback_)
						return;
					callback_invoked_ = true;
					auto outcome = borrowed_tu_access::invoke(compiler_, unit_, *callback_);
					if (!outcome)
						callback_error_ = std::move(outcome.error());
				}

			  private:
				clang::CompilerInstance& compiler_;
				const compile_unit& unit_;
				interop::clang_tu_callback* callback_;
				std::optional<error>& callback_error_;
				bool& callback_invoked_;
			};

			class borrowed_action final : public clang::ASTFrontendAction
			{
			  public:
				borrowed_action(const frontend::parse_task& task,
								interop::clang_tu_callback* callback,
								std::optional<error>& callback_error,
								bool& callback_invoked,
								std::optional<error>& extraction_error,
								std::vector<facts::observation_record>& observations)
					: task_{task}, callback_{callback}, callback_error_{callback_error},
					  callback_invoked_{callback_invoked}, extraction_error_{extraction_error},
					  observations_{observations}
				{
				}

				bool BeginSourceFileAction(clang::CompilerInstance& compiler) override
				{
					auto attached =
						attach_preprocessor_extractor(compiler, task_.unit, task_.files);
					if (!attached)
					{
						extraction_error_ = std::move(attached.error());
						return false;
					}
					extraction_ = std::move(attached.value());
					return true;
				}

				std::unique_ptr<clang::ASTConsumer>
				CreateASTConsumer(clang::CompilerInstance& compiler, llvm::StringRef) override
				{
					return std::make_unique<borrowed_consumer>(
						compiler, task_.unit, callback_, callback_error_, callback_invoked_);
				}

				void EndSourceFileAction() override
				{
					if (!extraction_)
						return;
					auto extracted = extraction_->take();
					if (!extracted)
						extraction_error_ = std::move(extracted.error());
					else
						observations_ = std::move(extracted.value());
				}

			  private:
				const frontend::parse_task& task_;
				interop::clang_tu_callback* callback_;
				std::optional<error>& callback_error_;
				bool& callback_invoked_;
				std::optional<error>& extraction_error_;
				std::vector<facts::observation_record>& observations_;
				std::unique_ptr<preprocessor_extraction_session> extraction_;
			};

			class borrowed_action_factory final : public clang::tooling::FrontendActionFactory
			{
			  public:
				borrowed_action_factory(const frontend::parse_task& task,
										interop::clang_tu_callback* callback,
										std::optional<error>& callback_error,
										bool& callback_invoked,
										std::optional<error>& extraction_error,
										std::vector<facts::observation_record>& observations)
					: task_{task}, callback_{callback}, callback_error_{callback_error},
					  callback_invoked_{callback_invoked}, extraction_error_{extraction_error},
					  observations_{observations}
				{
				}

				std::unique_ptr<clang::FrontendAction> create() override
				{
					return std::make_unique<borrowed_action>(task_,
															 callback_,
															 callback_error_,
															 callback_invoked_,
															 extraction_error_,
															 observations_);
				}

			  private:
				const frontend::parse_task& task_;
				interop::clang_tu_callback* callback_;
				std::optional<error>& callback_error_;
				bool& callback_invoked_;
				std::optional<error>& extraction_error_;
				std::vector<facts::observation_record>& observations_;
			};

			[[nodiscard]] std::vector<std::string> tool_arguments(const compile_command& command)
			{
				std::vector<std::string> output;
				for (std::size_t index = 1U; index < command.arguments.size(); ++index)
				{
					const auto& value = command.arguments[index];
					if (value == "-c" ||
						path{value}.lexically_normal() == command.file.lexically_normal())
						continue;
					if (value == "-o" && index + 1U < command.arguments.size())
					{
						++index;
						continue;
					}
					if (value.starts_with("-o") && value.size() > 2U)
						continue;
					output.push_back(value);
				}
				return output;
			}
#endif
		} // namespace

		frontend::adapter_capability capability()
		{
			frontend::adapter_capability output;
			output.adapter_version = adapter_version;
			output.explicit_components = components();
#if CXXLENS_HAS_CLANG22
			output.available = true;
			output.llvm_major = CLANG_VERSION_MAJOR;
			output.llvm_minor = CLANG_VERSION_MINOR;
			output.llvm_patch = CLANG_VERSION_PATCHLEVEL;
#else
			output.limitation = "Clang 22 development libraries were not found at configure time";
#endif
			return output;
		}

		result<frontend::observation_batch>
		execute(frontend::parse_task task,
				execution_context context, // NOLINT(performance-unnecessary-value-param)
				interop::clang_tu_callback callback)
		{
			if (auto checked = preflight(task, context); !checked)
				return std::move(checked.error());
#if !CXXLENS_HAS_CLANG22
			(void)callback;
			return frontend_error("core.capability-unavailable", "clang22-not-linked");
#else
			static std::atomic_uint64_t next_context{1U};
			frontend::observation_batch batch;
			batch.adapter_id = "clang22.frontend";
			batch.adapter_version = std::string{adapter_version};
			batch.unit = task.unit.id();
			batch.variant = task.unit.variant_id();
			batch.debug_context_identity = next_context.fetch_add(1U, std::memory_order_relaxed);

			const auto& command = task.unit.command();
			clang::tooling::FixedCompilationDatabase database{command.directory.generic_string(),
															  tool_arguments(command)};
			auto filesystem = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(
				llvm::vfs::getRealFileSystem());
			auto snapshot = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
			for (const auto& file : task.files)
			{
				const auto filename = file.file.lexically_normal().generic_string();
				if (!snapshot->addFile(
						filename, 0, llvm::MemoryBuffer::getMemBufferCopy(file.content, filename)))
					return frontend_error("parse.invocation-build-failed", "duplicate-vfs-file");
			}
			filesystem->pushOverlay(snapshot);
			clang::tooling::ClangTool tool{database,
										   {command.file.generic_string()},
										   std::make_shared<clang::PCHContainerOperations>(),
										   filesystem};
			diagnostic_bridge diagnostics{command.directory};
			tool.setDiagnosticConsumer(&diagnostics);
			std::optional<error> callback_error;
			std::optional<error> extraction_error;
			std::vector<facts::observation_record> observations;
			bool callback_invoked = false;
			borrowed_action_factory factory{task,
											callback ? &callback : nullptr,
											callback_error,
											callback_invoked,
											extraction_error,
											observations};
			const auto status = tool.run(&factory);
			batch.diagnostics = diagnostics.take();
			batch.observations = std::move(observations);
			if (context.cancellation.stop_requested())
				return frontend_error("core.cancelled", "stop-requested-during-parse");
			runtime::real_time_adapter clock;
			if (context.deadline && clock.steady_now() >= *context.deadline)
				return frontend_error("parse.timeout", "deadline-exceeded-during-parse");
			if (callback_error)
				return std::move(*callback_error);
			if (extraction_error)
				return std::move(*extraction_error);
			if (callback && !callback_invoked)
				return frontend_error("parse.frontend-failed", "callback-not-invoked");
			if (status == 0)
				batch.coverage.parsed = 1U;
			else
				batch.coverage.failed = 1U;
			if (auto validation = batch.validate(); !validation)
				return std::move(validation.error());
			return batch;
#endif
		}
	} // namespace detail::clang22

	namespace interop
	{
		clang_api_version linked_clang_version() noexcept
		{
#if CXXLENS_HAS_CLANG22
			return {CLANG_VERSION_MAJOR,
					CLANG_VERSION_MINOR,
					CLANG_VERSION_PATCHLEVEL,
					CLANG_VERSION_STRING};
#else
			return {0U, 0U, 0U, "unavailable"};
#endif
		}

		result<void>
		with_translation_unit(workspace& workspace,
							  compile_unit_id unit, // NOLINT(performance-unnecessary-value-param)
							  clang_tu_callback callback,
							  execution_context context)
		{
			if (!callback)
				return frontend_error("core.invalid-argument", "empty-callback");
			std::optional<compile_unit> selected;
			for (auto candidate : workspace.compile_units())
			{
				if (candidate.id() == unit)
				{
					selected = std::move(candidate);
					break;
				}
			}
			if (!selected)
				return frontend_error("core.invalid-argument", "compile-unit-not-found");
			detail::frontend::parse_task task{*selected, {}};
			auto batch =
				detail::clang22::execute(std::move(task), std::move(context), std::move(callback));
			if (!batch)
				return std::move(batch.error());
			if (batch.value().coverage.failed != 0U)
			{
				auto failure = frontend_error("parse.frontend-failed", "diagnostics-error");
				failure.attributes.emplace("diagnostic_count",
										   std::to_string(batch.value().diagnostics.size()));
				return failure;
			}
			return {};
		}
	} // namespace interop
} // namespace cxxlens
