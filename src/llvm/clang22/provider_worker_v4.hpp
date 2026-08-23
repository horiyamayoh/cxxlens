#pragma once

/** @file provider_worker_v4.hpp @brief Typed task-v4 worker ingress. */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/provider/clang22.hpp>
#include <cxxlens/sdk/common.hpp>

#include "provider_task_v4.hpp"
#include "source_closure.hpp"
#include "source_closure_task_v4.hpp"

namespace cxxlens::detail::clang22
{
	inline constexpr std::string_view provider_worker_v4_receipt_schema =
		"cxxlens.clang22.worker-receipt.v4";

	/**
	 * Product data still required before a task-v4 translation unit can become a provider batch.
	 *
	 * Task-v4 intentionally carries execution and source-closure authority only.  It does not
	 * select an analysis recipe, a relation output plan, or a publication target.  The ingress
	 * therefore reports those missing inputs in a typed receipt instead of fabricating a provider
	 * result from the task identity or from ambient process state.
	 */
	struct provider_worker_v4_missing_data
	{
		std::string field;
		std::string required_for;
		std::string reason;

		[[nodiscard]] bool operator==(const provider_worker_v4_missing_data&) const = default;
	};

	/**
	 * Detached result of the bounded task-v4 execution boundary.
	 *
	 * This is deliberately an execution receipt, not a claim or relation report.  The callback
	 * may inspect Clang's borrowed native state synchronously, but no native pointer escapes this
	 * boundary.  `missing_output` makes the current product boundary explicit: detached provider
	 * rows are not asserted until the task carries the recipe, output plan, and publication target
	 * required to derive them.
	 */
	struct provider_worker_v4_receipt
	{
		std::string schema{provider_worker_v4_receipt_schema};
		std::string task_id;
		std::string task_v4_digest;
		std::string task_v4_input_digest;
		std::string source_closure_id;
		std::string main_file_id;
		std::string output_state;
		bool translation_unit_executed{};
		std::vector<provider_worker_v4_missing_data> missing_output;

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/**
	 * Owned ingress input assembled by the Protocol 2.0 dispatcher.
	 *
	 * `metadata` is the already decoded canonical task-v4 payload.  `closure` is supplied by the
	 * separately authenticated source-closure transport and is checked against the metadata
	 * before Clang is entered.  Compiler arguments and qualified roots are explicit dispatcher
	 * inputs; there is no fallback to a host working directory, environment, or legacy decoder.
	 */
	struct provider_worker_v4_input
	{
		source_closure_task_v4_decoded metadata;
		source_closure_snapshot closure;
		provider_task_v4_input_authority input_authority;
		provider::clang22::translation_unit_callback callback;
	};

	/**
	 * Execute one authenticated task-v4 translation unit through the closure-only native VFS.
	 *
	 * The function validates both independently-owned closure identity and the recomputed task-v4
	 * identity before calling `with_source_closure_translation_unit`.  Success means that the
	 * callback completed and all native state was released; it does not mean that provider rows
	 * were produced.
	 */
	[[nodiscard]] sdk::result<provider_worker_v4_receipt>
	execute_provider_worker_v4(provider_worker_v4_input input);
} // namespace cxxlens::detail::clang22
