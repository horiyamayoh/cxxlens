#include "source_closure_task_v4_worker.hpp"

#include <string>
#include <utility>
#include <vector>

#include "source_closure_native.hpp"
#include "source_closure_task_v4.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}
	} // namespace

	sdk::result<source_closure_task_v4_worker_receipt>
	execute_source_closure_task_v4_candidate(source_closure_task_v4_worker_input input)
	{
		auto decoded = decode_source_closure_task_v4_input(input.input_payload,
														   input.closure,
														   input.expected_base_task_digest,
														   input.expected_task_v4_input_digest);
		if (!decoded)
			return sdk::unexpected(std::move(decoded.error()));
		if (auto valid =
				input.input_authority.validate(decoded->input.main_logical_path,
											   decoded->input.logical_working_directory,
											   decoded->input.normalized_invocation_digest);
			!valid)
			return sdk::unexpected(std::move(valid.error()));
		if (!input.callback)
			return sdk::unexpected(failure("source-closure.worker-input-invalid", "callback"));

		source_closure_native_input native_input{
			decoded->input.closure,
			decoded->input.main_logical_path,
			decoded->input.logical_working_directory,
			input.input_authority.effective_arguments,
			input.input_authority.qualified_read_roots,
			{},
		};
		if (auto result =
				with_source_closure_translation_unit(native_input, std::move(input.callback));
			!result)
			return sdk::unexpected(std::move(result.error()));

		return source_closure_task_v4_worker_receipt{decoded->identity.task_id,
													 decoded->identity.task_v4_digest,
													 decoded->identity.task_v4_input_digest,
													 decoded->input.closure.snapshot_id};
	}
} // namespace cxxlens::detail::clang22
