#include "source_map_adapter.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

#include "../../core/canonical_encoding.hpp"
#include "../../runtime/hash_port.hpp"

#ifndef CXXLENS_HAS_CLANG22
#define CXXLENS_HAS_CLANG22 0
#endif

#if CXXLENS_HAS_CLANG22
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/Preprocessor.h>
#endif

namespace cxxlens::detail::clang22
{
	namespace
	{
		[[nodiscard]] error source_error(std::string reason)
		{
			error failure;
			failure.code.value = "extractor.invalid-observation";
			failure.message = "Clang source location could not be normalized";
			failure.scope = failure_scope::compile_unit;
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}
	} // namespace

#if CXXLENS_HAS_CLANG22
	struct source_map_adapter::impl
	{
		clang::CompilerInstance& compiler;
		const compile_unit& unit;
		std::set<std::string> generated_files;
		runtime::fnv1a_hash_adapter hashes;
		mutable identity::collision_registry collisions;
		mutable std::map<std::string, normalized_source_file> files;

		impl(clang::CompilerInstance& compiler_value,
			 const compile_unit& unit_value,
			 const std::vector<frontend::virtual_source_file>& virtual_file_values)
			: compiler{compiler_value}, unit{unit_value}
		{
			for (const auto& file : virtual_file_values)
			{
				const auto normalized = file.file.lexically_normal();
				const auto relative =
					normalized.lexically_relative(unit.command().directory.lexically_normal());
				const auto key = !relative.empty() && !relative.native().starts_with("..")
					? relative.generic_string()
					: "external/" + normalized.filename().generic_string();
				if (normalized.filename().generic_string().find("generated") != std::string::npos)
					generated_files.insert(key);
			}
		}

		[[nodiscard]] std::string semantic_path(const clang::SourceLocation location) const
		{
			const auto& sources = compiler.getSourceManager();
			const auto file_location = sources.getFileLoc(location);
			if (file_location.isInvalid() || sources.isWrittenInBuiltinFile(location))
				return "builtin/predefined";
			path filename{sources.getFilename(file_location).str()};
			if (filename.empty())
				return "unknown/source";
			const auto normalized = filename.lexically_normal();
			const auto root = unit.command().directory.lexically_normal();
			const auto relative = normalized.lexically_relative(root);
			if (!relative.empty() && !relative.native().starts_with("..") &&
				!relative.is_absolute())
				return relative.generic_string();
			return "external/" + normalized.filename().generic_string();
		}

		[[nodiscard]] result<source_digest> digest_for(const clang::SourceLocation location) const
		{
			const auto& sources = compiler.getSourceManager();
			const auto file_location = sources.getFileLoc(location);
			if (file_location.isInvalid())
				return source_error("invalid-digest-location");
			bool invalid = false;
			const auto buffer = sources.getBufferData(sources.getFileID(file_location), &invalid);
			if (invalid)
				return source_error("unavailable-source-buffer");
			const std::string content = buffer.str();
			const auto request = runtime::make_hash_request("cxxlens.source-content.v1", content);
			auto calculated = hashes.calculate(request, {});
			if (!calculated)
				return source_error("source-digest-failed");
			return source_digest{calculated.value().algorithm,
								 calculated.value().version,
								 calculated.value().hexadecimal};
		}

		[[nodiscard]] result<file_id> id_for(const std::string& key) const
		{
			identity::identity_service identities{hashes};
			auto id = identities.make_file_id(key, collisions);
			if (!id)
				return source_error("file-identity-failed");
			return id.value();
		}

		[[nodiscard]] result<source_point> point(const clang::SourceLocation location) const
		{
			const auto& sources = compiler.getSourceManager();
			const auto file_location = sources.getFileLoc(location);
			if (file_location.isInvalid())
				return source_error("invalid-source-point");
			const auto key = semantic_path(file_location);
			auto id = id_for(key);
			if (!id)
				return id.error();
			const auto presumed = sources.getPresumedLoc(file_location);
			if (!presumed.isValid())
				return source_error("invalid-presumed-point");
			const auto offset = static_cast<std::uint64_t>(sources.getFileOffset(file_location));
			const auto line = presumed.getLine();
			const auto column = presumed.getColumn();
			if (line == 0U || column == 0U)
				return source_error("zero-source-coordinate");
			return source_point::at(std::move(id.value()), offset, line, column);
		}

