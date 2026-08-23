#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "json_internal.hpp"

namespace cxxlens::sdk
{
	namespace
	{
		void append_length(std::vector<std::byte>& output, const std::uint64_t value)
		{
			for (int shift = 56; shift >= 0; shift -= 8)
				output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
		}

		void append_canonical(std::vector<std::byte>& output, const canonical_value& value)
		{
			using kind = canonical_value::kind;
			switch (value.type)
			{
				case kind::null_value:
					output.push_back(std::byte{0x00});
					return;
				case kind::boolean:
					output.push_back(std::byte{0x01});
					output.push_back(value.boolean ? std::byte{0x01} : std::byte{0x00});
					return;
				case kind::signed_integer:
				{
					output.push_back(std::byte{0x02});
					output.push_back(value.integer < 0 ? std::byte{0x01} : std::byte{0x00});
					const auto magnitude = value.integer < 0
						? static_cast<std::uint64_t>(-(value.integer + 1)) + 1U
						: static_cast<std::uint64_t>(value.integer);
					std::size_t width = 1U;
					for (auto remaining = magnitude; remaining > 0xffU; remaining >>= 8U)
						++width;
					append_length(output, width);
					for (std::size_t index = width; index > 0U; --index)
						output.push_back(
							static_cast<std::byte>((magnitude >> ((index - 1U) * 8U)) & 0xffU));
					return;
				}
				case kind::bytes:
					output.push_back(std::byte{0x03});
					append_length(output, value.byte_string.size());
					output.insert(output.end(), value.byte_string.begin(), value.byte_string.end());
					return;
				case kind::utf8_string:
					output.push_back(std::byte{0x04});
					append_length(output, value.text.size());
					for (const auto byte : value.text)
						output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
					return;
				case kind::ordered_tuple:
					output.push_back(std::byte{0x05});
					append_length(output, value.tuple.size());
					for (const auto& item : value.tuple)
					{
						std::vector<std::byte> encoded;
						append_canonical(encoded, item);
						append_length(output, encoded.size());
						output.insert(output.end(), encoded.begin(), encoded.end());
					}
					return;
			}
		}

		[[nodiscard]] result<std::uint64_t>
		read_canonical_length(const std::span<const std::byte> bytes, std::size_t& offset)
		{
			if (bytes.size() - offset < 8U)
				return unexpected(
					error{"sdk.canonical-value-invalid", "binary", "truncated-length"});
			std::uint64_t value{};
			for (std::size_t index{}; index < 8U; ++index)
				value = (value << 8U) | std::to_integer<unsigned char>(bytes[offset + index]);
			offset += 8U;
			return value;
		}

