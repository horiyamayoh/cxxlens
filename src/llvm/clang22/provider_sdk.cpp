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
		if (source_snapshot.empty() || file.empty())
			return sdk::unexpected(native_error("native.input-invalid", "source-identity"));
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
		if (source_snapshot.empty() || file.empty() || role.empty() || logical_path.empty() ||
			logical_path.front() == '/' || end < begin || id.empty())
			return sdk::unexpected(native_error("native.source-span-invalid", "source"));
		auto expected = sdk::source_span_identity(source_snapshot, file, begin, end, role);
		if (!expected || *expected != id)
			return sdk::unexpected(native_error("native.source-span-invalid", "identity"));
		for (const auto& origin : origin_chain)
			if (auto valid = origin.validate(); !valid)
				return valid;
		return {};
	}

	sdk::result<void> detached_source_origin::validate() const
	{
		if (kind.empty() || logical_path.empty() || end < begin || !read_only ||
			kind.find('\0') != std::string::npos || logical_path.find('\0') != std::string::npos)
			return sdk::unexpected(native_error("native.source-origin-invalid", "origin"));
		return {};
	}

	sdk::result<void> source_range_identity::validate() const
	{
		if (source_snapshot.empty() || file.empty() || role.empty() ||
			role.find('\0') != std::string::npos)
			return sdk::unexpected(native_error("native.source-span-invalid", "identity-input"));
		return {};
	}

	sdk::result<detached_source_span> normalize_source(borrowed_translation_unit& unit,
													   const clang::SourceRange& range,
													   const source_range_identity& identity)
	{
		if (auto valid = identity.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
#if CXXLENS_HAS_CLANG22
		auto& source_manager = unit.source_manager();
		const auto expansion = source_manager.getExpansionRange(range);
		const auto begin_location = source_manager.getExpansionLoc(expansion.getBegin());
		auto end_location = source_manager.getExpansionLoc(expansion.getEnd());
		if (expansion.isTokenRange())
			end_location = clang::Lexer::getLocForEndOfToken(
				end_location, 0U, source_manager, unit.ast().getLangOpts());
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
		output.source_snapshot = identity.source_snapshot;
		output.file = identity.file;
		output.role = identity.role;
		output.logical_path = filename.str();
		output.begin = begin;
		output.end = end;
		output.read_only = range.getBegin().isMacroID() || range.getEnd().isMacroID();
		auto origin_begin = range.getBegin();
		auto origin_end = range.getEnd();
		for (std::size_t depth = 0U;
			 depth < 128U && (origin_begin.isMacroID() || origin_end.isMacroID());
			 ++depth)
		{
			const auto spelling_begin = source_manager.getSpellingLoc(origin_begin);
			const auto spelling_end_token = source_manager.getSpellingLoc(origin_end);
			const auto spelling_end = clang::Lexer::getLocForEndOfToken(
				spelling_end_token, 0U, source_manager, unit.ast().getLangOpts());
			if (spelling_begin.isInvalid() || spelling_end.isInvalid())
				return sdk::unexpected(native_error("native.source-origin-invalid", "range"));
			if (source_manager.isWrittenInSameFile(spelling_begin, spelling_end))
			{
				const auto origin_filename = source_manager.getFilename(spelling_begin);
				if (origin_filename.empty())
					return sdk::unexpected(native_error("native.source-origin-invalid", "file"));
				output.origin_chain.push_back({
					"macro-spelling",
					origin_filename.str(),
					source_manager.getFileOffset(spelling_begin),
					source_manager.getFileOffset(spelling_end),
					true,
				});
			}
			else
			{
				const auto begin_filename = source_manager.getFilename(spelling_begin);
				const auto end_filename = source_manager.getFilename(spelling_end_token);
				const auto begin_token_end = clang::Lexer::getLocForEndOfToken(
					spelling_begin, 0U, source_manager, unit.ast().getLangOpts());
				if (begin_filename.empty() || end_filename.empty() || begin_token_end.isInvalid())
					return sdk::unexpected(native_error("native.source-origin-invalid", "file"));
				output.origin_chain.push_back({
					"macro-spelling-begin",
					begin_filename.str(),
					source_manager.getFileOffset(spelling_begin),
					source_manager.getFileOffset(begin_token_end),
					true,
				});
				output.origin_chain.push_back({
					"macro-spelling-end",
					end_filename.str(),
					source_manager.getFileOffset(spelling_end_token),
					source_manager.getFileOffset(spelling_end),
					true,
				});
			}
			const auto next_begin = origin_begin.isMacroID()
				? source_manager.getImmediateExpansionRange(origin_begin).getBegin()
				: origin_begin;
			const auto next_end = origin_end.isMacroID()
				? source_manager.getImmediateExpansionRange(origin_end).getEnd()
				: origin_end;
			if (next_begin == origin_begin && next_end == origin_end)
				break;
			origin_begin = next_begin;
			origin_end = next_end;
		}
		if (origin_begin.isMacroID() || origin_end.isMacroID())
			return sdk::unexpected(native_error("native.source-origin-invalid", "depth"));
		auto id = sdk::source_span_identity(
			identity.source_snapshot, identity.file, output.begin, output.end, identity.role);
		if (!id)
			return sdk::unexpected(std::move(id.error()));
		output.id = std::move(*id);
		if (auto valid = output.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return output;
#else
		(void)unit;
		(void)range;
		(void)identity;
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
