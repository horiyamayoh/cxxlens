#include "sqlite_wal_receipt_internal.hpp"

#include <array>
#include <limits>
#include <span>
#include <utility>

namespace cxxlens::sdk
{
	namespace
	{
		struct wal_checksum
		{
			std::uint32_t first{};
			std::uint32_t second{};
		};

		enum class wal_header_failure : std::uint8_t
		{
			none,
			invalid_byte_count,
			invalid_magic,
			unsupported_format_version,
			invalid_page_size,
			checksum_mismatch,
		};

		struct wal_header_parse_outcome
		{
			sqlite_wal_header_receipt receipt;
			wal_header_failure failure{wal_header_failure::none};

			[[nodiscard]] bool valid() const noexcept
			{
				return failure == wal_header_failure::none;
			}
		};

		enum class exact_read_state : std::uint8_t
		{
			complete,
			end_of_input,
			counter_overflow,
		};

		struct exact_read_receipt
		{
			exact_read_state state{};
			std::size_t byte_count{};
		};

		[[nodiscard]] error wal_source_error()
		{
			return {"store.sqlite-failure", "sqlite-wal-source", "invalid-read-count"};
		}

		[[nodiscard]] error wal_header_error(const wal_header_failure failure)
		{
			switch (failure)
			{
				case wal_header_failure::invalid_byte_count:
					return {"store.sqlite-failure",
							"sqlite-wal-header",
							"exact-32-byte-header-required"};
				case wal_header_failure::invalid_magic:
					return {"store.sqlite-failure", "sqlite-wal-header", "invalid-magic"};
				case wal_header_failure::unsupported_format_version:
					return {
						"store.sqlite-failure", "sqlite-wal-header", "unsupported-format-version"};
				case wal_header_failure::invalid_page_size:
					return {"store.sqlite-failure", "sqlite-wal-header", "invalid-page-size"};
				case wal_header_failure::checksum_mismatch:
					return {"store.sqlite-failure", "sqlite-wal-header", "checksum-mismatch"};
				case wal_header_failure::none:
					break;
			}
			return {"store.sqlite-failure", "sqlite-wal-header", "invalid-validation-state"};
		}

		[[nodiscard]] sqlite_wal_scan_stop
		header_failure_scan_stop(const wal_header_failure failure) noexcept
		{
			switch (failure)
			{
				case wal_header_failure::invalid_byte_count:
					return sqlite_wal_scan_stop::torn_header;
				case wal_header_failure::invalid_magic:
					return sqlite_wal_scan_stop::invalid_magic;
				case wal_header_failure::unsupported_format_version:
					return sqlite_wal_scan_stop::unsupported_format_version;
				case wal_header_failure::invalid_page_size:
					return sqlite_wal_scan_stop::invalid_page_size;
				case wal_header_failure::checksum_mismatch:
					return sqlite_wal_scan_stop::header_checksum_mismatch;
				case wal_header_failure::none:
					break;
			}
			return sqlite_wal_scan_stop::torn_header;
		}

		[[nodiscard]] std::uint32_t read_big_endian_u32(const std::span<const std::byte> bytes)
		{
			return (std::to_integer<std::uint32_t>(bytes[0]) << 24U) |
				(std::to_integer<std::uint32_t>(bytes[1]) << 16U) |
				(std::to_integer<std::uint32_t>(bytes[2]) << 8U) |
				std::to_integer<std::uint32_t>(bytes[3]);
		}

		[[nodiscard]] std::uint32_t read_checksum_u32(const std::span<const std::byte> bytes,
													  const sqlite_wal_checksum_byte_order order)
		{
			if (order == sqlite_wal_checksum_byte_order::big_endian)
				return read_big_endian_u32(bytes);
			return std::to_integer<std::uint32_t>(bytes[0]) |
				(std::to_integer<std::uint32_t>(bytes[1]) << 8U) |
				(std::to_integer<std::uint32_t>(bytes[2]) << 16U) |
				(std::to_integer<std::uint32_t>(bytes[3]) << 24U);
		}

