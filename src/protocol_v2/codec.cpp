#include "codec.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>

namespace cxxlens::protocol_v2
{
	namespace
	{
		struct decoded_header
		{
			std::uint16_t major{};
			std::uint16_t minor{};
			std::uint16_t type_id{};
			std::uint16_t flags{};
			std::uint64_t stream_id{};
			std::uint64_t sequence{};
			std::size_t control_size{};
			std::size_t payload_size{};
			std::size_t total_size{};
		};

		[[nodiscard]] sdk::error failure(const std::string_view field,
										 const std::string_view detail,
										 const std::string_view code = "protocol-v2.header-invalid")
		{
			return {std::string{code}, std::string{field}, std::string{detail}};
		}

		void append_be(bytes& output, const std::uint64_t value, const std::size_t width)
		{
			for (std::size_t shift = width * 8U; shift > 0U; shift -= 8U)
				output.push_back(static_cast<byte>(value >> (shift - 8U)));
		}

		[[nodiscard]] std::uint64_t read_be(const std::span<const byte> input,
											const std::size_t offset,
											const std::size_t width) noexcept
		{
			std::uint64_t value{};
			for (std::size_t index{}; index < width; ++index)
				value = (value << 8U) | std::to_integer<std::uint64_t>(input[offset + index]);
			return value;
		}

		[[nodiscard]] bool all_zero(const digest32& value) noexcept
		{
			return std::ranges::all_of(value,
									   [](const byte item)
									   {
										   return item == byte{};
									   });
		}

