#pragma once

/**
 * @file materialization_v4_coordinator.hpp
 * @brief Source-private request-v2.2/task-v4 materialization orchestration.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_report2_2_builder.hpp"
#include "materialization_request_v2_2.hpp"
#include "materialization_v4_claim_binding.hpp"
#include "materialization_v4_incremental_ingress.hpp"
#include "sdk/provider_validation_internal.hpp"
#include "source_closure_receiver.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/** Hard request-wide residency and count bounds enforced before Store preparation. */
	struct materialization_v4_coordinator_limits
	{
		std::uint64_t maximum_tasks{materialization_v4_incremental_max_tasks};
		std::uint64_t maximum_closures{materialization_v4_incremental_max_tasks};
		std::uint64_t maximum_unique_blob_bytes{48U * 1024U * 1024U};
		std::uint64_t maximum_retained_result_bytes{std::uint64_t{512U} * 1024U * 1024U};
		materialization_report2_2_limits report{};
	};

	/**
	 * Validated request authority after transport JSON has been destroyed.
	 *
	 * This projection deliberately has no `json_value` member. The inherited source-free authority
	 * is represented only by `provider_task_v4_request_authority` and its independently recomputed
	 * digest at the coordinator boundary.
	 */
	struct materialization_v4_validated_request
	{
		materialization_v4_validated_request() = default;
		materialization_v4_validated_request(const materialization_v4_validated_request&) = delete;
		materialization_v4_validated_request&
		operator=(const materialization_v4_validated_request&) = delete;
		materialization_v4_validated_request(materialization_v4_validated_request&&) noexcept =
			default;
		materialization_v4_validated_request&
		operator=(materialization_v4_validated_request&&) noexcept = default;

		std::string schema;
		std::string request_version;
		std::uint16_t protocol_major{};
		std::uint16_t protocol_minor{};
		std::string request_id;
		std::string request_digest;
		std::vector<std::string> required_features;
		std::string materialization_request_id;
		std::string semantic_request_digest;
		std::vector<provider_task_v4_base_task> base_tasks;
		std::vector<source_closure_summary> source_closures;
		std::vector<provider_task_v4> task_extensions;
		std::vector<std::string> negotiated_features;
		std::uint64_t unique_blob_bytes{};
	};

	/** Revalidate the decoder result once, move out typed fields, and destroy its JSON DOM. */
	[[nodiscard]] sdk::result<materialization_v4_validated_request>
	make_materialization_v4_validated_request(validated_materialization_request_v2_2&& request);

	/**
	 * One complete closure admitted by the installed receiver.
	 *
	 * The vector returned by the receiver port is in request `source_closures` order. The snapshot
	 * and ACK credentials remain owned by the coordinator until every dependent worker and the
	 * Store preparation have completed.
	 */
	struct materialization_v4_admitted_closure
	{
		std::size_t canonical_closure_index{};
		std::size_t authority_task_index{};
		source_closure_transfer_binding ingress_binding;
		std::string expected_transfer_digest;
		source_closure_receiver_result receiver;
		std::uint64_t retained_bytes{};
	};

	/** Installed request/closure adapter. It must not launch a worker or open Store. */
	class materialization_v4_closure_port
	{
	  public:
		virtual ~materialization_v4_closure_port() = default;
		[[nodiscard]] virtual sdk::result<std::vector<materialization_v4_admitted_closure>>
		receive_all(const materialization_v4_validated_request& request,
					std::stop_token cancellation) = 0;
	};

	/**
	 * Authenticated source identity adapter. It must recompute content and line-index identity from
	 * the admitted snapshot bytes, not accept request metadata alone.
	 */
	class materialization_v4_source_identity_port
	{
	  public:
		virtual ~materialization_v4_source_identity_port() = default;
		[[nodiscard]] virtual sdk::result<void>
		validate_main(const provider_task_v4_base_task& base,
					  const provider_task_v4& task,
					  const source_closure_snapshot& snapshot,
					  std::stop_token cancellation) = 0;
	};

	/** Immutable per-task launch view. Implementations must not retain any reference. */
	struct materialization_v4_task_launch
	{
		const materialization_v4_validated_request& request;
		const provider_task_v4_request_authority& request_authority;
		std::string_view request_authority_digest;
		std::size_t task_index{};
		const materialization_v4_admitted_closure& closure;
		const materialization_v4_coordinator_limits& limits;
		std::stop_token cancellation;
	};

	/** Recomputable binding for a sealed worker transcript. */
	struct materialization_v4_worker_result_binding
	{
		std::string request_authority_digest;
		std::uint64_t task_index{};
		std::string task_id;
		std::string task_v4_digest;
		std::string source_closure_id;
		std::string source_closure_digest;
		std::string manifest_digest;
		std::string closure_receipt_digest;
		std::string provider_id;
		sdk::semantic_version provider_version;
		std::string provider_binary_digest;
		std::string provider_semantic_contract_digest;
		std::string launch_input_digest;
		std::string normalized_invocation_digest;
		std::string toolchain_digest;
		std::string environment_digest;
		std::string output_plan_digest;
		std::string runtime_receipt_digest;
		std::string sealed_transcript_digest;
		std::uint64_t retained_bytes{};
	};

	/** Value-owned, runtime-sealed provider result. Raw frame bytes are not adoption authority. */
	struct materialization_v4_worker_result
	{
		materialization_v4_worker_result_binding binding;
		sdk::provider::detail::sealed_provider_transcript transcript;

		materialization_v4_worker_result(
			materialization_v4_worker_result_binding binding,
			sdk::provider::detail::sealed_provider_transcript transcript)
			: binding{std::move(binding)}, transcript{std::move(transcript)}
		{
		}

		materialization_v4_worker_result(const materialization_v4_worker_result&) = delete;
		materialization_v4_worker_result&
		operator=(const materialization_v4_worker_result&) = delete;
		materialization_v4_worker_result(materialization_v4_worker_result&&) noexcept = default;
		materialization_v4_worker_result&
		operator=(materialization_v4_worker_result&&) noexcept = default;
	};

	/** Process/relay adapter. Success must already include closure ACK and Protocol 2 validation.
	 */
	class materialization_v4_worker_port
	{
	  public:
		virtual ~materialization_v4_worker_port() = default;
		[[nodiscard]] virtual sdk::result<materialization_v4_worker_result>
		execute(const materialization_v4_task_launch& launch) = 0;
	};

	/** One host-produced base-row set and the independently sealed task claim. */
	struct materialization_v4_host_claim_result
	{
		materialization_v4_claim_sealed claim;
		std::vector<sdk::detached_row> base_rows;
		std::string base_row_set_digest;
		std::uint64_t retained_bytes{};

		materialization_v4_host_claim_result(materialization_v4_claim_sealed claim,
											 std::vector<sdk::detached_row> base_rows,
											 std::string base_row_set_digest,
											 std::uint64_t retained_bytes)
			: claim{std::move(claim)}, base_rows{std::move(base_rows)},
			  base_row_set_digest{std::move(base_row_set_digest)}, retained_bytes{retained_bytes}
		{
		}

		materialization_v4_host_claim_result(const materialization_v4_host_claim_result&) = delete;
		materialization_v4_host_claim_result&
		operator=(const materialization_v4_host_claim_result&) = delete;
		materialization_v4_host_claim_result(materialization_v4_host_claim_result&&) noexcept =
			default;
		materialization_v4_host_claim_result&
		operator=(materialization_v4_host_claim_result&&) noexcept = default;
	};

	/** Host claim/base-row adapter. It receives only an immutable sealed transcript. */
	class materialization_v4_host_claim_port
	{
	  public:
		virtual ~materialization_v4_host_claim_port() = default;
		[[nodiscard]] virtual sdk::result<materialization_v4_host_claim_result>
		translate(const materialization_v4_task_launch& launch,
				  const materialization_v4_worker_result_binding& worker,
				  const sdk::provider::detail::sealed_provider_transcript& transcript,
				  const sdk::relation_engine& engine) = 0;
	};

	/**
	 * Type-erased unpublished Store source/candidate owned by the Store adapter. Destruction before
	 * `publish_once` must abort staged backend state and drain journal/descriptor custody.
	 */
	class materialization_v4_store_prepared_state
	{
	  public:
		virtual ~materialization_v4_store_prepared_state() = default;
	};

	/** Exact zero-effect Store preparation projection. */
	struct materialization_v4_store_preparation_projection
	{
		std::string materialization_request_id;
		std::string task_receipt_digest;
		std::uint64_t task_count{};
		std::uint64_t source_bytes{};
		std::string store_source_digest;
		/** Receipt from replaying only sealed task claims and canonical host base rows. */
		std::string sealed_input_replay_digest;
		std::string expected_projection_digest;
		/** Receipt from an independent cursor over the backend's physically staged records. */
		std::string backend_staged_cursor_digest;
		std::string actual_projection_digest;
		std::string journal_digest;
	};

	/** Move-only unpublished source after full expected/actual and journal validation. */
	class materialization_v4_store_preparation
	{
	  public:
		materialization_v4_store_preparation(const materialization_v4_store_preparation&) = delete;
		materialization_v4_store_preparation&
		operator=(const materialization_v4_store_preparation&) = delete;
		materialization_v4_store_preparation(materialization_v4_store_preparation&&) noexcept =
			default;
		materialization_v4_store_preparation&
		operator=(materialization_v4_store_preparation&&) noexcept = default;
		~materialization_v4_store_preparation() = default;

		[[nodiscard]] const materialization_v4_store_preparation_projection&
		projection() const noexcept;
		[[nodiscard]] std::string_view preparation_digest() const noexcept;
		[[nodiscard]] std::unique_ptr<materialization_v4_store_prepared_state>
		take_state() && noexcept;

	  private:
		materialization_v4_store_preparation(
			std::unique_ptr<materialization_v4_store_prepared_state> state,
			materialization_v4_store_preparation_projection projection,
			std::string preparation_digest);

		std::unique_ptr<materialization_v4_store_prepared_state> state_;
		materialization_v4_store_preparation_projection projection_;
		std::string preparation_digest_;

		friend sdk::result<materialization_v4_store_preparation>
		make_materialization_v4_store_preparation(
			std::unique_ptr<materialization_v4_store_prepared_state>,
			materialization_v4_store_preparation_projection,
			const materialization_v4_coordinator_limits&);
	};

	/** Seal a zero-effect preparation only after expected and actual projections match. */
	[[nodiscard]] sdk::result<materialization_v4_store_preparation>
	make_materialization_v4_store_preparation(
		std::unique_ptr<materialization_v4_store_prepared_state> state,
		materialization_v4_store_preparation_projection projection,
		const materialization_v4_coordinator_limits& limits = {});

	/** Store terminal returned exactly once after consuming an unpublished preparation. */
	struct materialization_v4_store_publication_result
	{
		materialization_report2_2_store_terminal terminal{
			materialization_report2_2_store_terminal::not_attempted};
		std::string store_preparation_digest;
		std::string store_result_digest;
		std::optional<std::string> publication_id;
		std::uint32_t publish_call_count{};
	};

	/**
	 * Bounded Store source adapter. `prepare` is zero-effect and must construct expected only by
	 * replaying the sealed inputs, construct actual only through a cursor over the real staged
	 * backend, close and validate the journal, and then use
	 * `make_materialization_v4_store_preparation`. It may not derive both projections from one
	 * manifest or substitute a no-op backend. `publish_once` is one-shot.
	 */
	class materialization_v4_store_port
	{
	  public:
		virtual ~materialization_v4_store_port() = default;
		[[nodiscard]] virtual sdk::result<materialization_v4_store_preparation>
		prepare(const materialization_v4_validated_request& request,
				const provider_task_v4_request_authority& authority,
				std::string_view authority_digest,
				const sdk::relation_engine& engine,
				const materialization_v4_incremental_receipt& receipt,
				std::span<const materialization_v4_claim_sealed* const> sealed_tasks,
				std::span<const materialization_v4_host_claim_result* const> host_results,
				const materialization_v4_coordinator_limits& limits,
				std::stop_token cancellation) = 0;

		/**
		 * Cross the effect boundary exactly once. Every path, including cancellation, backend
		 * failure, and an ambiguous effect, must return a typed terminal; exceptions are forbidden.
		 */
		[[nodiscard]] virtual materialization_v4_store_publication_result
		publish_once(materialization_v4_store_preparation preparation,
					 std::stop_token cancellation) noexcept = 0;
	};

	/** Allocate an anonymous replayable spool for one final report. */
	class materialization_report2_2_storage_port
	{
	  public:
		virtual ~materialization_report2_2_storage_port() = default;
		[[nodiscard]] virtual sdk::result<std::unique_ptr<materialization_report2_2_reserved_spool>>
		create(materialization_report2_2_limits limits, std::stop_token cancellation) = 0;
	};

	/** Move-owned coordinator input. JSON transport is absent from this boundary. */
	struct materialization_v4_coordinator_input
	{
		materialization_v4_validated_request request;
		provider_task_v4_request_authority authority;
		std::string authority_digest;
		sdk::relation_engine engine;
		materialization_v4_coordinator_limits limits;
		std::stop_token cancellation;
	};

	struct materialization_v4_coordinator_ports
	{
		materialization_v4_closure_port& closures;
		materialization_v4_source_identity_port& source_identity;
		materialization_v4_worker_port& worker;
		materialization_v4_host_claim_port& claims;
		materialization_v4_store_port& store;
		materialization_report2_2_storage_port& report_storage;
		materialization_report2_2_projection_port& report_projection;
	};

	/** Successful report construction, including non-committed Store terminals. */
	struct materialization_v4_coordinator_completed
	{
		materialization_v4_incremental_receipt task_receipt;
		materialization_v4_store_publication_result store;
		sealed_materialization_report2_2 report;

		materialization_v4_coordinator_completed(
			materialization_v4_incremental_receipt task_receipt,
			materialization_v4_store_publication_result store,
			sealed_materialization_report2_2 report)
			: task_receipt{std::move(task_receipt)}, store{std::move(store)},
			  report{std::move(report)}
		{
		}
	};

	/** Publication crossed, but no authoritative report bytes could be sealed. */
	struct materialization_v4_coordinator_postpublication_failure
	{
		materialization_v4_store_publication_result store;
		sdk::error report_error;
	};

	using materialization_v4_coordinator_outcome =
		std::variant<materialization_v4_coordinator_completed,
					 materialization_v4_coordinator_postpublication_failure>;

	/**
	 * Run one request through closure, worker, claim, bounded Store preparation, and two-phase
	 * reporting. Any `sdk::unexpected` result is prepublication and therefore has zero Store
	 * publication effect; closure/worker cleanup remains owned by their ports.
	 */
	[[nodiscard]] sdk::result<materialization_v4_coordinator_outcome>
	run_materialization_v4_coordinator(materialization_v4_coordinator_input input,
									   materialization_v4_coordinator_ports ports);

	/** Digest canonical base rows in the exact order required by the coordinator. */
	[[nodiscard]] sdk::result<std::string>
	materialization_v4_base_row_set_digest(std::span<const sdk::detached_row> rows);
} // namespace cxxlens::detail::clang22::materialization
