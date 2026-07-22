#include "sqlite_payload_streaming_internal.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <ranges>
#include <utility>

namespace cxxlens::sdk
{
	namespace
	{
		thread_local sqlite_payload_resident_observation_scope* active_resident_observation_scope{};

		constexpr std::array<std::uint32_t, 8U> sha256_initial_state{
			0x6a09e667U,
			0xbb67ae85U,
			0x3c6ef372U,
			0xa54ff53aU,
			0x510e527fU,
			0x9b05688cU,
			0x1f83d9abU,
			0x5be0cd19U,
		};

		constexpr std::array<std::uint32_t, 64U> sha256_round_constants{
			0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
			0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
			0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
			0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
			0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
			0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
			0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
			0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
			0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
			0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
			0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
		};

		[[nodiscard]] error stream_state_error(std::string detail)
		{
			return {"store.transaction-state", "sqlite-payload-stream", std::move(detail)};
		}

		[[nodiscard]] error stream_allocation_error()
		{
			return {"store.backend-unavailable", "sqlite-payload-stream", "allocation"};
		}

		[[nodiscard]] error stream_counter_error(std::string field)
		{
			return {"store.counter-overflow", std::move(field), {}};
		}

		[[nodiscard]] error chunk_corrupt_error(std::string detail)
		{
			return {"store.corrupt", "sqlite-payload-chunk", std::move(detail)};
		}

		[[nodiscard]] bool canonical_sha256(const std::string_view value) noexcept
		{
			return value.size() == 71U && value.starts_with("sha256:") &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] bool canonical_chunk_count(const std::uint64_t byte_count,
												 std::uint64_t& output) noexcept
		{
			if (byte_count == 0U)
			{
				output = 0U;
				return true;
			}
			output = 1U + (byte_count - 1U) / sqlite_payload_chunk_maximum_bytes;
			return output <= sqlite_payload_chunk_maximum_count;
		}
	} // namespace

	sqlite_payload_resident_observation_scope::sqlite_payload_resident_observation_scope() noexcept
	{
		if (active_resident_observation_scope != nullptr)
			return;
		active_resident_observation_scope = this;
		active_ = true;
	}

	sqlite_payload_resident_observation_scope::~sqlite_payload_resident_observation_scope() noexcept
	{
		if (active_ && active_resident_observation_scope == this)
			active_resident_observation_scope = nullptr;
	}

	bool sqlite_payload_resident_observation_scope::active() const noexcept
	{
		return active_;
	}

	sqlite_payload_resident_observation_receipt
	sqlite_payload_resident_observation_scope::observation() const noexcept
	{
		return receipt_;
	}

	void sqlite_payload_resident_observation_scope::record(
		const sqlite_payload_resident_instance_kind kind,
		const std::size_t resident_bytes,
		const bool instance_created) noexcept
	{
		const auto increment = [this](std::uint64_t& value)
		{
			if (value == std::numeric_limits<std::uint64_t>::max())
				receipt_.observation_count_overflow = true;
			else
				++value;
		};
		increment(receipt_.observation_count);
		if (instance_created)
		{
			switch (kind)
			{
				case sqlite_payload_resident_instance_kind::chunk_framer:
					increment(receipt_.chunk_framer_instance_count);
					break;
				case sqlite_payload_resident_instance_kind::validating_source:
					increment(receipt_.validating_source_instance_count);
					break;
			}
		}
		const auto bytes = static_cast<std::uint64_t>(resident_bytes);
		if (static_cast<std::size_t>(bytes) != resident_bytes)
		{
			receipt_.maximum_resident_payload_buffer_bytes =
				std::numeric_limits<std::uint64_t>::max();
			receipt_.observation_count_overflow = true;
			return;
		}
		receipt_.maximum_resident_payload_buffer_bytes =
			std::max(receipt_.maximum_resident_payload_buffer_bytes, bytes);
	}

	void observe_sqlite_payload_resident_bytes(const sqlite_payload_resident_instance_kind kind,
											   const std::size_t resident_bytes,
											   const bool instance_created) noexcept
	{
		if (active_resident_observation_scope != nullptr)
			active_resident_observation_scope->record(kind, resident_bytes, instance_created);
	}

