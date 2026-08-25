#pragma once

/**
 * @file codec.hpp
 * @brief Bounded protocol-2 frame codec and transcript guards.
 *
 * Protocol 2 intentionally lives beside, rather than inside, the existing
 * provider runtime.  Its wire contract is fixed at a 104-byte big-endian
 * header and exact version 2.0. Unknown optional message IDs are decoded only
 * after their complete frame has passed bounds, checksum, and canonical-CBOR
 * validation so callers can account and skip them without a second codec.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "cbor.hpp"

namespace cxxlens::protocol_v2
{
	using byte = std::byte;
	using bytes = std::vector<byte>;

	inline constexpr std::uint16_t protocol_major = 2U;
	inline constexpr std::size_t fixed_header_bytes = 104U;
	inline constexpr std::size_t max_control_bytes = 65'536U;
	inline constexpr std::size_t max_payload_bytes = 16'777'216U;
	inline constexpr std::size_t max_closure_payload_bytes = 1'048'576U;
	inline constexpr std::size_t max_frame_bytes =
		fixed_header_bytes + max_control_bytes + max_payload_bytes;
	inline constexpr std::uint16_t heartbeat_message_id = 23U;
	inline constexpr std::uint16_t closure_first_message_id = 24U;
	inline constexpr std::uint16_t closure_last_message_id = 29U;

	/** @brief Protocol 2 registry, including the six source-closure controls. */
	enum class message_type : std::uint16_t // NOLINT(performance-enum-size)
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
		heartbeat = heartbeat_message_id,
		source_closure_manifest = 24U,
		source_closure_blob = 25U,
		source_closure_chunk = 26U,
		source_closure_seal = 27U,
		source_closure_ack = 28U,
		source_closure_reject = 29U,
	};

	[[nodiscard]] constexpr bool is_closure_message(const message_type type) noexcept
	{
		const auto id = static_cast<std::uint16_t>(type);
		return id >= closure_first_message_id && id <= closure_last_message_id;
	}

	[[nodiscard]] constexpr bool is_known_message_id(const std::uint16_t id) noexcept
	{
		return id >= 1U && id <= closure_last_message_id;
	}

	[[nodiscard]] constexpr bool is_known_message_type(const message_type type) noexcept
	{
		return is_known_message_id(static_cast<std::uint16_t>(type));
	}

	/** @brief Closed frame flags; extensions and compression are not implicit. */
	inline constexpr std::uint16_t flag_required_extension = 1U;
	inline constexpr std::uint16_t flag_optional_extension = 2U;
	inline constexpr std::uint16_t flag_compressed_payload = 4U;
	inline constexpr std::uint16_t flag_end_of_stream = 8U;
	inline constexpr std::uint16_t known_flags = flag_required_extension | flag_optional_extension |
		flag_compressed_payload | flag_end_of_stream;

	/** @brief Exact SHA-256 bytes stored in the fixed frame header. */
	using digest32 = std::array<byte, 32U>;

	/** @brief Negotiated limits are checked before any wire allocation. */
	struct limits
	{
		std::uint16_t minimum_minor{};
		std::uint16_t maximum_minor{};
		std::uint16_t supported_flags{flag_end_of_stream};
		std::size_t max_control_bytes{protocol_v2::max_control_bytes};
		std::size_t max_payload_bytes{protocol_v2::max_payload_bytes};
		std::size_t max_frame_bytes{protocol_v2::max_frame_bytes};
	};

	/** @brief Decoded or to-be-encoded protocol 2 frame. */
	struct frame
	{
		message_type type{message_type::hello};
		std::uint16_t protocol_major{protocol_v2::protocol_major};
		std::uint16_t protocol_minor{};
		std::uint16_t flags{};
		std::uint64_t stream_id{1U};
		std::uint64_t sequence{};
		bytes control;
		bytes payload;
		digest32 control_digest{};
		digest32 payload_digest{};

		[[nodiscard]] bool operator==(const frame&) const = default;
	};

	/**
	 * @brief Move-only, header-validated decode state for direct body ownership.
	 *
	 * A transport first supplies the exact 104-byte header, allocates the two
	 * final vectors at the validated sizes, and reads directly into them. The
	 * rvalue-only finalizer verifies exact sizes, both digests, and canonical
	 * control CBOR before moving those same buffers into the decoded frame.
	 */
	class prepared_frame_decode
	{
	  public:
		prepared_frame_decode(const prepared_frame_decode&) = delete;
		prepared_frame_decode& operator=(const prepared_frame_decode&) = delete;
		prepared_frame_decode(prepared_frame_decode&& other) noexcept;
		prepared_frame_decode& operator=(prepared_frame_decode&& other) noexcept;

		[[nodiscard]] std::size_t control_bytes() const noexcept
		{
			return control_bytes_;
		}
		[[nodiscard]] std::size_t payload_bytes() const noexcept
		{
			return payload_bytes_;
		}
		[[nodiscard]] std::size_t body_resident_bytes() const noexcept
		{
			return control_bytes_ + payload_bytes_;
		}
		[[nodiscard]] std::size_t encoded_bytes() const noexcept
		{
			return fixed_header_bytes + body_resident_bytes();
		}
		[[nodiscard]] message_type type() const noexcept
		{
			return header_.type;
		}
		[[nodiscard]] std::uint16_t flags() const noexcept
		{
			return header_.flags;
		}
		[[nodiscard]] std::uint64_t stream_id() const noexcept
		{
			return header_.stream_id;
		}
		[[nodiscard]] std::uint64_t sequence() const noexcept
		{
			return header_.sequence;
		}

		[[nodiscard]] sdk::result<frame> finalize(bytes&& control, bytes&& payload) &&;

	  private:
		friend sdk::result<prepared_frame_decode>
			prepare_frame_header(std::span<const byte, fixed_header_bytes>, limits);

		prepared_frame_decode(frame header,
							  std::size_t control_bytes,
							  std::size_t payload_bytes,
							  limits bound) noexcept;

		frame header_;
		std::size_t control_bytes_{};
		std::size_t payload_bytes_{};
		limits bound_{};
		bool consumed_{};
	};

	/** @brief SHA-256 of exact bytes (without a semantic-domain prefix). */
	class sha256_workspace final
	{
	  public:
		sha256_workspace() noexcept;
		void update(std::span<const byte> input) noexcept;
		[[nodiscard]] digest32 finish() noexcept;

	  private:
		[[nodiscard]] static constexpr std::uint32_t rotate_right(std::uint32_t value,
																  std::uint32_t amount) noexcept
		{
			return (value >> amount) | (value << (32U - amount));
		}
		void transform(const std::uint8_t* input) noexcept;

		std::array<std::uint32_t, 8U> state_{};
		std::array<std::uint8_t, 64U> buffer_{};
		std::array<std::uint32_t, 64U> schedule_{};
		std::array<std::uint32_t, 8U> working_{};
		std::size_t buffer_size_{};
		std::uint64_t bit_length_{};
	};

	inline constexpr std::size_t sha256_workspace_bytes = sizeof(sha256_workspace);

	[[nodiscard]] digest32 sha256(std::span<const byte> input) noexcept;

	[[nodiscard]] bool digest_is_zero(const digest32& digest) noexcept;
	[[nodiscard]] bool digest_equal(const digest32& left, const digest32& right) noexcept;

	/** @brief Validate one exact wire header before any body allocation. */
	[[nodiscard]] sdk::result<prepared_frame_decode>
	prepare_frame_header(std::span<const byte, fixed_header_bytes> header, limits bound = {});

	/** @brief Encode a canonical-control frame with independent full SHA-256 digests. */
	[[nodiscard]] sdk::result<bytes> encode_frame(const frame& value, limits bound = {});

	/** @brief Decode exactly one frame, rejecting trailing bytes and digest mismatches. */
	[[nodiscard]] sdk::result<frame> decode_frame(std::span<const byte> input, limits bound = {});

	/** @brief Decode a bounded concatenated transcript without accepting a fragment. */
	[[nodiscard]] sdk::result<std::vector<frame>> decode_frame_stream(
		std::span<const byte> input, limits bound = {}, std::uint64_t maximum_frames = 65'536U);

	/** @brief Exact contiguous sequence guard for one stream and direction. */
	class sequence_guard
	{
	  public:
		explicit sequence_guard(std::uint64_t stream_id = 1U,
								std::uint64_t first_sequence = 0U) noexcept
			: stream_id_{stream_id}, next_sequence_{first_sequence}
		{
		}

		[[nodiscard]] sdk::result<void> accept(const frame& value);
		[[nodiscard]] std::uint64_t next_sequence() const noexcept
		{
			return next_sequence_;
		}
		[[nodiscard]] std::uint64_t stream_id() const noexcept
		{
			return stream_id_;
		}

	  private:
		std::uint64_t stream_id_{};
		std::uint64_t next_sequence_{};
		bool exhausted_{};
	};

	/** @brief Wire credit measured in complete encoded bytes (104-byte header included). */
	struct credit
	{
		std::uint64_t bytes{};
		std::uint64_t frames{};
	};

	class credit_window
	{
	  public:
		credit_window() = default;
		explicit credit_window(credit available) noexcept : available_{available} {}

		[[nodiscard]] sdk::result<void> grant(credit increment);
		[[nodiscard]] sdk::result<void> consume(const frame& value);
		[[nodiscard]] sdk::result<void> consume_encoded(std::size_t control_bytes,
														std::size_t payload_bytes);
		[[nodiscard]] credit available() const noexcept
		{
			return available_;
		}

	  private:
		credit available_{};
	};

	/** @brief Transactional sequence+credit admission used by closure tests. */
	class transcript_guard
	{
	  public:
		transcript_guard(std::uint64_t stream_id = 1U,
						 std::uint64_t first_sequence = 0U,
						 credit available = {}) noexcept
			: sequence_{stream_id, first_sequence}, credit_{available}
		{
		}

		[[nodiscard]] sdk::result<void> accept(const frame& value, bool consume_credit = true);
		[[nodiscard]] const sequence_guard& sequence() const noexcept
		{
			return sequence_;
		}
		[[nodiscard]] const credit_window& credit() const noexcept
		{
			return credit_;
		}

	  private:
		sequence_guard sequence_;
		credit_window credit_;
	};
} // namespace cxxlens::protocol_v2
