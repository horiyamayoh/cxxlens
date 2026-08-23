#include "protocol2_codec.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <type_traits>

namespace cxxlens::sdk::protocol2
{
	namespace
	{
		enum class major_type : std::uint8_t
		{
			unsigned_integer = 0U,
			text = 3U,
			map = 5U,
		};

		void append_u64(std::vector<std::byte>& output, const std::uint64_t value,
			const std::size_t width)
		{
			for (std::size_t index = width; index > 0U; --index)
				output.push_back(static_cast<std::byte>(value >> ((index - 1U) * 8U)));
		}

		void append_head(std::vector<std::byte>& output, const major_type major,
			const std::uint64_t value)
		{
			const auto prefix = static_cast<std::uint8_t>(static_cast<std::uint8_t>(major) << 5U);
			if (value < 24U)
				output.push_back(static_cast<std::byte>(prefix | static_cast<std::uint8_t>(value)));
			else if (value <= std::numeric_limits<std::uint8_t>::max())
			{
				output.push_back(static_cast<std::byte>(prefix | 24U));
				append_u64(output, value, 1U);
			}
			else if (value <= std::numeric_limits<std::uint16_t>::max())
			{
				output.push_back(static_cast<std::byte>(prefix | 25U));
				append_u64(output, value, 2U);
			}
			else if (value <= std::numeric_limits<std::uint32_t>::max())
			{
				output.push_back(static_cast<std::byte>(prefix | 26U));
				append_u64(output, value, 4U);
			}
			else
			{
				output.push_back(static_cast<std::byte>(prefix | 27U));
				append_u64(output, value, 8U);
			}
		}

		[[nodiscard]] std::vector<std::byte> encoded_text(const std::string_view value)
		{
			std::vector<std::byte> output;
			output.reserve(value.size() + 9U);
			append_head(output, major_type::text, value.size());
			for (const auto byte : value)
				output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
			return output;
		}

		struct encoded_field
		{
			std::vector<std::byte> key;
			std::vector<std::byte> value;
		};

		[[nodiscard]] bool canonical_key_less(const encoded_field& left,
			const encoded_field& right) noexcept
		{
			if (left.key.size() != right.key.size())
				return left.key.size() < right.key.size();
			return std::lexicographical_compare(left.key.begin(), left.key.end(), right.key.begin(),
				right.key.end());
		}

		[[nodiscard]] result<std::pair<std::uint64_t, std::size_t>>
		read_argument(const std::span<const std::byte> bytes, const std::size_t offset,
			const std::uint8_t additional)
		{
			if (additional < 24U)
				return std::pair{static_cast<std::uint64_t>(additional), offset};
			const auto width = additional == 24U ? 1U : additional == 25U ? 2U
				: additional == 26U             ? 4U
				: additional == 27U             ? 8U
				                               : 0U;
			if (width == 0U || offset > bytes.size() || width > bytes.size() - offset)
				return unexpected(codec_error("cbor", "truncated-or-indefinite"));
			std::uint64_t value{};
			for (std::size_t index{}; index < width; ++index)
				value = (value << 8U) | std::to_integer<std::uint64_t>(bytes[offset + index]);
			const auto minimum = width == 1U ? 24U : width == 2U ? 256U
				: width == 4U            ? 65'536U
				                          : (std::uint64_t{1U} << 32U);
			if (value < minimum)
				return unexpected(codec_error("cbor", "non-shortest"));
			return std::pair{value, offset + width};
		}

		[[nodiscard]] result<std::pair<std::string, std::size_t>>
		read_text(const std::span<const std::byte> bytes, const std::size_t offset,
			const std::uint8_t additional)
		{
			auto length = read_argument(bytes, offset, additional);
			if (!length)
				return unexpected(std::move(length.error()));
			if (length->first > bytes.size() - length->second)
				return unexpected(codec_error("cbor", "text-length"));
			std::string value{reinterpret_cast<const char*>(bytes.data() + length->second),
				static_cast<std::size_t>(length->first)};
			if (auto valid = validate_utf8_text(value); !valid)
				return unexpected(codec_error("cbor", "invalid-utf8"));
			return std::pair{std::move(value), length->second + static_cast<std::size_t>(length->first)};
		}

