#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <cxxlens/sdk/common.hpp>

#include "materialization_io.hpp"
#include "materialization_store.hpp"

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

	  private:
		explicit materialization_postpublication_journal(
			materialization_store_observation observation);
		materialization_store_observation observation_;

		friend class materialization_execution_journal;
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
} // namespace cxxlens::detail::clang22::materialization