	bool sqlite_u128_census::add(const std::uint64_t value) noexcept
	{
		const auto prior = low;
		low += value;
		if (low >= prior)
			return true;
		if (high == std::numeric_limits<std::uint64_t>::max())
		{
			low = prior;
			return false;
		}
		++high;
		return true;
	}

	bool sqlite_checked_add_u64(const std::uint64_t left,
								const std::uint64_t right,
								std::uint64_t& output) noexcept
	{
		if (right > std::numeric_limits<std::uint64_t>::max() - left)
			return false;
		output = left + right;
		return true;
	}

	bool sqlite_checked_multiply_u64(const std::uint64_t left,
									 const std::uint64_t right,
									 std::uint64_t& output) noexcept
	{
		if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
			return false;
		output = left * right;
		return true;
	}

	bool sqlite_checked_sha256_byte_count(const std::uint64_t current,
										  const std::uint64_t added,
										  std::uint64_t& output) noexcept
	{
		return current <= sqlite_sha256_maximum_byte_count &&
			added <= sqlite_sha256_maximum_byte_count - current &&
			sqlite_checked_add_u64(current, added, output);
	}

	sqlite_incremental_sha256::sqlite_incremental_sha256() noexcept
	{
		reset();
	}

	result<void> sqlite_incremental_sha256::update(const std::span<const std::byte> input_value)
	{
		if (finished_)
			return unexpected(stream_state_error("sha256-finished"));
		const auto added = static_cast<std::uint64_t>(input_value.size());
		if (static_cast<std::size_t>(added) != input_value.size())
			return unexpected(stream_counter_error("sha256-byte-count"));
		std::uint64_t next_total{};
		if (!sqlite_checked_sha256_byte_count(total_bytes_, added, next_total))
			return unexpected(stream_counter_error("sha256-bit-count"));

		auto input = input_value;
		if (pending_size_ != 0U)
		{
			const auto count = std::min(input.size(), block_bytes - pending_size_);
			std::ranges::copy(input.first(count), pending_.begin() + pending_size_);
			pending_size_ += count;
			input = input.subspan(count);
			if (pending_size_ != block_bytes)
			{
				total_bytes_ = next_total;
				return {};
			}
			transform(pending_);
			pending_size_ = 0U;
		}
		while (input.size() >= block_bytes)
		{
			transform(input.first(block_bytes));
			input = input.subspan(block_bytes);
		}
		std::ranges::copy(input, pending_.begin());
		pending_size_ = input.size();
		total_bytes_ = next_total;
		return {};
	}

	result<std::string> sqlite_incremental_sha256::finish()
	{
		if (finished_)
			return unexpected(stream_state_error("sha256-finished"));
		finished_ = true;
		const auto bit_count = total_bytes_ * 8U;
		pending_[pending_size_++] = std::byte{0x80U};
		if (pending_size_ > 56U)
		{
			std::fill(pending_.begin() + static_cast<std::ptrdiff_t>(pending_size_),
					  pending_.end(),
					  std::byte{});
			transform(pending_);
			pending_size_ = 0U;
		}
		std::fill(pending_.begin() + static_cast<std::ptrdiff_t>(pending_size_),
				  pending_.begin() + 56,
				  std::byte{});
		for (std::size_t index{}; index < 8U; ++index)
			pending_[56U + index] = static_cast<std::byte>(
				(bit_count >> (56U - static_cast<unsigned>(index * 8U))) & 0xffU);
		transform(pending_);

		try
		{
			constexpr std::string_view digits{"0123456789abcdef"};
			std::string output{"sha256:"};
			output.reserve(71U);
			for (const auto word : state_)
				for (std::uint32_t shift = 28U;; shift -= 4U)
				{
					output.push_back(digits[(word >> shift) & 0x0fU]);
					if (shift == 0U)
						break;
				}
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(stream_allocation_error());
		}
	}

	void sqlite_incremental_sha256::reset() noexcept
	{
		state_ = sha256_initial_state;
		pending_.fill(std::byte{});
		pending_size_ = 0U;
		total_bytes_ = 0U;
		finished_ = false;
	}