		[[nodiscard]] result<canonical_value>
		decode_canonical_value(const std::span<const std::byte> bytes)
		{
			if (bytes.empty())
				return unexpected(error{"sdk.canonical-value-invalid", "binary", "missing-tag"});
			std::size_t offset{1U};
			const auto tag = std::to_integer<std::uint8_t>(bytes.front());
			canonical_value output;
			switch (tag)
			{
				case 0x00U:
					output = canonical_value::null();
					break;
				case 0x01U:
					if (offset == bytes.size() || std::to_integer<std::uint8_t>(bytes[offset]) > 1U)
						return unexpected(
							error{"sdk.canonical-value-invalid", "binary", "invalid-boolean"});
					output = canonical_value::from_boolean(
						std::to_integer<std::uint8_t>(bytes[offset++]) == 1U);
					break;
				case 0x02U:
				{
					if (offset == bytes.size() || std::to_integer<std::uint8_t>(bytes[offset]) > 1U)
						return unexpected(
							error{"sdk.canonical-value-invalid", "binary", "invalid-integer-sign"});
					const bool negative = std::to_integer<std::uint8_t>(bytes[offset++]) == 1U;
					auto width = read_canonical_length(bytes, offset);
					if (!width)
						return unexpected(std::move(width.error()));
					if (*width == 0U || *width > 8U || *width > bytes.size() - offset)
						return unexpected(error{
							"sdk.canonical-value-invalid", "binary", "invalid-integer-width"});
					if (*width > 1U && bytes[offset] == std::byte{0})
						return unexpected(
							error{"sdk.canonical-value-invalid", "binary", "noncanonical-integer"});
					std::uint64_t magnitude{};
					for (std::uint64_t index{}; index < *width; ++index)
						magnitude =
							(magnitude << 8U) | std::to_integer<unsigned char>(bytes[offset++]);
					constexpr auto signed_max =
						static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
					if ((!negative && magnitude > signed_max) ||
						(negative && (magnitude == 0U || magnitude > signed_max + 1U)))
						return unexpected(
							error{"sdk.canonical-value-invalid", "binary", "integer-out-of-range"});
					const auto value = negative
						? (magnitude == signed_max + 1U ? std::numeric_limits<std::int64_t>::min()
														: -static_cast<std::int64_t>(magnitude))
						: static_cast<std::int64_t>(magnitude);
					output = canonical_value::from_integer(value);
					break;
				}
				case 0x03U:
				case 0x04U:
				{
					auto length = read_canonical_length(bytes, offset);
					if (!length)
						return unexpected(std::move(length.error()));
					if (*length > bytes.size() - offset)
						return unexpected(
							error{"sdk.canonical-value-invalid", "binary", "truncated-payload"});
					const auto end = offset + static_cast<std::size_t>(*length);
					if (tag == 0x03U)
						output = canonical_value::from_bytes(std::vector<std::byte>{
							bytes.begin() + static_cast<std::ptrdiff_t>(offset),
							bytes.begin() + static_cast<std::ptrdiff_t>(end)});
					else
						output = canonical_value::from_string(
							std::string{reinterpret_cast<const char*>(bytes.data() + offset),
										static_cast<std::size_t>(*length)});
					offset = end;
					break;
				}
				case 0x05U:
				{
					auto count = read_canonical_length(bytes, offset);
					if (!count)
						return unexpected(std::move(count.error()));
					if (*count > (bytes.size() - offset) / 9U)
						return unexpected(
							error{"sdk.canonical-value-invalid", "binary", "invalid-tuple-count"});
					std::vector<canonical_value> values;
					values.reserve(static_cast<std::size_t>(*count));
					for (std::uint64_t index{}; index < *count; ++index)
					{
						auto length = read_canonical_length(bytes, offset);
						if (!length)
							return unexpected(std::move(length.error()));
						if (*length == 0U || *length > bytes.size() - offset)
							return unexpected(error{"sdk.canonical-value-invalid",
													"binary",
													"invalid-tuple-item-length"});
						auto item = decode_canonical_value(
							bytes.subspan(offset, static_cast<std::size_t>(*length)));
						if (!item)
							return unexpected(std::move(item.error()));
						values.push_back(std::move(*item));
						offset += static_cast<std::size_t>(*length);
					}
					output = canonical_value::from_tuple(std::move(values));
					break;
				}
				default:
					return unexpected(
						error{"sdk.canonical-value-invalid", "binary", "unknown-tag"});
			}
			if (offset != bytes.size())
				return unexpected(error{"sdk.canonical-value-invalid", "binary", "trailing-bytes"});
			if (auto valid = output.validate(); !valid)
				return unexpected(std::move(valid.error()));
			return output;
		}

		[[nodiscard]] error canonical_error(std::string field, std::string detail)
		{
			return {"sdk.canonical-value-invalid", std::move(field), std::move(detail)};
		}

