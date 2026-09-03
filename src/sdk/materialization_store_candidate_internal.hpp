#pragma once

/**
 * @file materialization_store_candidate_internal.hpp
 * @brief Source-private bounded Store candidate and claim projection ports.
 *
 * The port deliberately stays below the installed SDK ABI.  It supplies one bounded staging
 * window to the memory and SQLite adapters: task results are framed into private spools, the
 * independent expected/actual projections are compared before publication, and publication is
 * a one-shot effect.  A failed candidate is terminal and cannot be retried or partially exposed.
 */

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

#include "materialization_io_internal.hpp"

namespace cxxlens::sdk::detail
{
	inline constexpr std::uint64_t bounded_store_max_tasks = 4096U;
	inline constexpr std::uint64_t bounded_store_max_aggregate_bytes =
		std::uint64_t{512U} * 1024U * 1024U;
	inline constexpr std::uint64_t bounded_store_resident_window_bytes = 77'729'792U;
	inline constexpr std::uint64_t bounded_store_report_tail_bytes = 28'321'546U;
	inline constexpr std::uint64_t bounded_store_max_report_bytes =
		std::uint64_t{1024U} * 1024U * 1024U;
	inline constexpr std::uint64_t bounded_store_sort_arena_bytes =
		std::uint64_t{8U} * 1024U * 1024U;
	inline constexpr std::uint64_t bounded_store_comparator_cursor_bytes =
		std::uint64_t{32U} * 1024U;
	inline constexpr std::uint64_t bounded_store_merge_file_descriptors = 18U;
	inline constexpr std::uint64_t bounded_store_sqlite_chunk_bytes =
		std::uint64_t{8U} * 1024U * 1024U;

	/** Checked limits for one bounded candidate and every private spool it owns. */
	struct bounded_store_limits
	{
		std::uint64_t max_tasks{bounded_store_max_tasks};
		std::uint64_t max_aggregate_bytes{bounded_store_max_aggregate_bytes};
		std::uint64_t max_record_bytes{std::uint64_t{1024U} * 1024U};
		std::uint64_t max_spool_bytes{bounded_store_max_aggregate_bytes};
		std::uint64_t report_tail_bytes{bounded_store_report_tail_bytes};
		std::uint64_t max_report_bytes{bounded_store_max_report_bytes};
	};

	/** Closed record kinds used by both independently constructed projection streams. */
	enum class bounded_store_record_kind : std::uint8_t
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

	/** One complete projection record.  Equality includes the full key and payload bytes. */
	struct bounded_store_record
	{
		bounded_store_record_kind kind{bounded_store_record_kind::partition_begin};
		std::string key;
		std::vector<std::byte> payload;

		[[nodiscard]] bool operator==(const bounded_store_record&) const = default;
	};

	/** Encode/decode one length-framed record with its recomputed SHA-256 content digest. */
	[[nodiscard]] sdk::result<std::vector<std::byte>>
	encode_bounded_store_record(const bounded_store_record& record,
								const bounded_store_limits& limits = {});
	[[nodiscard]] sdk::result<bounded_store_record>
	decode_bounded_store_record(std::span<const std::byte> bytes,
								const bounded_store_limits& limits = {});

	/**
	 * Check the complete frame length without allocating or touching a spool.  This is
	 * intentionally public to let task/claim producers reject a limit overflow before constructing
	 * a record.
	 */
	[[nodiscard]] sdk::result<std::uint64_t>
	checked_bounded_store_record_frame_bytes(std::uint64_t key_bytes,
											 std::uint64_t payload_bytes,
											 const bounded_store_limits& limits = {});

	/** External task/journal census required before candidate identity is minted. */
	struct bounded_store_external_census
	{
		std::uint64_t task_count{};
		std::uint64_t input_bytes{};
		std::string input_digest;
	};

	/** Exact Store publication terminal union. */
	enum class bounded_store_publication_terminal : std::uint8_t
	{
		not_attempted,
		rejected_stale,
		rejected_store_failure,
		publication_outcome_unknown,
		committed_unverified,
		committed_verified,
	};