		void update_checksum(wal_checksum& checksum,
							 const std::span<const std::byte> bytes,
							 const sqlite_wal_checksum_byte_order order) noexcept
		{
			for (std::size_t offset{}; offset < bytes.size(); offset += 8U)
			{
				checksum.first +=
					read_checksum_u32(bytes.subspan(offset, 4U), order) + checksum.second;
				checksum.second +=
					read_checksum_u32(bytes.subspan(offset + 4U, 4U), order) + checksum.first;
			}
		}

		[[nodiscard]] std::uint32_t
		canonical_page_size(const std::uint32_t encoded_page_size) noexcept
		{
			return encoded_page_size == 1U ? sqlite_wal_maximum_page_size : encoded_page_size;
		}

		[[nodiscard]] bool valid_page_size(const std::uint32_t page_size) noexcept
		{
			return page_size >= sqlite_wal_minimum_page_size &&
				page_size <= sqlite_wal_maximum_page_size && (page_size & (page_size - 1U)) == 0U;
		}

		[[nodiscard]] wal_header_parse_outcome
		parse_wal_header_bytes(const std::span<const std::byte> header_bytes) noexcept
		{
			if (header_bytes.size() != sqlite_wal_header_byte_count)
				return {{}, wal_header_failure::invalid_byte_count};

			const auto magic = read_big_endian_u32(header_bytes.subspan(0U, 4U));
			sqlite_wal_checksum_byte_order checksum_order{};
			if (magic == sqlite_wal_little_endian_checksum_magic)
				checksum_order = sqlite_wal_checksum_byte_order::little_endian;
			else if (magic == sqlite_wal_big_endian_checksum_magic)
				checksum_order = sqlite_wal_checksum_byte_order::big_endian;
			else
				return {{}, wal_header_failure::invalid_magic};

			const auto format_version = read_big_endian_u32(header_bytes.subspan(4U, 4U));
			if (format_version != sqlite_wal_format_version)
				return {{}, wal_header_failure::unsupported_format_version};
			const auto page_size =
				canonical_page_size(read_big_endian_u32(header_bytes.subspan(8U, 4U)));
			if (!valid_page_size(page_size))
				return {{}, wal_header_failure::invalid_page_size};

			wal_checksum checksum;
			update_checksum(checksum, header_bytes.first(24U), checksum_order);
			const auto stored_checksum_1 = read_big_endian_u32(header_bytes.subspan(24U, 4U));
			const auto stored_checksum_2 = read_big_endian_u32(header_bytes.subspan(28U, 4U));
			if (checksum.first != stored_checksum_1 || checksum.second != stored_checksum_2)
				return {{}, wal_header_failure::checksum_mismatch};

			return {sqlite_wal_header_receipt{
						magic,
						format_version,
						page_size,
						read_big_endian_u32(header_bytes.subspan(12U, 4U)),
						read_big_endian_u32(header_bytes.subspan(16U, 4U)),
						read_big_endian_u32(header_bytes.subspan(20U, 4U)),
						stored_checksum_1,
						stored_checksum_2,
						checksum_order,
					},
					wal_header_failure::none};
		}

		[[nodiscard]] result<exact_read_receipt> read_exact(sqlite_bounded_byte_source& source,
															const std::span<std::byte> output,
															std::uint64_t& inspected_byte_count)
		{
			std::size_t offset{};
			while (offset < output.size())
			{
				auto count = source.read(output.subspan(offset));
				if (!count)
					return unexpected(std::move(count.error()));

				const auto count_u64 = static_cast<std::uint64_t>(*count);
				if (static_cast<std::size_t>(count_u64) != *count)
					return exact_read_receipt{exact_read_state::counter_overflow, offset};
				std::uint64_t next_inspected{};
				if (!sqlite_checked_add_u64(inspected_byte_count, count_u64, next_inspected))
					return exact_read_receipt{exact_read_state::counter_overflow, offset};
				if (*count > output.size() - offset)
					return unexpected(wal_source_error());
				if (*count == 0U)
					return exact_read_receipt{exact_read_state::end_of_input, offset};
				inspected_byte_count = next_inspected;
				offset += *count;
			}
			return exact_read_receipt{exact_read_state::complete, offset};
		}

