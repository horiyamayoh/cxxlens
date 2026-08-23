#include "materialization_pipeline_v2_2.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace cxxlens::detail::clang22::materialization
{
	const source_closure_manifest* materialization_v2_2_closure_admission::manifest_for(
		const std::string_view closure_id) const noexcept
	{
		const auto found =
			std::ranges::find(manifests,
							  closure_id,
							  [](const source_closure_manifest& manifest) -> std::string_view
							  {
								  return manifest.closure_id;
							  });
		return found == manifests.end() ? nullptr : &*found;
	}

	sdk::result<materialization_v2_2_closure_admission>
	admit_materialization_request_v2_2_for_execution(
		materialization_request_v2_2 request,
		const std::span<const std::string> advertised_features,
		const std::span<const source_closure_manifest> manifests,
		const materialization_request_v2_2_limits limits)
	{
		if (manifests.empty())
			return sdk::unexpected(sdk::error{
				"materialization.source-closure-invalid", "manifests", "closure-not-validated"});

		// The request validator performs the complete one-to-one closure census, manifest digest
		// binding, task-v4 identity check, and main-member binding. Do not expose its pre-transfer
		// overload to the production caller: that result is intentionally insufficient for this
		// boundary.
		auto validated = validate_materialization_request_v2_2(
			std::move(request), advertised_features, manifests, limits);
		if (!validated)
			return sdk::unexpected(std::move(validated.error()));

		try
		{
			materialization_v2_2_closure_admission admission{
				std::move(*validated),
				std::vector<source_closure_manifest>{manifests.begin(), manifests.end()}};
			for (const auto& task : admission.request.request.task_extensions)
			{
				const auto* manifest =
					admission.manifest_for(task.source_closure.source_closure_id);
				if (manifest == nullptr)
					return sdk::unexpected(sdk::error{"materialization.source-closure-invalid",
													  "task.source_closure",
													  "manifest-missing"});
			}
			return admission;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				sdk::error{"materialization.spool-failure", "source-closure", "allocation"});
		}
	}

	sdk::result<materialization_v2_2_task_admission>
	accept_materialization_task_v2_2(const materialization_v2_2_closure_admission& admission,
									 const std::uint64_t task_index)
	{
		if (task_index >= admission.request.request.task_extensions.size() ||
			task_index >= admission.request.request.base_tasks.size())
			return sdk::unexpected(
				sdk::error{"materialization.task-binding-mismatch", "task_index", "out-of-range"});

		const auto& task =
			admission.request.request.task_extensions[static_cast<std::size_t>(task_index)];
		const auto* manifest = admission.manifest_for(task.source_closure.source_closure_id);
		if (manifest == nullptr)
			return sdk::unexpected(sdk::error{"materialization.source-closure-invalid",
											  "task.source_closure",
											  "manifest-missing"});
		if (auto valid = bind_provider_task_v4_main_member(
				admission.request.request.base_tasks[static_cast<std::size_t>(task_index)],
				task,
				*manifest);
			!valid)
			return sdk::unexpected(std::move(valid.error()));

		try
		{
			return materialization_v2_2_task_admission{
				task_index,
				task.task_id,
				task.task_v4_digest,
				task.source_closure.source_closure_id,
				task.source_closure.source_closure_digest,
				task.source_closure.manifest_digest,
				materialization_v2_2_task_phase::task_accepted};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				sdk::error{"materialization.spool-failure", "task-acceptance", "allocation"});
		}
	}

	sdk::result<void> begin_materialization_task_v2_2(materialization_v2_2_task_admission& task)
	{
		if (task.phase != materialization_v2_2_task_phase::task_accepted || task.task_id.empty() ||
			task.task_v4_digest.empty() || task.source_closure_id.empty() ||
			task.source_closure_digest.empty() || task.manifest_digest.empty())
			return sdk::unexpected(sdk::error{
				"materialization.task-binding-mismatch", "task-phase", "closure-not-accepted"});
		task.phase = materialization_v2_2_task_phase::materialization_started;
		return {};
	}
} // namespace cxxlens::detail::clang22::materialization
