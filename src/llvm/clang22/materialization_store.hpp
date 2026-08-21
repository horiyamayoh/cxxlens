#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "materialization_request.hpp"
#include "materialization_request_binding.hpp"
#include "materialization_io.hpp"

namespace cxxlens::detail::clang22::materialization
{
	class materialization_claim_stream_source;
	struct materialization_incremental_execution_journal_receipt;

	/**
	 * Source-private external completeness authority for the production streaming Store ingress.
	 *
	 * The typed partition replay source remains the Store input. This separate authority binds the
	 * Store boundary to the sealed execution journal and the independently validated event-stream
	 * census before a candidate can be opened or published. Neither pointer is retained after the
	 * prepublication preparation call.
	 */
	struct materialization_store_external_authority
	{
		const materialization_claim_stream_source* claim_stream{};
		const materialization_incremental_execution_journal_receipt* execution_journal{};
		std::optional<materialization_claim_request_binding> expected_request_binding;
	};

	/**
	 * Explicit resident/spool limits for one canonical Store projection stream.
	 *
	 * The limits belong to the source-private projection port rather than the SDK Store API.  A
	 * caller must select them before opening a stream; the writer never silently grows an
	 * unbounded aggregate buffer.
	 */
	struct materialization_store_projection_limits
	{
		std::uint64_t maximum_framed_bytes{512U * 1024U * 1024U};
		std::uint64_t maximum_record_bytes{1U * 1024U * 1024U};

		[[nodiscard]] bool operator==(const materialization_store_projection_limits&) const = default;
	};

	/** Receipt for one sealed canonical projection stream. */
	struct materialization_store_projection_receipt
	{
		std::uint64_t record_count{};
		std::uint64_t payload_bytes{};
		std::uint64_t framed_bytes{};
		std::string content_digest;

		[[nodiscard]] bool operator==(const materialization_store_projection_receipt&) const = default;
	};

	struct materialization_store_projection_comparison;

	/**
	 * Move-only bounded record writer backed by a private replayable spool.
	 *
	 * Records are framed as an unsigned big-endian u64 length followed by the exact record bytes.
	 * The framing is intentionally private: it is a transport for the actual and expected cursor
	 * ports and is not a semantic Store serialization.  A successful seal fixes the receipt and
	 * makes every subsequent append fail.
	 */
	class materialization_store_projection_writer final
	{
	  public:
		materialization_store_projection_writer(const materialization_store_projection_writer&) = delete;
		materialization_store_projection_writer&
		operator=(const materialization_store_projection_writer&) = delete;
		materialization_store_projection_writer(materialization_store_projection_writer&&) noexcept =
			default;
		materialization_store_projection_writer&
		operator=(materialization_store_projection_writer&&) noexcept = default;
		~materialization_store_projection_writer() = default;

		[[nodiscard]] static sdk::result<materialization_store_projection_writer>
		create(materialization_store_projection_limits limits = {});

		[[nodiscard]] sdk::result<void> append(std::span<const std::byte> record);
		[[nodiscard]] sdk::result<materialization_store_projection_receipt> seal();
		[[nodiscard]] bool sealed() const noexcept
		{
			return sealed_ && storage_ && storage_->sealed();
		}
		[[nodiscard]] const materialization_store_projection_receipt& receipt() const noexcept
		{
			return receipt_;
		}

	  private:
		materialization_store_projection_writer(
			std::unique_ptr<materialization_replayable_spool> storage,
			materialization_store_projection_limits limits) noexcept
			: storage_{std::move(storage)}, limits_{limits}
		{
		}

		std::unique_ptr<materialization_replayable_spool> storage_;
		materialization_store_projection_limits limits_;
		materialization_store_projection_receipt receipt_;
		std::uint64_t payload_bytes_{};
		std::uint64_t framed_bytes_{};
		std::uint64_t record_count_{};
		bool sealed_{};
		bool failed_{};

		/** The comparator is the only owner of the sealed cursor read capability. */
		[[nodiscard]] materialization_replayable_spool& storage() noexcept
		{
			return *storage_;
		}

		friend sdk::result<materialization_store_projection_comparison>
		compare_materialization_store_projections(
			materialization_store_projection_writer& actual,
			materialization_store_projection_writer& expected);
	};

