#pragma once

/**
 * @file materialization_store_projection.hpp
 * @brief Bounded, source-private Store projection cursors and an independent oracle.
 *
 * The projection path is intentionally independent from the SDK Store implementation.  An
 * expected stream is emitted from sealed semantic authorities, while an actual stream is emitted
 * by a backend in its already-authenticated physical-key order.  The two streams are sealed and
 * compared through separate cursors; neither side is sorted, re-used as the other side, or
 * represented by a request-sized vector.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_io.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/** Exact fixed limits shared by both projection cursors. */
	struct materialization_store_projection_limits
	{
		static constexpr std::uint64_t default_max_record_bytes = 1U * 1024U * 1024U;
		static constexpr std::uint64_t default_max_spool_bytes = 512U * 1024U * 1024U;
		static constexpr std::uint64_t default_max_records = 1U << 20U;

		std::uint64_t max_record_bytes{default_max_record_bytes};
		std::uint64_t max_spool_bytes{default_max_spool_bytes};
		std::uint64_t max_records{default_max_records};
	};

	/** Closed semantic record grammar used by both independent projections. */
	enum class materialization_store_projection_record_kind : std::uint8_t
	{
		partition_begin = 1U,
		semantic_key = 2U,
		claim_full_projection = 3U,
		detached_row = 4U,
		claim_annotation = 5U,
		coverage = 6U,
		unresolved = 7U,
		closure_binding = 8U,
		provenance = 9U,
		guarantee = 10U,
		partition_census = 11U,
		partition_end = 12U,
		global_identity = 13U,
		task_result = 14U,
	};

	/** A complete projection record. Equality includes every semantic byte. */
	struct materialization_store_projection_record
	{
		materialization_store_projection_record_kind kind{
			materialization_store_projection_record_kind::partition_begin};
		std::string key;
		std::vector<std::byte> payload;

		[[nodiscard]] bool
		operator==(const materialization_store_projection_record&) const = default;
	};

	/** Encode/decode one closed, length-framed record and verify its content digest. */
	[[nodiscard]] sdk::result<std::vector<std::byte>>
	encode_materialization_store_projection_record(
		const materialization_store_projection_record& record,
		const materialization_store_projection_limits& limits = {});

	[[nodiscard]] sdk::result<materialization_store_projection_record>
	decode_materialization_store_projection_record(
		std::span<const std::byte> bytes,
		const materialization_store_projection_limits& limits = {});

	/** One-record-at-a-time cursor over an immutable, sealed projection stream. */
	class materialization_store_projection_cursor
	{
	  public:
		virtual ~materialization_store_projection_cursor() = default;
		[[nodiscard]] virtual sdk::result<std::optional<materialization_store_projection_record>>
		next() = 0;
	};

	/**
	 * Sealed replayable stream used for one side of the independent comparison.
	 *
	 * The implementation stores framed bytes in the existing anonymous sealed spool.  Appending
	 * and decoding allocate at most one frame bounded by `max_record_bytes`; all aggregate state is
	 * counters and digests.  The stream is move-only and cannot be appended after sealing.
	 */
	class materialization_store_projection_stream final
	{
	  public:
		materialization_store_projection_stream(const materialization_store_projection_stream&) =
			delete;
		materialization_store_projection_stream&
		operator=(const materialization_store_projection_stream&) = delete;
		materialization_store_projection_stream(materialization_store_projection_stream&&) noexcept;
		materialization_store_projection_stream&
		operator=(materialization_store_projection_stream&&) noexcept;
		~materialization_store_projection_stream();

		[[nodiscard]] static sdk::result<materialization_store_projection_stream>
		create(materialization_store_projection_limits limits = {});

		[[nodiscard]] sdk::result<void>
		append(const materialization_store_projection_record& record);
		[[nodiscard]] sdk::result<void> seal();
		[[nodiscard]] sdk::result<std::unique_ptr<materialization_store_projection_cursor>>
		open_cursor() const;

		/** Validate that records already arrive in canonical physical-key order. */
		[[nodiscard]] sdk::result<void> validate_canonical_order() const;

		[[nodiscard]] std::uint64_t record_count() const noexcept;
		[[nodiscard]] std::uint64_t byte_count() const noexcept;
		[[nodiscard]] bool sealed() const noexcept;
		[[nodiscard]] const materialization_store_projection_limits& limits() const noexcept;

		/** Content digest of exact framed bytes; available only after `seal()`. */
		[[nodiscard]] sdk::result<std::string> content_digest() const;
		/** Semantic digest over the exact content digest; available only after `seal()`. */
		[[nodiscard]] sdk::result<std::string> semantic_digest() const;

	  private:
		struct state;
		explicit materialization_store_projection_stream(std::unique_ptr<state> state);
		std::unique_ptr<state> state_;
	};

	/** Independent comparison result. A mismatch is a value, not a fabricated SDK error. */
	enum class materialization_store_projection_mismatch_kind : std::uint8_t
	{
		none,
		expected_missing,
		actual_extra,
		full_byte_mismatch,
	};

	struct materialization_store_projection_comparison
	{
		materialization_store_projection_mismatch_kind kind{
			materialization_store_projection_mismatch_kind::none};
		std::uint64_t record_index{};
		std::optional<materialization_store_projection_record> expected;
		std::optional<materialization_store_projection_record> actual;

		[[nodiscard]] bool equal() const noexcept
		{
			return kind == materialization_store_projection_mismatch_kind::none;
		}
	};

	/**
	 * Independent oracle for two sealed streams.  The expected and actual cursors are opened
	 * separately and advanced in lockstep; only the current record from each side is resident.
	 */
	[[nodiscard]] sdk::result<materialization_store_projection_comparison>
	compare_materialization_store_projections(
		const materialization_store_projection_stream& expected,
		const materialization_store_projection_stream& actual);
} // namespace cxxlens::detail::clang22::materialization
