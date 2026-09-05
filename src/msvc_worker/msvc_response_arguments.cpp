#include "msvc_response_arguments.hpp"

#include <cctype>
#include <cstdint>
#include <utility>

namespace cxxlens::application_analysis_worker
{
	namespace
	{
		[[nodiscard]] std::expected<std::string, msvc_response_parse_failure>
		response_text(const std::span<const std::byte> content)
		{
			std::string text;
			text.reserve(content.size());
			if (content.size() >= 2U && content[0] == std::byte{0xffU} &&
				content[1] == std::byte{0xfeU})
			{
				if (content.size() % 2U != 0U)
					return std::unexpected(msvc_response_parse_failure::invalid_encoding);
				const auto append_utf8 = [&](const std::uint32_t code_point)
				{
					if (code_point <= 0x7fU)
						text.push_back(static_cast<char>(code_point));
					else if (code_point <= 0x7ffU)
					{
						text.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
						text.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
					}
					else if (code_point <= 0xffffU)
					{
						text.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
						text.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
						text.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
					}
					else
					{
						text.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
						text.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
						text.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
						text.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
					}
				};
				for (std::size_t offset{2U}; offset < content.size(); offset += 2U)
				{
					const auto unit = static_cast<std::uint16_t>(
						std::to_integer<unsigned char>(content[offset]) |
						(static_cast<std::uint16_t>(
							 std::to_integer<unsigned char>(content[offset + 1U]))
						 << 8U));
					if (unit == 0U)
						return std::unexpected(msvc_response_parse_failure::embedded_nul);
					if (unit >= 0xdc00U && unit <= 0xdfffU)
						return std::unexpected(msvc_response_parse_failure::invalid_encoding);
					if (unit < 0xd800U || unit > 0xdbffU)
					{
						append_utf8(unit);
						continue;
					}
					if (offset + 3U >= content.size())
						return std::unexpected(msvc_response_parse_failure::invalid_encoding);
					const auto low = static_cast<std::uint16_t>(
						std::to_integer<unsigned char>(content[offset + 2U]) |
						(static_cast<std::uint16_t>(
							 std::to_integer<unsigned char>(content[offset + 3U]))
						 << 8U));
					if (low < 0xdc00U || low > 0xdfffU)
						return std::unexpected(msvc_response_parse_failure::invalid_encoding);
					append_utf8(0x10000U + ((static_cast<std::uint32_t>(unit) - 0xd800U) << 10U) +
								(static_cast<std::uint32_t>(low) - 0xdc00U));
					offset += 2U;
				}
				return text;
			}

			std::size_t offset{};
			if (content.size() >= 3U && content[0] == std::byte{0xefU} &&
				content[1] == std::byte{0xbbU} && content[2] == std::byte{0xbfU})
				offset = 3U;
			for (; offset < content.size(); ++offset)
			{
				const auto value = std::to_integer<unsigned char>(content[offset]);
				if (value == 0U)
					return std::unexpected(msvc_response_parse_failure::embedded_nul);
				text.push_back(static_cast<char>(value));
			}
			return text;
		}
	} // namespace

	std::expected<std::vector<std::string>, msvc_response_parse_failure>
	parse_msvc_response_arguments(const std::span<const std::byte> content,
								  const std::size_t maximum_arguments)
	{
		auto text = response_text(content);
		if (!text)
			return std::unexpected(text.error());

		std::vector<std::string> output;
		std::string token;
		bool quoted{};
		std::size_t slashes{};
		const auto flush = [&]() -> std::expected<void, msvc_response_parse_failure>
		{
			if (token.empty())
				return {};
			if (output.size() >= maximum_arguments)
				return std::unexpected(msvc_response_parse_failure::argument_count);
			output.push_back(std::exchange(token, {}));
			return {};
		};
		for (const char byte : *text)
		{
			if (byte == '\\')
			{
				++slashes;
				continue;
			}
			if (byte == '"')
			{
				token.append(slashes / 2U, '\\');
				if (slashes % 2U != 0U)
					token.push_back('"');
				else
					quoted = !quoted;
				slashes = 0U;
				continue;
			}
			token.append(slashes, '\\');
			slashes = 0U;
			if (!quoted && std::isspace(static_cast<unsigned char>(byte)) != 0)
			{
				if (auto added = flush(); !added)
					return std::unexpected(added.error());
			}
			else
				token.push_back(byte);
		}
		token.append(slashes, '\\');
		if (quoted)
			return std::unexpected(msvc_response_parse_failure::unterminated_quote);
		if (auto added = flush(); !added)
			return std::unexpected(added.error());
		return output;
	}
} // namespace cxxlens::application_analysis_worker