	/** Exact comparison result for two independently produced projection cursors. */
	struct materialization_store_projection_comparison
	{
		bool equal{};
		std::uint64_t actual_record_count{};
		std::uint64_t expected_record_count{};
		std::uint64_t actual_payload_bytes{};
		std::uint64_t expected_payload_bytes{};
		std::optional<std::uint64_t> first_mismatch_offset;

		[[nodiscard]] bool operator==(const materialization_store_projection_comparison&) const =
			default;
	};

	/**
	 * Compare independently sealed actual and expected streams record-by-record.
	 *
	 * The comparator retains only one fixed-size read window from each stream.  It does not sort,
	 * hash-shortcut, or materialize either projection; a length mismatch, missing/extra record,
	 * byte mismatch, or divergent EOF produces `equal == false` with a bounded witness.
	 */
	[[nodiscard]] sdk::result<materialization_store_projection_comparison>
	compare_materialization_store_projections(
		materialization_store_projection_writer& actual,
		materialization_store_projection_writer& expected);

	/** Fully prepared SDK transaction input. This boundary only consumes Store-ready values. */
	struct prepared_store_transaction
	{
		sdk::snapshot_draft draft;
		std::vector<sdk::partition_draft> partitions;
		std::vector<sdk::closure_candidate> closures;
	};

	/**
	 * One source-private replay callback used by the streaming Store adapter.
	 *
	 * A source must produce a fresh, byte-equivalent partition sequence on every replay and must
	 * propagate a consumer error without continuing. The callback owns the partition after it
	 * returns, so an adapter never retains the source's complete partition vector.
	 */
	using materialization_store_partition_consumer =
		std::function<sdk::result<void>(sdk::partition_draft&&)>;

	/**
	 * Replayable source-private partition boundary for Store preparation.
	 *
	 * The first replay derives the exact manifest/index and the second replay stages one moved
	 * partition at a time. Implementations are expected to be backed by a canonical spool or an
	 * equivalent replayable source; a one-shot source is rejected by its own replay contract.
	 */
	class materialization_store_partition_replay_source
	{
	  public:
		virtual ~materialization_store_partition_replay_source() = default;
		[[nodiscard]] virtual sdk::result<void>
		replay(const materialization_store_partition_consumer& consumer) = 0;
	};

	/**
	 * Store metadata for the source-private streaming adapter. Unlike prepared_store_transaction,
	 * this value has no resident partition vector; the replay source supplies each draft.
	 */
	struct streaming_prepared_store_transaction
	{
		sdk::snapshot_draft draft;
		std::vector<sdk::closure_candidate> closures;
		materialization_store_external_authority external_authority;
	};

	/** Validate the external journal/task census before Store candidate preparation. */
	[[nodiscard]] sdk::result<void> validate_materialization_store_external_authority(
		const materialization_store_external_authority& authority);

	/** Exact operation ordering retained without mapping SDK failures to report outcomes. */
	enum class materialization_store_operation : std::uint8_t
	{
		configuration,
		store_open,
		head_current,
		writer_begin,
		partition_stage,
		closure_stage,
		writer_validate,
		writer_publish,
		store_reopen,
		verify_current,
		verify_open_publication,
		verify_open_snapshot,
		verify_projection,
	};

	enum class materialization_store_path : std::uint8_t
	{
		current_selector,
		open_publication,
		open_snapshot,
	};

	enum class materialization_store_receipt_status : std::uint8_t
	{
		not_attempted,
		present,
		sdk_error,
	};

	/** Typed projection copied directly from one SDK handle. */
	struct materialization_store_projection
	{
		sdk::publication_record publication;
		sdk::snapshot_manifest manifest;
		std::string physical_backend;

		[[nodiscard]] bool operator==(const materialization_store_projection&) const = default;
	};

	/** Publication identity fields constructible before the SDK returns a committed record. */
	struct materialization_publication_candidate
	{
		std::string publication_id;
		std::string series_id;
		std::string snapshot_id;
		std::uint64_t sequence{};
		std::optional<std::string> parent_publication;

		[[nodiscard]] bool operator==(const materialization_publication_candidate&) const = default;
	};

	/** One exact lookup receipt. Successful handles remain available for later private projection.
	 */
	struct materialization_store_path_receipt
	{
		materialization_store_path path{materialization_store_path::current_selector};
		materialization_store_receipt_status status{
			materialization_store_receipt_status::not_attempted};
		std::optional<sdk::snapshot_series_selector> selector_lookup;
		std::optional<std::string> id_lookup;
		std::optional<materialization_store_projection> projection;
		std::optional<sdk::snapshot_handle> handle;
		std::optional<sdk::error> error;
	};