		constexpr std::array<std::uint32_t, 64U> round_constants{
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

		[[nodiscard]] std::array<std::uint32_t, 8U> sha256_words(std::string_view input)
		{
			std::vector<std::uint8_t> bytes(input.begin(), input.end());
			const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
			bytes.push_back(0x80U);
			while (bytes.size() % 64U != 56U)
				bytes.push_back(0U);
			for (int shift = 56; shift >= 0; shift -= 8)
				bytes.push_back(static_cast<std::uint8_t>(bit_length >> shift));

			std::array<std::uint32_t, 8U> state{
				0x6a09e667U,
				0xbb67ae85U,
				0x3c6ef372U,
				0xa54ff53aU,
				0x510e527fU,
				0x9b05688cU,
				0x1f83d9abU,
				0x5be0cd19U,
			};
			for (std::size_t offset = 0U; offset < bytes.size(); offset += 64U)
			{
				std::array<std::uint32_t, 64U> words{};
				for (std::size_t index = 0U; index < 16U; ++index)
				{
					const auto byte = offset + index * 4U;
					words.at(index) = (static_cast<std::uint32_t>(bytes[byte]) << 24U) |
						(static_cast<std::uint32_t>(bytes[byte + 1U]) << 16U) |
						(static_cast<std::uint32_t>(bytes[byte + 2U]) << 8U) |
						static_cast<std::uint32_t>(bytes[byte + 3U]);
				}
				for (std::size_t index = 16U; index < words.size(); ++index)
				{
					const auto s0 = std::rotr(words.at(index - 15U), 7) ^
						std::rotr(words.at(index - 15U), 18) ^ (words.at(index - 15U) >> 3U);
					const auto s1 = std::rotr(words.at(index - 2U), 17) ^
						std::rotr(words.at(index - 2U), 19) ^ (words.at(index - 2U) >> 10U);
					words.at(index) = words.at(index - 16U) + s0 + words.at(index - 7U) + s1;
				}

				auto [a, b, c, d, e, f, g, h] = state;
				for (std::size_t index = 0U; index < words.size(); ++index)
				{
					const auto choose = (e & f) ^ ((~e) & g);
					const auto majority = (a & b) ^ (a & c) ^ (b & c);
					const auto upper_a = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
					const auto upper_e = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
					const auto first =
						h + upper_e + choose + round_constants.at(index) + words.at(index);
					const auto second = upper_a + majority;
					h = g;
					g = f;
					f = e;
					e = d + first;
					d = c;
					c = b;
					b = a;
					a = first + second;
				}
				state[0] += a;
				state[1] += b;
				state[2] += c;
				state[3] += d;
				state[4] += e;
				state[5] += f;
				state[6] += g;
				state[7] += h;
			}
			return state;
		}
	} // namespace

	std::string semantic_version::string() const
	{
		return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
	}

	canonical_value canonical_value::null()
	{
		return {};
	}

	canonical_value canonical_value::from_boolean(const bool value)
	{
		canonical_value output;
		output.type = kind::boolean;
		output.boolean = value;
		return output;
	}

	canonical_value canonical_value::from_integer(const std::int64_t value)
	{
		canonical_value output;
		output.type = kind::signed_integer;
		output.integer = value;
		return output;
	}

	canonical_value canonical_value::from_bytes(std::vector<std::byte> value)
	{
		canonical_value output;
		output.type = kind::bytes;
		output.byte_string = std::move(value);
		return output;
	}

	canonical_value canonical_value::from_string(std::string value)
	{
		canonical_value output;
		output.type = kind::utf8_string;
		output.text = std::move(value);
		return output;
	}

	canonical_value canonical_value::from_tuple(std::vector<canonical_value> value)
	{
		canonical_value output;
		output.type = kind::ordered_tuple;
		output.tuple = std::move(value);
		return output;
	}

	result<void> validate_utf8_text(const std::string_view value)
	{
		if (!detail::valid_utf8(value))
			return unexpected(error{"sdk.text-invalid", "value", "invalid-utf8"});
		return {};
	}

