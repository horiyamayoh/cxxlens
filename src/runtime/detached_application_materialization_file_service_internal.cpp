#include "detached_application_materialization_file_service_internal.hpp"

#include <utility>

#include "detached_run_input_file_port_internal.hpp"
#include "detached_run_trust_file_port_internal.hpp"

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
} // namespace cxxlens::runtime
