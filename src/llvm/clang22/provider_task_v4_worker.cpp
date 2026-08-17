#include "provider_task_v4_worker.hpp"

#include <utility>

namespace cxxlens::detail::clang22::source_closure
{
	std::expected<task_v4_worker_receipt, validation_error>
	execute_task_v4_worker(task_v4_replay& replay,
		clang_translation_unit_callback callback,
		const task_v4_decode_limits decode_limits)
	{
		if (!callback)
			return std::unexpected(validation_error{
				"source-closure.task-v4-worker-callback", "callback", {}});
		auto decoded = decode_task_v4(replay, decode_limits);
		if (!decoded)
			return std::unexpected(std::move(decoded.error()));
		const auto task_id = decoded->task.task_id;
		const auto closure_id = decoded->task.closure->snapshot_id;
		const auto main_path = decoded->task.main_logical_path;
		const auto decoded_bytes = decoded->consumed_bytes;
		auto executed = run_with_compiler_vfs(decoded->task.closure,
			decoded->task.effective_arguments,
			decoded->task.logical_working_directory,
			std::move(callback));
		if (!executed)
			return std::unexpected(validation_error{
				executed.error().code, executed.error().path,
				executed.error().detail});
		return task_v4_worker_receipt{
			std::move(task_id), std::move(closure_id), std::move(main_path),
			decoded_bytes};
	}
} // namespace cxxlens::detail::clang22::source_closure
