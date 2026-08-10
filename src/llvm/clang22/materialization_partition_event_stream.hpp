#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_io.hpp"

namespace cxxlens::detail::clang22::materialization
{
	inline constexpr std::size_t materialization_partition_event_stream_header_bytes = 86U;
	inline constexpr std::size_t materialization_partition_event_stream_trailer_bytes = 112U;

	/** The exact 128-bit event ordinal represented without a compiler-specific integer type. */
	struct materialization_event_ordinal
	{
		std::uint64_t high{};
		std::uint64_t low{};

		[[nodiscard]] bool operator==(const materialization_event_ordinal&) const = default;
	};

	enum class materialization_partition_event_kind : std::uint8_t
	{
		partition_begin = 1U,
		claim_occurrence = 2U,
		detached_row = 3U,
		claim_annotation = 4U,
		coverage = 5U,
		unresolved = 6U,
		partition_end = 7U,
	};

	/** Exact header/trailer and independently recomputed stream digests. */
	struct materialization_partition_event_stream_receipt
	{
		std::array<std::byte, 32U> stream_sequence_id{};
		std::uint64_t spool_index{};
		materialization_event_ordinal first_event_ordinal;
		materialization_event_ordinal next_event_ordinal;
		std::uint64_t actual_frame_count{};
		std::uint64_t actual_body_bytes{};
		std::array<std::byte, 32U> frames_digest{};
		std::array<std::byte, 32U> stream_prefix_digest{};

		[[nodiscard]] bool
		operator==(const materialization_partition_event_stream_receipt&) const = default;
	};

	/** Callback view for one validated event; key/payload remain valid only for the call. */
	using materialization_partition_event_consumer =
		std::function<sdk::result<void>(std::uint64_t,
										materialization_partition_event_kind,
										std::span<const std::byte>,
										std::span<const std::byte>)>;

	/** Return the checked framed length of one event before it is appended. */
	[[nodiscard]] sdk::result<std::uint64_t>
	materialization_partition_event_frame_size(std::span<const std::byte> key,
											   std::span<const std::byte> payload);

	/** Validate the closed D3 key/payload field projection before event framing. */
	[[nodiscard]] sdk::result<void>
	validate_materialization_partition_event_projection(materialization_partition_event_kind kind,
														std::span<const std::byte> key,
														std::span<const std::byte> payload);

	/**
	 * Source-private CXLPEV01 writer. The stream owns an anonymous sealed spool and never exposes
	 * a pathname. Header/trailer counts are declarations only; replay independently rechecks them.
	 */
	class materialization_partition_event_stream
	{
	  public:
		materialization_partition_event_stream(const materialization_partition_event_stream&) =
			delete;
		materialization_partition_event_stream&
		operator=(const materialization_partition_event_stream&) = delete;
		materialization_partition_event_stream(materialization_partition_event_stream&&) noexcept;
		materialization_partition_event_stream&
		operator=(materialization_partition_event_stream&&) noexcept;
		~materialization_partition_event_stream();

		[[nodiscard]] static sdk::result<materialization_partition_event_stream>
		begin(std::string materialization_request_id,
			  std::uint64_t spool_index,
			  materialization_event_ordinal first_event_ordinal,
			  std::uint64_t declared_frame_count,
			  std::uint64_t declared_body_bytes);

		[[nodiscard]] sdk::result<void> append(materialization_partition_event_kind kind,
											   std::span<const std::byte> key,
											   std::span<const std::byte> payload);

		/** Seal the spool after writing the exact trailer and return its independent receipt. */
		[[nodiscard]] sdk::result<materialization_partition_event_stream_receipt> finalize() &&;

		/** Transfer the sealed anonymous spool to a replay/Store adapter. */
		[[nodiscard]] std::unique_ptr<materialization_replayable_spool> release_spool() &&;

	  private:
		materialization_partition_event_stream() = default;

		std::unique_ptr<materialization_replayable_spool> spool_;
		std::vector<std::byte> header_bytes_;
		std::vector<std::byte> last_order_key_;
		std::uint64_t spool_index_{};
		materialization_event_ordinal first_event_ordinal_;
		std::uint64_t declared_frame_count_{};
		std::uint64_t declared_body_bytes_{};
		std::uint64_t actual_frame_count_{};
		std::uint64_t actual_body_bytes_{};
		bool began_{};
		bool ended_{};
		bool finalized_{};
		std::optional<materialization_partition_event_stream_receipt> receipt_;
	};

	/** Independently replay and validate one sealed CXLPEV01 spool. */
	[[nodiscard]] sdk::result<materialization_partition_event_stream_receipt>
	validate_materialization_partition_event_stream(
		materialization_replayable_spool& spool,
		std::optional<std::string_view> expected_materialization_request_id = std::nullopt);

	/** Replay one sealed stream with bounded frame storage after independent receipt validation. */
	[[nodiscard]] sdk::result<void> replay_materialization_partition_event_stream(
		materialization_replayable_spool& spool,
		std::optional<std::string_view> expected_materialization_request_id,
		const materialization_partition_event_consumer& consumer);
} // namespace cxxlens::detail::clang22::materialization
