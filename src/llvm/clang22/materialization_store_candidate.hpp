#pragma once

/**
 * @file materialization_store_candidate.hpp
 * @brief Private bounded candidate, projection, and report ports for Store adoption.
 *
 * This header is deliberately source-private.  It models the bounded #200 ingress without
 * changing the installed SDK Store ABI.  The production Store adapter can later bind these ports
 * to its memory and SQLite backends; the reference implementation already enforces the phase and
 * byte/resource invariants at this boundary.
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

#include "materialization_io.hpp"

namespace cxxlens::detail::clang22::materialization
{
	inline constexpr std::uint64_t bounded_store_max_tasks = 4096U;
	inline constexpr std::uint64_t bounded_store_max_aggregate_bytes = 512U * 1024U * 1024U;
	inline constexpr std::uint64_t bounded_store_resident_window_bytes = 77'729'792U;
	inline constexpr std::uint64_t bounded_store_report_tail_bytes = 28'321'546U;
	inline constexpr std::uint64_t bounded_store_max_report_bytes = 1024U * 1024U * 1024U;
	inline constexpr std::uint64_t bounded_store_sort_arena_bytes = 8U * 1024U * 1024U;
	inline constexpr std::uint64_t bounded_store_comparator_cursor_bytes = 32U * 1024U;
	inline constexpr std::uint64_t bounded_store_merge_file_descriptors = 18U;
	inline constexpr std::uint64_t bounded_store_sqlite_chunk_bytes = 8U * 1024U * 1024U;

	/** Exact checked limits shared by the reference spool and future backend adapters. */
	struct bounded_store_limits
	{
		std::uint64_t max_tasks{bounded_store_max_tasks};
		std::uint64_t max_aggregate_bytes{bounded_store_max_aggregate_bytes};
		std::uint64_t max_record_bytes{1024U * 1024U};
		std::uint64_t max_spool_bytes{bounded_store_max_aggregate_bytes};
		std::uint64_t report_tail_bytes{bounded_store_report_tail_bytes};
		std::uint64_t max_report_bytes{bounded_store_max_report_bytes};
	};

	/** Closed record grammar used by both expected and actual projection streams. */
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

	/** One full projection record.  Equality includes every byte-bearing field. */
	struct bounded_store_record
	{
		bounded_store_record_kind kind{bounded_store_record_kind::partition_begin};
		std::string key;
		std::vector<std::byte> payload;

		[[nodiscard]] bool operator==(const bounded_store_record&) const = default;
	};

	/** Encode/decode one length-framed record with its recomputed content digest. */
	[[nodiscard]] sdk::result<std::vector<std::byte>>
	encode_bounded_store_record(const bounded_store_record& record,
								const bounded_store_limits& limits = {});
	[[nodiscard]] sdk::result<bounded_store_record>
	decode_bounded_store_record(std::span<const std::byte> bytes,
								const bounded_store_limits& limits = {});

	/** External task/journal census required before candidate identity is minted. */
	struct bounded_store_external_census
	{
		std::uint64_t task_count{};
		std::uint64_t input_bytes{};
		std::string input_digest;
	};

	/** Exact Store terminal union.  `not_attempted` is the zero-effect terminal. */
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

	/** Cursor over a sealed stream.  At most one decoded record is live per call. */
	class bounded_store_record_cursor
	{
	  public:
		virtual ~bounded_store_record_cursor() = default;
		[[nodiscard]] virtual sdk::result<std::optional<bounded_store_record>> next() = 0;
	};

	/** Bounded append/seal/replay port for memory, SQLite, and disk-backed reference adapters. */
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

	/** Wrap one private replayable spool with the exact bounded framed-record contract. */
	[[nodiscard]] sdk::result<std::unique_ptr<bounded_store_record_spool>>
	make_bounded_store_record_spool(std::unique_ptr<materialization_replayable_spool> storage,
									bounded_store_limits limits = {});

	/** One publication attempt; backends return a terminal value, never an implicit retry. */
	class bounded_store_publication_port
	{
	  public:
		virtual ~bounded_store_publication_port() = default;
		[[nodiscard]] virtual bounded_store_publication_terminal
		publish_once(std::string_view candidate_id, std::string_view expected_head) = 0;
	};

	/** Bounded report tail writer kept separate from Store publication state. */
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

	/** Construct a report writer over a bounded private byte spool. */
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

	/** Move-only bounded candidate lifecycle.  This is source-private and does not alter SDK ABI.
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

		/** Append exactly one sealed task result; task bytes are never retained in a vector. */
		[[nodiscard]] sdk::result<void> append_task(std::span<const std::byte> sealed_task);
		/** Seal the external census and mint the semantic candidate identity exactly once. */
		[[nodiscard]] sdk::result<void> seal_input(const bounded_store_external_census& census);

		using projection_builder =
			std::function<sdk::result<void>(bounded_store_record_spool& sink)>;
		/** Build expected bytes from immutable external authorities only. */
		[[nodiscard]] sdk::result<void> build_expected_projection(projection_builder builder);
		/** Build actual bytes from backend rows in already-authenticated physical-key order. */
		[[nodiscard]] sdk::result<void> build_actual_projection(projection_builder builder);
		/** Compare the two sealed streams one full framed record at a time. */
		[[nodiscard]] sdk::result<void> compare_projections();

		/** Reserve the maximum report tail before any irreversible publication call. */
		[[nodiscard]] sdk::result<void> reserve_report_tail(bounded_store_report_writer& report);
		/** Complete the zero-effect path without calling the backend. */
		[[nodiscard]] sdk::result<void> finish_without_publication();
		/** Call the backend exactly once; an opaque backend result is a terminal unknown. */
		[[nodiscard]] sdk::result<void> publish_once(bounded_store_publication_port& backend);
		/** Finalize the report after a Store terminal; no outcome is rewritten by report failure.
		 */
		[[nodiscard]] sdk::result<void> finalize_report(bounded_store_report_writer& report);
		/** Close private staging without replacing an already captured failure/terminal. */
		void abort() noexcept;

	  private:
		struct state;
		explicit bounded_store_candidate(std::unique_ptr<state> state);
		std::unique_ptr<state> state_;
		friend sdk::result<bounded_store_candidate>
			begin_bounded_store_candidate(std::string,
										  std::string,
										  bounded_store_limits,
										  std::unique_ptr<bounded_store_record_spool>,
										  std::unique_ptr<bounded_store_record_spool>,
										  std::unique_ptr<bounded_store_record_spool>);
	};

	/** Begin a candidate with independent input/expected/actual streams. */
	[[nodiscard]] sdk::result<bounded_store_candidate>
	begin_bounded_store_candidate(std::string staging_session_id,
								  std::string expected_head,
								  bounded_store_limits limits,
								  std::unique_ptr<bounded_store_record_spool> input,
								  std::unique_ptr<bounded_store_record_spool> expected,
								  std::unique_ptr<bounded_store_record_spool> actual);
} // namespace cxxlens::detail::clang22::materialization
