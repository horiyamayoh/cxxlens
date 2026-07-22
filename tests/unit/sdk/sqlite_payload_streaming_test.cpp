#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "sdk/sqlite_payload_streaming_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
			throw std::runtime_error{message};
	}

	[[nodiscard]] std::vector<std::byte> bytes(const std::size_t count)
	{
		std::vector<std::byte> output(count);
		for (std::size_t index{}; index < count; ++index)
			output[index] = static_cast<std::byte>((index * 131U + index / 17U + 29U) & 0xffU);
		return output;
	}

	struct captured_chunk
	{
		std::uint64_t ordinal{};
		std::uint64_t offset{};
		std::uint64_t count{};
		std::string checksum;
	};

	class capturing_port final : public sqlite_payload_chunk_port
	{
	  public:
		result<void> emit(const sqlite_payload_chunk_frame& frame) override
		{
			if (frame.payload.size() != frame.byte_count ||
				content_digest(frame.payload) != frame.checksum)
				return unexpected(error{"test.chunk-invalid", "frame", {}});
			frames.push_back(
				{frame.ordinal, frame.byte_offset, frame.byte_count, std::string{frame.checksum}});
			return {};
		}

		std::vector<captured_chunk> frames;
	};

	struct owned_chunk
	{
		std::uint64_t ordinal{};
		std::uint64_t offset{};
		std::uint64_t count{};
		std::string checksum;
		std::vector<std::byte> payload;
	};

	[[nodiscard]] std::shared_ptr<std::vector<owned_chunk>>
	make_chunks(const std::span<const std::byte> input)
	{
		auto output = std::make_shared<std::vector<owned_chunk>>();
		std::uint64_t ordinal{};
		for (std::size_t offset{}; offset < input.size();)
		{
			const auto count =
				std::min<std::size_t>(static_cast<std::size_t>(sqlite_payload_chunk_maximum_bytes),
									  input.size() - offset);
			std::vector<std::byte> payload{input.begin() + static_cast<std::ptrdiff_t>(offset),
										   input.begin() +
											   static_cast<std::ptrdiff_t>(offset + count)};
			output->push_back({ordinal,
							   static_cast<std::uint64_t>(offset),
							   static_cast<std::uint64_t>(count),
							   content_digest(payload),
							   std::move(payload)});
			offset += count;
			++ordinal;
		}
		return output;
	}

	class memory_chunk_cursor final : public sqlite_payload_chunk_record_source
	{
	  public:
		explicit memory_chunk_cursor(std::shared_ptr<const std::vector<owned_chunk>> chunks)
			: chunks_{std::move(chunks)}
		{
		}

		result<std::optional<sqlite_payload_chunk_record>> next() override
		{
			if (index_ == chunks_->size())
				return std::optional<sqlite_payload_chunk_record>{};
			const auto& chunk = chunks_->at(index_++);
			return std::optional<sqlite_payload_chunk_record>{sqlite_payload_chunk_record{
				chunk.ordinal, chunk.offset, chunk.count, chunk.checksum, chunk.payload}};
		}

	  private:
		std::shared_ptr<const std::vector<owned_chunk>> chunks_;
		std::size_t index_{};
	};

	class memory_replay_source final : public sqlite_replayable_payload_chunk_source
	{
	  public:
		explicit memory_replay_source(std::shared_ptr<const std::vector<owned_chunk>> chunks)
			: chunks_{std::move(chunks)}
		{
		}

		result<std::unique_ptr<sqlite_payload_chunk_record_source>> open_chunk_pass() const override
		{
			std::unique_ptr<sqlite_payload_chunk_record_source> output =
				std::make_unique<memory_chunk_cursor>(chunks_);
			return output;
		}

	  private:
		std::shared_ptr<const std::vector<owned_chunk>> chunks_;
	};

	void verify_incremental_sha256()
	{
		{
			sqlite_incremental_sha256 digest;
			auto actual = digest.finish();
			require(
				actual &&
					*actual ==
						"sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
				"incremental SHA-256 failed the empty known-answer test");
		}
		{
			constexpr std::array<std::byte, 3U> abc{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
			sqlite_incremental_sha256 digest;
			require(static_cast<bool>(digest.update(abc)),
					"incremental SHA-256 rejected the abc known-answer input");
			auto actual = digest.finish();
			require(
				actual &&
					*actual ==
						"sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
				"incremental SHA-256 failed the abc known-answer test");
		}

		for (const auto count :
			 std::array<std::size_t, 9U>{0U, 1U, 55U, 56U, 63U, 64U, 65U, 4097U, 65'537U})
		{
			const auto input = bytes(count);
			sqlite_incremental_sha256 digest;
			std::size_t offset{};
			std::size_t turn{};
			constexpr std::array<std::size_t, 7U> fragments{1U, 3U, 61U, 64U, 257U, 4096U, 8191U};
			while (offset < input.size())
			{
				const auto length =
					std::min(fragments[turn++ % fragments.size()], input.size() - offset);
				auto updated = digest.update(std::span{input}.subspan(offset, length));
				require(static_cast<bool>(updated), "incremental SHA-256 rejected bounded input");
				offset += length;
			}
			auto actual = digest.finish();
			require(actual && *actual == content_digest(input),
					"incremental SHA-256 differs from sdk::content_digest");
			require(!digest.finish(), "incremental SHA-256 accepted a second finish");
			require(!digest.update(input), "incremental SHA-256 accepted update after finish");
			digest.reset();
			require(digest.byte_count() == 0U && !digest.finished(), "SHA-256 reset lost state");
		}

		std::uint64_t total{};
		require(sqlite_checked_sha256_byte_count(sqlite_sha256_maximum_byte_count, 0U, total) &&
					total == sqlite_sha256_maximum_byte_count,
				"SHA-256 exact checked boundary was rejected");
		require(!sqlite_checked_sha256_byte_count(sqlite_sha256_maximum_byte_count, 1U, total),
				"SHA-256 byte-to-bit overflow was accepted");

		sqlite_u128_census aggregate{0U, std::numeric_limits<std::uint64_t>::max() - 2U};
		require(aggregate.add(3U) && aggregate.high == 1U && aggregate.low == 0U,
				"u128 carry accounting failed");
		aggregate = {std::numeric_limits<std::uint64_t>::max(),
					 std::numeric_limits<std::uint64_t>::max()};
		require(!aggregate.add(1U) &&
					aggregate ==
						sqlite_u128_census{std::numeric_limits<std::uint64_t>::max(),
										   std::numeric_limits<std::uint64_t>::max()},
				"u128 overflow wrapped or changed the prior census");
	}

	void append_fragmented(sqlite_payload_chunk_framer& framer,
						   const std::span<const std::byte> input)
	{
		std::size_t offset{};
		std::size_t turn{};
		constexpr std::array<std::size_t, 6U> fragments{
			13U, 65'537U, 1U, 1'048'579U, 4'194'301U, 257U};
		while (offset < input.size())
		{
			const auto count =
				std::min(fragments[turn++ % fragments.size()], input.size() - offset);
			auto appended = framer.append(input.subspan(offset, count));
			require(static_cast<bool>(appended), "chunk framer rejected bounded fragment");
			require(framer.resident_payload_byte_count() <= sqlite_payload_chunk_maximum_bytes,
					"chunk framer retained more than one canonical chunk");
			offset += count;
		}
	}

	void verify_chunk_framer()
	{
		const auto input =
			bytes(static_cast<std::size_t>(sqlite_payload_chunk_maximum_bytes) + 257U);
		capturing_port port;
		sqlite_payload_chunk_framer framer{&port};
		append_fragmented(framer, input);
		auto receipt = framer.finish();
		require(receipt && receipt->byte_count == input.size() && receipt->chunk_count == 2U &&
					receipt->aggregate_byte_count == sqlite_u128_census{0U, input.size()} &&
					receipt->full_checksum == content_digest(input),
				"chunk framer receipt drifted from exact payload bytes");
		require(port.frames.size() == 2U && port.frames[0].ordinal == 0U &&
					port.frames[0].offset == 0U &&
					port.frames[0].count == sqlite_payload_chunk_maximum_bytes &&
					port.frames[1].ordinal == 1U &&
					port.frames[1].offset == sqlite_payload_chunk_maximum_bytes &&
					port.frames[1].count == 257U,
				"chunk framer emitted noncanonical ordinal/offset/boundary fields");
		require(!framer.finish(), "chunk framer accepted a second finish");

		sqlite_payload_chunk_framer measure;
		append_fragmented(measure, input);
		auto measured = measure.finish();
		require(measured && measure.measure_only() && measure.resident_payload_byte_count() == 0U &&
					measured->byte_count == receipt->byte_count &&
					measured->chunk_count == receipt->chunk_count &&
					measured->full_checksum == receipt->full_checksum,
				"measure pass retained bytes or differed from emit pass");

		const auto exact = bytes(static_cast<std::size_t>(sqlite_payload_chunk_maximum_bytes));
		capturing_port exact_port;
		sqlite_payload_chunk_framer exact_framer{&exact_port};
		require(exact_framer.append(exact) && exact_framer.finish() &&
					exact_port.frames.size() == 1U &&
					exact_port.frames.front().count == sqlite_payload_chunk_maximum_bytes,
				"exact 8 MiB payload produced an empty trailing chunk");

		sqlite_payload_chunk_framer empty;
		auto empty_receipt = empty.finish();
		require(empty_receipt && empty_receipt->byte_count == 0U &&
					empty_receipt->chunk_count == 0U &&
					empty_receipt->full_checksum == content_digest(std::span<const std::byte>{}),
				"empty measure pass did not produce the canonical empty checksum");
	}

	[[nodiscard]] sqlite_payload_stream_expectation
	expectation_for(const std::span<const std::byte> input)
	{
		return {static_cast<std::uint64_t>(input.size()),
				input.empty() ? 0U
							  : 1U +
						static_cast<std::uint64_t>((input.size() - 1U) /
												   sqlite_payload_chunk_maximum_bytes),
				content_digest(input),
				true};
	}

	void verify_validating_source()
	{
		const auto input =
			bytes(static_cast<std::size_t>(sqlite_payload_chunk_maximum_bytes) + 257U);
		const auto expected = expectation_for(input);
		auto chunks = make_chunks(input);
		auto source = sqlite_validating_payload_source::open(
			std::make_unique<memory_chunk_cursor>(chunks), expected);
		require(static_cast<bool>(source), "valid canonical chunk source was rejected");
		std::array<std::byte, 65'537U> buffer{};
		std::vector<std::byte> replay;
		for (;;)
		{
			auto count = (*source)->read(buffer);
			require(static_cast<bool>(count), "valid canonical chunk source failed during replay");
			if (*count == 0U)
				break;
			replay.insert(replay.end(), buffer.begin(), buffer.begin() + *count);
			require((*source)->resident_payload_byte_count() <= sqlite_payload_chunk_maximum_bytes,
					"validating source retained more than one borrowed chunk");
		}
		auto receipt = (*source)->finish();
		require(replay == input && receipt && receipt->full_checksum == expected.full_checksum,
				"validating chunk source changed payload bytes or checksum");

		auto replay_rows = std::make_shared<memory_replay_source>(chunks);
		sqlite_validated_replayable_payload_source replayable{replay_rows, expected};
		for (int pass{}; pass < 2; ++pass)
		{
			auto cursor = replayable.open_pass();
			require(static_cast<bool>(cursor),
					"replayable source did not open an independent pass");
			sqlite_incremental_sha256 digest;
			for (;;)
			{
				auto count = (*cursor)->read(buffer);
				require(static_cast<bool>(count), "replayable source pass failed");
				if (*count == 0U)
					break;
				require(static_cast<bool>(digest.update(std::span{buffer}.first(*count))),
						"replay pass SHA-256 rejected bounded bytes");
			}
			auto digest_value = digest.finish();
			require(digest_value && *digest_value == expected.full_checksum,
					"independent replay pass changed the full payload");
		}

		auto stopped = sqlite_validating_payload_source::open(
			std::make_unique<memory_chunk_cursor>(chunks), expected);
		require(stopped && (*stopped)->read(buffer) && !(*stopped)->finish(),
				"source finish accepted a decoder that stopped before EOF");

		auto empty_chunks = std::make_shared<std::vector<owned_chunk>>();
		auto empty = sqlite_validating_payload_source::open(
			std::make_unique<memory_chunk_cursor>(empty_chunks),
			expectation_for(std::span<const std::byte>{}));
		require(static_cast<bool>(empty), "empty validating source was rejected");
		auto empty_read = (*empty)->read(buffer);
		require(empty_read && *empty_read == 0U,
				"empty validating source did not reach canonical EOF");
	}

	void verify_resident_observation_scope()
	{
		const auto input = bytes(257U);
		{
			sqlite_payload_resident_observation_scope scope;
			require(scope.active(), "outer payload resident observation scope was not active");
			const auto initial = scope.observation();
			require(initial.maximum_resident_payload_buffer_bytes == 0U &&
						initial.observation_count == 0U &&
						initial.chunk_framer_instance_count == 0U &&
						initial.validating_source_instance_count == 0U &&
						!initial.observation_count_overflow,
					"fresh payload resident receipt was not empty");
			{
				sqlite_payload_resident_observation_scope nested;
				require(!nested.active(),
						"nested payload resident observation scope became active");
				capturing_port port;
				sqlite_payload_chunk_framer framer{&port};
				require(framer.append(input) && framer.finish(),
						"observed payload framer changed streaming behavior");
				const auto nested_receipt = nested.observation();
				require(nested_receipt.observation_count == 0U &&
							nested_receipt.chunk_framer_instance_count == 0U,
						"inactive nested payload scope captured observations");
			}
			const auto receipt = scope.observation();
			require(receipt.maximum_resident_payload_buffer_bytes ==
							sqlite_payload_chunk_maximum_bytes &&
						receipt.observation_count >= 2U &&
						receipt.chunk_framer_instance_count == 1U &&
						receipt.validating_source_instance_count == 0U &&
						!receipt.observation_count_overflow,
					"payload framer high-water receipt did not bind its actual instance");
		}

		{
			sqlite_payload_resident_observation_scope scope;
			auto chunks = make_chunks(input);
			auto source = sqlite_validating_payload_source::open(
				std::make_unique<memory_chunk_cursor>(chunks), expectation_for(input));
			require(static_cast<bool>(source), "observed validating source did not open");
			std::array<std::byte, 64U> output{};
			auto read = (*source)->read(output);
			require(read && *read == output.size(),
					"observed validating source changed read behavior");
			const auto receipt = scope.observation();
			require(receipt.maximum_resident_payload_buffer_bytes == input.size() &&
						receipt.observation_count >= 2U &&
						receipt.chunk_framer_instance_count == 0U &&
						receipt.validating_source_instance_count == 1U &&
						!receipt.observation_count_overflow,
					"validating-source high-water receipt did not bind its actual instance");
		}
	}

	template <class Mutator>
	void require_corrupt_source(const std::span<const std::byte> input,
								Mutator mutate,
								const std::string& message,
								sqlite_payload_stream_expectation expectation = {})
	{
		auto chunks = make_chunks(input);
		mutate(*chunks);
		if (expectation.full_checksum.empty())
			expectation = expectation_for(input);
		auto source = sqlite_validating_payload_source::open(
			std::make_unique<memory_chunk_cursor>(chunks), std::move(expectation));
		require(static_cast<bool>(source), message + " (invalid expectation instead of row)");
		std::array<std::byte, 64U * 1024U> buffer{};
		bool failed{};
		for (;;)
		{
			auto count = (*source)->read(buffer);
			if (!count)
			{
				failed = true;
				break;
			}
			if (*count == 0U)
				break;
		}
		require(failed, message);
	}

	void verify_source_rejections()
	{
		const auto boundary_input =
			bytes(static_cast<std::size_t>(sqlite_payload_chunk_maximum_bytes) + 257U);
		const auto small_input = bytes(257U);
		require_corrupt_source(
			small_input,
			[](auto& chunks)
			{
				++chunks[0].offset;
			},
			"validating source accepted a shifted byte offset");
		require_corrupt_source(
			boundary_input,
			[](auto& chunks)
			{
				--chunks[0].count;
				chunks[0].payload.pop_back();
			},
			"validating source accepted a noncanonical non-final byte count");
		require_corrupt_source(
			small_input,
			[](auto& chunks)
			{
				chunks[0].checksum[7U] = 'A';
			},
			"validating source accepted noncanonical checksum spelling");
		require_corrupt_source(
			small_input,
			[](auto& chunks)
			{
				chunks[0].payload[0] ^= std::byte{0x01U};
			},
			"validating source accepted a per-chunk checksum mismatch");
		require_corrupt_source(
			small_input,
			[](auto& chunks)
			{
				chunks.pop_back();
			},
			"validating source accepted a missing final chunk");
		require_corrupt_source(
			small_input,
			[](auto& chunks)
			{
				auto extra = chunks.back();
				extra.ordinal = 1U;
				extra.offset = sqlite_payload_chunk_maximum_bytes;
				chunks.push_back(std::move(extra));
			},
			"validating source accepted an extra chunk");

		auto wrong_full = expectation_for(small_input);
		wrong_full.full_checksum =
			"sha256:0000000000000000000000000000000000000000000000000000000000000000";
		require_corrupt_source(
			small_input,
			[](auto&)
			{
			},
			"validating source accepted a full checksum mismatch",
			std::move(wrong_full));
	}
} // namespace

int main()
{
	try
	{
		verify_incremental_sha256();
		verify_chunk_framer();
		verify_validating_source();
		verify_resident_observation_scope();
		verify_source_rejections();
		return 0;
	}
	catch (const std::exception& failure)
	{
		std::cerr << failure.what() << '\n';
		return 1;
	}
}