	[[nodiscard]] constexpr bool is_valid(const bounded_store_publication_terminal value) noexcept
	{
		return value >= bounded_store_publication_terminal::not_attempted &&
			value <= bounded_store_publication_terminal::committed_verified;
	}

	/** Cursor over one sealed stream.  The implementation keeps only one decoded record live. */
	class bounded_store_record_cursor
	{
	  public:
		virtual ~bounded_store_record_cursor() = default;
		[[nodiscard]] virtual sdk::result<std::optional<bounded_store_record>> next() = 0;
	};

	/** Append/seal/replay port for memory, SQLite, and private disk-backed adapters. */
	class bounded_store_record_spool
	{
	  public:
		virtual ~bounded_store_record_spool() = default;
		[[nodiscard]] virtual sdk::result<void> append(const bounded_store_record& record) = 0;
		[[nodiscard]] virtual sdk::result<void> seal() = 0;
		[[nodiscard]] virtual sdk::result<std::unique_ptr<bounded_store_record_cursor>>
		open_cursor() const = 0;
		[[nodiscard]] virtual std::uint64_t record_count() const noexcept = 0;
		[[nodiscard]] virtual std::uint64_t byte_count() const noexcept = 0;
		[[nodiscard]] virtual bool sealed() const noexcept = 0;
	};

	/** Wrap one private replayable spool with the checked framed-record contract. */
	[[nodiscard]] sdk::result<std::unique_ptr<bounded_store_record_spool>>
	make_bounded_store_record_spool(std::unique_ptr<materialization_replayable_spool> storage,
									bounded_store_limits limits = {});

	/** One backend publication attempt.  Backends must return a terminal, never request retry. */
	class bounded_store_publication_port
	{
	  public:
		virtual ~bounded_store_publication_port() = default;
		[[nodiscard]] virtual bounded_store_publication_terminal
		publish_once(std::string_view candidate_id, std::string_view expected_head) = 0;
	};

	/** Report tail writer kept separate from Store publication state. */
	class bounded_store_report_writer
	{
	  public:
		bounded_store_report_writer(const bounded_store_report_writer&) = delete;
		bounded_store_report_writer& operator=(const bounded_store_report_writer&) = delete;
		bounded_store_report_writer(bounded_store_report_writer&&) noexcept;
		bounded_store_report_writer& operator=(bounded_store_report_writer&&) noexcept;
		~bounded_store_report_writer();

		[[nodiscard]] sdk::result<void> reserve();
		[[nodiscard]] sdk::result<void> append(std::span<const std::byte> bytes);
		[[nodiscard]] sdk::result<void> finalize(bounded_store_publication_terminal terminal);
		[[nodiscard]] bool reserved() const noexcept;
		[[nodiscard]] bool finalized() const noexcept;
		[[nodiscard]] bool cleanup_failed() const noexcept;
		[[nodiscard]] std::uint64_t bytes_written() const noexcept;
		[[nodiscard]] std::optional<bounded_store_publication_terminal> terminal() const noexcept;

	  private:
		struct state;
		explicit bounded_store_report_writer(std::unique_ptr<state> state);
		std::unique_ptr<state> state_;
		friend sdk::result<bounded_store_report_writer>
			make_bounded_store_report_writer(std::unique_ptr<materialization_private_spool>,
											 bounded_store_limits);
	};

	[[nodiscard]] sdk::result<bounded_store_report_writer>
	make_bounded_store_report_writer(std::unique_ptr<materialization_private_spool> storage,
									 bounded_store_limits limits = {});

	enum class bounded_store_candidate_phase : std::uint8_t
	{
		staging_session_open,
		appending,
		input_sealed,
		candidate_identity_sealed,
		independently_validating,
		expected_projection_sealed,
		actual_projection_sealed,
		validation_sealed,
		report_tail_reserved,
		publication_attempted_once,
		publication_terminal,
		report_finalized,
		report_transport_failed,
		aborted,
	};

