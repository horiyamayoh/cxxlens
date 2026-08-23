#include "source_closure.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <map>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "unicode_nfc.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		constexpr std::size_t maximum_members = 4096U;
		constexpr std::size_t maximum_unique_blobs = 4096U;
		constexpr std::size_t maximum_logical_path_bytes = 4096U;
		constexpr std::uint64_t maximum_blob_bytes = std::uint64_t{16U} * 1024U * 1024U;
		constexpr std::uint64_t maximum_unique_blob_bytes =
			std::uint64_t{48U} * 1024U * 1024U;
		constexpr std::string_view project_prefix{"project://"};
		constexpr std::string_view closure_domain{"cxxlens.clang22.source-closure.v1"};

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::span<const std::byte> bytes(const std::string_view value) noexcept
		{
			return std::as_bytes(std::span{value.data(), value.size()});
		}

		[[nodiscard]] bool valid_role(const source_closure_role role) noexcept
		{
			return role >= source_closure_role::main && role <= source_closure_role::macro_file;
		}

		[[nodiscard]] bool valid_encoding(const source_closure_encoding encoding) noexcept
		{
			return encoding >= source_closure_encoding::utf8 &&
				encoding <= source_closure_encoding::binary_or_unknown;
		}

		[[nodiscard]] bool valid_content_digest(const std::string_view value) noexcept
		{
			if (value.size() != 71U || !value.starts_with("sha256:"))
				return false;
			return std::ranges::all_of(value.substr(7U),
									   [](const char byte)
									   {
										   return (byte >= '0' && byte <= '9') ||
											   (byte >= 'a' && byte <= 'f');
									   });
		}

		[[nodiscard]] std::string_view role_name(const source_closure_role role) noexcept
		{
			switch (role)
			{
				case source_closure_role::main:
					return "main";
				case source_closure_role::header:
					return "header";
				case source_closure_role::generated:
					return "generated";
				case source_closure_role::forced_include:
					return "forced-include";
				case source_closure_role::macro_file:
					return "macro-file";
			}
			return {};
		}

		[[nodiscard]] std::string_view
		encoding_name(const source_closure_encoding encoding) noexcept
		{
			switch (encoding)
			{
				case source_closure_encoding::utf8:
					return "utf8";
				case source_closure_encoding::utf16le:
					return "utf16le";
				case source_closure_encoding::utf16be:
					return "utf16be";
				case source_closure_encoding::locale_dependent:
					return "locale_dependent";
				case source_closure_encoding::binary_or_unknown:
					return "binary_or_unknown";
			}
			return {};
		}

		[[nodiscard]] sdk::result<std::string>
		portable_alias_key(const std::string_view logical_path)
		{
			auto folded = nfc_casefold_utf8(logical_path);
			if (!folded)
				return sdk::unexpected(
					failure("source-closure.path-invalid", "logical-path", folded.error().detail));
			return folded;
		}

		[[nodiscard]] sdk::result<std::string>
		compute_closure_digest(const std::vector<source_closure_member>& members,
							   const std::vector<source_closure_blob>& blobs)
		{
			std::vector<sdk::canonical_value> member_values;
			member_values.reserve(members.size());
			for (const auto& member : members)
			{
				if (member.size_bytes >
					static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
					return sdk::unexpected(failure(
						"source-closure.limit-exceeded", "member.size-bytes", member.logical_path));
				member_values.push_back(sdk::canonical_value::from_tuple({
					sdk::canonical_value::from_string(member.file_id),
					sdk::canonical_value::from_string(member.logical_path),
					sdk::canonical_value::from_string(std::string{role_name(member.role)}),
					sdk::canonical_value::from_string(std::string{encoding_name(member.encoding)}),
					sdk::canonical_value::from_integer(
						static_cast<std::int64_t>(member.size_bytes)),
					sdk::canonical_value::from_string(member.content_digest),
					sdk::canonical_value::from_boolean(member.read_only),
				}));
			}

			std::vector<sdk::canonical_value> blob_values;
			blob_values.reserve(blobs.size());
			for (const auto& blob : blobs)
			{
				if (blob.size_bytes >
					static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
					return sdk::unexpected(failure(
						"source-closure.limit-exceeded", "blob.size-bytes", blob.content_digest));
				blob_values.push_back(sdk::canonical_value::from_tuple({
					sdk::canonical_value::from_string(blob.content_digest),
					sdk::canonical_value::from_integer(static_cast<std::int64_t>(blob.size_bytes)),
				}));
			}

			auto projection = sdk::canonical_binary(sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_string(std::string{closure_domain}),
				sdk::canonical_value::from_string("unicode-default-casefold-then-nfc"),
				sdk::canonical_value::from_tuple(std::move(member_values)),
				sdk::canonical_value::from_tuple(std::move(blob_values)),
			}));
			if (!projection)
				return sdk::unexpected(std::move(projection.error()));
			std::string encoded;
			encoded.reserve(projection->size());
			for (const auto byte : *projection)
				encoded.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
			return sdk::semantic_digest(closure_domain, encoded);
		}
	} // namespace

	sdk::result<std::string> source_closure_relative_path(const std::string_view logical_path)
	{
		if (logical_path.empty() || logical_path.size() > maximum_logical_path_bytes ||
			logical_path.find('\0') != std::string_view::npos ||
			!logical_path.starts_with(project_prefix))
			return sdk::unexpected(
				failure("source-closure.path-invalid", "logical-path", std::string{logical_path}));
		if (auto valid_utf8 = sdk::validate_utf8_text(logical_path); !valid_utf8)
			return sdk::unexpected(
				failure("source-closure.path-invalid", "logical-path", valid_utf8.error().detail));
		auto normalized = is_nfc_utf8(logical_path);
		if (!normalized)
			return sdk::unexpected(
				failure("source-closure.path-invalid", "logical-path", normalized.error().detail));
		if (!*normalized)
			return sdk::unexpected(
				failure("source-closure.path-not-nfc", "logical-path", std::string{logical_path}));
		if (std::ranges::any_of(logical_path,
								[](const unsigned char character)
								{
									return character < 0x20U || character == 0x7fU;
								}))
			return sdk::unexpected(
				failure("source-closure.path-invalid", "logical-path", "control-character"));

		const auto relative = logical_path.substr(project_prefix.size());
		if (relative.empty() || relative.front() == '/' || relative.back() == '/' ||
			relative.contains('\\') || relative.contains('?') || relative.contains('#'))
			return sdk::unexpected(
				failure("source-closure.path-invalid", "logical-path", std::string{logical_path}));

		std::size_t begin{};
		while (begin <= relative.size())
		{
			const auto end = relative.find('/', begin);
			const auto segment = relative.substr(
				begin, end == std::string_view::npos ? relative.size() - begin : end - begin);
			if (segment.empty() || segment == "." || segment == "..")
				return sdk::unexpected(failure(
					"source-closure.path-invalid", "logical-path", std::string{logical_path}));
			if (end == std::string_view::npos)
				break;
			begin = end + 1U;
		}
		return std::string{relative};
	}

	sdk::result<std::string> source_closure_file_id(const std::string_view logical_path)
	{
		auto relative = source_closure_relative_path(logical_path);
		if (!relative)
			return sdk::unexpected(std::move(relative.error()));
		const std::array fields{
			sdk::canonical_value::from_string("project"),
			sdk::canonical_value::from_string(std::move(*relative)),
			sdk::canonical_value::from_string("cxxlens.logical-path.v1"),
		};
		return sdk::canonical_identity_digest("file", fields);
	}

	sdk::result<void> source_closure_blob::validate() const
	{
		if (!content)
			return sdk::unexpected(
				failure("source-closure.blob-missing", "blob.content", content_digest));
		if (size_bytes > maximum_blob_bytes)
			return sdk::unexpected(
				failure("source-closure.limit-exceeded", "blob.size-bytes", content_digest));
		if (size_bytes != content->size())
			return sdk::unexpected(
				failure("source-closure.digest-mismatch", "blob.size-bytes", content_digest));
		const auto actual = sdk::content_digest(bytes(*content));
		if (actual != content_digest)
			return sdk::unexpected(
				failure("source-closure.digest-mismatch", "blob.content-digest", content_digest));
		return {};
	}

	sdk::result<void> source_closure_member::validate() const
	{
		auto expected_file = source_closure_file_id(logical_path);
		if (!expected_file)
			return sdk::unexpected(std::move(expected_file.error()));
		if (*expected_file != file_id)
			return sdk::unexpected(
				failure("source-closure.digest-mismatch", "member.file-id", logical_path));
		if (!valid_role(role) || !valid_encoding(encoding) || !read_only)
			return sdk::unexpected(
				failure("source-closure.role-invalid", "member.metadata", logical_path));
		if (size_bytes > maximum_blob_bytes)
			return sdk::unexpected(
				failure("source-closure.limit-exceeded", "member.size-bytes", logical_path));
		if (!valid_content_digest(content_digest))
			return sdk::unexpected(
				failure("source-closure.digest-mismatch", "member.content-digest", logical_path));
		return {};
	}

	sdk::result<void> source_closure_snapshot::validate() const
	{
		if (members.empty() || members.size() > maximum_members || blobs.empty() ||
			blobs.size() > maximum_unique_blobs)
			return sdk::unexpected(failure("source-closure.limit-exceeded", "closure.census"));

		std::uint64_t aggregate_bytes{};
		std::string previous_digest;
		for (const auto& blob : blobs)
		{
			if (auto valid = blob.validate(); !valid)
				return valid;
			if (!previous_digest.empty() && previous_digest >= blob.content_digest)
				return sdk::unexpected(
					failure("source-closure.duplicate-member", "blob.order", blob.content_digest));
			previous_digest = blob.content_digest;
			if (blob.size_bytes > maximum_unique_blob_bytes - aggregate_bytes)
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", "closure.unique-blob-bytes"));
			aggregate_bytes += blob.size_bytes;
		}

		std::size_t main_count{};
		std::string previous_path;
		std::map<std::string, std::string, std::less<>> portable_aliases;
		std::map<std::string, std::size_t, std::less<>> blob_references;
		for (const auto& member : members)
		{
			if (auto valid = member.validate(); !valid)
				return valid;
			if (!previous_path.empty() && previous_path >= member.logical_path)
				return sdk::unexpected(failure(previous_path == member.logical_path
												   ? "source-closure.duplicate-member"
												   : "source-closure.path-collision",
											   "member.order",
											   member.logical_path));
			previous_path = member.logical_path;

			auto alias = portable_alias_key(member.logical_path);
			if (!alias)
				return sdk::unexpected(std::move(alias.error()));
			auto [alias_entry, inserted] =
				portable_aliases.try_emplace(std::move(*alias), member.logical_path);
			if (!inserted)
				return sdk::unexpected(failure("source-closure.case-collision",
											   "member.logical-path",
											   alias_entry->second + "|" + member.logical_path));

			const auto* blob = find_blob(member.content_digest);
			if (blob == nullptr)
				return sdk::unexpected(failure(
					"source-closure.blob-missing", "member.content-digest", member.logical_path));
			if (blob->size_bytes != member.size_bytes)
				return sdk::unexpected(failure(
					"source-closure.digest-mismatch", "member.size-bytes", member.logical_path));
			++blob_references[member.content_digest];
			if (member.role == source_closure_role::main)
				++main_count;
		}
		if (main_count != 1U)
			return sdk::unexpected(failure(
				"source-closure.main-invalid", "closure.main-count", std::to_string(main_count)));
		if (blob_references.size() != blobs.size())
			return sdk::unexpected(
				failure("source-closure.digest-mismatch", "closure.orphan-blob"));

		auto expected_digest = compute_closure_digest(members, blobs);
		if (!expected_digest)
			return sdk::unexpected(std::move(expected_digest.error()));
		if (*expected_digest != closure_digest || snapshot_id != "source-closure:" + closure_digest)
			return sdk::unexpected(failure("source-closure.digest-mismatch", "closure.identity"));
		return {};
	}

	const source_closure_member*
	source_closure_snapshot::find_member(const std::string_view logical_path) const noexcept
	{
		const auto found = std::ranges::lower_bound(
			members, logical_path, {}, &source_closure_member::logical_path);
		return found != members.end() && found->logical_path == logical_path ? &*found : nullptr;
	}

	const source_closure_blob*
	source_closure_snapshot::find_blob(const std::string_view content_digest_value) const noexcept
	{
		const auto found = std::ranges::lower_bound(
			blobs, content_digest_value, {}, &source_closure_blob::content_digest);
		return found != blobs.end() && found->content_digest == content_digest_value ? &*found
																					 : nullptr;
	}

	sdk::result<source_closure_snapshot>
	make_source_closure_snapshot(std::vector<source_closure_file_input> files)
	{
		if (files.empty() || files.size() > maximum_members)
			return sdk::unexpected(
				failure("source-closure.limit-exceeded", "closure.member-count"));

		source_closure_snapshot output;
		output.members.reserve(files.size());
		std::map<std::string, source_closure_blob, std::less<>> unique_blobs;
		for (auto& file : files)
		{
			if (!file.content)
				return sdk::unexpected(
					failure("source-closure.blob-missing", "file.content", file.logical_path));
			auto file_id = source_closure_file_id(file.logical_path);
			if (!file_id)
				return sdk::unexpected(std::move(file_id.error()));
			if (!valid_role(file.role) || !valid_encoding(file.encoding))
				return sdk::unexpected(
					failure("source-closure.role-invalid", "file.metadata", file.logical_path));
			if (file.content->size() > maximum_blob_bytes)
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", "file.size-bytes", file.logical_path));

			const auto digest = sdk::content_digest(bytes(*file.content));
			const auto size = static_cast<std::uint64_t>(file.content->size());
			auto sealed_content = std::make_shared<const std::string>(*file.content);
			auto [blob, inserted] = unique_blobs.try_emplace(
				digest, source_closure_blob{digest, size, std::move(sealed_content)});
			if (!inserted &&
				(blob->second.size_bytes != size || *blob->second.content != *file.content))
				return sdk::unexpected(
					failure("source-closure.digest-mismatch", "blob.conflicting-content", digest));
			output.members.push_back({
				std::move(*file_id),
				std::move(file.logical_path),
				file.role,
				file.encoding,
				size,
				digest,
				true,
			});
		}

		std::ranges::sort(output.members, {}, &source_closure_member::logical_path);
		output.blobs.reserve(unique_blobs.size());
		for (auto& [digest, blob] : unique_blobs)
		{
			(void)digest;
			output.blobs.push_back(std::move(blob));
		}
		auto digest = compute_closure_digest(output.members, output.blobs);
		if (!digest)
			return sdk::unexpected(std::move(digest.error()));
		output.closure_digest = std::move(*digest);
		output.snapshot_id = "source-closure:" + output.closure_digest;
		if (auto valid = output.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return output;
	}
} // namespace cxxlens::detail::clang22