		[[nodiscard]] sdk::result<void> validate_limits(const limits bound)
		{
			if (bound.minimum_minor != 0U || bound.maximum_minor != 0U)
				return sdk::unexpected(failure(
					"protocol_minor", "protocol-2.0-only", "protocol-v2.downgrade-rejected"));
			if (bound.max_control_bytes == 0U || bound.max_payload_bytes == 0U ||
				bound.max_frame_bytes < fixed_header_bytes ||
				bound.max_control_bytes > max_control_bytes ||
				bound.max_payload_bytes > max_payload_bytes ||
				bound.max_frame_bytes > max_frame_bytes)
				return sdk::unexpected(
					failure("limits", "outside-protocol-2-bounds", "protocol-v2.resource-limit"));
			if ((bound.supported_flags & static_cast<std::uint16_t>(~flag_end_of_stream)) != 0U)
				return sdk::unexpected(
					failure("flags", "unsupported-negotiation", "protocol-v2.unknown-extension"));
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_header_values(const std::uint16_t major,
															   const std::uint16_t minor,
															   const std::uint16_t type_id,
															   const std::uint16_t flags,
															   const std::size_t control_size,
															   const std::size_t payload_size,
															   const limits bound)
		{
			if (auto valid = validate_limits(bound); !valid)
				return valid;
			if (major != protocol_major)
				return sdk::unexpected(failure("protocol_major",
											   "downgrade-or-unsupported",
											   "protocol-v2.downgrade-rejected"));
			if (minor != 0U)
				return sdk::unexpected(failure("protocol_minor",
											   "downgrade-or-unnegotiated",
											   "protocol-v2.downgrade-rejected"));
			const auto known_type = is_known_message_id(type_id);
			const auto required_extension = (flags & flag_required_extension) != 0U;
			const auto optional_extension = (flags & flag_optional_extension) != 0U;
			const auto compressed_payload = (flags & flag_compressed_payload) != 0U;
			const auto end_of_stream = (flags & flag_end_of_stream) != 0U;
			if (required_extension)
				return sdk::unexpected(failure(
					"flags", "required-extension", "protocol-v2.unknown-required-extension"));
			if ((flags & static_cast<std::uint16_t>(~known_flags)) != 0U)
				return sdk::unexpected(
					failure("flags", "reserved-bit", "protocol-v2.unknown-extension"));
			if (compressed_payload)
				return sdk::unexpected(failure(
					"flags", "compression-unimplemented", "protocol-v2.unsupported-compression"));
			if (optional_extension && (known_type || end_of_stream))
				return sdk::unexpected(failure(
					"flags", known_type ? "optional-known-type" : "optional-end-of-stream"));
			if (type_id == 0U || (!known_type && !optional_extension))
				return sdk::unexpected(
					failure("message_type", "unknown", "protocol-v2.unknown-message"));
			if (end_of_stream && (bound.supported_flags & flag_end_of_stream) == 0U)
				return sdk::unexpected(failure(
					"flags", "end-of-stream-not-negotiated", "protocol-v2.unknown-extension"));
			if (end_of_stream &&
				type_id != static_cast<std::uint16_t>(message_type::task_complete) &&
				type_id != static_cast<std::uint16_t>(message_type::task_failed))
				return sdk::unexpected(
					failure("flags", "end-of-stream-message", "protocol-v2.header-invalid"));
			if (control_size == 0U)
				return sdk::unexpected(failure("control", "empty", "protocol-v2.cbor-invalid"));
			if (control_size > bound.max_control_bytes)
				return sdk::unexpected(
					failure("control", "limit-exceeded", "protocol-v2.resource-limit"));
			if (payload_size > bound.max_payload_bytes)
				return sdk::unexpected(
					failure("payload", "limit-exceeded", "protocol-v2.resource-limit"));
			if (control_size > std::numeric_limits<std::uint32_t>::max())
				return sdk::unexpected(
					failure("control", "wire-length-overflow", "protocol-v2.resource-limit"));
			if (payload_size > std::numeric_limits<std::size_t>::max() - fixed_header_bytes ||
				control_size >
					std::numeric_limits<std::size_t>::max() - fixed_header_bytes - payload_size)
				return sdk::unexpected(failure("length", "overflow", "protocol-v2.resource-limit"));
			const auto total = fixed_header_bytes + control_size + payload_size;
			if (total > bound.max_frame_bytes)
				return sdk::unexpected(
					failure("frame", "limit-exceeded", "protocol-v2.resource-limit"));
			if (type_id == heartbeat_message_id && payload_size != 0U)
				return sdk::unexpected(
					failure("payload", "heartbeat-must-be-empty", "protocol-v2.header-invalid"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_canonical_control(const std::span<const byte> control, const limits bound)
		{
			cbor::limits cbor_bound;
			cbor_bound.max_bytes = bound.max_control_bytes;
			cbor_bound.max_text_bytes = bound.max_control_bytes;
			cbor_bound.max_byte_string_bytes = bound.max_control_bytes;
			cbor_bound.max_items = 65'536U;
			auto decoded = cbor::decode(control, cbor_bound);
			if (!decoded)
				return sdk::unexpected(
					failure("control", decoded.error().detail, "protocol-v2.cbor-invalid"));
			auto encoded = cbor::encode(*decoded, cbor_bound);
			if (!encoded || encoded->size() != control.size() ||
				!std::ranges::equal(*encoded, control))
				return sdk::unexpected(
					failure("control", "non-canonical", "protocol-v2.cbor-invalid"));
			return {};
		}

		void set_digest(bytes& output, const digest32& digest)
		{
			output.insert(output.end(), digest.begin(), digest.end());
		}

		[[nodiscard]] sdk::result<void> check_digest(const std::span<const byte> actual,
													 const std::span<const byte> expected,
													 const std::string_view field)
		{
			const auto digest = sha256(actual);
			if (expected.size() != digest.size() || !std::ranges::equal(digest, expected))
				return sdk::unexpected(failure(field, "mismatch", "protocol-v2.digest-mismatch"));
			return {};
		}

		[[nodiscard]] sdk::result<decoded_header> inspect_header(const std::span<const byte> input,
																 const limits bound)
		{
			if (input.size() < fixed_header_bytes)
				return sdk::unexpected(failure("header", "truncated"));
			if (input[0] != byte{'C'} || input[1] != byte{'X'} || input[2] != byte{'X'} ||
				input[3] != byte{'P'})
				return sdk::unexpected(failure("magic", "mismatch"));
			decoded_header output;
			output.major = static_cast<std::uint16_t>(read_be(input, 4U, 2U));
			output.minor = static_cast<std::uint16_t>(read_be(input, 6U, 2U));
			output.type_id = static_cast<std::uint16_t>(read_be(input, 8U, 2U));
			output.flags = static_cast<std::uint16_t>(read_be(input, 10U, 2U));
			output.stream_id = read_be(input, 12U, 8U);
			output.sequence = read_be(input, 20U, 8U);
			const auto control_length = read_be(input, 28U, 4U);
			const auto payload_length = read_be(input, 32U, 8U);
			if (control_length > std::numeric_limits<std::size_t>::max() - fixed_header_bytes ||
				payload_length > std::numeric_limits<std::size_t>::max() - fixed_header_bytes -
						static_cast<std::size_t>(control_length))
				return sdk::unexpected(failure("length", "overflow", "protocol-v2.resource-limit"));
			output.control_size = static_cast<std::size_t>(control_length);
			output.payload_size = static_cast<std::size_t>(payload_length);
			output.total_size = fixed_header_bytes + output.control_size + output.payload_size;
			if (auto valid = validate_header_values(output.major,
													output.minor,
													output.type_id,
													output.flags,
													output.control_size,
													output.payload_size,
													bound);
				!valid)
				return sdk::unexpected(valid.error());
			return output;
		}

		// FIPS 180-4 SHA-256, kept local so this bounded slice does not depend on
		// OpenSSL or the shared provider runtime.
		class sha256_state
		{
		  public:
			sha256_state() noexcept
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

			void update(const std::span<const byte> input) noexcept
			{
				for (const auto item : input)
				{
					buffer_[buffer_size_++] = std::to_integer<std::uint8_t>(item);
					if (buffer_size_ == buffer_.size())
					{
						transform(buffer_.data());
						buffer_size_ = 0U;
					}
				}
				bit_length_ += static_cast<std::uint64_t>(input.size()) * 8U;
			}

			[[nodiscard]] digest32 finish() noexcept
			{
				buffer_[buffer_size_++] = 0x80U;
				if (buffer_size_ > 56U)
				{
					std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
							  buffer_.end(),
							  0U);
					transform(buffer_.data());
					buffer_size_ = 0U;
				}
				std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
						  buffer_.begin() + 56,
						  0U);
				for (std::size_t index{}; index < 8U; ++index)
					buffer_[56U + index] =
						static_cast<std::uint8_t>(bit_length_ >> (56U - index * 8U));
				transform(buffer_.data());

				digest32 output{};
				for (std::size_t index{}; index < state_.size(); ++index)
				{
					output[index * 4U] = static_cast<byte>(state_[index] >> 24U);
					output[index * 4U + 1U] = static_cast<byte>(state_[index] >> 16U);
					output[index * 4U + 2U] = static_cast<byte>(state_[index] >> 8U);
					output[index * 4U + 3U] = static_cast<byte>(state_[index]);
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

			static constexpr std::uint32_t rotate_right(const std::uint32_t value,
														const std::uint32_t amount) noexcept
			{
				return (value >> amount) | (value << (32U - amount));
			}

			void transform(const std::uint8_t* input) noexcept
			{
				std::array<std::uint32_t, 64U> schedule{};
				for (std::size_t index{}; index < 16U; ++index)
					schedule[index] = (static_cast<std::uint32_t>(input[index * 4U]) << 24U) |
						(static_cast<std::uint32_t>(input[index * 4U + 1U]) << 16U) |
						(static_cast<std::uint32_t>(input[index * 4U + 2U]) << 8U) |
						static_cast<std::uint32_t>(input[index * 4U + 3U]);
				for (std::size_t index = 16U; index < schedule.size(); ++index)
				{
					const auto s0 = rotate_right(schedule[index - 15U], 7U) ^
						rotate_right(schedule[index - 15U], 18U) ^ (schedule[index - 15U] >> 3U);
					const auto s1 = rotate_right(schedule[index - 2U], 17U) ^
						rotate_right(schedule[index - 2U], 19U) ^ (schedule[index - 2U] >> 10U);
					schedule[index] = schedule[index - 16U] + s0 + schedule[index - 7U] + s1;
				}
				auto working = state_;
				for (std::size_t index{}; index < constants.size(); ++index)
				{
					const auto s1 = rotate_right(working[4], 6U) ^ rotate_right(working[4], 11U) ^
						rotate_right(working[4], 25U);
					const auto choose = (working[4] & working[5]) ^ (~working[4] & working[6]);
					const auto temp1 =
						working[7] + s1 + choose + constants[index] + schedule[index];
					const auto s0 = rotate_right(working[0], 2U) ^ rotate_right(working[0], 13U) ^
						rotate_right(working[0], 22U);
					const auto majority = (working[0] & working[1]) ^ (working[0] & working[2]) ^
						(working[1] & working[2]);
					const auto temp2 = s0 + majority;
					working[7] = working[6];
					working[6] = working[5];
					working[5] = working[4];
					working[4] = working[3] + temp1;
					working[3] = working[2];
					working[2] = working[1];
					working[1] = working[0];
					working[0] = temp1 + temp2;
				}
				for (std::size_t index{}; index < state_.size(); ++index)
					state_[index] += working[index];
			}

			std::array<std::uint32_t, 8U> state_{};
			std::array<std::uint8_t, 64U> buffer_{};
			std::size_t buffer_size_{};
			std::uint64_t bit_length_{};
		};
	} // namespace

	digest32 sha256(const std::span<const byte> input) noexcept
	{
		sha256_state state;
		state.update(input);
		return state.finish();
	}

	bool digest_is_zero(const digest32& digest) noexcept
	{
		return all_zero(digest);
	}

	bool digest_equal(const digest32& left, const digest32& right) noexcept
	{
		return std::equal(left.begin(), left.end(), right.begin(), right.end());
	}

	prepared_frame_decode::prepared_frame_decode(frame header,
												 const std::size_t control_bytes,
												 const std::size_t payload_bytes,
												 const limits bound) noexcept
		: header_{std::move(header)}, control_bytes_{control_bytes}, payload_bytes_{payload_bytes},
		  bound_{bound}
	{
	}

	prepared_frame_decode::prepared_frame_decode(prepared_frame_decode&& other) noexcept
		: header_{std::move(other.header_)}, control_bytes_{other.control_bytes_},
		  payload_bytes_{other.payload_bytes_}, bound_{other.bound_}, consumed_{other.consumed_}
	{
		other.consumed_ = true;
		other.control_bytes_ = 0U;
		other.payload_bytes_ = 0U;
	}

	prepared_frame_decode& prepared_frame_decode::operator=(prepared_frame_decode&& other) noexcept
	{
		if (this == &other)
			return *this;
		header_ = std::move(other.header_);
		control_bytes_ = other.control_bytes_;
		payload_bytes_ = other.payload_bytes_;
		bound_ = other.bound_;
		consumed_ = other.consumed_;
		other.consumed_ = true;
		other.control_bytes_ = 0U;
		other.payload_bytes_ = 0U;
		return *this;
	}

	sdk::result<prepared_frame_decode>
	prepare_frame_header(const std::span<const byte, fixed_header_bytes> header, const limits bound)
	{
		try
		{
			auto inspected = inspect_header(header, bound);
			if (!inspected)
				return sdk::unexpected(inspected.error());
			frame output;
			output.protocol_major = inspected->major;
			output.protocol_minor = inspected->minor;
			output.type = static_cast<message_type>(inspected->type_id);
			output.flags = inspected->flags;
			output.stream_id = inspected->stream_id;
			output.sequence = inspected->sequence;
			std::copy_n(
				header.begin() + 40, output.control_digest.size(), output.control_digest.begin());
			std::copy_n(
				header.begin() + 72, output.payload_digest.size(), output.payload_digest.begin());
			return prepared_frame_decode{
				std::move(output), inspected->control_size, inspected->payload_size, bound};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("header", "allocation", "protocol-v2.resource-limit"));
		}
	}

	sdk::result<frame> prepared_frame_decode::finalize(bytes&& control, bytes&& payload) &&
	{
		try
		{
			if (consumed_)
				return sdk::unexpected(failure("prepared-frame", "already-consumed"));
			consumed_ = true;
			if (control.size() != control_bytes_ || payload.size() != payload_bytes_)
				return sdk::unexpected(failure("frame", "body-length-mismatch"));
			if (auto valid = validate_header_values(header_.protocol_major,
													header_.protocol_minor,
													static_cast<std::uint16_t>(header_.type),
													header_.flags,
													control.size(),
													payload.size(),
													bound_);
				!valid)
				return sdk::unexpected(valid.error());
			if (auto valid = check_digest(control, header_.control_digest, "control"); !valid)
				return sdk::unexpected(valid.error());
			if (auto valid = check_digest(payload, header_.payload_digest, "payload"); !valid)
				return sdk::unexpected(valid.error());
			if (auto valid = validate_canonical_control(control, bound_); !valid)
				return sdk::unexpected(valid.error());
			header_.control = std::move(control);
			header_.payload = std::move(payload);
			return std::move(header_);
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("frame", "allocation", "protocol-v2.resource-limit"));
		}
	}

	sdk::result<bytes> encode_frame(const frame& value, const limits bound)
	{
		try
		{
			if (auto valid = validate_header_values(value.protocol_major,
													value.protocol_minor,
													static_cast<std::uint16_t>(value.type),
													value.flags,
													value.control.size(),
													value.payload.size(),
													bound);
				!valid)
				return sdk::unexpected(valid.error());
			if (auto valid = validate_canonical_control(value.control, bound); !valid)
				return sdk::unexpected(valid.error());

			const auto control_digest = sha256(value.control);
			const auto payload_digest = sha256(value.payload);
			if ((!digest_is_zero(value.control_digest) &&
				 !digest_equal(value.control_digest, control_digest)) ||
				(!digest_is_zero(value.payload_digest) &&
				 !digest_equal(value.payload_digest, payload_digest)))
				return sdk::unexpected(
					failure("digest", "caller-supplied-mismatch", "protocol-v2.digest-mismatch"));

			bytes output;
			output.reserve(fixed_header_bytes + value.control.size() + value.payload.size());
			output.insert(output.end(), {byte{'C'}, byte{'X'}, byte{'X'}, byte{'P'}});
			append_be(output, value.protocol_major, 2U);
			append_be(output, value.protocol_minor, 2U);
			append_be(output, static_cast<std::uint16_t>(value.type), 2U);
			append_be(output, value.flags, 2U);
			append_be(output, value.stream_id, 8U);
			append_be(output, value.sequence, 8U);
			append_be(output, value.control.size(), 4U);
			append_be(output, value.payload.size(), 8U);
			set_digest(output, control_digest);
			set_digest(output, payload_digest);
			output.insert(output.end(), value.control.begin(), value.control.end());
			output.insert(output.end(), value.payload.begin(), value.payload.end());
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("frame", "allocation", "protocol-v2.resource-limit"));
		}
	}

