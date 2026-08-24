#pragma once

/**
 * @file bounded_store_v6_internal.hpp
 * @brief The source-private bounded Store publication state machine.
 *
 * This header is deliberately independent from the public snapshot writer.  A production
 * materializer supplies a typed backend port and a typed expected projection cursor.  The phase
 * core owns all capability construction; callers cannot manufacture a validation, publication,
 * terminal, or report-release token from an observation or a string.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/store.hpp>

namespace cxxlens::sdk::detail
{
	// Product limits.  They are constants, not caller-selectable budgets.  A smaller value would
	// make a supported input fail for an operational reason, while a larger value would invalidate
	// the bounded-memory contract.
	inline constexpr std::uint64_t bounded_store_v6_max_tasks = 4'096U;
	inline constexpr std::uint64_t bounded_store_v6_max_aggregate_bytes = 512U * 1024U * 1024U;
	inline constexpr std::uint64_t bounded_store_v6_source_window_bytes = 64U * 1024U * 1024U;
	inline constexpr std::uint64_t bounded_store_v6_sort_arena_bytes = 8U * 1024U * 1024U;
	inline constexpr std::uint64_t bounded_store_v6_comparator_cursor_bytes = 32U * 1024U;
	inline constexpr std::uint64_t bounded_store_v6_backend_cursor_bytes = 1024U * 1024U;
	inline constexpr std::uint64_t bounded_store_v6_codec_hash_bytes = 64U * 1024U;
	inline constexpr std::uint64_t bounded_store_v6_record_buffer_bytes = 1024U * 1024U;
	inline constexpr std::uint64_t bounded_store_v6_counter_state_bytes = 4U * 1024U;
	inline constexpr std::uint64_t bounded_store_v6_resident_window_bytes = 77'729'792U;
	inline constexpr std::uint64_t bounded_store_v6_merge_file_descriptors = 18U;
	inline constexpr std::uint64_t bounded_store_v6_sqlite_chunk_bytes = 8U * 1024U * 1024U;
	inline constexpr std::uint64_t bounded_store_v6_stream_header_bytes = 86U;
	inline constexpr std::uint64_t bounded_store_v6_stream_trailer_bytes = 112U;
	inline constexpr std::uint64_t bounded_store_v6_record_fixed_bytes = 49U;
	inline constexpr std::uint64_t bounded_store_v6_exact_report_tail_bytes = 28'321'546U;
	inline constexpr std::uint64_t bounded_store_v6_max_report_bytes = 1024U * 1024U * 1024U;
	inline constexpr std::uint32_t bounded_store_v6_publication_terminal_count = 6U;
	inline constexpr std::uint32_t bounded_store_v6_report_section_count = 19U;

	static_assert(bounded_store_v6_record_fixed_bytes == 49U);
	static_assert(bounded_store_v6_source_window_bytes + bounded_store_v6_sort_arena_bytes +
					  bounded_store_v6_comparator_cursor_bytes * 2U +
					  bounded_store_v6_backend_cursor_bytes + bounded_store_v6_codec_hash_bytes +
					  bounded_store_v6_record_buffer_bytes + bounded_store_v6_counter_state_bytes ==
				  bounded_store_v6_resident_window_bytes);

	enum class bounded_store_v6_backend : std::uint8_t
	{
		memory,
		sqlite,
	};

	[[nodiscard]] constexpr bool is_valid(const bounded_store_v6_backend value) noexcept
	{
		return value >= bounded_store_v6_backend::memory &&
			value <= bounded_store_v6_backend::sqlite;
	}

	enum class bounded_store_v6_publication_terminal : std::uint8_t
	{
		not_attempted,
		rejected_stale,
		rejected_store_failure,
		publication_outcome_unknown,
		committed_unverified,
		committed_verified,
	};

	static_assert(
		static_cast<std::uint8_t>(bounded_store_v6_publication_terminal::committed_verified) + 1U ==
		bounded_store_v6_publication_terminal_count);

	enum class bounded_store_v6_record_kind : std::uint8_t
	{
		partition_begin = 1U,
		claim_occurrence = 2U,
		detached_row = 3U,
		claim_annotation = 4U,
		coverage = 5U,
		unresolved = 6U,
		partition_end = 7U,
	};

	[[nodiscard]] constexpr bool is_valid(const bounded_store_v6_record_kind value) noexcept
	{
		return value >= bounded_store_v6_record_kind::partition_begin &&
			value <= bounded_store_v6_record_kind::partition_end;
	}

	[[nodiscard]] constexpr bool
	is_valid(const bounded_store_v6_publication_terminal value) noexcept
	{
		return value >= bounded_store_v6_publication_terminal::not_attempted &&
			value <= bounded_store_v6_publication_terminal::committed_verified;
	}

	/** Extent announced by a forward-only source before its frame is read. */
	struct bounded_store_v6_record_extent
	{
		bounded_store_v6_record_kind kind{bounded_store_v6_record_kind::partition_begin};
		std::uint64_t key_bytes{};
		std::uint64_t payload_bytes{};
		std::uint64_t framed_bytes{};

		[[nodiscard]] bool operator==(const bounded_store_v6_record_extent&) const = default;
	};

	/** Bounded identity returned by Store, never a full snapshot graph. */
	struct bounded_store_v6_publication_observation
	{
		std::string publication_id;
		std::string series_id;
		std::string snapshot_id;
		std::uint64_t sequence{};
		std::uint64_t physical_generation{};
		std::optional<std::string> parent_publication;
		publication_state state{publication_state::created};
		bool corrupt{};

		[[nodiscard]] bool
		operator==(const bounded_store_v6_publication_observation&) const = default;
	};

	struct bounded_store_v6_snapshot_observation
	{
		std::string snapshot_id;
		std::uint64_t partition_count{};
		std::uint64_t row_count{};
		std::uint64_t claim_count{};
		std::uint64_t coverage_count{};
		std::uint64_t unresolved_count{};
		std::string semantic_projection_digest;
		std::string canonical_export_digest;

		[[nodiscard]] bool operator==(const bounded_store_v6_snapshot_observation&) const = default;
	};

	struct bounded_store_v6_lookup_observation
	{
		enum class state : std::uint8_t
		{
			not_attempted,
			present,
			not_found,
			failed,
		};
		state status{state::not_attempted};
		std::optional<error> failure;
		std::optional<bounded_store_v6_publication_observation> publication;
		std::optional<bounded_store_v6_snapshot_observation> snapshot;

		[[nodiscard]] bool operator==(const bounded_store_v6_lookup_observation&) const = default;
	};

	/** Fresh-open data is deliberately a bounded summary, not a copied manifest or graph. */
	struct bounded_store_v6_reopen_observation
	{
		bool factory_attempted{};
		std::optional<error> factory_error;
		std::string fresh_backend_binding;
		bounded_store_v6_lookup_observation current;
		bounded_store_v6_lookup_observation expected_parent;
		bounded_store_v6_lookup_observation publication;
		bounded_store_v6_lookup_observation snapshot;
		std::optional<std::string> canonical_export_digest;
		std::optional<error> canonical_export_error;

		[[nodiscard]] bool operator==(const bounded_store_v6_reopen_observation&) const = default;
	};

	struct bounded_store_v6_effect_result
	{
		/** The backend reports only its exact SDK operation result; the core classifies it. */
		std::optional<error> failure;
		std::optional<bounded_store_v6_publication_observation> publication;
		bool operation_attempted{};
		bool effect_may_have_occurred{};
		bool returned_handle{};

		[[nodiscard]] bool operator==(const bounded_store_v6_effect_result&) const = default;
	};

	/** A closed genesis or publication identity used by the compare-and-swap port. */
	struct bounded_store_v6_expected_head
	{
		enum class kind : std::uint8_t
		{
			genesis,
			publication,
		};
		kind value{kind::genesis};
		snapshot_series_selector selector;
		std::optional<bounded_store_v6_publication_observation> publication;
		std::optional<bounded_store_v6_snapshot_observation> snapshot;

		[[nodiscard]] bool operator==(const bounded_store_v6_expected_head&) const = default;
	};

	[[nodiscard]] constexpr bool is_valid(const bounded_store_v6_expected_head::kind value) noexcept
	{
		return value >= bounded_store_v6_expected_head::kind::genesis &&
			value <= bounded_store_v6_expected_head::kind::publication;
	}

	struct bounded_store_v6_external_census
	{
		std::uint64_t task_count{};
		std::uint64_t partition_count{};
		std::uint64_t event_count{};
		std::uint64_t claim_count{};
		std::uint64_t row_count{};
		std::uint64_t annotation_count{};
		std::uint64_t coverage_count{};
		std::uint64_t unresolved_count{};
		std::uint64_t input_bytes{};
		std::array<std::byte, 32U> binary_input_sha256{};
		std::string immutable_authority_binding;

		[[nodiscard]] bool operator==(const bounded_store_v6_external_census&) const = default;
	};

	/** Closed per-task receipt independently sealed by the request/journal path. */
	struct bounded_store_v6_task_receipt
	{
		std::string task_id;
		std::uint64_t ordinal{};
		std::uint64_t partition_count{};
		std::uint64_t event_count{};
		std::uint64_t claim_count{};
		std::uint64_t row_count{};
		std::uint64_t annotation_count{};
		std::uint64_t coverage_count{};
		std::uint64_t unresolved_count{};
		std::uint64_t framed_bytes{};
		std::array<std::byte, 32U> binary_sha256{};
		std::string immutable_binding;

		[[nodiscard]] bool operator==(const bounded_store_v6_task_receipt&) const = default;
	};

	struct bounded_store_v6_measured_projection
	{
		std::uint64_t record_count{};
		std::uint64_t framed_bytes{};
		std::array<std::byte, 32U> binary_sha256{};
		/**
		 * Backend-derived semantic observation.  The common comparator authenticates the physical
		 * frame stream independently; it must never manufacture semantic identity by relabelling
		 * the physical frame digest.
		 */
		bounded_store_v6_snapshot_observation candidate_snapshot;
		std::string immutable_binding;

		[[nodiscard]] bool operator==(const bounded_store_v6_measured_projection&) const = default;
	};

	struct bounded_store_v6_physical_cursor_observation
	{
		std::uint64_t record_count{};
		std::uint64_t framed_bytes{};
		std::array<std::byte, 32U> binary_sha256{};
		std::string physical_anchor_binding;
		bool canonical_order_validated{};

		[[nodiscard]] bool
		operator==(const bounded_store_v6_physical_cursor_observation&) const = default;
	};

	struct bounded_store_v6_session_metadata
	{
		bounded_store_v6_backend backend{bounded_store_v6_backend::memory};
		std::string relation_engine_generation;
		snapshot_series_selector selector;
		std::string staging_session_id;
		std::optional<std::string> exact_sqlite_path;
		bounded_store_v6_expected_head expected_head;

		[[nodiscard]] bool operator==(const bounded_store_v6_session_metadata&) const = default;
	};

	/** Canonical identity of the sealed expected input. */
	struct bounded_store_v6_input_observation
	{
		std::string staging_session_id;
		bounded_store_v6_expected_head expected_head;
		bounded_store_v6_external_census external_census;

		[[nodiscard]] bool operator==(const bounded_store_v6_input_observation&) const = default;
	};

	/** One lossless typed event emitted by the pre-encoder semantic oracle. */
	struct bounded_store_v6_semantic_record
	{
		bounded_store_v6_record_kind kind{bounded_store_v6_record_kind::partition_begin};
		canonical_value key;
		canonical_value payload;

		[[nodiscard]] bool operator==(const bounded_store_v6_semantic_record&) const = default;
	};

	/** Raw canonical task stream.  It is never accepted as the independent expected oracle. */
	class bounded_store_v6_task_frame_source
	{
	  public:
		virtual ~bounded_store_v6_task_frame_source() = default;
		[[nodiscard]] virtual result<std::optional<bounded_store_v6_record_extent>>
		next_record() = 0;
		[[nodiscard]] virtual result<std::size_t>
		read_record_bytes(std::span<std::byte> destination) = 0;
		[[nodiscard]] virtual result<bool> canonical_order_validated() const = 0;
	};

	/** Independent sealed request/journal traversal; it never exposes pre-framed transport bytes.
	 */
	class bounded_store_v6_expected_semantic_cursor
	{
	  public:
		virtual ~bounded_store_v6_expected_semantic_cursor() = default;
		[[nodiscard]] virtual result<std::optional<bounded_store_v6_semantic_record>>
		next_semantic_record() = 0;
		[[nodiscard]] virtual result<bool> authority_complete() const = 0;
	};

	/** Opaque physical identity shared by prepared backing, actual cursor, terminal and report. */
	struct bounded_store_v6_physical_anchor;

	/** Backend-authenticated actual traversal; expected cursors cannot be passed as actuals. */
	class bounded_store_v6_actual_cursor_source
	{
	  public:
		virtual ~bounded_store_v6_actual_cursor_source() = default;
		[[nodiscard]] virtual result<std::optional<bounded_store_v6_record_extent>>
		next_record() = 0;
		[[nodiscard]] virtual result<std::size_t>
		read_record_bytes(std::span<std::byte> destination) = 0;
		[[nodiscard]] virtual std::shared_ptr<const bounded_store_v6_physical_anchor>
		physical_anchor() const noexcept = 0;
		[[nodiscard]] virtual result<bounded_store_v6_physical_cursor_observation> finish() = 0;
	};

	/**
	 * Typed backend port.  A backend owns its physical staging rows and writer/lease.  The common
	 * core gives it exact framed bytes one record at a time; publication and reopen have no caller
	 * supplied graph, callback, raw pointer, or snapshot_writer escape hatch.
	 */
	class bounded_store_v6_backend_port
	{
	  public:
		virtual ~bounded_store_v6_backend_port() = default;
		[[nodiscard]] virtual bounded_store_v6_backend backend() const noexcept = 0;
		[[nodiscard]] virtual result<void>
		bind_physical_anchor(std::shared_ptr<const bounded_store_v6_physical_anchor> anchor) = 0;
		[[nodiscard]] virtual std::shared_ptr<const bounded_store_v6_physical_anchor>
		physical_anchor() const noexcept = 0;
		[[nodiscard]] virtual std::string_view physical_anchor_binding() const noexcept = 0;
		[[nodiscard]] virtual result<void>
		begin_record(const bounded_store_v6_record_extent& extent) = 0;
		[[nodiscard]] virtual result<void>
		append_record_bytes(std::span<const std::byte> bytes) = 0;
		[[nodiscard]] virtual result<void> finish_record() = 0;
		[[nodiscard]] virtual result<void> seal_staging() = 0;
		[[nodiscard]] virtual result<bounded_store_v6_measured_projection>
		measured_projection() const = 0;
		[[nodiscard]] virtual result<std::unique_ptr<bounded_store_v6_actual_cursor_source>>
		open_actual_cursor() = 0;
		[[nodiscard]] virtual result<bounded_store_v6_effect_result> publish_once() = 0;
		[[nodiscard]] virtual result<bounded_store_v6_reopen_observation> reopen() = 0;
		/** Bounded private cleanup; this is called at most once. */
		[[nodiscard]] virtual result<void> abort_staging() = 0;
	};

	[[nodiscard]] result<bounded_store_v6_expected_head>
	make_bounded_store_v6_genesis_head(snapshot_series_selector selector);
	[[nodiscard]] result<bounded_store_v6_expected_head>
	make_bounded_store_v6_publication_head(snapshot_series_selector selector,
										   bounded_store_v6_publication_observation publication,
										   bounded_store_v6_snapshot_observation snapshot);

	/** Move-only state token after the external input census is sealed; candidate identity is later
	 * minted only after backend staging has been measured and sealed. */
	class bounded_store_v6_staging_session final
	{
	  public:
		bounded_store_v6_staging_session(const bounded_store_v6_staging_session&) = delete;
		bounded_store_v6_staging_session&
		operator=(const bounded_store_v6_staging_session&) = delete;
		bounded_store_v6_staging_session(bounded_store_v6_staging_session&&) noexcept;
		bounded_store_v6_staging_session& operator=(bounded_store_v6_staging_session&&) noexcept;
		~bounded_store_v6_staging_session();

		[[nodiscard]] std::string_view staging_session_id() const noexcept;
		[[nodiscard]] bounded_store_v6_backend backend() const noexcept;

	  private:
		struct state;
		explicit bounded_store_v6_staging_session(std::unique_ptr<state>);
		std::unique_ptr<state> state_;
		friend class bounded_store_v6_phase_core;
	};

	class bounded_store_sealed_input_binding final
	{
	  public:
		bounded_store_sealed_input_binding(const bounded_store_sealed_input_binding&) = delete;
		bounded_store_sealed_input_binding&
		operator=(const bounded_store_sealed_input_binding&) = delete;
		bounded_store_sealed_input_binding(bounded_store_sealed_input_binding&&) noexcept;
		bounded_store_sealed_input_binding&
		operator=(bounded_store_sealed_input_binding&&) noexcept;
		~bounded_store_sealed_input_binding();

		[[nodiscard]] const bounded_store_v6_input_observation& observation() const noexcept;
		[[nodiscard]] std::string_view staging_session_id() const noexcept;
		[[nodiscard]] const bounded_store_v6_expected_head& expected_head() const noexcept;
		[[nodiscard]] const bounded_store_v6_external_census& external_census() const noexcept;

	  private:
		struct state;
		explicit bounded_store_sealed_input_binding(std::unique_ptr<state>);
		std::unique_ptr<state> state_;
		friend class bounded_store_v6_phase_core;
	};

	/** Move-only task source; only the common core can consume and authenticate its receipt. */
	class bounded_store_v6_sealed_task final
	{
	  public:
		bounded_store_v6_sealed_task(const bounded_store_v6_sealed_task&) = delete;
		bounded_store_v6_sealed_task& operator=(const bounded_store_v6_sealed_task&) = delete;
		bounded_store_v6_sealed_task(bounded_store_v6_sealed_task&&) noexcept;
		bounded_store_v6_sealed_task& operator=(bounded_store_v6_sealed_task&&) noexcept;
		~bounded_store_v6_sealed_task();

		[[nodiscard]] const bounded_store_v6_task_receipt& receipt() const noexcept;

	  private:
		struct state;
		explicit bounded_store_v6_sealed_task(std::unique_ptr<state>);
		std::unique_ptr<state> state_;
		friend class bounded_store_v6_phase_core;
	};

	/** Expected cursor plus the immutable input binding; construction is core-only. */
	class bounded_store_v6_expected_projection final
	{
	  public:
		bounded_store_v6_expected_projection(const bounded_store_v6_expected_projection&) = delete;
		bounded_store_v6_expected_projection&
		operator=(const bounded_store_v6_expected_projection&) = delete;
		bounded_store_v6_expected_projection(bounded_store_v6_expected_projection&&) noexcept;
		bounded_store_v6_expected_projection&
		operator=(bounded_store_v6_expected_projection&&) noexcept;
		~bounded_store_v6_expected_projection();

		[[nodiscard]] const bounded_store_v6_input_observation& observation() const noexcept;

	  private:
		struct state;
		explicit bounded_store_v6_expected_projection(std::unique_ptr<state>);
		std::unique_ptr<state> state_;
		friend class bounded_store_v6_phase_core;
	};

	struct bounded_store_prepared_publication_observation
	{
		bounded_store_v6_backend backend{bounded_store_v6_backend::memory};
		std::optional<std::string> exact_sqlite_path;
		std::string relation_engine_generation;
		std::string series_id;
		std::string staging_session_id;
		std::string candidate_id;
		std::string candidate_snapshot_id;
		bounded_store_v6_expected_head expected_head;
		std::string physical_anchor_binding;

		[[nodiscard]] bool
		operator==(const bounded_store_prepared_publication_observation&) const = default;
	};

	class bounded_store_prepared_publication final
	{
	  public:
		bounded_store_prepared_publication(const bounded_store_prepared_publication&) = delete;
		bounded_store_prepared_publication&
		operator=(const bounded_store_prepared_publication&) = delete;
		bounded_store_prepared_publication(bounded_store_prepared_publication&&) noexcept;
		bounded_store_prepared_publication&
		operator=(bounded_store_prepared_publication&&) noexcept;
		~bounded_store_prepared_publication();

		[[nodiscard]] const bounded_store_prepared_publication_observation&
		observation() const noexcept;
		[[nodiscard]] std::optional<bounded_store_v6_backend> backend() const noexcept;
		[[nodiscard]] std::string_view staging_session_id() const noexcept;
		[[nodiscard]] std::string_view candidate_id() const noexcept;
		[[nodiscard]] const bounded_store_v6_expected_head& expected_head() const noexcept;

	  private:
		struct state;
		explicit bounded_store_prepared_publication(std::unique_ptr<state>);
		std::unique_ptr<state> state_;
		friend class bounded_store_v6_phase_core;
	};

	/** One-shot actual cursor.  The byte operations are private to the phase core. */
	class bounded_store_authenticated_actual_cursor final
	{
	  public:
		bounded_store_authenticated_actual_cursor(
			const bounded_store_authenticated_actual_cursor&) = delete;
		bounded_store_authenticated_actual_cursor&
		operator=(const bounded_store_authenticated_actual_cursor&) = delete;
		bounded_store_authenticated_actual_cursor(
			bounded_store_authenticated_actual_cursor&&) noexcept;
		bounded_store_authenticated_actual_cursor&
		operator=(bounded_store_authenticated_actual_cursor&&) noexcept;
		~bounded_store_authenticated_actual_cursor();

	  private:
		struct state;
		explicit bounded_store_authenticated_actual_cursor(std::unique_ptr<state>);
		std::unique_ptr<state> state_;
		friend class bounded_store_v6_phase_core;
	};

	struct bounded_store_projection_match_observation
	{
		std::string staging_session_id;
		std::string candidate_id;
		std::string candidate_snapshot_id;
		bounded_store_v6_snapshot_observation candidate_snapshot;
		bounded_store_v6_expected_head expected_head;
		bounded_store_v6_external_census external_census;
		std::uint64_t record_count{};
		std::uint64_t framed_bytes{};
		std::array<std::byte, 32U> expected_binary_sha256{};
		std::array<std::byte, 32U> actual_binary_sha256{};
		std::string physical_anchor_binding;

		[[nodiscard]] bool
		operator==(const bounded_store_projection_match_observation&) const = default;
	};

	class bounded_store_projection_match final
	{
	  public:
		bounded_store_projection_match(const bounded_store_projection_match&) = delete;
		bounded_store_projection_match& operator=(const bounded_store_projection_match&) = delete;
		bounded_store_projection_match(bounded_store_projection_match&&) noexcept;
		bounded_store_projection_match& operator=(bounded_store_projection_match&&) noexcept;
		~bounded_store_projection_match();

		[[nodiscard]] const bounded_store_projection_match_observation&
		observation() const noexcept;

	  private:
		struct state;
		explicit bounded_store_projection_match(std::unique_ptr<state>);
		std::unique_ptr<state> state_;
		friend class bounded_store_v6_phase_core;
	};

	/** Reservation returned by the report writer before the publication attempt. */
	struct bounded_store_v6_report_tail_reservation
	{
		std::string writer_object_binding;
		std::string spool_object_binding;
		std::string reservation_binding;
		std::uint64_t prefix_bytes{};
		std::uint64_t reserved_tail_bytes{};
		std::uint64_t capacity_bytes{};
		std::uint64_t maximum_report_bytes{bounded_store_v6_max_report_bytes};

		[[nodiscard]] bool
		operator==(const bounded_store_v6_report_tail_reservation&) const = default;
	};

	struct bounded_store_report_tail_custody_observation
	{
		std::string staging_session_id;
		std::string candidate_id;
		bounded_store_v6_snapshot_observation candidate_snapshot;
		bounded_store_v6_expected_head expected_head;
		std::uint64_t prefix_bytes{};
		std::uint64_t reserved_tail_bytes{};
		std::uint64_t capacity_bytes{};
		std::uint64_t maximum_report_bytes{};
		std::string writer_object_binding;
		std::string spool_object_binding;
		std::string reservation_binding;
		std::string physical_anchor_binding;
		bool live{};

		[[nodiscard]] bool
		operator==(const bounded_store_report_tail_custody_observation&) const = default;
	};

	class bounded_store_report_tail_writer
	{
	  public:
		virtual ~bounded_store_report_tail_writer() = default;
		[[nodiscard]] virtual result<bounded_store_v6_report_tail_reservation>
		reserve_maximum_tail(std::uint64_t exact_tail_bytes,
							 std::uint64_t maximum_report_bytes) = 0;
		[[nodiscard]] virtual result<void>
		append_terminal(const bounded_store_v6_publication_terminal terminal) = 0;
		[[nodiscard]] virtual result<void> validate_full_schema() = 0;
		[[nodiscard]] virtual result<void>
		validate_complete_section_census(std::uint32_t exact_section_count) = 0;
		[[nodiscard]] virtual result<void> validate_bottom_up_bindings() = 0;
		[[nodiscard]] virtual result<std::uint64_t> sealed_report_bytes() const = 0;
		[[nodiscard]] virtual result<void> release() = 0;
	};

	class bounded_store_report_tail_custody final
	{
	  public:
		bounded_store_report_tail_custody(const bounded_store_report_tail_custody&) = delete;
		bounded_store_report_tail_custody&
		operator=(const bounded_store_report_tail_custody&) = delete;
		bounded_store_report_tail_custody(bounded_store_report_tail_custody&&) noexcept;
		bounded_store_report_tail_custody& operator=(bounded_store_report_tail_custody&&) noexcept;
		~bounded_store_report_tail_custody();

		[[nodiscard]] const bounded_store_report_tail_custody_observation&
		observation() const noexcept;

	  private:
		struct state;
		explicit bounded_store_report_tail_custody(std::unique_ptr<state>);
		std::unique_ptr<state> state_;
		friend class bounded_store_v6_phase_core;
	};

	class bounded_store_validated_publication final
	{
	  public:
		bounded_store_validated_publication(const bounded_store_validated_publication&) = delete;
		bounded_store_validated_publication&
		operator=(const bounded_store_validated_publication&) = delete;
		bounded_store_validated_publication(bounded_store_validated_publication&&) noexcept;
		bounded_store_validated_publication&
		operator=(bounded_store_validated_publication&&) noexcept;
		~bounded_store_validated_publication();

	  private:
		struct state;
		explicit bounded_store_validated_publication(std::unique_ptr<state>);
		std::unique_ptr<state> state_;
		friend class bounded_store_v6_phase_core;
	};

	enum class bounded_store_v6_verification_failure : std::uint8_t
	{
		returned_handle_missing,
		returned_publication_fields,
		publication_identity,
		parent_publication,
		publication_sequence,
		physical_generation,
		reopen_observation,
		local_verification_unavailable,
	};

	[[nodiscard]] constexpr bool
	is_valid(const bounded_store_v6_verification_failure value) noexcept
	{
		return value >= bounded_store_v6_verification_failure::returned_handle_missing &&
			value <= bounded_store_v6_verification_failure::local_verification_unavailable;
	}

	struct bounded_store_v6_terminal_observation
	{
		bounded_store_v6_backend backend{bounded_store_v6_backend::memory};
		std::string staging_session_id;
		std::string candidate_id;
		std::string candidate_snapshot_id;
		bounded_store_v6_snapshot_observation candidate_snapshot;
		bounded_store_v6_expected_head expected_head;
		std::string series_id;
		std::string physical_anchor_binding;
		/** Digest of the authenticated physical frame stream; never a semantic/export identity. */
		std::array<std::byte, 32U> physical_binary_sha256{};
		bounded_store_v6_publication_terminal terminal{
			bounded_store_v6_publication_terminal::not_attempted};
		std::uint32_t publish_call_count{};
		bool publication_attempted{};
		std::optional<error> sdk_error;
		/** Failure of the effect-capable backend call itself; never reclassified as an SDK tuple.
		 */
		std::optional<error> backend_call_error;
		/** Successful-call mismatch; never fabricated as an SDK error tuple. */
		std::optional<bounded_store_v6_verification_failure> verification_failure;
		std::optional<bounded_store_v6_publication_observation> returned_publication;
		std::optional<bounded_store_v6_snapshot_observation> returned_snapshot;
		std::optional<std::string> returned_export_digest;
		bounded_store_v6_reopen_observation reopen;

		[[nodiscard]] bool operator==(const bounded_store_v6_terminal_observation&) const = default;
	};

	struct bounded_store_v6_cleanup_observation
	{
		bool attempted{};
		bool drained{};
		bool report_release_attempted{};
		bool report_released{};
		bool backend_cleanup_attempted{};
		bool backend_cleanup_drained{};
		std::optional<error> report_failure;
		std::optional<error> backend_failure;
		std::optional<error> failure;

		[[nodiscard]] bool operator==(const bounded_store_v6_cleanup_observation&) const = default;
	};

	class bounded_store_terminal_custody final
	{
	  public:
		bounded_store_terminal_custody(const bounded_store_terminal_custody&) = delete;
		bounded_store_terminal_custody& operator=(const bounded_store_terminal_custody&) = delete;
		bounded_store_terminal_custody(bounded_store_terminal_custody&&) noexcept;
		bounded_store_terminal_custody& operator=(bounded_store_terminal_custody&&) noexcept;
		~bounded_store_terminal_custody();

		[[nodiscard]] const bounded_store_v6_terminal_observation& observation() const noexcept;
		[[nodiscard]] const bounded_store_report_tail_custody_observation&
		report_custody_observation() const noexcept;
		[[nodiscard]] const bounded_store_v6_cleanup_observation&
		cleanup_observation() const noexcept;
		[[nodiscard]] bool cleanup_drained() const noexcept;

	  private:
		struct state;
		explicit bounded_store_terminal_custody(std::unique_ptr<state>);
		std::unique_ptr<state> state_;
		friend class bounded_store_v6_phase_core;
	};

	struct bounded_store_full_report_observation
	{
		std::string staging_session_id;
		std::string candidate_id;
		bounded_store_v6_snapshot_observation candidate_snapshot;
		bounded_store_v6_expected_head expected_head;
		std::string physical_anchor_binding;
		std::string writer_object_binding;
		std::string spool_object_binding;
		std::string reservation_binding;
		std::string sealed_report_binding;
		std::uint64_t report_bytes{};
		std::uint32_t section_count{};
		bounded_store_v6_publication_terminal terminal{
			bounded_store_v6_publication_terminal::not_attempted};
		bool full_schema_validated{};
		bool complete_section_census_validated{};
		bool bottom_up_cross_bindings_validated{};

		[[nodiscard]] bool operator==(const bounded_store_full_report_observation&) const = default;
	};

	class bounded_store_full_report_release final
	{
	  public:
		bounded_store_full_report_release(const bounded_store_full_report_release&) = delete;
		bounded_store_full_report_release&
		operator=(const bounded_store_full_report_release&) = delete;
		bounded_store_full_report_release(bounded_store_full_report_release&&) noexcept;
		bounded_store_full_report_release& operator=(bounded_store_full_report_release&&) noexcept;
		~bounded_store_full_report_release();

		[[nodiscard]] const bounded_store_full_report_observation& observation() const noexcept;

	  private:
		struct state;
		explicit bounded_store_full_report_release(std::unique_ptr<state>);
		std::unique_ptr<state> state_;
		friend class bounded_store_v6_phase_core;
	};

	/** Closed classifier used by the terminal phase; normal errors cannot be guessed from prose. */
	enum class bounded_store_v6_error_class : std::uint8_t
	{
		invariant_breach,
		stale_parent,
		sqlite_failure,
		corrupt_store,
		resource_limit,
	};

	[[nodiscard]] bounded_store_v6_error_class
	classify_bounded_store_v6_error(bounded_store_v6_backend backend,
									std::string_view exact_series_id,
									std::string_view exact_candidate_snapshot_id,
									std::string_view exact_expected_publication_id,
									const error& failure) noexcept;

	/** Pure verifier used by every backend after a fresh factory/open traversal. */
	[[nodiscard]] result<void> validate_bounded_store_v6_reopen_observation(
		const bounded_store_v6_expected_head& expected_head,
		const bounded_store_v6_publication_observation& committed_publication,
		const bounded_store_v6_snapshot_observation& candidate_snapshot,
		const bounded_store_v6_reopen_observation& observed);

	/** Sole issuer of all phase tokens. */
	class bounded_store_v6_phase_core final
	{
	  public:
		bounded_store_v6_phase_core() = delete;

		[[nodiscard]] static result<bounded_store_v6_staging_session>
		begin_staging_session(bounded_store_v6_session_metadata metadata);
		[[nodiscard]] static result<bounded_store_sealed_input_binding>
		seal_input(bounded_store_v6_staging_session session,
				   bounded_store_v6_external_census census);
		[[nodiscard]] static result<bounded_store_v6_sealed_task>
		seal_task_source(bounded_store_v6_task_receipt receipt,
						 std::unique_ptr<bounded_store_v6_task_frame_source> source);
		[[nodiscard]] static result<bounded_store_v6_expected_projection> seal_expected_projection(
			std::unique_ptr<bounded_store_v6_expected_semantic_cursor> expected,
			const bounded_store_prepared_publication& prepared);
		[[nodiscard]] static result<bounded_store_prepared_publication>
		prepare_publication(bounded_store_sealed_input_binding input,
							std::unique_ptr<bounded_store_v6_backend_port> backend);
		[[nodiscard]] static result<void>
		stage_from_source(bounded_store_prepared_publication& prepared,
						  bounded_store_v6_sealed_task task);
		[[nodiscard]] static result<void>
		seal_prepared_publication(bounded_store_prepared_publication& prepared);
		[[nodiscard]] static result<bounded_store_authenticated_actual_cursor>
		take_physical_actual_cursor(bounded_store_prepared_publication& prepared);
		[[nodiscard]] static result<bounded_store_projection_match>
		compare_bounded_store_projections(bounded_store_prepared_publication& prepared,
										  bounded_store_v6_expected_projection expected,
										  bounded_store_authenticated_actual_cursor actual);
		[[nodiscard]] static result<bounded_store_report_tail_custody>
		reserve_report_tail(bounded_store_prepared_publication& prepared,
							bounded_store_projection_match& match,
							std::unique_ptr<bounded_store_report_tail_writer> writer);
		[[nodiscard]] static result<bounded_store_validated_publication>
		bind_publication(bounded_store_prepared_publication prepared,
						 bounded_store_projection_match match,
						 bounded_store_report_tail_custody report_custody);
		[[nodiscard]] static result<bounded_store_terminal_custody>
		publish_once(bounded_store_validated_publication publication);
		[[nodiscard]] static result<bounded_store_terminal_custody>
		capture_not_attempted(bounded_store_prepared_publication prepared,
							  bounded_store_report_tail_custody report_custody);
		[[nodiscard]] static result<bounded_store_full_report_release>
		finalize_and_validate_report(bounded_store_terminal_custody& terminal);
		[[nodiscard]] static result<bounded_store_v6_cleanup_observation>
		drain(bounded_store_terminal_custody& terminal, bounded_store_full_report_release release);
	};

	/** Checked product bounds; all limits are fixed product constants. */
	[[nodiscard]] result<void> validate_bounded_store_v6_task_count(std::uint64_t task_count);
	[[nodiscard]] result<std::uint64_t>
	checked_bounded_store_v6_aggregate_charge(std::uint64_t current_bytes,
											  std::uint64_t next_bytes);
	[[nodiscard]] result<std::uint64_t>
	checked_bounded_store_v6_record_frame_bytes(std::uint64_t key_bytes,
												std::uint64_t payload_bytes);
	[[nodiscard]] result<void> validate_bounded_store_v6_product_constants();
} // namespace cxxlens::sdk::detail
