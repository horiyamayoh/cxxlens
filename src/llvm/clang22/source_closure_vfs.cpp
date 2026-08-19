#include "source_closure_vfs.hpp"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "unicode_nfc.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		constexpr std::string_view project_prefix{"project://"};

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::result<void> validate_include_spelling(const std::string_view value)
		{
			if (value.empty() || value.size() > 4096U ||
				value.find('\0') != std::string_view::npos || value.contains('\\') ||
				value.contains('?') || value.contains('#'))
				return sdk::unexpected(
					failure("source-closure.path-invalid", "include-path", std::string{value}));
			if (auto valid_utf8 = sdk::validate_utf8_text(value); !valid_utf8)
				return sdk::unexpected(failure(
					"source-closure.path-invalid", "include-path", valid_utf8.error().detail));
			auto normalized = is_nfc_utf8(value);
			if (!normalized)
				return sdk::unexpected(failure(
					"source-closure.path-invalid", "include-path", normalized.error().detail));
			if (!*normalized)
				return sdk::unexpected(
					failure("source-closure.path-not-nfc", "include-path", std::string{value}));
			if (std::ranges::any_of(value,
									[](const unsigned char character)
									{
										return character < 0x20U || character == 0x7fU;
									}))
				return sdk::unexpected(
					failure("source-closure.path-invalid", "include-path", "control-character"));
			return {};
		}

		[[nodiscard]] sdk::result<std::vector<std::string>>
		normalize_relative_segments(const std::string_view base_directory,
									const std::string_view requested)
		{
			std::vector<std::string> segments;
			auto append = [&segments](const std::string_view path,
									  const bool allow_parent) -> sdk::result<void>
			{
				std::size_t begin{};
				while (begin <= path.size())
				{
					const auto end = path.find('/', begin);
					const auto segment = path.substr(
						begin, end == std::string_view::npos ? path.size() - begin : end - begin);
					if (segment.empty() || segment == ".")
					{
						if (segment.empty())
							return sdk::unexpected(failure(
								"source-closure.path-invalid", "include-path", std::string{path}));
					}
					else if (segment == "..")
					{
						if (!allow_parent || segments.empty())
							return sdk::unexpected(failure("source-closure.ambient-fallback-denied",
														   "include-path",
														   std::string{path}));
						segments.pop_back();
					}
					else
					{
						segments.emplace_back(segment);
					}
					if (end == std::string_view::npos)
						break;
					begin = end + 1U;
				}
				return {};
			};

			if (!base_directory.empty())
				if (auto base = append(base_directory, false); !base)
					return sdk::unexpected(std::move(base.error()));
			if (auto relative = append(requested, true); !relative)
				return sdk::unexpected(std::move(relative.error()));
			if (segments.empty())
				return sdk::unexpected(failure(
					"source-closure.member-missing", "include-path", std::string{requested}));
			return segments;
		}

		[[nodiscard]] std::string join_segments(const std::vector<std::string>& segments)
		{
			std::string output;
			for (const auto& segment : segments)
			{
				if (!output.empty())
					output.push_back('/');
				output.append(segment);
			}
			return output;
		}

		[[nodiscard]] std::string parent_path(const std::string_view relative)
		{
			const auto separator = relative.rfind('/');
			return separator == std::string_view::npos
				? std::string{}
				: std::string{relative.substr(0U, separator)};
		}
	} // namespace

	source_closure_vfs::source_closure_vfs(source_closure_snapshot closure)
		: closure_{std::move(closure)}
	{
	}

	sdk::result<source_closure_vfs> source_closure_vfs::mount(source_closure_snapshot closure)
	{
		if (auto valid = closure.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return source_closure_vfs{std::move(closure)};
	}

	sdk::result<std::string>
	source_closure_vfs::logical_path(const std::string_view compiler_path) const
	{
		if (compiler_path.starts_with(project_prefix))
		{
			if (auto relative = source_closure_relative_path(compiler_path); !relative)
				return sdk::unexpected(std::move(relative.error()));
			return std::string{compiler_path};
		}
		if (!compiler_path.starts_with(synthetic_root()))
			return sdk::unexpected(failure("source-closure.toolchain-input-unqualified",
										   "compiler-path",
										   std::string{compiler_path}));
		const auto suffix = compiler_path.substr(synthetic_root().size());
		if (suffix.empty() || suffix.front() != '/')
			return sdk::unexpected(failure(
				"source-closure.path-invalid", "compiler-path", std::string{compiler_path}));
		const auto relative = suffix.substr(1U);
		if (relative.empty())
			return sdk::unexpected(failure(
				"source-closure.member-missing", "compiler-path", std::string{compiler_path}));
		const auto logical = std::string{project_prefix} + std::string{relative};
		if (auto admitted = source_closure_relative_path(logical); !admitted)
			return sdk::unexpected(std::move(admitted.error()));
		return logical;
	}

	sdk::result<source_closure_vfs_file>
	source_closure_vfs::open_logical(std::string logical_path) const
	{
		const auto* member = closure_.find_member(logical_path);
		if (member == nullptr)
			return sdk::unexpected(
				failure("source-closure.member-missing", "logical-path", std::move(logical_path)));
		const auto* blob = closure_.find_blob(member->content_digest);
		if (blob == nullptr || !blob->content)
			return sdk::unexpected(
				failure("source-closure.blob-missing", "logical-path", member->logical_path));
		auto relative = source_closure_relative_path(member->logical_path);
		if (!relative)
			return sdk::unexpected(std::move(relative.error()));
		return source_closure_vfs_file{
			member->logical_path,
			std::string{synthetic_root()} + "/" + *relative,
			member->role,
			member->encoding,
			blob->content,
		};
	}

	sdk::result<source_closure_vfs_file>
	source_closure_vfs::open(const std::string_view compiler_path) const
	{
		auto logical = logical_path(compiler_path);
		if (!logical)
			return sdk::unexpected(std::move(logical.error()));
		return open_logical(std::move(*logical));
	}

	sdk::result<source_closure_vfs_file>
	source_closure_vfs::open_relative(const std::string_view include_path,
									  const std::string_view including_logical_path) const
	{
		if (auto valid = validate_include_spelling(include_path); !valid)
			return sdk::unexpected(std::move(valid.error()));
		auto including_relative = source_closure_relative_path(including_logical_path);
		if (!including_relative)
			return sdk::unexpected(std::move(including_relative.error()));

		if (include_path.starts_with(project_prefix) || include_path.starts_with(synthetic_root()))
			return open(include_path);
		if (include_path.front() == '/')
			return sdk::unexpected(failure("source-closure.toolchain-input-unqualified",
										   "include-path",
										   std::string{include_path}));

		auto segments = normalize_relative_segments(parent_path(*including_relative), include_path);
		if (!segments)
			return sdk::unexpected(std::move(segments.error()));
		return open_logical(std::string{project_prefix} + join_segments(*segments));
	}

	sdk::result<std::vector<std::string>>
	source_closure_vfs::list_directory(const std::string_view compiler_path) const
	{
		std::string relative_directory;
		if (compiler_path == synthetic_root() || compiler_path == project_prefix)
		{
			relative_directory.clear();
		}
		else
		{
			auto logical = logical_path(compiler_path);
			if (!logical)
				return sdk::unexpected(std::move(logical.error()));
			auto relative = source_closure_relative_path(*logical);
			if (!relative)
				return sdk::unexpected(std::move(relative.error()));
			relative_directory = std::move(*relative);
		}

		const std::string prefix =
			relative_directory.empty() ? std::string{} : relative_directory + "/";
		std::vector<std::string> children;
		for (const auto& member : closure_.members)
		{
			auto relative = source_closure_relative_path(member.logical_path);
			if (!relative || !relative->starts_with(prefix))
				continue;
			const auto remainder = std::string_view{*relative}.substr(prefix.size());
			if (remainder.empty())
				continue;
			const auto separator = remainder.find('/');
			const auto child = remainder.substr(
				0U, separator == std::string_view::npos ? remainder.size() : separator);
			const auto synthetic =
				std::string{synthetic_root()} + "/" + prefix + std::string{child};
			children.push_back(synthetic);
		}
		std::ranges::sort(children);
		children.erase(std::unique(children.begin(), children.end()), children.end());
		if (children.empty())
			return sdk::unexpected(
				failure("source-closure.member-missing", "directory", std::string{compiler_path}));
		return children;
	}

	sdk::result<void> source_closure_vfs::deny_write(const std::string_view compiler_path) const
	{
		return sdk::unexpected(failure("source-closure.ambient-fallback-denied",
									   "write-operation",
									   std::string{compiler_path}));
	}

	const source_closure_snapshot& source_closure_vfs::closure() const noexcept
	{
		return closure_;
	}
} // namespace cxxlens::detail::clang22