	std::uint64_t sqlite_incremental_sha256::byte_count() const noexcept
	{
		return total_bytes_;
	}

	bool sqlite_incremental_sha256::finished() const noexcept
	{
		return finished_;
	}

	void sqlite_incremental_sha256::transform(const std::span<const std::byte> block) noexcept
	{
		std::array<std::uint32_t, 64U> schedule{};
		for (std::size_t index{}; index < 16U; ++index)
		{
			const auto offset = index * 4U;
			schedule[index] = (std::to_integer<std::uint32_t>(block[offset]) << 24U) |
				(std::to_integer<std::uint32_t>(block[offset + 1U]) << 16U) |
				(std::to_integer<std::uint32_t>(block[offset + 2U]) << 8U) |
				std::to_integer<std::uint32_t>(block[offset + 3U]);
		}
		for (std::size_t index = 16U; index < schedule.size(); ++index)
		{
			const auto small_zero = std::rotr(schedule[index - 15U], 7) ^
				std::rotr(schedule[index - 15U], 18) ^ (schedule[index - 15U] >> 3U);
			const auto small_one = std::rotr(schedule[index - 2U], 17) ^
				std::rotr(schedule[index - 2U], 19) ^ (schedule[index - 2U] >> 10U);
			schedule[index] = schedule[index - 16U] + small_zero + schedule[index - 7U] + small_one;
		}
		auto [a, b, c, d, e, f, g, h] = state_;
		for (std::size_t index{}; index < schedule.size(); ++index)
		{
			const auto big_one = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
			const auto choose = (e & f) ^ (~e & g);
			const auto first =
				h + big_one + choose + sha256_round_constants[index] + schedule[index];
			const auto big_zero = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
			const auto majority = (a & b) ^ (a & c) ^ (b & c);
			const auto second = big_zero + majority;
			h = g;
			g = f;
			f = e;
			e = d + first;
			d = c;
			c = b;
			b = a;
			a = first + second;
		}
		state_[0U] += a;
		state_[1U] += b;
		state_[2U] += c;
		state_[3U] += d;
		state_[4U] += e;
		state_[5U] += f;
		state_[6U] += g;
		state_[7U] += h;
	}

	sqlite_payload_chunk_framer::sqlite_payload_chunk_framer(
		sqlite_payload_chunk_port* const port) noexcept
		: port_{port}
	{
		observe_sqlite_payload_resident_bytes(
			sqlite_payload_resident_instance_kind::chunk_framer, 0U, true);
	}

	result<void> sqlite_payload_chunk_framer::append(const std::span<const std::byte> bytes)
	{
		if (finished_ || poisoned_)
			return unexpected(stream_state_error("chunk-framer-closed"));
		const auto added = static_cast<std::uint64_t>(bytes.size());
		if (static_cast<std::size_t>(added) != bytes.size())
		{
			poisoned_ = true;
			return unexpected(stream_counter_error("payload_byte_count"));
		}
		std::uint64_t next_total{};
		std::uint64_t next_sha_total{};
		auto next_aggregate = aggregate_byte_count_;
		if (!sqlite_checked_add_u64(total_byte_count_, added, next_total) ||
			!sqlite_checked_sha256_byte_count(full_digest_.byte_count(), added, next_sha_total) ||
			next_sha_total != next_total || !next_aggregate.add(added))
		{
			poisoned_ = true;
			return unexpected(stream_counter_error("payload_byte_count"));
		}
		if (port_ != nullptr && !bytes.empty() &&
			chunk_.capacity() < sqlite_payload_chunk_maximum_bytes)
		{
			try
			{
				chunk_.reserve(static_cast<std::size_t>(sqlite_payload_chunk_maximum_bytes));
				observe_sqlite_payload_resident_bytes(
					sqlite_payload_resident_instance_kind::chunk_framer, chunk_.capacity(), false);
			}
			catch (const std::bad_alloc&)
			{
				poisoned_ = true;
				return unexpected(stream_allocation_error());
			}
		}
		if (auto updated = full_digest_.update(bytes); !updated)
		{
			poisoned_ = true;
			return unexpected(std::move(updated.error()));
		}
		total_byte_count_ = next_total;
		aggregate_byte_count_ = next_aggregate;

		auto remaining = bytes;
		while (!remaining.empty())
		{
			const auto available = sqlite_payload_chunk_maximum_bytes - current_chunk_byte_count_;
			const auto count =
				static_cast<std::size_t>(std::min<std::uint64_t>(available, remaining.size()));
			const auto part = remaining.first(count);
			if (auto updated = chunk_digest_.update(part); !updated)
			{
				poisoned_ = true;
				return unexpected(std::move(updated.error()));
			}
			if (port_ != nullptr)
			{
				try
				{
					chunk_.insert(chunk_.end(), part.begin(), part.end());
				}
				catch (const std::bad_alloc&)
				{
					poisoned_ = true;
					return unexpected(stream_allocation_error());
				}
			}
			current_chunk_byte_count_ += static_cast<std::uint64_t>(count);
			remaining = remaining.subspan(count);
			if (current_chunk_byte_count_ == sqlite_payload_chunk_maximum_bytes)
				if (auto completed = complete_chunk(); !completed)
					return completed;
		}
		return {};
	}

