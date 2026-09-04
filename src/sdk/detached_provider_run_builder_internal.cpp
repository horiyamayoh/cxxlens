#include "detached_provider_run_builder_internal.hpp"

#include <algorithm>
#include <new>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace cxxlens::sdk::detail
{
	namespace
	{
		constexpr std::string_view signature_scope{"detached-provider-run"};
		constexpr std::string_view successful_provider_terminal{"provider.success"};

		[[nodiscard]] error builder_error(std::string field, std::string detail)
		{
			return {"application-analysis.detached-provider-run-build-failed",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] result<void> validate_runtime_binding(
			const detached_provider_run_authority& authority,
			const std::span<const std::byte> protocol_transcript,
			const provider::detail::validated_detached_provider_transcript& validated)
		{
			if (auto valid = validated.runtime_receipt.validate(); !valid)
				return unexpected(std::move(valid.error()));
			const auto& host_task = validated.input_seal.task();
			const auto& provenance = validated.runtime_receipt.provenance();
			if (host_task.task_id != authority.task_id ||
				host_task.task_input_digest != authority.task_input_digest ||
				host_task.normalized_invocation_digest != authority.normalized_invocation_digest ||
				host_task.toolchain_digest != authority.toolchain_digest ||
				host_task.environment_digest != authority.environment_digest ||
				provenance.task_id != authority.task_id ||
				provenance.task_input_digest != authority.task_input_digest ||
				provenance.normalized_invocation_digest != authority.normalized_invocation_digest ||
				provenance.toolchain_digest != authority.toolchain_digest ||
				provenance.environment_digest != authority.environment_digest)
				return unexpected(builder_error("task", "runtime-binding-mismatch"));
			if (provenance.provider_id != authority.provider.provider_id ||
				provenance.provider_version != authority.provider.provider_version ||
				provenance.provider_binary_digest != authority.provider.binary_digest ||
				provenance.provider_semantic_contract_digest !=
					authority.provider.semantic_contract_digest ||
				provenance.sandbox_policy_digest != authority.provider.sandbox_policy_digest)
				return unexpected(builder_error("provider", "runtime-binding-mismatch"));
			if (validated.runtime_receipt.raw_stdout_byte_count() != protocol_transcript.size() ||
				validated.runtime_receipt.raw_stdout_sha256() !=
					content_digest(protocol_transcript))
				return unexpected(builder_error("protocol_transcript", "runtime-binding-mismatch"));
			auto sealed_digest = provider::detail::provider_sealed_transcript_receipt_digest(
				authority.task_id, successful_provider_terminal, validated.sealed);
			if (!sealed_digest)
				return unexpected(std::move(sealed_digest.error()));
			if (*sealed_digest != validated.runtime_receipt.sealed_transcript_digest())
				return unexpected(builder_error("sealed_transcript", "runtime-binding-mismatch"));
			return {};
		}
	} // namespace

	detached_transcript_projection
	project_detached_provider_transcript(const provider::detail::sealed_provider_transcript& sealed)
	{
		detached_transcript_projection output;
		output.partitions.reserve(sealed.batches().size());
		for (const auto& batch : sealed.batches())
			output.partitions.push_back({std::string{batch.descriptor_id()},
										 std::string{batch.descriptor_digest()},
										 std::string{batch.dependency_group_id()},
										 std::string{batch.atomic_output_group_id()},
										 std::string{batch.batch_id()},
										 std::string{batch.batch_digest()},
										 batch.rows().size()});
		output.coverage.reserve(sealed.coverage().size());
		for (const auto& value : sealed.coverage())
			output.coverage.push_back({value.kind, value.id, value.state, value.reason});
		output.unresolved.reserve(sealed.unresolved().size());
		for (const auto& value : sealed.unresolved())
			output.unresolved.push_back({value.code, value.subject, value.detail});
		output.provenance.reserve(sealed.evidence().size());
		for (const auto& value : sealed.evidence())
			output.provenance.push_back({value.kind, value.subject, value.producer, value.summary});
		std::ranges::sort(output.partitions,
						  {},
						  [](const auto& value)
						  {
							  return std::tie(value.descriptor_id,
											  value.dependency_group_id,
											  value.atomic_output_group_id,
											  value.batch_id);
						  });
		std::ranges::sort(output.coverage,
						  {},
						  [](const auto& value)
						  {
							  return std::tie(value.kind, value.id);
						  });
		std::ranges::sort(output.unresolved,
						  {},
						  [](const auto& value)
						  {
							  return std::tie(value.code, value.subject, value.detail);
						  });
		std::ranges::sort(output.provenance,
						  {},
						  [](const auto& value)
						  {
							  return std::tie(
								  value.kind, value.subject, value.producer, value.summary);
						  });
		return output;
	}

	result<validated_detached_provider_run> build_detached_provider_run_from_validated_transcript(
		detached_provider_run_authority authority,
		const std::span<const std::byte> protocol_transcript,
		const provider::detail::validated_detached_provider_transcript& validated,
		const detached_run_signer& signer,
		const import_limits limits)
	{
		try
		{
			if (auto bound = validate_runtime_binding(authority, protocol_transcript, validated);
				!bound)
				return unexpected(std::move(bound.error()));
			detached_provider_run_draft draft;
			draft.task_id = std::move(authority.task_id);
			draft.task_input_digest = std::move(authority.task_input_digest);
			draft.replay_plan_digest = std::move(authority.replay_plan_digest);
			draft.provider = std::move(authority.provider);
			draft.protocol_transcript.assign(protocol_transcript.begin(),
											 protocol_transcript.end());
			auto projection = project_detached_provider_transcript(validated.sealed);
			draft.partitions = std::move(projection.partitions);
			draft.coverage = std::move(projection.coverage);
			draft.unresolved = std::move(projection.unresolved);
			draft.provenance = std::move(projection.provenance);
			const bool partial = !draft.unresolved.empty() ||
				std::ranges::any_of(draft.coverage,
									[](const auto& value)
									{
										return value.state != "covered";
									});
			draft.terminal = partial ? detached_provider_terminal::partial
									 : detached_provider_terminal::complete;
			auto receipt =
				provider::detail::provider_runtime_receipt_digest(validated.runtime_receipt);
			if (!receipt)
				return unexpected(std::move(receipt.error()));
			draft.runtime_receipt_digest = std::move(*receipt);
			auto subject = detached_provider_run_signed_subject_digest(draft);
			if (!subject)
				return unexpected(std::move(subject.error()));
			auto signature = signer.sign(signature_scope, *subject);
			if (!signature)
				return unexpected(std::move(signature.error()));
			draft.authentication.signer_id = std::move(signature->signer_id);
			draft.authentication.key_fingerprint = std::move(signature->key_fingerprint);
			draft.authentication.signed_subject_digest = std::move(*subject);
			draft.authentication.signature = signature->signature;
			draft.authentication.signature_digest = content_digest(draft.authentication.signature);
			return validate_detached_provider_run(std::move(draft), limits);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(builder_error("memory", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(builder_error("memory", "length"));
		}
	}

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
				return unexpected(builder_error("provider", "trusted-signature-required"));

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
			return unexpected(builder_error("memory", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(builder_error("memory", "length"));
		}
	}
} // namespace cxxlens::sdk::detail