		[[nodiscard]] result<std::pair<control_value, std::size_t>>
		read_scalar(const std::span<const std::byte> bytes, const std::size_t offset)
		{
			if (offset >= bytes.size())
				return unexpected(codec_error("cbor", "truncated"));
			const auto initial = std::to_integer<std::uint8_t>(bytes[offset]);
			const auto major = static_cast<major_type>(initial >> 5U);
			if (major == major_type::unsigned_integer)
			{
				auto argument = read_argument(bytes, offset + 1U, initial & 0x1fU);
				if (!argument)
					return unexpected(std::move(argument.error()));
				return std::pair{control_value{argument->first}, argument->second};
			}
			if (major == major_type::text)
			{
				auto value = read_text(bytes, offset + 1U, initial & 0x1fU);
				if (!value)
					return unexpected(std::move(value.error()));
				return std::pair{control_value{std::move(value->first)}, value->second};
			}
			return unexpected(codec_error("cbor", "scalar-type"));
		}

		[[nodiscard]] std::array<std::byte, 32U> digest_bytes(std::string digest)
		{
			std::array<std::byte, 32U> output{};
			if (digest.starts_with("sha256:"))
				digest.erase(0U, 7U);
			if (digest.size() != output.size() * 2U)
				return output;
			for (std::size_t index{}; index < output.size(); ++index)
			{
				const auto hex = [](const char value) -> std::uint8_t
				{
					if (value >= '0' && value <= '9')
						return static_cast<std::uint8_t>(value - '0');
					if (value >= 'a' && value <= 'f')
						return static_cast<std::uint8_t>(value - 'a' + 10);
					return 0U;
				};
				output[index] = static_cast<std::byte>((hex(digest[index * 2U]) << 4U) |
					hex(digest[index * 2U + 1U]));
			}
			return output;
		}

		template <class T>
		void append_be(std::vector<std::byte>& output, const T value)
		{
			for (std::size_t index = sizeof(T); index > 0U; --index)
				output.push_back(static_cast<std::byte>(value >> ((index - 1U) * 8U)));
		}

		template <class T>
		[[nodiscard]] T read_be(const std::span<const std::byte> bytes, const std::size_t offset)
		{
			T value{};
			for (std::size_t index{}; index < sizeof(T); ++index)
				value = static_cast<T>((static_cast<std::uint64_t>(value) << 8U) |
					std::to_integer<std::uint64_t>(bytes[offset + index]));
			return value;
		}

	} // namespace

	error codec_error(std::string field, std::string detail)
	{
		return {"provider.protocol-v2-invalid", std::move(field), std::move(detail)};
	}

	result<std::vector<std::byte>> encode_control(const std::span<const control_field> fields)
	{
		if (fields.size() > 4096U)
			return unexpected(codec_error("control", "field-count-limit"));
		std::set<std::string, std::less<>> keys;
		std::vector<encoded_field> encoded;
		encoded.reserve(fields.size());
		for (const auto& field : fields)
		{
			if (field.key.empty() || !keys.insert(field.key).second)
				return unexpected(codec_error("control", "duplicate-or-empty-key"));
			if (auto valid = validate_utf8_text(field.key); !valid)
				return unexpected(codec_error(field.key, "invalid-utf8"));
			encoded_field item{encoded_text(field.key), {}};
			std::visit(
				[&item](const auto& value)
				{
					using value_type = std::remove_cvref_t<decltype(value)>;
					if constexpr (std::is_same_v<value_type, std::string>)
						item.value = encoded_text(value);
					else
					{
						append_head(item.value, major_type::unsigned_integer, value);
					}
				},
				field.value);
			if (const auto* text = std::get_if<std::string>(&field.value); text != nullptr)
				if (auto valid = validate_utf8_text(*text); !valid)
					return unexpected(codec_error(field.key, "invalid-utf8"));
			encoded.push_back(std::move(item));
		}
		std::ranges::sort(encoded, canonical_key_less);
		std::vector<std::byte> output;
		append_head(output, major_type::map, encoded.size());
		for (const auto& field : encoded)
		{
			output.insert(output.end(), field.key.begin(), field.key.end());
			output.insert(output.end(), field.value.begin(), field.value.end());
		}
		if (output.size() > maximum_control_bytes)
			return unexpected(codec_error("control", "limit-exceeded"));
		return output;
	}

