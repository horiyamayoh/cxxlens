#include "detached_application_materialization_file_service_internal.hpp"

#include <utility>

#include "detached_run_input_file_port_internal.hpp"
#include "detached_run_trust_file_port_internal.hpp"
#include "sdk/application_analysis_service_internal.hpp"
#include "sdk/application_materialization_execution_internal.hpp"

namespace cxxlens::runtime
{
	sdk::result<sdk::detail::application_materialization_adoption>
	publish_detached_application_materializations_from_files(
		const sdk::relation_engine& engine,
		sdk::snapshot_store& store,
		const sdk::detail::application_materialization_execution_plan& plan,
		const detached_application_materialization_file_request& request)
	{
		const detached_run_input_file_port inputs;
		auto detached_runs = inputs.read(request.detached_run_paths, request.limits);
		if (!detached_runs)
			return sdk::unexpected(std::move(detached_runs.error()));
		const detached_run_trust_file_port trust{
			request.signer_id, request.trusted_public_key_path, request.public_key_state};
		const sdk::detail::openssl_detached_run_signature_verifier verifier{trust};
		return sdk::detail::publish_detached_application_materializations(
			engine, store, plan, *detached_runs, verifier, request.limits);
	}

	sdk::result<sdk::materialization_result> materialize_detached_application_from_files(
		sdk::snapshot_store& store,
		const sdk::imported_project& project,
		const sdk::materialization_request& materialization,
		const detached_application_materialization_file_request& files)
	{
		const auto& request =
			sdk::application_analysis_materialization_access_internal::request(materialization);
		if (request.cancellation.stop_requested())
			return sdk::detail::application_materialization_terminal_result(
				sdk::materialization_terminal::cancelled);
		if (!request.selection)
			return sdk::unexpected(sdk::error{"application-analysis.target-unavailable",
											  "materialization",
											  "application analysis providers are not configured"});
		const auto& imported = sdk::application_analysis_imported_value_internal(project);
		auto plan = sdk::detail::make_application_materialization_execution_plan(
			imported,
			request.engine,
			request.publication,
			request.relation_descriptor_ids,
			request.interpretation,
			*request.selection,
			request.budget,
			request.cancellation,
			files.limits);
		if (!plan)
			return sdk::unexpected(std::move(plan.error()));
		auto adopted = publish_detached_application_materializations_from_files(
			request.engine, store, *plan, files);
		if (!adopted)
			return sdk::unexpected(std::move(adopted.error()));
		const auto& manifest =
			plan->units.front().process.selection.selected_candidate().description;
		return sdk::detail::application_materialization_published_result(std::move(*adopted),
																		 manifest);
	}
} // namespace cxxlens::runtime
