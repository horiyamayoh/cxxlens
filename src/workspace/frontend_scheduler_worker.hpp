#pragma once

#include "scheduler.hpp"

namespace cxxlens::detail::scheduling
{
	class frontend_scheduler_worker final : public worker_port
	{
	  public:
		[[nodiscard]] result<frontend::observation_batch>
		execute(const task_request& task, execution_context context) override;
	};
} // namespace cxxlens::detail::scheduling
