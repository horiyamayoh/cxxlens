#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::detail::clang22
{
	/**
	 * Product identity domains for the Protocol 2.0 source-closure boundary.
	 *
	 * The closure digest is the identity of the authenticated Clang 22 source snapshot.  The
	 * manifest schema/digest are deliberately separate because they bind the canonical JSON wire
	 * projection and must not be confused with the source-content identity.
	 */
	inline constexpr std::string_view source_closure_digest_domain =
		"cxxlens.clang22.source-closure.v1";
	inline constexpr std::string_view source_closure_manifest_schema =
		"cxxlens.source-closure-manifest.v1";
	inline constexpr std::string_view source_closure_manifest_digest_domain =
		"cxxlens.source-closure-manifest.v1";

	/** Closed semantic role of one compiler-visible source-closure member. */
	enum class source_closure_role : std::uint8_t
	{
		main = 1,
		header = 2,
		generated = 3,
		forced_include = 4,
		macro_file = 5,
	};

	/** Exact source byte interpretation retained as member metadata. */
	enum class source_closure_encoding : std::uint8_t
	{
		utf8 = 1,
		utf16le = 2,
		utf16be = 3,
		locale_dependent = 4,
		binary_or_unknown = 5,
	};

	/** Immutable content-addressed bytes shared by one or more closure members. */
	struct source_closure_blob
	{
		std::string content_digest;
		std::uint64_t size_bytes{};
		std::shared_ptr<const std::string> content;

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/** One ordered canonical logical-path-to-blob binding in a source closure. */
	struct source_closure_member
	{
		std::string file_id;
		std::string logical_path;
		source_closure_role role{source_closure_role::header};
		source_closure_encoding encoding{source_closure_encoding::utf8};
		std::uint64_t size_bytes{};
		std::string content_digest;
		bool read_only{true};

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/** Builder input; immutable content may be shared across tasks and members. */
	struct source_closure_file_input
	{
		std::string logical_path;
		source_closure_role role{source_closure_role::header};
		source_closure_encoding encoding{source_closure_encoding::utf8};
		std::shared_ptr<const std::string> content;
	};

	/**
	 * Digest-addressed immutable compiler input snapshot. Members and blobs are stored in their
	 * canonical identity order and are independent from transfer order or physical staging.
	 */
	struct source_closure_snapshot
	{
		std::string snapshot_id;
		std::string closure_digest;
		std::vector<source_closure_member> members;
		std::vector<source_closure_blob> blobs;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] const source_closure_member*
		find_member(std::string_view logical_path) const noexcept;
		[[nodiscard]] const source_closure_blob*
		find_blob(std::string_view content_digest) const noexcept;
	};

	/** Validate and return the path relative to the strict `project://` logical root. */
	[[nodiscard]] sdk::result<std::string>
	source_closure_relative_path(std::string_view logical_path);

	/** Derive the canonical source.file identity for one admitted logical path. */
	[[nodiscard]] sdk::result<std::string> source_closure_file_id(std::string_view logical_path);

	/**
	 * Derive the authenticated byte line-index identity for the closure's unique main blob.
	 *
	 * The offsets are byte offsets for the first line and for the byte after every LF.  A trailing
	 * LF therefore contributes the EOF offset, while the bytes and their content digest remain the
	 * authority for the result.
	 */
	[[nodiscard]] sdk::result<std::string>
	source_closure_main_line_index_id(const source_closure_snapshot& snapshot);

	/** Construct, sort, deduplicate, identify, and fully validate one source closure. */
	[[nodiscard]] sdk::result<source_closure_snapshot>
	make_source_closure_snapshot(std::vector<source_closure_file_input> files);
} // namespace cxxlens::detail::clang22
