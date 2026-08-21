#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace cxxlens::test::provider_timeout
{
	/**
	 * Identity-bound readiness record used only by the pipe-holding timeout fixture.
	 *
	 * The record is deliberately encoded field-by-field rather than by writing a C++ object
	 * representation.  It is therefore fixed-width, endian-stable, and remains below
	 * PIPE_BUF on the Linux hosts on which the fixture is admitted.
	 */
	struct readiness_record
	{
		std::uint64_t nonce{};
		std::uint32_t direct_pid{};
		std::uint32_t process_group{};
		std::uint64_t direct_start_time{};
		std::uint32_t holder_pid{};
		std::uint64_t holder_start_time{};
		std::uint32_t sentinel_pid{};
		std::uint64_t sentinel_start_time{};
	};

	inline constexpr std::uint32_t magic = 0x43584c52U; // "CXLR"
	inline constexpr std::uint16_t version = 1U;
	inline constexpr std::size_t encoded_bytes = 56U;

	inline void append_u16(std::array<std::byte, encoded_bytes>& output,
						   std::size_t& offset,
						   const std::uint16_t value) noexcept
	{
		output[offset++] = static_cast<std::byte>((value >> 8U) & 0xffU);
		output[offset++] = static_cast<std::byte>(value & 0xffU);
	}

	inline void append_u32(std::array<std::byte, encoded_bytes>& output,
						   std::size_t& offset,
						   const std::uint32_t value) noexcept
	{
		for (std::size_t index = sizeof(value); index > 0U; --index)
			output[offset++] = static_cast<std::byte>(value >> ((index - 1U) * 8U));
	}

	inline void append_u64(std::array<std::byte, encoded_bytes>& output,
						   std::size_t& offset,
						   const std::uint64_t value) noexcept
	{
		for (std::size_t index = sizeof(value); index > 0U; --index)
			output[offset++] = static_cast<std::byte>(value >> ((index - 1U) * 8U));
	}

	[[nodiscard]] inline std::uint16_t read_u16(const std::span<const std::byte> input,
												const std::size_t offset) noexcept
	{
		return static_cast<std::uint16_t>(
			(static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset])) << 8U) |
			static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset + 1U])));
	}

	[[nodiscard]] inline std::uint32_t read_u32(const std::span<const std::byte> input,
												const std::size_t offset) noexcept
	{
		std::uint32_t value{};
		for (std::size_t index{}; index < sizeof(value); ++index)
			value = (value << 8U) |
				static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + index]));
		return value;
	}

	[[nodiscard]] inline std::uint64_t read_u64(const std::span<const std::byte> input,
												const std::size_t offset) noexcept
	{
		std::uint64_t value{};
		for (std::size_t index{}; index < sizeof(value); ++index)
			value = (value << 8U) |
				static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[offset + index]));
		return value;
	}

	[[nodiscard]] inline std::array<std::byte, encoded_bytes>
	encode(const readiness_record& record) noexcept
	{
		std::array<std::byte, encoded_bytes> output{};
		std::size_t offset{};
		append_u32(output, offset, magic);
		append_u16(output, offset, version);
		append_u16(output, offset, static_cast<std::uint16_t>(encoded_bytes));
		append_u64(output, offset, record.nonce);
		append_u32(output, offset, record.direct_pid);
		append_u32(output, offset, record.process_group);
		append_u64(output, offset, record.direct_start_time);
		append_u32(output, offset, record.holder_pid);
		append_u64(output, offset, record.holder_start_time);
		append_u32(output, offset, record.sentinel_pid);
		append_u64(output, offset, record.sentinel_start_time);
		return output;
	}

	[[nodiscard]] inline std::optional<readiness_record>
	decode(const std::span<const std::byte> input) noexcept
	{
		if (input.size() != encoded_bytes || read_u32(input, 0U) != magic ||
			read_u16(input, 4U) != version || read_u16(input, 6U) != encoded_bytes)
			return std::nullopt;
		readiness_record record{read_u64(input, 8U),
								read_u32(input, 16U),
								read_u32(input, 20U),
								read_u64(input, 24U),
								read_u32(input, 32U),
								read_u64(input, 36U),
								read_u32(input, 44U),
								read_u64(input, 48U)};
		if (record.nonce == 0U || record.direct_pid == 0U || record.process_group == 0U ||
			record.direct_start_time == 0U || record.holder_pid == 0U ||
			record.holder_start_time == 0U || record.sentinel_pid == 0U ||
			record.sentinel_start_time == 0U)
			return std::nullopt;
		return record;
	}
} // namespace cxxlens::test::provider_timeout