		void set_overflow(sqlite_wal_scan_receipt& receipt) noexcept
		{
			receipt.classification = sqlite_wal_scan_classification::accounting_overflow;
			receipt.stop = sqlite_wal_scan_stop::counter_overflow;
			receipt.authoritative_prefix_byte_count = 0U;
			receipt.last_valid_commit.reset();
		}

		void set_frame_terminal(sqlite_wal_scan_receipt& receipt,
								const sqlite_wal_scan_stop stop) noexcept
		{
			receipt.classification = receipt.last_valid_commit
				? sqlite_wal_scan_classification::committed_prefix
				: sqlite_wal_scan_classification::no_valid_commit;
			receipt.stop = stop;
		}
	} // namespace

	bool sqlite_wal_checked_frame_prefix_length(const sqlite_wal_frame_layout layout,
												std::uint64_t& output) noexcept
	{
		std::uint64_t frame_byte_count{};
		if (!sqlite_checked_add_u64(
				sqlite_wal_frame_header_byte_count, layout.page_size, frame_byte_count))
			return false;
		std::uint64_t frames_byte_count{};
		if (!sqlite_checked_multiply_u64(layout.frame_count, frame_byte_count, frames_byte_count))
			return false;
		return sqlite_checked_add_u64(sqlite_wal_header_byte_count, frames_byte_count, output);
	}

	result<sqlite_wal_header_receipt>
	parse_sqlite_wal_header(const std::span<const std::byte> header_bytes)
	{
		const auto parsed = parse_wal_header_bytes(header_bytes);
		if (!parsed.valid())
			return unexpected(wal_header_error(parsed.failure));
		return parsed.receipt;
	}

