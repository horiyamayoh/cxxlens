#include "provider_worker_v4_claim_bridge.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

		[[nodiscard]] sdk::result<void>
		validate_binding_identity(const source_closure_task_v4_decoded& metadata,
								  std::string_view closure_id,
								  std::string_view closure_digest,
								  std::string_view manifest_digest,
								  std::string_view main_file_id,
								  const materialization::materialization_v4_claim_binding& binding)
		{
			const auto& base = binding.base_task;
			const auto& task = binding.task;
			const auto reject = [&](const std::string_view field)
			{
				return sdk::unexpected(failure(
					"source-closure.task-v4-binding-mismatch", "claim-output", std::string{field}));
			};
			if (binding.task_index != metadata.input.base_task_index)
				return reject("task-index");
			if (base.provider_task_id != metadata.input.base_provider_task_id)
				return reject("base-provider-task-id");
			if (base.canonical_base_task_digest != metadata.identity.base_task_digest)
				return reject("base-task-digest");
			if (base.task_input_digest != metadata.input.task_input_digest)
				return reject("base-task-input-digest");
			if (base.normalized_invocation_digest != metadata.input.normalized_invocation_digest)
				return reject("base-invocation-digest");
			if (base.working_directory != metadata.input.logical_working_directory)
				return reject("base-working-directory");
			if (base.source.logical_path != metadata.input.main_logical_path)
				return reject("base-main-path");
			if (task.task_id != metadata.identity.task_id)
				return reject("task-id");
			if (task.task_v4_digest != metadata.identity.task_v4_digest)
				return reject("task-v4-digest");
			if (task.base_task_index != metadata.input.base_task_index)
				return reject("task-index-extension");
			if (task.base_provider_task_id != metadata.input.base_provider_task_id)
				return reject("task-base-provider-task-id");
			if (task.base_task_digest != metadata.identity.base_task_digest)
				return reject("task-base-task-digest");
			if (task.open_task.task_input_digest != metadata.input.task_input_digest)
				return reject("task-input-digest");
			if (task.open_task.normalized_invocation_digest !=
				metadata.input.normalized_invocation_digest)
				return reject("task-invocation-digest");
			if (task.logical_working_directory != metadata.input.logical_working_directory)
				return reject("task-working-directory");
			if (task.main_logical_path != metadata.input.main_logical_path)
				return reject("task-main-path");
			if (task.source_closure.source_closure_id != closure_id)
				return reject("task-closure-id");
			if (task.source_closure.source_closure_digest != closure_digest)
				return reject("task-closure-digest");
			if (task.source_closure.manifest_digest != manifest_digest)
				return reject("task-manifest-digest");
			if (binding.manifest.closure_id != closure_id)
				return reject("manifest-closure-id");
			if (binding.manifest.closure_digest != closure_digest)
				return reject("manifest-closure-digest");
			if (binding.manifest.manifest_digest != manifest_digest)
				return reject("manifest-digest");

			const auto main = std::ranges::find_if(
				binding.manifest.members,
				[&](const materialization::source_closure_manifest_member& member)
				{
					return member.logical_path == metadata.input.main_logical_path;
				});
			if (main == binding.manifest.members.end() || main->file_id != main_file_id)
				return sdk::unexpected(failure(
					"source-closure.task-v4-binding-mismatch", "claim-output", "main-file"));
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_publication(
			const materialization::materialization_v4_store_publication_authority& value)
		{
			const std::array<std::pair<std::string_view, std::string_view>, 3U> ids{{
				{"analysis-recipe", value.analysis_recipe_digest},
				{"output-plan", value.output_plan_digest},
				{"publication-target", value.publication_target},
			}};
			for (const auto& [field, id] : ids)
				if (auto valid = sdk::validate_strong_id(id); !valid)
					return sdk::unexpected(failure("provider-worker-v4.output-authority-invalid",
												   std::string{field},
												   "strong-id"));
			return {};
		}
	} // namespace

	sdk::result<void> provider_worker_v4_output_authority::validate() const
	{
		if (engine.descriptors().empty())
			return sdk::unexpected(
				failure("provider-worker-v4.output-authority-invalid", "relation-engine", "empty"));
		return validate_publication(publication);
	}

	sdk::result<provider_worker_v4_claim_receipt>
	execute_provider_worker_v4_with_claim_output(provider_worker_v4_claim_input input)
	{
		if (!input.output_callback)
			return sdk::unexpected(
				failure("provider-worker-v4.output-invalid", "callback", "missing"));
		if (input.worker.callback)
			return sdk::unexpected(
				failure("provider-worker-v4.output-invalid", "callback", "duplicate"));
		if (auto valid = input.output_authority.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));

		// The worker is moved into the execution API below.  Keep an immutable detached copy of
		// the authority inputs so a moved-from worker cannot weaken the post-execution binding
		// comparison.
		const auto metadata = input.worker.metadata;
		const auto closure = input.worker.closure;
		const auto closure_id = closure.snapshot_id;
		const auto closure_digest = closure.closure_digest;
		auto manifest_digest = source_closure_manifest_digest(closure);
		if (!manifest_digest)
			return sdk::unexpected(std::move(manifest_digest.error()));
		const auto* main = closure.find_member(metadata.input.main_logical_path);
		if (main == nullptr)
			return sdk::unexpected(
				failure("source-closure.main-invalid", "main_logical_path", "claim-output"));
		const auto main_file_id = main->file_id;

		std::optional<materialization::materialization_v4_claim_translation> translation;
		input.worker.callback =
			[callback = std::move(input.output_callback), &translation](
				provider::clang22::borrowed_translation_unit& unit) mutable -> sdk::result<void>
		{
			auto output = callback(unit);
			if (!output)
				return sdk::unexpected(std::move(output.error()));
			translation.emplace(std::move(*output));
			return {};
		};
		auto execution = execute_provider_worker_v4(std::move(input.worker));
		if (!execution)
			return sdk::unexpected(std::move(execution.error()));
		if (!translation)
			return sdk::unexpected(
				failure("provider-worker-v4.output-invalid", "callback", "not-produced"));

		if (auto valid = validate_binding_identity(metadata,
												   closure_id,
												   closure_digest,
												   *manifest_digest,
												   main_file_id,
												   translation->binding);
			!valid)
			return sdk::unexpected(std::move(valid.error()));

		auto sealed = materialization::seal_materialization_v4_claim_translation(
			input.output_authority.engine, std::move(*translation));
		if (!sealed)
			return sdk::unexpected(std::move(sealed.error()));
		const std::array<const materialization::materialization_v4_claim_sealed*, 1U> tasks{
			&*sealed};
		auto receipt = materialization::make_materialization_v4_incremental_receipt(
			input.output_authority.engine, tasks);
		if (!receipt)
			return sdk::unexpected(std::move(receipt.error()));
		auto ingress = materialization::admit_materialization_v4_store_ingress(
			input.output_authority.engine, *receipt, tasks, input.output_authority.publication);
		if (!ingress)
			return sdk::unexpected(std::move(ingress.error()));
		return provider_worker_v4_claim_receipt{
			std::move(*execution), std::move(*sealed), std::move(*ingress)};
	}
} // namespace cxxlens::detail::clang22