	/**
	 * Move-only candidate lifecycle.  `cleanup` is called exactly once for pre-publication aborts;
	 * a cleanup failure is retained without replacing the original failure.  The callback is kept
	 * source-private so no cleanup/error machinery reaches the installed SDK ABI.
	 */
	class bounded_store_candidate
	{
	  public:
		bounded_store_candidate(const bounded_store_candidate&) = delete;
		bounded_store_candidate& operator=(const bounded_store_candidate&) = delete;
		bounded_store_candidate(bounded_store_candidate&&) noexcept;
		bounded_store_candidate& operator=(bounded_store_candidate&&) noexcept;
		~bounded_store_candidate();

		[[nodiscard]] bounded_store_candidate_phase phase() const noexcept;
		[[nodiscard]] std::string_view staging_session_id() const noexcept;
		[[nodiscard]] std::string_view candidate_id() const noexcept;
		[[nodiscard]] std::optional<bounded_store_publication_terminal>
		publication_terminal() const noexcept;
		[[nodiscard]] bool cleanup_failed() const noexcept;

		/** Append one sealed task result; no task-result vector is retained. */
		[[nodiscard]] sdk::result<void> append_task(std::span<const std::byte> sealed_task);
		/** Seal external census and mint the semantic candidate identity exactly once. */
		[[nodiscard]] sdk::result<void> seal_input(const bounded_store_external_census& census);

		using projection_builder = std::function<sdk::result<void>(bounded_store_record_spool&)>;
		/** Build expected bytes from immutable external authorities only. */
		[[nodiscard]] sdk::result<void>
		build_expected_projection(const projection_builder& builder);
		/** Build actual bytes from an already authenticated physical-key order. */
		[[nodiscard]] sdk::result<void> build_actual_projection(const projection_builder& builder);
		/** Compare the two sealed streams one full framed record at a time. */
		[[nodiscard]] sdk::result<void> compare_projections();

		/** Reserve maximum report tail before any irreversible publication call. */
		[[nodiscard]] sdk::result<void> reserve_report_tail(bounded_store_report_writer& report);
		/** Complete the zero-effect path without calling the backend. */
		[[nodiscard]] sdk::result<void> finish_without_publication();
		/** Call the backend exactly once; opaque result is a terminal unknown. */
		[[nodiscard]] sdk::result<void> publish_once(bounded_store_publication_port& backend);
		/** Finalize report after a Store terminal; never rewrites the terminal. */
		[[nodiscard]] sdk::result<void> finalize_report(bounded_store_report_writer& report);
		/** Close private staging without replacing a captured terminal. */
		void abort() noexcept;

		// The definition stays private to the implementation; exposing only the name lets source
		// private failure/cleanup helpers share the move-only state without exporting its fields.
		struct state;

	  private:
		explicit bounded_store_candidate(std::unique_ptr<state> state);
		std::unique_ptr<state> state_;
		friend sdk::result<bounded_store_candidate>
			begin_bounded_store_candidate(std::string,
										  std::string,
										  bounded_store_limits,
										  std::unique_ptr<bounded_store_record_spool>,
										  std::unique_ptr<bounded_store_record_spool>,
										  std::unique_ptr<bounded_store_record_spool>,
										  std::function<sdk::result<void>()>);
	};

	/** Begin a candidate with independent input/expected/actual streams and cleanup callback. */
	[[nodiscard]] sdk::result<bounded_store_candidate>
	begin_bounded_store_candidate(std::string staging_session_id,
								  std::string expected_head,
								  bounded_store_limits limits,
								  std::unique_ptr<bounded_store_record_spool> input,
								  std::unique_ptr<bounded_store_record_spool> expected,
								  std::unique_ptr<bounded_store_record_spool> actual,
								  std::function<sdk::result<void>()> cleanup = {});
} // namespace cxxlens::sdk::detail
