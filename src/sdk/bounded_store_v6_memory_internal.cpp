#include "bounded_store_v6_memory_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

#include "store_identity_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		constexpr std::string_view frame_domain{"cxxlens/df-0200-partition-event-frame/v1"};
		constexpr std::size_t frame_prefix_bytes = 17U;
		constexpr std::size_t frame_checksum_bytes = 32U;
		constexpr std::size_t memory_chunk_bytes = 64U * 1024U;

		[[nodiscard]] error failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}
		[[nodiscard]] error invariant(std::string field, std::string detail)
		{
			return failure("store.invariant-breach", std::move(field), std::move(detail));
		}
		[[nodiscard]] error corrupt(std::string field, std::string detail)
		{
			return failure("store.corrupt", std::move(field), std::move(detail));
		}
		[[nodiscard]] error resource(std::string field, std::string detail)
		{
			return failure("store.resource-limit", std::move(field), std::move(detail));
		}

		[[nodiscard]] error
		memory_observation_error(const store_backend_operation_observation observation)
		{
			switch (observation.fault)
			{
				case store_backend_observation_fault::publication_sequence_exhausted:
					return failure("store.counter-overflow", "publication_sequence");
				case store_backend_observation_fault::physical_generation_exhausted:
					return failure("store.counter-overflow", "physical_generation");
				case store_backend_observation_fault::projection_mismatch:
					return corrupt("projection", "expected-actual");
				case store_backend_observation_fault::none:
					return invariant("operation-port", "missing-failure");
				case store_backend_observation_fault::corrupt_current:
				case store_backend_observation_fault::corrupt_publication:
				case store_backend_observation_fault::snapshot_ambiguous:
				case store_backend_observation_fault::backend_failure:
				case store_backend_observation_fault::commit_outcome_unknown:
					return invariant("operation-port", "memory-failure");
			}
			return invariant("operation-port", "unknown-fault");
		}

		[[nodiscard]] result<void>
		observe_memory_operation(const std::shared_ptr<store_operation_port>& operations,
								 const store_backend_operation_event& event)
		{
			if (!operations)
				return unexpected(invariant("operation-port", "missing"));
			const auto observed = operations->observe_backend_operation(event);
			if (observed.fault != store_backend_observation_fault::none)
				return unexpected(memory_observation_error(observed));
			return {};
		}

		[[nodiscard]] bool checked_add(const std::uint64_t left,
									   const std::uint64_t right,
									   std::uint64_t& output) noexcept
		{
			if (right > std::numeric_limits<std::uint64_t>::max() - left)
				return false;
			output = left + right;
			return true;
		}

		[[nodiscard]] bool checked_increment(std::uint64_t& value) noexcept
		{
			return checked_add(value, 1U, value);
		}

		void append_u64(std::vector<std::byte>& output, const std::uint64_t value)
		{
			for (std::size_t shift = 56U;; shift -= 8U)
			{
				output.push_back(static_cast<std::byte>(value >> shift));
				if (shift == 0U)
					break;
			}
		}

		void append_u64(std::array<std::byte, 8U>& output, const std::uint64_t value) noexcept
		{
			for (std::size_t index{}; index < output.size(); ++index)
				output[index] = static_cast<std::byte>(value >> (56U - index * 8U));
		}

		[[nodiscard]] std::uint64_t read_u64(const auto& bytes, const std::size_t offset) noexcept
		{
			std::uint64_t value{};
			for (std::size_t index{}; index < 8U; ++index)
				value = (value << 8U) |
					static_cast<std::uint64_t>(
							std::to_integer<unsigned char>(bytes[offset + index]));
			return value;
		}

		[[nodiscard]] bool equal_bytes(const std::array<std::byte, 32U>& left,
									   const std::array<std::byte, 32U>& right) noexcept
		{
			return std::equal(left.begin(), left.end(), right.begin(), right.end());
		}

		class sha256 final
		{
		  public:
			sha256() noexcept
				: state_{0x6a09e667U,
						 0xbb67ae85U,
						 0x3c6ef372U,
						 0xa54ff53aU,
						 0x510e527fU,
						 0x9b05688cU,
						 0x1f83d9abU,
						 0x5be0cd19U}
			{
			}

			void update(const std::span<const std::byte> input) noexcept
			{
				for (const auto byte : input)
				{
					buffer_[buffer_size_++] = byte;
					if (buffer_size_ == buffer_.size())
					{
						transform(buffer_);
						buffer_size_ = 0U;
					}
					++total_bytes_;
				}
			}

			[[nodiscard]] std::array<std::byte, 32U> finish() const noexcept
			{
				sha256 copy{*this};
				const auto bits = copy.total_bytes_ * 8U;
				copy.buffer_[copy.buffer_size_++] = static_cast<std::byte>(0x80U);
				if (copy.buffer_size_ > 56U)
				{
					while (copy.buffer_size_ < 64U)
						copy.buffer_[copy.buffer_size_++] = std::byte{};
					copy.transform(copy.buffer_);
					copy.buffer_size_ = 0U;
				}
				while (copy.buffer_size_ < 56U)
					copy.buffer_[copy.buffer_size_++] = std::byte{};
				for (std::size_t index{}; index < 8U; ++index)
					copy.buffer_[56U + index] = static_cast<std::byte>(bits >> (56U - index * 8U));
				copy.transform(copy.buffer_);
				std::array<std::byte, 32U> output{};
				for (std::size_t index{}; index < copy.state_.size(); ++index)
				{
					output[index * 4U] = static_cast<std::byte>(copy.state_[index] >> 24U);
					output[index * 4U + 1U] = static_cast<std::byte>(copy.state_[index] >> 16U);
					output[index * 4U + 2U] = static_cast<std::byte>(copy.state_[index] >> 8U);
					output[index * 4U + 3U] = static_cast<std::byte>(copy.state_[index]);
				}
				return output;
			}

		  private:
			static constexpr std::array<std::uint32_t, 64U> constants{
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
				0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

			static std::uint32_t rotr(const std::uint32_t value, const unsigned count) noexcept
			{
				return (value >> count) | (value << (32U - count));
			}

			void transform(const std::array<std::byte, 64U>& block) noexcept
			{
				std::array<std::uint32_t, 64U> schedule{};
				for (std::size_t index{}; index < 16U; ++index)
				{
					const auto offset = index * 4U;
					schedule[index] =
						(static_cast<std::uint32_t>(std::to_integer<unsigned char>(block[offset]))
						 << 24U) |
						(static_cast<std::uint32_t>(
							 std::to_integer<unsigned char>(block[offset + 1U]))
						 << 16U) |
						(static_cast<std::uint32_t>(
							 std::to_integer<unsigned char>(block[offset + 2U]))
						 << 8U) |
						static_cast<std::uint32_t>(
							std::to_integer<unsigned char>(block[offset + 3U]));
				}
				for (std::size_t index = 16U; index < schedule.size(); ++index)
				{
					const auto s0 = rotr(schedule[index - 15U], 7U) ^
						rotr(schedule[index - 15U], 18U) ^ (schedule[index - 15U] >> 3U);
					const auto s1 = rotr(schedule[index - 2U], 17U) ^
						rotr(schedule[index - 2U], 19U) ^ (schedule[index - 2U] >> 10U);
					schedule[index] = schedule[index - 16U] + s0 + schedule[index - 7U] + s1;
				}
				std::uint32_t a = state_[0U], b = state_[1U], c = state_[2U], d = state_[3U];
				std::uint32_t e = state_[4U], f = state_[5U], g = state_[6U], h = state_[7U];
				for (std::size_t index{}; index < 64U; ++index)
				{
					const auto s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
					const auto choose = (e & f) ^ ((~e) & g);
					const auto temp1 = h + s1 + choose + constants[index] + schedule[index];
					const auto s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
					const auto majority = (a & b) ^ (a & c) ^ (b & c);
					const auto temp2 = s0 + majority;
					h = g;
					g = f;
					f = e;
					e = d + temp1;
					d = c;
					c = b;
					b = a;
					a = temp1 + temp2;
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

			std::array<std::uint32_t, 8U> state_{};
			std::array<std::byte, 64U> buffer_{};
			std::size_t buffer_size_{};
			std::uint64_t total_bytes_{};
		};

		[[nodiscard]] std::string hex_digest(const std::array<std::byte, 32U>& digest)
		{
			static constexpr char hex[] = "0123456789abcdef";
			std::string result{"sha256:"};
			result.reserve(71U);
			for (const auto byte : digest)
			{
				const auto value = std::to_integer<unsigned char>(byte);
				result.push_back(hex[value >> 4U]);
				result.push_back(hex[value & 0x0fU]);
			}
			return result;
		}

		[[nodiscard]] std::array<std::byte, 32U>
		raw_digest(const std::span<const std::byte> projection)
		{
			sha256 hash;
			std::array<std::byte, 8U> length{};
			append_u64(length, static_cast<std::uint64_t>(frame_domain.size()));
			hash.update(length);
			hash.update(std::span<const std::byte>{
				reinterpret_cast<const std::byte*>(frame_domain.data()), frame_domain.size()});
			append_u64(length, static_cast<std::uint64_t>(projection.size()));
			hash.update(length);
			hash.update(projection);
			return hash.finish();
		}

		void update_raw_digest_prefix(sha256& hash,
									  const std::uint64_t projection_bytes,
									  const std::uint64_t offset,
									  const std::span<const std::byte> bytes) noexcept
		{
			if (offset >= projection_bytes)
				return;
			const auto count = static_cast<std::size_t>(
				std::min<std::uint64_t>(bytes.size(), projection_bytes - offset));
			hash.update(bytes.first(count));
		}

		[[nodiscard]] std::vector<std::byte> order_key(const bounded_store_v6_record_kind kind,
													   const std::span<const std::byte> key,
													   const std::span<const std::byte> payload)
		{
			std::vector<std::byte> kind_bytes{static_cast<std::byte>(kind)};
			auto encoded = canonical_binary(canonical_value::from_tuple(
				{canonical_value::from_bytes(std::move(kind_bytes)),
				 canonical_value::from_bytes(std::vector<std::byte>{key.begin(), key.end()}),
				 canonical_value::from_bytes(
					 std::vector<std::byte>{payload.begin(), payload.end()})}));
			return encoded ? std::move(*encoded) : std::vector<std::byte>{};
		}

		void update_record_identity_hash(sha256& hash,
										 const std::string_view domain,
										 const bounded_store_v6_record_kind kind,
										 const std::span<const std::byte> key,
										 const std::span<const std::byte> payload) noexcept
		{
			std::array<std::byte, 8U> length{};
			append_u64(length, static_cast<std::uint64_t>(domain.size()));
			hash.update(length);
			hash.update(std::span<const std::byte>{
				reinterpret_cast<const std::byte*>(domain.data()), domain.size()});
			const std::array kind_bytes{static_cast<std::byte>(kind)};
			hash.update(kind_bytes);
			append_u64(length, key.size());
			hash.update(length);
			hash.update(key);
			append_u64(length, payload.size());
			hash.update(length);
			hash.update(payload);
		}

		struct memory_chunk
		{
			std::array<std::byte, memory_chunk_bytes> bytes{};
		};

		struct memory_backing
		{
			std::vector<std::unique_ptr<memory_chunk>> chunks;
			std::uint64_t size{};
			std::uint64_t record_count{};
			std::uint64_t partition_count{};
			std::uint64_t claim_count{};
			std::uint64_t row_count{};
			std::uint64_t coverage_count{};
			std::uint64_t unresolved_count{};
			bool began{};
			bool ended{};
			sha256 all_hash;
			sha256 semantic_hash;
			sha256 export_hash;

			[[nodiscard]] std::byte byte_at(const std::uint64_t offset) const noexcept
			{
				const auto chunk = static_cast<std::size_t>(offset / memory_chunk_bytes);
				const auto within = static_cast<std::size_t>(offset % memory_chunk_bytes);
				return chunks[chunk]->bytes[within];
			}

			void copy_bytes(const std::uint64_t offset,
							const std::span<std::byte> destination) const noexcept
			{
				std::uint64_t current = offset;
				std::size_t copied{};
				while (copied < destination.size())
				{
					const auto chunk = static_cast<std::size_t>(current / memory_chunk_bytes);
					const auto within = static_cast<std::size_t>(current % memory_chunk_bytes);
					const auto available =
						std::min(destination.size() - copied, memory_chunk_bytes - within);
					std::copy_n(chunks[chunk]->bytes.data() + within,
								available,
								destination.data() + copied);
					current += available;
					copied += available;
				}
			}

			[[nodiscard]] result<void> append(const std::span<const std::byte> bytes)
			{
				if (bytes.size() > bounded_store_v6_max_aggregate_bytes - size)
					return unexpected(resource("aggregate-bytes", "limit-exceeded"));
				std::size_t consumed{};
				while (consumed < bytes.size())
				{
					if (chunks.empty() || size % memory_chunk_bytes == 0U)
						chunks.push_back(std::make_unique<memory_chunk>());
					const auto within = static_cast<std::size_t>(size % memory_chunk_bytes);
					const auto count =
						std::min(bytes.size() - consumed, memory_chunk_bytes - within);
					std::copy_n(
						bytes.data() + consumed, count, chunks.back()->bytes.data() + within);
					all_hash.update(bytes.subspan(consumed, count));
					consumed += count;
					size += count;
				}
				return {};
			}
		};

		[[nodiscard]] result<bounded_store_v6_snapshot_observation>
		observe_semantic_snapshot(const memory_backing& backing,
								  const snapshot_series_selector& selector)
		{
			const auto semantic_digest = hex_digest(backing.semantic_hash.finish());
			const auto export_digest = hex_digest(backing.export_hash.finish());
			const std::array fields{
				canonical_value::from_string(selector.id()),
				canonical_value::from_string(semantic_digest),
				canonical_value::from_string(export_digest),
				canonical_value::from_string(std::to_string(backing.partition_count)),
				canonical_value::from_string(std::to_string(backing.claim_count)),
				canonical_value::from_string(std::to_string(backing.row_count)),
				canonical_value::from_string(std::to_string(backing.coverage_count)),
				canonical_value::from_string(std::to_string(backing.unresolved_count)),
			};
			auto snapshot_id =
				canonical_identity_digest("cxxlens.df-0200.memory-reference-snapshot.v1", fields);
			if (!snapshot_id)
				return unexpected(invariant("snapshot", "semantic-identity"));
			return bounded_store_v6_snapshot_observation{std::move(*snapshot_id),
														 backing.partition_count,
														 backing.row_count,
														 backing.claim_count,
														 backing.coverage_count,
														 backing.unresolved_count,
														 semantic_digest,
														 export_digest};
		}

		[[nodiscard]] result<bounded_store_v6_record_extent>
		read_extent(const memory_backing& backing, std::uint64_t offset);

		struct fresh_backing_observation
		{
			bounded_store_v6_snapshot_observation snapshot;
			std::string physical_digest;
			std::uint64_t record_count{};
			std::uint64_t framed_bytes{};
		};

		[[nodiscard]] result<fresh_backing_observation>
		scan_fresh_memory_backing(const memory_backing& backing,
								  const snapshot_series_selector& selector)
		{
			memory_backing summary;
			std::uint64_t offset{};
			std::vector<std::byte> previous_order;
			bool began{};
			bool ended{};
			while (offset < backing.size)
			{
				auto extent = read_extent(backing, offset);
				if (!extent)
					return unexpected(std::move(extent.error()));
				if ((!began || ended) &&
					extent->kind != bounded_store_v6_record_kind::partition_begin)
					return unexpected(corrupt("fresh-reopen", "partition-begin"));
				if (extent->kind == bounded_store_v6_record_kind::partition_begin && began &&
					!ended)
					return unexpected(corrupt("fresh-reopen", "nested-partition"));
				if (ended && extent->kind == bounded_store_v6_record_kind::partition_begin)
					previous_order.clear();
				std::vector<std::byte> frame(static_cast<std::size_t>(extent->framed_bytes));
				backing.copy_bytes(offset, frame);
				const auto projection =
					std::span<const std::byte>{frame.data(), frame.size() - frame_checksum_bytes};
				std::array<std::byte, frame_checksum_bytes> supplied{};
				std::copy_n(frame.data() + projection.size(), supplied.size(), supplied.data());
				if (!equal_bytes(raw_digest(projection), supplied))
					return unexpected(corrupt("fresh-reopen", "checksum"));
				const auto key = projection.subspan(frame_prefix_bytes,
													static_cast<std::size_t>(extent->key_bytes));
				const auto payload = projection.subspan(
					frame_prefix_bytes + static_cast<std::size_t>(extent->key_bytes),
					static_cast<std::size_t>(extent->payload_bytes));
				auto decoded_key = canonical_binary_decode(key);
				auto decoded_payload = canonical_binary_decode(payload);
				if (!decoded_key || !decoded_payload ||
					decoded_key->type != canonical_value::kind::ordered_tuple ||
					decoded_payload->type != canonical_value::kind::ordered_tuple)
					return unexpected(corrupt("fresh-reopen", "semantic-tuple"));
				auto current_order = order_key(extent->kind, key, payload);
				if (current_order.empty() ||
					(!previous_order.empty() &&
					 !std::lexicographical_compare(previous_order.begin(),
												   previous_order.end(),
												   current_order.begin(),
												   current_order.end())))
					return unexpected(corrupt("fresh-reopen", "physical-order"));
				previous_order = std::move(current_order);
				summary.all_hash.update(frame);
				update_record_identity_hash(summary.semantic_hash,
											"cxxlens/df-0200-semantic-projection/v1",
											extent->kind,
											key,
											payload);
				update_record_identity_hash(summary.export_hash,
											"cxxlens/df-0200-canonical-export/v1",
											extent->kind,
											key,
											payload);
				if (!checked_increment(summary.record_count))
					return unexpected(resource("fresh-reopen", "record-overflow"));
				switch (extent->kind)
				{
					case bounded_store_v6_record_kind::partition_begin:
						if (!checked_increment(summary.partition_count))
							return unexpected(resource("fresh-reopen", "partition-overflow"));
						break;
					case bounded_store_v6_record_kind::claim_occurrence:
						if (!checked_increment(summary.claim_count))
							return unexpected(resource("fresh-reopen", "claim-overflow"));
						break;
					case bounded_store_v6_record_kind::detached_row:
						if (!checked_increment(summary.row_count))
							return unexpected(resource("fresh-reopen", "row-overflow"));
						break;
					case bounded_store_v6_record_kind::coverage:
						if (!checked_increment(summary.coverage_count))
							return unexpected(resource("fresh-reopen", "coverage-overflow"));
						break;
					case bounded_store_v6_record_kind::unresolved:
						if (!checked_increment(summary.unresolved_count))
							return unexpected(resource("fresh-reopen", "unresolved-overflow"));
						break;
					case bounded_store_v6_record_kind::claim_annotation:
					case bounded_store_v6_record_kind::partition_end:
						break;
				}
				began = true;
				ended = extent->kind == bounded_store_v6_record_kind::partition_end;
				if (!checked_add(offset, extent->framed_bytes, offset))
					return unexpected(resource("fresh-reopen", "offset-overflow"));
			}
			if (!began || !ended || offset != backing.size)
				return unexpected(corrupt("fresh-reopen", "eof"));
			auto snapshot = observe_semantic_snapshot(summary, selector);
			if (!snapshot)
				return unexpected(std::move(snapshot.error()));
			return fresh_backing_observation{std::move(*snapshot),
											 hex_digest(summary.all_hash.finish()),
											 summary.record_count,
											 offset};
		}

		[[nodiscard]] std::uint64_t frame_order_key_size(const std::uint64_t key_bytes,
														 const std::uint64_t payload_bytes)
		{
			return 61U + key_bytes + payload_bytes;
		}

		[[nodiscard]] std::byte order_key_byte(const memory_backing& backing,
											   const std::uint64_t frame_offset,
											   const bounded_store_v6_record_extent extent,
											   const std::uint64_t offset) noexcept
		{
			const auto key = extent.key_bytes;
			const auto payload = extent.payload_bytes;
			const auto key_start = frame_offset + frame_prefix_bytes;
			const auto payload_start = key_start + key;
			if (offset == 0U)
				return std::byte{0x05U};
			if (offset <= 8U)
				return static_cast<std::byte>(std::uint64_t{3U} >> ((8U - offset) * 8U));
			if (offset <= 16U)
				return static_cast<std::byte>(std::uint64_t{10U} >> ((16U - offset) * 8U));
			if (offset == 17U)
				return std::byte{0x03U};
			if (offset <= 25U)
				return static_cast<std::byte>(std::uint64_t{1U} >> ((25U - offset) * 8U));
			if (offset == 26U)
				return static_cast<std::byte>(static_cast<std::uint8_t>(extent.kind));
			const auto key_data_start = 44U;
			if (offset <= 34U)
				return static_cast<std::byte>((17U + key) >> ((34U - offset) * 8U));
			if (offset == 35U)
				return std::byte{0x03U};
			if (offset <= 43U)
				return static_cast<std::byte>(key >> ((43U - offset) * 8U));
			if (offset < key_data_start + key)
				return backing.byte_at(key_start + (offset - key_data_start));
			const auto payload_header_start = key_data_start + key;
			const auto payload_data_start = payload_header_start + 17U;
			if (offset < payload_data_start)
			{
				if (offset <= payload_header_start + 7U)
					return static_cast<std::byte>((17U + payload) >>
												  ((payload_header_start + 7U - offset) * 8U));
				if (offset == payload_header_start + 8U)
					return std::byte{0x03U};
				return static_cast<std::byte>(
					payload >> (static_cast<unsigned>(payload_header_start + 16U - offset) * 8U));
			}
			return backing.byte_at(payload_start + (offset - payload_data_start));
		}

		[[nodiscard]] bool order_less(const memory_backing& backing,
									  const std::uint64_t left_offset,
									  const bounded_store_v6_record_extent left,
									  const std::uint64_t right_offset,
									  const bounded_store_v6_record_extent right)
		{
			const auto left_size = frame_order_key_size(left.key_bytes, left.payload_bytes);
			const auto right_size = frame_order_key_size(right.key_bytes, right.payload_bytes);
			const auto common = std::min(left_size, right_size);
			for (std::uint64_t index{}; index < common; ++index)
			{
				const auto l = order_key_byte(backing, left_offset, left, index);
				const auto r = order_key_byte(backing, right_offset, right, index);
				if (l != r)
					return std::to_integer<unsigned char>(l) < std::to_integer<unsigned char>(r);
			}
			return left_size < right_size;
		}

		[[nodiscard]] result<bounded_store_v6_record_extent>
		read_extent(const memory_backing& backing, const std::uint64_t offset)
		{
			if (offset > backing.size || backing.size - offset < frame_prefix_bytes)
				return unexpected(corrupt("projection", "truncated-header"));
			std::array<std::byte, frame_prefix_bytes> prefix{};
			backing.copy_bytes(offset, prefix);
			const auto raw_kind = std::to_integer<unsigned char>(prefix[0]);
			if (raw_kind < 1U || raw_kind > 7U)
				return unexpected(corrupt("projection", "unknown-kind"));
			const auto key_bytes = read_u64(prefix, 1U);
			const auto payload_bytes = read_u64(prefix, 9U);
			auto framed = checked_bounded_store_v6_record_frame_bytes(key_bytes, payload_bytes);
			if (!framed)
				return unexpected(std::move(framed.error()));
			if (*framed > backing.size - offset)
				return unexpected(corrupt("projection", "truncated-frame"));
			return bounded_store_v6_record_extent{
				static_cast<bounded_store_v6_record_kind>(raw_kind),
				key_bytes,
				payload_bytes,
				*framed};
		}

		struct stored_publication_metadata
		{
			bounded_store_v6_publication_observation publication;
			bounded_store_v6_snapshot_observation snapshot;
		};

		struct sealed_journal_entry
		{
			std::vector<std::byte> sealed_metadata;
			std::shared_ptr<const memory_backing> backing;
		};

		struct decoded_journal_record
		{
			stored_publication_metadata metadata;
			std::string physical_digest;
		};

		[[nodiscard]] result<std::vector<std::byte>>
		encode_journal_record(const stored_publication_metadata& value,
							  const std::string_view physical_digest)
		{
			const auto& publication = value.publication;
			const auto& snapshot = value.snapshot;
			return canonical_binary(canonical_value::from_tuple({
				canonical_value::from_string("cxxlens.df-0200.memory-journal.v1"),
				canonical_value::from_string(publication.publication_id),
				canonical_value::from_string(publication.series_id),
				canonical_value::from_string(publication.snapshot_id),
				canonical_value::from_string(std::to_string(publication.sequence)),
				canonical_value::from_string(std::to_string(publication.physical_generation)),
				canonical_value::from_string(
					publication.parent_publication.value_or(std::string{})),
				canonical_value::from_string(
					std::to_string(static_cast<std::uint8_t>(publication.state))),
				canonical_value::from_boolean(publication.corrupt),
				canonical_value::from_string(snapshot.snapshot_id),
				canonical_value::from_string(std::to_string(snapshot.partition_count)),
				canonical_value::from_string(std::to_string(snapshot.row_count)),
				canonical_value::from_string(std::to_string(snapshot.claim_count)),
				canonical_value::from_string(std::to_string(snapshot.coverage_count)),
				canonical_value::from_string(std::to_string(snapshot.unresolved_count)),
				canonical_value::from_string(snapshot.semantic_projection_digest),
				canonical_value::from_string(snapshot.canonical_export_digest),
				canonical_value::from_string(std::string{physical_digest}),
			}));
		}

		[[nodiscard]] result<decoded_journal_record>
		decode_journal_record(const std::span<const std::byte> bytes)
		{
			auto decoded = canonical_binary_decode(bytes);
			if (!decoded || decoded->type != canonical_value::kind::ordered_tuple ||
				decoded->tuple.size() != 18U)
				return unexpected(corrupt("memory-journal", "shape"));
			const auto& fields = decoded->tuple;
			for (std::size_t index{}; index < fields.size(); ++index)
				if (index != 8U && fields[index].type != canonical_value::kind::utf8_string)
					return unexpected(corrupt("memory-journal", "field-type"));
			if (fields[8U].type != canonical_value::kind::boolean ||
				fields[0U].text != "cxxlens.df-0200.memory-journal.v1")
				return unexpected(corrupt("memory-journal", "schema"));
			const auto counter = [&](const std::size_t index) -> result<std::uint64_t>
			{
				std::uint64_t value{};
				const auto text = std::string_view{fields[index].text};
				const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
				if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
					std::to_string(value) != text)
					return unexpected(corrupt("memory-journal", "counter"));
				return value;
			};
			auto sequence = counter(4U);
			auto generation = counter(5U);
			auto state = counter(7U);
			auto partitions = counter(10U);
			auto rows = counter(11U);
			auto claims = counter(12U);
			auto coverage = counter(13U);
			auto unresolved = counter(14U);
			if (!sequence || !generation || !state || !partitions || !rows || !claims ||
				!coverage || !unresolved ||
				*state > static_cast<std::uint64_t>(publication_state::rolled_back))
				return unexpected(corrupt("memory-journal", "counter"));
			bounded_store_v6_publication_observation publication{
				fields[1U].text,
				fields[2U].text,
				fields[3U].text,
				*sequence,
				*generation,
				fields[6U].text.empty() ? std::optional<std::string>{}
										: std::optional<std::string>{fields[6U].text},
				static_cast<publication_state>(*state),
				fields[8U].boolean};
			bounded_store_v6_snapshot_observation snapshot{fields[9U].text,
														   *partitions,
														   *rows,
														   *claims,
														   *coverage,
														   *unresolved,
														   fields[15U].text,
														   fields[16U].text};
			return decoded_journal_record{
				stored_publication_metadata{std::move(publication), std::move(snapshot)},
				fields[17U].text};
		}

		[[nodiscard]] bool
		same_publication(const bounded_store_v6_expected_head& expected,
						 const std::optional<stored_publication_metadata>& current) noexcept
		{
			if (expected.value == bounded_store_v6_expected_head::kind::genesis)
				return !current.has_value();
			if (!current || !expected.publication || !expected.snapshot)
				return false;
			const auto& observed = current->publication;
			const auto& authority = *expected.publication;
			return observed.publication_id == authority.publication_id &&
				observed.series_id == authority.series_id &&
				observed.snapshot_id == authority.snapshot_id &&
				observed.sequence == authority.sequence &&
				observed.parent_publication == authority.parent_publication &&
				observed.state == authority.state && observed.corrupt == authority.corrupt &&
				observed.physical_generation >= authority.physical_generation &&
				current->snapshot == *expected.snapshot;
		}
	} // namespace

	struct bounded_store_v6_memory_store::state
	{
		state(snapshot_series_selector value, std::shared_ptr<store_operation_port> operation_value)
			: selector{std::move(value)}, operations{std::move(operation_value)}
		{
		}
		snapshot_series_selector selector;
		std::shared_ptr<store_operation_port> operations;
		mutable std::mutex mutex;
		std::string current_publication_id;
		std::map<std::string, sealed_journal_entry, std::less<>> journal;
		std::atomic<std::uint64_t> live_staging_payloads{};
	};

	struct bounded_store_v6_memory_backend_port::state
	{
		std::shared_ptr<bounded_store_v6_memory_store::state> store;
		bounded_store_v6_session_metadata metadata;
		std::shared_ptr<const bounded_store_v6_physical_anchor> anchor;
		std::string anchor_binding;
		std::shared_ptr<memory_backing> backing{std::make_shared<memory_backing>()};
		bounded_store_v6_record_extent current_extent{};
		std::uint64_t current_start{};
		std::uint64_t current_received{};
		sha256 current_hash;
		std::array<std::byte, 17U> current_prefix{};
		std::array<std::byte, 32U> current_checksum{};
		bool current_open{};
		std::uint64_t previous_start{};
		bounded_store_v6_record_extent previous_extent{};
		bool has_previous{};
		bool sealed{};
		bool published{};
		bool publish_called{};
		bool reopen_called{};
		bool cleanup_called{};
		std::optional<bounded_store_v6_publication_observation> publication;
		std::optional<bounded_store_v6_snapshot_observation> snapshot;
	};

	class memory_actual_cursor final : public bounded_store_v6_actual_cursor_source
	{
	  public:
		memory_actual_cursor(std::shared_ptr<const memory_backing> backing,
							 std::shared_ptr<const bounded_store_v6_physical_anchor> anchor,
							 std::shared_ptr<store_operation_port> operations,
							 std::string binding,
							 std::string staging_session_id,
							 std::string series_id,
							 std::string candidate_snapshot_id)
			: backing_{std::move(backing)}, anchor_{std::move(anchor)},
			  operations_{std::move(operations)}, binding_{std::move(binding)},
			  staging_session_id_{std::move(staging_session_id)}, series_id_{std::move(series_id)},
			  candidate_snapshot_id_{std::move(candidate_snapshot_id)}
		{
		}

		result<std::optional<bounded_store_v6_record_extent>> next_record() override
		{
			if (eof_)
				return unexpected(invariant("cursor", "replay"));
			if (current_open_)
			{
				if (current_read_ != current_extent_.framed_bytes)
					return unexpected(corrupt("projection", "record-eof"));
				if (!equal_bytes(current_hash_.finish(), current_checksum_))
					return unexpected(corrupt("projection", "checksum-mismatch"));
				began_ =
					began_ || current_extent_.kind == bounded_store_v6_record_kind::partition_begin;
				ended_ = current_extent_.kind == bounded_store_v6_record_kind::partition_end;
				current_open_ = false;
				if (!checked_add(offset_, current_extent_.framed_bytes, offset_) ||
					!checked_increment(record_count_) ||
					!checked_add(framed_bytes_, current_extent_.framed_bytes, framed_bytes_))
					return unexpected(resource("cursor", "checked-overflow"));
				previous_start_ = current_start_;
				previous_extent_ = current_extent_;
				has_previous_ = true;
			}
			if (offset_ == backing_->size)
			{
				eof_ = true;
				if (!ended_)
					return unexpected(corrupt("projection", "missing-partition-end"));
				return std::optional<bounded_store_v6_record_extent>{};
			}
			auto extent = read_extent(*backing_, offset_);
			if (!extent)
				return unexpected(std::move(extent.error()));
			if ((!began_ || ended_) &&
				extent->kind != bounded_store_v6_record_kind::partition_begin)
				return unexpected(corrupt("projection", "missing-partition-begin"));
			if (extent->kind == bounded_store_v6_record_kind::partition_begin && began_ && !ended_)
				return unexpected(corrupt("projection", "duplicate-partition-begin"));
			if (ended_ && extent->kind == bounded_store_v6_record_kind::partition_begin)
				has_previous_ = false;
			if (has_previous_ &&
				!order_less(*backing_, previous_start_, previous_extent_, offset_, *extent))
				return unexpected(corrupt("projection", "reordered-or-duplicate"));
			current_extent_ = *extent;
			current_start_ = offset_;
			current_read_ = 0U;
			current_open_ = true;
			current_hash_ = sha256{};
			std::array<std::byte, 8U> length{};
			append_u64(length, static_cast<std::uint64_t>(frame_domain.size()));
			current_hash_.update(length);
			current_hash_.update(std::span<const std::byte>{
				reinterpret_cast<const std::byte*>(frame_domain.data()), frame_domain.size()});
			append_u64(length, current_extent_.framed_bytes - frame_checksum_bytes);
			current_hash_.update(length);
			current_checksum_.fill(std::byte{});
			return std::optional<bounded_store_v6_record_extent>{current_extent_};
		}

		result<std::size_t> read_record_bytes(const std::span<std::byte> destination) override
		{
			if (!current_open_)
				return unexpected(invariant("cursor", "record-not-open"));
			if (destination.empty())
				return std::size_t{};
			const auto remaining = current_extent_.framed_bytes - current_read_;
			const auto count = std::min<std::uint64_t>(remaining, destination.size());
			backing_->copy_bytes(current_start_ + current_read_,
								 destination.first(static_cast<std::size_t>(count)));
			const auto projection = current_extent_.framed_bytes - frame_checksum_bytes;
			update_raw_digest_prefix(current_hash_,
									 projection,
									 current_read_,
									 destination.first(static_cast<std::size_t>(count)));
			for (std::size_t index{}; index < count; ++index)
			{
				const auto frame_offset = current_read_ + index;
				if (frame_offset >= projection)
					current_checksum_[static_cast<std::size_t>(frame_offset - projection)] =
						destination[index];
			}
			all_hash_.update(destination.first(static_cast<std::size_t>(count)));
			current_read_ += count;
			return static_cast<std::size_t>(count);
		}

		std::shared_ptr<const bounded_store_v6_physical_anchor>
		physical_anchor() const noexcept override
		{
			return anchor_;
		}

		result<bounded_store_v6_physical_cursor_observation> finish() override
		{
			if (!eof_ || current_open_ || !backing_ || !anchor_ || finished_)
				return unexpected(invariant("cursor", "not-exhausted"));
			finished_ = true;
			if (auto observed =
					observe_memory_operation(operations_,
											 {store_backend_kind::memory,
											  store_backend_operation::finish_physical_cursor,
											  store_backend_observation_point::after_operation,
											  binding_,
											  staging_session_id_,
											  series_id_,
											  {},
											  candidate_snapshot_id_,
											  {},
											  0U,
											  record_count_,
											  framed_bytes_,
											  0U,
											  0U,
											  true,
											  false});
				!observed)
				return unexpected(std::move(observed.error()));
			return bounded_store_v6_physical_cursor_observation{record_count_,
																framed_bytes_,
																all_hash_.finish(),
																binding_,
																canonical_order_validated_};
		}

	  private:
		std::shared_ptr<const memory_backing> backing_;
		std::shared_ptr<const bounded_store_v6_physical_anchor> anchor_;
		std::shared_ptr<store_operation_port> operations_;
		std::string binding_;
		std::string staging_session_id_;
		std::string series_id_;
		std::string candidate_snapshot_id_;
		std::uint64_t offset_{};
		std::uint64_t current_start_{};
		std::uint64_t current_read_{};
		std::uint64_t record_count_{};
		std::uint64_t framed_bytes_{};
		std::uint64_t previous_start_{};
		bounded_store_v6_record_extent current_extent_{};
		bounded_store_v6_record_extent previous_extent_{};
		std::array<std::byte, 32U> current_checksum_{};
		sha256 current_hash_;
		sha256 all_hash_;
		bool current_open_{};
		bool eof_{};
		bool began_{};
		bool ended_{};
		bool has_previous_{};
		bool canonical_order_validated_{true};
		bool finished_{};
	};

	class memory_task_source final : public bounded_store_v6_task_frame_source
	{
	  public:
		explicit memory_task_source(std::vector<bounded_store_v6_memory_source_frame> frames)
			: frames_{std::move(frames)}
		{
		}

		result<std::optional<bounded_store_v6_record_extent>> next_record() override
		{
			if (eof_)
				return unexpected(invariant("source", "replay"));
			if (current_open_)
			{
				if (current_read_ != current_.framed_bytes)
					return unexpected(corrupt("source", "record-eof"));
				current_open_ = false;
				++index_;
			}
			if (index_ == frames_.size())
			{
				eof_ = true;
				return std::optional<bounded_store_v6_record_extent>{};
			}
			const auto& frame = frames_[index_];
			if (frame.bytes.size() < 49U)
				return unexpected(corrupt("source", "truncated-frame"));
			std::array<std::byte, 17U> prefix{};
			std::copy_n(frame.bytes.data(), prefix.size(), prefix.data());
			const auto key_bytes = read_u64(prefix, 1U);
			const auto payload_bytes = read_u64(prefix, 9U);
			auto size = checked_bounded_store_v6_record_frame_bytes(key_bytes, payload_bytes);
			if (!size || *size != frame.bytes.size() ||
				static_cast<std::uint8_t>(frame.kind) != std::to_integer<unsigned char>(prefix[0]))
				return unexpected(corrupt("source", "frame-shape"));
			std::array<std::byte, 32U> supplied_checksum{};
			std::copy_n(frame.bytes.data() + frame.bytes.size() - 32U,
						supplied_checksum.size(),
						supplied_checksum.data());
			if (!equal_bytes(raw_digest(std::span<const std::byte>{frame.bytes.data(),
																   frame.bytes.size() - 32U}),
							 supplied_checksum))
				return unexpected(corrupt("source", "checksum"));
			current_ = {frame.kind, key_bytes, payload_bytes, *size};
			current_read_ = 0U;
			current_open_ = true;
			return std::optional<bounded_store_v6_record_extent>{current_};
		}

		result<std::size_t> read_record_bytes(const std::span<std::byte> destination) override
		{
			if (!current_open_)
				return unexpected(invariant("source", "record-not-open"));
			if (destination.empty())
				return std::size_t{};
			const auto& bytes = frames_[index_].bytes;
			const auto count =
				std::min<std::uint64_t>(current_.framed_bytes - current_read_, destination.size());
			std::copy_n(
				bytes.data() + current_read_, static_cast<std::size_t>(count), destination.data());
			current_read_ += count;
			return static_cast<std::size_t>(count);
		}

		result<bool> canonical_order_validated() const override
		{
			return canonical_order_validated_;
		}

	  private:
		std::vector<bounded_store_v6_memory_source_frame> frames_;
		std::size_t index_{};
		std::uint64_t current_read_{};
		bounded_store_v6_record_extent current_{};
		bool current_open_{};
		bool eof_{};
		bool canonical_order_validated_{true};
	};

	class memory_expected_semantic_source final : public bounded_store_v6_expected_semantic_cursor
	{
	  public:
		explicit memory_expected_semantic_source(
			std::vector<bounded_store_v6_semantic_record> records)
			: records_{std::move(records)}
		{
		}

		result<std::optional<bounded_store_v6_semantic_record>> next_semantic_record() override
		{
			if (eof_)
				return unexpected(invariant("expected", "replay"));
			if (index_ == records_.size())
			{
				eof_ = true;
				return std::optional<bounded_store_v6_semantic_record>{};
			}
			return std::optional<bounded_store_v6_semantic_record>{std::move(records_[index_++])};
		}

		result<bool> authority_complete() const override
		{
			return eof_ && index_ == records_.size();
		}

	  private:
		std::vector<bounded_store_v6_semantic_record> records_;
		std::size_t index_{};
		bool eof_{};
	};

	bounded_store_v6_memory_store::bounded_store_v6_memory_store(
		snapshot_series_selector selector, std::shared_ptr<store_operation_port> operations)
		: state_{std::make_shared<state>(std::move(selector), std::move(operations))}
	{
	}
	bounded_store_v6_memory_store::~bounded_store_v6_memory_store() = default;
	bounded_store_v6_memory_store::bounded_store_v6_memory_store(
		bounded_store_v6_memory_store&&) noexcept = default;
	bounded_store_v6_memory_store&
	bounded_store_v6_memory_store::operator=(bounded_store_v6_memory_store&&) noexcept = default;

	result<std::unique_ptr<bounded_store_v6_backend_port>>
	bounded_store_v6_memory_store::make_backend_port(
		const bounded_store_v6_session_metadata& metadata)
	{
		if (!state_ || !state_->operations || metadata.backend != bounded_store_v6_backend::memory)
			return unexpected(invariant("backend", "memory-required"));
		if (auto valid = state_->selector.validate(); !valid)
			return unexpected(invariant("backend", "invalid-selector"));
		if (metadata.selector != state_->selector ||
			metadata.expected_head.selector != state_->selector)
			return unexpected(invariant("backend", "selector-mismatch"));
		if (metadata.staging_session_id.empty())
			return unexpected(invariant("backend", "missing-staging-session"));
		if (metadata.expected_head.value != bounded_store_v6_expected_head::kind::genesis ||
			metadata.expected_head.publication || metadata.expected_head.snapshot)
			return unexpected(invariant("backend", "memory-genesis-required"));
		try
		{
			std::lock_guard lock{state_->mutex};
			if (!state_->current_publication_id.empty() || !state_->journal.empty())
				return unexpected(invariant("backend", "memory-genesis-consumed"));
			auto port = std::unique_ptr<bounded_store_v6_backend_port>{
				new bounded_store_v6_memory_backend_port{state_, metadata}};
			state_->live_staging_payloads.fetch_add(1U, std::memory_order_relaxed);
			return port;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(resource("allocation", "unavailable"));
		}
	}

	result<bounded_store_v6_expected_head> bounded_store_v6_memory_store::current_head() const
	{
		if (!state_)
			return unexpected(invariant("store", "empty"));
		std::lock_guard lock{state_->mutex};
		if (state_->current_publication_id.empty())
			return bounded_store_v6_expected_head{bounded_store_v6_expected_head::kind::genesis,
												  state_->selector,
												  std::nullopt,
												  std::nullopt};
		const auto found = state_->journal.find(state_->current_publication_id);
		if (found == state_->journal.end())
			return unexpected(corrupt("memory-journal", "missing-current"));
		auto decoded = decode_journal_record(found->second.sealed_metadata);
		if (!decoded)
			return unexpected(std::move(decoded.error()));
		return bounded_store_v6_expected_head{bounded_store_v6_expected_head::kind::publication,
											  state_->selector,
											  decoded->metadata.publication,
											  decoded->metadata.snapshot};
	}

	std::size_t bounded_store_v6_memory_store::retained_publication_count() const noexcept
	{
		if (!state_)
			return 0U;
		std::lock_guard lock{state_->mutex};
		return state_->journal.size();
	}

	std::size_t bounded_store_v6_memory_store::retained_complete_payload_count() const noexcept
	{
		if (!state_)
			return 0U;
		std::lock_guard lock{state_->mutex};
		if (state_->current_publication_id.empty())
			return 0U;
		const auto found = state_->journal.find(state_->current_publication_id);
		return found != state_->journal.end() && found->second.backing ? 1U : 0U;
	}

	std::uint64_t bounded_store_v6_memory_store::live_staging_payload_count() const noexcept
	{
		return state_ ? state_->live_staging_payloads.load(std::memory_order_relaxed) : 0U;
	}

	bounded_store_v6_memory_backend_port::bounded_store_v6_memory_backend_port(
		std::shared_ptr<bounded_store_v6_memory_store::state> store,
		bounded_store_v6_session_metadata metadata)
		: state_{std::make_unique<state>()}
	{
		state_->store = std::move(store);
		state_->metadata = std::move(metadata);
		state_->anchor_binding = "memory:" + state_->metadata.selector.id();
	}
	bounded_store_v6_memory_backend_port::~bounded_store_v6_memory_backend_port()
	{
		if (state_ && state_->backing)
		{
			state_->backing.reset();
			if (state_->store)
				state_->store->live_staging_payloads.fetch_sub(1U, std::memory_order_relaxed);
		}
	}

	bounded_store_v6_backend bounded_store_v6_memory_backend_port::backend() const noexcept
	{
		return bounded_store_v6_backend::memory;
	}

	result<void> bounded_store_v6_memory_backend_port::bind_physical_anchor(
		std::shared_ptr<const bounded_store_v6_physical_anchor> anchor)
	{
		if (!state_ || !anchor)
			return unexpected(invariant("anchor", "missing"));
		if (state_->anchor)
			return unexpected(invariant("anchor", "rebind"));
		state_->anchor = std::move(anchor);
		return {};
	}

	std::shared_ptr<const bounded_store_v6_physical_anchor>
	bounded_store_v6_memory_backend_port::physical_anchor() const noexcept
	{
		return state_ ? state_->anchor : nullptr;
	}
	std::string_view bounded_store_v6_memory_backend_port::physical_anchor_binding() const noexcept
	{
		return state_ ? std::string_view{state_->anchor_binding} : std::string_view{};
	}

	result<void>
	bounded_store_v6_memory_backend_port::begin_record(const bounded_store_v6_record_extent& extent)
	{
		if (!state_ || state_->sealed || state_->current_open || !state_->backing)
			return unexpected(invariant("stage", "not-appendable"));
		if (!is_valid(extent.kind))
			return unexpected(corrupt("projection", "unknown-kind"));
		auto expected =
			checked_bounded_store_v6_record_frame_bytes(extent.key_bytes, extent.payload_bytes);
		if (!expected || *expected != extent.framed_bytes)
			return unexpected(corrupt("projection", "frame-size"));
		if (extent.framed_bytes > bounded_store_v6_max_aggregate_bytes ||
			extent.framed_bytes > bounded_store_v6_max_aggregate_bytes - state_->backing->size)
			return unexpected(resource("record-bytes", "limit-exceeded"));
		if ((!state_->backing->began || state_->backing->ended) &&
			extent.kind != bounded_store_v6_record_kind::partition_begin)
			return unexpected(corrupt("projection", "missing-partition-begin"));
		if (extent.kind == bounded_store_v6_record_kind::partition_begin &&
			state_->backing->began && !state_->backing->ended)
			return unexpected(corrupt("projection", "duplicate-partition-begin"));
		if (state_->backing->ended && extent.kind == bounded_store_v6_record_kind::partition_begin)
			state_->has_previous = false;
		if (auto observed =
				observe_memory_operation(state_->store->operations,
										 {store_backend_kind::memory,
										  store_backend_operation::stage_record,
										  store_backend_observation_point::before_operation,
										  state_->anchor_binding,
										  state_->metadata.staging_session_id,
										  state_->metadata.selector.id(),
										  {},
										  {},
										  {},
										  state_->backing->record_count,
										  state_->backing->record_count,
										  extent.framed_bytes});
			!observed)
			return unexpected(std::move(observed.error()));
		state_->current_extent = extent;
		state_->current_start = state_->backing->size;
		state_->current_received = 0U;
		state_->current_open = true;
		state_->current_hash = sha256{};
		state_->current_prefix.fill(std::byte{});
		state_->current_checksum.fill(std::byte{});
		std::array<std::byte, 8U> length{};
		append_u64(length, static_cast<std::uint64_t>(frame_domain.size()));
		state_->current_hash.update(length);
		state_->current_hash.update(std::span<const std::byte>{
			reinterpret_cast<const std::byte*>(frame_domain.data()), frame_domain.size()});
		append_u64(length, extent.framed_bytes - frame_checksum_bytes);
		state_->current_hash.update(length);
		return {};
	}

	result<void> bounded_store_v6_memory_backend_port::append_record_bytes(
		const std::span<const std::byte> bytes)
	{
		if (!state_ || !state_->current_open || !state_->backing)
			return unexpected(invariant("stage", "record-not-open"));
		if (bytes.size() > state_->current_extent.framed_bytes - state_->current_received)
			return unexpected(corrupt("projection", "record-overrun"));
		if (auto appended = state_->backing->append(bytes); !appended)
			return appended;
		const auto projection = state_->current_extent.framed_bytes - frame_checksum_bytes;
		update_raw_digest_prefix(state_->current_hash, projection, state_->current_received, bytes);
		for (std::size_t index{}; index < bytes.size(); ++index)
		{
			const auto offset = state_->current_received + index;
			if (offset < state_->current_prefix.size())
				state_->current_prefix[static_cast<std::size_t>(offset)] = bytes[index];
			if (offset >= projection)
				state_->current_checksum[static_cast<std::size_t>(offset - projection)] =
					bytes[index];
		}
		state_->current_received += bytes.size();
		return {};
	}

	result<void> bounded_store_v6_memory_backend_port::finish_record()
	{
		if (!state_ || !state_->current_open || !state_->backing)
			return unexpected(invariant("stage", "record-not-open"));
		if (state_->current_received != state_->current_extent.framed_bytes)
			return unexpected(corrupt("projection", "record-eof"));
		if (static_cast<std::uint8_t>(state_->current_extent.kind) !=
				std::to_integer<unsigned char>(state_->current_prefix[0U]) ||
			read_u64(state_->current_prefix, 1U) != state_->current_extent.key_bytes ||
			read_u64(state_->current_prefix, 9U) != state_->current_extent.payload_bytes ||
			!equal_bytes(state_->current_hash.finish(), state_->current_checksum))
			return unexpected(corrupt("projection", "checksum-or-header"));
		if (state_->has_previous &&
			!order_less(*state_->backing,
						state_->previous_start,
						state_->previous_extent,
						state_->current_start,
						state_->current_extent))
			return unexpected(corrupt("projection", "reordered-or-duplicate"));
		std::vector<std::byte> key(static_cast<std::size_t>(state_->current_extent.key_bytes));
		std::vector<std::byte> payload(
			static_cast<std::size_t>(state_->current_extent.payload_bytes));
		state_->backing->copy_bytes(state_->current_start + frame_prefix_bytes,
									std::span<std::byte>{key});
		state_->backing->copy_bytes(state_->current_start + frame_prefix_bytes +
										state_->current_extent.key_bytes,
									std::span<std::byte>{payload});
		auto decoded_key = canonical_binary_decode(key);
		auto decoded_payload = canonical_binary_decode(payload);
		if (!decoded_key || !decoded_payload ||
			decoded_key->type != canonical_value::kind::ordered_tuple ||
			decoded_payload->type != canonical_value::kind::ordered_tuple)
			return unexpected(corrupt("projection", "semantic-tuple"));
		update_record_identity_hash(state_->backing->semantic_hash,
									"cxxlens/df-0200-semantic-projection/v1",
									state_->current_extent.kind,
									key,
									payload);
		update_record_identity_hash(state_->backing->export_hash,
									"cxxlens/df-0200-canonical-export/v1",
									state_->current_extent.kind,
									key,
									payload);
		state_->backing->began = true;
		state_->backing->ended =
			state_->current_extent.kind == bounded_store_v6_record_kind::partition_end;
		if (!checked_increment(state_->backing->record_count))
			return unexpected(resource("record-count", "checked-overflow"));
		switch (state_->current_extent.kind)
		{
			case bounded_store_v6_record_kind::partition_begin:
				if (!checked_increment(state_->backing->partition_count))
					return unexpected(resource("partition-count", "checked-overflow"));
				break;
			case bounded_store_v6_record_kind::claim_occurrence:
				if (!checked_increment(state_->backing->claim_count))
					return unexpected(resource("claim-count", "checked-overflow"));
				break;
			case bounded_store_v6_record_kind::detached_row:
				if (!checked_increment(state_->backing->row_count))
					return unexpected(resource("row-count", "checked-overflow"));
				break;
			case bounded_store_v6_record_kind::coverage:
				if (!checked_increment(state_->backing->coverage_count))
					return unexpected(resource("coverage-count", "checked-overflow"));
				break;
			case bounded_store_v6_record_kind::unresolved:
				if (!checked_increment(state_->backing->unresolved_count))
					return unexpected(resource("unresolved-count", "checked-overflow"));
				break;
			case bounded_store_v6_record_kind::claim_annotation:
			case bounded_store_v6_record_kind::partition_end:
				break;
		}
		state_->previous_start = state_->current_start;
		state_->previous_extent = state_->current_extent;
		state_->has_previous = true;
		state_->current_open = false;
		return {};
	}

	result<void> bounded_store_v6_memory_backend_port::seal_staging()
	{
		if (!state_ || state_->sealed || state_->current_open || !state_->backing ||
			!state_->backing->began || !state_->backing->ended)
			return unexpected(invariant("seal", "precondition"));
		if (auto observed =
				observe_memory_operation(state_->store->operations,
										 {store_backend_kind::memory,
										  store_backend_operation::seal_staging,
										  store_backend_observation_point::before_operation,
										  state_->anchor_binding,
										  state_->metadata.staging_session_id,
										  state_->metadata.selector.id(),
										  {},
										  {},
										  {},
										  0U,
										  state_->backing->record_count,
										  state_->backing->size});
			!observed)
			return unexpected(std::move(observed.error()));
		state_->sealed = true;
		return {};
	}

	result<bounded_store_v6_measured_projection>
	bounded_store_v6_memory_backend_port::measured_projection() const
	{
		if (!state_ || !state_->sealed || !state_->backing || !state_->anchor)
			return unexpected(invariant("measure", "not-sealed"));
		const auto physical_digest = state_->backing->all_hash.finish();
		auto snapshot = observe_semantic_snapshot(*state_->backing, state_->metadata.selector);
		if (!snapshot)
			return unexpected(std::move(snapshot.error()));
		return bounded_store_v6_measured_projection{state_->backing->record_count,
													state_->backing->size,
													physical_digest,
													std::move(*snapshot),
													state_->anchor_binding + ":" +
														hex_digest(physical_digest)};
	}

	result<std::unique_ptr<bounded_store_v6_actual_cursor_source>>
	bounded_store_v6_memory_backend_port::open_actual_cursor()
	{
		if (!state_ || !state_->sealed || !state_->backing || !state_->anchor)
			return unexpected(invariant("actual", "not-sealed"));
		auto snapshot = observe_semantic_snapshot(*state_->backing, state_->metadata.selector);
		if (!snapshot)
			return unexpected(std::move(snapshot.error()));
		const auto candidate_snapshot_id = snapshot->snapshot_id;
		if (auto observed =
				observe_memory_operation(state_->store->operations,
										 {store_backend_kind::memory,
										  store_backend_operation::open_physical_cursor,
										  store_backend_observation_point::before_operation,
										  state_->anchor_binding,
										  state_->metadata.staging_session_id,
										  state_->metadata.selector.id(),
										  {},
										  candidate_snapshot_id,
										  {},
										  0U,
										  state_->backing->record_count,
										  state_->backing->size});
			!observed)
			return unexpected(std::move(observed.error()));
		return std::unique_ptr<bounded_store_v6_actual_cursor_source>{
			std::make_unique<memory_actual_cursor>(
				std::shared_ptr<const memory_backing>{state_->backing},
				state_->anchor,
				state_->store->operations,
				state_->anchor_binding,
				state_->metadata.staging_session_id,
				state_->metadata.selector.id(),
				candidate_snapshot_id)};
	}

	result<bounded_store_v6_effect_result> bounded_store_v6_memory_backend_port::publish_once()
	{
		if (!state_ || !state_->sealed || state_->publish_called || !state_->store ||
			!state_->backing || !state_->anchor)
			return unexpected(invariant("publish", "not-ready-or-replay"));
		state_->publish_called = true;
		std::lock_guard lock{state_->store->mutex};
		std::optional<stored_publication_metadata> current;
		if (!state_->store->current_publication_id.empty())
		{
			const auto found = state_->store->journal.find(state_->store->current_publication_id);
			if (found == state_->store->journal.end())
				return unexpected(corrupt("memory-journal", "missing-current"));
			auto decoded = decode_journal_record(found->second.sealed_metadata);
			if (!decoded)
				return unexpected(std::move(decoded.error()));
			current = std::move(decoded->metadata);
		}
		if (!same_publication(state_->metadata.expected_head, current))
			return unexpected(invariant("head", "compare-and-swap"));
		const auto prior_sequence = current ? current->publication.sequence : 0U;
		const auto prior_generation = current ? current->publication.physical_generation : 0U;
		if (auto observed = observe_memory_operation(
				state_->store->operations,
				{store_backend_kind::memory,
				 store_backend_operation::allocate_publication_sequence,
				 store_backend_observation_point::before_operation,
				 state_->anchor_binding,
				 state_->metadata.staging_session_id,
				 state_->metadata.selector.id(),
				 {},
				 {},
				 current ? current->publication.publication_id : std::string_view{},
				 0U,
				 state_->backing->record_count,
				 state_->backing->size,
				 prior_sequence,
				 prior_generation});
			!observed)
			return unexpected(std::move(observed.error()));
		if (prior_sequence == std::numeric_limits<std::uint64_t>::max())
			return unexpected(failure("store.counter-overflow", "publication_sequence"));
		if (auto observed = observe_memory_operation(
				state_->store->operations,
				{store_backend_kind::memory,
				 store_backend_operation::allocate_physical_generation,
				 store_backend_observation_point::before_operation,
				 state_->anchor_binding,
				 state_->metadata.staging_session_id,
				 state_->metadata.selector.id(),
				 {},
				 {},
				 current ? current->publication.publication_id : std::string_view{},
				 0U,
				 state_->backing->record_count,
				 state_->backing->size,
				 prior_sequence,
				 prior_generation});
			!observed)
			return unexpected(std::move(observed.error()));
		if (prior_generation == std::numeric_limits<std::uint64_t>::max())
			return unexpected(failure("store.counter-overflow", "physical_generation"));
		const auto sequence = prior_sequence + 1U;
		const auto generation = prior_generation + 1U;
		auto semantic_snapshot =
			observe_semantic_snapshot(*state_->backing, state_->metadata.selector);
		if (!semantic_snapshot)
			return unexpected(std::move(semantic_snapshot.error()));
		bounded_store_v6_publication_observation publication;
		publication.snapshot_id = semantic_snapshot->snapshot_id;
		publication.series_id = state_->metadata.selector.id();
		publication.sequence = sequence;
		publication.physical_generation = generation;
		publication.parent_publication = current
			? std::optional<std::string>{current->publication.publication_id}
			: std::nullopt;
		auto publication_id = publication_record_identity(publication.series_id,
														  publication.snapshot_id,
														  publication.sequence,
														  publication.parent_publication);
		if (!publication_id)
			return unexpected(invariant("publication_id", "identity"));
		publication.publication_id = std::move(*publication_id);
		publication.state = publication_state::committed;
		publication.corrupt = false;
		if (state_->store->journal.contains(publication.publication_id))
			return unexpected(invariant("publication_id", "collision"));
		bounded_store_v6_snapshot_observation snapshot = std::move(*semantic_snapshot);
		stored_publication_metadata metadata{publication, snapshot};
		auto sealed_metadata =
			encode_journal_record(metadata, hex_digest(state_->backing->all_hash.finish()));
		if (!sealed_metadata)
			return unexpected(std::move(sealed_metadata.error()));
		std::string new_current = publication.publication_id;
		sealed_journal_entry entry{std::move(*sealed_metadata),
								   std::shared_ptr<const memory_backing>{state_->backing}};
		state_->publication = publication;
		state_->snapshot = snapshot;
		if (auto observed =
				observe_memory_operation(state_->store->operations,
										 {store_backend_kind::memory,
										  store_backend_operation::publish_once,
										  store_backend_observation_point::before_operation,
										  state_->anchor_binding,
										  state_->metadata.staging_session_id,
										  publication.series_id,
										  {},
										  publication.snapshot_id,
										  publication.publication_id,
										  0U,
										  state_->backing->record_count,
										  state_->backing->size,
										  publication.sequence,
										  publication.physical_generation});
			!observed)
			return unexpected(std::move(observed.error()));
		const auto [position, inserted] =
			state_->store->journal.emplace(publication.publication_id, std::move(entry));
		(void)position;
		if (!inserted)
			return unexpected(invariant("publication_id", "collision"));
		state_->store->current_publication_id.swap(new_current);
		state_->published = true;
		if (auto observed =
				observe_memory_operation(state_->store->operations,
										 {store_backend_kind::memory,
										  store_backend_operation::publish_once,
										  store_backend_observation_point::after_operation,
										  state_->anchor_binding,
										  state_->metadata.staging_session_id,
										  publication.series_id,
										  {},
										  publication.snapshot_id,
										  publication.publication_id,
										  0U,
										  state_->backing->record_count,
										  state_->backing->size,
										  publication.sequence,
										  publication.physical_generation,
										  true,
										  true});
			!observed)
			return unexpected(std::move(observed.error()));
		return bounded_store_v6_effect_result{std::nullopt, publication, true, true, true};
	}

	result<bounded_store_v6_reopen_observation> bounded_store_v6_memory_backend_port::reopen()
	{
		if (!state_ || !state_->published || !state_->store || !state_->publication ||
			!state_->snapshot || state_->reopen_called)
			return unexpected(invariant("reopen", "not-published"));
		state_->reopen_called = true;
		bounded_store_v6_reopen_observation output;
		output.factory_attempted = true;
		static std::atomic<std::uint64_t> next_fresh_factory{1U};
		output.fresh_backend_binding = "memory-fresh:" + state_->metadata.selector.id() + ":" +
			std::to_string(next_fresh_factory.fetch_add(1U, std::memory_order_relaxed));
		const auto event = [&](const store_backend_operation operation,
							   const store_backend_observation_point point)
		{
			return store_backend_operation_event{
				store_backend_kind::memory,
				operation,
				point,
				output.fresh_backend_binding,
				state_->metadata.staging_session_id,
				state_->publication->series_id,
				{},
				state_->publication->snapshot_id,
				state_->publication->publication_id,
				0U,
				state_->backing->record_count,
				state_->backing->size,
				state_->publication->sequence,
				state_->publication->physical_generation,
				point == store_backend_observation_point::after_operation,
				false};
		};
		if (auto observed =
				observe_memory_operation(state_->store->operations,
										 event(store_backend_operation::reopen_factory,
											   store_backend_observation_point::before_operation));
			!observed)
		{
			output.factory_error = std::move(observed.error());
			return output;
		}

		std::vector<std::byte> sealed_metadata;
		std::shared_ptr<const memory_backing> reopened_backing;
		{
			std::lock_guard lock{state_->store->mutex};
			if (state_->store->current_publication_id != state_->publication->publication_id)
			{
				output.factory_error = invariant("reopen", "current-mismatch");
				return output;
			}
			const auto found = state_->store->journal.find(state_->store->current_publication_id);
			if (found == state_->store->journal.end() || !found->second.backing)
			{
				output.factory_error = corrupt("memory-journal", "missing-current");
				return output;
			}
			sealed_metadata = found->second.sealed_metadata;
			reopened_backing = found->second.backing;
		}

		if (auto observed =
				observe_memory_operation(state_->store->operations,
										 event(store_backend_operation::open_physical_cursor,
											   store_backend_observation_point::before_operation));
			!observed)
		{
			output.factory_error = std::move(observed.error());
			return output;
		}
		auto physical = scan_fresh_memory_backing(*reopened_backing, state_->metadata.selector);
		if (!physical)
		{
			output.factory_error = std::move(physical.error());
			return output;
		}
		if (auto observed =
				observe_memory_operation(state_->store->operations,
										 event(store_backend_operation::finish_physical_cursor,
											   store_backend_observation_point::after_operation));
			!observed)
		{
			output.factory_error = std::move(observed.error());
			return output;
		}

		const auto perform_lookup = [&](const store_backend_operation operation,
										bounded_store_v6_lookup_observation& destination,
										const bool require_publication,
										const bool require_snapshot)
		{
			if (auto observed = observe_memory_operation(
					state_->store->operations,
					event(operation, store_backend_observation_point::before_operation));
				!observed)
			{
				destination.status = bounded_store_v6_lookup_observation::state::failed;
				destination.failure = std::move(observed.error());
				return;
			}
			auto decoded = decode_journal_record(sealed_metadata);
			if (!decoded || decoded->physical_digest != physical->physical_digest ||
				decoded->metadata.snapshot != physical->snapshot ||
				(require_publication &&
				 decoded->metadata.publication.publication_id !=
					 state_->publication->publication_id) ||
				(require_snapshot &&
				 decoded->metadata.snapshot.snapshot_id != state_->snapshot->snapshot_id))
			{
				destination.status = bounded_store_v6_lookup_observation::state::failed;
				destination.failure =
					decoded ? corrupt("fresh-reopen", "authority-mismatch") : decoded.error();
				return;
			}
			destination.status = bounded_store_v6_lookup_observation::state::present;
			destination.publication = decoded->metadata.publication;
			destination.snapshot = decoded->metadata.snapshot;
			if (auto observed = observe_memory_operation(
					state_->store->operations,
					event(operation, store_backend_observation_point::after_operation));
				!observed)
			{
				destination.status = bounded_store_v6_lookup_observation::state::failed;
				destination.failure = std::move(observed.error());
				destination.publication.reset();
				destination.snapshot.reset();
			}
		};
		perform_lookup(store_backend_operation::lookup_current, output.current, true, true);
		perform_lookup(
			store_backend_operation::lookup_publication, output.publication, true, false);
		perform_lookup(store_backend_operation::lookup_snapshot, output.snapshot, false, true);
		if (auto observed =
				observe_memory_operation(state_->store->operations,
										 event(store_backend_operation::lookup_expected_parent,
											   store_backend_observation_point::before_operation));
			!observed)
		{
			output.expected_parent.status = bounded_store_v6_lookup_observation::state::failed;
			output.expected_parent.failure = std::move(observed.error());
		}
		else
			output.expected_parent.status = bounded_store_v6_lookup_observation::state::not_found;

		if (auto observed =
				observe_memory_operation(state_->store->operations,
										 event(store_backend_operation::canonical_export,
											   store_backend_observation_point::before_operation));
			!observed)
			output.canonical_export_error = std::move(observed.error());
		else
			output.canonical_export_digest = physical->snapshot.canonical_export_digest;

		if (auto observed =
				observe_memory_operation(state_->store->operations,
										 event(store_backend_operation::reopen_factory,
											   store_backend_observation_point::after_operation));
			!observed)
			output.factory_error = std::move(observed.error());
		return output;
	}

	result<void> bounded_store_v6_memory_backend_port::abort_staging()
	{
		if (!state_ || state_->cleanup_called)
			return {};
		state_->cleanup_called = true;
		std::optional<error> cleanup_failure;
		try
		{
			if (auto observed = observe_memory_operation(
					state_->store->operations,
					{store_backend_kind::memory,
					 store_backend_operation::abort_staging,
					 store_backend_observation_point::before_operation,
					 state_->anchor_binding,
					 state_->metadata.staging_session_id,
					 state_->metadata.selector.id(),
					 {},
					 state_->publication ? state_->publication->snapshot_id : std::string_view{},
					 state_->publication ? state_->publication->publication_id : std::string_view{},
					 0U,
					 state_->backing ? state_->backing->record_count : 0U,
					 state_->backing ? state_->backing->size : 0U});
				!observed)
				cleanup_failure = std::move(observed.error());
		}
		catch (...)
		{
			cleanup_failure = invariant("operation-port", "abort-exception");
		}
		if (state_->backing)
		{
			state_->backing.reset();
			if (state_->store)
				state_->store->live_staging_payloads.fetch_sub(1U, std::memory_order_relaxed);
		}
		if (cleanup_failure)
			return unexpected(std::move(*cleanup_failure));
		return {};
	}

	result<std::vector<std::byte>>
	encode_bounded_store_v6_memory_frame(const bounded_store_v6_record_kind kind,
										 const std::span<const std::byte> key,
										 const std::span<const std::byte> payload)
	{
		if (!is_valid(kind))
			return unexpected(corrupt("frame", "unknown-kind"));
		auto size = checked_bounded_store_v6_record_frame_bytes(key.size(), payload.size());
		if (!size)
			return unexpected(std::move(size.error()));
		std::vector<std::byte> projection;
		projection.reserve(static_cast<std::size_t>(*size - frame_checksum_bytes));
		projection.push_back(static_cast<std::byte>(kind));
		append_u64(projection, key.size());
		append_u64(projection, payload.size());
		projection.insert(projection.end(), key.begin(), key.end());
		projection.insert(projection.end(), payload.begin(), payload.end());
		auto checksum = raw_digest(projection);
		projection.insert(projection.end(), checksum.begin(), checksum.end());
		return projection;
	}

	result<std::unique_ptr<bounded_store_v6_task_frame_source>>
	make_bounded_store_v6_memory_task_frame_source(
		std::vector<bounded_store_v6_memory_source_frame> frames)
	{
		try
		{
			std::vector<std::byte> previous_order;
			std::uint64_t aggregate_bytes{};
			bool began{};
			bool ended{};
			for (const auto& frame : frames)
			{
				if (frame.bytes.size() > bounded_store_v6_max_aggregate_bytes - aggregate_bytes)
					return unexpected(resource("aggregate-bytes", "limit-exceeded"));
				aggregate_bytes += frame.bytes.size();
				if (!is_valid(frame.kind) || frame.bytes.size() < 49U)
					return unexpected(corrupt("source", "frame-shape"));
				std::array<std::byte, 17U> prefix{};
				std::copy_n(frame.bytes.data(), prefix.size(), prefix.data());
				if (static_cast<std::uint8_t>(frame.kind) !=
					std::to_integer<unsigned char>(prefix[0]))
					return unexpected(corrupt("source", "kind"));
				const auto key_size = read_u64(prefix, 1U);
				const auto payload_size = read_u64(prefix, 9U);
				auto expected = checked_bounded_store_v6_record_frame_bytes(key_size, payload_size);
				if (!expected || *expected != frame.bytes.size())
					return unexpected(corrupt("source", "frame-size"));
				const auto projection =
					std::span<const std::byte>{frame.bytes.data(), frame.bytes.size() - 32U};
				std::array<std::byte, 32U> supplied{};
				std::copy_n(frame.bytes.data() + frame.bytes.size() - 32U,
							supplied.size(),
							supplied.data());
				if (!equal_bytes(raw_digest(projection), supplied))
					return unexpected(corrupt("source", "checksum"));
				const auto key = std::span<const std::byte>{frame.bytes.data() + 17U,
															static_cast<std::size_t>(key_size)};
				const auto payload = std::span<const std::byte>{
					frame.bytes.data() + 17U + key_size, static_cast<std::size_t>(payload_size)};
				auto decoded_key = canonical_binary_decode(key);
				auto decoded_payload = canonical_binary_decode(payload);
				if (!decoded_key || !decoded_payload ||
					decoded_key->type != canonical_value::kind::ordered_tuple ||
					decoded_payload->type != canonical_value::kind::ordered_tuple)
					return unexpected(corrupt("source", "canonical-tuple"));
				if (ended && frame.kind == bounded_store_v6_record_kind::partition_begin)
					previous_order.clear();
				const auto current_order = order_key(frame.kind, key, payload);
				if (current_order.empty() ||
					(!previous_order.empty() &&
					 !std::lexicographical_compare(previous_order.begin(),
												   previous_order.end(),
												   current_order.begin(),
												   current_order.end())))
					return unexpected(corrupt("source", "reordered-or-duplicate"));
				previous_order = current_order;
				if ((!began || ended) &&
					frame.kind != bounded_store_v6_record_kind::partition_begin)
					return unexpected(corrupt("source", "missing-partition-begin"));
				if (frame.kind == bounded_store_v6_record_kind::partition_begin && began && !ended)
					return unexpected(corrupt("source", "duplicate-partition-begin"));
				began = began || frame.kind == bounded_store_v6_record_kind::partition_begin;
				ended = frame.kind == bounded_store_v6_record_kind::partition_end;
			}
			if (!began || !ended)
				return unexpected(corrupt("source", "missing-partition-end"));
			return std::unique_ptr<bounded_store_v6_task_frame_source>{
				std::make_unique<memory_task_source>(std::move(frames))};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(resource("allocation", "unavailable"));
		}
	}

	result<std::unique_ptr<bounded_store_v6_expected_semantic_cursor>>
	make_bounded_store_v6_memory_expected_semantic_source(
		std::vector<bounded_store_v6_memory_source_frame> frames)
	{
		try
		{
			std::vector<bounded_store_v6_semantic_record> records;
			records.reserve(frames.size());
			std::uint64_t aggregate_bytes{};
			for (const auto& frame : frames)
			{
				if (!is_valid(frame.kind) ||
					frame.bytes.size() < bounded_store_v6_record_fixed_bytes ||
					frame.bytes.size() > bounded_store_v6_max_aggregate_bytes - aggregate_bytes)
					return unexpected(corrupt("expected", "frame-shape"));
				aggregate_bytes += frame.bytes.size();
				std::array<std::byte, frame_prefix_bytes> prefix{};
				std::copy_n(frame.bytes.data(), prefix.size(), prefix.data());
				const auto key_size = read_u64(prefix, 1U);
				const auto payload_size = read_u64(prefix, 9U);
				auto expected = checked_bounded_store_v6_record_frame_bytes(key_size, payload_size);
				if (!expected || *expected != frame.bytes.size() ||
					std::to_integer<unsigned char>(prefix[0]) != static_cast<unsigned>(frame.kind))
					return unexpected(corrupt("expected", "frame-header"));
				const auto projection = std::span<const std::byte>{
					frame.bytes.data(), frame.bytes.size() - frame_checksum_bytes};
				std::array<std::byte, frame_checksum_bytes> supplied{};
				std::copy_n(
					frame.bytes.data() + projection.size(), supplied.size(), supplied.data());
				if (!equal_bytes(raw_digest(projection), supplied))
					return unexpected(corrupt("expected", "checksum"));
				const auto key =
					projection.subspan(frame_prefix_bytes, static_cast<std::size_t>(key_size));
				const auto payload =
					projection.subspan(frame_prefix_bytes + static_cast<std::size_t>(key_size),
									   static_cast<std::size_t>(payload_size));
				auto decoded_key = canonical_binary_decode(key);
				auto decoded_payload = canonical_binary_decode(payload);
				if (!decoded_key || !decoded_payload ||
					decoded_key->type != canonical_value::kind::ordered_tuple ||
					decoded_payload->type != canonical_value::kind::ordered_tuple)
					return unexpected(corrupt("expected", "semantic-tuple"));
				records.push_back(bounded_store_v6_semantic_record{
					frame.kind, std::move(*decoded_key), std::move(*decoded_payload)});
			}
			if (records.empty())
				return unexpected(corrupt("expected", "empty"));
			return std::unique_ptr<bounded_store_v6_expected_semantic_cursor>{
				std::make_unique<memory_expected_semantic_source>(std::move(records))};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(resource("allocation", "unavailable"));
		}
	}
} // namespace cxxlens::sdk::detail