	result<void> sqlite_payload_chunk_framer::complete_chunk()
	{
		if (current_chunk_byte_count_ == 0U ||
			current_chunk_byte_count_ > sqlite_payload_chunk_maximum_bytes ||
			chunk_count_ >= sqlite_payload_chunk_maximum_count)
		{
			poisoned_ = true;
			return unexpected(stream_counter_error("payload_chunk_count"));
		}

		if (port_ != nullptr)
		{
			auto checksum = chunk_digest_.finish();
			if (!checksum)
			{
				poisoned_ = true;
				return unexpected(std::move(checksum.error()));
			}
			std::uint64_t byte_offset{};
			if (chunk_count_ > sqlite_payload_chunk_maximum_ordinal ||
				!sqlite_checked_multiply_u64(
					chunk_count_, sqlite_payload_chunk_maximum_bytes, byte_offset) ||
				chunk_.size() != current_chunk_byte_count_)
			{
				poisoned_ = true;
				return unexpected(stream_counter_error("chunk_ordinal"));
			}
			const sqlite_payload_chunk_frame frame{
				chunk_count_, byte_offset, current_chunk_byte_count_, *checksum, chunk_};
			if (auto emitted = port_->emit(frame); !emitted)
			{
				poisoned_ = true;
				return unexpected(std::move(emitted.error()));
			}
			chunk_.clear();
		}
		++chunk_count_;
		current_chunk_byte_count_ = 0U;
		chunk_digest_.reset();
		return {};
	}

	result<sqlite_payload_stream_receipt> sqlite_payload_chunk_framer::finish()
	{
		if (finished_ || poisoned_)
			return unexpected(stream_state_error("chunk-framer-closed"));
		if (current_chunk_byte_count_ != 0U)
			if (auto completed = complete_chunk(); !completed)
				return unexpected(std::move(completed.error()));

		std::uint64_t expected_chunks{};
		if (!canonical_chunk_count(total_byte_count_, expected_chunks) ||
			expected_chunks != chunk_count_ || aggregate_byte_count_.high != 0U ||
			aggregate_byte_count_.low != total_byte_count_)
		{
			poisoned_ = true;
			return unexpected(stream_counter_error("payload_chunk_count"));
		}
		auto checksum = full_digest_.finish();
		if (!checksum)
		{
			poisoned_ = true;
			return unexpected(std::move(checksum.error()));
		}
		finished_ = true;
		return sqlite_payload_stream_receipt{
			total_byte_count_, chunk_count_, aggregate_byte_count_, std::move(*checksum)};
	}

	std::size_t sqlite_payload_chunk_framer::resident_payload_byte_count() const noexcept
	{
		return chunk_.capacity();
	}

	bool sqlite_payload_chunk_framer::measure_only() const noexcept
	{
		return port_ == nullptr;
	}

	sqlite_validating_payload_source::sqlite_validating_payload_source(
		std::unique_ptr<sqlite_payload_chunk_record_source> rows,
		sqlite_payload_stream_expectation expectation) noexcept
		: rows_{std::move(rows)}, expectation_{std::move(expectation)}
	{
		observe_sqlite_payload_resident_bytes(
			sqlite_payload_resident_instance_kind::validating_source, 0U, true);
	}