	sdk::result<frame> decode_frame(const std::span<const byte> input, const limits bound)
	{
		try
		{
			if (input.size() < fixed_header_bytes)
				return sdk::unexpected(failure("header", "truncated"));
			auto prepared = prepare_frame_header(
				std::span<const byte, fixed_header_bytes>{input.data(), fixed_header_bytes}, bound);
			if (!prepared)
				return sdk::unexpected(prepared.error());
			if (prepared->encoded_bytes() != input.size())
				return sdk::unexpected(failure("frame", "length-mismatch"));
			const auto control_begin = fixed_header_bytes;
			const auto payload_begin = control_begin + prepared->control_bytes();
			bytes control(input.begin() + static_cast<std::ptrdiff_t>(control_begin),
						  input.begin() + static_cast<std::ptrdiff_t>(payload_begin));
			bytes payload(input.begin() + static_cast<std::ptrdiff_t>(payload_begin), input.end());
			return std::move(*prepared).finalize(std::move(control), std::move(payload));
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("frame", "allocation", "protocol-v2.resource-limit"));
		}
	}

	sdk::result<std::vector<frame>> decode_frame_stream(const std::span<const byte> input,
														const limits bound,
														const std::uint64_t maximum_frames)
	{
		try
		{
			if (auto valid = validate_limits(bound); !valid)
				return sdk::unexpected(valid.error());
			if (maximum_frames == 0U)
				return sdk::unexpected(
					failure("maximum_frames", "zero", "protocol-v2.resource-limit"));
			if (input.empty())
				return sdk::unexpected(failure("transcript", "empty"));
			std::vector<frame> output;
			output.reserve(std::min<std::size_t>(static_cast<std::size_t>(maximum_frames), 16U));
			std::size_t offset{};
			while (offset < input.size())
			{
				if (output.size() >= maximum_frames)
					return sdk::unexpected(
						failure("transcript", "frame-count", "protocol-v2.resource-limit"));
				if (input.size() - offset < fixed_header_bytes)
					return sdk::unexpected(failure("header", "truncated"));
				auto prepared = prepare_frame_header(
					std::span<const byte, fixed_header_bytes>{input.data() + offset,
															  fixed_header_bytes},
					bound);
				if (!prepared)
					return sdk::unexpected(prepared.error());
				if (prepared->encoded_bytes() > input.size() - offset)
					return sdk::unexpected(failure("frame", "length-mismatch"));
				const auto control_begin = offset + fixed_header_bytes;
				const auto payload_begin = control_begin + prepared->control_bytes();
				const auto frame_end = offset + prepared->encoded_bytes();
				bytes control(input.begin() + static_cast<std::ptrdiff_t>(control_begin),
							  input.begin() + static_cast<std::ptrdiff_t>(payload_begin));
				bytes payload(input.begin() + static_cast<std::ptrdiff_t>(payload_begin),
							  input.begin() + static_cast<std::ptrdiff_t>(frame_end));
				auto decoded =
					std::move(*prepared).finalize(std::move(control), std::move(payload));
				if (!decoded)
					return sdk::unexpected(decoded.error());
				output.push_back(std::move(*decoded));
				offset = frame_end;
			}
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("frame", "allocation", "protocol-v2.resource-limit"));
		}
	}

	sdk::result<void> sequence_guard::accept(const frame& value)
	{
		if (value.stream_id != stream_id_)
			return sdk::unexpected(
				failure("stream_id", "mismatch", "protocol-v2.sequence-invalid"));
		if (exhausted_ || value.sequence < next_sequence_)
			return sdk::unexpected(failure("sequence", "replay", "protocol-v2.replay-rejected"));
		if (value.sequence != next_sequence_)
			return sdk::unexpected(
				failure("sequence", "gap-or-reorder", "protocol-v2.sequence-invalid"));
		if (next_sequence_ == std::numeric_limits<std::uint64_t>::max())
			exhausted_ = true;
		else
			++next_sequence_;
		return {};
	}

	sdk::result<void> credit_window::grant(const credit increment)
	{
		if (increment.bytes == 0U || increment.frames == 0U)
			return sdk::unexpected(failure("credit", "zero", "protocol-v2.credit-invalid"));
		if (increment.bytes > std::numeric_limits<std::uint64_t>::max() - available_.bytes ||
			increment.frames > std::numeric_limits<std::uint64_t>::max() - available_.frames)
			return sdk::unexpected(failure("credit", "overflow", "protocol-v2.credit-invalid"));
		available_.bytes += increment.bytes;
		available_.frames += increment.frames;
		return {};
	}

	sdk::result<void> credit_window::consume(const frame& value)
	{
		if (available_.frames == 0U)
			return sdk::unexpected(
				failure("credit.frames", "exceeded", "protocol-v2.credit-exceeded"));
		if (value.payload.size() > std::numeric_limits<std::uint64_t>::max() - fixed_header_bytes ||
			value.control.size() > std::numeric_limits<std::uint64_t>::max() - fixed_header_bytes -
					value.payload.size())
			return sdk::unexpected(
				failure("credit.bytes", "overflow", "protocol-v2.credit-exceeded"));
		const auto bytes_used = static_cast<std::uint64_t>(fixed_header_bytes) +
			static_cast<std::uint64_t>(value.control.size()) +
			static_cast<std::uint64_t>(value.payload.size());
		if (bytes_used > available_.bytes)
			return sdk::unexpected(
				failure("credit.bytes", "exceeded", "protocol-v2.credit-exceeded"));
		--available_.frames;
		available_.bytes -= bytes_used;
		return {};
	}

	sdk::result<void> transcript_guard::accept(const frame& value, const bool consume_credit)
	{
		auto sequence_copy = sequence_;
		auto credit_copy = credit_;
		if (auto valid = sequence_copy.accept(value); !valid)
			return sdk::unexpected(valid.error());
		if (consume_credit)
			if (auto valid = credit_copy.consume(value); !valid)
				return sdk::unexpected(valid.error());
		sequence_ = sequence_copy;
		credit_ = credit_copy;
		return {};
	}
} // namespace cxxlens::protocol_v2
