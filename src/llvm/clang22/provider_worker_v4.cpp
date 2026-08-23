#include "provider_worker_v4.hpp"

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "source_closure_native.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool nonempty(const std::string_view value) noexcept
		{
			return !value.empty();
		}

		[[nodiscard]] std::vector<provider_worker_v4_missing_data> missing_provider_output()
		{
			return {
				{"provider-output.analysis-recipe",
				 "provider-observation-selection",
				 "task-v4-metadata-does-not-carry-analysis-recipe"},
				{"provider-output.output-plan",
				 "detached-provider-rows",
				 "task-v4-metadata-does-not-carry-relation-output-plan"},
				{"provider-output.publication-target",
				 "materializer-store-publication",
				 "task-v4-metadata-does-not-carry-publication-target"},
			};
		}

		[[nodiscard]] sdk::result<void>
		validate_identity(const source_closure_task_v4_decoded& metadata)
		{
			auto recomputed = derive_source_closure_task_v4_identity(metadata.input);
			if (!recomputed)
				return sdk::unexpected(std::move(recomputed.error()));
			if (*recomputed != metadata.identity)
				return sdk::unexpected(failure("source-closure.task-v4-binding-mismatch",
											   "identity",
											   "decoded-metadata-recomputation"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_closure_binding(const source_closure_task_v4_decoded& metadata,
								 const source_closure_snapshot& closure)
		{
			if (auto valid = closure.validate(); !valid)
				return valid;
			if (metadata.input.closure.snapshot_id != closure.snapshot_id ||
				metadata.input.closure.closure_digest != closure.closure_digest)
				return sdk::unexpected(failure("source-closure.task-v4-binding-mismatch",
											   "source_closure",
											   "snapshot-identity"));

			auto manifest_digest = source_closure_manifest_digest(closure);
			if (!manifest_digest)
				return sdk::unexpected(std::move(manifest_digest.error()));
			if (*manifest_digest != metadata.identity.manifest_digest)
				return sdk::unexpected(failure("source-closure.task-v4-binding-mismatch",
											   "manifest_digest",
											   "authenticated-closure"));

			const auto* main = closure.find_member(metadata.input.main_logical_path);
			if (main == nullptr || main->role != source_closure_role::main)
				return sdk::unexpected(failure("source-closure.main-invalid",
											   "main_logical_path",
											   metadata.input.main_logical_path));
			return {};
		}
	} // namespace

	sdk::result<void> provider_worker_v4_receipt::validate() const
	{
		if (schema != provider_worker_v4_receipt_schema)
			return sdk::unexpected(failure("provider-worker-v4.receipt-invalid", "schema"));
		if (!nonempty(task_id) || !nonempty(task_v4_digest) || !nonempty(task_v4_input_digest) ||
			!nonempty(source_closure_id) || !nonempty(main_file_id))
			return sdk::unexpected(failure("provider-worker-v4.receipt-invalid", "identity"));
		if (output_state != "translation-unit-executed")
			return sdk::unexpected(failure("provider-worker-v4.receipt-invalid", "output_state"));
		if (!translation_unit_executed)
			return sdk::unexpected(
				failure("provider-worker-v4.receipt-invalid", "translation_unit_executed"));
		if (missing_output.empty())
			return sdk::unexpected(
				failure("provider-worker-v4.receipt-invalid", "missing_output", "empty"));
		std::string previous;
		for (const auto& item : missing_output)
		{
			if (!nonempty(item.field) || !nonempty(item.required_for) || !nonempty(item.reason))
				return sdk::unexpected(
					failure("provider-worker-v4.receipt-invalid", "missing_output"));
			if (!previous.empty() && previous >= item.field)
				return sdk::unexpected(failure(
					"provider-worker-v4.receipt-invalid", "missing_output", "noncanonical-order"));
			previous = item.field;
		}
		return {};
	}

	sdk::result<provider_worker_v4_receipt>
	execute_provider_worker_v4(provider_worker_v4_input input)
	{
		if (auto valid = validate_identity(input.metadata); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (auto valid = validate_closure_binding(input.metadata, input.closure); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (auto valid =
				input.input_authority.validate(input.metadata.input.main_logical_path,
											   input.metadata.input.logical_working_directory,
											   input.metadata.input.normalized_invocation_digest);
			!valid)
			return sdk::unexpected(std::move(valid.error()));
		if (!input.callback)
			return sdk::unexpected(failure("provider-worker-v4.input-invalid", "callback"));

		bool callback_ran = false;
		const auto metadata_input = input.metadata.input;
		source_closure_native_input native_input{
			input.closure,
			metadata_input.main_logical_path,
			metadata_input.logical_working_directory,
			input.input_authority.effective_arguments,
			input.input_authority.qualified_read_roots,
			{},
		};
		auto callback =
			[callback = std::move(input.callback), &callback_ran](
				provider::clang22::borrowed_translation_unit& unit) mutable -> sdk::result<void>
		{
			callback_ran = true;
			return callback(unit);
		};
		if (auto result = with_source_closure_translation_unit(native_input, std::move(callback));
			!result)
			return sdk::unexpected(std::move(result.error()));
		if (!callback_ran)
			return sdk::unexpected(
				failure("provider-worker-v4.execution-invalid", "callback", "not-invoked"));

		const auto* main = input.closure.find_member(metadata_input.main_logical_path);
		if (main == nullptr)
			return sdk::unexpected(failure("source-closure.main-invalid", "main_logical_path"));
		provider_worker_v4_receipt receipt{
			std::string{provider_worker_v4_receipt_schema},
			input.metadata.identity.task_id,
			input.metadata.identity.task_v4_digest,
			input.metadata.identity.task_v4_input_digest,
			input.closure.snapshot_id,
			main->file_id,
			"translation-unit-executed",
			true,
			missing_provider_output(),
		};
		if (auto valid = receipt.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return receipt;
	}
} // namespace cxxlens::detail::clang22
