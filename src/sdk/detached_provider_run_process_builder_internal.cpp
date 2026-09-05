#include <new>
#include <stdexcept>
#include <utility>

#include "detached_provider_run_builder_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error process_builder_error(std::string field, std::string detail)
		{
			return {"application-analysis.detached-provider-run-build-failed",
					std::move(field),
					std::move(detail)};
		}
	} // namespace

	result<validated_detached_provider_run>
	build_detached_provider_run(const provider::process_task_request& request,
								std::string replay_plan_digest,
								const std::span<const std::byte> protocol_transcript,
								const detached_run_signer& signer,
								const import_limits limits)
	{
		try
		{
			if (auto valid = request.selection.validate(); !valid)
				return unexpected(std::move(valid.error()));
			auto validated = provider::detail::validate_detached_provider_transcript(
				request, protocol_transcript);
			if (!validated)
				return unexpected(std::move(validated.error()));
			const auto& candidate = request.selection.selected_candidate();
			if (!candidate.trust_valid || !candidate.description.signature)
				return unexpected(process_builder_error("provider", "trusted-signature-required"));

			return build_detached_provider_run_from_validated_transcript(
				{request.task_id,
				 request.task_input_digest,
				 request.normalized_invocation_digest,
				 request.toolchain_digest,
				 request.environment_digest,
				 std::move(replay_plan_digest),
				 {candidate.description.provider_id,
				  candidate.description.provider_version,
				  candidate.description.provider_binary_digest,
				  candidate.description.provider_semantic_contract_digest,
				  *candidate.description.signature,
				  "not-revoked",
				  request.sandbox.policy_digest}},
				protocol_transcript,
				*validated,
				signer,
				limits);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(process_builder_error("memory", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(process_builder_error("memory", "length"));
		}
	}
} // namespace cxxlens::sdk::detail