	result<void> validate_strong_id(const std::string_view value)
	{
		if (value.empty())
			return unexpected(error{"sdk.text-invalid", "value", "empty"});
		if (auto utf8 = validate_utf8_text(value); !utf8)
			return utf8;
		std::size_t scalar_count{};
		for (std::size_t index{}; index < value.size(); ++scalar_count)
		{
			const auto first = static_cast<unsigned char>(value[index]);
			if (first < 0x20U || first == 0x7fU)
				return unexpected(error{"sdk.text-invalid", "value", "control-character"});
			index += first <= 0x7fU ? 1U : first <= 0xdfU ? 2U : first <= 0xefU ? 3U : 4U;
		}
		if (scalar_count > 512U)
			return unexpected(error{"sdk.text-invalid", "value", "max-length"});
		return {};
	}

	result<void> validate_registered_symbol(const std::string_view value)
	{
		if (auto strong = validate_strong_id(value); !strong)
			return strong;
		if (value.size() < 2U || value.front() < 'a' || value.front() > 'z' ||
			!std::ranges::all_of(value.substr(1U),
								 [](const char byte)
								 {
									 return (byte >= 'a' && byte <= 'z') ||
										 (byte >= '0' && byte <= '9') || byte == '_' ||
										 byte == '.' || byte == '-';
								 }))
			return unexpected(error{"sdk.text-invalid", "value", "registered-symbol"});
		return {};
	}

	result<canonical_value> canonical_utf8_string(std::string value)
	{
		if (auto valid = validate_utf8_text(value); !valid)
			return unexpected(std::move(valid.error()));
		return canonical_value::from_string(std::move(value));
	}

	result<std::string> canonical_json_text(const std::string_view value)
	{
		if (auto valid = validate_utf8_text(value); !valid)
			return unexpected(std::move(valid.error()));
		return detail::canonical_json_string(value);
	}

	result<void> canonical_value::validate() const
	{
		if (!is_valid(type))
			return unexpected(canonical_error("type", "closed-enum"));
		const bool common_inactive =
			boolean || integer != 0 || !byte_string.empty() || !text.empty() || !tuple.empty();
		switch (type)
		{
			case kind::null_value:
				if (common_inactive)
					return unexpected(canonical_error("payload", "inactive-field"));
				break;
			case kind::boolean:
				if (integer != 0 || !byte_string.empty() || !text.empty() || !tuple.empty())
					return unexpected(canonical_error("payload", "inactive-field"));
				break;
			case kind::signed_integer:
				if (boolean || !byte_string.empty() || !text.empty() || !tuple.empty())
					return unexpected(canonical_error("payload", "inactive-field"));
				break;
			case kind::bytes:
				if (boolean || integer != 0 || !text.empty() || !tuple.empty())
					return unexpected(canonical_error("payload", "inactive-field"));
				break;
			case kind::utf8_string:
				if (boolean || integer != 0 || !byte_string.empty() || !tuple.empty())
					return unexpected(canonical_error("payload", "inactive-field"));
				if (!detail::valid_utf8(text))
					return unexpected(canonical_error("text", "invalid-utf8"));
				break;
			case kind::ordered_tuple:
				if (boolean || integer != 0 || !byte_string.empty() || !text.empty())
					return unexpected(canonical_error("payload", "inactive-field"));
				for (std::size_t index{}; index < tuple.size(); ++index)
					if (auto valid = tuple[index].validate(); !valid)
						return unexpected(canonical_error("tuple[" + std::to_string(index) + "]",
														  valid.error().detail));
				break;
		}
		return {};
	}

	result<std::vector<std::byte>> canonical_binary(const canonical_value& value)
	{
		if (auto valid = value.validate(); !valid)
			return unexpected(std::move(valid.error()));
		std::vector<std::byte> output;
		append_canonical(output, value);
		return output;
	}

