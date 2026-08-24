#include "bounded_store_v6_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

#include "store_identity_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		constexpr std::string_view frame_domain{"cxxlens/df-0200-partition-event-frame/v1"};
		constexpr std::size_t frame_projection_prefix_bytes = 17U;
		constexpr std::size_t frame_checksum_bytes = 32U;
		constexpr std::size_t comparison_buffer_bytes =
			static_cast<std::size_t>(bounded_store_v6_comparator_cursor_bytes);

		[[nodiscard]] error failure(std::string code, std::string field, std::string detail)
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

		[[nodiscard]] error allocation_failure()
		{
			return resource("allocation", "unavailable");
		}

		[[nodiscard]] error exception_failure(const bounded_store_v6_backend backend,
											  const char* what)
		{
			if (backend == bounded_store_v6_backend::sqlite)
				return failure(
					"store.sqlite-failure", "database", what == nullptr ? "exception" : what);
			return invariant("publish", what == nullptr ? "exception" : what);
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

		void append_u64(std::array<std::byte, 8U>& output, const std::uint64_t value) noexcept
		{
			for (std::size_t index{}; index < output.size(); ++index)
				output[index] = static_cast<std::byte>(value >> (56U - index * 8U));
		}

		[[nodiscard]] std::uint64_t
		read_u64(const std::array<std::byte, frame_projection_prefix_bytes>& bytes,
				 const std::size_t offset) noexcept
		{
			std::uint64_t value{};
			for (std::size_t index{}; index < 8U; ++index)
				value = (value << 8U) |
					static_cast<std::uint64_t>(
							std::to_integer<unsigned char>(bytes[offset + index]));
			return value;
		}

		[[nodiscard]] bool bytes_equal(const std::array<std::byte, 32U>& left,
									   const std::array<std::byte, 32U>& right) noexcept
		{
			return std::equal(left.begin(), left.end(), right.begin(), right.end());
		}

		/** Small self-contained SHA-256 state; no full projection is retained. */
		class sha256 final
		{
		  public:
			sha256() noexcept
			{
				state_ = {0x6a09e667U,
						  0xbb67ae85U,
						  0x3c6ef372U,
						  0xa54ff53aU,
						  0x510e527fU,
						  0x9b05688cU,
						  0x1f83d9abU,
						  0x5be0cd19U};
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
					total_bytes_++;
				}
			}

			[[nodiscard]] std::array<std::byte, 32U> finish() const noexcept
			{
				sha256 copy{*this};
				const auto bit_count = copy.total_bytes_ * 8U;
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
					copy.buffer_[56U + index] =
						static_cast<std::byte>(bit_count >> (56U - index * 8U));
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
			static constexpr std::array<std::uint32_t, 64U> round_constants{
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

			static constexpr std::uint32_t rotate_right(const std::uint32_t value,
														const unsigned count) noexcept
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
					const auto s0 = rotate_right(schedule[index - 15U], 7U) ^
						rotate_right(schedule[index - 15U], 18U) ^ (schedule[index - 15U] >> 3U);
					const auto s1 = rotate_right(schedule[index - 2U], 17U) ^
						rotate_right(schedule[index - 2U], 19U) ^ (schedule[index - 2U] >> 10U);
					schedule[index] = schedule[index - 16U] + s0 + schedule[index - 7U] + s1;
				}
				std::uint32_t a = state_[0U];
				std::uint32_t b = state_[1U];
				std::uint32_t c = state_[2U];
				std::uint32_t d = state_[3U];
				std::uint32_t e = state_[4U];
				std::uint32_t f = state_[5U];
				std::uint32_t g = state_[6U];
				std::uint32_t h = state_[7U];
				for (std::size_t index{}; index < 64U; ++index)
				{
					const auto s1 =
						rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
					const auto choose = (e & f) ^ ((~e) & g);
					const auto temp1 = h + s1 + choose + round_constants[index] + schedule[index];
					const auto s0 =
						rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
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

		[[nodiscard]] std::string
		digest_binding(std::string_view left, std::string_view right, std::string_view third = {})
		{
			std::vector<std::byte> bytes;
			bytes.reserve(left.size() + right.size() + third.size() + 2U);
			for (const auto value : {left, right, third})
			{
				bytes.insert(bytes.end(),
							 reinterpret_cast<const std::byte*>(value.data()),
							 reinterpret_cast<const std::byte*>(value.data() + value.size()));
				bytes.push_back(std::byte{0});
			}
			return content_digest(bytes);
		}

		[[nodiscard]] bool
		same_anchor(const std::shared_ptr<const bounded_store_v6_physical_anchor>& left,
					const std::shared_ptr<const bounded_store_v6_physical_anchor>& right) noexcept
		{
			return left && right && !left.owner_before(right) && !right.owner_before(left);
		}

		[[nodiscard]] bool zero_digest(const std::array<std::byte, 32U>& value) noexcept
		{
			return std::ranges::all_of(value,
									   [](const std::byte byte)
									   {
										   return byte == std::byte{};
									   });
		}

		[[nodiscard]] std::string hex_digest(const std::array<std::byte, 32U>& value)
		{
			static constexpr char digits[] = "0123456789abcdef";
			std::string output{"sha256:"};
			output.reserve(71U);
			for (const auto byte : value)
			{
				const auto encoded = std::to_integer<unsigned char>(byte);
				output.push_back(digits[encoded >> 4U]);
				output.push_back(digits[encoded & 0x0fU]);
			}
			return output;
		}

		[[nodiscard]] bool canonical_digest(const std::string_view value) noexcept
		{
			return value.size() == 71U && value.starts_with("sha256:") &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] bool
		same_logical_publication(const bounded_store_v6_publication_observation& expected,
								 const bounded_store_v6_publication_observation& observed) noexcept
		{
			return expected.publication_id == observed.publication_id &&
				expected.series_id == observed.series_id &&
				expected.snapshot_id == observed.snapshot_id &&
				expected.sequence == observed.sequence &&
				expected.parent_publication == observed.parent_publication &&
				expected.state == observed.state && expected.corrupt == observed.corrupt &&
				observed.physical_generation >= expected.physical_generation &&
				observed.physical_generation != 0U;
		}

		[[nodiscard]] result<std::vector<std::byte>>
		canonical_frame_order_key(const bounded_store_v6_record_kind kind,
								  const std::span<const std::byte> body,
								  const std::uint64_t key_bytes,
								  const std::uint64_t payload_bytes)
		{
			if (key_bytes > body.size() || payload_bytes > body.size() - key_bytes ||
				key_bytes + payload_bytes != body.size())
				return unexpected(corrupt("projection-order", "record-shape"));
			const auto key_size = static_cast<std::size_t>(key_bytes);
			const auto payload_size = static_cast<std::size_t>(payload_bytes);
			const auto key = body.first(key_size);
			const auto payload = body.subspan(key_size, payload_size);
			auto decoded_key = canonical_binary_decode(key);
			auto decoded_payload = canonical_binary_decode(payload);
			if (!decoded_key || !decoded_payload ||
				decoded_key->type != canonical_value::kind::ordered_tuple ||
				decoded_payload->type != canonical_value::kind::ordered_tuple)
				return unexpected(corrupt("projection-order", "canonical-tuple"));
			std::vector<std::byte> kind_bytes{static_cast<std::byte>(kind)};
			auto output = canonical_binary(canonical_value::from_tuple({
				canonical_value::from_bytes(std::move(kind_bytes)),
				canonical_value::from_bytes(std::vector<std::byte>{key.begin(), key.end()}),
				canonical_value::from_bytes(std::vector<std::byte>{payload.begin(), payload.end()}),
			}));
			if (!output)
				return unexpected(corrupt("projection-order", "canonical-key"));
			return output;
		}

		struct staged_projection_census
		{
			std::uint64_t source_count{};
			std::uint64_t partition_begin_count{};
			std::uint64_t partition_end_count{};
			std::uint64_t event_count{};
			std::uint64_t claim_count{};
			std::uint64_t row_count{};
			std::uint64_t annotation_count{};
			std::uint64_t coverage_count{};
			std::uint64_t unresolved_count{};
			std::uint64_t framed_bytes{};
			sha256 binary_hash;
			sha256 task_receipt_hash;
		};

		struct input_authority
		{
			bounded_store_v6_session_metadata metadata;
			bounded_store_v6_external_census census;
			bounded_store_v6_input_observation observation;
		};

		struct backend_state
		{
			std::shared_ptr<bounded_store_v6_backend_port> port;
			std::shared_ptr<const bounded_store_v6_physical_anchor> anchor;
			std::string anchor_binding;
			bool sealed{};
			bool candidate_sealed{};
			bool actual_cursor_taken{};
			bool compare_consumed{};
			bool cleanup_called{};
			std::optional<error> cleanup_failure;
			bounded_store_v6_measured_projection measured;
			void retain_cleanup_failure(const error& value) noexcept
			{
				try
				{
					if (!cleanup_failure)
						cleanup_failure = value;
				}
				catch (...)
				{
				}
			}
			void retain_cleanup_exception(const char* detail) noexcept
			{
				try
				{
					if (!cleanup_failure)
						cleanup_failure =
							invariant("cleanup", detail == nullptr ? "exception" : detail);
				}
				catch (...)
				{
				}
			}
			~backend_state() noexcept
			{
				if (!port || cleanup_called)
					return;
				cleanup_called = true;
				try
				{
					auto result = port->abort_staging();
					if (!result)
						retain_cleanup_failure(result.error());
				}
				catch (const std::exception& exception)
				{
					retain_cleanup_exception(exception.what());
				}
				catch (...)
				{
					retain_cleanup_exception("non-standard-exception");
				}
			}
		};

		[[nodiscard]] result<std::vector<std::byte>>
		encode_semantic_frame(const bounded_store_v6_semantic_record& record)
		{
			if (!is_valid(record.kind) || record.key.type != canonical_value::kind::ordered_tuple ||
				record.payload.type != canonical_value::kind::ordered_tuple)
				return unexpected(corrupt("expected", "semantic-record"));
			if (auto valid = record.key.validate(); !valid)
				return unexpected(corrupt("expected", "semantic-key"));
			if (auto valid = record.payload.validate(); !valid)
				return unexpected(corrupt("expected", "semantic-payload"));
			auto key = canonical_binary(record.key);
			auto payload = canonical_binary(record.payload);
			if (!key || !payload)
				return unexpected(corrupt("expected", "semantic-codec"));
			auto framed = checked_bounded_store_v6_record_frame_bytes(key->size(), payload->size());
			if (!framed || *framed > bounded_store_v6_record_buffer_bytes)
				return unexpected(framed ? resource("record-bytes", "limit-exceeded")
										 : std::move(framed.error()));
			std::vector<std::byte> output;
			output.reserve(static_cast<std::size_t>(*framed));
			output.push_back(static_cast<std::byte>(record.kind));
			std::array<std::byte, 8U> length{};
			append_u64(length, key->size());
			output.insert(output.end(), length.begin(), length.end());
			append_u64(length, payload->size());
			output.insert(output.end(), length.begin(), length.end());
			output.insert(output.end(), key->begin(), key->end());
			output.insert(output.end(), payload->begin(), payload->end());
			sha256 checksum;
			append_u64(length, static_cast<std::uint64_t>(frame_domain.size()));
			checksum.update(length);
			checksum.update(std::span<const std::byte>{
				reinterpret_cast<const std::byte*>(frame_domain.data()), frame_domain.size()});
			append_u64(length, output.size());
			checksum.update(length);
			checksum.update(output);
			const auto digest = checksum.finish();
			output.insert(output.end(), digest.begin(), digest.end());
			return output;
		}

		/** Expected-side state machine over lossless semantic records, never backend bytes. */
		struct expected_semantic_stream
		{
			std::unique_ptr<bounded_store_v6_expected_semantic_cursor> cursor;
			std::vector<std::byte> encoded;
			std::vector<std::byte> previous_order_key;
			std::size_t offset{};
			bounded_store_v6_record_extent extent{};
			bool open{};
			bool eof{};
			bool begun{};
			bool ended{};

			explicit expected_semantic_stream(
				std::unique_ptr<bounded_store_v6_expected_semantic_cursor> source)
				: cursor{std::move(source)}
			{
			}

			[[nodiscard]] result<void> open_next()
			{
				if (!cursor || eof)
					return unexpected(invariant("expected", "closed"));
				if (open)
					return unexpected(invariant("expected", "record-not-consumed"));
				auto next = cursor->next_semantic_record();
				if (!next)
					return unexpected(std::move(next.error()));
				if (!*next)
				{
					eof = true;
					if (!ended)
						return unexpected(corrupt("expected", "missing-partition-end"));
					auto complete = cursor->authority_complete();
					if (!complete || !*complete)
						return unexpected(corrupt("expected", "incomplete-authority"));
					return {};
				}
				const auto& record = **next;
				if ((!begun || ended) &&
					record.kind != bounded_store_v6_record_kind::partition_begin)
					return unexpected(corrupt("expected", "missing-partition-begin"));
				if (record.kind == bounded_store_v6_record_kind::partition_begin && begun && !ended)
					return unexpected(corrupt("expected", "duplicate-partition-begin"));
				if (ended && record.kind == bounded_store_v6_record_kind::partition_begin)
					previous_order_key.clear();
				auto key = canonical_binary(record.key);
				auto payload = canonical_binary(record.payload);
				if (!key || !payload)
					return unexpected(corrupt("expected", "semantic-codec"));
				std::vector<std::byte> body;
				body.reserve(key->size() + payload->size());
				body.insert(body.end(), key->begin(), key->end());
				body.insert(body.end(), payload->begin(), payload->end());
				auto order =
					canonical_frame_order_key(record.kind, body, key->size(), payload->size());
				if (!order ||
					(!previous_order_key.empty() &&
					 !std::lexicographical_compare(previous_order_key.begin(),
												   previous_order_key.end(),
												   order->begin(),
												   order->end())))
					return unexpected(corrupt("expected", "reordered-or-duplicate"));
				previous_order_key = std::move(*order);
				auto frame = encode_semantic_frame(record);
				if (!frame)
					return unexpected(std::move(frame.error()));
				extent = {record.kind,
						  static_cast<std::uint64_t>(key->size()),
						  static_cast<std::uint64_t>(payload->size()),
						  static_cast<std::uint64_t>(frame->size())};
				encoded = std::move(*frame);
				offset = 0U;
				open = true;
				return {};
			}

			[[nodiscard]] result<std::byte> next_byte()
			{
				if (!open || offset >= encoded.size())
					return unexpected(invariant("expected", "record-eof"));
				return encoded[offset++];
			}

			[[nodiscard]] result<void> finish_record()
			{
				if (!open || offset != encoded.size())
					return unexpected(corrupt("expected", "record-eof"));
				begun = begun || extent.kind == bounded_store_v6_record_kind::partition_begin;
				ended = extent.kind == bounded_store_v6_record_kind::partition_end;
				open = false;
				encoded.clear();
				return {};
			}
		};

		/** Actual-side state machine over backend-authenticated physical bytes only. */
		struct physical_actual_stream
		{
			std::unique_ptr<bounded_store_v6_actual_cursor_source> cursor;
			std::array<std::byte, comparison_buffer_bytes> buffer{};
			std::array<std::byte, frame_projection_prefix_bytes> prefix{};
			std::array<std::byte, frame_checksum_bytes> checksum{};
			std::vector<std::byte> body;
			std::vector<std::byte> previous_order_key;
			std::size_t buffered{};
			std::size_t buffer_offset{};
			std::uint64_t consumed{};
			std::uint64_t projection_bytes{};
			bounded_store_v6_record_extent extent{};
			bool open{};
			bool eof{};
			bool begun{};
			bool ended{};
			sha256 hash;

			explicit physical_actual_stream(
				std::unique_ptr<bounded_store_v6_actual_cursor_source> source)
				: cursor{std::move(source)}
			{
			}

			[[nodiscard]] result<void> open_next()
			{
				if (!cursor || eof)
					return unexpected(invariant("cursor", "closed"));
				if (open)
					return unexpected(invariant("cursor", "record-not-consumed"));
				auto next = cursor->next_record();
				if (!next)
					return unexpected(std::move(next.error()));
				if (!*next)
				{
					eof = true;
					if (!ended)
						return unexpected(corrupt("projection", "missing-partition-end"));
					return {};
				}
				extent = **next;
				if (!is_valid(extent.kind))
					return unexpected(corrupt("projection", "unknown-kind"));
				auto frame_size = checked_bounded_store_v6_record_frame_bytes(extent.key_bytes,
																			  extent.payload_bytes);
				if (!frame_size || *frame_size != extent.framed_bytes)
					return unexpected(corrupt("projection", "frame-size"));
				if (extent.framed_bytes < frame_projection_prefix_bytes + frame_checksum_bytes)
					return unexpected(corrupt("projection", "frame-size"));
				if (extent.framed_bytes > bounded_store_v6_record_buffer_bytes)
					return unexpected(resource("record-bytes", "limit-exceeded"));
				if (!begun && extent.kind != bounded_store_v6_record_kind::partition_begin)
					return unexpected(corrupt("projection", "missing-partition-begin"));
				if (ended && extent.kind != bounded_store_v6_record_kind::partition_begin)
					return unexpected(corrupt("projection", "after-partition-end"));
				if (extent.kind == bounded_store_v6_record_kind::partition_begin && begun && !ended)
					return unexpected(corrupt("projection", "duplicate-partition-begin"));
				if (ended && extent.kind == bounded_store_v6_record_kind::partition_begin)
					previous_order_key.clear();
				projection_bytes = extent.framed_bytes - frame_checksum_bytes;
				consumed = 0U;
				buffered = 0U;
				buffer_offset = 0U;
				prefix.fill(std::byte{});
				checksum.fill(std::byte{});
				body.clear();
				body.reserve(static_cast<std::size_t>(extent.key_bytes + extent.payload_bytes));
				hash = sha256{};
				std::array<std::byte, 8U> length{};
				append_u64(length, static_cast<std::uint64_t>(frame_domain.size()));
				hash.update(length);
				hash.update(std::span<const std::byte>{
					reinterpret_cast<const std::byte*>(frame_domain.data()), frame_domain.size()});
				append_u64(length, projection_bytes);
				hash.update(length);
				open = true;
				return {};
			}

			[[nodiscard]] result<std::byte> next_byte()
			{
				if (!open || consumed >= extent.framed_bytes)
					return unexpected(invariant("cursor", "record-eof"));
				if (buffer_offset == buffered)
				{
					buffered = 0U;
					buffer_offset = 0U;
					auto read = cursor->read_record_bytes(buffer);
					if (!read)
						return unexpected(std::move(read.error()));
					if (*read == 0U || *read > buffer.size() ||
						*read > extent.framed_bytes - consumed)
						return unexpected(corrupt("projection", "short-or-overrun-read"));
					buffered = *read;
					const auto frame_offset = consumed;
					for (std::size_t index{}; index < buffered; ++index)
					{
						const auto offset = frame_offset + index;
						if (offset < prefix.size())
							prefix[static_cast<std::size_t>(offset)] = buffer[index];
						if (offset < projection_bytes)
							hash.update(std::span<const std::byte>{buffer.data() + index, 1U});
						else
							checksum[static_cast<std::size_t>(offset - projection_bytes)] =
								buffer[index];
						if (offset >= prefix.size() && offset < projection_bytes)
							body.push_back(buffer[index]);
					}
				}
				const auto output = buffer[buffer_offset++];
				++consumed;
				return output;
			}

			[[nodiscard]] result<void> finish_record()
			{
				if (!open || consumed != extent.framed_bytes)
					return unexpected(corrupt("projection", "record-eof"));
				if (static_cast<std::uint8_t>(prefix[0U]) !=
						static_cast<std::uint8_t>(extent.kind) ||
					read_u64(prefix, 1U) != extent.key_bytes ||
					read_u64(prefix, 9U) != extent.payload_bytes)
					return unexpected(corrupt("projection", "frame-header"));
				if (!bytes_equal(hash.finish(), checksum))
					return unexpected(corrupt("projection", "checksum-mismatch"));
				auto order_key = canonical_frame_order_key(
					extent.kind, body, extent.key_bytes, extent.payload_bytes);
				if (!order_key)
					return unexpected(std::move(order_key.error()));
				if (!previous_order_key.empty() &&
					!std::lexicographical_compare(previous_order_key.begin(),
												  previous_order_key.end(),
												  order_key->begin(),
												  order_key->end()))
					return unexpected(corrupt("projection-order", "reordered-or-duplicate"));
				previous_order_key = std::move(*order_key);
				begun = begun || extent.kind == bounded_store_v6_record_kind::partition_begin;
				ended = extent.kind == bounded_store_v6_record_kind::partition_end;
				open = false;
				return {};
			}
		};

		[[nodiscard]] result<void>
		validate_external_census(const bounded_store_v6_external_census& census)
		{
			if (auto tasks = validate_bounded_store_v6_task_count(census.task_count); !tasks)
				return unexpected(std::move(tasks.error()));
			if (census.partition_count == 0U || census.event_count == 0U ||
				census.task_count > census.partition_count ||
				census.partition_count > census.event_count)
				return unexpected(invariant("input", "invalid-census"));
			if (census.input_bytes == 0U ||
				census.input_bytes > bounded_store_v6_max_aggregate_bytes)
				return unexpected(resource("input-bytes", "limit-exceeded"));
			const auto closed_event_count = static_cast<__uint128_t>(census.partition_count) * 2U +
				census.claim_count + census.row_count + census.annotation_count +
				census.coverage_count + census.unresolved_count;
			if (closed_event_count != census.event_count)
				return unexpected(invariant("input", "event-census"));
			if (zero_digest(census.binary_input_sha256) ||
				census.immutable_authority_binding.empty())
				return unexpected(invariant("input", "missing-authority-binding"));
			return {};
		}

		[[nodiscard]] result<void>
		validate_task_receipt(const bounded_store_v6_task_receipt& receipt)
		{
			if (receipt.task_id.empty() || receipt.task_id.size() > 2'048U ||
				receipt.immutable_binding.empty() || receipt.immutable_binding.size() > 2'048U)
				return unexpected(invariant("task", "identity-or-binding"));
			if (receipt.ordinal >= bounded_store_v6_max_tasks || receipt.partition_count == 0U ||
				receipt.event_count == 0U || receipt.partition_count > receipt.event_count)
				return unexpected(resource("task", "census"));
			if (receipt.framed_bytes == 0U ||
				receipt.framed_bytes > bounded_store_v6_source_window_bytes)
				return unexpected(resource("task-bytes", "limit-exceeded"));
			const auto closed_event_count = static_cast<__uint128_t>(receipt.partition_count) * 2U +
				receipt.claim_count + receipt.row_count + receipt.annotation_count +
				receipt.coverage_count + receipt.unresolved_count;
			if (closed_event_count != receipt.event_count || zero_digest(receipt.binary_sha256))
				return unexpected(invariant("task", "event-census"));
			return {};
		}

		void update_task_receipt_hash(sha256& hash,
									  const bounded_store_v6_task_receipt& receipt) noexcept
		{
			const auto update_u64 = [&](const std::uint64_t value)
			{
				std::array<std::byte, 8U> bytes{};
				append_u64(bytes, value);
				hash.update(bytes);
			};
			const auto update_text = [&](const std::string_view value)
			{
				update_u64(static_cast<std::uint64_t>(value.size()));
				hash.update(std::span<const std::byte>{
					reinterpret_cast<const std::byte*>(value.data()), value.size()});
			};
			update_text("cxxlens/store-v6-task-receipt/v1");
			update_text(receipt.task_id);
			for (const auto value : {receipt.ordinal,
									 receipt.partition_count,
									 receipt.event_count,
									 receipt.claim_count,
									 receipt.row_count,
									 receipt.annotation_count,
									 receipt.coverage_count,
									 receipt.unresolved_count,
									 receipt.framed_bytes})
				update_u64(value);
			hash.update(receipt.binary_sha256);
			update_text(receipt.immutable_binding);
		}

		[[nodiscard]] result<void> validate_head(const bounded_store_v6_expected_head& head,
												 const snapshot_series_selector& selector)
		{
			if (!is_valid(head.value))
				return unexpected(invariant("expected-head", "invalid-kind"));
			if (auto valid = selector.validate(); !valid)
				return unexpected(invariant("expected-head", "invalid-selector"));
			const auto selector_id = selector.id();
			if (head.selector != selector)
				return unexpected(invariant("expected-head", "series-mismatch"));
			if (head.value == bounded_store_v6_expected_head::kind::genesis)
			{
				if (head.publication || head.snapshot)
					return unexpected(invariant("expected-head", "invalid-genesis"));
				return {};
			}
			if (!head.publication || !head.snapshot)
				return unexpected(invariant("expected-head", "invalid-publication"));
			const auto& publication = *head.publication;
			const auto& snapshot = *head.snapshot;
			if (publication.publication_id.empty() || publication.series_id != selector_id ||
				publication.snapshot_id.empty() || publication.sequence == 0U ||
				publication.physical_generation == 0U || publication.corrupt ||
				publication.state != publication_state::committed ||
				snapshot.snapshot_id != publication.snapshot_id || snapshot.partition_count == 0U ||
				!canonical_digest(snapshot.semantic_projection_digest) ||
				!canonical_digest(snapshot.canonical_export_digest))
				return unexpected(invariant("expected-head", "invalid-publication"));
			auto identity = publication_record_identity(publication.series_id,
														publication.snapshot_id,
														publication.sequence,
														publication.parent_publication);
			if (!identity || *identity != publication.publication_id)
				return unexpected(invariant("expected-head", "publication-identity"));
			return {};
		}

		[[nodiscard]] result<void>
		validate_reservation(const bounded_store_v6_report_tail_reservation& reservation)
		{
			if (reservation.writer_object_binding.empty() ||
				reservation.spool_object_binding.empty() || reservation.reservation_binding.empty())
				return unexpected(invariant("report", "missing-binding"));
			if (reservation.reserved_tail_bytes != bounded_store_v6_exact_report_tail_bytes ||
				reservation.maximum_report_bytes != bounded_store_v6_max_report_bytes ||
				reservation.capacity_bytes < reservation.prefix_bytes ||
				reservation.capacity_bytes - reservation.prefix_bytes <
					reservation.reserved_tail_bytes ||
				reservation.capacity_bytes > bounded_store_v6_max_report_bytes)
				return unexpected(resource("report-tail", "reservation-mismatch"));
			return {};
		}

		[[nodiscard]] std::string
		candidate_identity(const input_authority& input,
						   const backend_state& backend,
						   const std::array<std::byte, 32U>& task_receipts)
		{
			sha256 hash;
			const auto update_u64 = [&](const std::uint64_t value)
			{
				std::array<std::byte, 8U> bytes{};
				append_u64(bytes, value);
				hash.update(bytes);
			};
			const auto update_text = [&](const std::string_view value)
			{
				update_u64(static_cast<std::uint64_t>(value.size()));
				hash.update(std::span<const std::byte>{
					reinterpret_cast<const std::byte*>(value.data()), value.size()});
			};
			const auto update_optional = [&](const std::optional<std::string>& value)
			{
				update_u64(value ? 1U : 0U);
				if (value)
					update_text(*value);
			};
			const auto update_snapshot = [&](const bounded_store_v6_snapshot_observation& value)
			{
				update_text(value.snapshot_id);
				for (const auto count : {value.partition_count,
										 value.row_count,
										 value.claim_count,
										 value.coverage_count,
										 value.unresolved_count})
					update_u64(count);
				update_text(value.semantic_projection_digest);
				update_text(value.canonical_export_digest);
			};
			const auto update_publication =
				[&](const bounded_store_v6_publication_observation& value)
			{
				update_text(value.publication_id);
				update_text(value.series_id);
				update_text(value.snapshot_id);
				update_u64(value.sequence);
				update_u64(value.physical_generation);
				update_optional(value.parent_publication);
				update_u64(static_cast<std::uint8_t>(value.state));
				update_u64(value.corrupt ? 1U : 0U);
			};

			update_text("cxxlens/store-v6-candidate/v2");
			update_u64(static_cast<std::uint8_t>(input.metadata.backend));
			update_text(input.metadata.relation_engine_generation);
			update_text(input.metadata.selector.id());
			update_text(input.metadata.staging_session_id);
			update_optional(input.metadata.exact_sqlite_path);
			update_u64(static_cast<std::uint8_t>(input.metadata.expected_head.value));
			if (input.metadata.expected_head.publication)
				update_publication(*input.metadata.expected_head.publication);
			if (input.metadata.expected_head.snapshot)
				update_snapshot(*input.metadata.expected_head.snapshot);
			for (const auto count : {input.census.task_count,
									 input.census.partition_count,
									 input.census.event_count,
									 input.census.claim_count,
									 input.census.row_count,
									 input.census.annotation_count,
									 input.census.coverage_count,
									 input.census.unresolved_count,
									 input.census.input_bytes})
				update_u64(count);
			hash.update(input.census.binary_input_sha256);
			update_text(input.census.immutable_authority_binding);
			hash.update(task_receipts);
			update_u64(backend.measured.record_count);
			update_u64(backend.measured.framed_bytes);
			hash.update(backend.measured.binary_sha256);
			update_snapshot(backend.measured.candidate_snapshot);
			update_text(backend.measured.immutable_binding);
			return "store-candidate:" + hex_digest(hash.finish());
		}
	} // namespace

	// The opaque anchor is intentionally empty.  Identity is object identity, while the backend's
	// non-authoritative spelling is exposed only in bounded observations.
	struct bounded_store_v6_physical_anchor
	{
		std::uint64_t nonce{};
	};

	struct bounded_store_v6_staging_session::state
	{
		bounded_store_v6_session_metadata metadata;
		bool consumed{};
	};

	struct bounded_store_sealed_input_binding::state
	{
		std::shared_ptr<const input_authority> authority;
	};

	struct bounded_store_v6_sealed_task::state
	{
		bounded_store_v6_task_receipt receipt;
		std::unique_ptr<bounded_store_v6_task_frame_source> source;
		bool consumed{};
	};

	struct bounded_store_v6_expected_projection::state
	{
		std::unique_ptr<bounded_store_v6_expected_semantic_cursor> cursor;
		std::shared_ptr<const input_authority> authority;
		std::string candidate_id;
		bool consumed{};
	};

	struct bounded_store_prepared_publication::state
	{
		enum class phase : std::uint8_t
		{
			created,
			staging,
			staged,
			sealed,
			aborted,
		};

		std::shared_ptr<const input_authority> input;
		std::shared_ptr<backend_state> backend;
		bounded_store_prepared_publication_observation observation;
		phase current_phase{phase::created};
		staged_projection_census staged_census;
		std::set<std::string, std::less<>> staged_task_ids;
		bool sealed{};
		bool actual_cursor_taken{};
		bool compared{};
	};

	struct bounded_store_authenticated_actual_cursor::state
	{
		std::unique_ptr<bounded_store_v6_actual_cursor_source> cursor;
		std::shared_ptr<const bounded_store_v6_physical_anchor> anchor;
		std::string anchor_binding;
		bool finished{};
	};

	struct bounded_store_projection_match::state
	{
		bounded_store_projection_match_observation observation;
		std::shared_ptr<const bounded_store_v6_physical_anchor> anchor;
		bool consumed{};
	};

	struct bounded_store_report_tail_custody::state
	{
		std::shared_ptr<backend_state> backend;
		std::shared_ptr<bounded_store_report_tail_writer> writer;
		std::shared_ptr<const bounded_store_v6_physical_anchor> anchor;
		bounded_store_report_tail_custody_observation observation;
		bool consumed{};
		~state() noexcept
		{
			if (!writer || consumed || !observation.live)
				return;
			try
			{
				auto result = writer->release();
				if (backend && !result)
					backend->retain_cleanup_failure(result.error());
			}
			catch (const std::exception& exception)
			{
				if (backend)
					backend->retain_cleanup_exception(exception.what());
			}
			catch (...)
			{
				if (backend)
					backend->retain_cleanup_exception("non-standard-exception");
			}
		}
	};

	struct bounded_store_validated_publication::state
	{
		std::shared_ptr<backend_state> backend;
		std::shared_ptr<const bounded_store_v6_physical_anchor> anchor;
		std::shared_ptr<bounded_store_report_tail_writer> writer;
		bounded_store_v6_terminal_observation identity;
		bounded_store_report_tail_custody_observation report;
		bool consumed{};
		~state() noexcept
		{
			if (writer && !consumed && report.live)
			{
				try
				{
					auto result = writer->release();
					if (backend && !result)
						backend->retain_cleanup_failure(result.error());
				}
				catch (const std::exception& exception)
				{
					if (backend)
						backend->retain_cleanup_exception(exception.what());
				}
				catch (...)
				{
					if (backend)
						backend->retain_cleanup_exception("non-standard-exception");
				}
			}
		}
	};

	struct bounded_store_terminal_custody::state
	{
		std::shared_ptr<backend_state> backend;
		std::shared_ptr<const bounded_store_v6_physical_anchor> anchor;
		std::shared_ptr<bounded_store_report_tail_writer> writer;
		bounded_store_v6_terminal_observation observation;
		bounded_store_report_tail_custody_observation report;
		bool report_finalization_attempted{};
		bool report_finalized{};
		bool writer_release_attempted{};
		bool cleanup_drained{};
		bounded_store_v6_cleanup_observation cleanup;
		~state() noexcept
		{
			if (!cleanup_drained && !writer_release_attempted && writer && report.live)
			{
				writer_release_attempted = true;
				try
				{
					auto result = writer->release();
					if (backend && !result)
						backend->retain_cleanup_failure(result.error());
				}
				catch (const std::exception& exception)
				{
					if (backend)
						backend->retain_cleanup_exception(exception.what());
				}
				catch (...)
				{
					if (backend)
						backend->retain_cleanup_exception("non-standard-exception");
				}
			}
		}
	};

	struct bounded_store_full_report_release::state
	{
		bounded_store_full_report_observation observation;
		std::shared_ptr<const bounded_store_v6_physical_anchor> anchor;
		std::string terminal_binding;
		bool drain_attempted{};
		bool consumed{};
	};

	// Move-only token boilerplate.  A moved-from token is inert and its observation accessors
	// return stable empty values rather than dereferencing a caller-forged state.
	bounded_store_v6_staging_session::bounded_store_v6_staging_session(std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}
	bounded_store_v6_staging_session::bounded_store_v6_staging_session(
		bounded_store_v6_staging_session&&) noexcept = default;
	bounded_store_v6_staging_session& bounded_store_v6_staging_session::operator=(
		bounded_store_v6_staging_session&&) noexcept = default;
	bounded_store_v6_staging_session::~bounded_store_v6_staging_session() = default;

	bounded_store_sealed_input_binding::bounded_store_sealed_input_binding(
		std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}
	bounded_store_sealed_input_binding::bounded_store_sealed_input_binding(
		bounded_store_sealed_input_binding&&) noexcept = default;
	bounded_store_sealed_input_binding& bounded_store_sealed_input_binding::operator=(
		bounded_store_sealed_input_binding&&) noexcept = default;
	bounded_store_sealed_input_binding::~bounded_store_sealed_input_binding() = default;

	bounded_store_v6_sealed_task::bounded_store_v6_sealed_task(std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}
	bounded_store_v6_sealed_task::bounded_store_v6_sealed_task(
		bounded_store_v6_sealed_task&&) noexcept = default;
	bounded_store_v6_sealed_task&
	bounded_store_v6_sealed_task::operator=(bounded_store_v6_sealed_task&&) noexcept = default;
	bounded_store_v6_sealed_task::~bounded_store_v6_sealed_task() = default;

	bounded_store_v6_expected_projection::bounded_store_v6_expected_projection(
		std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}
	bounded_store_v6_expected_projection::bounded_store_v6_expected_projection(
		bounded_store_v6_expected_projection&&) noexcept = default;
	bounded_store_v6_expected_projection& bounded_store_v6_expected_projection::operator=(
		bounded_store_v6_expected_projection&&) noexcept = default;
	bounded_store_v6_expected_projection::~bounded_store_v6_expected_projection() = default;

	bounded_store_prepared_publication::bounded_store_prepared_publication(
		std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}
	bounded_store_prepared_publication::bounded_store_prepared_publication(
		bounded_store_prepared_publication&&) noexcept = default;
	bounded_store_prepared_publication& bounded_store_prepared_publication::operator=(
		bounded_store_prepared_publication&&) noexcept = default;
	bounded_store_prepared_publication::~bounded_store_prepared_publication() = default;

	bounded_store_authenticated_actual_cursor::bounded_store_authenticated_actual_cursor(
		std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}
	bounded_store_authenticated_actual_cursor::bounded_store_authenticated_actual_cursor(
		bounded_store_authenticated_actual_cursor&&) noexcept = default;
	bounded_store_authenticated_actual_cursor& bounded_store_authenticated_actual_cursor::operator=(
		bounded_store_authenticated_actual_cursor&&) noexcept = default;
	bounded_store_authenticated_actual_cursor::~bounded_store_authenticated_actual_cursor() =
		default;

	bounded_store_projection_match::bounded_store_projection_match(std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}
	bounded_store_projection_match::bounded_store_projection_match(
		bounded_store_projection_match&&) noexcept = default;
	bounded_store_projection_match&
	bounded_store_projection_match::operator=(bounded_store_projection_match&&) noexcept = default;
	bounded_store_projection_match::~bounded_store_projection_match() = default;

	bounded_store_report_tail_custody::bounded_store_report_tail_custody(
		std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}
	bounded_store_report_tail_custody::bounded_store_report_tail_custody(
		bounded_store_report_tail_custody&&) noexcept = default;
	bounded_store_report_tail_custody& bounded_store_report_tail_custody::operator=(
		bounded_store_report_tail_custody&&) noexcept = default;
	bounded_store_report_tail_custody::~bounded_store_report_tail_custody() = default;

	bounded_store_validated_publication::bounded_store_validated_publication(
		std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}
	bounded_store_validated_publication::bounded_store_validated_publication(
		bounded_store_validated_publication&&) noexcept = default;
	bounded_store_validated_publication& bounded_store_validated_publication::operator=(
		bounded_store_validated_publication&&) noexcept = default;
	bounded_store_validated_publication::~bounded_store_validated_publication() = default;

	bounded_store_terminal_custody::bounded_store_terminal_custody(std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}
	bounded_store_terminal_custody::bounded_store_terminal_custody(
		bounded_store_terminal_custody&&) noexcept = default;
	bounded_store_terminal_custody&
	bounded_store_terminal_custody::operator=(bounded_store_terminal_custody&&) noexcept = default;
	bounded_store_terminal_custody::~bounded_store_terminal_custody() = default;

	bounded_store_full_report_release::bounded_store_full_report_release(
		std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}
	bounded_store_full_report_release::bounded_store_full_report_release(
		bounded_store_full_report_release&&) noexcept = default;
	bounded_store_full_report_release& bounded_store_full_report_release::operator=(
		bounded_store_full_report_release&&) noexcept = default;
	bounded_store_full_report_release::~bounded_store_full_report_release() = default;

	std::string_view bounded_store_v6_staging_session::staging_session_id() const noexcept
	{
		return state_ ? state_->metadata.staging_session_id : std::string_view{};
	}
	bounded_store_v6_backend bounded_store_v6_staging_session::backend() const noexcept
	{
		return state_ ? state_->metadata.backend : bounded_store_v6_backend::memory;
	}

	const bounded_store_v6_input_observation&
	bounded_store_sealed_input_binding::observation() const noexcept
	{
		static const bounded_store_v6_input_observation empty{};
		return state_ && state_->authority ? state_->authority->observation : empty;
	}
	std::string_view bounded_store_sealed_input_binding::staging_session_id() const noexcept
	{
		return observation().staging_session_id;
	}
	const bounded_store_v6_expected_head&
	bounded_store_sealed_input_binding::expected_head() const noexcept
	{
		return observation().expected_head;
	}
	const bounded_store_v6_external_census&
	bounded_store_sealed_input_binding::external_census() const noexcept
	{
		return observation().external_census;
	}

	const bounded_store_v6_task_receipt& bounded_store_v6_sealed_task::receipt() const noexcept
	{
		static const bounded_store_v6_task_receipt empty{};
		return state_ ? state_->receipt : empty;
	}

	const bounded_store_v6_input_observation&
	bounded_store_v6_expected_projection::observation() const noexcept
	{
		static const bounded_store_v6_input_observation empty{};
		return state_ && state_->authority ? state_->authority->observation : empty;
	}

	const bounded_store_prepared_publication_observation&
	bounded_store_prepared_publication::observation() const noexcept
	{
		static const bounded_store_prepared_publication_observation empty{};
		return state_ ? state_->observation : empty;
	}
	std::optional<bounded_store_v6_backend>
	bounded_store_prepared_publication::backend() const noexcept
	{
		if (!state_ || !state_->backend || !state_->backend->port)
			return std::nullopt;
		return state_->backend->port->backend();
	}
	std::string_view bounded_store_prepared_publication::staging_session_id() const noexcept
	{
		return observation().staging_session_id;
	}
	std::string_view bounded_store_prepared_publication::candidate_id() const noexcept
	{
		return observation().candidate_id;
	}
	const bounded_store_v6_expected_head&
	bounded_store_prepared_publication::expected_head() const noexcept
	{
		return observation().expected_head;
	}

	const bounded_store_projection_match_observation&
	bounded_store_projection_match::observation() const noexcept
	{
		static const bounded_store_projection_match_observation empty{};
		return state_ ? state_->observation : empty;
	}
	const bounded_store_report_tail_custody_observation&
	bounded_store_report_tail_custody::observation() const noexcept
	{
		static const bounded_store_report_tail_custody_observation empty{};
		return state_ ? state_->observation : empty;
	}
	const bounded_store_v6_terminal_observation&
	bounded_store_terminal_custody::observation() const noexcept
	{
		static const bounded_store_v6_terminal_observation empty{};
		return state_ ? state_->observation : empty;
	}
	const bounded_store_report_tail_custody_observation&
	bounded_store_terminal_custody::report_custody_observation() const noexcept
	{
		static const bounded_store_report_tail_custody_observation empty{};
		return state_ ? state_->report : empty;
	}
	bool bounded_store_terminal_custody::cleanup_drained() const noexcept
	{
		return state_ && state_->cleanup_drained;
	}
	const bounded_store_v6_cleanup_observation&
	bounded_store_terminal_custody::cleanup_observation() const noexcept
	{
		static const bounded_store_v6_cleanup_observation empty{};
		return state_ ? state_->cleanup : empty;
	}
	const bounded_store_full_report_observation&
	bounded_store_full_report_release::observation() const noexcept
	{
		static const bounded_store_full_report_observation empty{};
		return state_ ? state_->observation : empty;
	}

	result<bounded_store_v6_expected_head>
	make_bounded_store_v6_genesis_head(snapshot_series_selector selector)
	{
		try
		{
			if (auto valid = selector.validate(); !valid)
				return unexpected(std::move(valid.error()));
			return bounded_store_v6_expected_head{bounded_store_v6_expected_head::kind::genesis,
												  std::move(selector),
												  std::nullopt,
												  std::nullopt};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("session", exception.what()));
		}
	}

	result<bounded_store_v6_expected_head>
	make_bounded_store_v6_publication_head(snapshot_series_selector selector,
										   bounded_store_v6_publication_observation publication,
										   bounded_store_v6_snapshot_observation snapshot)
	{
		try
		{
			bounded_store_v6_expected_head output{bounded_store_v6_expected_head::kind::publication,
												  std::move(selector),
												  std::move(publication),
												  std::move(snapshot)};
			if (auto valid = validate_head(output, output.selector); !valid)
				return unexpected(std::move(valid.error()));
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("input", exception.what()));
		}
	}

	result<void> validate_bounded_store_v6_product_constants()
	{
		if (bounded_store_v6_max_tasks != 4'096U ||
			bounded_store_v6_max_aggregate_bytes != 512U * 1024U * 1024U ||
			bounded_store_v6_source_window_bytes != 64U * 1024U * 1024U ||
			bounded_store_v6_sort_arena_bytes != 8U * 1024U * 1024U ||
			bounded_store_v6_comparator_cursor_bytes != 32U * 1024U ||
			bounded_store_v6_backend_cursor_bytes != 1024U * 1024U ||
			bounded_store_v6_codec_hash_bytes != 64U * 1024U ||
			bounded_store_v6_record_buffer_bytes != 1024U * 1024U ||
			bounded_store_v6_counter_state_bytes != 4U * 1024U ||
			bounded_store_v6_resident_window_bytes != 77'729'792U ||
			bounded_store_v6_merge_file_descriptors != 18U ||
			bounded_store_v6_sqlite_chunk_bytes != 8U * 1024U * 1024U ||
			bounded_store_v6_stream_header_bytes != 86U ||
			bounded_store_v6_stream_trailer_bytes != 112U ||
			bounded_store_v6_record_fixed_bytes != 49U ||
			bounded_store_v6_exact_report_tail_bytes != 28'321'546U ||
			bounded_store_v6_max_report_bytes != 1024U * 1024U * 1024U ||
			bounded_store_v6_publication_terminal_count != 6U ||
			bounded_store_v6_report_section_count != 19U)
			return unexpected(invariant("bounds", "contract-mismatch"));
		return {};
	}

	result<void> validate_bounded_store_v6_task_count(const std::uint64_t task_count)
	{
		if (task_count == 0U || task_count > bounded_store_v6_max_tasks)
			return unexpected(resource("tasks", "limit-exceeded"));
		return {};
	}

	result<std::uint64_t>
	checked_bounded_store_v6_aggregate_charge(const std::uint64_t current_bytes,
											  const std::uint64_t next_bytes)
	{
		const auto total =
			static_cast<__uint128_t>(current_bytes) + static_cast<__uint128_t>(next_bytes);
		if (total > bounded_store_v6_max_aggregate_bytes)
			return unexpected(resource("aggregate-bytes", "limit-exceeded"));
		return static_cast<std::uint64_t>(total);
	}

	result<std::uint64_t>
	checked_bounded_store_v6_record_frame_bytes(const std::uint64_t key_bytes,
												const std::uint64_t payload_bytes)
	{
		std::uint64_t total{};
		if (!checked_add(bounded_store_v6_record_fixed_bytes, key_bytes, total) ||
			!checked_add(total, payload_bytes, total))
			return unexpected(resource("record-bytes", "checked-overflow"));
		if (total > bounded_store_v6_max_aggregate_bytes)
			return unexpected(resource("record-bytes", "limit-exceeded"));
		return total;
	}

	bounded_store_v6_error_class
	classify_bounded_store_v6_error(const bounded_store_v6_backend backend,
									const std::string_view exact_series_id,
									const std::string_view exact_candidate_snapshot_id,
									const std::string_view exact_expected_publication_id,
									const error& value) noexcept
	{
		// The accepted writer-publish map is SQLite-only.  Memory publication is atomic and cannot
		// surface a recoverable SDK error; any such tuple is a backend invariant breach.
		if (backend != bounded_store_v6_backend::sqlite)
			return bounded_store_v6_error_class::invariant_breach;
		if (value.code == "store.publication-conflict" && value.field == exact_series_id &&
			value.detail.empty())
			return bounded_store_v6_error_class::stale_parent;
		if (value.code == "store.sqlite-failure" && value.field == "database" &&
			!value.detail.empty())
			return bounded_store_v6_error_class::sqlite_failure;
		if (value.code == "store.counter-overflow" && value.detail.empty() &&
			(value.field == "publication_sequence" || value.field == "physical_generation"))
			return bounded_store_v6_error_class::corrupt_store;
		if (value.code == "store.hash-collision" && value.detail.empty() &&
			value.field == exact_candidate_snapshot_id)
			return bounded_store_v6_error_class::corrupt_store;
		if (value.code == "store.snapshot-ambiguous" && value.detail.empty() &&
			value.field == exact_candidate_snapshot_id)
			return bounded_store_v6_error_class::corrupt_store;
		if (value.code == "store.corrupt")
		{
			const bool sqlite_tuple = value.field == "sqlite" &&
				(value.detail == "backend" || value.detail == "column-count" ||
				 value.detail == "publication-row" || value.detail == "series-head-count" ||
				 value.detail == "series-head" || value.detail == "series-head-sequence");
			const bool publication_tuple = value.field == exact_expected_publication_id &&
				!exact_expected_publication_id.empty() &&
				(value.detail == "authority-record" || value.detail == "duplicate-publication-id" ||
				 value.detail == "parent" || value.detail == "parent-sequence");
			const bool series_tuple = value.field == exact_series_id &&
				(value.detail == "duplicate-sequence" || value.detail == "series-roots" ||
				 value.detail == "series-head-cas");
			if (sqlite_tuple || publication_tuple || series_tuple)
				return bounded_store_v6_error_class::corrupt_store;
		}
		return bounded_store_v6_error_class::invariant_breach;
	}

	result<void> validate_bounded_store_v6_reopen_observation(
		const bounded_store_v6_expected_head& expected_head,
		const bounded_store_v6_publication_observation& committed_publication,
		const bounded_store_v6_snapshot_observation& candidate_snapshot,
		const bounded_store_v6_reopen_observation& observed)
	{
		try
		{
			const auto closed_lookup = [](const bounded_store_v6_lookup_observation& value)
			{
				switch (value.status)
				{
					case bounded_store_v6_lookup_observation::state::not_attempted:
						return !value.failure && !value.publication && !value.snapshot;
					case bounded_store_v6_lookup_observation::state::present:
						return !value.failure && value.publication && value.snapshot;
					case bounded_store_v6_lookup_observation::state::not_found:
						return !value.failure && !value.publication && !value.snapshot;
					case bounded_store_v6_lookup_observation::state::failed:
						return value.failure && !value.publication && !value.snapshot;
				}
				return false;
			};
			if (!closed_lookup(observed.current) || !closed_lookup(observed.expected_parent) ||
				!closed_lookup(observed.publication) || !closed_lookup(observed.snapshot))
				return unexpected(invariant("reopen", "non-total-lookup"));
			const bool parent_valid = expected_head.publication && expected_head.snapshot
				? observed.expected_parent.status ==
						bounded_store_v6_lookup_observation::state::present &&
					observed.expected_parent.publication && observed.expected_parent.snapshot &&
					same_logical_publication(*expected_head.publication,
											 *observed.expected_parent.publication) &&
					*observed.expected_parent.snapshot == *expected_head.snapshot
				: !expected_head.publication && !expected_head.snapshot &&
					observed.expected_parent.status ==
						bounded_store_v6_lookup_observation::state::not_found;
			const bool current_valid =
				observed.current.status == bounded_store_v6_lookup_observation::state::present &&
				observed.current.publication && observed.current.snapshot &&
				same_logical_publication(committed_publication, *observed.current.publication) &&
				*observed.current.snapshot == candidate_snapshot;
			const bool publication_valid = observed.publication.status ==
					bounded_store_v6_lookup_observation::state::present &&
				observed.publication.publication && observed.publication.snapshot &&
				same_logical_publication(committed_publication,
										 *observed.publication.publication) &&
				*observed.publication.snapshot == candidate_snapshot;
			const bool snapshot_valid =
				observed.snapshot.status == bounded_store_v6_lookup_observation::state::present &&
				observed.snapshot.publication && observed.snapshot.snapshot &&
				same_logical_publication(committed_publication, *observed.snapshot.publication) &&
				*observed.snapshot.snapshot == candidate_snapshot;
			const bool export_valid = observed.canonical_export_digest &&
				!observed.canonical_export_error &&
				*observed.canonical_export_digest == candidate_snapshot.canonical_export_digest;
			if (!observed.factory_attempted || observed.factory_error ||
				observed.fresh_backend_binding.empty() || !parent_valid || !current_valid ||
				!publication_valid || !snapshot_valid || !export_valid)
				return unexpected(invariant("reopen", "verification-mismatch"));
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("reopen", exception.what()));
		}
		catch (...)
		{
			return unexpected(invariant("reopen", "non-standard-exception"));
		}
	}

	result<bounded_store_v6_staging_session>
	bounded_store_v6_phase_core::begin_staging_session(bounded_store_v6_session_metadata metadata)
	{
		try
		{
			if (auto constants = validate_bounded_store_v6_product_constants(); !constants)
				return unexpected(std::move(constants.error()));
			if (!is_valid(metadata.backend))
				return unexpected(invariant("backend", "invalid-kind"));
			if (metadata.relation_engine_generation.empty())
				return unexpected(invariant("session", "missing-series-binding"));
			if (metadata.backend == bounded_store_v6_backend::sqlite)
			{
				if (!metadata.exact_sqlite_path || metadata.exact_sqlite_path->empty() ||
					metadata.exact_sqlite_path->front() != '/')
					return unexpected(invariant("database", "non-absolute-path"));
			}
			else if (metadata.exact_sqlite_path)
				return unexpected(invariant("database", "memory-path"));
			if (auto valid = validate_head(metadata.expected_head, metadata.selector); !valid)
				return unexpected(std::move(valid.error()));
			if (metadata.staging_session_id.empty())
			{
				static std::atomic<std::uint64_t> next_id{1U};
				metadata.staging_session_id = "staging-" + std::to_string(next_id.fetch_add(1U));
			}
			return bounded_store_v6_staging_session{
				std::make_unique<bounded_store_v6_staging_session::state>(
					bounded_store_v6_staging_session::state{std::move(metadata), false})};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("prepare", exception.what()));
		}
		catch (...)
		{
			return unexpected(invariant("prepare", "non-standard-exception"));
		}
	}

	result<bounded_store_sealed_input_binding>
	bounded_store_v6_phase_core::seal_input(bounded_store_v6_staging_session session,
											bounded_store_v6_external_census census)
	{
		try
		{
			if (!session.state_ || session.state_->consumed)
				return unexpected(invariant("input", "session-replayed"));
			if (auto valid = validate_external_census(census); !valid)
				return unexpected(std::move(valid.error()));
			if (auto valid = validate_head(session.state_->metadata.expected_head,
										   session.state_->metadata.selector);
				!valid)
				return unexpected(std::move(valid.error()));
			const auto metadata = session.state_->metadata;
			session.state_->consumed = true;
			auto authority = std::make_shared<input_authority>();
			authority->metadata = metadata;
			authority->census = std::move(census);
			authority->observation = {
				metadata.staging_session_id, metadata.expected_head, authority->census};
			return bounded_store_sealed_input_binding{
				std::make_unique<bounded_store_sealed_input_binding::state>(
					bounded_store_sealed_input_binding::state{std::move(authority)})};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("stage", exception.what()));
		}
		catch (...)
		{
			return unexpected(invariant("stage", "non-standard-exception"));
		}
	}

	result<bounded_store_v6_sealed_task> bounded_store_v6_phase_core::seal_task_source(
		bounded_store_v6_task_receipt receipt,
		std::unique_ptr<bounded_store_v6_task_frame_source> source)
	{
		try
		{
			if (!source)
				return unexpected(invariant("task", "missing-source"));
			if (auto valid = validate_task_receipt(receipt); !valid)
				return unexpected(std::move(valid.error()));
			auto state = std::make_unique<bounded_store_v6_sealed_task::state>();
			state->receipt = std::move(receipt);
			state->source = std::move(source);
			return bounded_store_v6_sealed_task{std::move(state)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("task", exception.what()));
		}
		catch (...)
		{
			return unexpected(invariant("task", "non-standard-exception"));
		}
	}

	result<bounded_store_prepared_publication> bounded_store_v6_phase_core::prepare_publication(
		bounded_store_sealed_input_binding input,
		std::unique_ptr<bounded_store_v6_backend_port> backend)
	{
		try
		{
			if (!input.state_ || !input.state_->authority || !backend)
				return unexpected(invariant("prepare", "missing-input-or-backend"));
			if (backend->backend() != input.state_->authority->metadata.backend)
				return unexpected(invariant("prepare", "backend-mismatch"));
			struct backend_cleanup_guard final
			{
				std::unique_ptr<bounded_store_v6_backend_port>& port;
				bool transferred{};
				~backend_cleanup_guard() noexcept
				{
					if (transferred || !port)
						return;
					try
					{
						(void)port->abort_staging();
					}
					catch (...)
					{
					}
				}
			} cleanup{backend};
			static std::atomic<std::uint64_t> next_anchor{1U};
			auto anchor = std::make_shared<bounded_store_v6_physical_anchor>();
			anchor->nonce = next_anchor.fetch_add(1U);
			if (auto bound = backend->bind_physical_anchor(anchor); !bound)
				return unexpected(std::move(bound.error()));
			if (!same_anchor(anchor, backend->physical_anchor()) ||
				backend->physical_anchor_binding().empty())
				return unexpected(invariant("prepare", "missing-physical-anchor"));
			auto state = std::make_shared<backend_state>();
			state->anchor = std::move(anchor);
			state->anchor_binding = std::string{backend->physical_anchor_binding()};
			state->port = std::shared_ptr<bounded_store_v6_backend_port>{std::move(backend)};
			cleanup.transferred = true;
			auto prepared_state = std::make_unique<bounded_store_prepared_publication::state>();
			prepared_state->input = input.state_->authority;
			prepared_state->backend = std::move(state);
			prepared_state->observation.backend = prepared_state->backend->port->backend();
			prepared_state->observation.exact_sqlite_path =
				prepared_state->input->metadata.exact_sqlite_path;
			prepared_state->observation.relation_engine_generation =
				prepared_state->input->metadata.relation_engine_generation;
			prepared_state->observation.series_id = prepared_state->input->metadata.selector.id();
			prepared_state->observation.staging_session_id =
				prepared_state->input->metadata.staging_session_id;
			prepared_state->observation.expected_head =
				prepared_state->input->metadata.expected_head;
			prepared_state->observation.physical_anchor_binding =
				prepared_state->backend->anchor_binding;
			return bounded_store_prepared_publication{std::move(prepared_state)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("seal", exception.what()));
		}
		catch (...)
		{
			return unexpected(invariant("seal", "non-standard-exception"));
		}
	}

	result<void>
	bounded_store_v6_phase_core::stage_from_source(bounded_store_prepared_publication& prepared,
												   bounded_store_v6_sealed_task task)
	{
		try
		{
			if (!prepared.state_ || prepared.state_->sealed || !prepared.state_->backend ||
				!prepared.state_->backend->port || !task.state_ || task.state_->consumed ||
				!task.state_->source ||
				(prepared.state_->current_phase !=
					 bounded_store_prepared_publication::state::phase::created &&
				 prepared.state_->current_phase !=
					 bounded_store_prepared_publication::state::phase::staged))
				return unexpected(invariant("prepare", "not-appendable"));
			const auto& receipt = task.state_->receipt;
			const auto& external = prepared.state_->input->census;
			if (receipt.ordinal != prepared.state_->staged_census.source_count ||
				receipt.ordinal >= external.task_count ||
				prepared.state_->staged_task_ids.contains(receipt.task_id))
				return unexpected(invariant("task", "ordinal-or-identity"));
			auto admitted_bytes = checked_bounded_store_v6_aggregate_charge(
				prepared.state_->staged_census.framed_bytes, receipt.framed_bytes);
			const auto admitted_events =
				static_cast<__uint128_t>(prepared.state_->staged_census.event_count) +
				receipt.event_count;
			const auto admitted_partitions =
				static_cast<__uint128_t>(prepared.state_->staged_census.partition_begin_count) +
				receipt.partition_count;
			if (!admitted_bytes || *admitted_bytes > external.input_bytes ||
				admitted_events > external.event_count ||
				admitted_partitions > external.partition_count)
				return unexpected(admitted_bytes ? resource("task-census", "aggregate-limit")
												 : std::move(admitted_bytes.error()));
			prepared.state_->current_phase =
				bounded_store_prepared_publication::state::phase::staging;
			struct staging_guard final
			{
				bounded_store_prepared_publication::state& value;
				bool committed{};
				~staging_guard()
				{
					if (!committed)
						value.current_phase =
							bounded_store_prepared_publication::state::phase::aborted;
				}
			} guard{*prepared.state_};
			if (!prepared.state_->staged_task_ids.insert(receipt.task_id).second)
				return unexpected(invariant("task", "duplicate-identity"));
			task.state_->consumed = true;
			auto source = std::move(task.state_->source);
			std::array<std::byte, bounded_store_v6_record_buffer_bytes> buffer{};
			bool began{};
			bool ended{};
			std::vector<std::byte> previous_order_key;
			staged_projection_census task_census;
			for (;;)
			{
				auto next = source->next_record();
				if (!next)
					return unexpected(std::move(next.error()));
				if (!*next)
				{
					if (!ended)
						return unexpected(corrupt("projection", "missing-partition-end"));
					break;
				}
				const auto extent = **next;
				if (!is_valid(extent.kind))
					return unexpected(corrupt("projection", "unknown-kind"));
				auto frame_size = checked_bounded_store_v6_record_frame_bytes(extent.key_bytes,
																			  extent.payload_bytes);
				if (!frame_size || *frame_size != extent.framed_bytes ||
					extent.framed_bytes > bounded_store_v6_record_buffer_bytes)
					return unexpected(corrupt("projection", "frame-size"));
				if ((!began || ended) &&
					extent.kind != bounded_store_v6_record_kind::partition_begin)
					return unexpected(corrupt("projection", "missing-partition-begin"));
				if (extent.kind == bounded_store_v6_record_kind::partition_begin && began && !ended)
					return unexpected(corrupt("projection", "duplicate-partition-begin"));
				if (ended && extent.kind == bounded_store_v6_record_kind::partition_begin)
					previous_order_key.clear();

				// Charge the complete frame and its census slot before the first backend byte can
				// be observed.  The widened arithmetic prevents wraparound from becoming backend
				// I/O.
				auto prospective_bytes = checked_bounded_store_v6_aggregate_charge(
					prepared.state_->staged_census.framed_bytes, extent.framed_bytes);
				auto prospective_task_bytes = checked_bounded_store_v6_aggregate_charge(
					task_census.framed_bytes, extent.framed_bytes);
				const auto prospective_events =
					static_cast<__uint128_t>(prepared.state_->staged_census.event_count) + 1U;
				const auto prospective_task_events =
					static_cast<__uint128_t>(task_census.event_count) + 1U;
				if (!prospective_bytes || !prospective_task_bytes ||
					prospective_events > external.event_count ||
					prospective_task_events > receipt.event_count ||
					*prospective_task_bytes > receipt.framed_bytes)
					return unexpected(prospective_bytes && prospective_task_bytes
										  ? resource("events", "limit-exceeded")
										  : !prospective_bytes
										  ? std::move(prospective_bytes.error())
										  : std::move(prospective_task_bytes.error()));

				if (auto begin = prepared.state_->backend->port->begin_record(extent); !begin)
					return unexpected(std::move(begin.error()));
				sha256 hash;
				std::array<std::byte, 8U> length{};
				append_u64(length, static_cast<std::uint64_t>(frame_domain.size()));
				hash.update(length);
				hash.update(std::span<const std::byte>{
					reinterpret_cast<const std::byte*>(frame_domain.data()), frame_domain.size()});
				const auto projection_bytes = extent.framed_bytes - frame_checksum_bytes;
				append_u64(length, projection_bytes);
				hash.update(length);
				std::array<std::byte, frame_projection_prefix_bytes> prefix{};
				std::array<std::byte, frame_checksum_bytes> checksum{};
				std::vector<std::byte> body;
				body.reserve(static_cast<std::size_t>(extent.key_bytes + extent.payload_bytes));
				std::uint64_t consumed{};
				while (consumed < extent.framed_bytes)
				{
					const auto remaining = extent.framed_bytes - consumed;
					const auto request =
						static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
					auto read =
						source->read_record_bytes(std::span<std::byte>{buffer.data(), request});
					if (!read || *read == 0U || *read > request)
						return unexpected(corrupt("projection", "short-or-overrun-read"));
					if (auto appended = prepared.state_->backend->port->append_record_bytes(
							std::span<const std::byte>{buffer.data(), *read});
						!appended)
						return unexpected(std::move(appended.error()));
					prepared.state_->staged_census.binary_hash.update(
						std::span<const std::byte>{buffer.data(), *read});
					task_census.binary_hash.update(
						std::span<const std::byte>{buffer.data(), *read});
					for (std::size_t index{}; index < *read; ++index)
					{
						const auto offset = consumed + index;
						if (offset < prefix.size())
							prefix[static_cast<std::size_t>(offset)] = buffer[index];
						if (offset < projection_bytes)
							hash.update(std::span<const std::byte>{buffer.data() + index, 1U});
						else
							checksum[static_cast<std::size_t>(offset - projection_bytes)] =
								buffer[index];
						if (offset >= prefix.size() && offset < projection_bytes)
							body.push_back(buffer[index]);
					}
					if (!checked_add(consumed, *read, consumed))
						return unexpected(resource("record-bytes", "checked-overflow"));
				}
				if (static_cast<std::uint8_t>(prefix[0U]) !=
						static_cast<std::uint8_t>(extent.kind) ||
					read_u64(prefix, 1U) != extent.key_bytes ||
					read_u64(prefix, 9U) != extent.payload_bytes ||
					!bytes_equal(hash.finish(), checksum))
					return unexpected(corrupt("projection", "checksum-or-header"));
				auto order_key = canonical_frame_order_key(
					extent.kind, body, extent.key_bytes, extent.payload_bytes);
				if (!order_key)
					return unexpected(std::move(order_key.error()));
				if (!previous_order_key.empty() &&
					!std::lexicographical_compare(previous_order_key.begin(),
												  previous_order_key.end(),
												  order_key->begin(),
												  order_key->end()))
					return unexpected(corrupt("projection-order", "reordered-or-duplicate"));
				previous_order_key = std::move(*order_key);
				if (auto finished = prepared.state_->backend->port->finish_record(); !finished)
					return unexpected(std::move(finished.error()));
				began = began || extent.kind == bounded_store_v6_record_kind::partition_begin;
				ended = extent.kind == bounded_store_v6_record_kind::partition_end;
				prepared.state_->staged_census.event_count =
					static_cast<std::uint64_t>(prospective_events);
				prepared.state_->staged_census.framed_bytes = *prospective_bytes;
				task_census.event_count = static_cast<std::uint64_t>(prospective_task_events);
				task_census.framed_bytes = *prospective_task_bytes;
				auto& census = prepared.state_->staged_census;
				switch (extent.kind)
				{
					case bounded_store_v6_record_kind::partition_begin:
						++census.partition_begin_count;
						break;
					case bounded_store_v6_record_kind::claim_occurrence:
						++census.claim_count;
						break;
					case bounded_store_v6_record_kind::detached_row:
						++census.row_count;
						break;
					case bounded_store_v6_record_kind::claim_annotation:
						++census.annotation_count;
						break;
					case bounded_store_v6_record_kind::coverage:
						++census.coverage_count;
						break;
					case bounded_store_v6_record_kind::unresolved:
						++census.unresolved_count;
						break;
					case bounded_store_v6_record_kind::partition_end:
						++census.partition_end_count;
						break;
				}
				auto& local = task_census;
				switch (extent.kind)
				{
					case bounded_store_v6_record_kind::partition_begin:
						++local.partition_begin_count;
						break;
					case bounded_store_v6_record_kind::claim_occurrence:
						++local.claim_count;
						break;
					case bounded_store_v6_record_kind::detached_row:
						++local.row_count;
						break;
					case bounded_store_v6_record_kind::claim_annotation:
						++local.annotation_count;
						break;
					case bounded_store_v6_record_kind::coverage:
						++local.coverage_count;
						break;
					case bounded_store_v6_record_kind::unresolved:
						++local.unresolved_count;
						break;
					case bounded_store_v6_record_kind::partition_end:
						++local.partition_end_count;
						break;
				}
			}
			if (!began || !ended)
				return unexpected(corrupt("projection", "unclosed-partition"));
			auto order = source->canonical_order_validated();
			if (!order || !*order)
				return unexpected(corrupt("projection", "expected-order"));
			const bool receipt_matches =
				task_census.partition_begin_count == receipt.partition_count &&
				task_census.partition_end_count == receipt.partition_count &&
				task_census.event_count == receipt.event_count &&
				task_census.claim_count == receipt.claim_count &&
				task_census.row_count == receipt.row_count &&
				task_census.annotation_count == receipt.annotation_count &&
				task_census.coverage_count == receipt.coverage_count &&
				task_census.unresolved_count == receipt.unresolved_count &&
				task_census.framed_bytes == receipt.framed_bytes &&
				bytes_equal(task_census.binary_hash.finish(), receipt.binary_sha256);
			if (!receipt_matches)
				return unexpected(corrupt("task", "receipt-mismatch"));
			update_task_receipt_hash(prepared.state_->staged_census.task_receipt_hash, receipt);
			++prepared.state_->staged_census.source_count;
			prepared.state_->current_phase =
				bounded_store_prepared_publication::state::phase::staged;
			guard.committed = true;
			return {};
		}
		catch (const std::bad_alloc&)
		{
			if (prepared.state_ && !prepared.state_->sealed)
				prepared.state_->current_phase =
					bounded_store_prepared_publication::state::phase::aborted;
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			if (prepared.state_ && !prepared.state_->sealed)
				prepared.state_->current_phase =
					bounded_store_prepared_publication::state::phase::aborted;
			return unexpected(invariant("expected", exception.what()));
		}
		catch (...)
		{
			if (prepared.state_ && !prepared.state_->sealed)
				prepared.state_->current_phase =
					bounded_store_prepared_publication::state::phase::aborted;
			return unexpected(invariant("task", "non-standard-exception"));
		}
	}

	result<void> bounded_store_v6_phase_core::seal_prepared_publication(
		bounded_store_prepared_publication& prepared)
	{
		try
		{
			if (!prepared.state_ || prepared.state_->sealed || !prepared.state_->backend ||
				!prepared.state_->backend->port ||
				prepared.state_->current_phase !=
					bounded_store_prepared_publication::state::phase::staged)
				return unexpected(invariant("prepare", "not-sealable"));
			auto& staged = prepared.state_->staged_census;
			const auto& external = prepared.state_->input->census;
			const bool census_matches = staged.source_count == external.task_count &&
				prepared.state_->staged_task_ids.size() == external.task_count &&
				staged.partition_begin_count == external.partition_count &&
				staged.partition_end_count == external.partition_count &&
				staged.event_count == external.event_count &&
				staged.claim_count == external.claim_count &&
				staged.row_count == external.row_count &&
				staged.annotation_count == external.annotation_count &&
				staged.coverage_count == external.coverage_count &&
				staged.unresolved_count == external.unresolved_count &&
				staged.framed_bytes == external.input_bytes &&
				bytes_equal(staged.binary_hash.finish(), external.binary_input_sha256);
			if (!census_matches)
			{
				prepared.state_->current_phase =
					bounded_store_prepared_publication::state::phase::aborted;
				return unexpected(corrupt("projection", "sealed-input-census"));
			}
			if (auto sealed = prepared.state_->backend->port->seal_staging(); !sealed)
			{
				prepared.state_->current_phase =
					bounded_store_prepared_publication::state::phase::aborted;
				return unexpected(std::move(sealed.error()));
			}
			auto measured = prepared.state_->backend->port->measured_projection();
			if (!measured)
			{
				prepared.state_->current_phase =
					bounded_store_prepared_publication::state::phase::aborted;
				return unexpected(std::move(measured.error()));
			}
			const auto& candidate_snapshot = measured->candidate_snapshot;
			if (measured->record_count != staged.event_count ||
				measured->framed_bytes != staged.framed_bytes ||
				!bytes_equal(measured->binary_sha256, staged.binary_hash.finish()) ||
				candidate_snapshot.snapshot_id.empty() ||
				candidate_snapshot.partition_count != external.partition_count ||
				candidate_snapshot.row_count != external.row_count ||
				candidate_snapshot.claim_count != external.claim_count ||
				candidate_snapshot.coverage_count != external.coverage_count ||
				candidate_snapshot.unresolved_count != external.unresolved_count ||
				!canonical_digest(candidate_snapshot.semantic_projection_digest) ||
				!canonical_digest(candidate_snapshot.canonical_export_digest) ||
				measured->immutable_binding.empty())
			{
				prepared.state_->current_phase =
					bounded_store_prepared_publication::state::phase::aborted;
				return unexpected(corrupt("projection", "measurement-mismatch"));
			}
			prepared.state_->backend->measured = *measured;
			prepared.state_->observation.candidate_id =
				candidate_identity(*prepared.state_->input,
								   *prepared.state_->backend,
								   prepared.state_->staged_census.task_receipt_hash.finish());
			prepared.state_->observation.candidate_snapshot_id =
				measured->candidate_snapshot.snapshot_id;
			prepared.state_->sealed = true;
			prepared.state_->current_phase =
				bounded_store_prepared_publication::state::phase::sealed;
			prepared.state_->backend->sealed = true;
			prepared.state_->backend->candidate_sealed = true;
			return {};
		}
		catch (const std::bad_alloc&)
		{
			if (prepared.state_ && !prepared.state_->sealed)
				prepared.state_->current_phase =
					bounded_store_prepared_publication::state::phase::aborted;
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			if (prepared.state_ && !prepared.state_->sealed)
				prepared.state_->current_phase =
					bounded_store_prepared_publication::state::phase::aborted;
			return unexpected(invariant("actual", exception.what()));
		}
		catch (...)
		{
			if (prepared.state_ && !prepared.state_->sealed)
				prepared.state_->current_phase =
					bounded_store_prepared_publication::state::phase::aborted;
			return unexpected(invariant("actual", "non-standard-exception"));
		}
	}

	result<bounded_store_v6_expected_projection>
	bounded_store_v6_phase_core::seal_expected_projection(
		std::unique_ptr<bounded_store_v6_expected_semantic_cursor> expected,
		const bounded_store_prepared_publication& prepared)
	{
		try
		{
			if (!expected || !prepared.state_ || !prepared.state_->sealed ||
				!prepared.state_->input)
				return unexpected(invariant("expected", "candidate-not-sealed"));
			auto state = std::make_unique<bounded_store_v6_expected_projection::state>();
			state->cursor = std::move(expected);
			state->authority = prepared.state_->input;
			state->candidate_id = prepared.state_->observation.candidate_id;
			return bounded_store_v6_expected_projection{std::move(state)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("compare", exception.what()));
		}
		catch (...)
		{
			return unexpected(invariant("compare", "non-standard-exception"));
		}
	}

	result<bounded_store_authenticated_actual_cursor>
	bounded_store_v6_phase_core::take_physical_actual_cursor(
		bounded_store_prepared_publication& prepared)
	{
		try
		{
			if (!prepared.state_ || !prepared.state_->sealed ||
				prepared.state_->actual_cursor_taken || !prepared.state_->backend ||
				!prepared.state_->backend->port)
				return unexpected(invariant("actual", "not-available"));
			auto cursor = prepared.state_->backend->port->open_actual_cursor();
			if (!cursor)
				return unexpected(std::move(cursor.error()));
			if (!*cursor ||
				!same_anchor(prepared.state_->backend->anchor,
							 prepared.state_->backend->port->physical_anchor()) ||
				!same_anchor(prepared.state_->backend->anchor, (*cursor)->physical_anchor()))
				return unexpected(invariant("actual", "anchor-mismatch"));
			prepared.state_->actual_cursor_taken = true;
			auto state = std::make_unique<bounded_store_authenticated_actual_cursor::state>();
			state->cursor = std::move(*cursor);
			state->anchor = prepared.state_->backend->anchor;
			state->anchor_binding = prepared.state_->backend->anchor_binding;
			return bounded_store_authenticated_actual_cursor{std::move(state)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("report", exception.what()));
		}
		catch (...)
		{
			return unexpected(invariant("report", "non-standard-exception"));
		}
	}

	result<bounded_store_projection_match>
	bounded_store_v6_phase_core::compare_bounded_store_projections(
		bounded_store_prepared_publication& prepared,
		bounded_store_v6_expected_projection expected,
		bounded_store_authenticated_actual_cursor actual)
	{
		try
		{
			if (!prepared.state_ || !prepared.state_->sealed || prepared.state_->compared ||
				!expected.state_ || expected.state_->consumed ||
				expected.state_->candidate_id != prepared.state_->observation.candidate_id ||
				!actual.state_ || actual.state_->finished ||
				!same_anchor(actual.state_->anchor, prepared.state_->backend->anchor))
				return unexpected(invariant("compare", "phase-or-anchor-mismatch"));
			expected_semantic_stream expected_stream{std::move(expected.state_->cursor)};
			physical_actual_stream actual_stream{std::move(actual.state_->cursor)};
			std::uint64_t record_count{};
			std::uint64_t framed_bytes{};
			sha256 expected_projection_hash;
			sha256 actual_projection_hash;
			for (;;)
			{
				if (auto valid = expected_stream.open_next(); !valid)
					return unexpected(std::move(valid.error()));
				if (auto valid = actual_stream.open_next(); !valid)
					return unexpected(std::move(valid.error()));
				if (expected_stream.eof || actual_stream.eof)
				{
					if (expected_stream.eof != actual_stream.eof)
						return unexpected(corrupt("projection", "eof-mismatch"));
					break;
				}
				if (!(expected_stream.extent == actual_stream.extent))
					return unexpected(corrupt("projection", "shape-mismatch"));
				for (std::uint64_t offset{}; offset < expected_stream.extent.framed_bytes; ++offset)
				{
					auto expected_byte = expected_stream.next_byte();
					if (!expected_byte)
						return unexpected(std::move(expected_byte.error()));
					auto actual_byte = actual_stream.next_byte();
					if (!actual_byte)
						return unexpected(std::move(actual_byte.error()));
					expected_projection_hash.update(
						std::span<const std::byte>{&*expected_byte, 1U});
					actual_projection_hash.update(std::span<const std::byte>{&*actual_byte, 1U});
					if (*expected_byte != *actual_byte)
						return unexpected(corrupt("projection", "byte-mismatch"));
				}
				if (auto valid = expected_stream.finish_record(); !valid)
					return unexpected(std::move(valid.error()));
				if (auto valid = actual_stream.finish_record(); !valid)
					return unexpected(std::move(valid.error()));
				if (!checked_increment(record_count) ||
					!checked_add(framed_bytes, expected_stream.extent.framed_bytes, framed_bytes) ||
					framed_bytes > bounded_store_v6_max_aggregate_bytes)
					return unexpected(resource("aggregate-bytes", "checked-overflow"));
			}
			if (record_count != prepared.state_->input->census.event_count ||
				framed_bytes != prepared.state_->backend->measured.framed_bytes)
				return unexpected(corrupt("projection", "event-count"));
			// Hash the exact framed bytes independently of the per-frame checksum hash.  The
			// canonical frame streams were compared byte-for-byte, so this is a compact full
			// projection witness.
			const auto expected_digest = expected_projection_hash.finish();
			const auto actual_digest = actual_projection_hash.finish();
			auto physical = actual_stream.cursor->finish();
			if (!physical)
				return unexpected(std::move(physical.error()));
			if (physical->record_count != record_count || physical->framed_bytes != framed_bytes ||
				!bytes_equal(physical->binary_sha256, actual_digest) ||
				!same_anchor(actual_stream.cursor->physical_anchor(),
							 prepared.state_->backend->anchor) ||
				physical->physical_anchor_binding != prepared.state_->backend->anchor_binding ||
				!physical->canonical_order_validated)
				return unexpected(corrupt("projection", "physical-finish-mismatch"));
			if (prepared.state_->backend->measured.record_count != record_count ||
				prepared.state_->backend->measured.framed_bytes != framed_bytes ||
				!bytes_equal(prepared.state_->backend->measured.binary_sha256, actual_digest))
				return unexpected(corrupt("projection", "measured-projection-mismatch"));
			auto state = std::make_unique<bounded_store_projection_match::state>();
			state->anchor = actual.state_->anchor;
			state->observation.staging_session_id = prepared.state_->observation.staging_session_id;
			state->observation.candidate_id = prepared.state_->observation.candidate_id;
			state->observation.candidate_snapshot_id =
				prepared.state_->observation.candidate_snapshot_id;
			state->observation.candidate_snapshot =
				prepared.state_->backend->measured.candidate_snapshot;
			state->observation.expected_head = prepared.state_->observation.expected_head;
			state->observation.external_census = prepared.state_->input->census;
			state->observation.record_count = record_count;
			state->observation.framed_bytes = framed_bytes;
			state->observation.expected_binary_sha256 = expected_digest;
			state->observation.actual_binary_sha256 = actual_digest;
			state->observation.physical_anchor_binding = prepared.state_->backend->anchor_binding;
			prepared.state_->compared = true;
			expected.state_->consumed = true;
			actual.state_->finished = true;
			return bounded_store_projection_match{std::move(state)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("bind", exception.what()));
		}
		catch (...)
		{
			return unexpected(invariant("bind", "non-standard-exception"));
		}
	}

	result<bounded_store_report_tail_custody> bounded_store_v6_phase_core::reserve_report_tail(
		bounded_store_prepared_publication& prepared,
		bounded_store_projection_match& match,
		std::unique_ptr<bounded_store_report_tail_writer> writer)
	{
		try
		{
			if (!prepared.state_ || !prepared.state_->sealed || !prepared.state_->compared ||
				!match.state_ || match.state_->consumed ||
				!same_anchor(match.state_->anchor, prepared.state_->backend->anchor) || !writer)
				return unexpected(invariant("report", "phase-or-anchor-mismatch"));
			struct report_release_guard final
			{
				std::unique_ptr<bounded_store_report_tail_writer>& writer;
				std::shared_ptr<backend_state> backend;
				bool transferred{};
				~report_release_guard() noexcept
				{
					if (transferred || !writer)
						return;
					try
					{
						auto released = writer->release();
						if (!released && backend)
							backend->retain_cleanup_failure(released.error());
					}
					catch (const std::exception& exception)
					{
						if (backend)
							backend->retain_cleanup_exception(exception.what());
					}
					catch (...)
					{
						if (backend)
							backend->retain_cleanup_exception("non-standard-exception");
					}
				}
			} release_guard{writer, prepared.state_->backend};
			auto reservation = writer->reserve_maximum_tail(
				bounded_store_v6_exact_report_tail_bytes, bounded_store_v6_max_report_bytes);
			if (!reservation)
				return unexpected(std::move(reservation.error()));
			if (auto valid = validate_reservation(*reservation); !valid)
				return unexpected(std::move(valid.error()));
			bounded_store_report_tail_custody_observation observation;
			observation.staging_session_id = prepared.state_->observation.staging_session_id;
			observation.candidate_id = prepared.state_->observation.candidate_id;
			observation.candidate_snapshot = match.state_->observation.candidate_snapshot;
			observation.expected_head = prepared.state_->observation.expected_head;
			observation.prefix_bytes = reservation->prefix_bytes;
			observation.reserved_tail_bytes = reservation->reserved_tail_bytes;
			observation.capacity_bytes = reservation->capacity_bytes;
			observation.maximum_report_bytes = reservation->maximum_report_bytes;
			observation.writer_object_binding = reservation->writer_object_binding;
			observation.spool_object_binding = reservation->spool_object_binding;
			observation.reservation_binding = reservation->reservation_binding;
			observation.physical_anchor_binding = prepared.state_->backend->anchor_binding;
			observation.live = true;
			auto state = std::make_unique<bounded_store_report_tail_custody::state>();
			state->backend = prepared.state_->backend;
			state->anchor = prepared.state_->backend->anchor;
			state->observation = std::move(observation);
			state->writer = std::shared_ptr<bounded_store_report_tail_writer>{std::move(writer)};
			release_guard.transferred = true;
			match.state_->consumed = true;
			return bounded_store_report_tail_custody{std::move(state)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("terminal", exception.what()));
		}
		catch (...)
		{
			return unexpected(invariant("terminal", "non-standard-exception"));
		}
	}

	result<bounded_store_validated_publication>
	bounded_store_v6_phase_core::bind_publication(bounded_store_prepared_publication prepared,
												  bounded_store_projection_match match,
												  bounded_store_report_tail_custody report_custody)
	{
		try
		{
			if (!prepared.state_ || !prepared.state_->sealed || !prepared.state_->compared ||
				!match.state_ || !report_custody.state_ || report_custody.state_->consumed ||
				!same_anchor(match.state_->anchor, prepared.state_->backend->anchor) ||
				!same_anchor(report_custody.state_->anchor, prepared.state_->backend->anchor) ||
				match.state_->observation.candidate_id !=
					prepared.state_->observation.candidate_id ||
				report_custody.state_->observation.candidate_id !=
					prepared.state_->observation.candidate_id ||
				report_custody.state_->observation.candidate_snapshot !=
					match.state_->observation.candidate_snapshot)
				return unexpected(invariant("bind", "identity-mismatch"));
			bounded_store_v6_terminal_observation identity;
			identity.backend = prepared.state_->observation.backend;
			identity.staging_session_id = prepared.state_->observation.staging_session_id;
			identity.candidate_id = prepared.state_->observation.candidate_id;
			identity.candidate_snapshot_id = prepared.state_->observation.candidate_snapshot_id;
			identity.candidate_snapshot = match.state_->observation.candidate_snapshot;
			identity.expected_head = prepared.state_->observation.expected_head;
			identity.series_id = prepared.state_->observation.series_id;
			identity.physical_anchor_binding = prepared.state_->observation.physical_anchor_binding;
			identity.physical_binary_sha256 = match.state_->observation.actual_binary_sha256;
			identity.terminal = bounded_store_v6_publication_terminal::not_attempted;
			auto state = std::make_unique<bounded_store_validated_publication::state>();
			state->backend = prepared.state_->backend;
			state->anchor = prepared.state_->backend->anchor;
			state->identity = std::move(identity);
			state->report = report_custody.state_->observation;
			state->writer = report_custody.state_->writer;
			report_custody.state_->consumed = true;
			return bounded_store_validated_publication{std::move(state)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("report", exception.what()));
		}
		catch (...)
		{
			return unexpected(invariant("report", "non-standard-exception"));
		}
	}

	result<bounded_store_terminal_custody> bounded_store_v6_phase_core::capture_not_attempted(
		bounded_store_prepared_publication prepared,
		bounded_store_report_tail_custody report_custody)
	{
		try
		{
			if (!prepared.state_ || !prepared.state_->sealed || !prepared.state_->compared ||
				!report_custody.state_ || report_custody.state_->consumed ||
				!same_anchor(report_custody.state_->anchor, prepared.state_->backend->anchor))
				return unexpected(invariant("terminal", "not-attempted-precondition"));
			bounded_store_v6_terminal_observation observation;
			observation.backend = prepared.state_->observation.backend;
			observation.staging_session_id = prepared.state_->observation.staging_session_id;
			observation.candidate_id = prepared.state_->observation.candidate_id;
			observation.candidate_snapshot_id = prepared.state_->observation.candidate_snapshot_id;
			observation.candidate_snapshot = report_custody.state_->observation.candidate_snapshot;
			observation.expected_head = prepared.state_->observation.expected_head;
			observation.series_id = prepared.state_->observation.series_id;
			observation.physical_anchor_binding =
				prepared.state_->observation.physical_anchor_binding;
			observation.physical_binary_sha256 = prepared.state_->backend->measured.binary_sha256;
			observation.terminal = bounded_store_v6_publication_terminal::not_attempted;
			observation.reopen.factory_attempted = false;
			auto state = std::make_unique<bounded_store_terminal_custody::state>();
			state->backend = prepared.state_->backend;
			state->anchor = prepared.state_->backend->anchor;
			state->report = report_custody.state_->observation;
			state->observation = std::move(observation);
			state->writer = report_custody.state_->writer;
			report_custody.state_->consumed = true;
			return bounded_store_terminal_custody{std::move(state)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("terminal", exception.what()));
		}
		catch (...)
		{
			return unexpected(invariant("terminal", "non-standard-exception"));
		}
	}

	result<bounded_store_terminal_custody>
	bounded_store_v6_phase_core::publish_once(bounded_store_validated_publication publication)
	{
		if (!publication.state_ || publication.state_->consumed || !publication.state_->backend ||
			!publication.state_->backend->port)
			return unexpected(invariant("publish", "not-ready"));

		std::unique_ptr<bounded_store_terminal_custody::state> state;
		try
		{
			state = std::make_unique<bounded_store_terminal_custody::state>();
			state->backend = publication.state_->backend;
			state->anchor = publication.state_->anchor;
			state->report = publication.state_->report;
			state->observation = publication.state_->identity;
			state->observation.publish_call_count = 1U;
			state->observation.publication_attempted = true;
			// Install the conservative terminal before the backend can perform an effect.  Every
			// path after the following consume returns this custody, even if allocation or
			// observation fails.
			state->observation.terminal =
				bounded_store_v6_publication_terminal::committed_unverified;
			state->writer = publication.state_->writer;
			publication.state_->consumed = true;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("publish", exception.what()));
		}
		catch (...)
		{
			return unexpected(invariant("publish", "non-standard-exception"));
		}

		const auto retain_error = [&](const error& value) noexcept
		{
			try
			{
				state->observation.sdk_error = value;
			}
			catch (...)
			{
			}
		};
		const auto retain_backend_call_error = [&](const error& value) noexcept
		{
			try
			{
				state->observation.backend_call_error = value;
			}
			catch (...)
			{
			}
		};
		const auto expected_publication_id = [&]() -> std::string_view
		{
			const auto& head = state->observation.expected_head;
			return head.publication ? std::string_view{head.publication->publication_id}
									: std::string_view{};
		}();
		const auto classify = [&](const error& value) noexcept
		{
			return classify_bounded_store_v6_error(state->observation.backend,
												   state->observation.series_id,
												   state->observation.candidate_snapshot_id,
												   expected_publication_id,
												   value);
		};
		bounded_store_v6_effect_result effect;
		try
		{
			auto result = state->backend->port->publish_once();
			if (!result)
			{
				// Once the effect-capable backend call begins, an outer result error cannot prove
				// that no commit happened.  Preserve terminal/report/backend custody and the exact
				// outer error without fabricating an SDK tuple or a retryable normal outcome.
				try
				{
					state->observation.backend_call_error = result.error();
				}
				catch (...)
				{
				}
				state->observation.verification_failure =
					bounded_store_v6_verification_failure::local_verification_unavailable;
				return bounded_store_terminal_custody{std::move(state)};
			}
			effect = std::move(*result);
		}
		catch (const std::bad_alloc&)
		{
			retain_backend_call_error(allocation_failure());
			state->observation.verification_failure =
				bounded_store_v6_verification_failure::local_verification_unavailable;
			return bounded_store_terminal_custody{std::move(state)};
		}
		catch (const std::exception& exception)
		{
			retain_backend_call_error(
				exception_failure(state->observation.backend, exception.what()));
			state->observation.verification_failure =
				bounded_store_v6_verification_failure::local_verification_unavailable;
			return bounded_store_terminal_custody{std::move(state)};
		}
		catch (...)
		{
			retain_backend_call_error(
				exception_failure(state->observation.backend, "non-standard-exception"));
			state->observation.verification_failure =
				bounded_store_v6_verification_failure::local_verification_unavailable;
			return bounded_store_terminal_custody{std::move(state)};
		}

		state->observation.publication_attempted = effect.operation_attempted;
		if (effect.failure)
		{
			const auto classification = classify(*effect.failure);
			if (!effect.operation_attempted ||
				((classification == bounded_store_v6_error_class::stale_parent ||
				  classification == bounded_store_v6_error_class::corrupt_store) &&
				 effect.effect_may_have_occurred) ||
				classification == bounded_store_v6_error_class::invariant_breach ||
				classification == bounded_store_v6_error_class::resource_limit)
			{
				retain_backend_call_error(*effect.failure);
				state->observation.verification_failure =
					bounded_store_v6_verification_failure::local_verification_unavailable;
				return bounded_store_terminal_custody{std::move(state)};
			}
			retain_error(*effect.failure);
			if (classification == bounded_store_v6_error_class::stale_parent)
				state->observation.terminal = bounded_store_v6_publication_terminal::rejected_stale;
			else if (classification == bounded_store_v6_error_class::sqlite_failure)
				state->observation.terminal =
					bounded_store_v6_publication_terminal::publication_outcome_unknown;
			else if (classification == bounded_store_v6_error_class::corrupt_store)
				state->observation.terminal =
					bounded_store_v6_publication_terminal::rejected_store_failure;
			return bounded_store_terminal_custody{std::move(state)};
		}

		try
		{
			state->observation.returned_publication = std::move(effect.publication);
		}
		catch (...)
		{
			state->observation.verification_failure =
				bounded_store_v6_verification_failure::local_verification_unavailable;
			return bounded_store_terminal_custody{std::move(state)};
		}
		if (!effect.operation_attempted || !effect.effect_may_have_occurred ||
			!state->observation.returned_publication || !effect.returned_handle)
		{
			// A successful SDK return followed by an exact-record mismatch is a verification
			// failure, not an SDK failure.  Preserve committed_unverified without fabricating an
			// error tuple that the SDK never returned.
			state->observation.verification_failure =
				bounded_store_v6_verification_failure::returned_handle_missing;
			return bounded_store_terminal_custody{std::move(state)};
		}
		const auto& returned = *state->observation.returned_publication;
		if (returned.state != publication_state::committed || returned.corrupt ||
			returned.series_id != state->observation.series_id ||
			returned.snapshot_id != state->observation.candidate_snapshot_id)
		{
			state->observation.verification_failure =
				bounded_store_v6_verification_failure::returned_publication_fields;
			return bounded_store_terminal_custody{std::move(state)};
		}

		try
		{
			auto identity = publication_record_identity(returned.series_id,
														returned.snapshot_id,
														returned.sequence,
														returned.parent_publication);
			const auto expected_parent = state->observation.expected_head.publication
				? std::optional<std::string>{state->observation.expected_head.publication
												 ->publication_id}
				: std::optional<std::string>{};
			const auto prior_sequence = state->observation.expected_head.publication
				? state->observation.expected_head.publication->sequence
				: 0U;
			const auto prior_generation = state->observation.expected_head.publication
				? state->observation.expected_head.publication->physical_generation
				: 0U;
			if (!identity || *identity != returned.publication_id)
			{
				state->observation.verification_failure =
					bounded_store_v6_verification_failure::publication_identity;
				return bounded_store_terminal_custody{std::move(state)};
			}
			if (returned.parent_publication != expected_parent)
			{
				state->observation.verification_failure =
					bounded_store_v6_verification_failure::parent_publication;
				return bounded_store_terminal_custody{std::move(state)};
			}
			if (prior_sequence == std::numeric_limits<std::uint64_t>::max() ||
				returned.sequence != prior_sequence + 1U)
			{
				state->observation.verification_failure =
					bounded_store_v6_verification_failure::publication_sequence;
				return bounded_store_terminal_custody{std::move(state)};
			}
			if (prior_generation == std::numeric_limits<std::uint64_t>::max() ||
				returned.physical_generation <= prior_generation)
			{
				state->observation.verification_failure =
					bounded_store_v6_verification_failure::physical_generation;
				return bounded_store_terminal_custody{std::move(state)};
			}
		}
		catch (const std::bad_alloc&)
		{
			state->observation.verification_failure =
				bounded_store_v6_verification_failure::local_verification_unavailable;
			return bounded_store_terminal_custody{std::move(state)};
		}

		try
		{
			auto reopened = state->backend->port->reopen();
			if (!reopened)
			{
				state->observation.reopen.factory_attempted = true;
				try
				{
					state->observation.reopen.factory_error = reopened.error();
				}
				catch (...)
				{
				}
				state->observation.verification_failure =
					bounded_store_v6_verification_failure::reopen_observation;
				return bounded_store_terminal_custody{std::move(state)};
			}
			state->observation.reopen = std::move(*reopened);
			const auto& observed = state->observation.reopen;
			auto verified =
				validate_bounded_store_v6_reopen_observation(state->observation.expected_head,
															 returned,
															 state->observation.candidate_snapshot,
															 observed);
			if (!verified)
			{
				state->observation.verification_failure =
					bounded_store_v6_verification_failure::reopen_observation;
				return bounded_store_terminal_custody{std::move(state)};
			}
			state->observation.returned_snapshot = *observed.snapshot.snapshot;
			state->observation.returned_export_digest = *observed.canonical_export_digest;
			state->observation.terminal = bounded_store_v6_publication_terminal::committed_verified;
		}
		catch (const std::bad_alloc&)
		{
			state->observation.verification_failure =
				bounded_store_v6_verification_failure::local_verification_unavailable;
		}
		catch (const std::exception&)
		{
			state->observation.verification_failure =
				bounded_store_v6_verification_failure::local_verification_unavailable;
		}
		catch (...)
		{
			state->observation.verification_failure =
				bounded_store_v6_verification_failure::local_verification_unavailable;
		}
		return bounded_store_terminal_custody{std::move(state)};
	}

	result<bounded_store_full_report_release>
	bounded_store_v6_phase_core::finalize_and_validate_report(
		bounded_store_terminal_custody& terminal)
	{
		try
		{
			if (!terminal.state_ || terminal.state_->report_finalization_attempted ||
				terminal.state_->report_finalized || !terminal.state_->writer ||
				!terminal.state_->report.live)
				return unexpected(invariant("report", "not-live"));
			auto state = std::make_unique<bounded_store_full_report_release::state>();
			state->anchor = terminal.state_->anchor;
			state->terminal_binding =
				digest_binding(terminal.state_->observation.staging_session_id,
							   terminal.state_->observation.candidate_id,
							   terminal.state_->observation.physical_anchor_binding);
			state->observation.staging_session_id = terminal.state_->observation.staging_session_id;
			state->observation.candidate_id = terminal.state_->observation.candidate_id;
			state->observation.candidate_snapshot = terminal.state_->observation.candidate_snapshot;
			state->observation.expected_head = terminal.state_->observation.expected_head;
			state->observation.physical_anchor_binding =
				terminal.state_->observation.physical_anchor_binding;
			state->observation.writer_object_binding =
				terminal.state_->report.writer_object_binding;
			state->observation.spool_object_binding = terminal.state_->report.spool_object_binding;
			state->observation.reservation_binding = terminal.state_->report.reservation_binding;
			state->observation.sealed_report_binding.reserve(71U);
			state->observation.section_count = bounded_store_v6_report_section_count;
			state->observation.terminal = terminal.state_->observation.terminal;
			terminal.state_->report_finalization_attempted = true;
			if (auto appended =
					terminal.state_->writer->append_terminal(terminal.state_->observation.terminal);
				!appended)
				return unexpected(std::move(appended.error()));
			if (auto schema = terminal.state_->writer->validate_full_schema(); !schema)
				return unexpected(std::move(schema.error()));
			if (auto census = terminal.state_->writer->validate_complete_section_census(
					bounded_store_v6_report_section_count);
				!census)
				return unexpected(std::move(census.error()));
			if (auto bottom_up = terminal.state_->writer->validate_bottom_up_bindings(); !bottom_up)
				return unexpected(std::move(bottom_up.error()));
			auto bytes = terminal.state_->writer->sealed_report_bytes();
			if (!bytes)
				return unexpected(std::move(bytes.error()));
			if (*bytes > terminal.state_->report.capacity_bytes ||
				*bytes < terminal.state_->report.prefix_bytes)
				return unexpected(resource("report", "size"));
			state->observation.sealed_report_binding =
				digest_binding(terminal.state_->report.reservation_binding, std::to_string(*bytes));
			state->observation.report_bytes = *bytes;
			state->observation.full_schema_validated = true;
			state->observation.complete_section_census_validated = true;
			state->observation.bottom_up_cross_bindings_validated = true;
			terminal.state_->report_finalized = true;
			return bounded_store_full_report_release{std::move(state)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("report", exception.what()));
		}
		catch (...)
		{
			return unexpected(invariant("report", "non-standard-exception"));
		}
	}

	result<bounded_store_v6_cleanup_observation>
	bounded_store_v6_phase_core::drain(bounded_store_terminal_custody& terminal,
									   bounded_store_full_report_release release)
	{
		bounded_store_v6_cleanup_observation rejected;
		try
		{
			if (!terminal.state_ || !release.state_ || release.state_->consumed ||
				release.state_->drain_attempted || !terminal.state_->backend ||
				!terminal.state_->backend->port || !terminal.state_->writer ||
				terminal.state_->cleanup.attempted ||
				!same_anchor(terminal.state_->anchor, release.state_->anchor) ||
				release.state_->terminal_binding !=
					digest_binding(terminal.state_->observation.staging_session_id,
								   terminal.state_->observation.candidate_id,
								   terminal.state_->observation.physical_anchor_binding) ||
				release.state_->observation.staging_session_id !=
					terminal.state_->observation.staging_session_id ||
				release.state_->observation.candidate_id !=
					terminal.state_->observation.candidate_id ||
				release.state_->observation.expected_head !=
					terminal.state_->observation.expected_head ||
				release.state_->observation.candidate_snapshot !=
					terminal.state_->observation.candidate_snapshot ||
				release.state_->observation.physical_anchor_binding !=
					terminal.state_->observation.physical_anchor_binding ||
				release.state_->observation.writer_object_binding !=
					terminal.state_->report.writer_object_binding ||
				release.state_->observation.spool_object_binding !=
					terminal.state_->report.spool_object_binding ||
				release.state_->observation.reservation_binding !=
					terminal.state_->report.reservation_binding ||
				release.state_->observation.terminal != terminal.state_->observation.terminal ||
				!terminal.state_->report_finalized)
			{
				rejected.failure = invariant("cleanup", "release-mismatch");
				return rejected;
			}

			// Latch every one-shot authority before either physical cleanup call.  A valid release
			// is consumed even when one or both cleanup operations fail; neither operation may be
			// retried.
			release.state_->drain_attempted = true;
			release.state_->consumed = true;
			auto& cleanup = terminal.state_->cleanup;
			cleanup.attempted = true;
			cleanup.report_release_attempted = true;
			terminal.state_->writer_release_attempted = true;
			terminal.state_->report.live = false;
			result<void> released;
			try
			{
				released = terminal.state_->writer->release();
			}
			catch (const std::bad_alloc&)
			{
				released = unexpected(allocation_failure());
			}
			catch (const std::exception& exception)
			{
				released = unexpected(invariant("report-cleanup", exception.what()));
			}
			catch (...)
			{
				released = unexpected(invariant("report-cleanup", "non-standard-exception"));
			}

			// Backend custody must be drained even when report release failed.  This ordering
			// prevents a report-writer fault from retaining a full staging payload.
			cleanup.backend_cleanup_attempted = true;
			terminal.state_->backend->cleanup_called = true;
			result<void> aborted;
			try
			{
				aborted = terminal.state_->backend->port->abort_staging();
			}
			catch (const std::bad_alloc&)
			{
				aborted = unexpected(allocation_failure());
			}
			catch (const std::exception& exception)
			{
				aborted = unexpected(invariant("backend-cleanup", exception.what()));
			}
			catch (...)
			{
				aborted = unexpected(invariant("backend-cleanup", "non-standard-exception"));
			}

			cleanup.report_released = released.has_value();
			cleanup.backend_cleanup_drained = aborted.has_value();
			if (!released)
			{
				cleanup.report_failure = released.error();
				terminal.state_->backend->retain_cleanup_failure(released.error());
			}
			if (!aborted)
			{
				cleanup.backend_failure = aborted.error();
				terminal.state_->backend->retain_cleanup_failure(aborted.error());
			}
			if (cleanup.report_failure)
				cleanup.failure = cleanup.report_failure;
			else if (cleanup.backend_failure)
				cleanup.failure = cleanup.backend_failure;
			cleanup.drained = cleanup.report_released && cleanup.backend_cleanup_drained;
			terminal.state_->cleanup_drained = cleanup.drained;
			return cleanup;
		}
		catch (const std::bad_alloc&)
		{
			try
			{
				if (terminal.state_ && terminal.state_->cleanup.attempted)
				{
					terminal.state_->cleanup.failure = allocation_failure();
					return terminal.state_->cleanup;
				}
				rejected.failure = allocation_failure();
			}
			catch (...)
			{
			}
			return rejected;
		}
		catch (const std::exception& exception)
		{
			const auto value = exception_failure(terminal.state_ && terminal.state_->backend &&
														 terminal.state_->backend->port
													 ? terminal.state_->backend->port->backend()
													 : bounded_store_v6_backend::memory,
												 exception.what());
			if (terminal.state_ && terminal.state_->cleanup.attempted)
			{
				terminal.state_->cleanup.failure = value;
				return terminal.state_->cleanup;
			}
			rejected.failure = value;
			return rejected;
		}
		catch (...)
		{
			try
			{
				const auto value = exception_failure(terminal.state_ && terminal.state_->backend &&
															 terminal.state_->backend->port
														 ? terminal.state_->backend->port->backend()
														 : bounded_store_v6_backend::memory,
													 "non-standard-exception");
				if (terminal.state_ && terminal.state_->cleanup.attempted)
				{
					terminal.state_->cleanup.failure = value;
					return terminal.state_->cleanup;
				}
				rejected.failure = value;
			}
			catch (...)
			{
			}
			return rejected;
		}
	}
} // namespace cxxlens::sdk::detail
