#pragma once

#include <string>
#include <vector>

#include "scheduler.hpp"

namespace cxxlens::detail::scheduling
{
	class frontend_scheduler_worker final : public worker_port
	{
	  public:
		explicit frontend_scheduler_worker(std::string executable = "cxxlens-frontend-worker");
		explicit frontend_scheduler_worker(std::vector<std::string> argv);
		[[nodiscard]] result<frontend::observation_batch>
		execute(const task_request& task, execution_context context) override;

	  private:
		std::vector<std::string> argv_;
	};
} // namespace cxxlens::detail::scheduling
