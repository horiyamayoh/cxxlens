#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace cxxlens::sdk
{
	/** Closed result of one exact sequential file write. */
	enum class store_write_state : std::uint8_t
	{
		complete,
		resource_exhausted,
		failed,
		outcome_unknown,
	};

	struct store_write_outcome
	{
		store_write_state state{store_write_state::failed};
		std::size_t transferred_byte_count{};
		int native_code{};
		bool operation_attempted{};
		bool effect_may_have_occurred{};

		[[nodiscard]] constexpr bool
		completed(const std::size_t requested_byte_count) const noexcept
		{
			return state == store_write_state::complete &&
				transferred_byte_count == requested_byte_count && native_code == 0;
		}

		[[nodiscard]] constexpr bool operator==(const store_write_outcome&) const = default;
	};

	/** The durability boundary requested from a held filesystem descriptor. */
	enum class store_sync_target : std::uint8_t
	{
		file_data,
		file,
		parent_directory,
	};

	enum class store_sync_state : std::uint8_t
	{
		durable,
		resource_exhausted,
		failed,
		outcome_unknown,
	};

	struct store_sync_outcome
	{
		store_sync_target target{store_sync_target::file};
		store_sync_state state{store_sync_state::failed};
		int native_code{};
		bool operation_attempted{};

		[[nodiscard]] constexpr bool durable() const noexcept
		{
			return state == store_sync_state::durable && native_code == 0;
		}

		[[nodiscard]] constexpr bool operator==(const store_sync_outcome&) const = default;
	};

	enum class store_close_state : std::uint8_t
	{
		confirmed_closed,
		not_attempted,
		outcome_unknown,
	};

	struct store_close_outcome
	{
		store_close_state state{store_close_state::not_attempted};
		int native_code{};
		bool operation_attempted{};

		[[nodiscard]] constexpr bool confirmed() const noexcept
		{
			return state == store_close_state::confirmed_closed && native_code == 0;
		}

		[[nodiscard]] constexpr bool operator==(const store_close_outcome&) const = default;
	};

	/** Product backends which traverse the same private observation boundary. */
	enum class store_backend_kind : std::uint8_t
	{
		memory,
		sqlite,
	};

	/** Closed Store phases; no candidate graph, writer, or native handle crosses this boundary. */
	enum class store_backend_operation : std::uint8_t
	{
		stage_record,
		seal_staging,
		open_physical_cursor,
		finish_physical_cursor,
		compare_projection,
		validate_current,
		allocate_publication_sequence,
		allocate_physical_generation,
		publish_once,
		reopen_factory,
		lookup_current,
		lookup_expected_parent,
		lookup_publication,
		lookup_snapshot,
		canonical_export,
		abort_staging,
	};

	/** Typed observation faults available only through an external tests/support decorator. */
	enum class store_backend_observation_fault : std::uint8_t
	{
		none,
		corrupt_current,
		corrupt_publication,
		snapshot_ambiguous,
		publication_sequence_exhausted,
		physical_generation_exhausted,
		projection_mismatch,
		backend_failure,
		commit_outcome_unknown,
	};

	enum class store_backend_observation_point : std::uint8_t
	{
		before_operation,
		after_operation,
	};

	/**
	 * Borrowed identifiers are valid only for observe_backend_operation().  The event carries no
	 * mutable candidate state and cannot authorize or perform a backend effect.
	 */
	struct store_backend_operation_event
	{
		store_backend_kind backend{store_backend_kind::memory};
		store_backend_operation operation{store_backend_operation::stage_record};
		store_backend_observation_point point{store_backend_observation_point::before_operation};
		std::string_view backend_binding;
		std::string_view staging_session_id;
		std::string_view series_id;
		std::string_view candidate_id;
		std::string_view snapshot_id;
		std::string_view publication_id;
		std::uint64_t ordinal{};
		std::uint64_t record_count{};
		std::uint64_t framed_bytes{};
		std::uint64_t publication_sequence{};
		std::uint64_t physical_generation{};
		bool operation_attempted{};
		bool effect_may_have_occurred{};

		[[nodiscard]] constexpr bool
		operator==(const store_backend_operation_event&) const = default;
	};

	struct store_backend_operation_observation
	{
		store_backend_observation_fault fault{store_backend_observation_fault::none};
		int native_code{};
		bool operation_attempted{};
		bool effect_may_have_occurred{};

		[[nodiscard]] constexpr bool
		operator==(const store_backend_operation_observation&) const = default;
	};

	enum class store_commit_state : std::uint8_t
	{
		committed,
		not_attempted,
		resource_exhausted,
		outcome_unknown,
	};

	struct store_commit_outcome
	{
		store_commit_state state{store_commit_state::not_attempted};
		int native_code{};
		bool operation_attempted{};
		bool effect_may_have_occurred{};

		[[nodiscard]] constexpr bool confirmed() const noexcept
		{
			return state == store_commit_state::committed && native_code == 0;
		}

		[[nodiscard]] constexpr bool operator==(const store_commit_outcome&) const = default;
	};

	/**
	 * One already-bound SQLite operation. The callback performs exactly one close or commit against
	 * `context`; the port never constructs SQL, resolves a library, or guesses a native result
	 * code.
	 */
	using store_sqlite_operation_callback = int (*)(void* context);

	struct store_sqlite_operation_binding
	{
		void* context{};
		store_sqlite_operation_callback invoke{};
		int success_code{};
		/** Defaults to SQLite's stable SQLITE_FULL result code without importing sqlite3.h. */
		int resource_exhausted_code{13};
	};

	/**
	 * Neutral filesystem/SQLite effect port used by Store persistence and normalization owners.
	 *
	 * The port owns no descriptor or SQLite handle. A close method consumes the caller's one close
	 * attempt: `outcome_unknown` must be quarantined and must never be retried. Likewise, a sync or
	 * commit ambiguity is not a retryable ordinary failure and cannot mint a durability receipt.
	 */
	class store_operation_port
	{
	  public:
		virtual ~store_operation_port() = default;

		[[nodiscard]] virtual store_write_outcome
		write_exact(int descriptor, std::span<const std::byte> bytes) noexcept = 0;
		[[nodiscard]] virtual store_sync_outcome synchronize(int descriptor,
															 store_sync_target target) noexcept = 0;
		[[nodiscard]] virtual store_close_outcome close_descriptor(int descriptor) noexcept = 0;
		[[nodiscard]] virtual store_close_outcome
		close_sqlite(store_sqlite_operation_binding binding) noexcept = 0;
		[[nodiscard]] virtual store_commit_outcome
		commit_sqlite(store_sqlite_operation_binding binding) noexcept = 0;
		/**
		 * Product-private observation boundary.  The default is inert so existing persistence ports
		 * remain source-compatible; Store backends must still derive and perform every operation.
		 */
		[[nodiscard]] virtual store_backend_operation_observation
		observe_backend_operation(const store_backend_operation_event&) noexcept
		{
			return {};
		}
	};

	/**
	 * POSIX-backed production adapter with no alternate outcome command or mutable script state.
	 */
	class default_store_operation_port final : public store_operation_port
	{
	  public:
		[[nodiscard]] store_write_outcome
		write_exact(int descriptor, std::span<const std::byte> bytes) noexcept override;
		[[nodiscard]] store_sync_outcome synchronize(int descriptor,
													 store_sync_target target) noexcept override;
		[[nodiscard]] store_close_outcome close_descriptor(int descriptor) noexcept override;
		[[nodiscard]] store_close_outcome
		close_sqlite(store_sqlite_operation_binding binding) noexcept override;
		[[nodiscard]] store_commit_outcome
		commit_sqlite(store_sqlite_operation_binding binding) noexcept override;
		[[nodiscard]] store_backend_operation_observation
		observe_backend_operation(const store_backend_operation_event& event) noexcept override;
	};

	/** Owned production port retained by Store and every private phase token. */
	[[nodiscard]] std::shared_ptr<store_operation_port> make_default_store_operation_port();
} // namespace cxxlens::sdk
