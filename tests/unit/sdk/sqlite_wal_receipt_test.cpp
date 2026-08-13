#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "sdk/sqlite_wal_receipt_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
			throw std::runtime_error{message};
	}

	[[nodiscard]] std::uint32_t read_big_endian_u32(const std::span<const std::byte> bytes)
	{
		return (std::to_integer<std::uint32_t>(bytes[0]) << 24U) |
			(std::to_integer<std::uint32_t>(bytes[1]) << 16U) |
			(std::to_integer<std::uint32_t>(bytes[2]) << 8U) |
			std::to_integer<std::uint32_t>(bytes[3]);
	}

	void append_big_endian_u32(std::vector<std::byte>& output, const std::uint32_t value)
	{
		output.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
		output.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
		output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
		output.push_back(static_cast<std::byte>(value & 0xffU));
	}

	struct test_checksum
	{
		std::uint32_t first{};
		std::uint32_t second{};
	};

	[[nodiscard]] std::uint32_t checksum_word(const std::span<const std::byte> bytes,
											  const sqlite_wal_checksum_byte_order order)
	{
		if (order == sqlite_wal_checksum_byte_order::big_endian)
			return read_big_endian_u32(bytes);
		return std::to_integer<std::uint32_t>(bytes[0]) |
			(std::to_integer<std::uint32_t>(bytes[1]) << 8U) |
			(std::to_integer<std::uint32_t>(bytes[2]) << 16U) |
			(std::to_integer<std::uint32_t>(bytes[3]) << 24U);
	}

	void update_checksum(test_checksum& checksum,
						 const std::span<const std::byte> bytes,
						 const sqlite_wal_checksum_byte_order order)
	{
		require(bytes.size() % 8U == 0U, "test checksum input is not an 8-byte multiple");
		for (std::size_t offset{}; offset < bytes.size(); offset += 8U)
		{
			checksum.first += checksum_word(bytes.subspan(offset, 4U), order) + checksum.second;
			checksum.second +=
				checksum_word(bytes.subspan(offset + 4U, 4U), order) + checksum.first;
		}
	}

	struct page_pattern
	{
		std::uint32_t page_size{};
		std::uint8_t seed{};
	};

	[[nodiscard]] std::vector<std::byte> page_bytes(const page_pattern pattern)
	{
		std::vector<std::byte> output(pattern.page_size);
		for (std::size_t index{}; index < output.size(); ++index)
			output[index] = static_cast<std::byte>((index * 37U + pattern.seed) & 0xffU);
		return output;
	}

	struct synthetic_wal_options
	{
		sqlite_wal_checksum_byte_order order{};
		std::uint32_t page_size{512U};
		std::uint32_t magic{};
		std::uint32_t format_version{sqlite_wal_format_version};
		bool corrupt_header_checksum{};
	};

	struct synthetic_frame
	{
		std::uint32_t page_number{};
		std::uint32_t database_page_count{};
		std::uint8_t seed{};
		bool corrupt_checksum{};
		std::optional<std::uint32_t> salt_1 = std::nullopt;
		std::optional<std::uint32_t> salt_2 = std::nullopt;
	};

	class synthetic_wal
	{
	  public:
		explicit synthetic_wal(const synthetic_wal_options options)
			: order_{options.order}, page_size_{options.page_size}, salt_1_{0x11223344U},
			  salt_2_{0x55667788U}
		{
			const auto selected_magic = options.magic != 0U ? options.magic
				: options.order == sqlite_wal_checksum_byte_order::little_endian
				? sqlite_wal_little_endian_checksum_magic
				: sqlite_wal_big_endian_checksum_magic;
			append_big_endian_u32(bytes_, selected_magic);
			append_big_endian_u32(bytes_, options.format_version);
			append_big_endian_u32(bytes_, page_size_);
			append_big_endian_u32(bytes_, 0x01020304U);
			append_big_endian_u32(bytes_, salt_1_);
			append_big_endian_u32(bytes_, salt_2_);
			update_checksum(checksum_, std::span<const std::byte>{bytes_}, order_);
			append_big_endian_u32(bytes_,
								  checksum_.first ^
									  static_cast<std::uint32_t>(options.corrupt_header_checksum));
			append_big_endian_u32(bytes_, checksum_.second);
		}

		void append_frame(const synthetic_frame frame)
		{
			std::vector<std::byte> header;
			header.reserve(static_cast<std::size_t>(sqlite_wal_frame_header_byte_count));
			append_big_endian_u32(header, frame.page_number);
			append_big_endian_u32(header, frame.database_page_count);
			append_big_endian_u32(header, frame.salt_1.value_or(salt_1_));
			append_big_endian_u32(header, frame.salt_2.value_or(salt_2_));
			const auto page = page_bytes({page_size_, frame.seed});
			auto next_checksum = checksum_;
			update_checksum(next_checksum, std::span<const std::byte>{header}.first(8U), order_);
			update_checksum(next_checksum, page, order_);
			append_big_endian_u32(
				header, next_checksum.first ^ static_cast<std::uint32_t>(frame.corrupt_checksum));
			append_big_endian_u32(header, next_checksum.second);
			bytes_.insert(bytes_.end(), header.begin(), header.end());
			bytes_.insert(bytes_.end(), page.begin(), page.end());
			checksum_ = next_checksum;
		}

		[[nodiscard]] const std::vector<std::byte>& bytes() const noexcept
		{
			return bytes_;
		}

		[[nodiscard]] std::vector<std::byte>& bytes() noexcept
		{
			return bytes_;
		}

	  private:
		sqlite_wal_checksum_byte_order order_{};
		std::uint32_t page_size_{};
		std::uint32_t salt_1_{};
		std::uint32_t salt_2_{};
		test_checksum checksum_{};
		std::vector<std::byte> bytes_;
	};

	struct source_metrics
	{
		std::size_t open_count{};
		std::size_t read_count{};
		std::size_t maximum_requested_byte_count{};
		std::uint64_t returned_byte_count{};
	};

	class fragmented_cursor final : public sqlite_bounded_byte_source
	{
	  public:
		fragmented_cursor(std::shared_ptr<const std::vector<std::byte>> bytes,
						  std::shared_ptr<source_metrics> metrics,
						  const std::size_t fragment_limit)
			: bytes_{std::move(bytes)}, metrics_{std::move(metrics)},
			  fragment_limit_{fragment_limit}
		{
		}

		result<std::size_t> read(const std::span<std::byte> output) override
		{
			++metrics_->read_count;
			metrics_->maximum_requested_byte_count =
				std::max(metrics_->maximum_requested_byte_count, output.size());
			if (offset_ == bytes_->size())
				return 0U;
			const auto count = std::min({output.size(), fragment_limit_, bytes_->size() - offset_});
			std::copy_n(
				bytes_->begin() + static_cast<std::ptrdiff_t>(offset_), count, output.begin());
			offset_ += count;
			metrics_->returned_byte_count += static_cast<std::uint64_t>(count);
			return count;
		}

	  private:
		std::shared_ptr<const std::vector<std::byte>> bytes_;
		std::shared_ptr<source_metrics> metrics_;
		std::size_t fragment_limit_{};
		std::size_t offset_{};
	};

	class fragmented_replay_source final : public sqlite_replayable_byte_source
	{
	  public:
		fragmented_replay_source(std::shared_ptr<const std::vector<std::byte>> bytes,
								 std::shared_ptr<source_metrics> metrics,
								 const std::size_t fragment_limit)
			: bytes_{std::move(bytes)}, metrics_{std::move(metrics)},
			  fragment_limit_{fragment_limit}
		{
		}

		[[nodiscard]] result<std::unique_ptr<sqlite_bounded_byte_source>> open_pass() const override
		{
			++metrics_->open_count;
			std::unique_ptr<sqlite_bounded_byte_source> output =
				std::make_unique<fragmented_cursor>(bytes_, metrics_, fragment_limit_);
			return output;
		}

	  private:
		std::shared_ptr<const std::vector<std::byte>> bytes_;
		std::shared_ptr<source_metrics> metrics_;
		std::size_t fragment_limit_{};
	};

	struct scan_observation
	{
		sqlite_wal_scan_receipt receipt;
		std::shared_ptr<source_metrics> metrics;
	};

	[[nodiscard]] scan_observation
	scan(const std::vector<std::byte>& bytes,
		 const std::size_t fragment_limit = std::numeric_limits<std::size_t>::max())
	{
		auto owned = std::make_shared<const std::vector<std::byte>>(bytes);
		auto metrics = std::make_shared<source_metrics>();
		fragmented_replay_source source{owned, metrics, fragment_limit};
		auto receipt = scan_sqlite_wal(source);
		require(static_cast<bool>(receipt), "WAL scan returned a source failure");
		return {*receipt, std::move(metrics)};
	}

	void verify_known_checksum_orders()
	{
		struct known_answer
		{
			sqlite_wal_checksum_byte_order order;
			std::uint32_t header_1;
			std::uint32_t header_2;
			std::uint32_t frame_1;
			std::uint32_t frame_2;
		};
		for (const auto known :
			 std::array{known_answer{sqlite_wal_checksum_byte_order::little_endian,
									 0x1d012725U,
									 0x61556720U,
									 0x1926532dU,
									 0xedc28771U},
						known_answer{sqlite_wal_checksum_byte_order::big_endian,
									 0x2829011fU,
									 0x256a5564U,
									 0x72c7d0e7U,
									 0x0204cc68U}})
		{
			synthetic_wal wal{{.order = known.order}};
			wal.append_frame({.page_number = 7U, .database_page_count = 9U, .seed = 11U});
			const auto expected_magic = known.order == sqlite_wal_checksum_byte_order::little_endian
				? sqlite_wal_little_endian_checksum_magic
				: sqlite_wal_big_endian_checksum_magic;
			const auto bytes = std::span<const std::byte>{wal.bytes()};
			require(read_big_endian_u32(bytes.subspan(24U, 4U)) == known.header_1 &&
						read_big_endian_u32(bytes.subspan(28U, 4U)) == known.header_2 &&
						read_big_endian_u32(bytes.subspan(48U, 4U)) == known.frame_1 &&
						read_big_endian_u32(bytes.subspan(52U, 4U)) == known.frame_2,
					"synthetic WAL builder drifted from the hard-coded checksum vector");
			auto parsed_header = parse_sqlite_wal_header(bytes.first(sqlite_wal_header_byte_count));
			require(parsed_header && parsed_header->magic == expected_magic &&
						parsed_header->format_version == sqlite_wal_format_version &&
						parsed_header->page_size == 512U &&
						parsed_header->checkpoint_sequence == 0x01020304U &&
						parsed_header->salt_1 == 0x11223344U &&
						parsed_header->salt_2 == 0x55667788U &&
						parsed_header->checksum_1 == known.header_1 &&
						parsed_header->checksum_2 == known.header_2 &&
						parsed_header->checksum_byte_order == known.order,
					"pure WAL header parser failed a hard-coded checksum vector");

			auto observed = scan(wal.bytes(), 3U);
			const auto& receipt = observed.receipt;
			require(receipt.classification == sqlite_wal_scan_classification::committed_prefix &&
						receipt.stop == sqlite_wal_scan_stop::end_of_input && receipt.header &&
						receipt.header->magic == expected_magic &&
						receipt.header->format_version == sqlite_wal_format_version &&
						receipt.header->page_size == 512U &&
						receipt.header->checksum_byte_order == known.order &&
						receipt.header->checkpoint_sequence == 0x01020304U &&
						receipt.header->salt_1 == 0x11223344U &&
						receipt.header->salt_2 == 0x55667788U &&
						receipt.header->checksum_1 == known.header_1 &&
						receipt.header->checksum_2 == known.header_2 && receipt.last_valid_commit &&
						receipt.last_valid_commit->page_number == 7U &&
						receipt.last_valid_commit->database_page_count == 9U &&
						receipt.last_valid_commit->checksum_1 == known.frame_1 &&
						receipt.last_valid_commit->checksum_2 == known.frame_2 &&
						receipt.authoritative_prefix_byte_count == wal.bytes().size(),
					"known WAL checksum vector did not produce an exact committed receipt");
			require(observed.metrics->open_count == 1U &&
						observed.metrics->returned_byte_count == wal.bytes().size(),
					"fragmented replay source was not consumed in exactly one pass");
		}
	}

	void require_header_failure(const result<sqlite_wal_header_receipt>& parsed,
								const std::string_view detail)
	{
		require(!parsed && parsed.error().code == "store.sqlite-failure" &&
					parsed.error().field == "sqlite-wal-header" && parsed.error().detail == detail,
				"pure WAL header parser returned the wrong typed failure tuple");
	}

	void verify_pure_header_parser_failures_and_page_encoding()
	{
		const synthetic_wal valid{{.order = sqlite_wal_checksum_byte_order::little_endian}};
		const auto valid_bytes = std::span<const std::byte>{valid.bytes()};
		require_header_failure(parse_sqlite_wal_header(valid_bytes.first(31U)),
							   "exact-32-byte-header-required");

		const synthetic_wal bad_magic{
			{.order = sqlite_wal_checksum_byte_order::little_endian, .magic = 0x377f0684U}};
		require_header_failure(parse_sqlite_wal_header(bad_magic.bytes()), "invalid-magic");

		const synthetic_wal bad_page{
			{.order = sqlite_wal_checksum_byte_order::little_endian, .page_size = 513U}};
		require_header_failure(parse_sqlite_wal_header(bad_page.bytes()), "invalid-page-size");

		const synthetic_wal bad_checksum{
			{.order = sqlite_wal_checksum_byte_order::big_endian, .corrupt_header_checksum = true}};
		require_header_failure(parse_sqlite_wal_header(bad_checksum.bytes()), "checksum-mismatch");

		const synthetic_wal encoded_maximum{
			{.order = sqlite_wal_checksum_byte_order::little_endian, .page_size = 1U}};
		auto parsed = parse_sqlite_wal_header(encoded_maximum.bytes());
		auto scanned = scan(encoded_maximum.bytes());
		require(
			parsed && parsed->page_size == sqlite_wal_maximum_page_size && scanned.receipt.header &&
				scanned.receipt.header->page_size == sqlite_wal_maximum_page_size &&
				scanned.receipt.classification == sqlite_wal_scan_classification::no_valid_commit,
			"encoded WAL page size 1 was not canonicalized to 65536");
	}

	void verify_commit_census()
	{
		auto zero_byte = scan({});
		require(zero_byte.receipt.classification == sqlite_wal_scan_classification::empty &&
					zero_byte.receipt.stop == sqlite_wal_scan_stop::end_of_input &&
					zero_byte.receipt.inspected_byte_count == 0U &&
					zero_byte.receipt.authoritative_prefix_byte_count == 0U,
				"zero-byte WAL acquired header or frame authority");

		const synthetic_wal empty_frames{{.order = sqlite_wal_checksum_byte_order::little_endian}};
		auto header_only = scan(empty_frames.bytes());
		require(
			header_only.receipt.classification == sqlite_wal_scan_classification::no_valid_commit &&
				header_only.receipt.validated_prefix_byte_count == sqlite_wal_header_byte_count &&
				header_only.receipt.authoritative_prefix_byte_count == 0U,
			"header-only WAL acquired commit authority");

		synthetic_wal no_commit{{.order = sqlite_wal_checksum_byte_order::little_endian}};
		no_commit.append_frame({.page_number = 1U, .database_page_count = 0U, .seed = 1U});
		auto uncommitted = scan(no_commit.bytes());
		require(
			uncommitted.receipt.classification == sqlite_wal_scan_classification::no_valid_commit &&
				uncommitted.receipt.valid_frame_count == 1U &&
				uncommitted.receipt.valid_commit_count == 0U &&
				uncommitted.receipt.last_valid_frame && !uncommitted.receipt.last_valid_commit &&
				uncommitted.receipt.authoritative_prefix_byte_count == 0U,
			"valid uncommitted frame acquired authority");

		synthetic_wal multiple{{.order = sqlite_wal_checksum_byte_order::big_endian}};
		multiple.append_frame({.page_number = 1U, .database_page_count = 0U, .seed = 1U});
		multiple.append_frame({.page_number = 2U, .database_page_count = 20U, .seed = 2U});
		multiple.append_frame({.page_number = 3U, .database_page_count = 0U, .seed = 3U});
		multiple.append_frame({.page_number = 4U, .database_page_count = 17U, .seed = 4U});
		const auto last_commit_prefix = multiple.bytes().size();
		multiple.append_frame({.page_number = 5U, .database_page_count = 0U, .seed = 5U});
		auto observed = scan(multiple.bytes(), 7U);
		require(
			observed.receipt.classification == sqlite_wal_scan_classification::committed_prefix &&
				observed.receipt.valid_frame_count == 5U &&
				observed.receipt.valid_commit_count == 2U && observed.receipt.last_valid_frame &&
				observed.receipt.last_valid_frame->frame_number == 5U &&
				observed.receipt.last_valid_commit &&
				observed.receipt.last_valid_commit->frame_number == 4U &&
				observed.receipt.last_valid_commit->database_page_count == 17U &&
				observed.receipt.authoritative_prefix_byte_count == last_commit_prefix &&
				observed.receipt.validated_prefix_byte_count == multiple.bytes().size(),
			"multiple commit census did not stop authority at the last commit");
	}

	void verify_first_invalid_stop()
	{
		synthetic_wal salt{{.order = sqlite_wal_checksum_byte_order::little_endian}};
		salt.append_frame({.page_number = 1U, .database_page_count = 5U, .seed = 1U});
		const auto valid_prefix = salt.bytes().size();
		salt.append_frame(
			{.page_number = 2U, .database_page_count = 0U, .seed = 2U, .salt_1 = 0x11223345U});
		salt.append_frame({.page_number = 3U, .database_page_count = 7U, .seed = 3U});
		auto salt_result = scan(salt.bytes());
		require(salt_result.receipt.classification ==
						sqlite_wal_scan_classification::committed_prefix &&
					salt_result.receipt.stop == sqlite_wal_scan_stop::frame_salt_mismatch &&
					salt_result.receipt.valid_frame_count == 1U &&
					salt_result.receipt.authoritative_prefix_byte_count == valid_prefix &&
					salt_result.receipt.inspected_byte_count ==
						valid_prefix + sqlite_wal_frame_header_byte_count &&
					salt_result.metrics->returned_byte_count ==
						salt_result.receipt.inspected_byte_count,
				"salt mismatch did not stop before the invalid frame payload");

		synthetic_wal first_salt{{.order = sqlite_wal_checksum_byte_order::big_endian}};
		first_salt.append_frame(
			{.page_number = 1U, .database_page_count = 3U, .seed = 1U, .salt_2 = 0x55667789U});
		auto first_salt_result = scan(first_salt.bytes());
		require(first_salt_result.receipt.classification ==
						sqlite_wal_scan_classification::no_valid_commit &&
					first_salt_result.receipt.stop == sqlite_wal_scan_stop::frame_salt_mismatch &&
					first_salt_result.receipt.authoritative_prefix_byte_count == 0U,
				"invalid first-frame salt acquired commit authority");

		synthetic_wal checksum{{.order = sqlite_wal_checksum_byte_order::little_endian}};
		checksum.append_frame({.page_number = 1U, .database_page_count = 5U, .seed = 1U});
		const auto checksum_valid_prefix = checksum.bytes().size();
		checksum.append_frame(
			{.page_number = 2U, .database_page_count = 0U, .seed = 2U, .corrupt_checksum = true});
		const auto invalid_frame_end = checksum.bytes().size();
		checksum.append_frame({.page_number = 3U, .database_page_count = 7U, .seed = 3U});
		auto checksum_result = scan(checksum.bytes(), 19U);
		require(checksum_result.receipt.classification ==
						sqlite_wal_scan_classification::committed_prefix &&
					checksum_result.receipt.stop == sqlite_wal_scan_stop::frame_checksum_mismatch &&
					checksum_result.receipt.valid_frame_count == 1U &&
					checksum_result.receipt.validated_prefix_byte_count == checksum_valid_prefix &&
					checksum_result.receipt.authoritative_prefix_byte_count ==
						checksum_valid_prefix &&
					checksum_result.receipt.inspected_byte_count == invalid_frame_end &&
					checksum_result.metrics->returned_byte_count == invalid_frame_end,
				"checksum mismatch resumed scanning after the first invalid frame");
	}

	void verify_torn_suffixes()
	{
		synthetic_wal torn{{.order = sqlite_wal_checksum_byte_order::little_endian}};
		torn.append_frame({.page_number = 1U, .database_page_count = 5U, .seed = 1U});
		const auto valid_prefix = torn.bytes().size();
		torn.append_frame({.page_number = 2U, .database_page_count = 0U, .seed = 2U});
		torn.bytes().resize(valid_prefix + sqlite_wal_frame_header_byte_count + 17U);
		auto torn_result = scan(torn.bytes(), 5U);
		require(torn_result.receipt.classification ==
						sqlite_wal_scan_classification::committed_prefix &&
					torn_result.receipt.stop == sqlite_wal_scan_stop::torn_frame &&
					torn_result.receipt.valid_frame_count == 1U &&
					torn_result.receipt.authoritative_prefix_byte_count == valid_prefix &&
					torn_result.receipt.torn_remainder_byte_count ==
						sqlite_wal_frame_header_byte_count + 17U &&
					torn_result.receipt.inspected_byte_count == torn.bytes().size(),
				"torn terminal frame was not kept outside the authoritative prefix");

		std::vector<std::byte> torn_header(torn.bytes().begin(), torn.bytes().begin() + 17);
		auto header_result = scan(torn_header, 1U);
		require(header_result.receipt.classification ==
						sqlite_wal_scan_classification::invalid_header &&
					header_result.receipt.stop == sqlite_wal_scan_stop::torn_header &&
					header_result.receipt.torn_remainder_byte_count == 17U &&
					header_result.receipt.authoritative_prefix_byte_count == 0U,
				"torn WAL header was not rejected without authority");
	}

	void verify_header_and_frame_rejections()
	{
		for (const auto page_size : std::array<std::uint32_t, 4U>{0U, 511U, 513U, 131'072U})
		{
			synthetic_wal wal{
				{.order = sqlite_wal_checksum_byte_order::little_endian, .page_size = page_size}};
			auto result = scan(wal.bytes());
			require(result.receipt.classification ==
							sqlite_wal_scan_classification::invalid_header &&
						result.receipt.stop == sqlite_wal_scan_stop::invalid_page_size,
					"invalid WAL page size passed the header gate");
		}
		for (const auto page_size : std::array<std::uint32_t, 2U>{sqlite_wal_minimum_page_size,
																  sqlite_wal_maximum_page_size})
		{
			synthetic_wal wal{
				{.order = sqlite_wal_checksum_byte_order::big_endian, .page_size = page_size}};
			auto result = scan(wal.bytes());
			require(result.receipt.classification ==
							sqlite_wal_scan_classification::no_valid_commit &&
						result.receipt.header && result.receipt.header->page_size == page_size,
					"valid boundary WAL page size was rejected");
		}

		synthetic_wal bad_magic{
			{.order = sqlite_wal_checksum_byte_order::little_endian, .magic = 0x377f0684U}};
		require(scan(bad_magic.bytes()).receipt.stop == sqlite_wal_scan_stop::invalid_magic,
				"unknown WAL magic was accepted");
		synthetic_wal bad_version{{.order = sqlite_wal_checksum_byte_order::little_endian,
								   .format_version = sqlite_wal_format_version + 1U}};
		require(scan(bad_version.bytes()).receipt.stop ==
					sqlite_wal_scan_stop::unsupported_format_version,
				"unknown WAL format version was accepted");
		synthetic_wal bad_header_checksum{{.order = sqlite_wal_checksum_byte_order::little_endian,
										   .corrupt_header_checksum = true}};
		require(scan(bad_header_checksum.bytes()).receipt.stop ==
					sqlite_wal_scan_stop::header_checksum_mismatch,
				"invalid WAL header checksum was accepted");

		synthetic_wal zero_page{{.order = sqlite_wal_checksum_byte_order::little_endian}};
		zero_page.append_frame({.page_number = 0U, .database_page_count = 7U, .seed = 1U});
		auto zero_page_result = scan(zero_page.bytes());
		require(zero_page_result.receipt.classification ==
						sqlite_wal_scan_classification::no_valid_commit &&
					zero_page_result.receipt.stop == sqlite_wal_scan_stop::frame_page_number_zero &&
					zero_page_result.receipt.inspected_byte_count ==
						sqlite_wal_header_byte_count + sqlite_wal_frame_header_byte_count,
				"zero page number was treated as a valid commit frame");
	}

	class overflow_cursor final : public sqlite_bounded_byte_source
	{
	  public:
		explicit overflow_cursor(std::span<const std::byte> header) : header_{header} {}

		result<std::size_t> read(const std::span<std::byte> output) override
		{
			if (!header_returned_)
			{
				require(output.size() == header_.size(), "overflow cursor header request drifted");
				std::copy(header_.begin(), header_.end(), output.begin());
				header_returned_ = true;
				return header_.size();
			}
			return std::numeric_limits<std::size_t>::max();
		}

	  private:
		std::span<const std::byte> header_;
		bool header_returned_{};
	};

	void verify_checked_accounting()
	{
		constexpr auto page_size = sqlite_wal_maximum_page_size;
		constexpr auto frame_size = sqlite_wal_frame_header_byte_count + page_size;
		constexpr auto maximum_frames =
			(std::numeric_limits<std::uint64_t>::max() - sqlite_wal_header_byte_count) / frame_size;
		std::uint64_t prefix{};
		require(
			sqlite_wal_checked_frame_prefix_length({maximum_frames, page_size}, prefix) &&
				prefix <= std::numeric_limits<std::uint64_t>::max() &&
				!sqlite_wal_checked_frame_prefix_length({maximum_frames + 1U, page_size}, prefix),
			"WAL prefix u64 accounting wrapped at its exact boundary");

		if constexpr (std::numeric_limits<std::size_t>::max() ==
					  std::numeric_limits<std::uint64_t>::max())
		{
			synthetic_wal wal{{.order = sqlite_wal_checksum_byte_order::little_endian}};
			overflow_cursor source{wal.bytes()};
			auto receipt = scan_sqlite_wal(source);
			require(receipt &&
						receipt->classification ==
							sqlite_wal_scan_classification::accounting_overflow &&
						receipt->stop == sqlite_wal_scan_stop::counter_overflow &&
						receipt->authoritative_prefix_byte_count == 0U,
					"source byte-count overflow was not distinguished and failed closed");
		}
	}

	void verify_fragmentation_and_residency_bound()
	{
		synthetic_wal fragmented{{.order = sqlite_wal_checksum_byte_order::big_endian}};
		fragmented.append_frame({.page_number = 1U, .database_page_count = 0U, .seed = 1U});
		fragmented.append_frame({.page_number = 2U, .database_page_count = 2U, .seed = 2U});
		auto fragmented_result = scan(fragmented.bytes(), 1U);
		require(fragmented_result.receipt.classification ==
						sqlite_wal_scan_classification::committed_prefix &&
					fragmented_result.metrics->read_count > fragmented.bytes().size(),
				"one-byte fragmented reads did not produce the same WAL receipt");

		synthetic_wal large{{.order = sqlite_wal_checksum_byte_order::little_endian,
							 .page_size = sqlite_wal_maximum_page_size}};
		for (std::uint32_t frame = 1U; frame <= 20U; ++frame)
			large.append_frame({.page_number = frame,
								.database_page_count = frame == 20U ? 20U : 0U,
								.seed = static_cast<std::uint8_t>(frame)});
		auto large_result = scan(large.bytes());
		require(large.bytes().size() > sqlite_wal_parser_resident_byte_bound * 16U &&
					large_result.receipt.classification ==
						sqlite_wal_scan_classification::committed_prefix &&
					large_result.receipt.valid_frame_count == 20U &&
					large_result.metrics->maximum_requested_byte_count <=
						sqlite_wal_maximum_page_size &&
					sqlite_wal_parser_resident_byte_bound == 65'592U,
				"WAL scanner requested or retained a whole-WAL-sized byte window");
	}
} // namespace

int main()
{
	try
	{
		verify_known_checksum_orders();
		verify_pure_header_parser_failures_and_page_encoding();
		verify_commit_census();
		verify_first_invalid_stop();
		verify_torn_suffixes();
		verify_header_and_frame_rejections();
		verify_checked_accounting();
		verify_fragmentation_and_residency_bound();
		return 0;
	}
	catch (const std::exception& failure)
	{
		std::cerr << failure.what() << '\n';
		return 1;
	}
}