	struct materialization_store_sdk_failure
	{
		materialization_store_operation operation{materialization_store_operation::store_open};
		std::optional<materialization_store_path> path;
		sdk::error error;

		[[nodiscard]] bool operator==(const materialization_store_sdk_failure&) const = default;
	};

	using materialization_store_mismatch_value = std::variant<std::string,
															  bool,
															  std::uint64_t,
															  std::optional<std::string>,
															  sdk::snapshot_series_selector,
															  sdk::publication_record,
															  sdk::snapshot_manifest>;

	/** Successful SDK calls that disagree are retained as values, never as fabricated errors. */
	struct materialization_store_mismatch
	{
		materialization_store_operation operation{materialization_store_operation::configuration};
		std::optional<materialization_store_path> path;
		std::string projection;
		materialization_store_mismatch_value expected;
		materialization_store_mismatch_value actual;

		[[nodiscard]] bool operator==(const materialization_store_mismatch&) const = default;
	};

	using materialization_store_issue =
		std::variant<materialization_store_sdk_failure, materialization_store_mismatch>;

	enum class materialization_store_reopen_status : std::uint8_t
	{
		not_attempted,
		opened,
		open_failed,
	};

	enum class materialization_store_lookup_status : std::uint8_t
	{
		not_attempted,
		present,
		not_found,
		not_applicable,
		sdk_error,
	};

	/** One exact recovery lookup. Not-found remains distinct from all other SDK failures. */
	struct materialization_store_publication_lookup
	{
		materialization_store_lookup_status status{
			materialization_store_lookup_status::not_attempted};
		std::optional<std::string> requested_publication_id;
		std::optional<sdk::publication_record> record;
		std::optional<sdk::error> error;
	};

	/** SQLite close/reopen observation after an attempted publish that returned an SDK error. */
	struct materialization_store_recovery_receipt
	{
		sdk::snapshot_series_selector selector;
		materialization_store_reopen_status reopen_status{
			materialization_store_reopen_status::not_attempted};
		std::optional<sdk::error> open_error;
		materialization_store_publication_lookup current;
		materialization_store_publication_lookup expected_parent;
		materialization_store_publication_lookup candidate;
	};

	/**
	 * Actual-value source on both sides of the irreversible publish boundary.
	 *
	 * This value deliberately has no stale/store/unknown/committed-unverified classification and
	 * contains no report or stdout bytes. A later two-phase report layer classifies only from the
	 * exact SDK record, receipts, and first issue retained here.
	 */
	struct materialization_store_observation
	{
		std::string backend;
		sdk::snapshot_series_selector selector;
		std::string series_id;
		std::optional<std::string> expected_parent_publication;
		materialization_store_path_receipt head_observation;
		std::uint32_t writer_begin_call_count{};
		bool publication_attempted{};
		std::uint32_t publish_call_count{};
		std::optional<sdk::snapshot_manifest> candidate_manifest;
		std::optional<materialization_publication_candidate> candidate_identity;
		std::optional<sdk::snapshot_handle> publish_returned_handle;
		std::optional<sdk::publication_record> publish_returned_record;
		std::optional<materialization_store_recovery_receipt> recovery_receipt;
		std::array<materialization_store_path_receipt, 3U> verification_receipts;
		std::optional<sdk::snapshot_store> verification_store;
		std::optional<materialization_store_issue> first_issue;
	};

	/** Private filesystem/backend port used to enforce close/reopen and inject typed failures. */
	class materialization_store_opener
	{
	  public:
		virtual ~materialization_store_opener() = default;
		[[nodiscard]] virtual sdk::result<sdk::snapshot_store>
		open_memory(sdk::relation_engine engine) = 0;
		[[nodiscard]] virtual sdk::result<sdk::snapshot_store>
		open_sqlite(const std::string& exact_path, sdk::relation_engine engine) = 0;
	};

