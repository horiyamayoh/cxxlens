#include <string>
#include <utility>

#include "provider_worker.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		[[nodiscard]] sdk::error
		provider_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}
	} // namespace

	sdk::result<source_closure_task_v4_worker_receipt>
	run_provider_worker_v2_2(provider_worker_v2_2_dispatch_input input)
	{
		if (input.closure == nullptr)
			return sdk::unexpected(provider_error(
				"source-closure.worker-input-invalid", "closure-authority", "missing"));
		if (!input.task_accepted)
			return sdk::unexpected(provider_error(
				"source-closure.worker-input-invalid", "task-accepted", "callback-missing"));
		if (!input.task.callback)
			return sdk::unexpected(provider_error(
				"source-closure.worker-input-invalid", "compiler-callback", "missing"));

		// The concrete Protocol 2.0 owner must revalidate its message-24..29 transcript before this
		// worker consumes any source bytes.  In particular, an acknowledged-looking task field is
		// not sufficient to authorize task_accepted.
		if (auto valid = input.closure->revalidate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (!input.closure->acknowledged())
			return sdk::unexpected(provider_error(
				"source-closure.task-accepted-before-ack", "closure", "ack-required"));
		if (input.closure->session_id().empty() || input.closure->transfer_digest().empty())
			return sdk::unexpected(provider_error(
				"source-closure.worker-input-invalid", "closure-authority", "identity"));

		// Decode the source-free task-v4 payload before task_accepted.  This is intentionally the
		// v4 decoder; no obsolete task decoder is reachable from this entrypoint.
		auto decoded =
			decode_source_closure_task_v4_input(input.task.input_payload,
												input.task.closure,
												input.task.expected_base_task_digest,
												input.task.expected_task_v4_input_digest);
		if (!decoded)
			return sdk::unexpected(std::move(decoded.error()));

		const auto& authority = *input.closure;
		if (decoded->identity.task_id != authority.task_id() ||
			decoded->identity.task_v4_digest != authority.task_v4_digest() ||
			decoded->input.closure.snapshot_id != authority.closure_id() ||
			decoded->input.closure.closure_digest != authority.closure_digest())
			return sdk::unexpected(provider_error(
				"source-closure.task-binding-mismatch", "closure-authority", "identity"));
		if (auto valid =
				input.task.input_authority.validate(decoded->input.main_logical_path,
													decoded->input.logical_working_directory,
													decoded->input.normalized_invocation_digest);
			!valid)
			return sdk::unexpected(std::move(valid.error()));

		// No task-accepted notification is possible until the complete closure ACK and every
		// source-free task binding have been checked.  Native Clang is still not entered here.
		if (auto accepted = input.task_accepted(decoded->identity); !accepted)
			return sdk::unexpected(provider_error(
				"source-closure.task-accepted-failed", "task-accepted", accepted.error().code));

		// The callback is the only effectful step.  The candidate bridge mounts the authenticated
		// closure VFS and returns a receipt only after the exact Clang callback has completed.
		return execute_source_closure_task_v4_candidate(std::move(input.task));
	}
} // namespace cxxlens::detail::clang22
