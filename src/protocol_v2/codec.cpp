#include "codec.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace cxxlens::protocol_v2
{
	namespace
	{
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

		[[nodiscard]] bool valid_minor(const std::uint16_t minor, const limits bound) noexcept
		{
			return minor >= bound.minimum_minor && minor <= bound.maximum_minor;
		}

		[[nodiscard]] sdk::result<void> validate_header_values(const frame& value,
															   const limits bound)
		{
			if (value.protocol_major != protocol_major)
				return sdk::unexpected(failure("protocol_major",
											   "downgrade-or-unsupported",
											   "protocol-v2.downgrade-rejected"));
			if (value.protocol_minor < bound.minimum_minor ||
				value.protocol_minor > bound.maximum_minor)
				return sdk::unexpected(failure("protocol_minor",
											   "downgrade-or-unnegotiated",
											   "protocol-v2.downgrade-rejected"));
			if (!is_known_message_type(value.type))
				return sdk::unexpected(
					failure("message_type", "unknown", "protocol-v2.unknown-message"));
			if ((value.flags & static_cast<std::uint16_t>(~known_flags)) != 0U)
				return sdk::unexpected(
					failure("flags", "reserved-bit", "protocol-v2.unknown-extension"));
			if ((value.flags & static_cast<std::uint16_t>(~bound.supported_flags)) != 0U)
				return sdk::unexpected(
					failure("flags", "not-negotiated", "protocol-v2.unknown-extension"));
			if ((value.flags & flag_compressed_payload) != 0U)
				return sdk::unexpected(failure(
					"flags", "compression-unimplemented", "protocol-v2.unsupported-compression"));
			if ((value.flags & (flag_required_extension | flag_optional_extension)) != 0U)
				return sdk::unexpected(
					failure("flags", "extension-unimplemented", "protocol-v2.unknown-extension"));
			if ((value.flags & flag_end_of_stream) != 0U &&
				value.type != message_type::task_complete &&
				value.type != message_type::task_failed)
				return sdk::unexpected(
					failure("flags", "end-of-stream-message", "protocol-v2.header-invalid"));
			if (value.control.empty())
				return sdk::unexpected(failure("control", "empty", "protocol-v2.cbor-invalid"));
			if (value.control.size() > bound.max_control_bytes ||
				value.payload.size() > bound.max_payload_bytes)
				return sdk::unexpected(
					failure("length", "limit-exceeded", "protocol-v2.resource-limit"));
			if (value.control.size() > std::numeric_limits<std::uint32_t>::max())
				return sdk::unexpected(
					failure("control", "wire-length-overflow", "protocol-v2.resource-limit"));
			if (value.control.size() >
				std::numeric_limits<std::size_t>::max() - value.payload.size() - fixed_header_bytes)
				return sdk::unexpected(failure("length", "overflow", "protocol-v2.resource-limit"));
			const auto total = fixed_header_bytes + value.control.size() + value.payload.size();
			if (total > bound.max_frame_bytes)
				return sdk::unexpected(
					failure("frame", "limit-exceeded", "protocol-v2.resource-limit"));
			if (value.type == message_type::heartbeat && !value.payload.empty())
				return sdk::unexpected(
					failure("payload", "heartbeat-must-be-empty", "protocol-v2.header-invalid"));
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_canonical_control(const bytes& control,
																   const limits bound)
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
			if (!encoded || *encoded != control)
				return sdk::unexpected(
					failure("control", "non-canonical", "protocol-v2.cbor-invalid"));
			return {};
		}

		void set_digest(bytes& output, const digest32& digest)
		{
			output.insert(output.end(), digest.begin(), digest.end());
		}

		[[nodiscard]] sdk::result<void> check_digest(const std::span<const byte> actual,
													 const digest32& expected,
													 const std::string_view field)
		{
			if (!digest_equal(sha256(actual), expected))
				return sdk::unexpected(failure(field, "mismatch", "protocol-v2.digest-mismatch"));
			return {};
		}

		[[nodiscard]] std::size_t frame_size_from_header(const std::span<const byte> input)
		{
			const auto control_length = read_be(input, 28U, 4U);
			const auto payload_length = read_be(input, 32U, 8U);
			if (control_length > std::numeric_limits<std::size_t>::max() ||
				payload_length > std::numeric_limits<std::size_t>::max() - fixed_header_bytes -
						static_cast<std::size_t>(control_length))
				return std::numeric_limits<std::size_t>::max();
			return fixed_header_bytes + static_cast<std::size_t>(control_length) +
				static_cast<std::size_t>(payload_length);
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

	sdk::result<bytes> encode_frame(const frame& value, const limits bound)
	{
		if (auto valid = validate_header_values(value, bound); !valid)
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

	sdk::result<frame> decode_frame(const std::span<const byte> input, const limits bound)
	{
		if (input.size() < fixed_header_bytes)
			return sdk::unexpected(failure("header", "truncated"));
		if (input[0] != byte{'C'} || input[1] != byte{'X'} || input[2] != byte{'X'} ||
			input[3] != byte{'P'})
			return sdk::unexpected(failure("magic", "mismatch"));
		const auto major = read_be(input, 4U, 2U);
		if (major != protocol_major)
			return sdk::unexpected(failure("protocol_major",
										   major < protocol_major ? "downgrade" : "unsupported",
										   "protocol-v2.downgrade-rejected"));
		const auto minor = static_cast<std::uint16_t>(read_be(input, 6U, 2U));
		if (!valid_minor(minor, bound))
			return sdk::unexpected(failure(
				"protocol_minor", "downgrade-or-unnegotiated", "protocol-v2.downgrade-rejected"));
		const auto type_id = static_cast<std::uint16_t>(read_be(input, 8U, 2U));
		if (!is_known_message_id(type_id))
			return sdk::unexpected(
				failure("message_type", "unknown", "protocol-v2.unknown-message"));
		const auto flags = static_cast<std::uint16_t>(read_be(input, 10U, 2U));
		if ((flags & static_cast<std::uint16_t>(~known_flags)) != 0U ||
			(flags & static_cast<std::uint16_t>(~bound.supported_flags)) != 0U)
			return sdk::unexpected(
				failure("flags", "unknown-or-not-negotiated", "protocol-v2.unknown-extension"));
		if ((flags & flag_compressed_payload) != 0U)
			return sdk::unexpected(failure(
				"flags", "compression-unimplemented", "protocol-v2.unsupported-compression"));
		if ((flags & (flag_required_extension | flag_optional_extension)) != 0U)
			return sdk::unexpected(
				failure("flags", "extension-unimplemented", "protocol-v2.unknown-extension"));
		const auto control_length = read_be(input, 28U, 4U);
		const auto payload_length = read_be(input, 32U, 8U);
		if (control_length > bound.max_control_bytes || payload_length > bound.max_payload_bytes ||
			control_length > std::numeric_limits<std::size_t>::max() ||
			payload_length > std::numeric_limits<std::size_t>::max())
			return sdk::unexpected(
				failure("length", "limit-exceeded", "protocol-v2.resource-limit"));
		const auto control_size = static_cast<std::size_t>(control_length);
		const auto payload_size = static_cast<std::size_t>(payload_length);
		if (control_size >
			std::numeric_limits<std::size_t>::max() - fixed_header_bytes - payload_size)
			return sdk::unexpected(failure("length", "overflow", "protocol-v2.resource-limit"));
		const auto total = fixed_header_bytes + control_size + payload_size;
		if (total > bound.max_frame_bytes || total != input.size())
			return sdk::unexpected(
				failure("frame", "length-mismatch", "protocol-v2.resource-limit"));
		frame output;
		output.protocol_major = static_cast<std::uint16_t>(major);
		output.protocol_minor = minor;
		output.type = static_cast<message_type>(type_id);
		output.flags = flags;
		output.stream_id = read_be(input, 12U, 8U);
		output.sequence = read_be(input, 20U, 8U);
		output.control.assign(input.begin() + static_cast<std::ptrdiff_t>(fixed_header_bytes),
							  input.begin() +
								  static_cast<std::ptrdiff_t>(fixed_header_bytes + control_size));
		output.payload.assign(input.begin() +
								  static_cast<std::ptrdiff_t>(fixed_header_bytes + control_size),
							  input.end());
		std::copy_n(
			input.begin() + 40, output.control_digest.size(), output.control_digest.begin());
		std::copy_n(
			input.begin() + 72, output.payload_digest.size(), output.payload_digest.begin());
		if (auto valid = validate_header_values(output, bound); !valid)
			return sdk::unexpected(valid.error());
		if (auto valid = check_digest(output.control, output.control_digest, "control"); !valid)
			return sdk::unexpected(valid.error());
		if (auto valid = check_digest(output.payload, output.payload_digest, "payload"); !valid)
			return sdk::unexpected(valid.error());
		if (auto valid = validate_canonical_control(output.control, bound); !valid)
			return sdk::unexpected(valid.error());
		return output;
	}

	sdk::result<std::vector<frame>> decode_frame_stream(const std::span<const byte> input,
														const limits bound,
														const std::uint64_t maximum_frames)
	{
		if (input.empty())
			return sdk::unexpected(failure("transcript", "empty"));
		std::vector<frame> output;
		output.reserve(std::min<std::size_t>(static_cast<std::size_t>(maximum_frames), 16U));
		std::size_t offset{};
		while (offset < input.size())
		{
			if (output.size() >= maximum_frames || input.size() - offset < fixed_header_bytes)
				return sdk::unexpected(failure(
					"transcript", "frame-count-or-truncation", "protocol-v2.resource-limit"));
			const auto frame_size = frame_size_from_header(input.subspan(offset));
			if (frame_size == std::numeric_limits<std::size_t>::max() ||
				frame_size < fixed_header_bytes || frame_size > input.size() - offset)
				return sdk::unexpected(
					failure("transcript", "frame-length", "protocol-v2.resource-limit"));
			auto decoded = decode_frame(input.subspan(offset, frame_size), bound);
			if (!decoded)
				return sdk::unexpected(decoded.error());
			output.push_back(std::move(*decoded));
			offset += frame_size;
		}
		return output;
	}

	sdk::result<void> sequence_guard::accept(const frame& value)
	{
		if (value.stream_id != stream_id_)
			return sdk::unexpected(
				failure("stream_id", "mismatch", "protocol-v2.sequence-invalid"));
		if (value.sequence < next_sequence_ ||
			(value.sequence == next_sequence_ &&
			 next_sequence_ == std::numeric_limits<std::uint64_t>::max()))
			return sdk::unexpected(failure("sequence", "replay", "protocol-v2.replay-rejected"));
		if (value.sequence != next_sequence_)
			return sdk::unexpected(
				failure("sequence", "gap-or-reorder", "protocol-v2.sequence-invalid"));
		if (next_sequence_ != std::numeric_limits<std::uint64_t>::max())
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
		if (value.control.size() > std::numeric_limits<std::uint64_t>::max() - value.payload.size())
			return sdk::unexpected(
				failure("credit.bytes", "overflow", "protocol-v2.credit-exceeded"));
		const auto bytes_used =
			static_cast<std::uint64_t>(value.control.size() + value.payload.size());
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
