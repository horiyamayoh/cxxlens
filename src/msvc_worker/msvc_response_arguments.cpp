#include "msvc_response_arguments.hpp"

#include <cctype>
#include <utility>

namespace cxxlens::application_analysis_worker
{
	std::expected<std::vector<std::string>, msvc_response_parse_failure>
	parse_msvc_response_arguments(const std::span<const std::byte> content,
								  const std::size_t maximum_arguments)
	{
		std::string text;
		text.reserve(content.size());
		for (const auto byte : content)
		{
			const auto value = std::to_integer<unsigned char>(byte);
			if (value == 0U)
				return std::unexpected(msvc_response_parse_failure::embedded_nul);
			text.push_back(static_cast<char>(value));
		}

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
		for (const char byte : text)
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