		[[nodiscard]] result<file_range> range(const clang::SourceRange& input,
											   const source_range_kind kind) const
		{
			if (input.isInvalid())
				return source_error("invalid-source-range");
			const auto& sources = compiler.getSourceManager();
			const auto begin_location = sources.getFileLoc(input.getBegin());
			auto end_location = sources.getFileLoc(input.getEnd());
			if (kind == source_range_kind::token)
				end_location = clang::Lexer::getLocForEndOfToken(
					end_location, 0U, sources, compiler.getLangOpts());
			if (begin_location.isInvalid() || end_location.isInvalid())
				return source_error("unmappable-source-range");
			auto begin = point(begin_location);
			auto end = point(end_location);
			if (!begin)
				return begin.error();
			if (!end)
				return end.error();
			file_range output{std::move(begin.value()), std::move(end.value()), kind};
			if (output.begin.file != output.end.file ||
				output.begin.byte_offset > output.end.byte_offset)
				return source_error("cross-file-or-reversed-range");
			return output;
		}

		[[nodiscard]] source_origin materialized_origin(const clang::SourceLocation location,
														const source_origin requested) const
		{
			if (requested == source_origin::macro_argument ||
				requested == source_origin::macro_body ||
				requested == source_origin::macro_expansion)
				return requested;
			const auto& sources = compiler.getSourceManager();
			if (sources.isWrittenInBuiltinFile(location))
				return source_origin::builtin;
			if (sources.isWrittenInScratchSpace(location))
				return source_origin::implicit_compiler_node;
			if (sources.isInSystemHeader(location))
				return source_origin::system_header;
			const auto key = semantic_path(location);
			if (generated_files.contains(key))
				return source_origin::generated_file;
			return requested;
		}

		[[nodiscard]] bool read_only(const source_origin origin) const noexcept
		{
			return origin != source_origin::directly_spelled;
		}
	};
#else
	struct source_map_adapter::impl
	{
	};
#endif

