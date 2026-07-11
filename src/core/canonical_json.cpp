#include "canonical_json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace cxxlens::detail::json
{
	namespace
	{
		[[nodiscard]] error json_error(std::string field, std::string reason)
		{
			error failure;
			failure.code.value = "core.schema-validation-failed";
			failure.message = "canonical JSON validation failed";
			failure.attributes.emplace("field", std::move(field));
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		void append_escaped(const std::string_view value, std::string& output)
		{
			constexpr std::array hexadecimal{
				'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
			output.push_back('"');
			for (const char character : value)
			{
				const auto byte = static_cast<unsigned char>(character);
				switch (byte)
				{
					case '"':
						output += R"(\")";
						break;
					case '\\':
						output += R"(\\)";
						break;
					case '\b':
						output += R"(\b)";
						break;
					case '\f':
						output += R"(\f)";
						break;
					case '\n':
						output += R"(\n)";
						break;
					case '\r':
						output += R"(\r)";
						break;
					case '\t':
						output += R"(\t)";
						break;
					default:
						if (byte < 0x20U)
						{
							output += R"(\u00)";
							output.push_back(hexadecimal.at(byte >> 4U));
							output.push_back(hexadecimal.at(byte & 0x0FU));
						}
						else
							output.push_back(static_cast<char>(byte));
						break;
				}
			}
			output.push_back('"');
		}

		[[nodiscard]] result<void> append_value(const json_value& input,
												std::string& output,
												const std::size_t depth,
												const std::size_t maximum_depth)
		{
			if (depth > maximum_depth)
				return json_error("value", "maximum-depth-exceeded");
			if (std::holds_alternative<null_value>(input.value))
				output += "null";
			else if (const auto* boolean = std::get_if<bool>(&input.value))
				output += *boolean ? "true" : "false";
			else if (const auto* signed_integer = std::get_if<std::int64_t>(&input.value))
			{
				std::array<char, 32U> buffer{};
				const auto converted =
					std::to_chars(buffer.data(), buffer.data() + buffer.size(), *signed_integer);
				output.append(buffer.data(), converted.ptr);
			}
			else if (const auto* unsigned_integer = std::get_if<std::uint64_t>(&input.value))
			{
				std::array<char, 32U> buffer{};
				const auto converted =
					std::to_chars(buffer.data(), buffer.data() + buffer.size(), *unsigned_integer);
				output.append(buffer.data(), converted.ptr);
			}
			else if (const auto* number = std::get_if<double>(&input.value))
			{
				if (!std::isfinite(*number))
					return json_error("number", "non-finite-number");
				if (*number == 0.0)
				{
					output.push_back('0');
					return {};
				}
				std::array<char, 64U> buffer{};
				const auto converted = std::to_chars(buffer.data(),
													 buffer.data() + buffer.size(),
													 *number,
													 std::chars_format::general,
													 std::numeric_limits<double>::max_digits10);
				if (converted.ec != std::errc{})
					return json_error("number", "number-encoding-failed");
				output.append(buffer.data(), converted.ptr);
			}
			else if (const auto* string = std::get_if<std::string>(&input.value))
			{
				if (!valid_utf8(*string))
					return json_error("string", "invalid-utf8");
				append_escaped(*string, output);
			}
			else if (const auto* array = std::get_if<json_value::array>(&input.value))
			{
				output.push_back('[');
				for (std::size_t index = 0U; index < array->size(); ++index)
				{
					if (index != 0U)
						output.push_back(',');
					if (auto result =
							append_value(array->at(index), output, depth + 1U, maximum_depth);
						!result)
						return result;
				}
				output.push_back(']');
			}
			else
			{
				auto object = std::get<json_value::object>(input.value);
				std::ranges::sort(object, {}, &std::pair<std::string, json_value>::first);
				for (std::size_t index = 1U; index < object.size(); ++index)
				{
					if (object.at(index - 1U).first == object.at(index).first)
						return json_error("object", "duplicate-key");
				}
				output.push_back('{');
				for (std::size_t index = 0U; index < object.size(); ++index)
				{
					if (index != 0U)
						output.push_back(',');
					if (!valid_utf8(object.at(index).first))
						return json_error("object.key", "invalid-utf8");
					append_escaped(object.at(index).first, output);
					output.push_back(':');
					if (auto result = append_value(
							object.at(index).second, output, depth + 1U, maximum_depth);
						!result)
						return result;
				}
				output.push_back('}');
			}
			return {};
		}
	} // namespace

	json_value::object envelope(document_versions versions, json_value::object semantic_fields)
	{
		semantic_fields.emplace_back("schema", std::move(versions.schema));
		semantic_fields.emplace_back("semantics_version", std::move(versions.semantics));
		semantic_fields.emplace_back("library_version", std::move(versions.library));
		return semantic_fields;
	}

	result<std::string> write(const json_value& value, const std::size_t maximum_depth)
	{
		std::string output;
		if (auto result = append_value(value, output, 0U, maximum_depth); !result)
			return std::move(result.error());
		return output;
	}

	bool valid_utf8(const std::string_view value) noexcept
	{
		for (std::size_t index = 0U; index < value.size();)
		{
			const auto first = static_cast<unsigned char>(value.at(index));
			if (first <= 0x7FU)
			{
				++index;
				continue;
			}
			std::size_t continuation_count = 0U;
			std::uint32_t code_point = 0U;
			if (first >= 0xC2U && first <= 0xDFU)
			{
				continuation_count = 1U;
				code_point = first & 0x1FU;
			}
			else if (first >= 0xE0U && first <= 0xEFU)
			{
				continuation_count = 2U;
				code_point = first & 0x0FU;
			}
			else if (first >= 0xF0U && first <= 0xF4U)
			{
				continuation_count = 3U;
				code_point = first & 0x07U;
			}
			else
				return false;
			if (index + continuation_count >= value.size())
				return false;
			for (std::size_t offset = 1U; offset <= continuation_count; ++offset)
			{
				const auto continuation = static_cast<unsigned char>(value.at(index + offset));
				if ((continuation & 0xC0U) != 0x80U)
					return false;
				code_point = (code_point << 6U) | (continuation & 0x3FU);
			}
			if ((continuation_count == 2U && code_point < 0x800U) ||
				(continuation_count == 3U && code_point < 0x10000U) || code_point > 0x10FFFFU ||
				(code_point >= 0xD800U && code_point <= 0xDFFFU))
				return false;
			index += continuation_count + 1U;
		}
		return true;
	}
} // namespace cxxlens::detail::json