	/**
	 * Move-only prepublication Store state after `stage all -> validate` and before `publish()`.
	 *
	 * The observation is always available. `ready_for_publish()` is false when preparation ended
	 * with a typed prepublication issue. A report layer may construct and validate its bounded
	 * publication-independent projection while a ready value keeps the unpublished writer alive.
	 */
	class materialization_store_preparation
	{
	  public:
		materialization_store_preparation(const materialization_store_preparation&) = delete;
		materialization_store_preparation&
		operator=(const materialization_store_preparation&) = delete;
		materialization_store_preparation(materialization_store_preparation&&) noexcept;
		materialization_store_preparation& operator=(materialization_store_preparation&&) noexcept;
		~materialization_store_preparation();

		[[nodiscard]] bool ready_for_publish() const noexcept;
		[[nodiscard]] const materialization_store_observation& observation() const noexcept;

	  private:
		struct state;
		explicit materialization_store_preparation(std::unique_ptr<state> state);
		std::unique_ptr<state> state_;

		friend materialization_store_preparation
		prepare_materialization_store(const sdk::relation_engine& engine,
									  const validated_publication_request& publication,
									  prepared_store_transaction prepared);
		friend materialization_store_preparation
		prepare_materialization_store(const sdk::relation_engine& engine,
									  const validated_publication_request& publication,
									  prepared_store_transaction prepared,
									  materialization_store_opener& opener);
		friend materialization_store_preparation prepare_materialization_store_streaming(
			const sdk::relation_engine& engine,
			const validated_publication_request& publication,
			streaming_prepared_store_transaction prepared,
			materialization_store_partition_replay_source& source,
			materialization_store_opener& opener);
		friend materialization_store_preparation prepare_materialization_store_streaming(
			const sdk::relation_engine& engine,
			const validated_publication_request& publication,
			streaming_prepared_store_transaction prepared,
			materialization_store_partition_replay_source& source);
		friend materialization_store_observation
		publish_materialization_store(materialization_store_preparation&& prepared);
	};

	/** Prepare and independently validate one unpublished Store transaction. */
	[[nodiscard]] materialization_store_preparation
	prepare_materialization_store(const sdk::relation_engine& engine,
								  const validated_publication_request& publication,
								  prepared_store_transaction prepared);

	/** Same prepublication boundary with an injected long-lived opener. */
	[[nodiscard]] materialization_store_preparation
	prepare_materialization_store(const sdk::relation_engine& engine,
								  const validated_publication_request& publication,
								  prepared_store_transaction prepared,
								  materialization_store_opener& opener);

	/** Prepare one replayable source without retaining all partition drafts in the transaction. */
	[[nodiscard]] materialization_store_preparation
	prepare_materialization_store_streaming(const sdk::relation_engine& engine,
											const validated_publication_request& publication,
											streaming_prepared_store_transaction prepared,
											materialization_store_partition_replay_source& source);

	/** Same streaming boundary with an injected private opener for deterministic failure tests. */
	[[nodiscard]] materialization_store_preparation
	prepare_materialization_store_streaming(const sdk::relation_engine& engine,
											const validated_publication_request& publication,
											streaming_prepared_store_transaction prepared,
											materialization_store_partition_replay_source& source,
											materialization_store_opener& opener);

	/** Cross the irreversible boundary exactly once, then retain success verification or recovery.
	 */
	[[nodiscard]] materialization_store_observation
	publish_materialization_store(materialization_store_preparation&& prepared);

	/** Execute one prepared Store transaction and its fixed-order postcommit verification. */
	[[nodiscard]] materialization_store_observation
	execute_materialization_store(const sdk::relation_engine& engine,
								  const validated_publication_request& publication,
								  prepared_store_transaction prepared);

	/** Same boundary with an injected private opener for deterministic failure verification. */
	[[nodiscard]] materialization_store_observation
	execute_materialization_store(const sdk::relation_engine& engine,
								  const validated_publication_request& publication,
								  prepared_store_transaction prepared,
								  materialization_store_opener& opener);

	/** Execute the source-private replayable Store adapter with the same observation contract. */
	[[nodiscard]] materialization_store_observation
	execute_materialization_store_streaming(const sdk::relation_engine& engine,
											const validated_publication_request& publication,
											streaming_prepared_store_transaction prepared,
											materialization_store_partition_replay_source& source);

	/** Same streaming execution boundary with an injected private opener. */
	[[nodiscard]] materialization_store_observation
	execute_materialization_store_streaming(const sdk::relation_engine& engine,
											const validated_publication_request& publication,
											streaming_prepared_store_transaction prepared,
											materialization_store_partition_replay_source& source,
											materialization_store_opener& opener);
} // namespace cxxlens::detail::clang22::materialization
