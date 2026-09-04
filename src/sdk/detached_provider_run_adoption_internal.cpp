#include "detached_provider_run_adoption_internal.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

#include "detached_provider_run_builder_internal.hpp"
#include "provider_runtime_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		constexpr std::string_view signature_scope{"detached-provider-run"};

		[[nodiscard]] error adoption_error(std::string field, std::string detail)
		{
			return {"application-analysis.detached-run-adoption-invalid",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] bool digest_like(const std::string_view value)
		{
			return value.starts_with("sha256:") && value.size() == 71U &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] bool same_budget(const provider::execution_budget& left,
									   const provider::execution_budget& right) noexcept
		{
			return left.wall_ms == right.wall_ms && left.cpu_ms == right.cpu_ms &&
				left.address_space_bytes == right.address_space_bytes &&
				left.transport_bytes == right.transport_bytes &&
				left.output_bytes == right.output_bytes && left.rows == right.rows &&
				left.diagnostics == right.diagnostics && left.open_files == right.open_files &&
				left.subprocesses == right.subprocesses;
		}

		[[nodiscard]] bool same_descriptors(const std::span<const relation_descriptor> left,
											const std::span<const relation_descriptor> right)
		{
			if (left.size() != right.size())
				return false;
			for (std::size_t index{}; index < left.size(); ++index)
				if (left[index].id != right[index].id ||
					left[index].descriptor_digest != right[index].descriptor_digest)
					return false;
			return true;
		}

		[[nodiscard]] result<detached_run_signature_verification>
		authenticate(const validated_detached_provider_run& run,
					 const trusted_detached_run_signature_verifier& verifier)
		{
			const auto& authentication = run.value().authentication;
			const auto verification = verifier.verify(signature_scope,
													  authentication.signer_id,
													  authentication.key_fingerprint,
													  authentication.signed_subject_digest,
													  authentication.signature,
													  authentication.signature_digest);
			if (verification.verdict == detached_run_signature_verdict::unavailable)
				return unexpected(
					error{"application-analysis.detached-run-authentication-unavailable",
						  "run_authentication",
						  "trusted-verifier-unavailable"});
			if (verification.verdict == detached_run_signature_verdict::revoked)
				return unexpected(adoption_error("run_authentication", "signing-key-revoked"));
			if (verification.verdict != detached_run_signature_verdict::verified)
				return unexpected(adoption_error("run_authentication", "signature-rejected"));
			if (!validate_strong_id(verification.verifier_id) ||
				!digest_like(verification.trust_store_digest) ||
				verification.signer_id != authentication.signer_id ||
				verification.key_fingerprint != authentication.key_fingerprint ||
				verification.signed_subject_digest != authentication.signed_subject_digest ||
				verification.signature_digest != authentication.signature_digest)
				return unexpected(adoption_error("run_authentication", "verifier-binding"));
			return verification;
		}

	} // namespace

	result<prepared_application_materialization> prepare_detached_application_materialization(
		const relation_engine& engine,
		const validated_materialization_task& task,
		const validated_compiler_replay_input& replay_input,
		const provider::process_task_request& process_request,
		const validated_detached_provider_run& detached_run,
		const trusted_detached_run_signature_verifier& signature_verifier,
		const std::span<const partition_draft> host_partitions)
	{
		const auto& run = detached_run.value();
		if (run.terminal != detached_provider_terminal::complete &&
			run.terminal != detached_provider_terminal::partial)
			return unexpected(adoption_error("terminal", "not-adoptable"));
		if (run.task_id != process_request.task_id ||
			run.task_id != task.value().provider_task.task_id)
			return unexpected(adoption_error("task_id", "authority-mismatch"));
		if (run.task_input_digest != replay_input.input_digest() ||
			run.task_input_digest != process_request.task_input_digest ||
			run.task_input_digest != task.value().provider_input_digest ||
			!std::ranges::equal(process_request.payload, replay_input.bytes()))
			return unexpected(adoption_error("task_input_digest", "authority-mismatch"));
		if (run.replay_plan_digest != replay_input.value().replay_plan_digest)
			return unexpected(adoption_error("replay_plan_digest", "authority-mismatch"));

		if (auto valid = process_request.selection.validate(); !valid)
			return unexpected(std::move(valid.error()));
		const auto& manifest = process_request.selection.selected_candidate().description;
		const auto& requirement = task.value().provider;
		const auto& capture = task.value().capture.value();
		if (!manifest.signature || *manifest.signature != run.provider.signature_digest ||
			run.provider.revocation_state != "not-revoked" ||
			run.provider.provider_id != manifest.provider_id ||
			run.provider.provider_version != manifest.provider_version ||
			run.provider.binary_digest != manifest.provider_binary_digest ||
			run.provider.semantic_contract_digest != manifest.provider_semantic_contract_digest ||
			requirement.provider_id != manifest.provider_id ||
			requirement.provider_version != manifest.provider_version ||
			requirement.provider_binary_digest != manifest.provider_binary_digest ||
			requirement.provider_semantics_digest != manifest.provider_semantic_contract_digest ||
			run.provider.sandbox_policy_digest != process_request.sandbox.policy_digest ||
			process_request.sandbox.policy_digest !=
				process_request.selection.authority_request().sandbox.policy_digest ||
			process_request.sandbox.minimum != requirement.sandbox.minimum ||
			process_request.sandbox.policy_digest != requirement.sandbox.policy_digest ||
			!same_budget(process_request.budget, requirement.budget) ||
			!same_descriptors(process_request.output_descriptors,
							  task.value().provider_task.outputs) ||
			process_request.normalized_invocation_digest !=
				capture.invocation.effective_invocation_digest ||
			process_request.toolchain_digest != capture.toolchain_digest ||
			process_request.environment_digest != capture.invocation.environment_digest)
			return unexpected(adoption_error("provider_identity", "authority-mismatch"));
		auto authenticated = authenticate(detached_run, signature_verifier);
		if (!authenticated)
			return unexpected(std::move(authenticated.error()));

		auto transcript = provider::detail::validate_detached_provider_transcript(
			process_request, run.protocol_transcript);
		if (!transcript)
			return unexpected(std::move(transcript.error()));
		auto projection = project_detached_provider_transcript(transcript->sealed);
		if (projection.partitions != run.partitions || projection.coverage != run.coverage ||
			projection.unresolved != run.unresolved || projection.provenance != run.provenance)
			return unexpected(adoption_error("projection", "raw-transcript-mismatch"));
		const bool partial = !transcript->sealed.unresolved().empty() ||
			std::ranges::any_of(transcript->sealed.coverage(),
								[](const auto& value)
								{
									return value.state != "covered";
								});
		if ((partial && run.terminal != detached_provider_terminal::partial) ||
			(!partial && run.terminal != detached_provider_terminal::complete))
			return unexpected(adoption_error("terminal", "projection-mismatch"));

		auto provider_receipt =
			provider::detail::provider_runtime_receipt_digest(transcript->runtime_receipt);
		if (!provider_receipt || !run.runtime_receipt_digest ||
			*run.runtime_receipt_digest != *provider_receipt)
			return unexpected(adoption_error("runtime_receipt_digest", "authority-mismatch"));
		const std::array receipt_fields{
			canonical_value::from_string(std::string{detached_run.digest()}),
			canonical_value::from_string(*provider_receipt),
			canonical_value::from_string(authenticated->verifier_id),
			canonical_value::from_string(authenticated->trust_store_digest),
			canonical_value::from_string(authenticated->signer_id),
			canonical_value::from_string(authenticated->key_fingerprint),
			canonical_value::from_string(authenticated->signed_subject_digest),
			canonical_value::from_string(authenticated->signature_digest),
		};
		auto adoption_receipt =
			canonical_identity_digest("detached-provider-run-adoption-receipt", receipt_fields);
		if (!adoption_receipt)
			return unexpected(std::move(adoption_receipt.error()));

		auto frontend = resolve_compiler_replay_frontend(replay_input.value().analysis_frontend,
														 replay_input.value().target_abi,
														 replay_input.value().effective_arguments);
		if (!frontend)
			return unexpected(std::move(frontend.error()));
		materialization_runtime_binding runtime{manifest.provider_id,
												manifest.provider_version,
												manifest.provider_binary_digest,
												manifest.provider_semantic_contract_digest,
												std::string{replay_input.input_digest()},
												*adoption_receipt};
		return prepare_sealed_application_materialization(engine,
														  task,
														  transcript->sealed,
														  std::move(runtime),
														  *adoption_receipt,
														  run.replay_plan_digest,
														  frontend->observation_technique,
														  host_partitions);
	}
} // namespace cxxlens::sdk::detail
