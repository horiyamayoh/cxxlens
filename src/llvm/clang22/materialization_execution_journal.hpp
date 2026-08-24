#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_io.hpp"
#include "materialization_store.hpp"
#include "materialization_v4_incremental_ingress.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/** Exact request identity available only after complete request validation. */
	struct compact_request_binding
	{
		std::string materialization_request_id;
		std::string request_digest;
		std::string semantic_request_digest;

		[[nodiscard]] bool operator==(const compact_request_binding&) const = default;
	};

	/** Stable report error whose phase is supplied only by the execution journal. */
	struct compact_report_error
	{
		std::string code;
		std::string subject;
		std::string diagnostic;
	};

	enum class compact_failure_phase : std::uint8_t
	{
		input_limit,
		json_decode,
		request_envelope,
		request_version,
		request_schema,
		request_binding,
		installation_binding,
		worker_launch,
		transcript,
		materialization_validation,
		store_open,
		store_stage,
		report_construction,
	};

	enum class compact_store_draft_state : std::uint8_t
	{
		not_created,
		discarded,
	};

	enum class compact_head_observation : std::uint8_t
	{
		not_observed,
		absent,
		present,
		sdk_error,
	};

	/** Bounded, prose-independent observation of the first prepublication SDK error. */
	struct compact_store_failure_observation
	{
		materialization_store_operation operation{materialization_store_operation::store_open};
		std::optional<materialization_store_path> path;
		std::string code;
		std::string field;
		std::string detail;
		std::uint64_t detail_byte_count{};
		std::string detail_digest;

		[[nodiscard]] bool operator==(const compact_store_failure_observation&) const = default;
	};

	class materialization_execution_journal;

	/** Phase in which a response became unsafe after the Store publish boundary. */
	enum class materialization_postpublication_failure_phase : std::uint8_t
	{
		store_persistence,
		report_construction,
		report_validation,
		response_spool,
		stdout_transport,
	};

	/** Recovery authority retained when no post-publication response is authoritative. */
	enum class materialization_postpublication_recovery_authority : std::uint8_t
	{
		committed_record_only,
		read_only_recovery_required,
	};

	/**
	 * Non-forgeable authority for exactly one compact prepublication failure.
	 *
	 * Only a consumed execution journal can construct this move-only token. Its accessors expose
	 * observations for serialization, never setters or a caller-constructible effect ledger.
	 */
	class compact_failure_authority
	{
	  public:
		compact_failure_authority(const compact_failure_authority&) = delete;
		compact_failure_authority& operator=(const compact_failure_authority&) = delete;
		compact_failure_authority(compact_failure_authority&&) noexcept;
		compact_failure_authority& operator=(compact_failure_authority&&) noexcept;
		~compact_failure_authority();

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] const raw_input_observation& raw_input() const noexcept;
		[[nodiscard]] const std::optional<compact_request_binding>&
		request_binding() const noexcept;
		[[nodiscard]] compact_failure_phase phase() const noexcept;
		[[nodiscard]] const compact_report_error& error() const noexcept;
		[[nodiscard]] std::uint64_t task_attempt_count() const noexcept;
		[[nodiscard]] std::uint64_t task_success_count() const noexcept;
		[[nodiscard]] std::uint64_t worker_launch_attempt_count() const noexcept;
		[[nodiscard]] std::uint64_t worker_launch_success_count() const noexcept;
		[[nodiscard]] compact_store_draft_state store_draft_state() const noexcept;
		[[nodiscard]] compact_head_observation head_observation() const noexcept;
		[[nodiscard]] const std::optional<std::string>& observed_head_publication() const noexcept;
		[[nodiscard]] const std::optional<compact_store_failure_observation>&
		store_failure_cause() const noexcept;

	  private:
		struct state;
		explicit compact_failure_authority(std::unique_ptr<state> state) noexcept;
		std::unique_ptr<state> state_;

		friend class materialization_execution_journal;
	};

	/**
	 * State after the irreversible Store publication boundary has been crossed.
	 *
	 * This type intentionally has no compact-failure-token operation.
	 */
	class materialization_postpublication_journal
	{
	  public:
		materialization_postpublication_journal(const materialization_postpublication_journal&) =
			delete;
		materialization_postpublication_journal&
		operator=(const materialization_postpublication_journal&) = delete;
		materialization_postpublication_journal(materialization_postpublication_journal&&) noexcept;
		materialization_postpublication_journal&
		operator=(materialization_postpublication_journal&&) noexcept;
		~materialization_postpublication_journal();

		[[nodiscard]] const materialization_store_observation& store_observation() const noexcept;

		/**
		 * Consume the post-publication journal as an exit-2/no-response outcome.
		 *
		 * This operation has no compact-failure counterpart: once publish() was attempted, a
		 * zero-effect response cannot be authored. The returned token retains the exact Store
		 * observation so callers cannot accidentally claim zero commit or retry blindly. Successful
		 * authority issuance consumes this journal; repeated rvalue issuance is rejected.
		 */
		[[nodiscard]] sdk::result<class materialization_postpublication_failure_authority>
		issue_no_response_failure(materialization_postpublication_failure_phase phase,
								  sdk::error error) &&;

	  private:
		explicit materialization_postpublication_journal(
			materialization_store_observation observation);
		materialization_store_observation observation_;
		bool consumed_{};

		friend class materialization_execution_journal;
	};

	/**
	 * Non-forgeable authority for a post-publication failure.
	 *
	 * The token carries no response bytes and deliberately exposes exit 2, non-authoritative
	 * stdout, and the only valid recovery route. It is source-private evidence, not a failure JSON
	 * response and not a permission to downgrade to the compact zero-effect branch.
	 */
	class materialization_postpublication_failure_authority
	{
	  public:
		materialization_postpublication_failure_authority(
			const materialization_postpublication_failure_authority&) = delete;
		materialization_postpublication_failure_authority&
		operator=(const materialization_postpublication_failure_authority&) = delete;
		materialization_postpublication_failure_authority(
			materialization_postpublication_failure_authority&&) noexcept;
		materialization_postpublication_failure_authority&
		operator=(materialization_postpublication_failure_authority&&) noexcept;
		~materialization_postpublication_failure_authority();

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] materialization_postpublication_failure_phase phase() const noexcept;
		[[nodiscard]] const sdk::error& error() const noexcept;
		[[nodiscard]] const materialization_store_observation& store_observation() const noexcept;
		[[nodiscard]] materialization_postpublication_recovery_authority
		recovery_authority() const noexcept;
		[[nodiscard]] constexpr int process_exit_status() const noexcept
		{
			return 2;
		}
		[[nodiscard]] constexpr bool response_authoritative() const noexcept
		{
			return false;
		}
		[[nodiscard]] constexpr bool compact_downgrade_allowed() const noexcept
		{
			return false;
		}

	  private:
		struct state;
		explicit materialization_postpublication_failure_authority(
			std::unique_ptr<state> state) noexcept;
		std::unique_ptr<state> state_;

		friend class materialization_postpublication_journal;
	};

	/**
	 * Move-only, source-private execution journal for every compact-failure-capable phase.
	 *
	 * Named transitions authenticate phase order. Worker and Store effects are observed here and
	 * cannot be supplied to the report encoder. Consuming `begin_publication()` returns a distinct
	 * postpublication type that cannot issue compact authority.
	 */
	class materialization_execution_journal
	{
	  public:
		materialization_execution_journal(const materialization_execution_journal&) = delete;
		materialization_execution_journal&
		operator=(const materialization_execution_journal&) = delete;
		materialization_execution_journal(materialization_execution_journal&&) noexcept;
		materialization_execution_journal& operator=(materialization_execution_journal&&) noexcept;
		~materialization_execution_journal();

		[[nodiscard]] static sdk::result<materialization_execution_journal>
		begin(raw_input_observation input);

		[[nodiscard]] sdk::result<void> pass_input_limit();
		[[nodiscard]] sdk::result<void> pass_json_decode();
		[[nodiscard]] sdk::result<void> pass_request_envelope();
		[[nodiscard]] sdk::result<void> pass_request_version();
		[[nodiscard]] sdk::result<void> pass_request_schema();
		[[nodiscard]] sdk::result<void> authenticate_request(compact_request_binding binding,
															 std::uint64_t actual_task_count);
		[[nodiscard]] sdk::result<void> complete_installation_binding();

		/** Record one authenticated task window, including exact-reuse tasks. */
		[[nodiscard]] sdk::result<void> record_task_attempt();
		[[nodiscard]] sdk::result<void> record_task_success();

		/** Record only an actual provider/worker frontend launch. */
		[[nodiscard]] sdk::result<void> record_worker_launch_attempt();
		[[nodiscard]] sdk::result<void> record_worker_launch_success();
		[[nodiscard]] sdk::result<void> complete_worker_launches();
		[[nodiscard]] sdk::result<void> complete_transcript_validation();
		[[nodiscard]] sdk::result<void> complete_materialization_validation();

		/** Consume the actual Store preparation and retain its exact first observation. */
		[[nodiscard]] sdk::result<void>
		record_store_preparation(materialization_store_preparation preparation);
		[[nodiscard]] sdk::result<void> complete_store_preparation();

		/** Consume the prepublication journal and discard any unpublished Store draft first. */
		[[nodiscard]] sdk::result<compact_failure_authority>
		issue_compact_failure(compact_report_error error) &&;

		/** Consume the only unpublished Store draft and cross exactly one publish boundary. */
		[[nodiscard]] sdk::result<materialization_postpublication_journal> begin_publication() &&;

	  private:
		struct state;
		explicit materialization_execution_journal(std::unique_ptr<state> state) noexcept;
		std::unique_ptr<state> state_;
	};

	/**
	 * One task-v4 execution census entry.
	 *
	 * The receipt is the complete source-closure/provider/partition identity.  A reuse entry is
	 * therefore never keyed by a task number alone.  `provider_call_count` is an observation of the
	 * current invocation: a prior-artifact reuse must explicitly carry zero calls.
	 */
	struct materialization_v4_task_execution
	{
		materialization_v4_claim_receipt receipt;
		bool reused{};
		std::uint64_t provider_call_count{};

		[[nodiscard]] bool operator==(const materialization_v4_task_execution&) const = default;
	};

	/** Immutable execution result consumed by the v4 materializer/store handoff. */
	struct materialization_v4_execution_receipt
	{
		inline static constexpr std::string_view schema =
			"cxxlens.clang22.materialization-execution-receipt.v4";
		std::string materialization_request_id;
		std::uint64_t task_count{};
		std::vector<materialization_v4_task_execution> tasks;
		std::uint64_t provider_call_count{};
		std::uint64_t reused_task_count{};
		std::string incremental_receipt_digest;
		std::string execution_digest;

		[[nodiscard]] bool operator==(const materialization_v4_execution_receipt&) const = default;
	};

	/**
	 * Source-private journal for the task-v4 execution/reuse decision.
	 *
	 * This is deliberately separate from the compact-failure journal above.  It has no request
	 * 2.1/task-v3 binding and it does not manufacture a result from counters: finalization compares
	 * every ordered task receipt and the recomputed incremental digest.  Consequently stale,
	 * tampered, and reordered prior artifacts fail before a provider/store effect is authorized.
	 */
	class materialization_v4_execution_journal
	{
	  public:
		materialization_v4_execution_journal(const materialization_v4_execution_journal&) = delete;
		materialization_v4_execution_journal&
		operator=(const materialization_v4_execution_journal&) = delete;
		materialization_v4_execution_journal(materialization_v4_execution_journal&&) noexcept;
		materialization_v4_execution_journal&
		operator=(materialization_v4_execution_journal&&) noexcept;
		~materialization_v4_execution_journal();

		[[nodiscard]] static sdk::result<materialization_v4_execution_journal>
		begin(std::string materialization_request_id, std::uint64_t task_count);

		/** Record exactly the next task index; reuse requires provider_call_count == 0. */
		[[nodiscard]] sdk::result<void> record(materialization_v4_claim_receipt receipt,
											   bool reused,
											   std::uint64_t provider_call_count);

		/**
		 * Consume the journal only when the ordered records reproduce the complete v4 receipt.
		 * `expected` is normally the receipt read from a prior artifact or the current sealed
		 * output.
		 */
		[[nodiscard]] sdk::result<materialization_v4_execution_receipt>
		finish(materialization_v4_incremental_receipt expected) &&;

		/** Compare a candidate prior receipt with current task-v4 identities, including order. */
		[[nodiscard]] static sdk::result<void>
		validate_exact_reuse(const materialization_v4_incremental_receipt& prior,
							 const materialization_v4_incremental_receipt& current);

	  private:
		struct state;
		explicit materialization_v4_execution_journal(std::unique_ptr<state> state) noexcept;
		std::unique_ptr<state> state_;
	};
} // namespace cxxlens::detail::clang22::materialization
