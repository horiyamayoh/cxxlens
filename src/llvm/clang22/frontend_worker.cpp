#include "frontend_worker.hpp"

#include <csignal>
#include <string>
#include <utility>

#include "../common/frontend_worker_ipc.hpp"
#include "frontend_job.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		[[nodiscard]] error worker_error(std::string reason)
		{
			error failure;
			failure.code.value = "parse.frontend-failed";
			failure.message = "Frontend worker request was rejected";
			failure.scope = failure_scope::compile_unit;
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}
	} // namespace

	result<std::string> run_frontend_worker(const std::string_view request_bytes)
	{
		auto request = frontend::decode_worker_request(request_bytes);
		if (!request)
			return frontend::encode_worker_response(
				result<frontend::observation_batch>{std::move(request.error())});
		const auto adapter = capability();
		const auto actual_toolchain = std::string{"clang22:"} + std::to_string(adapter.llvm_major) +
			"." + std::to_string(adapter.llvm_minor) + "." + std::to_string(adapter.llvm_patch) +
			"@" + adapter.adapter_version;
		if (request.value().toolchain_version != actual_toolchain)
			return frontend::encode_worker_response(
				result<frontend::observation_batch>{worker_error("toolchain-version-mismatch")});
		if (request.value().task.injected_fault == frontend::frontend_fault::crash)
			(void)::raise(SIGABRT);
		auto outcome = execute(std::move(request.value().task));
		return frontend::encode_worker_response(outcome);
	}
} // namespace cxxlens::detail::clang22
