#include "frontend_scheduler_worker.hpp"

#include "../llvm/clang22/frontend_job.hpp"

namespace cxxlens::detail::scheduling
{
	result<frontend::observation_batch>
	frontend_scheduler_worker::execute(const task_request& task, execution_context context)
	{
		return clang22::execute(task.parse, std::move(context));
	}
} // namespace cxxlens::detail::scheduling
