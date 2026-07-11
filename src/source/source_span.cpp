#include <algorithm>

#include <cxxlens/source.hpp>

#include "../core/json_projections.hpp"

namespace cxxlens
{
	namespace
	{
		bool valid_range(const file_range& range) noexcept
		{
			return range.begin.state == source_location_state::valid &&
				range.end.state == source_location_state::valid && range.begin.file.valid() &&
				range.end.file.valid() && range.begin.line != 0U && range.begin.column != 0U &&
				range.end.line != 0U && range.end.column != 0U &&
				range.begin.file == range.end.file &&
				range.begin.byte_offset <= range.end.byte_offset;
		}

		std::optional<source_validation_error> validate_range(const file_range& range,
															  const std::string& field)
		{
			if (range.begin.state != source_location_state::valid ||
				range.end.state != source_location_state::valid || range.begin.line == 0U ||
				range.begin.column == 0U || range.end.line == 0U || range.end.column == 0U)
			{
				return source_validation_error{source_validation_code::invalid_point_coordinates,
											   field};
			}
			if (!range.begin.file.valid() || !range.end.file.valid())
			{
				return source_validation_error{source_validation_code::invalid_semantic_file_key,
											   field};
			}
			if (range.begin.file != range.end.file)
			{
				return source_validation_error{source_validation_code::different_files, field};
			}
			if (range.begin.byte_offset > range.end.byte_offset)
			{
				return source_validation_error{source_validation_code::reversed_range, field};
			}
			return std::nullopt;
		}

		detail::json::json_value range_value(const file_range& range)
		{
			using detail::json::json_value;
			return json_value::object{
				{"file", std::string{range.begin.file.value()}},
				{"begin", range.begin.byte_offset},
				{"end", range.end.byte_offset},
				{"begin_line", static_cast<std::uint64_t>(range.begin.line)},
				{"begin_column", static_cast<std::uint64_t>(range.begin.column)},
				{"end_line", static_cast<std::uint64_t>(range.end.line)},
				{"end_column", static_cast<std::uint64_t>(range.end.column)},
				{"kind", range.kind == source_range_kind::token ? "token" : "character"},
			};
		}

		const char* origin_name(const source_origin origin)
		{
			switch (origin)
			{
				case source_origin::directly_spelled:
					return "directly_spelled";
				case source_origin::macro_argument:
					return "macro_argument";
				case source_origin::macro_body:
					return "macro_body";
				case source_origin::macro_expansion:
					return "macro_expansion";
				case source_origin::implicit_compiler_node:
					return "implicit_compiler_node";
				case source_origin::generated_file:
					return "generated_file";
				case source_origin::system_header:
					return "system_header";
				case source_origin::builtin:
					return "builtin";
				case source_origin::unknown:
					return "unknown";
			}
			return "unknown";
		}

		bool identifier_start(const char value) noexcept
		{
			return value == '_' || (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
		}

		bool valid_macro_name(const std::string_view name) noexcept
		{
			if (name.empty() || !identifier_start(name.front()))
			{
				return false;
			}
			return std::ranges::all_of(name.substr(1U),
									   [](const char value)
									   {
										   return value == '_' || (value >= 'A' && value <= 'Z') ||
											   (value >= 'a' && value <= 'z') ||
											   (value >= '0' && value <= '9');
									   });
		}

		bool valid_digest(const source_digest& digest) noexcept
		{
			return !digest.algorithm.empty() && digest.version != 0U && !digest.value.empty() &&
				std::ranges::all_of(digest.algorithm,
									[](const char value)
									{
										return (value >= 'a' && value <= 'z') ||
											(value >= '0' && value <= '9') || value == '-' ||
											value == '_';
									}) &&
				std::ranges::all_of(digest.value,
									[](const char value)
									{
										return (value >= '0' && value <= '9') ||
											(value >= 'a' && value <= 'f');
									});
		}
	} // namespace

	source_point source_point::at(file_id file,
								  const std::uint64_t byte_offset,
								  const std::uint32_t line,
								  const std::uint32_t utf8_byte_column)
	{
		return {std::move(file), byte_offset, line, utf8_byte_column, source_location_state::valid};
	}
	source_point source_point::invalid() noexcept
	{
		return {};
	}
	source_point source_point::unknown() noexcept
	{
		auto result = source_point{};
		result.state = source_location_state::unknown;
		return result;
	}

	std::optional<source_validation_error> source_span::validate() const
	{
		if (auto error = validate_range(primary, "primary"))
			return error;
		if (spelling)
			if (auto error = validate_range(*spelling, "spelling"))
				return error;
		if (expansion)
			if (auto error = validate_range(*expansion, "expansion"))
				return error;
		if (!valid_digest(digest))
			return source_validation_error{source_validation_code::invalid_digest, "digest"};
		for (const auto& frame : macro_stack)
		{
			if (!valid_macro_name(frame.macro_name) ||
				validate_range(frame.invocation, "macro.invocation") ||
				(frame.definition && validate_range(*frame.definition, "macro.definition")))
			{
				return source_validation_error{source_validation_code::invalid_macro_frame,
											   "macro_stack"};
			}
		}
		const bool macro_origin = origin == source_origin::macro_argument ||
			origin == source_origin::macro_body || origin == source_origin::macro_expansion;
		if (macro_origin != !macro_stack.empty())
			return source_validation_error{source_validation_code::origin_stack_mismatch, "origin"};
		return std::nullopt;
	}

	bool source_span::is_directly_editable() const noexcept
	{
		return !read_only && origin == source_origin::directly_spelled && macro_stack.empty() &&
			valid_range(primary) && valid_digest(digest);
	}

	detail::json::json_value detail::json::source_span_value(const source_span& span)
	{
		json_value::array frames;
		frames.reserve(span.macro_stack.size());
		for (const auto& frame : span.macro_stack)
		{
			frames.emplace_back(json_value::object{
				{"name", frame.macro_name},
				{"invocation", range_value(frame.invocation)},
				{"definition", frame.definition ? range_value(*frame.definition) : json_value{}},
				{"argument_index",
				 frame.argument_index
					 ? json_value{static_cast<std::uint64_t>(*frame.argument_index)}
					 : json_value{}},
			});
		}
		json_value::object fields{
			{"primary", range_value(span.primary)},
			{"spelling", span.spelling ? range_value(*span.spelling) : json_value{}},
			{"expansion", span.expansion ? range_value(*span.expansion) : json_value{}},
			{"macro_stack", std::move(frames)},
			{"origin", origin_name(span.origin)},
			{"digest",
			 json_value::object{{"algorithm", span.digest.algorithm},
								{"version", static_cast<std::uint64_t>(span.digest.version)},
								{"value", span.digest.value}}},
			{"read_only", span.read_only},
		};
		return json_value{envelope(document_versions{"cxxlens.source-span.v1"}, std::move(fields))};
	}

	std::string source_span::to_canonical_json() const
	{
		return detail::json::write(detail::json::source_span_value(*this)).value();
	}
} // namespace cxxlens
