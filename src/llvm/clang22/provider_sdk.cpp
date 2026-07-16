#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <memory>
#include <ranges>
#include <string_view>

#include <cxxlens/provider/clang22.hpp>

#if CXXLENS_HAS_CLANG22
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Lex/Lexer.h>
#include <clang/Tooling/Tooling.h>
#endif

namespace cxxlens::provider::clang22
{
	namespace detail
	{
		struct native_access
		{
			[[nodiscard]] static borrowed_translation_unit
			make(clang::ASTContext& ast, clang::SourceManager& source_manager)
			{
				return {ast, source_manager};
			}
		};
	} // namespace detail

	namespace
	{
		[[nodiscard]] sdk::error
		native_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool address_literal(const std::string_view value)
		{
			if (value.starts_with("native-address:") || value.starts_with("native-pointer:"))
				return true;
			if (!value.starts_with("0x") || value.size() < 10U)
				return false;
			return std::ranges::all_of(value.substr(2U),
									   [](const char byte)
									   {
										   return std::isxdigit(static_cast<unsigned char>(byte)) !=
											   0;
									   });
		}

#if CXXLENS_HAS_CLANG22
		class callback_consumer final : public clang::ASTConsumer
		{
		  public:
			callback_consumer(translation_unit_callback& callback, sdk::result<void>& outcome)
				: callback_{&callback}, outcome_{&outcome}
			{
			}

			void HandleTranslationUnit(clang::ASTContext& context) override
			{
				auto borrowed = detail::native_access::make(context, context.getSourceManager());
				*outcome_ = (*callback_)(borrowed);
			}

		  private:
			translation_unit_callback* callback_;
			sdk::result<void>* outcome_;
		};

		class callback_action final : public clang::ASTFrontendAction
		{
		  public:
			callback_action(translation_unit_callback& callback, sdk::result<void>& outcome)
				: callback_{&callback}, outcome_{&outcome}
			{
			}

			std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&,
																  llvm::StringRef) override
			{
				return std::make_unique<callback_consumer>(*callback_, *outcome_);
			}

		  private:
			translation_unit_callback* callback_;
			sdk::result<void>* outcome_;
		};
#endif
	} // namespace

	sdk::result<void> translation_unit_input::validate() const
	{
		if (logical_path.empty() || logical_path.front() == '/' ||
			logical_path.find("..") != std::string::npos)
			return sdk::unexpected(
				native_error("native.input-invalid", "logical_path", logical_path));
		if (source.empty())
			return sdk::unexpected(native_error("native.input-invalid", "source"));
		if (arguments.size() > 1024U)
			return sdk::unexpected(native_error("native.input-invalid", "arguments"));
		for (const auto& argument : arguments)
			if (argument.empty() || argument.find('\0') != std::string::npos)
				return sdk::unexpected(native_error("native.input-invalid", "argument"));
		return {};
	}

	borrowed_translation_unit::borrowed_translation_unit(clang::ASTContext& ast,
														 clang::SourceManager& source_manager)
		: ast_{&ast}, source_manager_{&source_manager}
	{
	}

	clang::ASTContext& borrowed_translation_unit::ast() const noexcept
	{
		return *ast_;
	}

	clang::SourceManager& borrowed_translation_unit::source_manager() const noexcept
	{
		return *source_manager_;
	}

	sdk::result<void> with_translation_unit(const translation_unit_input& input,
											translation_unit_callback callback)
	{
		if (auto valid = input.validate(); !valid)
			return valid;
		if (!callback)
			return sdk::unexpected(native_error("native.input-invalid", "callback"));
#if CXXLENS_HAS_CLANG22
		sdk::result<void> outcome{};
		auto action = std::make_unique<callback_action>(callback, outcome);
		const auto parsed = clang::tooling::runToolOnCodeWithArgs(std::move(action),
																  input.source,
																  input.arguments,
																  input.logical_path,
																  "cxxlens-clang22");
		if (!parsed && outcome)
			return sdk::unexpected(native_error("native.parse-failed", input.logical_path));
		return outcome;
#else
		(void)input;
		(void)callback;
		return sdk::unexpected(native_error("native.unsupported-clang-major", "clang", "22"));
#endif
	}

	sdk::result<void> detached_source_span::validate() const
	{
		if (logical_path.empty() || logical_path.front() == '/' || end < begin || id.empty())
			return sdk::unexpected(native_error("native.source-span-invalid", "source"));
		return {};
	}

	sdk::result<detached_source_span> normalize_source(borrowed_translation_unit& unit,
													   const clang::SourceRange& range)
	{
#if CXXLENS_HAS_CLANG22
		auto& source_manager = unit.source_manager();
		const auto begin_location = source_manager.getSpellingLoc(range.getBegin());
		const auto end_location =
			clang::Lexer::getLocForEndOfToken(source_manager.getSpellingLoc(range.getEnd()),
											  0U,
											  source_manager,
											  unit.ast().getLangOpts());
		if (begin_location.isInvalid() || end_location.isInvalid() ||
			!source_manager.isWrittenInSameFile(begin_location, end_location))
			return sdk::unexpected(native_error("native.source-span-invalid", "range"));
		const auto filename = source_manager.getFilename(begin_location);
		if (filename.empty())
			return sdk::unexpected(native_error("native.source-span-invalid", "file"));
		const auto begin = source_manager.getFileOffset(begin_location);
		const auto end = source_manager.getFileOffset(end_location);
		if (end < begin)
			return sdk::unexpected(native_error("native.source-span-invalid", "offset"));
		detached_source_span output;
		output.logical_path = filename.str();
		output.begin = begin;
		output.end = end;
		output.read_only = begin_location.isMacroID() || range.getBegin().isMacroID();
		output.id = sdk::canonical_identity_digest(
			"source-span",
			std::array{
				sdk::canonical_value::from_string(output.logical_path),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(output.begin)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(output.end)),
			});
		if (auto valid = output.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return output;
#else
		(void)unit;
		(void)range;
		return sdk::unexpected(native_error("native.unsupported-clang-major", "clang", "22"));
#endif
	}

	sdk::result<void> detect_native_escape(const sdk::detached_row& row)
	{
		for (const auto& [column, cell] : row.cells)
		{
			if (column.contains("pointer") || column.contains("address"))
				return cxxlens::sdk::unexpected(
					sdk::error{"native.address-escape", column, "identity-marker"});
			if (!cell.value || !std::holds_alternative<std::string>(*cell.value))
				continue;
			if (address_literal(std::get<std::string>(*cell.value)))
				return cxxlens::sdk::unexpected(
					sdk::error{"native.address-escape", column, "value-marker"});
		}
		return {};
	}
} // namespace cxxlens::provider::clang22
