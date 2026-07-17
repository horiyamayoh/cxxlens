#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

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
						const auto encoded = canonical_binary(item);
						append_length(output, encoded.size());
						output.insert(output.end(), encoded.begin(), encoded.end());
					}
					return;
			}
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

	std::vector<std::byte> canonical_binary(const canonical_value& value)
	{
		std::vector<std::byte> output;
		append_canonical(output, value);
		return output;
	}

	std::string canonical_identity_digest(const std::string_view identity_kind,
										  const std::span<const canonical_value> fields)
	{
		std::string domain{"cxxlens"};
		domain.push_back('\0');
		domain.append(identity_kind);
		domain.append("\0v1\0", 4U);
		std::vector<canonical_value> values{fields.begin(), fields.end()};
		const auto encoded = canonical_binary(canonical_value::from_tuple(std::move(values)));
		std::vector<std::byte> bytes;
		bytes.reserve(domain.size() + encoded.size());
		for (const auto byte : domain)
			bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		bytes.insert(bytes.end(), encoded.begin(), encoded.end());
		return std::string{identity_kind} + ':' + content_digest(bytes);
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
		std::string hash_input;
		hash_input.reserve(framed.size());
		for (const auto byte : framed)
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
