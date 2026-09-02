#include "source_identity_internal.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error invalid(std::string field, std::string detail)
		{
			return {"source-identity.invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool relative_logical_path(const std::string_view value) noexcept
		{
			if (value.empty() || value.starts_with('/') || value.ends_with('/') ||
				value.starts_with("project://"))
				return false;
			std::size_t offset{};
			while (offset < value.size())
			{
				const auto next = value.find('/', offset);
				const auto segment = value.substr(
					offset, next == std::string_view::npos ? value.size() - offset : next - offset);
				if (segment.empty() || segment == "." || segment == "..")
					return false;
				if (next == std::string_view::npos)
					break;
				offset = next + 1U;
			}
			return true;
		}

		[[nodiscard]] bool digest_like(const std::string_view value) noexcept
		{
			return value.size() == 71U && value.starts_with("sha256:") &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] bool source_encoding(const std::string_view value) noexcept
		{
			return value == "utf8" || value == "utf16le" || value == "utf16be" ||
				value == "locale_dependent" || value == "binary_or_unknown";
		}
	} // namespace

	result<std::string> derive_source_file_id(const std::string_view project_relative_logical_path)
	{
		if (!relative_logical_path(project_relative_logical_path) ||
			!validate_utf8_text(project_relative_logical_path))
			return unexpected(invalid("logical_path", "project-relative-canonical-path"));
		const std::array fields{
			canonical_value::from_string("project"),
			canonical_value::from_string(std::string{project_relative_logical_path}),
			canonical_value::from_string("cxxlens.logical-path.v1"),
		};
		return canonical_identity_digest("file", fields);
	}

	result<std::string> derive_source_snapshot_id(const std::string_view file_id,
												  const std::string_view content_digest_value,
												  const std::string_view encoding)
	{
		if (!file_id.starts_with("file:") || !validate_strong_id(file_id))
			return unexpected(invalid("file_id", "strong-file-id"));
		if (!digest_like(content_digest_value))
			return unexpected(invalid("content_digest", "sha256"));
		if (!source_encoding(encoding))
			return unexpected(invalid("encoding", "enum"));
		const std::array fields{
			canonical_value::from_string(std::string{file_id}),
			canonical_value::from_string(std::string{content_digest_value}),
			canonical_value::from_string(std::string{encoding}),
		};
		return canonical_identity_digest("source-snapshot", fields);
	}
} // namespace cxxlens::sdk::detail