	result<std::unique_ptr<sqlite_validating_payload_source>>
	sqlite_validating_payload_source::open(std::unique_ptr<sqlite_payload_chunk_record_source> rows,
										   sqlite_payload_stream_expectation expectation)
	{
		std::uint64_t expected_chunks{};
		if (!rows || !canonical_chunk_count(expectation.byte_count, expected_chunks) ||
			expected_chunks != expectation.chunk_count ||
			expectation.byte_count > sqlite_sha256_maximum_byte_count ||
			(expectation.validate_full_checksum && !canonical_sha256(expectation.full_checksum)))
			return unexpected(chunk_corrupt_error("expectation"));
		try
		{
			return std::unique_ptr<sqlite_validating_payload_source>{
				new sqlite_validating_payload_source{std::move(rows), std::move(expectation)}};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(stream_allocation_error());
		}
	}

	result<bool> sqlite_validating_payload_source::load_next_chunk()
	{
		if (source_eof_)
			return false;
		auto next = rows_->next();
		if (!next)
		{
			poisoned_ = true;
			return unexpected(std::move(next.error()));
		}
		if (!*next)
		{
			source_eof_ = true;
			current_chunk_ = {};
			current_offset_ = 0U;
			return false;
		}

		const auto& row = **next;
		observe_sqlite_payload_resident_bytes(
			sqlite_payload_resident_instance_kind::validating_source, row.payload.size(), false);
		std::uint64_t expected_offset{};
		const auto actual_size = static_cast<std::uint64_t>(row.payload.size());
		const bool final_chunk = observed_chunk_count_ < expectation_.chunk_count &&
			observed_chunk_count_ + 1U == expectation_.chunk_count;
		if (static_cast<std::size_t>(actual_size) != row.payload.size() ||
			observed_chunk_count_ >= expectation_.chunk_count ||
			observed_chunk_count_ > sqlite_payload_chunk_maximum_ordinal ||
			row.ordinal != observed_chunk_count_ ||
			!sqlite_checked_multiply_u64(
				observed_chunk_count_, sqlite_payload_chunk_maximum_bytes, expected_offset) ||
			row.byte_offset != expected_offset || row.byte_count != actual_size ||
			row.byte_count == 0U || row.byte_count > sqlite_payload_chunk_maximum_bytes ||
			(!final_chunk && row.byte_count != sqlite_payload_chunk_maximum_bytes) ||
			!canonical_sha256(row.checksum))
		{
			poisoned_ = true;
			return unexpected(chunk_corrupt_error("framing"));
		}

		sqlite_incremental_sha256 chunk_digest;
		if (auto updated = chunk_digest.update(row.payload); !updated)
		{
			poisoned_ = true;
			return unexpected(std::move(updated.error()));
		}
		auto checksum = chunk_digest.finish();
		if (!checksum)
		{
			poisoned_ = true;
			return unexpected(std::move(checksum.error()));
		}
		if (*checksum != row.checksum)
		{
			poisoned_ = true;
			return unexpected(chunk_corrupt_error("chunk-checksum"));
		}

		std::uint64_t next_total{};
		auto next_aggregate = aggregate_byte_count_;
		if (!sqlite_checked_add_u64(observed_byte_count_, row.byte_count, next_total) ||
			next_total > expectation_.byte_count || !next_aggregate.add(row.byte_count))
		{
			poisoned_ = true;
			return unexpected(stream_counter_error("payload_byte_count"));
		}
		if (auto updated = full_digest_.update(row.payload); !updated)
		{
			poisoned_ = true;
			return unexpected(std::move(updated.error()));
		}
		observed_byte_count_ = next_total;
		aggregate_byte_count_ = next_aggregate;
		++observed_chunk_count_;
		current_chunk_ = row.payload;
		current_offset_ = 0U;
		return true;
	}

