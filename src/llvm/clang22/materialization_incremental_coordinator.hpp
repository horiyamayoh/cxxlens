#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/sdk/incremental.hpp>

#include "materialization_bounded_claim_source.hpp"
#include "materialization_claim_stream.hpp"
#include "materialization_claims.hpp"
#include "materialization_incremental_receipt.hpp"
#include "materialization_io.hpp"
#include "materialization_request_v2_1.hpp"
#include "materialization_store.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/** Canonical task identity that binds a planner partition to one request task. */
	struct materialization_incremental_task_identity
	{
		std::size_t canonical_task_ordinal{};
		std::string provider_task_id;
		std::string task_input_digest;
		std::string selected_catalog_compile_unit_id;
		std::string final_relation_compile_unit_id;

		[[nodiscard]] bool
		operator==(const materialization_incremental_task_identity&) const = default;
	};

	/** Exact planner state and artifact digest required before a sealed result may be reused. */
	struct materialization_incremental_prior_artifact
	{
		sdk::incremental::partition_state state;
		std::string sealed_artifact_digest;

		[[nodiscard]] bool
		operator==(const materialization_incremental_prior_artifact&) const = default;
	};

	/** One exact partition member of a task receipt; a task may own multiple partitions. */
	struct materialization_incremental_partition_binding
	{
		std::string partition_id;
		std::optional<sdk::incremental::partition_state> current_state;
		std::optional<materialization_incremental_prior_artifact> prior_artifact;

		materialization_incremental_partition_binding(
			std::string partition_id,
			std::optional<sdk::incremental::partition_state> current_state = std::nullopt,
			std::optional<materialization_incremental_prior_artifact> prior_artifact = std::nullopt)
			: partition_id{std::move(partition_id)}, current_state{std::move(current_state)},
			  prior_artifact{std::move(prior_artifact)}
		{
		}

		[[nodiscard]] bool
		operator==(const materialization_incremental_partition_binding&) const = default;
	};

	struct materialization_incremental_task_binding;

	/**
	 * Opaque source-private authority passed to the delayed event encoder.  The serialized task
	 * receipt remains schema-closed; these values are checked against the result and exact
	 * binding immediately before encoding and are never accepted from a replayed stream.
	 */
	struct materialization_incremental_pre_encoder_seal
	{
		materialization_incremental_task_receipt task_receipt;
		std::string result_artifact_digest;
		std::string task_partition_set_digest;
		std::vector<std::string> partition_ids;

		[[nodiscard]] bool
		operator==(const materialization_incremental_pre_encoder_seal&) const = default;
	};

	/**
	 * Delayed event encoder.  The coordinator invokes this only after the immutable task
	 * receipt has been independently recomputed and sealed.  Returning already-created spools
	 * from execute() would make that ordering unprovable.
	 */
	using materialization_incremental_partition_spool_encoder =
		std::function<sdk::result<std::vector<std::unique_ptr<materialization_replayable_spool>>>(
			const sealed_materialization_result&,
			const materialization_incremental_pre_encoder_seal&)>;

	/** Runtime-owned receipt; the coordinator never substitutes its own loop count for this. */
	struct materialization_incremental_provider_execution_receipt
	{
		std::uint64_t provider_call_count{};
		std::string provider_task_id;
		std::string provider_execution_id;
		std::string sealed_artifact_digest;
		/** Independently reported, canonical task-to-partition coverage. */
		std::vector<std::string> covered_partition_ids;
		std::string task_partition_set_digest;
		/** External D3 completeness authority sealed before event encoding or Store ingress. */
		std::optional<materialization_incremental_pre_encoder_seal> pre_encoder_seal;
	};

	/** One provider output together with the independent runtime execution receipt. */
	struct materialization_incremental_task_execution
	{
		sealed_materialization_result result;
		materialization_incremental_provider_execution_receipt receipt;
		/** Created only after the coordinator seals and validates the external task receipt. */
		materialization_incremental_partition_spool_encoder encode_partition_spools;

		materialization_incremental_task_execution(
			sealed_materialization_result result,
			materialization_incremental_provider_execution_receipt receipt,
			materialization_incremental_partition_spool_encoder encode_partition_spools)
			: result{std::move(result)}, receipt{std::move(receipt)},
			  encode_partition_spools{std::move(encode_partition_spools)}
		{
		}

		materialization_incremental_task_execution(
			const materialization_incremental_task_execution&) = delete;
		materialization_incremental_task_execution&
		operator=(const materialization_incremental_task_execution&) = delete;
		materialization_incremental_task_execution(
			materialization_incremental_task_execution&&) noexcept = default;
		materialization_incremental_task_execution&
		operator=(materialization_incremental_task_execution&&) noexcept = default;
	};

	/** Exact reuse receipt; a cache hit must independently report zero provider calls. */
	struct materialization_incremental_task_reuse
	{
		sealed_materialization_result result;
		materialization_incremental_provider_execution_receipt receipt;
		/** Created only after the coordinator seals and validates the external task receipt. */
		materialization_incremental_partition_spool_encoder encode_partition_spools;

		materialization_incremental_task_reuse(
			sealed_materialization_result result,
			materialization_incremental_provider_execution_receipt receipt,
			materialization_incremental_partition_spool_encoder encode_partition_spools)
			: result{std::move(result)}, receipt{std::move(receipt)},
			  encode_partition_spools{std::move(encode_partition_spools)}
		{
		}

		materialization_incremental_task_reuse(const materialization_incremental_task_reuse&) =
			delete;
		materialization_incremental_task_reuse&
		operator=(const materialization_incremental_task_reuse&) = delete;
		materialization_incremental_task_reuse(materialization_incremental_task_reuse&&) noexcept =
			default;
		materialization_incremental_task_reuse&
		operator=(materialization_incremental_task_reuse&&) noexcept = default;
	};

	/** Source-private execution seam; the coordinator owns the execution census around it. */
	class materialization_incremental_task_executor
	{
	  public:
		virtual ~materialization_incremental_task_executor() = default;

		[[nodiscard]] virtual sdk::result<materialization_incremental_task_execution>
		execute(std::size_t request_task_index,
				const validated_task_request& task,
				const materialization_incremental_task_binding& binding) = 0;

		/** Load one validated exact-reuse artifact without performing a frontend execution. */
		[[nodiscard]] virtual sdk::result<materialization_incremental_task_reuse>
		load_reusable(std::size_t request_task_index,
					  const validated_task_request& task,
					  const materialization_incremental_task_binding& binding) = 0;

		/** Cancellation is checked before each recompute and after the final task result. */
		[[nodiscard]] virtual bool cancellation_requested() const noexcept = 0;
	};

	/** Compute the complete source-private digest bound by a reuse or execution receipt. */
	[[nodiscard]] sdk::result<std::string>
	seal_materialization_incremental_artifact_digest(const sealed_materialization_result& result);

	/** Compute the exact ordered task-to-partition coverage digest carried by a runtime receipt. */
	[[nodiscard]] sdk::result<std::string>
	seal_materialization_incremental_task_partition_set_digest(
		std::span<const std::string> partition_ids);

	/** Explicit canonical task-to-partition binding; names alone never authorize a swap. */
	struct materialization_incremental_task_binding
	{
		materialization_incremental_task_identity task_identity;
		std::vector<materialization_incremental_partition_binding> partitions;

		materialization_incremental_task_binding(
			materialization_incremental_task_identity task_identity,
			std::vector<materialization_incremental_partition_binding> partitions)
			: task_identity{std::move(task_identity)}, partitions{std::move(partitions)}
		{
		}

		/** Compatibility constructor for the common one-partition task shape. */
		materialization_incremental_task_binding(
			std::string partition_id,
			materialization_incremental_task_identity task_identity,
			std::optional<sdk::incremental::partition_state> current_state = std::nullopt,
			std::optional<materialization_incremental_prior_artifact> prior_artifact = std::nullopt)
			: task_identity{std::move(task_identity)},
			  partitions{materialization_incremental_partition_binding{
				  std::move(partition_id), std::move(current_state), std::move(prior_artifact)}}
		{
		}

		materialization_incremental_task_binding(const materialization_incremental_task_binding&) =
			delete;
		materialization_incremental_task_binding&
		operator=(const materialization_incremental_task_binding&) = delete;
		materialization_incremental_task_binding(
			materialization_incremental_task_binding&&) noexcept = default;
		materialization_incremental_task_binding&
		operator=(materialization_incremental_task_binding&&) noexcept = default;
	};

	/**
	 * Source-private v2.1 execution seam. The consumer must finish the bounded task window before
	 * returning; the coordinator destroys that window before it advances the cursor. The callback
	 * owns the downstream result/spool handoff because the legacy claim/store result type cannot
	 * represent a v2.1 task without first materializing a request-wide legacy task vector.
	 */
	using materialization_v2_1_task_cursor_consumer =
		std::function<sdk::result<void>(std::size_t,
										sdk::incremental::action,
										materialization_v2_1_task_execution&,
										const materialization_incremental_task_binding&)>;

	/** Independent evidence of actual provider calls; planner counters are not substituted. */
	struct materialization_incremental_execution_census
	{
		/** Planned recompute partition count from the validated SDK plan. */
		std::uint64_t planned_provider_executions{};
		/** Planned frontend task-call count, distinct from partition coverage count. */
		std::uint64_t planned_provider_task_executions{};
		std::uint64_t actual_provider_executions{};
		/** Exact affected partition count independently covered by execution receipts. */
		std::uint64_t actual_recomputed_partition_count{};
		bool warm_zero{};
		std::vector<std::string> executed_partition_ids;
		std::vector<std::string> executed_provider_task_ids;
		std::vector<std::string> executed_provider_execution_ids;
		std::vector<std::string> executed_artifact_digests;
		std::vector<std::string> executed_task_partition_set_digests;
		std::optional<materialization_incremental_execution_journal_receipt>
			execution_journal_receipt;

		[[nodiscard]] bool
		operator==(const materialization_incremental_execution_census&) const = default;
	};

	/** Immutable claim output plus the independent execution census for one incremental run. */
	class sealed_materialization_incremental_result
	{
	  public:
		sealed_materialization_incremental_result(
			const sealed_materialization_incremental_result&) = delete;
		sealed_materialization_incremental_result&
		operator=(const sealed_materialization_incremental_result&) = delete;
		sealed_materialization_incremental_result(
			sealed_materialization_incremental_result&&) noexcept = default;
		sealed_materialization_incremental_result&
		operator=(sealed_materialization_incremental_result&&) noexcept = default;
		~sealed_materialization_incremental_result() = default;

		[[nodiscard]] const sealed_materialization_claims& claims() const noexcept;
		/** Production typed partition source; claims() remains a qualification oracle. */
		[[nodiscard]] materialization_bounded_claim_source& bounded_claim_source() noexcept;
		[[nodiscard]] const materialization_bounded_claim_source&
		bounded_claim_source() const noexcept;
		[[nodiscard]] const materialization_incremental_execution_census&
		execution_census() const noexcept;
		/** Independently replayable D2/D3 event source retained behind sealed spools. */
		[[nodiscard]] const materialization_claim_stream_source* claim_stream() const noexcept;

	  private:
		sealed_materialization_incremental_result(
			sealed_materialization_claims claims,
			materialization_bounded_claim_source bounded_claim_source,
			materialization_incremental_execution_census execution_census,
			materialization_claim_stream_source claim_stream) noexcept;

		sealed_materialization_claims claims_;
		materialization_bounded_claim_source bounded_claim_source_;
		materialization_incremental_execution_census execution_census_;
		materialization_claim_stream_source claim_stream_;

		friend sdk::result<sealed_materialization_incremental_result>
		run_materialization_incremental_coordinator(
			const validated_materialization_request& request,
			const sdk::incremental::materialization_plan& plan,
			std::vector<materialization_incremental_task_binding> bindings,
			materialization_incremental_task_executor& executor,
			const materialization_producer_authority& producer_authority,
			const materialization_guarantee_authority& guarantee_authority);
	};

	/** Coordinator result plus the exact Store observation, including any publication issue. */
	class materialization_incremental_publication_result
	{
	  public:
		materialization_incremental_publication_result(
			const materialization_incremental_publication_result&) = delete;
		materialization_incremental_publication_result&
		operator=(const materialization_incremental_publication_result&) = delete;
		materialization_incremental_publication_result(
			materialization_incremental_publication_result&&) noexcept = default;
		materialization_incremental_publication_result&
		operator=(materialization_incremental_publication_result&&) noexcept = default;
		~materialization_incremental_publication_result() = default;

		[[nodiscard]] const sealed_materialization_incremental_result&
		materialization() const noexcept;
		[[nodiscard]] const materialization_store_observation& store() const noexcept;
		/** True only when the single publish and all fixed-order verification passed. */
		[[nodiscard]] bool publication_verified() const noexcept;

	  private:
		materialization_incremental_publication_result(
			sealed_materialization_incremental_result materialization,
			materialization_store_observation store) noexcept;

		sealed_materialization_incremental_result materialization_;
		materialization_store_observation store_;

		friend sdk::result<materialization_incremental_publication_result>
		run_materialization_incremental_coordinator_and_publish(
			const validated_materialization_request& request,
			const sdk::incremental::materialization_plan& plan,
			std::vector<materialization_incremental_task_binding> bindings,
			materialization_incremental_task_executor& executor,
			const materialization_producer_authority& producer_authority,
			const materialization_guarantee_authority& guarantee_authority);
	};

	/**
	 * Consume one admitted v2.1 request through the exact canonical task cursor lifecycle.
	 *
	 * This seam intentionally does not construct claims, reports, or Store transactions. It is the
	 * coordinator-owned bridge for the production caller: the callback performs the bounded worker
	 * execution and hands its result to the next source-private spool boundary. On success, every
	 * task has been released before the cursor is finalized. On failure, the cursor is destroyed
	 * without fabricating a complete finalization receipt.
	 */
	[[nodiscard]] sdk::result<void> run_materialization_incremental_v2_1_task_cursor(
		validated_materialization_request_v2_1& request,
		const sdk::incremental::materialization_plan& plan,
		std::span<const materialization_incremental_task_binding> bindings,
		const materialization_v2_1_task_cursor_consumer& consumer);

	/**
	 * Execute or reuse a validated plan and construct claims only from the complete ordered result
	 * set. Store publication/CAS deliberately remains outside this source-private unit.
	 */
	[[nodiscard]] sdk::result<sealed_materialization_incremental_result>
	run_materialization_incremental_coordinator(
		const validated_materialization_request& request,
		const sdk::incremental::materialization_plan& plan,
		std::vector<materialization_incremental_task_binding> bindings,
		materialization_incremental_task_executor& executor,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority);

	/**
	 * Run the exact coordinator, prepare the transaction, and cross the existing Store publish/CAS
	 * boundary. A returned value always retains the Store observation; callers must inspect its
	 * first issue before treating publication as successful.
	 */
	[[nodiscard]] sdk::result<materialization_incremental_publication_result>
	run_materialization_incremental_coordinator_and_publish(
		const validated_materialization_request& request,
		const sdk::incremental::materialization_plan& plan,
		std::vector<materialization_incremental_task_binding> bindings,
		materialization_incremental_task_executor& executor,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority);
} // namespace cxxlens::detail::clang22::materialization
