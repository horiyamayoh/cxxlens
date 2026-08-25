#pragma once

/**
 * @file materialization_store_memory_backend.hpp
 * @brief Bounded process-local Store backend port for the #200 materializer slice.
 *
 * This is deliberately a provisional, source-private adapter.  The session owner supplies
 * task/receipt and semantic projection records through this narrow value port; no request JSON or
 * SDK snapshot writer is reconstructed here.  A later session-core
 * port can map the same staged-record/cursor/CAS operations to the production Store lifecycle.
 *
 * The memory implementation retains one immutable byte payload after sealing and decodes one
 * physical record at a time.  It therefore retains F bytes for the final payload plus O(W) cursor
 * working storage, where W is the configured maximum record frame.  It never builds a second full
 * semantic graph in memory.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::detail::clang22::materialization
{
	inline constexpr std::uint64_t bounded_memory_backend_max_tasks = 4096U;
	inline constexpr std::uint64_t bounded_memory_backend_max_payload_bytes =
		std::uint64_t{512U} * 1024U * 1024U;
	inline constexpr std::uint64_t bounded_memory_backend_max_record_bytes = 1024U * 1024U;
	inline constexpr std::uint64_t bounded_memory_backend_wire_overhead =
		1U + sizeof(std::uint64_t) * 2U + 71U;

	/** Bounds accepted by one provisional memory session. */
	struct bounded_memory_backend_limits
	{
		std::uint64_t max_tasks{bounded_memory_backend_max_tasks};
		std::uint64_t max_payload_bytes{bounded_memory_backend_max_payload_bytes};
		std::uint64_t max_record_bytes{bounded_memory_backend_max_record_bytes};
		/** Maximum decoded physical record window; this is the O(W) term. */
		std::uint64_t max_window_bytes{bounded_memory_backend_max_record_bytes};

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/** Closed physical record classes accepted by the provisional session. */
	enum class bounded_memory_record_kind : std::uint8_t
	{
		task_result = 1U,
		projection = 2U,
		metadata = 3U,
	};

	/** One value-owned record staged into the physical payload. */
	struct bounded_memory_record
	{
		bounded_memory_record_kind kind{bounded_memory_record_kind::projection};
		std::string key;
		std::vector<std::byte> payload;

		[[nodiscard]] bool operator==(const bounded_memory_record&) const = default;
	};

	/** One-shot terminal returned by the backend CAS/publication operation. */
	enum class bounded_memory_publication_terminal : std::uint8_t
	{
		not_attempted,
		rejected_stale,
		rejected_store_failure,
		publication_outcome_unknown,
		committed_verified,
	};

	[[nodiscard]] constexpr bool is_valid(const bounded_memory_publication_terminal value) noexcept
	{
		return value >= bounded_memory_publication_terminal::not_attempted &&
			value <= bounded_memory_publication_terminal::committed_verified;
	}

	/**
	 * One-shot compare/exchange port for the publication effect.
	 *
	 * The backend supplies a synchronous effect callback.  A port compares `expected_head` with
	 * `observed_head` and invokes that callback at most once.  The ordinary production port is
	 * installed by default; a storage boundary may provide another implementation when its own
	 * effect authority owns the compare/exchange operation.  A port must never retain the callback
	 * or retry it after returning.
	 */
	class bounded_memory_cas_port
	{
	  public:
		using effect = std::function<sdk::result<void>()>;

		virtual ~bounded_memory_cas_port() = default;
		[[nodiscard]] virtual sdk::result<bounded_memory_publication_terminal>
		compare_exchange_once(std::string_view expected_head,
							  std::string_view observed_head,
							  effect commit) = 0;
	};

	/** Exact semantic identity retained for one committed in-memory publication. */
	struct bounded_memory_publication
	{
		std::string publication_id;
		std::string candidate_id;
		std::string parent_publication;
		std::string payload_digest;
		std::uint64_t sequence{};
		std::uint64_t task_count{};
		std::uint64_t record_count{};
		std::uint64_t payload_bytes{};

		[[nodiscard]] bool operator==(const bounded_memory_publication&) const = default;
	};

	/** Receipt of an independent expected/actual physical-record comparison. */
	struct bounded_memory_parity_receipt
	{
		std::uint64_t record_count{};
		std::uint64_t payload_bytes{};
		std::uint64_t maximum_window_bytes{};
		std::string physical_payload_digest;

		[[nodiscard]] bool operator==(const bounded_memory_parity_receipt&) const = default;
	};

	/** Cursor over immutable physical staged bytes.  Only the current record is decoded. */
	class bounded_memory_record_cursor
	{
	  public:
		~bounded_memory_record_cursor();
		bounded_memory_record_cursor(bounded_memory_record_cursor&&) noexcept;
		bounded_memory_record_cursor& operator=(bounded_memory_record_cursor&&) noexcept;
		bounded_memory_record_cursor(const bounded_memory_record_cursor&) = delete;
		bounded_memory_record_cursor& operator=(const bounded_memory_record_cursor&) = delete;

		[[nodiscard]] sdk::result<std::optional<bounded_memory_record>> next();
		[[nodiscard]] std::uint64_t decoded_window_bytes() const noexcept;
		[[nodiscard]] std::uint64_t maximum_window_bytes() const noexcept;

	  private:
		struct state;
		explicit bounded_memory_record_cursor(std::shared_ptr<const state> state);
		std::unique_ptr<state> state_;
		friend class bounded_memory_backend_session;
		friend class bounded_memory_backend_snapshot;
	};

	/** Immutable reopened snapshot view; it shares the final payload, never copies it. */
	class bounded_memory_backend_snapshot
	{
	  public:
		bounded_memory_backend_snapshot(bounded_memory_backend_snapshot&&) noexcept;
		bounded_memory_backend_snapshot& operator=(bounded_memory_backend_snapshot&&) noexcept;
		bounded_memory_backend_snapshot(const bounded_memory_backend_snapshot&) = delete;
		bounded_memory_backend_snapshot& operator=(const bounded_memory_backend_snapshot&) = delete;
		~bounded_memory_backend_snapshot();

		[[nodiscard]] const bounded_memory_publication& publication() const noexcept;
		[[nodiscard]] sdk::result<std::unique_ptr<bounded_memory_record_cursor>>
		open_cursor() const;
		/** Recompute payload and publication identities without mutating the backend. */
		[[nodiscard]] sdk::result<void> verify_identity() const;
		[[nodiscard]] std::span<const std::byte> final_payload() const noexcept;

	  private:
		struct state;
		explicit bounded_memory_backend_snapshot(std::shared_ptr<const state> state);
		std::unique_ptr<state> state_;
		friend class bounded_memory_backend;
	};

	/**
	 * One mutable staging session.  The session owns no task vector: every task and physical record
	 * is framed directly into the eventual immutable payload.  `publish_once` is the sole effectful
	 * operation and performs an in-process compare-and-swap against `expected_head`.
	 */
	class bounded_memory_backend_session
	{
	  public:
		bounded_memory_backend_session(bounded_memory_backend_session&&) noexcept;
		bounded_memory_backend_session& operator=(bounded_memory_backend_session&&) noexcept;
		bounded_memory_backend_session(const bounded_memory_backend_session&) = delete;
		bounded_memory_backend_session& operator=(const bounded_memory_backend_session&) = delete;
		~bounded_memory_backend_session();

		[[nodiscard]] std::string_view candidate_id() const noexcept;
		[[nodiscard]] std::string_view expected_head() const noexcept;
		[[nodiscard]] std::uint64_t task_count() const noexcept;
		[[nodiscard]] std::uint64_t record_count() const noexcept;
		[[nodiscard]] std::uint64_t payload_bytes() const noexcept;
		[[nodiscard]] bool sealed() const noexcept;
		[[nodiscard]] bool parity_verified() const noexcept;
		[[nodiscard]] std::optional<bounded_memory_publication_terminal>
		publication_terminal() const noexcept;
		[[nodiscard]] const std::optional<bounded_memory_publication>& publication() const noexcept;

		/** Append a task result without retaining the caller's task vector. */
		[[nodiscard]] sdk::result<void> append_task(std::span<const std::byte> task_payload);
		/** Append one physical record in the order selected by the backend cursor. */
		[[nodiscard]] sdk::result<void> append_record(bounded_memory_record record);
		/** Seal the single final payload and derive its content identity. */
		[[nodiscard]] sdk::result<void> seal();
		/** Open the actual cursor over physical staged records after sealing. */
		[[nodiscard]] sdk::result<std::unique_ptr<bounded_memory_record_cursor>>
		open_cursor() const;

		using expected_record_reader =
			std::function<sdk::result<std::optional<bounded_memory_record>>()>;
		/** Compare a streamed expected projection with the physical cursor, one record at a time.
		 */
		[[nodiscard]] sdk::result<bounded_memory_parity_receipt>
		compare_expected(expected_record_reader expected);

		/** Perform exactly one CAS/publication attempt; retry is never represented. */
		[[nodiscard]] sdk::result<bounded_memory_publication_terminal> publish_once();

	  private:
		struct state;
		explicit bounded_memory_backend_session(std::unique_ptr<state> state);
		std::unique_ptr<state> state_;
		friend class bounded_memory_backend;
	};

	/**
	 * Process-local bounded memory backend.  It is intentionally not the SDK `snapshot_store`; the
	 * session core must later port the typed publication identity and cursor to that public writer.
	 */
	class bounded_memory_backend
	{
	  public:
		struct options
		{
			bounded_memory_backend_limits limits;
		};

		bounded_memory_backend();
		explicit bounded_memory_backend(options value);
		bounded_memory_backend(options value, std::shared_ptr<bounded_memory_cas_port> cas_port);
		bounded_memory_backend(bounded_memory_backend&&) noexcept;
		bounded_memory_backend& operator=(bounded_memory_backend&&) noexcept;
		bounded_memory_backend(const bounded_memory_backend&) = delete;
		bounded_memory_backend& operator=(const bounded_memory_backend&) = delete;
		~bounded_memory_backend();

		[[nodiscard]] sdk::result<bounded_memory_backend_session> begin(std::string candidate_id,
																		std::string expected_head);
		[[nodiscard]] sdk::result<bounded_memory_backend_snapshot>
		reopen(std::string_view publication_id) const;
		[[nodiscard]] sdk::result<std::string> current_head() const;
		[[nodiscard]] std::uint64_t committed_publication_count() const noexcept;

	  private:
		struct state;
		std::shared_ptr<state> state_;
		friend class bounded_memory_backend_session;
		friend class bounded_memory_backend_snapshot;
	};

	/** Derive the publication identity used by CAS and reopen verification. */
	[[nodiscard]] sdk::result<std::string>
	derive_bounded_memory_publication_id(std::string_view candidate_id,
										 std::string_view parent_publication,
										 std::string_view payload_digest,
										 std::uint64_t sequence);
} // namespace cxxlens::detail::clang22::materialization
