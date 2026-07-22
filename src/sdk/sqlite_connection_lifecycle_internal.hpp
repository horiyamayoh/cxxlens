#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <variant>

namespace cxxlens::sdk
{
	using sqlite_close_v2_callback = int (*)(void*);

	/** Pins which must remain alive until a SQLite connection is confirmed closed. */
	struct sqlite_connection_lifetime_pins
	{
		std::shared_ptr<void> runtime;
		std::shared_ptr<void> vfs;
		std::shared_ptr<void> observation;
		std::shared_ptr<const void> authority_anchor;
	};

	enum class sqlite_confirmed_close_kind : std::uint8_t
	{
		no_connection,
		sqlite_ok,
	};

	/** Move-only proof that no connection remains owned by one lifecycle object. */
	class sqlite_confirmed_close_token
	{
	  public:
		sqlite_confirmed_close_token(sqlite_confirmed_close_token&& other) noexcept;
		sqlite_confirmed_close_token& operator=(sqlite_confirmed_close_token&& other) noexcept;
		sqlite_confirmed_close_token(const sqlite_confirmed_close_token&) = delete;
		sqlite_confirmed_close_token& operator=(const sqlite_confirmed_close_token&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] sqlite_confirmed_close_kind kind() const noexcept;
		[[nodiscard]] bool close_was_attempted() const noexcept;

	  private:
		friend class sqlite_connection_lifecycle;
		explicit sqlite_confirmed_close_token(sqlite_confirmed_close_kind kind) noexcept;

		sqlite_confirmed_close_kind kind_{sqlite_confirmed_close_kind::no_connection};
		bool valid_{true};
	};

	enum class sqlite_connection_quarantine_reason : std::uint8_t
	{
		close_non_ok,
		close_callback_missing,
		close_callback_threw,
	};

	/**
	 * Move-only receipt for an unclosed handle transferred into process-lifetime quarantine.
	 * The receipt does not own the handle or pins and cannot release or retry them.
	 */
	class sqlite_quarantined_connection
	{
	  public:
		sqlite_quarantined_connection(sqlite_quarantined_connection&& other) noexcept;
		sqlite_quarantined_connection& operator=(sqlite_quarantined_connection&& other) noexcept;
		sqlite_quarantined_connection(const sqlite_quarantined_connection&) = delete;
		sqlite_quarantined_connection& operator=(const sqlite_quarantined_connection&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] sqlite_connection_quarantine_reason reason() const noexcept;
		[[nodiscard]] std::optional<int> sqlite_code() const noexcept;

	  private:
		friend class sqlite_connection_lifecycle;
		sqlite_quarantined_connection(sqlite_connection_quarantine_reason reason,
									  std::optional<int> sqlite_code) noexcept;

		sqlite_connection_quarantine_reason reason_{
			sqlite_connection_quarantine_reason::close_callback_missing};
		std::optional<int> sqlite_code_;
		bool valid_{true};
	};

	using sqlite_connection_close_outcome =
		std::variant<sqlite_confirmed_close_token, sqlite_quarantined_connection>;

	/**
	 * Source-private exact-once owner for one raw SQLite connection.
	 *
	 * Ownership transfers only after successful construction. A non-null handle receives at most
	 * one `close_v2` call. A non-OK or unknown close result permanently quarantines the handle,
	 * callback, and all pins without retrying or releasing them.
	 */
	class sqlite_connection_lifecycle
	{
	  public:
		sqlite_connection_lifecycle(void* connection,
									sqlite_close_v2_callback close_v2,
									sqlite_connection_lifetime_pins pins);
		~sqlite_connection_lifecycle() noexcept;

		sqlite_connection_lifecycle(sqlite_connection_lifecycle&& other) noexcept;
		sqlite_connection_lifecycle& operator=(sqlite_connection_lifecycle&& other) noexcept;
		sqlite_connection_lifecycle(const sqlite_connection_lifecycle&) = delete;
		sqlite_connection_lifecycle& operator=(const sqlite_connection_lifecycle&) = delete;

		[[nodiscard]] bool owns_connection() const noexcept;
		[[nodiscard]] void* get() const noexcept;
		/**
		 * Returns SQLite's one open-result slot while this owner has no connection.
		 * Construct the lifecycle before calling SQLite so even a non-OK/non-null result is owned.
		 */
		[[nodiscard]] void** open_handle_out_parameter() noexcept;
		[[nodiscard]] sqlite_connection_close_outcome close_exactly_once() noexcept;

	  private:
		struct state;

		[[nodiscard]] static sqlite_quarantined_connection
		quarantine(std::unique_ptr<state> owned,
				   sqlite_connection_quarantine_reason reason,
				   std::optional<int> sqlite_code) noexcept;
		void cleanup_noexcept() noexcept;

		std::unique_ptr<state> state_;
	};
} // namespace cxxlens::sdk
