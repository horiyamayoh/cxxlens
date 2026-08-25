#pragma once

/** @file materialization_v4_execution_journal.hpp
 *  @brief Source-private task-v4 execution/reuse journal.
 *
 * This header intentionally contains only task-v4 dependencies. Report publication remains
 * separate, so a v4 consumer cannot accidentally acquire unrelated response-authority APIs
 * through this boundary.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_v4_incremental_ingress.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/**
	 * One task-v4 execution census entry.
	 *
	 * The receipt is the complete source-closure/provider/partition identity. A reuse entry is
	 * therefore never keyed by a task number alone. `provider_call_count` is an observation of the
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
	 * This is deliberately separate from report publication. It has no request/report binding and
	 * it does not manufacture a result from counters: finalization compares every
	 * ordered task receipt and the recomputed incremental digest. Consequently stale, tampered, and
	 * reordered prior artifacts fail before a provider/store effect is authorized.
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
