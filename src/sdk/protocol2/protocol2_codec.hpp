#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk::protocol2
{
	inline constexpr std::uint16_t protocol_major = 2U;
	inline constexpr std::uint16_t protocol_minor = 0U;
	inline constexpr std::size_t frame_header_bytes = 104U;
	inline constexpr std::uint32_t maximum_control_bytes = 65'536U;
	inline constexpr std::uint64_t maximum_payload_bytes = 16U * 1024U * 1024U;
	inline constexpr std::uint16_t end_of_stream_flag = 8U;

	/** Protocol 2.0 message registry. IDs are semantic product identity, not source-byte identity.
	 */
	enum class message_type : std::uint16_t
	{
		hello = 1U,
		hello_ack = 2U,
		schema_negotiate = 3U,
		open_task = 4U,
		task_accepted = 5U,
		input_descriptor = 6U,
		input_chunk = 7U,
		credit = 8U,
		batch_begin = 9U,
		column_chunk = 10U,
		batch_end = 11U,
		batch_ack = 12U,
		batch_reject = 13U,
		coverage_chunk = 14U,
		unresolved_chunk = 15U,
		closure_candidate = 16U,
		progress = 17U,
		cancel = 18U,
		resume = 19U,
		task_complete = 20U,
		task_failed = 21U,
		close = 22U,
		heartbeat = 23U,
		source_closure_manifest = 24U,
		source_closure_blob = 25U,
		source_closure_chunk = 26U,
		source_closure_seal = 27U,
		source_closure_ack = 28U,
		source_closure_reject = 29U,
	};

	[[nodiscard]] constexpr bool is_known_message(const std::uint16_t value) noexcept
	{
		return value >= 1U && value <= 29U;
	}

	[[nodiscard]] constexpr bool is_source_closure_message(const message_type value) noexcept
	{
		return static_cast<std::uint16_t>(value) >= 24U && static_cast<std::uint16_t>(value) <= 29U;
	}

	[[nodiscard]] constexpr bool is_ng1_message(const message_type value) noexcept
	{
		return value == message_type::progress || value == message_type::resume ||
			value == message_type::heartbeat;
	}

	struct limits
	{
		std::uint32_t max_control_bytes{maximum_control_bytes};
		std::uint64_t max_payload_bytes{maximum_payload_bytes};
		std::uint16_t supported_flags{end_of_stream_flag};
	};

	struct frame
	{
		message_type type{message_type::hello};
		std::uint64_t stream_id{1U};
		std::uint64_t sequence{};
		std::uint16_t flags{};
		std::vector<std::byte> control;
		std::vector<std::byte> payload;
	};

	using control_value = std::variant<std::uint64_t, std::string>;
	struct control_field
	{
		std::string key;
		control_value value;
		[[nodiscard]] bool operator==(const control_field&) const = default;
	};

	/** Encode one strict deterministic closed-map CBOR control object. */
	[[nodiscard]] result<std::vector<std::byte>>
	encode_control(std::span<const control_field> fields);
	/** Decode one complete canonical closed-map CBOR control object. */
	[[nodiscard]] result<std::vector<control_field>>
	decode_control(std::span<const std::byte> bytes);

	/** Encode one Protocol 2.0 frame with independent SHA-256 checksums. */
	[[nodiscard]] result<std::vector<std::byte>> encode_frame(const frame& value,
															  limits negotiated = {});
	/** Decode and validate exactly one complete Protocol 2.0 frame. */
	[[nodiscard]] result<frame> decode_frame(std::span<const std::byte> bytes,
											 limits negotiated = {});

	[[nodiscard]] result<std::string> control_text(const std::vector<control_field>& fields,
												   std::string_view key);
	[[nodiscard]] result<std::uint64_t> control_uint(const std::vector<control_field>& fields,
													 std::string_view key);

	[[nodiscard]] error codec_error(std::string field, std::string detail);
} // namespace cxxlens::sdk::protocol2