	result<std::vector<control_field>> decode_control(const std::span<const std::byte> bytes)
	{
		if (bytes.empty() || bytes.size() > maximum_control_bytes)
			return unexpected(codec_error("control", "size"));
		const auto initial = std::to_integer<std::uint8_t>(bytes.front());
		if (static_cast<major_type>(initial >> 5U) != major_type::map)
			return unexpected(codec_error("cbor", "not-map"));
		auto count = read_argument(bytes, 1U, initial & 0x1fU);
		if (!count || count->first > 4096U)
			return unexpected(codec_error("cbor", "map-count"));
		std::vector<control_field> output;
		output.reserve(static_cast<std::size_t>(count->first));
		std::vector<std::byte> previous_key;
		std::set<std::string, std::less<>> keys;
		std::size_t offset = count->second;
		for (std::uint64_t index{}; index < count->first; ++index)
		{
			if (offset >= bytes.size())
				return unexpected(codec_error("cbor", "truncated-key"));
			const auto key_start = offset;
			const auto initial_key = std::to_integer<std::uint8_t>(bytes[offset++]);
			if (static_cast<major_type>(initial_key >> 5U) != major_type::text)
				return unexpected(codec_error("cbor", "key-not-text"));
			auto key = read_text(bytes, offset, initial_key & 0x1fU);
			if (!key)
				return unexpected(std::move(key.error()));
			offset = key->second;
			std::vector<std::byte> encoded_key{bytes.begin() + static_cast<std::ptrdiff_t>(key_start),
				bytes.begin() + static_cast<std::ptrdiff_t>(offset)};
			if (!previous_key.empty() && !canonical_key_less(
					encoded_field{previous_key, {}}, encoded_field{encoded_key, {}}))
				return unexpected(codec_error("cbor", "noncanonical-map-order"));
			previous_key = encoded_key;
			if (!keys.insert(key->first).second)
				return unexpected(codec_error("cbor", "duplicate-key"));
			auto value = read_scalar(bytes, offset);
			if (!value)
				return unexpected(std::move(value.error()));
			offset = value->second;
			output.push_back({std::move(key->first), std::move(value->first)});
		}
		if (offset != bytes.size())
			return unexpected(codec_error("cbor", "trailing-bytes"));
		return output;
	}

	result<std::vector<std::byte>> encode_frame(const frame& value, const limits negotiated)
	{
		if (value.stream_id == 0U || value.flags & ~negotiated.supported_flags)
			return unexpected(codec_error("header", "stream-or-flags"));
		if (value.control.size() > negotiated.max_control_bytes ||
			value.payload.size() > negotiated.max_payload_bytes)
			return unexpected(codec_error("header", "length-limit"));
		std::vector<std::byte> output;
		output.reserve(frame_header_bytes + value.control.size() + value.payload.size());
		for (const auto byte : std::array<std::byte, 4U>{std::byte{'C'}, std::byte{'X'}, std::byte{'X'},
			std::byte{'P'}})
			output.push_back(byte);
		append_be(output, protocol_major);
		append_be(output, protocol_minor);
		append_be(output, static_cast<std::uint16_t>(value.type));
		append_be(output, value.flags);
		append_be(output, value.stream_id);
		append_be(output, value.sequence);
		append_be(output, static_cast<std::uint32_t>(value.control.size()));
		append_be(output, static_cast<std::uint64_t>(value.payload.size()));
		const auto control_digest = digest_bytes(content_digest(value.control));
		const auto payload_digest = digest_bytes(content_digest(value.payload));
		output.insert(output.end(), control_digest.begin(), control_digest.end());
		output.insert(output.end(), payload_digest.begin(), payload_digest.end());
		output.insert(output.end(), value.control.begin(), value.control.end());
		output.insert(output.end(), value.payload.begin(), value.payload.end());
		return output;
	}

