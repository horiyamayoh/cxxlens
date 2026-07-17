#pragma once

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace cxxlens::sdk::detail
{
	[[nodiscard]] inline bool valid_utf8(const std::string_view input) noexcept
	{
		std::size_t index{};
		while (index < input.size())
		{
			const auto first = static_cast<unsigned char>(input[index]);
			if (first <= 0x7fU)
			{
				++index;
				continue;
			}
			std::size_t width{};
			std::uint32_t code_point{};
			std::uint32_t minimum{};
			if (first >= 0xc2U && first <= 0xdfU)
			{
				width = 2U;
				code_point = first & 0x1fU;
				minimum = 0x80U;
			}
			else if (first >= 0xe0U && first <= 0xefU)
			{
				width = 3U;
				code_point = first & 0x0fU;
				minimum = 0x800U;
			}
			else if (first >= 0xf0U && first <= 0xf4U)
			{
				width = 4U;
				code_point = first & 0x07U;
				minimum = 0x10000U;
			}
			else
				return false;
			if (index + width > input.size())
				return false;
			for (std::size_t offset = 1U; offset < width; ++offset)
			{
				const auto continuation = static_cast<unsigned char>(input[index + offset]);
				if ((continuation & 0xc0U) != 0x80U)
					return false;
				code_point = (code_point << 6U) | (continuation & 0x3fU);
			}
			if (code_point < minimum || code_point > 0x10ffffU ||
				(code_point >= 0xd800U && code_point <= 0xdfffU))
				return false;
			index += width;
		}
		return true;
	}

	inline void append_json_hex_escape(std::ostringstream& output, const unsigned char byte)
	{
		output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
			   << static_cast<unsigned int>(byte) << std::dec;
	}

	[[nodiscard]] inline std::string canonical_json_escape(const std::string_view input)
	{
		std::ostringstream output;
		if (!valid_utf8(input))
			return {};
		for (const auto byte : input)
		{
			const auto unsigned_byte = static_cast<unsigned char>(byte);
			switch (byte)
			{
				case '\\':
					output << "\\\\";
					break;
				case '"':
					output << "\\\"";
					break;
				case '\b':
					output << "\\b";
					break;
				case '\f':
					output << "\\f";
					break;
				case '\n':
					output << "\\n";
					break;
				case '\r':
					output << "\\r";
					break;
				case '\t':
					output << "\\t";
					break;
				default:
					if (unsigned_byte < 0x20U)
						append_json_hex_escape(output, unsigned_byte);
					else
						output << byte;
			}
		}
		return output.str();
	}

	[[nodiscard]] inline std::string canonical_json_string(const std::string_view value)
	{
		return "\"" + canonical_json_escape(value) + "\"";
	}
} // namespace cxxlens::sdk::detail