	result<sqlite_wal_scan_receipt> scan_sqlite_wal(sqlite_bounded_byte_source& source)
	{
		sqlite_wal_scan_receipt receipt;
		std::array<std::byte, static_cast<std::size_t>(sqlite_wal_header_byte_count)>
			header_bytes{};
		auto header_read = read_exact(source, header_bytes, receipt.inspected_byte_count);
		if (!header_read)
			return unexpected(std::move(header_read.error()));
		if (header_read->state == exact_read_state::counter_overflow)
		{
			set_overflow(receipt);
			return receipt;
		}
		if (header_read->state == exact_read_state::end_of_input)
		{
			if (header_read->byte_count == 0U)
				return receipt;
			receipt.classification = sqlite_wal_scan_classification::invalid_header;
			receipt.stop = sqlite_wal_scan_stop::torn_header;
			receipt.torn_remainder_byte_count = header_read->byte_count;
			return receipt;
		}

		const auto parsed_header = parse_wal_header_bytes(header_bytes);
		if (!parsed_header.valid())
		{
			receipt.classification = sqlite_wal_scan_classification::invalid_header;
			receipt.stop = header_failure_scan_stop(parsed_header.failure);
			return receipt;
		}

		receipt.header = parsed_header.receipt;
		receipt.validated_prefix_byte_count = sqlite_wal_header_byte_count;
		receipt.classification = sqlite_wal_scan_classification::no_valid_commit;
		const auto page_size = receipt.header->page_size;
		const auto checksum_order = receipt.header->checksum_byte_order;
		wal_checksum rolling_checksum{receipt.header->checksum_1, receipt.header->checksum_2};

		std::array<std::byte, static_cast<std::size_t>(sqlite_wal_frame_header_byte_count)>
			frame_header_bytes{};
		std::array<std::byte, sqlite_wal_maximum_page_size> page_bytes{};
		for (;;)
		{
			const auto frame_start = receipt.validated_prefix_byte_count;
			auto frame_header_read =
				read_exact(source, frame_header_bytes, receipt.inspected_byte_count);
			if (!frame_header_read)
				return unexpected(std::move(frame_header_read.error()));
			if (frame_header_read->state == exact_read_state::counter_overflow)
			{
				set_overflow(receipt);
				return receipt;
			}
			if (frame_header_read->state == exact_read_state::end_of_input)
			{
				if (frame_header_read->byte_count == 0U)
				{
					set_frame_terminal(receipt, sqlite_wal_scan_stop::end_of_input);
					return receipt;
				}
				receipt.torn_remainder_byte_count = receipt.inspected_byte_count - frame_start;
				set_frame_terminal(receipt, sqlite_wal_scan_stop::torn_frame);
				return receipt;
			}

			const auto frame_header_span = std::span<const std::byte>{frame_header_bytes};
			const auto salt_1 = read_big_endian_u32(frame_header_span.subspan(8U, 4U));
			const auto salt_2 = read_big_endian_u32(frame_header_span.subspan(12U, 4U));
			if (salt_1 != receipt.header->salt_1 || salt_2 != receipt.header->salt_2)
			{
				set_frame_terminal(receipt, sqlite_wal_scan_stop::frame_salt_mismatch);
				return receipt;
			}

			const auto page_number = read_big_endian_u32(frame_header_span.first(4U));
			if (page_number == 0U)
			{
				set_frame_terminal(receipt, sqlite_wal_scan_stop::frame_page_number_zero);
				return receipt;
			}

			auto page_read = read_exact(
				source, std::span{page_bytes}.first(page_size), receipt.inspected_byte_count);
			if (!page_read)
				return unexpected(std::move(page_read.error()));
			if (page_read->state == exact_read_state::counter_overflow)
			{
				set_overflow(receipt);
				return receipt;
			}
			if (page_read->state == exact_read_state::end_of_input)
			{
				receipt.torn_remainder_byte_count = receipt.inspected_byte_count - frame_start;
				set_frame_terminal(receipt, sqlite_wal_scan_stop::torn_frame);
				return receipt;
			}

			auto frame_checksum = rolling_checksum;
			update_checksum(frame_checksum, frame_header_span.first(8U), checksum_order);
			update_checksum(frame_checksum,
							std::span<const std::byte>{page_bytes}.first(page_size),
							checksum_order);
			const auto stored_frame_checksum_1 =
				read_big_endian_u32(frame_header_span.subspan(16U, 4U));
			const auto stored_frame_checksum_2 =
				read_big_endian_u32(frame_header_span.subspan(20U, 4U));
			if (frame_checksum.first != stored_frame_checksum_1 ||
				frame_checksum.second != stored_frame_checksum_2)
			{
				set_frame_terminal(receipt, sqlite_wal_scan_stop::frame_checksum_mismatch);
				return receipt;
			}

			std::uint64_t next_frame_count{};
			if (!sqlite_checked_add_u64(receipt.valid_frame_count, 1U, next_frame_count))
			{
				set_overflow(receipt);
				return receipt;
			}
			std::uint64_t frame_prefix_byte_count{};
			if (!sqlite_wal_checked_frame_prefix_length({next_frame_count, page_size},
														frame_prefix_byte_count))
			{
				set_overflow(receipt);
				return receipt;
			}

			const auto database_page_count = read_big_endian_u32(frame_header_span.subspan(4U, 4U));
			if (database_page_count != 0U)
			{
				std::uint64_t next_commit_count{};
				if (!sqlite_checked_add_u64(receipt.valid_commit_count, 1U, next_commit_count))
				{
					set_overflow(receipt);
					return receipt;
				}
				receipt.valid_commit_count = next_commit_count;
			}

			const sqlite_wal_frame_receipt frame_receipt{next_frame_count,
														 page_number,
														 database_page_count,
														 stored_frame_checksum_1,
														 stored_frame_checksum_2,
														 frame_prefix_byte_count};
			receipt.valid_frame_count = next_frame_count;
			receipt.validated_prefix_byte_count = frame_prefix_byte_count;
			receipt.last_valid_frame = frame_receipt;
			if (database_page_count != 0U)
			{
				receipt.last_valid_commit = frame_receipt;
				receipt.authoritative_prefix_byte_count = frame_prefix_byte_count;
				receipt.classification = sqlite_wal_scan_classification::committed_prefix;
			}
			rolling_checksum = frame_checksum;
		}
	}

	result<sqlite_wal_scan_receipt> scan_sqlite_wal(const sqlite_replayable_byte_source& source)
	{
		auto pass = source.open_pass();
		if (!pass)
			return unexpected(std::move(pass.error()));
		if (*pass == nullptr)
			return unexpected(wal_source_error());
		return scan_sqlite_wal(**pass);
	}
} // namespace cxxlens::sdk