	result<frame> decode_frame(const std::span<const std::byte> bytes, const limits negotiated)
	{
		if (bytes.size() < frame_header_bytes)
			return unexpected(codec_error("header", "truncated"));
		if (bytes[0] != std::byte{'C'} || bytes[1] != std::byte{'X'} || bytes[2] != std::byte{'X'} ||
			bytes[3] != std::byte{'P'})
			return unexpected(codec_error("header", "magic"));
		if (read_be<std::uint16_t>(bytes, 4U) != protocol_major ||
			read_be<std::uint16_t>(bytes, 6U) != protocol_minor)
			return unexpected(codec_error("header", "protocol-version"));
		const auto flags = read_be<std::uint16_t>(bytes, 10U);
		if (flags & ~negotiated.supported_flags)
			return unexpected(codec_error("header", "reserved-flags"));
		const auto control_size = read_be<std::uint32_t>(bytes, 28U);
		const auto payload_size = read_be<std::uint64_t>(bytes, 32U);
		if (control_size > negotiated.max_control_bytes || payload_size > negotiated.max_payload_bytes)
			return unexpected(codec_error("header", "length-limit"));
		if (payload_size > std::numeric_limits<std::size_t>::max() - control_size ||
			frame_header_bytes + static_cast<std::size_t>(control_size) +
				static_cast<std::size_t>(payload_size) != bytes.size())
			return unexpected(codec_error("header", "frame-length"));
		const auto stream_id = read_be<std::uint64_t>(bytes, 12U);
		if (stream_id == 0U)
			return unexpected(codec_error("header", "stream-id"));
		const auto control = bytes.subspan(frame_header_bytes, control_size);
		const auto payload = bytes.subspan(frame_header_bytes + control_size, payload_size);
		const auto expected_control_digest = digest_bytes(content_digest(control));
		const auto expected_payload_digest = digest_bytes(content_digest(payload));
		if (!std::equal(expected_control_digest.begin(), expected_control_digest.end(), bytes.begin() + 40))
			return unexpected(codec_error("header", "control-checksum"));
		if (!std::equal(expected_payload_digest.begin(), expected_payload_digest.end(), bytes.begin() + 72))
			return unexpected(codec_error("header", "payload-checksum"));
		frame output;
		output.type = static_cast<message_type>(read_be<std::uint16_t>(bytes, 8U));
		output.stream_id = stream_id;
		output.sequence = read_be<std::uint64_t>(bytes, 20U);
		output.flags = flags;
		output.control.assign(control.begin(), control.end());
		output.payload.assign(payload.begin(), payload.end());
		return output;
	}

	result<std::string> control_text(const std::vector<control_field>& fields,
		const std::string_view key)
	{
		for (const auto& field : fields)
			if (field.key == key)
				if (const auto* value = std::get_if<std::string>(&field.value); value != nullptr)
					return *value;
		return unexpected(codec_error(std::string{key}, "missing-or-not-text"));
	}

	result<std::uint64_t> control_uint(const std::vector<control_field>& fields,
		const std::string_view key)
	{
		for (const auto& field : fields)
			if (field.key == key)
				if (const auto* value = std::get_if<std::uint64_t>(&field.value); value != nullptr)
					return *value;
		return unexpected(codec_error(std::string{key}, "missing-or-not-uint"));
	}
} // namespace cxxlens::sdk::protocol2
