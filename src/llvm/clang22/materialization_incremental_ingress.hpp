#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "materialization_incremental_receipt.hpp"
#include "materialization_seal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/**
	 * One move-only task handoff to the private bounded ingress.
	 *
	 * The task result and event spools are owned by this value only until consume_task() has
	 * independently checked the external receipt and every sealed partition stream. They are
	 * destroyed before consume_task() returns; the ingress retains only receipt metadata.
	 */
	struct materialization_incremental_task_ingress
	{
		sealed_materialization_result result;
		materialization_incremental_task_receipt receipt;
		std::vector<std::unique_ptr<materialization_replayable_spool>> partition_spools;

		materialization_incremental_task_ingress(
			sealed_materialization_result result,
			materialization_incremental_task_receipt receipt,
			std::vector<std::unique_ptr<materialization_replayable_spool>> partition_spools)
			: result{std::move(result)}, receipt{std::move(receipt)},
			  partition_spools{std::move(partition_spools)}
		{
		}

		materialization_incremental_task_ingress(const materialization_incremental_task_ingress&) =
			delete;
		materialization_incremental_task_ingress&
		operator=(const materialization_incremental_task_ingress&) = delete;
		materialization_incremental_task_ingress(
			materialization_incremental_task_ingress&&) noexcept = default;
		materialization_incremental_task_ingress&
		operator=(materialization_incremental_task_ingress&&) noexcept = default;
	};

	/**
	 * D2/D3 private ingress lifecycle: exact next task, one live task seal, then a cycle-free
	 * execution journal. No Store candidate is visible until finalize()&& succeeds.
	 */
	class materialization_incremental_ingress
	{
	  public:
		materialization_incremental_ingress(const materialization_incremental_ingress&) = delete;
		materialization_incremental_ingress&
		operator=(const materialization_incremental_ingress&) = delete;
		materialization_incremental_ingress(materialization_incremental_ingress&&) noexcept =
			default;
		materialization_incremental_ingress&
		operator=(materialization_incremental_ingress&&) noexcept = default;
		~materialization_incremental_ingress() = default;

		/** Begin one exact selected-request task census. */
		[[nodiscard]] static sdk::result<materialization_incremental_ingress>
		begin(const validated_materialization_request& request,
			  std::vector<std::vector<std::string>> expected_partition_ids);

		/** Consume only the canonical next task and destroy its input before returning. */
		[[nodiscard]] sdk::result<void>
		consume_task(materialization_incremental_task_ingress task) &&;

		/** Seal the exact task receipt set and return the immutable journal receipt. */
		[[nodiscard]] sdk::result<materialization_incremental_execution_journal_receipt>
		finalize() &&;

		[[nodiscard]] std::size_t consumed_task_count() const noexcept
		{
			return next_task_index_;
		}

	  private:
		const validated_materialization_request* request_{};
		std::string request_id_;
		std::vector<std::vector<std::string>> expected_partition_ids_;
		std::vector<std::optional<materialization_incremental_task_receipt>> task_receipts_;
		std::size_t next_task_index_{};

		materialization_incremental_ingress(
			const validated_materialization_request& request,
			std::string request_id,
			std::vector<std::vector<std::string>> expected_partition_ids);
	};
} // namespace cxxlens::detail::clang22::materialization
