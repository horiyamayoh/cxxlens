#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk
{
	inline constexpr std::uint64_t sqlite_payload_chunk_maximum_bytes = 8'388'608U;
	inline constexpr std::uint64_t sqlite_payload_chunk_maximum_count = 2'199'023'255'552U;
	inline constexpr std::uint64_t sqlite_payload_chunk_maximum_ordinal =
		sqlite_payload_chunk_maximum_count - 1U;
	inline constexpr std::uint64_t sqlite_sha256_maximum_byte_count =
		std::numeric_limits<std::uint64_t>::max() / 8U;

	/** Exact unsigned 128-bit census without a compiler-specific integer in the ABI. */
	struct sqlite_u128_census
	{
		std::uint64_t high{};
		std::uint64_t low{};

		/** Add one u64 operand, returning false rather than wrapping at 2^128. */
		[[nodiscard]] bool add(std::uint64_t value) noexcept;
		[[nodiscard]] bool operator==(const sqlite_u128_census&) const = default;
	};

	/** Checked helpers shared by payload, chunk, and SHA-256 byte accounting. */
	[[nodiscard]] bool
	sqlite_checked_add_u64(std::uint64_t left, std::uint64_t right, std::uint64_t& output) noexcept;
	[[nodiscard]] bool sqlite_checked_multiply_u64(std::uint64_t left,
												   std::uint64_t right,
												   std::uint64_t& output) noexcept;
	[[nodiscard]] bool sqlite_checked_sha256_byte_count(std::uint64_t current,
														std::uint64_t added,
														std::uint64_t& output) noexcept;

	/**
	 * Incremental SHA-256 with checked byte-to-bit accounting.
	 *
	 * `finish()` returns the exact `sha256:<64 lowercase hex>` spelling. A state can be reset and
	 * reused, but cannot be updated or finalized twice between resets.
	 */
	class sqlite_incremental_sha256
	{
	  public:
		sqlite_incremental_sha256() noexcept;
		[[nodiscard]] result<void> update(std::span<const std::byte> input);
		[[nodiscard]] result<std::string> finish();
		void reset() noexcept;
		[[nodiscard]] std::uint64_t byte_count() const noexcept;
		[[nodiscard]] bool finished() const noexcept;

	  private:
		static constexpr std::size_t block_bytes = 64U;
		void transform(std::span<const std::byte> block) noexcept;

		std::array<std::uint32_t, 8U> state_{};
		std::array<std::byte, block_bytes> pending_{};
		std::size_t pending_size_{};
		std::uint64_t total_bytes_{};
		bool finished_{};
	};

	/** Bounded append target used by canonical encoders and byte-exact comparison sinks. */
	class sqlite_bounded_byte_sink
	{
	  public:
		virtual ~sqlite_bounded_byte_sink() = default;
		[[nodiscard]] virtual result<void> append(std::span<const std::byte> bytes) = 0;
	};

	/** One forward-only bounded cursor. A zero return is EOF; callers provide the storage window.
	 */
	class sqlite_bounded_byte_source
	{
	  public:
		virtual ~sqlite_bounded_byte_source() = default;
		[[nodiscard]] virtual result<std::size_t> read(std::span<std::byte> output) = 0;
	};

	/** Factory for independent passes over the same immutable bytes. */
	class sqlite_replayable_byte_source
	{
	  public:
		virtual ~sqlite_replayable_byte_source() = default;
		[[nodiscard]] virtual result<std::unique_ptr<sqlite_bounded_byte_source>>
		open_pass() const = 0;
	};

	/** Append target which becomes an immutable replay source only after a successful seal. */
	class sqlite_replayable_byte_sink : public sqlite_bounded_byte_sink
	{
	  public:
		[[nodiscard]] virtual result<std::shared_ptr<const sqlite_replayable_byte_source>>
		seal() = 0;
	};

	/** One canonical SQLite v3 payload chunk. `payload` is valid only during `emit()`. */
	struct sqlite_payload_chunk_frame
	{
		std::uint64_t ordinal{};
		std::uint64_t byte_offset{};
		std::uint64_t byte_count{};
		std::string_view checksum;
		std::span<const std::byte> payload;
	};

	/** Prepared-statement or test port which consumes one canonical chunk at a time. */
	class sqlite_payload_chunk_port
	{
	  public:
		virtual ~sqlite_payload_chunk_port() = default;
		[[nodiscard]] virtual result<void> emit(const sqlite_payload_chunk_frame& frame) = 0;
	};

	struct sqlite_payload_stream_receipt
	{
		std::uint64_t byte_count{};
		std::uint64_t chunk_count{};
		sqlite_u128_census aggregate_byte_count;
		std::string full_checksum;
	};

	/** Source-private identity of a production payload-streaming instance. */
	enum class sqlite_payload_resident_instance_kind : std::uint8_t
	{
		chunk_framer,
		validating_source,
	};

	/** Allocation-free high-water receipt from actual production streaming instances. */
	struct sqlite_payload_resident_observation_receipt
	{
		std::uint64_t maximum_resident_payload_buffer_bytes{};
		std::uint64_t observation_count{};
		std::uint64_t chunk_framer_instance_count{};
		std::uint64_t validating_source_instance_count{};
		bool observation_count_overflow{};
	};

	/**
	 * Passive, source-private qualification observation scope.
	 *
	 * The outermost scope on a thread observes high-water notifications made by the actual
	 * `sqlite_payload_chunk_framer` and `sqlite_validating_payload_source` instances. It never
	 * changes allocation, chunking, or read/write behavior. Production code creates no scope.
	 */
#if defined(__GNUC__) || defined(__clang__)
	class __attribute__((visibility("default"))) sqlite_payload_resident_observation_scope
#else
	class sqlite_payload_resident_observation_scope
#endif
	{
	  public:
		sqlite_payload_resident_observation_scope() noexcept;
		~sqlite_payload_resident_observation_scope() noexcept;

		sqlite_payload_resident_observation_scope(
			const sqlite_payload_resident_observation_scope&) = delete;
		sqlite_payload_resident_observation_scope&
		operator=(const sqlite_payload_resident_observation_scope&) = delete;
		sqlite_payload_resident_observation_scope(sqlite_payload_resident_observation_scope&&) =
			delete;
		sqlite_payload_resident_observation_scope&
		operator=(sqlite_payload_resident_observation_scope&&) = delete;

		[[nodiscard]] bool active() const noexcept;
		[[nodiscard]] sqlite_payload_resident_observation_receipt observation() const noexcept;

	  private:
		friend void observe_sqlite_payload_resident_bytes(sqlite_payload_resident_instance_kind,
														  std::size_t,
														  bool) noexcept;

		void record(sqlite_payload_resident_instance_kind kind,
					std::size_t resident_bytes,
					bool instance_created) noexcept;

		sqlite_payload_resident_observation_receipt receipt_;
		bool active_{};
	};

	/** Internal notification point; callers must report an actual instance's live buffer only. */
	void observe_sqlite_payload_resident_bytes(sqlite_payload_resident_instance_kind kind,
											   std::size_t resident_bytes,
											   bool instance_created) noexcept;

	/**
	 * Canonical 8 MiB chunk framer.
	 *
	 * A null port is the first/measure pass: no payload vector or chunk collection is retained.
	 * With a port, at most one 8 MiB chunk is resident and each frame is consumed synchronously.
	 */
	class sqlite_payload_chunk_framer final : public sqlite_bounded_byte_sink
	{
	  public:
		explicit sqlite_payload_chunk_framer(sqlite_payload_chunk_port* port = nullptr) noexcept;
		sqlite_payload_chunk_framer(const sqlite_payload_chunk_framer&) = delete;
		sqlite_payload_chunk_framer& operator=(const sqlite_payload_chunk_framer&) = delete;

		[[nodiscard]] result<void> append(std::span<const std::byte> bytes) override;
		[[nodiscard]] result<sqlite_payload_stream_receipt> finish();
		[[nodiscard]] std::size_t resident_payload_byte_count() const noexcept;
		[[nodiscard]] bool measure_only() const noexcept;

	  private:
		[[nodiscard]] result<void> complete_chunk();

		sqlite_payload_chunk_port* port_{};
		std::vector<std::byte> chunk_;
		sqlite_incremental_sha256 chunk_digest_;
		sqlite_incremental_sha256 full_digest_;
		sqlite_u128_census aggregate_byte_count_;
		std::uint64_t total_byte_count_{};
		std::uint64_t chunk_count_{};
		std::uint64_t current_chunk_byte_count_{};
		bool finished_{};
		bool poisoned_{};
	};

	/** Borrowed SQLite row. All views remain valid until the next cursor call. */
	struct sqlite_payload_chunk_record
	{
		std::uint64_t ordinal{};
		std::uint64_t byte_offset{};
		std::uint64_t byte_count{};
		std::string_view checksum;
		std::span<const std::byte> payload;
	};

	/** Numeric-ordinal row source, normally backed by one prepared ORDER BY cursor. */
	class sqlite_payload_chunk_record_source
	{
	  public:
		virtual ~sqlite_payload_chunk_record_source() = default;
		[[nodiscard]] virtual result<std::optional<sqlite_payload_chunk_record>> next() = 0;
	};

	/** Factory required to replay a payload without retaining its chunks. */
	class sqlite_replayable_payload_chunk_source
	{
	  public:
		virtual ~sqlite_replayable_payload_chunk_source() = default;
		[[nodiscard]] virtual result<std::unique_ptr<sqlite_payload_chunk_record_source>>
		open_chunk_pass() const = 0;
	};

	struct sqlite_payload_stream_expectation
	{
		std::uint64_t byte_count{};
		std::uint64_t chunk_count{};
		std::string full_checksum;
		bool validate_full_checksum{true};
	};

	/**
	 * Forward-only payload adapter which validates row framing and checksums before exposing bytes.
	 * It retains one borrowed chunk and never collects rows. `finish()` proves that the consumer
	 * consumed the complete stream and that no extra row exists.
	 */
	class sqlite_validating_payload_source final : public sqlite_bounded_byte_source
	{
	  public:
		[[nodiscard]] static result<std::unique_ptr<sqlite_validating_payload_source>>
		open(std::unique_ptr<sqlite_payload_chunk_record_source> rows,
			 sqlite_payload_stream_expectation expectation);

		[[nodiscard]] result<std::size_t> read(std::span<std::byte> output) override;
		[[nodiscard]] result<sqlite_payload_stream_receipt> finish();
		[[nodiscard]] std::size_t resident_payload_byte_count() const noexcept;

	  private:
		sqlite_validating_payload_source(std::unique_ptr<sqlite_payload_chunk_record_source> rows,
										 sqlite_payload_stream_expectation expectation) noexcept;
		[[nodiscard]] result<bool> load_next_chunk();
		[[nodiscard]] result<sqlite_payload_stream_receipt> finalize_at_eof();

		std::unique_ptr<sqlite_payload_chunk_record_source> rows_;
		sqlite_payload_stream_expectation expectation_;
		std::span<const std::byte> current_chunk_;
		std::size_t current_offset_{};
		sqlite_incremental_sha256 full_digest_;
		sqlite_u128_census aggregate_byte_count_;
		std::uint64_t observed_byte_count_{};
		std::uint64_t observed_chunk_count_{};
		bool source_eof_{};
		bool finished_{};
		bool poisoned_{};
		std::optional<sqlite_payload_stream_receipt> receipt_;
	};

	/** Replay wrapper: every pass receives a fresh row cursor and the same closed expectation. */
	class sqlite_validated_replayable_payload_source final : public sqlite_replayable_byte_source
	{
	  public:
		sqlite_validated_replayable_payload_source(
			std::shared_ptr<const sqlite_replayable_payload_chunk_source> rows,
			sqlite_payload_stream_expectation expectation);
		[[nodiscard]] result<std::unique_ptr<sqlite_bounded_byte_source>>
		open_pass() const override;

	  private:
		std::shared_ptr<const sqlite_replayable_payload_chunk_source> rows_;
		sqlite_payload_stream_expectation expectation_;
	};
} // namespace cxxlens::sdk
