#include "materialization_partition_event_stream.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		constexpr std::string_view stream_magic{"CXLPEV01"};
		constexpr std::string_view trailer_magic{"CXLPEEND"};
		constexpr std::string_view sequence_domain{
			"cxxlens/df-0200-partition-event-stream-sequence/v1"};
		constexpr std::string_view frame_domain{"cxxlens/df-0200-partition-event-frame/v1"};
		constexpr std::string_view frames_domain{"cxxlens/df-0200-partition-event-frames/v1"};
		constexpr std::string_view prefix_domain{
			"cxxlens/df-0200-partition-event-stream-prefix/v1"};

		[[nodiscard]] sdk::error event_error(const std::string_view field,
											 const std::string_view detail)
		{
			return {
				"materialization.partition-event-invalid", std::string{field}, std::string{detail}};
		}

		[[nodiscard]] sdk::error spool_error(const std::string_view field,
											 const std::string_view detail)
		{
			return {"materialization.partition-event-spool-failure",
					std::string{field},
					std::string{detail}};
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

		[[nodiscard]] bool checked_mul8(const std::uint64_t value, std::uint64_t& output) noexcept
		{
			if (value > std::numeric_limits<std::uint64_t>::max() / 8U)
				return false;
			output = value * 8U;
			return true;
		}

		void append_u16(std::vector<std::byte>& output, const std::uint16_t value)
		{
			output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
			output.push_back(static_cast<std::byte>(value & 0xffU));
		}

		void append_u32(std::vector<std::byte>& output, const std::uint32_t value)
		{
			for (std::size_t shift = 24U;; shift -= 8U)
			{
				output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
				if (shift == 0U)
					break;
			}
		}

		void append_u64(std::vector<std::byte>& output, const std::uint64_t value)
		{
			for (std::size_t shift = 56U;; shift -= 8U)
			{
				output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
				if (shift == 0U)
					break;
			}
		}

		void append_ordinal(std::vector<std::byte>& output,
							const materialization_event_ordinal ordinal)
		{
			append_u64(output, ordinal.high);
			append_u64(output, ordinal.low);
		}

		void append_string(std::vector<std::byte>& output, const std::string_view value)
		{
			output.insert(output.end(),
						  reinterpret_cast<const std::byte*>(value.data()),
						  reinterpret_cast<const std::byte*>(value.data() + value.size()));
		}

		[[nodiscard]] std::uint16_t read_u16(const std::span<const std::byte> bytes,
											 const std::size_t offset)
		{
			const auto high = static_cast<std::uint16_t>(
				static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset])) << 8U);
			const auto low =
				static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset + 1U]));
			return static_cast<std::uint16_t>(high | low);
		}

		[[nodiscard]] std::uint32_t read_u32(const std::span<const std::byte> bytes,
											 const std::size_t offset)
		{
			std::uint32_t output{};
			for (std::size_t index{}; index < 4U; ++index)
				output = (output << 8U) |
					static_cast<std::uint32_t>(
							 std::to_integer<unsigned char>(bytes[offset + index]));
			return output;
		}

		[[nodiscard]] std::uint64_t read_u64(const std::span<const std::byte> bytes,
											 const std::size_t offset)
		{
			std::uint64_t output{};
			for (std::size_t index{}; index < 8U; ++index)
				output = (output << 8U) |
					static_cast<std::uint64_t>(
							 std::to_integer<unsigned char>(bytes[offset + index]));
			return output;
		}

		[[nodiscard]] materialization_event_ordinal
		read_ordinal(const std::span<const std::byte> bytes, const std::size_t offset)
		{
			return {read_u64(bytes, offset), read_u64(bytes, offset + 8U)};
		}

		[[nodiscard]] bool bytes_equal(const std::span<const std::byte> left,
									   const std::string_view right) noexcept
		{
			return left.size() == right.size() &&
				std::equal(left.begin(),
						   left.end(),
						   right.begin(),
						   right.end(),
						   [](const std::byte value, const char expected)
						   {
							   return std::to_integer<unsigned char>(value) ==
								   static_cast<unsigned char>(expected);
						   });
		}

		[[nodiscard]] bool valid_kind(const std::uint8_t value) noexcept
		{
			return value >= 1U && value <= 7U;
		}

		[[nodiscard]] bool increment_ordinal(materialization_event_ordinal& ordinal,
											 const std::uint64_t amount) noexcept
		{
			if (amount > std::numeric_limits<std::uint64_t>::max() - ordinal.low)
			{
				const auto carry =
					amount - (std::numeric_limits<std::uint64_t>::max() - ordinal.low) - 1U;
				if (ordinal.high == std::numeric_limits<std::uint64_t>::max())
					return false;
				++ordinal.high;
				ordinal.low = carry;
				return true;
			}
			ordinal.low += amount;
			return true;
		}

		[[nodiscard]] std::vector<std::byte>
		frame_projection_bytes(const materialization_partition_event_kind kind,
							   const std::span<const std::byte> key,
							   const std::span<const std::byte> payload)
		{
			std::vector<std::byte> output;
			output.reserve(17U + key.size() + payload.size());
			output.push_back(static_cast<std::byte>(kind));
			append_u64(output, static_cast<std::uint64_t>(key.size()));
			append_u64(output, static_cast<std::uint64_t>(payload.size()));
			output.insert(output.end(), key.begin(), key.end());
			output.insert(output.end(), payload.begin(), payload.end());
			return output;
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		event_order_key(const std::uint8_t kind,
						const std::span<const std::byte> key,
						const std::span<const std::byte> payload)
		{
			std::vector<std::byte> kind_bytes{static_cast<std::byte>(kind)};
			auto output = sdk::canonical_binary(sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_bytes(std::move(kind_bytes)),
				sdk::canonical_value::from_bytes(std::vector<std::byte>{key.begin(), key.end()}),
				sdk::canonical_value::from_bytes(
					std::vector<std::byte>{payload.begin(), payload.end()}),
			}));
			if (!output)
				return sdk::unexpected(event_error("frame", "order-key"));
			return output;
		}

		[[nodiscard]] sdk::result<void>
		validate_canonical_tuple(const std::span<const std::byte> bytes,
								 const std::string_view field)
		{
			auto decoded = sdk::canonical_binary_decode(bytes);
			if (!decoded || decoded->type != sdk::canonical_value::kind::ordered_tuple)
				return sdk::unexpected(event_error(field, "canonical-ordered-tuple"));
			return {};
		}

		enum class event_field_kind : std::uint8_t
		{
			string,
			canonical_bytes,
			u64_canonical_bytes,
			ordered_unique_canonical_bytes,
		};

		[[nodiscard]] bool nonempty_string(const sdk::canonical_value& value) noexcept
		{
			return value.type == sdk::canonical_value::kind::utf8_string && !value.text.empty();
		}

		[[nodiscard]] bool canonical_bytes(const sdk::canonical_value& value)
		{
			if (value.type != sdk::canonical_value::kind::bytes)
				return false;
			return sdk::canonical_binary_decode(value.byte_string).has_value();
		}

		[[nodiscard]] bool u64_canonical_bytes(const sdk::canonical_value& value)
		{
			return value.type == sdk::canonical_value::kind::bytes &&
				value.byte_string.size() == sizeof(std::uint64_t);
		}

		[[nodiscard]] bool ordered_unique_canonical_bytes(const sdk::canonical_value& value)
		{
			if (value.type != sdk::canonical_value::kind::ordered_tuple)
				return false;
			std::vector<std::vector<std::byte>> encoded;
			encoded.reserve(value.tuple.size());
			for (const auto& item : value.tuple)
			{
				if (!canonical_bytes(item))
					return false;
				auto bytes = sdk::canonical_binary(item);
				if (!bytes)
					return false;
				encoded.push_back(std::move(*bytes));
			}
			return std::ranges::is_sorted(encoded) &&
				std::ranges::adjacent_find(encoded) == encoded.end();
		}

		[[nodiscard]] sdk::result<void>
		validate_event_fields(const sdk::canonical_value& tuple,
							  const std::initializer_list<event_field_kind> expected,
							  const std::string_view field)
		{
			if (tuple.type != sdk::canonical_value::kind::ordered_tuple ||
				tuple.tuple.size() != expected.size())
				return sdk::unexpected(event_error(field, "field-cardinality"));
			std::size_t index{};
			for (const auto expected_kind : expected)
			{
				const auto& value = tuple.tuple[index++];
				const bool valid = [&]
				{
					switch (expected_kind)
					{
						case event_field_kind::string:
							return nonempty_string(value);
						case event_field_kind::canonical_bytes:
							return canonical_bytes(value);
						case event_field_kind::u64_canonical_bytes:
							return u64_canonical_bytes(value);
						case event_field_kind::ordered_unique_canonical_bytes:
							return ordered_unique_canonical_bytes(value);
					}
					return false;
				}();
				if (!valid)
					return sdk::unexpected(event_error(field, "field-type-or-order"));
			}
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_event_projection(const materialization_partition_event_kind kind,
								  const std::span<const std::byte> key,
								  const std::span<const std::byte> payload)
		{
			auto decoded_key = sdk::canonical_binary_decode(key);
			auto decoded_payload = sdk::canonical_binary_decode(payload);
			if (!decoded_key || !decoded_payload)
				return sdk::unexpected(event_error("event", "canonical-decode"));
			using field = event_field_kind;
			switch (kind)
			{
				case materialization_partition_event_kind::partition_begin:
					if (auto valid = validate_event_fields(
							*decoded_key, {field::string, field::string}, "partition-begin.key");
						!valid)
						return valid;
					return validate_event_fields(*decoded_payload,
												 {field::string,
												  field::string,
												  field::string,
												  field::string,
												  field::string,
												  field::string,
												  field::string,
												  field::string},
												 "partition-begin.payload");
				case materialization_partition_event_kind::claim_occurrence:
					if (auto valid = validate_event_fields(*decoded_key,
														   {field::string,
															field::string,
															field::canonical_bytes,
															field::canonical_bytes},
														   "claim-occurrence.key");
						!valid)
						return valid;
					return validate_event_fields(*decoded_payload,
												 {field::canonical_bytes,
												  field::canonical_bytes,
												  field::ordered_unique_canonical_bytes,
												  field::ordered_unique_canonical_bytes,
												  field::ordered_unique_canonical_bytes,
												  field::ordered_unique_canonical_bytes},
												 "claim-occurrence.payload");
				case materialization_partition_event_kind::detached_row:
					if (auto valid = validate_event_fields(
							*decoded_key,
							{field::string, field::string, field::string, field::canonical_bytes},
							"detached-row.key");
						!valid)
						return valid;
					return validate_event_fields(
						*decoded_payload, {field::canonical_bytes}, "detached-row.payload");
				case materialization_partition_event_kind::claim_annotation:
					if (auto valid = validate_event_fields(
							*decoded_key,
							{field::string, field::string, field::string, field::canonical_bytes},
							"claim-annotation.key");
						!valid)
						return valid;
					return validate_event_fields(
						*decoded_payload, {field::canonical_bytes}, "claim-annotation.payload");
				case materialization_partition_event_kind::coverage:
					if (auto valid = validate_event_fields(
							*decoded_key,
							{field::string, field::string, field::canonical_bytes},
							"coverage.key");
						!valid)
						return valid;
					return validate_event_fields(
						*decoded_payload, {field::canonical_bytes}, "coverage.payload");
				case materialization_partition_event_kind::unresolved:
					if (auto valid = validate_event_fields(
							*decoded_key,
							{field::string, field::string, field::canonical_bytes},
							"unresolved.key");
						!valid)
						return valid;
					return validate_event_fields(
						*decoded_payload, {field::canonical_bytes}, "unresolved.payload");
				case materialization_partition_event_kind::partition_end:
					if (auto valid = validate_event_fields(
							*decoded_key, {field::string, field::string}, "partition-end.key");
						!valid)
						return valid;
					return validate_event_fields(*decoded_payload,
												 {field::u64_canonical_bytes,
												  field::u64_canonical_bytes,
												  field::u64_canonical_bytes,
												  field::u64_canonical_bytes,
												  field::u64_canonical_bytes,
												  field::string,
												  field::string,
												  field::string,
												  field::string,
												  field::string,
												  field::string},
												 "partition-end.payload");
			}
			return sdk::unexpected(event_error("event", "unknown-kind"));
		}

		[[nodiscard]] std::array<std::byte, 32U> parse_raw_digest(const std::string_view digest,
																  const std::string_view field)
		{
			std::array<std::byte, 32U> output{};
			if (!digest.starts_with("sha256:") || digest.size() != 71U)
				return output;
			for (std::size_t index{}; index < output.size(); ++index)
			{
				const auto high = digest[7U + index * 2U];
				const auto low = digest[8U + index * 2U];
				auto nibble = [](const char value) -> int
				{
					if (value >= '0' && value <= '9')
						return value - '0';
					if (value >= 'a' && value <= 'f')
						return value - 'a' + 10;
					return -1;
				};
				const auto high_value = nibble(high);
				const auto low_value = nibble(low);
				if (high_value < 0 || low_value < 0)
					return {};
				output[index] = static_cast<std::byte>((high_value << 4) | low_value);
			}
			(void)field;
			return output;
		}

		[[nodiscard]] sdk::result<std::array<std::byte, 32U>>
		raw_digest(const std::string_view domain, const std::span<const std::byte> projection)
		{
			std::vector<std::byte> input;
			if (domain.size() > std::numeric_limits<std::uint64_t>::max() ||
				projection.size() > std::numeric_limits<std::uint64_t>::max())
				return sdk::unexpected(event_error("digest", "length-overflow"));
			input.reserve(16U + domain.size() + projection.size());
			append_u64(input, static_cast<std::uint64_t>(domain.size()));
			append_string(input, domain);
			append_u64(input, static_cast<std::uint64_t>(projection.size()));
			input.insert(input.end(), projection.begin(), projection.end());
			const auto digest = sdk::content_digest(input);
			return parse_raw_digest(digest, "digest");
		}

		class digest_writer
		{
		  public:
			digest_writer() : accumulator_{make_materialization_sha256_accumulator()} {}

			[[nodiscard]] sdk::result<void> update(const std::span<const std::byte> bytes)
			{
				if (!accumulator_)
					return sdk::unexpected(event_error("digest", "unavailable"));
				if (auto updated = accumulator_->update(bytes); !updated)
					return sdk::unexpected(spool_error("digest", "update"));
				return {};
			}

			[[nodiscard]] sdk::result<void> update_u64(const std::uint64_t value)
			{
				std::vector<std::byte> bytes;
				bytes.reserve(8U);
				append_u64(bytes, value);
				return update(bytes);
			}

			[[nodiscard]] sdk::result<std::array<std::byte, 32U>> finish()
			{
				if (!accumulator_)
					return sdk::unexpected(event_error("digest", "unavailable"));
				auto digest = accumulator_->finish();
				if (!digest)
					return sdk::unexpected(spool_error("digest", "finish"));
				return parse_raw_digest(*digest, "digest");
			}

		  private:
			std::unique_ptr<materialization_digest_accumulator> accumulator_;
		};

		[[nodiscard]] sdk::result<void> read_exact(materialization_replayable_spool& spool,
												   std::uint64_t offset,
												   std::span<std::byte> destination);

		template <class Callback>
		[[nodiscard]] sdk::result<void> scan_frames(materialization_replayable_spool& spool,
													const std::uint64_t body_end,
													Callback&& callback)
		{
			std::uint64_t offset = materialization_partition_event_stream_header_bytes;
			std::uint64_t ordinal{};
			while (offset < body_end)
			{
				std::array<std::byte, 17U> prefix{};
				if (auto read = read_exact(spool, offset, prefix); !read)
					return sdk::unexpected(std::move(read.error()));
				const auto key_length = read_u64(prefix, 1U);
				const auto payload_length = read_u64(prefix, 9U);
				std::uint64_t framed_length{};
				if (!checked_add(49U, key_length, framed_length) ||
					!checked_add(framed_length, payload_length, framed_length))
					return sdk::unexpected(event_error("frame", "length-overflow"));
				if (framed_length > body_end - offset ||
					framed_length > static_cast<std::uint64_t>(std::vector<std::byte>{}.max_size()))
					return sdk::unexpected(event_error("frame", "truncated-body"));
				std::vector<std::byte> frame(static_cast<std::size_t>(framed_length));
				if (auto read = read_exact(spool, offset, frame); !read)
					return sdk::unexpected(std::move(read.error()));
				const auto projection_length = framed_length - 32U;
				std::vector<std::byte> projection(
					frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(projection_length));
				auto checksum_input = raw_digest(frame_domain, projection);
				if (!checksum_input ||
					!std::equal(checksum_input->begin(),
								checksum_input->end(),
								frame.begin() + static_cast<std::ptrdiff_t>(projection_length)))
					return sdk::unexpected(event_error("frame", "checksum-mismatch"));
				if (auto valid = callback(frame, ordinal); !valid)
					return sdk::unexpected(std::move(valid.error()));
				if (!checked_add(offset, framed_length, offset) ||
					!checked_add(ordinal, 1U, ordinal))
					return sdk::unexpected(event_error("frame", "ordinal-overflow"));
			}
			if (offset != body_end)
				return sdk::unexpected(event_error("frame", "body-boundary"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_frame_shape(const std::span<const std::byte> frame,
							 const std::uint64_t ordinal,
							 bool& began,
							 bool& ended,
							 std::vector<std::byte>& previous_order_key)
		{
			if (frame.size() < 49U || !valid_kind(std::to_integer<unsigned char>(frame[0])))
				return sdk::unexpected(event_error("frame", "unknown-kind-or-truncated"));
			const auto key_length = read_u64(frame, 1U);
			const auto payload_length = read_u64(frame, 9U);
			constexpr auto variable_header_size = std::size_t{17U};
			if (key_length > frame.size() - variable_header_size)
				return sdk::unexpected(event_error("frame", "key-length-overflow"));
			const auto key_size = static_cast<std::size_t>(key_length);
			if (payload_length > frame.size() - variable_header_size - key_size)
				return sdk::unexpected(event_error("frame", "payload-length-overflow"));
			const auto expected_size = materialization_partition_event_frame_size(
				std::span<const std::byte>{frame.data() + variable_header_size, key_size},
				std::span<const std::byte>{frame.data() + variable_header_size + key_size,
										   static_cast<std::size_t>(payload_length)});
			if (!expected_size || *expected_size != frame.size())
				return sdk::unexpected(event_error("frame", "length-mismatch"));
			const auto key =
				std::span<const std::byte>{frame.data() + variable_header_size, key_size};
			const auto payload =
				std::span<const std::byte>{frame.data() + variable_header_size + key_size,
										   static_cast<std::size_t>(payload_length)};
			if (auto valid = validate_canonical_tuple(key, "key"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = validate_canonical_tuple(payload, "payload"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			const auto kind = std::to_integer<unsigned char>(frame[0]);
			if (auto valid = validate_event_projection(
					static_cast<materialization_partition_event_kind>(kind), key, payload);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			if (ordinal == 0U && kind != 1U)
				return sdk::unexpected(event_error("frame", "missing-partition-begin"));
			if (kind == 1U && (ordinal != 0U || began))
				return sdk::unexpected(event_error("frame", "duplicate-partition-begin"));
			if (ended)
				return sdk::unexpected(event_error("frame", "after-partition-end"));
			if (kind == 1U)
				began = true;
			if (kind == 7U)
				ended = true;
			auto order_key = event_order_key(kind, key, payload);
			if (!order_key)
				return sdk::unexpected(std::move(order_key.error()));
			if (!previous_order_key.empty() &&
				!std::lexicographical_compare(previous_order_key.begin(),
											  previous_order_key.end(),
											  order_key->begin(),
											  order_key->end()))
				return sdk::unexpected(event_error("frame", "reordered-or-duplicate"));
			previous_order_key = std::move(*order_key);
			return {};
		}

		[[nodiscard]] sdk::result<std::array<std::byte, 32U>>
		digest_frames(materialization_replayable_spool& spool,
					  const std::uint64_t body_end,
					  const std::uint64_t spool_index,
					  const std::uint64_t frame_count,
					  const std::uint64_t body_bytes)
		{
			std::uint64_t list_bytes{};
			if (!checked_mul8(frame_count, list_bytes) ||
				!checked_add(list_bytes, body_bytes, list_bytes) ||
				!checked_add(list_bytes, 16U, list_bytes))
				return sdk::unexpected(event_error("digest", "frames-length-overflow"));
			digest_writer frames;
			std::vector<std::byte> frames_prefix;
			append_u64(frames_prefix, static_cast<std::uint64_t>(frames_domain.size()));
			append_string(frames_prefix, frames_domain);
			append_u64(frames_prefix, list_bytes);
			if (auto updated = frames.update(frames_prefix); !updated)
				return sdk::unexpected(std::move(updated.error()));
			if (auto updated = frames.update_u64(spool_index); !updated)
				return sdk::unexpected(std::move(updated.error()));
			if (auto updated = frames.update_u64(frame_count); !updated)
				return sdk::unexpected(std::move(updated.error()));

			auto scanned = scan_frames(
				spool,
				body_end,
				[&](const std::vector<std::byte>& frame, const std::uint64_t) -> sdk::result<void>
				{
					if (auto updated = frames.update_u64(static_cast<std::uint64_t>(frame.size()));
						!updated)
						return sdk::unexpected(std::move(updated.error()));
					if (auto updated = frames.update(frame); !updated)
						return sdk::unexpected(std::move(updated.error()));
					return {};
				});
			if (!scanned)
				return sdk::unexpected(std::move(scanned.error()));
			(void)frames_prefix;
			return frames.finish();
		}

		[[nodiscard]] sdk::result<std::array<std::byte, 32U>>
		digest_prefix(materialization_replayable_spool& spool,
					  const std::uint64_t body_end,
					  const std::uint64_t spool_index,
					  const std::uint64_t frame_count,
					  const std::uint64_t body_bytes,
					  const std::vector<std::byte>& header)
		{
			std::uint64_t list_bytes{};
			if (!checked_mul8(frame_count, list_bytes) ||
				!checked_add(list_bytes, body_bytes, list_bytes) ||
				!checked_add(
					list_bytes, 8U + static_cast<std::uint64_t>(header.size()), list_bytes))
				return sdk::unexpected(event_error("digest", "prefix-length-overflow"));
			digest_writer prefix;
			std::vector<std::byte> prefix_header;
			append_u64(prefix_header, static_cast<std::uint64_t>(prefix_domain.size()));
			append_string(prefix_header, prefix_domain);
			append_u64(prefix_header, list_bytes);
			if (auto updated = prefix.update(prefix_header); !updated)
				return sdk::unexpected(std::move(updated.error()));
			if (auto updated = prefix.update(header); !updated)
				return sdk::unexpected(std::move(updated.error()));
			if (auto updated = prefix.update_u64(body_bytes); !updated)
				return sdk::unexpected(std::move(updated.error()));
			auto scanned = scan_frames(
				spool,
				body_end,
				[&](const std::vector<std::byte>& frame, const std::uint64_t) -> sdk::result<void>
				{
					if (auto updated = prefix.update_u64(frame.size()); !updated)
						return sdk::unexpected(std::move(updated.error()));
					return prefix.update(frame);
				});
			if (!scanned)
				return sdk::unexpected(std::move(scanned.error()));
			(void)spool_index;
			return prefix.finish();
		}

		[[nodiscard]] sdk::result<void> read_exact(materialization_replayable_spool& spool,
												   const std::uint64_t offset,
												   const std::span<std::byte> destination)
		{
			std::size_t read_bytes{};
			while (read_bytes < destination.size())
			{
				const auto read =
					spool.read_at(offset + read_bytes, destination.subspan(read_bytes));
				if (!read || *read == 0U || *read > destination.size() - read_bytes)
					return sdk::unexpected(spool_error("spool", "read"));
				read_bytes += *read;
			}
			return {};
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		make_header(const std::string_view request_id,
					const std::uint64_t spool_index,
					const materialization_event_ordinal first,
					const std::uint64_t frame_count,
					const std::uint64_t body_bytes,
					std::array<std::byte, 32U>& sequence)
		{
			if (auto valid = sdk::validate_strong_id(request_id); !valid)
				return sdk::unexpected(event_error("materialization-request-id", "strong-id"));
			std::vector<std::byte> sequence_projection;
			append_u64(sequence_projection, static_cast<std::uint64_t>(request_id.size()));
			append_string(sequence_projection, request_id);
			append_u64(sequence_projection, spool_index);
			append_ordinal(sequence_projection, first);
			auto sequence_digest = raw_digest(sequence_domain, sequence_projection);
			if (!sequence_digest)
				return sdk::unexpected(std::move(sequence_digest.error()));
			sequence = *sequence_digest;

			std::vector<std::byte> header;
			header.reserve(materialization_partition_event_stream_header_bytes);
			append_string(header, stream_magic);
			append_u16(header, 1U);
			append_u32(
				header,
				static_cast<std::uint32_t>(materialization_partition_event_stream_header_bytes));
			header.insert(header.end(), sequence.begin(), sequence.end());
			append_u64(header, spool_index);
			append_ordinal(header, first);
			append_u64(header, frame_count);
			append_u64(header, body_bytes);
			return header;
		}

		[[nodiscard]] sdk::result<materialization_partition_event_stream_receipt>
		validate_body_and_digests(materialization_replayable_spool& spool,
								  const std::uint64_t body_end,
								  const std::vector<std::byte>& header,
								  const std::array<std::byte, 32U>& sequence,
								  const std::uint64_t spool_index,
								  const materialization_event_ordinal first,
								  const std::uint64_t declared_count,
								  const std::uint64_t declared_body)
		{
			(void)sequence;
			std::uint64_t count{};
			std::uint64_t body_bytes{};
			bool began = false;
			bool ended = false;
			std::vector<std::byte> previous_order_key;
			auto scanned = scan_frames(
				spool,
				body_end,
				[&](const std::vector<std::byte>& frame,
					const std::uint64_t ordinal) -> sdk::result<void>
				{
					if (auto valid =
							validate_frame_shape(frame, ordinal, began, ended, previous_order_key);
						!valid)
						return sdk::unexpected(std::move(valid.error()));
					if (!checked_add(count, 1U, count) ||
						!checked_add(body_bytes, frame.size(), body_bytes))
						return sdk::unexpected(event_error("stream", "counter-overflow"));
					return {};
				});
			if (!scanned)
				return sdk::unexpected(std::move(scanned.error()));
			if (!began || !ended || count != declared_count || body_bytes != declared_body)
				return sdk::unexpected(event_error("stream", "declared-census-mismatch"));
			auto next = first;
			if (!increment_ordinal(next, count))
				return sdk::unexpected(event_error("stream", "ordinal-overflow"));
			auto frames_digest = digest_frames(spool, body_end, spool_index, count, body_bytes);
			if (!frames_digest)
				return sdk::unexpected(std::move(frames_digest.error()));
			auto prefix_digest =
				digest_prefix(spool, body_end, spool_index, count, body_bytes, header);
			if (!prefix_digest)
				return sdk::unexpected(std::move(prefix_digest.error()));
			return materialization_partition_event_stream_receipt{sequence,
																  spool_index,
																  first,
																  next,
																  count,
																  body_bytes,
																  *frames_digest,
																  *prefix_digest};
		}
	} // namespace

	sdk::result<void> validate_materialization_partition_event_projection(
		const materialization_partition_event_kind kind,
		const std::span<const std::byte> key,
		const std::span<const std::byte> payload)
	{
		return validate_event_projection(kind, key, payload);
	}

	sdk::result<std::uint64_t>
	materialization_partition_event_frame_size(const std::span<const std::byte> key,
											   const std::span<const std::byte> payload)
	{
		if (key.size() > std::numeric_limits<std::uint64_t>::max() ||
			payload.size() > std::numeric_limits<std::uint64_t>::max())
			return sdk::unexpected(event_error("frame", "length-overflow"));
		std::uint64_t output{};
		if (!checked_add(49U, static_cast<std::uint64_t>(key.size()), output) ||
			!checked_add(output, static_cast<std::uint64_t>(payload.size()), output))
			return sdk::unexpected(event_error("frame", "length-overflow"));
		return output;
	}

	materialization_partition_event_stream::materialization_partition_event_stream(
		materialization_partition_event_stream&&) noexcept = default;
	materialization_partition_event_stream& materialization_partition_event_stream::operator=(
		materialization_partition_event_stream&&) noexcept = default;
	materialization_partition_event_stream::~materialization_partition_event_stream() = default;

	sdk::result<materialization_partition_event_stream>
	materialization_partition_event_stream::begin(
		std::string materialization_request_id,
		const std::uint64_t spool_index,
		const materialization_event_ordinal first_event_ordinal,
		const std::uint64_t declared_frame_count,
		const std::uint64_t declared_body_bytes)
	{
		try
		{
			auto spool = make_materialization_private_spool();
			if (!spool)
				return sdk::unexpected(spool_error("spool", "create"));
			std::array<std::byte, 32U> sequence{};
			auto header = make_header(materialization_request_id,
									  spool_index,
									  first_event_ordinal,
									  declared_frame_count,
									  declared_body_bytes,
									  sequence);
			if (!header)
				return sdk::unexpected(std::move(header.error()));
			if (auto appended = (*spool)->append(*header); !appended)
				return sdk::unexpected(spool_error("spool", "header"));
			materialization_partition_event_stream output;
			output.spool_ = std::move(*spool);
			output.header_bytes_ = std::move(*header);
			output.spool_index_ = spool_index;
			output.first_event_ordinal_ = first_event_ordinal;
			output.declared_frame_count_ = declared_frame_count;
			output.declared_body_bytes_ = declared_body_bytes;
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(event_error("allocation", "unavailable"));
		}
	}

	sdk::result<void>
	materialization_partition_event_stream::append(const materialization_partition_event_kind kind,
												   const std::span<const std::byte> key,
												   const std::span<const std::byte> payload)
	{
		try
		{
			if (!spool_ || finalized_)
				return sdk::unexpected(event_error("stream", "not-writable"));
			if (!valid_kind(static_cast<std::uint8_t>(kind)))
				return sdk::unexpected(event_error("frame", "unknown-kind"));
			if (auto valid = validate_canonical_tuple(key, "key"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = validate_canonical_tuple(payload, "payload"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = validate_event_projection(kind, key, payload); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (actual_frame_count_ >= declared_frame_count_)
				return sdk::unexpected(event_error("stream", "declared-frame-count"));
			auto size = materialization_partition_event_frame_size(key, payload);
			if (!size)
				return sdk::unexpected(std::move(size.error()));
			std::uint64_t next_body{};
			if (!checked_add(actual_body_bytes_, *size, next_body) ||
				next_body > declared_body_bytes_)
				return sdk::unexpected(event_error("stream", "declared-body-bytes"));
			const auto kind_value = static_cast<std::uint8_t>(kind);
			if (actual_frame_count_ == 0U && kind_value != 1U)
				return sdk::unexpected(event_error("frame", "missing-partition-begin"));
			if (kind_value == 1U && (began_ || actual_frame_count_ != 0U))
				return sdk::unexpected(event_error("frame", "duplicate-partition-begin"));
			if (ended_)
				return sdk::unexpected(event_error("frame", "after-partition-end"));
			auto order_key = event_order_key(kind_value, key, payload);
			if (!order_key)
				return sdk::unexpected(std::move(order_key.error()));
			if (!last_order_key_.empty() &&
				!std::lexicographical_compare(last_order_key_.begin(),
											  last_order_key_.end(),
											  order_key->begin(),
											  order_key->end()))
				return sdk::unexpected(event_error("frame", "reordered-or-duplicate"));
			const auto frame_projection = frame_projection_bytes(kind, key, payload);
			auto checksum = raw_digest(frame_domain, frame_projection);
			if (!checksum)
				return sdk::unexpected(std::move(checksum.error()));
			std::vector<std::byte> frame = frame_projection;
			frame.insert(frame.end(), checksum->begin(), checksum->end());
			if (auto appended = spool_->append(frame); !appended)
				return sdk::unexpected(spool_error("spool", "frame"));
			began_ = began_ || kind_value == 1U;
			ended_ = ended_ || kind_value == 7U;
			last_order_key_ = std::move(*order_key);
			actual_body_bytes_ = next_body;
			++actual_frame_count_;
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(event_error("allocation", "unavailable"));
		}
	}

	sdk::result<materialization_partition_event_stream_receipt>
	materialization_partition_event_stream::finalize() &&
	{
		try
		{
			if (!spool_ || finalized_ || !began_ || !ended_ ||
				actual_frame_count_ != declared_frame_count_ ||
				actual_body_bytes_ != declared_body_bytes_)
				return sdk::unexpected(event_error("stream", "finalize-precondition"));
			const auto body_end = spool_->size_bytes();
			std::array<std::byte, 32U> sequence{};
			std::copy(header_bytes_.begin() + 14, header_bytes_.begin() + 46, sequence.begin());
			auto checked = validate_body_and_digests(*spool_,
													 body_end,
													 header_bytes_,
													 sequence,
													 spool_index_,
													 first_event_ordinal_,
													 actual_frame_count_,
													 actual_body_bytes_);
			if (!checked)
				return sdk::unexpected(std::move(checked.error()));
			const auto& value = *checked;
			std::vector<std::byte> trailer;
			trailer.reserve(materialization_partition_event_stream_trailer_bytes);
			append_string(trailer, trailer_magic);
			append_u64(trailer, value.spool_index);
			append_ordinal(trailer, value.next_event_ordinal);
			append_u64(trailer, value.actual_frame_count);
			append_u64(trailer, value.actual_body_bytes);
			trailer.insert(trailer.end(), value.frames_digest.begin(), value.frames_digest.end());
			trailer.insert(trailer.end(),
						   value.stream_prefix_digest.begin(),
						   value.stream_prefix_digest.end());
			if (trailer.size() != materialization_partition_event_stream_trailer_bytes)
				return sdk::unexpected(event_error("trailer", "length"));
			if (auto appended = spool_->append(trailer); !appended)
				return sdk::unexpected(spool_error("spool", "trailer"));
			if (auto sealed = spool_->seal(); !sealed)
				return sdk::unexpected(spool_error("spool", "seal"));
			finalized_ = true;
			receipt_ = value;
			return value;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(event_error("allocation", "unavailable"));
		}
	}

	std::unique_ptr<materialization_replayable_spool>
	materialization_partition_event_stream::release_spool() &&
	{
		if (!finalized_)
			return {};
		return std::move(spool_);
	}

	sdk::result<materialization_partition_event_stream_receipt>
	validate_materialization_partition_event_stream(
		materialization_replayable_spool& spool,
		const std::optional<std::string_view> expected_materialization_request_id)
	{
		try
		{
			if (!spool.sealed())
				return sdk::unexpected(event_error("spool", "unsealed"));
			const auto total = spool.size_bytes();
			if (total < materialization_partition_event_stream_header_bytes +
					materialization_partition_event_stream_trailer_bytes)
				return sdk::unexpected(event_error("stream", "truncated"));
			std::vector<std::byte> header(materialization_partition_event_stream_header_bytes);
			if (auto read = read_exact(spool, 0U, header); !read)
				return sdk::unexpected(std::move(read.error()));
			if (!bytes_equal(std::span<const std::byte>{header.data(), 8U}, stream_magic) ||
				read_u16(header, 8U) != 1U ||
				read_u32(header, 10U) != materialization_partition_event_stream_header_bytes)
				return sdk::unexpected(event_error("header", "magic-version-length"));
			std::array<std::byte, 32U> sequence{};
			std::copy(header.begin() + 14, header.begin() + 46, sequence.begin());
			const auto spool_index = read_u64(header, 46U);
			const auto first = read_ordinal(header, 54U);
			const auto declared_count = read_u64(header, 70U);
			const auto declared_body = read_u64(header, 78U);
			if (expected_materialization_request_id)
			{
				std::array<std::byte, 32U> expected_sequence{};
				auto expected_header = make_header(*expected_materialization_request_id,
												   spool_index,
												   first,
												   declared_count,
												   declared_body,
												   expected_sequence);
				if (!expected_header || expected_sequence != sequence)
					return sdk::unexpected(event_error("header", "sequence-mismatch"));
			}
			const auto trailer_offset =
				total - materialization_partition_event_stream_trailer_bytes;
			const auto body_end = trailer_offset;
			std::vector<std::byte> trailer(materialization_partition_event_stream_trailer_bytes);
			if (auto read = read_exact(spool, trailer_offset, trailer); !read)
				return sdk::unexpected(std::move(read.error()));
			if (!bytes_equal(std::span<const std::byte>{trailer.data(), 8U}, trailer_magic))
				return sdk::unexpected(event_error("trailer", "magic"));
			const auto trailer_spool_index = read_u64(trailer, 8U);
			const auto next = read_ordinal(trailer, 16U);
			const auto trailer_count = read_u64(trailer, 32U);
			const auto trailer_body = read_u64(trailer, 40U);
			std::array<std::byte, 32U> trailer_frames{};
			std::array<std::byte, 32U> trailer_prefix{};
			std::copy(trailer.begin() + 48, trailer.begin() + 80, trailer_frames.begin());
			std::copy(trailer.begin() + 80, trailer.begin() + 112, trailer_prefix.begin());
			auto computed = validate_body_and_digests(spool,
													  body_end,
													  header,
													  sequence,
													  spool_index,
													  first,
													  declared_count,
													  declared_body);
			if (!computed)
				return sdk::unexpected(std::move(computed.error()));
			if (trailer_spool_index != spool_index ||
				trailer_count != computed->actual_frame_count ||
				trailer_body != computed->actual_body_bytes ||
				next != computed->next_event_ordinal || trailer_frames != computed->frames_digest ||
				trailer_prefix != computed->stream_prefix_digest)
				return sdk::unexpected(event_error("trailer", "receipt-mismatch"));
			return *computed;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(event_error("allocation", "unavailable"));
		}
	}

	sdk::result<void> replay_materialization_partition_event_stream(
		materialization_replayable_spool& spool,
		const std::optional<std::string_view> expected_materialization_request_id,
		const materialization_partition_event_consumer& consumer)
	{
		try
		{
			if (!consumer)
				return sdk::unexpected(event_error("replay", "consumer"));
			auto receipt = validate_materialization_partition_event_stream(
				spool, expected_materialization_request_id);
			if (!receipt)
				return sdk::unexpected(std::move(receipt.error()));
			const auto total = spool.size_bytes();
			if (total < materialization_partition_event_stream_header_bytes +
					materialization_partition_event_stream_trailer_bytes)
				return sdk::unexpected(event_error("replay", "truncated"));
			const auto body_end = total - materialization_partition_event_stream_trailer_bytes;
			bool began = false;
			bool ended = false;
			std::vector<std::byte> previous_order_key;
			auto scanned = scan_frames(
				spool,
				body_end,
				[&](const std::vector<std::byte>& frame,
					const std::uint64_t ordinal) -> sdk::result<void>
				{
					if (auto valid =
							validate_frame_shape(frame, ordinal, began, ended, previous_order_key);
						!valid)
						return sdk::unexpected(std::move(valid.error()));
					const auto key_length = read_u64(frame, 1U);
					const auto payload_length = read_u64(frame, 9U);
					constexpr auto variable_header_size = std::size_t{17U};
					if (key_length > frame.size() - variable_header_size)
						return sdk::unexpected(event_error("replay", "key-length-overflow"));
					const auto key_size = static_cast<std::size_t>(key_length);
					if (payload_length > frame.size() - variable_header_size - key_size)
						return sdk::unexpected(event_error("replay", "payload-length-overflow"));
					const auto key =
						std::span<const std::byte>{frame.data() + variable_header_size, key_size};
					const auto payload =
						std::span<const std::byte>{frame.data() + variable_header_size + key_size,
												   static_cast<std::size_t>(payload_length)};
					return consumer(ordinal,
									static_cast<materialization_partition_event_kind>(
										std::to_integer<unsigned char>(frame[0])),
									key,
									payload);
				});
			if (!scanned)
				return sdk::unexpected(std::move(scanned.error()));
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(event_error("allocation", "unavailable"));
		}
	}
} // namespace cxxlens::detail::clang22::materialization
