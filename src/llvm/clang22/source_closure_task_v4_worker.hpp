#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include <cxxlens/provider/clang22.hpp>

#include "source_closure.hpp"

namespace cxxlens::detail::clang22
{
	/**
	 * Candidate worker input after the inherited v2.2 task authority has supplied effective
	 * compiler arguments.  Arguments and qualified roots are deliberately not reconstructed from
	 * the task-v4 payload: they remain inputs owned by the validated outer task.
	 */
	struct source_closure_task_v4_worker_input
	{
		std::span<const std::byte> input_payload;
		const source_closure_snapshot& closure;
		std::string_view expected_base_task_v3_digest;
		std::string_view expected_task_v4_input_digest;
		std::span<const std::string> effective_arguments;
		std::span<const std::string> qualified_read_roots;
		provider::clang22::translation_unit_callback callback;
	};

	/** Detached receipt returned only after the closure-backed compiler callback succeeds. */
	struct source_closure_task_v4_worker_receipt
	{
		std::string task_id;
		std::string task_v4_digest;
		std::string task_v4_input_digest;
		std::string closure_id;
	};

	/**
	 * Candidate codec-to-VFS bridge for one task-v4 payload.
	 *
	 * This source-private seam is intentionally not called by `run_provider_worker`: ADR 0102 is
	 * still proposed and protocol 1.2/request 2.2 activation remains fail-closed.  Once the
	 * authority is accepted, the production dispatcher can bind its already-validated base-task
	 * arguments and toolchain roots here without introducing an ambient fallback.
	 */
	[[nodiscard]] sdk::result<source_closure_task_v4_worker_receipt>
	execute_source_closure_task_v4_candidate(source_closure_task_v4_worker_input input);
} // namespace cxxlens::detail::clang22
