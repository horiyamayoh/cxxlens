#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "materialization_request.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/** Fully prepared SDK transaction input. This boundary only consumes Store-ready values. */
	struct prepared_store_transaction
	{
		sdk::snapshot_draft draft;
		std::vector<sdk::partition_draft> partitions;
		std::vector<sdk::closure_candidate> closures;
	};

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
} // namespace cxxlens::detail::clang22::materialization
