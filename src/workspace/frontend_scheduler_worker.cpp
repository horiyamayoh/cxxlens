#include "frontend_scheduler_worker.hpp"

#include <chrono>
#include <string>
#include <utility>

#include "../llvm/clang22/frontend_job.hpp"
#include "../llvm/common/frontend_worker_ipc.hpp"
#include "../runtime/process_port.hpp"

namespace cxxlens::detail::scheduling
{
	namespace
	{
		[[nodiscard]] error
		worker_failure(std::string code, std::string reason, const int detail = 0)
		{
			error failure;
			failure.code.value = std::move(code);
			failure.message = "Isolated Clang frontend worker failed";
			failure.scope = failure_scope::compile_unit;
			failure.attributes.emplace("reason", std::move(reason));
			if (detail != 0)
				failure.attributes.emplace("platform_code", std::to_string(detail));
			return failure;
		}

		[[nodiscard]] std::string toolchain_version()
		{
			const auto adapter = clang22::capability();
			return std::string{"clang22:"} + std::to_string(adapter.llvm_major) + "." +
				std::to_string(adapter.llvm_minor) + "." + std::to_string(adapter.llvm_patch) +
				"@" + adapter.adapter_version;
		}
	} // namespace

	frontend_scheduler_worker::frontend_scheduler_worker(std::string executable)
		: argv_{std::move(executable)}
	{
	}

	frontend_scheduler_worker::frontend_scheduler_worker(std::vector<std::string> argv)
		: argv_{std::move(argv)}
	{
	}

	result<frontend::observation_batch>
	frontend_scheduler_worker::execute(const task_request& task, execution_context context)
	{
		if (argv_.empty() || argv_.front().empty() || task.input_fingerprint.empty())
			return worker_failure("parse.invocation-build-failed", "worker-input-incomplete");
		frontend::worker_request request{task.parse,
										 task.profile.to_json(),
										 task.snapshot_key,
										 task.input_fingerprint,
										 toolchain_version()};
		auto encoded = frontend::encode_worker_request(request);
		if (!encoded)
			return std::move(encoded.error());

		runtime::process_request process_request;
		process_request.argv = argv_;
		process_request.standard_input = std::move(encoded.value());
		process_request.timeout = std::chrono::minutes{5};
		runtime::request_context process_context;
		process_context.operation = "frontend.worker.execute";
		process_context.cancellation = context.cancellation;
		process_context.deadline = context.deadline;
		process_context.output_limit = std::size_t{64U} * 1024U * 1024U;
		runtime::argv_process_adapter processes;
		auto launched = processes.run(process_request, process_context);
		if (!launched)
		{
			switch (launched.error().status)
			{
				case runtime::runtime_status::cancelled:
					return worker_failure("core.cancelled", "worker-cancelled");
				case runtime::runtime_status::timed_out:
					return worker_failure("parse.timeout", "worker-deadline-exceeded");
				case runtime::runtime_status::output_limit:
					return worker_failure("parse.frontend-failed", "worker-output-limit");
				default:
					return worker_failure("parse.frontend-failed",
										  "worker-launch-failed",
										  launched.error().platform_code);
			}
		}
		if (launched.value().termination_signal != 0)
			return worker_failure(
				"parse.crashed", "worker-signal", launched.value().termination_signal);
		if (launched.value().exit_code != 0)
			return worker_failure(
				"parse.crashed", "worker-abnormal-exit", launched.value().exit_code);
		return frontend::decode_worker_response(launched.value().standard_output);
	}
} // namespace cxxlens::detail::scheduling