	result<canonical_value> canonical_binary_decode(const std::span<const std::byte> bytes)
	{
		auto decoded = decode_canonical_value(bytes);
		if (!decoded)
			return unexpected(std::move(decoded.error()));
		auto encoded = canonical_binary(*decoded);
		if (!encoded)
			return unexpected(std::move(encoded.error()));
		if (*encoded != std::vector<std::byte>{bytes.begin(), bytes.end()})
			return unexpected(
				error{"sdk.canonical-value-invalid", "binary", "noncanonical-encoding"});
		return decoded;
	}

	result<std::string> canonical_identity_digest(const std::string_view identity_kind,
												  const std::span<const canonical_value> fields)
	{
		if (identity_kind.empty() || !detail::valid_utf8(identity_kind))
			return unexpected(canonical_error("identity_kind", "invalid-utf8-or-empty"));
		std::string domain{"cxxlens"};
		domain.push_back('\0');
		domain.append(identity_kind);
		domain.append("\0v1\0", 4U);
		std::vector<canonical_value> values{fields.begin(), fields.end()};
		auto encoded = canonical_binary(canonical_value::from_tuple(std::move(values)));
		if (!encoded)
			return unexpected(std::move(encoded.error()));
		std::vector<std::byte> bytes;
		bytes.reserve(domain.size() + encoded->size());
		for (const auto byte : domain)
			bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		bytes.insert(bytes.end(), encoded->begin(), encoded->end());
		return std::string{identity_kind} + ':' + content_digest(bytes);
	}

	result<std::string> source_span_identity(const std::string_view source_snapshot,
											 const std::string_view file,
											 const std::uint64_t begin,
											 const std::uint64_t end,
											 const std::string_view role)
	{
		if (!validate_strong_id(source_snapshot) || !validate_strong_id(file) ||
			!validate_strong_id(role) || end < begin ||
			begin > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
			end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
			return unexpected(error{"sdk.source-span-identity-invalid", "projection", {}});
		return canonical_identity_digest(
			"source-span",
			std::array{
				canonical_value::from_string(std::string{source_snapshot}),
				canonical_value::from_string(std::string{file}),
				canonical_value::from_integer(static_cast<std::int64_t>(begin)),
				canonical_value::from_integer(static_cast<std::int64_t>(end)),
				canonical_value::from_string(std::string{role}),
			});
	}

	// The public order is part of the v2 contract and both views have intentionally distinct roles.
	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	result<std::string> semantic_digest(const std::string_view domain, const std::string_view bytes)
	{
		if (domain.empty() || domain.front() < 'a' || domain.front() > 'z')
			return unexpected(error{"sdk.semantic-domain-invalid", "domain", std::string{domain}});
		for (const auto byte : domain)
		{
			const bool valid = (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
				byte == '.' || byte == '_' || byte == '-';
			if (!valid)
				return unexpected(
					error{"sdk.semantic-domain-invalid", "domain", std::string{domain}});
		}

		std::vector<std::byte> payload;
		payload.reserve(bytes.size());
		for (const auto byte : bytes)
			payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		auto framed = canonical_binary(
			canonical_value::from_tuple({canonical_value::from_string("cxxlens-semantic-digest-v2"),
										 canonical_value::from_string(std::string{domain}),
										 canonical_value::from_bytes(std::move(payload))}));
		if (!framed)
			return unexpected(std::move(framed.error()));
		std::string hash_input;
		hash_input.reserve(framed->size());
		for (const auto byte : *framed)
			hash_input.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
		const auto state = sha256_words(hash_input);
		std::ostringstream output;
		output << "semantic-v2:sha256:" << std::hex << std::setfill('0');
		for (const auto value : state)
			output << std::setw(8) << value;
		return output.str();
	}

	std::string content_digest(const std::span<const std::byte> bytes)
	{
		std::string content;
		content.reserve(bytes.size());
		for (const auto byte : bytes)
			content.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
		const auto state = sha256_words(content);
		std::ostringstream output;
		output << "sha256:" << std::hex << std::setfill('0');
		for (const auto value : state)
			output << std::setw(8) << value;
		return output.str();
	}
} // namespace cxxlens::sdk