	result<std::size_t> sqlite_validating_payload_source::read(const std::span<std::byte> output)
	{
		if (poisoned_)
			return unexpected(stream_state_error("payload-source-poisoned"));
		if (output.empty())
			return unexpected(stream_state_error("payload-source-empty-window"));
		if (finished_)
			return std::size_t{};
		if (current_offset_ == current_chunk_.size())
		{
			current_chunk_ = {};
			current_offset_ = 0U;
			auto loaded = load_next_chunk();
			if (!loaded)
				return unexpected(std::move(loaded.error()));
			if (!*loaded)
			{
				auto finalized = finalize_at_eof();
				if (!finalized)
					return unexpected(std::move(finalized.error()));
				return std::size_t{};
			}
		}
		const auto count = std::min(output.size(), current_chunk_.size() - current_offset_);
		std::memmove(output.data(), current_chunk_.data() + current_offset_, count);
		current_offset_ += count;
		return count;
	}

	result<sqlite_payload_stream_receipt> sqlite_validating_payload_source::finalize_at_eof()
	{
		if (!source_eof_ || observed_chunk_count_ != expectation_.chunk_count ||
			observed_byte_count_ != expectation_.byte_count || aggregate_byte_count_.high != 0U ||
			aggregate_byte_count_.low != observed_byte_count_)
		{
			poisoned_ = true;
			return unexpected(chunk_corrupt_error("payload-census"));
		}
		auto checksum = full_digest_.finish();
		if (!checksum)
		{
			poisoned_ = true;
			return unexpected(std::move(checksum.error()));
		}
		if (expectation_.validate_full_checksum && *checksum != expectation_.full_checksum)
		{
			poisoned_ = true;
			return unexpected(chunk_corrupt_error("payload-checksum"));
		}
		try
		{
			receipt_ = sqlite_payload_stream_receipt{observed_byte_count_,
													 observed_chunk_count_,
													 aggregate_byte_count_,
													 std::move(*checksum)};
		}
		catch (const std::bad_alloc&)
		{
			poisoned_ = true;
			return unexpected(stream_allocation_error());
		}
		finished_ = true;
		try
		{
			return *receipt_;
		}
		catch (const std::bad_alloc&)
		{
			poisoned_ = true;
			return unexpected(stream_allocation_error());
		}
	}

	result<sqlite_payload_stream_receipt> sqlite_validating_payload_source::finish()
	{
		if (poisoned_)
			return unexpected(stream_state_error("payload-source-poisoned"));
		if (receipt_)
			try
			{
				return *receipt_;
			}
			catch (const std::bad_alloc&)
			{
				poisoned_ = true;
				return unexpected(stream_allocation_error());
			}
		if (current_offset_ != current_chunk_.size() ||
			observed_chunk_count_ != expectation_.chunk_count)
			return unexpected(stream_state_error("payload-source-not-consumed"));
		if (!source_eof_)
		{
			auto extra = rows_->next();
			if (!extra)
			{
				poisoned_ = true;
				return unexpected(std::move(extra.error()));
			}
			if (*extra)
			{
				poisoned_ = true;
				return unexpected(chunk_corrupt_error("extra-chunk"));
			}
			source_eof_ = true;
		}
		return finalize_at_eof();
	}

	std::size_t sqlite_validating_payload_source::resident_payload_byte_count() const noexcept
	{
		return current_chunk_.size();
	}

	sqlite_validated_replayable_payload_source::sqlite_validated_replayable_payload_source(
		std::shared_ptr<const sqlite_replayable_payload_chunk_source> rows,
		sqlite_payload_stream_expectation expectation)
		: rows_{std::move(rows)}, expectation_{std::move(expectation)}
	{
	}

	result<std::unique_ptr<sqlite_bounded_byte_source>>
	sqlite_validated_replayable_payload_source::open_pass() const
	{
		if (!rows_)
			return unexpected(stream_state_error("payload-replay-source-missing"));
		try
		{
			auto rows = rows_->open_chunk_pass();
			if (!rows)
				return unexpected(std::move(rows.error()));
			auto source = sqlite_validating_payload_source::open(std::move(*rows), expectation_);
			if (!source)
				return unexpected(std::move(source.error()));
			std::unique_ptr<sqlite_bounded_byte_source> output{std::move(*source)};
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(stream_allocation_error());
		}
	}
} // namespace cxxlens::sdk
