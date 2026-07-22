#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <cxxlens/sdk/common.hpp>

#include "sqlite_payload_streaming_internal.hpp"

namespace cxxlens::sdk
{
	inline constexpr std::uint32_t sqlite_wal_little_endian_checksum_magic = 0x377f0682U;
	inline constexpr std::uint32_t sqlite_wal_big_endian_checksum_magic = 0x377f0683U;
	inline constexpr std::uint32_t sqlite_wal_format_version = 3'007'000U;
	inline constexpr std::uint32_t sqlite_wal_minimum_page_size = 512U;
	inline constexpr std::uint32_t sqlite_wal_maximum_page_size = 65'536U;
	inline constexpr std::uint64_t sqlite_wal_header_byte_count = 32U;
	inline constexpr std::uint64_t sqlite_wal_frame_header_byte_count = 24U;
	inline constexpr std::size_t sqlite_wal_parser_resident_byte_bound =
		static_cast<std::size_t>(sqlite_wal_header_byte_count + sqlite_wal_frame_header_byte_count +
								 sqlite_wal_maximum_page_size);

	enum class sqlite_wal_checksum_byte_order : std::uint8_t
	{
		little_endian,
		big_endian,
	};

	/** Whether any prefix is safe to pass to private SQLite recovery. */
	enum class sqlite_wal_scan_classification : std::uint8_t
	{
		empty,
		invalid_header,
		no_valid_commit,
		committed_prefix,
		accounting_overflow,
	};

	/** The first terminal condition observed. Scanning never resumes after this boundary. */
	enum class sqlite_wal_scan_stop : std::uint8_t
	{
		end_of_input,
		torn_header,
		invalid_magic,
		unsupported_format_version,
		invalid_page_size,
		header_checksum_mismatch,
		torn_frame,
		frame_salt_mismatch,
		frame_page_number_zero,
		frame_checksum_mismatch,
		counter_overflow,
	};

	struct sqlite_wal_header_receipt
	{
		std::uint32_t magic{};
		std::uint32_t format_version{};
		std::uint32_t page_size{};
		std::uint32_t checkpoint_sequence{};
		std::uint32_t salt_1{};
		std::uint32_t salt_2{};
		std::uint32_t checksum_1{};
		std::uint32_t checksum_2{};
		sqlite_wal_checksum_byte_order checksum_byte_order{};

		[[nodiscard]] bool operator==(const sqlite_wal_header_receipt&) const = default;
	};

	/** One complete valid frame. `frame_number` is one-based. */
	struct sqlite_wal_frame_receipt
	{
		std::uint64_t frame_number{};
		std::uint32_t page_number{};
		std::uint32_t database_page_count{};
		std::uint32_t checksum_1{};
		std::uint32_t checksum_2{};
		std::uint64_t prefix_byte_count{};

		[[nodiscard]] bool operator==(const sqlite_wal_frame_receipt&) const = default;
	};

	/**
	 * Closed WAL scan evidence.
	 *
	 * `validated_prefix_byte_count` includes the valid header and every consecutive valid frame.
	 * `authoritative_prefix_byte_count` is zero unless at least one valid commit exists; otherwise
	 * it ends exactly after the last valid commit frame. Bytes after that point are never
	 * authority.
	 */
	struct sqlite_wal_scan_receipt
	{
		sqlite_wal_scan_classification classification{sqlite_wal_scan_classification::empty};
		sqlite_wal_scan_stop stop{sqlite_wal_scan_stop::end_of_input};
		std::optional<sqlite_wal_header_receipt> header;
		std::optional<sqlite_wal_frame_receipt> last_valid_frame;
		std::optional<sqlite_wal_frame_receipt> last_valid_commit;
		std::uint64_t inspected_byte_count{};
		std::uint64_t validated_prefix_byte_count{};
		std::uint64_t authoritative_prefix_byte_count{};
		std::uint64_t valid_frame_count{};
		std::uint64_t valid_commit_count{};
		std::uint64_t torn_remainder_byte_count{};
	};

	struct sqlite_wal_frame_layout
	{
		std::uint64_t frame_count{};
		std::uint32_t page_size{};
	};

	/** Checked `32 + frame_count * (24 + page_size)` calculation. */
	[[nodiscard]] bool sqlite_wal_checked_frame_prefix_length(sqlite_wal_frame_layout layout,
															  std::uint64_t& output) noexcept;

	/** Parse and validate exactly one 32-byte WAL header without reading ambient state. */
	[[nodiscard]] result<sqlite_wal_header_receipt>
	parse_sqlite_wal_header(std::span<const std::byte> header_bytes);

	/** Consume one bounded pass. Source errors remain typed `result` failures. */
	[[nodiscard]] result<sqlite_wal_scan_receipt>
	scan_sqlite_wal(sqlite_bounded_byte_source& source);

	/** Open exactly one independent pass over held immutable bytes and scan it. */
	[[nodiscard]] result<sqlite_wal_scan_receipt>
	scan_sqlite_wal(const sqlite_replayable_byte_source& source);
} // namespace cxxlens::sdk