	source_map_adapter::source_map_adapter(
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

	source_map_adapter::~source_map_adapter() = default;
	source_map_adapter::source_map_adapter(source_map_adapter&&) noexcept = default;
	source_map_adapter& source_map_adapter::operator=(source_map_adapter&&) noexcept = default;

	result<normalized_source_file>
	source_map_adapter::file_for(const clang::SourceLocation& location) const
	{
#if !CXXLENS_HAS_CLANG22
		(void)location;
		return source_error("clang22-not-linked");
#else
		if (location.isInvalid())
			return source_error("invalid-file-location");
		const auto key = impl_->semantic_path(location);
		if (const auto found = impl_->files.find(key); found != impl_->files.end())
			return found->second;
		const auto& sources = impl_->compiler.getSourceManager();
		const auto file_location = sources.getFileLoc(location);
		if (file_location.isInvalid())
			return source_error("invalid-file-location");
		bool invalid = false;
		const auto buffer = sources.getBufferData(sources.getFileID(file_location), &invalid);
		if (invalid)
			return source_error("unavailable-file-buffer");
		auto digest = impl_->digest_for(file_location);
		auto id = impl_->id_for(key);
		if (!digest)
			return digest.error();
		if (!id)
			return id.error();
		std::uint32_t line = 1U;
		std::uint32_t column = 1U;
		for (const auto value : buffer)
		{
			if (value == '\n')
			{
				++line;
				column = 1U;
			}
			else
				++column;
		}
		const auto begin = source_point::at(id.value(), 0U, 1U, 1U);
		const auto end = source_point::at(id.value(), buffer.size(), line, column);
		const auto origin =
			impl_->materialized_origin(file_location, source_origin::directly_spelled);
		normalized_source_file output;
		output.id = id.value();
		output.semantic_path = key;
		output.digest = digest.value();
		output.size = buffer.size();
		output.system = origin == source_origin::system_header;
		output.generated = origin == source_origin::generated_file;
		output.builtin = origin == source_origin::builtin;
		output.whole_file.primary = {begin, end, source_range_kind::character};
		output.whole_file.origin = origin;
		output.whole_file.digest = output.digest;
		output.whole_file.read_only = impl_->read_only(origin);
		impl_->files.emplace(key, output);
		return output;
#endif
	}

	result<source_span> source_map_adapter::direct_span(const clang::SourceRange& range) const
	{
#if !CXXLENS_HAS_CLANG22
		(void)range;
		return source_error("clang22-not-linked");
#else
		auto primary = impl_->range(range, source_range_kind::token);
		if (!primary)
			return primary.error();
		auto file = file_for(range.getBegin());
		if (!file)
			return file.error();
		source_span output;
		output.primary = primary.value();
		output.origin = file.value().whole_file.origin;
		output.digest = file.value().digest;
		output.read_only = impl_->read_only(output.origin);
		if (auto invalid = output.validate())
			return source_error("invalid-direct-span");
		return output;
#endif
	}

	result<source_span> source_map_adapter::macro_span(
		const clang::SourceRange& primary, // NOLINT(bugprone-easily-swappable-parameters)
		const clang::SourceRange& invocation,
		const clang::SourceRange* definition,
		const std::string& macro_name,
		const source_origin origin,
		const std::optional<std::uint32_t> argument_index) const
	{
#if !CXXLENS_HAS_CLANG22
		(void)primary;
		(void)invocation;
		(void)definition;
		(void)macro_name;
		(void)origin;
		(void)argument_index;
		return source_error("clang22-not-linked");
#else
		if (origin != source_origin::macro_argument && origin != source_origin::macro_body &&
			origin != source_origin::macro_expansion)
			return source_error("non-macro-origin");
		auto primary_range = impl_->range(primary, source_range_kind::token);
		auto invocation_range = impl_->range(invocation, source_range_kind::token);
		if (!primary_range)
			return primary_range.error();
		if (!invocation_range)
			return invocation_range.error();
		std::optional<file_range> definition_range;
		if (definition != nullptr && definition->isValid())
		{
			auto normalized = impl_->range(*definition, source_range_kind::token);
			if (normalized)
				definition_range = std::move(normalized.value());
		}
		auto file = file_for(primary.getBegin());
		if (!file)
			return file.error();
		source_span output;
		output.primary = primary_range.value();
		output.spelling =
			definition_range ? definition_range : std::optional{primary_range.value()};
		output.expansion = invocation_range.value();
		std::vector<macro_frame> outer_frames;
		auto nested_location = invocation.getBegin();
		while (nested_location.isMacroID())
		{
			const auto immediate =
				impl_->compiler.getSourceManager().getImmediateExpansionRange(nested_location);
			auto normalized = impl_->range(immediate.getAsRange(), source_range_kind::token);
			if (!normalized)
				break;
			const auto immediate_name =
				impl_->compiler.getPreprocessor().getImmediateMacroName(nested_location).str();
			if (!immediate_name.empty())
				outer_frames.push_back(
					{immediate_name, normalized.value(), std::nullopt, std::nullopt});
			const auto next = immediate.getBegin();
			if (next == nested_location)
				break;
			nested_location = next;
		}
		std::ranges::reverse(outer_frames);
		output.macro_stack = std::move(outer_frames);
		output.macro_stack.push_back(
			{macro_name, invocation_range.value(), definition_range, argument_index});
		output.origin = origin;
		output.digest = file.value().digest;
		output.read_only = true;
		if (auto invalid = output.validate())
			return source_error("invalid-macro-span");
		return output;
#endif
	}

	std::string source_map_adapter::source_text(const clang::SourceRange& range) const
	{
#if !CXXLENS_HAS_CLANG22
		(void)range;
		return {};
#else
		bool invalid = false;
		const auto text = clang::Lexer::getSourceText(clang::CharSourceRange::getTokenRange(range),
													  impl_->compiler.getSourceManager(),
													  impl_->compiler.getLangOpts(),
													  &invalid);
		return invalid ? std::string{} : text.str();
#endif
	}

} // namespace cxxlens::detail::clang22
