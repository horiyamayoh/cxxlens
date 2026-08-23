#include "materialization_v4_claim_binding.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error invalid(std::string field, std::string detail = {})
		{
			return {"materialization.v4-claim-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error mismatch(std::string field, std::string detail = {})
		{
			return {
				"materialization.v4-claim-binding-mismatch", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::result<void> strong(const std::string_view value,
											   const std::string_view field)
		{
			if (auto valid = sdk::validate_strong_id(value); !valid)
				return sdk::unexpected(invalid(std::string{field}, "strong-id"));
			return {};
		}

		[[nodiscard]] sdk::canonical_value text(const std::string_view value)
		{
			return sdk::canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] sdk::canonical_value count(const std::uint64_t value)
		{
			// All counts in this boundary are bounded below INT64_MAX before this helper is used.
			return sdk::canonical_value::from_integer(static_cast<std::int64_t>(value));
		}

		[[nodiscard]] sdk::canonical_value base_projection(const provider_task_v4_base_task& value)
		{
			return sdk::canonical_value::from_tuple({
				text(value.provider_task_id),
				text(value.provider_execution_id),
				text(value.canonical_base_task_digest),
				text(value.task_input_digest),
				text(value.normalized_invocation_digest),
				text(value.toolchain_digest),
				text(value.environment_digest),
				text(value.working_directory),
				sdk::canonical_value::from_tuple({
					text(value.source.source_snapshot_id),
					text(value.source.file_id),
					text(value.source.logical_path),
					text(value.source.content_digest),
					count(value.source.size_bytes),
					text(value.source.encoding),
					text(value.source.line_index_id),
					sdk::canonical_value::from_boolean(value.source.read_only),
				}),
			});
		}

		[[nodiscard]] sdk::canonical_value
		closure_summary_projection(const source_closure_summary& value)
		{
			return sdk::canonical_value::from_tuple({
				text(value.source_closure_id),
				text(value.source_closure_digest),
				text(value.manifest_digest),
				count(value.member_count),
				count(value.blob_count),
				count(value.unique_blob_bytes),
			});
		}

		[[nodiscard]] sdk::canonical_value task_projection(const provider_task_v4& value)
		{
			return sdk::canonical_value::from_tuple({
				text(value.schema),
				text(value.task_id),
				text(value.task_v4_digest),
				count(value.base_task_index),
				text(value.base_provider_task_id),
				text(value.base_task_digest),
				sdk::canonical_value::from_tuple({
					text(value.open_task.task_input_digest),
					text(value.open_task.normalized_invocation_digest),
					text(value.open_task.toolchain_digest),
					text(value.open_task.environment_digest),
				}),
				closure_summary_projection(value.source_closure),
				text(value.main_logical_path),
				text(value.logical_working_directory),
			});
		}

		[[nodiscard]] sdk::canonical_value manifest_projection(const source_closure_manifest& value)
		{
			return sdk::canonical_value::from_tuple({
				text(value.schema),
				text(value.closure_id),
				text(value.closure_digest),
				text(value.manifest_digest),
			});
		}

		[[nodiscard]] sdk::canonical_value guarantee_projection(const sdk::claim_guarantee& value)
		{
			std::vector<sdk::canonical_value> modalities;
			modalities.reserve(value.verification_modalities.size());
			for (const auto& modality : value.verification_modalities)
				modalities.push_back(text(modality));
			return sdk::canonical_value::from_tuple({
				text(value.approximation),
				text(value.scope),
				text(value.assumptions),
				sdk::canonical_value::from_tuple(std::move(modalities)),
			});
		}

		[[nodiscard]] bool same_guarantee(const sdk::claim_guarantee& left,
										  const sdk::claim_guarantee& right)
		{
			return left.approximation == right.approximation && left.scope == right.scope &&
				left.assumptions == right.assumptions &&
				left.verification_modalities == right.verification_modalities;
		}

		[[nodiscard]] sdk::result<void>
		validate_binding(const materialization_v4_claim_binding& value)
		{
			if (value.schema != materialization_v4_claim_binding_schema)
				return sdk::unexpected(invalid("binding.schema", "unsupported"));
			if (auto valid = strong(value.materialization_request_id, "binding.request-id"); !valid)
				return valid;
			if (value.task_index > 4095U)
				return sdk::unexpected(invalid("binding.task-index", "bound"));
			if (auto valid = value.base_task.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = validate_provider_task_v4_identity(value.task); !valid)
				return valid;
			if (auto valid =
					bind_provider_task_v4_main_member(value.base_task, value.task, value.manifest);
				!valid)
				return valid;
			if (value.task.base_task_index != value.task_index)
				return sdk::unexpected(mismatch("binding.task-index", "task-v4-index"));

			const std::array<std::pair<std::string_view, std::string_view>, 11U> ids{{
				{"provider-id", value.provider_id},
				{"provider-semantic-contract", value.provider_semantic_contract_digest},
				{"materializer-id", value.materializer_id},
				{"materializer-semantic-contract", value.materializer_semantic_contract_digest},
				{"direct-basis", value.direct_basis_digest},
				{"canonical-transform", value.canonical_adoption_transform_digest},
				{"base-transform", value.base_ingestion_transform_digest},
				{"assumption-set", value.assumption_set_id},
				{"relation-descriptor", value.relation_descriptor_id},
				{"scope", value.scope},
				{"interpretation", value.interpretation},
			}};
			for (const auto& [field, id] : ids)
				if (auto valid = strong(id, std::string{"binding."} + std::string{field}); !valid)
					return valid;
			if (auto valid = strong(value.precision_profile, "binding.precision-profile"); !valid)
				return valid;
			if (auto valid = value.guarantee.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (value.assumption_set_id != value.guarantee.assumptions)
				return sdk::unexpected(mismatch("binding.assumption-set", "guarantee"));
			return {};
		}

		[[nodiscard]] sdk::result<std::string> batch_digest(const sdk::claim_batch_result& batch)
		{
			return sdk::claim_batch_content_digest(
				batch.claims, batch.unresolved, batch.conflicts, batch.differential_disagreements);
		}

		[[nodiscard]] sdk::result<std::string>
		claim_set_digest(const std::vector<sdk::claim>& claims,
						 const std::vector<sdk::unresolved_reference>& unresolved)
		{
			return sdk::claim_batch_content_digest(claims, unresolved, {}, {});
		}

		[[nodiscard]] sdk::result<void>
		validate_claim_payload(const sdk::relation_engine& engine,
							   const materialization_v4_claim_binding& binding,
							   const sdk::claim_batch_result& batch)
		{
			for (const auto& claim : batch.claims)
			{
				if (auto valid = sdk::validate_claim(engine, claim); !valid)
					return sdk::unexpected(std::move(valid.error()));
				if (claim.provenance_root.empty())
					return sdk::unexpected(invalid("claim.provenance-root", "missing"));
				if (!same_guarantee(claim.guarantee, binding.guarantee))
					return sdk::unexpected(mismatch("claim.guarantee", claim.content));
				const bool provider = claim.producer.id == binding.provider_id &&
					claim.producer.semantic_contract == binding.provider_semantic_contract_digest;
				const bool materializer = claim.producer.id == binding.materializer_id &&
					claim.producer.semantic_contract ==
						binding.materializer_semantic_contract_digest;
				if (!provider && !materializer)
					return sdk::unexpected(mismatch("claim.producer", claim.content));
				if (provider && claim.stage != sdk::claim_stage::assertion)
					return sdk::unexpected(mismatch("claim.stage", "provider-assertion"));
				if (materializer && claim.stage == sdk::claim_stage::assertion)
					return sdk::unexpected(mismatch("claim.stage", "materializer-derived"));
			}
			return {};
		}

		[[nodiscard]] sdk::result<materialization_v4_claim_receipt>
		make_receipt(const materialization_v4_claim_translation& translation,
					 const std::string& binding_digest,
					 const sdk::partition_manifest& manifest)
		{
			const auto& binding = translation.binding;
			materialization_v4_claim_receipt receipt;
			receipt.binding_digest = binding_digest;
			receipt.materialization_request_id = binding.materialization_request_id;
			receipt.task_index = binding.task_index;
			receipt.task_id = binding.task.task_id;
			receipt.task_v4_digest = binding.task.task_v4_digest;
			receipt.provider_execution_id = binding.base_task.provider_execution_id;
			receipt.source_closure_id = binding.task.source_closure.source_closure_id;
			receipt.source_closure_digest = binding.task.source_closure.source_closure_digest;
			receipt.manifest_digest = binding.task.source_closure.manifest_digest;
			receipt.task_input_digest = binding.base_task.task_input_digest;
			auto batch = batch_digest(translation.batch);
			if (!batch)
				return sdk::unexpected(std::move(batch.error()));
			receipt.claim_batch_content_digest = std::move(*batch);
			receipt.partition_id = manifest.partition_id;
			receipt.partition_content_digest = manifest.content_digest;
			receipt.coverage_digest = manifest.coverage_digest;
			receipt.claim_count = manifest.claim_count;
			receipt.unresolved_count = translation.batch.unresolved.size();
			receipt.conflict_count = translation.batch.conflicts.size();
			receipt.differential_disagreement_count =
				translation.batch.differential_disagreements.size();
			receipt.complete = manifest.complete;

			const auto digest = sdk::canonical_identity_digest(
				"cxxlens.clang22.materialization-claim-receipt.v4",
				std::array{
					text(receipt.schema),
					text(receipt.binding_digest),
					text(receipt.materialization_request_id),
					count(receipt.task_index),
					text(receipt.task_id),
					text(receipt.task_v4_digest),
					text(receipt.provider_execution_id),
					text(receipt.source_closure_id),
					text(receipt.source_closure_digest),
					text(receipt.manifest_digest),
					text(receipt.task_input_digest),
					text(receipt.claim_batch_content_digest),
					text(receipt.partition_id),
					text(receipt.partition_content_digest),
					text(receipt.coverage_digest),
					count(receipt.claim_count),
					count(receipt.unresolved_count),
					count(receipt.conflict_count),
					count(receipt.differential_disagreement_count),
					sdk::canonical_value::from_boolean(receipt.complete),
				});
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			receipt.receipt_digest = *digest;
			return receipt;
		}

		[[nodiscard]] sdk::result<void>
		validate_receipt_shape(const materialization_v4_claim_receipt& value)
		{
			if (value.schema != materialization_v4_claim_receipt_schema)
				return sdk::unexpected(invalid("receipt.schema", "unsupported"));
			const std::array<std::pair<std::string_view, std::string_view>, 15U> ids{{
				{"binding-digest", value.binding_digest},
				{"request-id", value.materialization_request_id},
				{"task-id", value.task_id},
				{"task-v4-digest", value.task_v4_digest},
				{"provider-execution-id", value.provider_execution_id},
				{"source-closure-id", value.source_closure_id},
				{"source-closure-digest", value.source_closure_digest},
				{"manifest-digest", value.manifest_digest},
				{"task-input-digest", value.task_input_digest},
				{"claim-batch-digest", value.claim_batch_content_digest},
				{"partition-id", value.partition_id},
				{"partition-content-digest", value.partition_content_digest},
				{"coverage-digest", value.coverage_digest},
				{"receipt-digest", value.receipt_digest},
				{"schema", value.schema},
			}};
			for (const auto& [field, id] : ids)
				if (auto valid = strong(id, std::string{"receipt."} + std::string{field}); !valid)
					return valid;
			if (value.task_index > 4095U)
				return sdk::unexpected(invalid("receipt.task-index", "bound"));
			return {};
		}
	} // namespace

	sdk::result<std::string>
	materialization_v4_claim_binding_digest(const materialization_v4_claim_binding& binding)
	{
		if (auto valid = validate_binding(binding); !valid)
			return sdk::unexpected(std::move(valid.error()));
		const auto projection = sdk::canonical_value::from_tuple({
			text(binding.schema),
			text(binding.materialization_request_id),
			count(binding.task_index),
			base_projection(binding.base_task),
			task_projection(binding.task),
			manifest_projection(binding.manifest),
			text(binding.provider_id),
			text(binding.provider_semantic_contract_digest),
			text(binding.materializer_id),
			text(binding.materializer_semantic_contract_digest),
			text(binding.direct_basis_digest),
			text(binding.canonical_adoption_transform_digest),
			text(binding.base_ingestion_transform_digest),
			guarantee_projection(binding.guarantee),
			text(binding.assumption_set_id),
			text(binding.relation_descriptor_id),
			text(binding.scope),
			text(binding.interpretation),
			text(binding.precision_profile),
		});
		auto encoded = sdk::canonical_binary(projection);
		if (!encoded)
			return sdk::unexpected(std::move(encoded.error()));
		std::string bytes;
		bytes.reserve(encoded->size());
		for (const auto byte : *encoded)
			bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
		return sdk::semantic_digest("cxxlens.clang22.materialization-claim-binding.v4", bytes);
	}

	sdk::result<materialization_v4_claim_sealed>
	seal_materialization_v4_claim_translation(const sdk::relation_engine& engine,
											  materialization_v4_claim_translation translation)
	{
		if (auto valid = validate_binding(translation.binding); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (auto valid = validate_claim_payload(engine, translation.binding, translation.batch);
			!valid)
			return sdk::unexpected(std::move(valid.error()));

		auto expected_batch = batch_digest(translation.batch);
		if (!expected_batch || translation.batch.content_digest != *expected_batch)
			return sdk::unexpected(invalid("claim-batch.content-digest", "recomputed"));
		sdk::claim_batch recomputed_batch;
		for (const auto& claim : translation.batch.claims)
			if (auto added = recomputed_batch.add(claim); !added)
				return sdk::unexpected(std::move(added.error()));
		auto recomputed = std::move(recomputed_batch).commit(engine);
		if (!recomputed || recomputed->content_digest != translation.batch.content_digest)
			return sdk::unexpected(invalid("claim-batch", "independent-replay"));

		const auto& binding = translation.binding;
		const auto& partition = translation.partition;
		if (partition.relation_descriptor_id != binding.relation_descriptor_id ||
			partition.scope != binding.scope ||
			partition.interpretation != binding.interpretation ||
			partition.precision_profile != binding.precision_profile ||
			partition.assumption_set_id != binding.assumption_set_id)
			return sdk::unexpected(mismatch("partition.identity", "binding"));
		if (partition.producer_semantics == binding.provider_semantic_contract_digest &&
			partition.producer_input_basis_digest != binding.direct_basis_digest)
			return sdk::unexpected(mismatch("partition.producer-input-basis", "direct-authority"));
		if (partition.coverage.empty())
			return sdk::unexpected(invalid("partition.coverage", "missing"));
		for (const auto& coverage : partition.coverage)
			if (auto valid = coverage.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
		for (const auto& unresolved : partition.unresolved)
			if (unresolved.reason.empty() || unresolved.source_assertion.empty() ||
				unresolved.source_relation.empty() || unresolved.target_relation.empty() ||
				unresolved.source_columns.empty())
				return sdk::unexpected(invalid("partition.unresolved", "typed-record"));

		auto partition_claim_digest = claim_set_digest(partition.claims, partition.unresolved);
		if (!partition_claim_digest)
			return sdk::unexpected(std::move(partition_claim_digest.error()));
		auto batch_claim_digest =
			claim_set_digest(translation.batch.claims, translation.batch.unresolved);
		if (!batch_claim_digest || *partition_claim_digest != *batch_claim_digest)
			return sdk::unexpected(mismatch("partition.claims", "batch"));
		if (partition.claims.size() != translation.batch.claims.size() ||
			partition.unresolved.size() != translation.batch.unresolved.size())
			return sdk::unexpected(mismatch("partition.census", "batch"));

		for (const auto& claim : partition.claims)
		{
			if (claim.descriptor != partition.relation_descriptor_id ||
				claim.presence != partition.condition ||
				claim.interpretation != partition.interpretation ||
				claim.producer.semantic_contract != partition.producer_semantics)
				return sdk::unexpected(mismatch("partition.claim", claim.content));
			auto input_basis = sdk::claim_input_basis_digest(claim.input_basis);
			if (!input_basis || *input_basis != partition.producer_input_basis_digest)
				return sdk::unexpected(mismatch("partition.producer-input-basis", claim.content));
		}
		if (partition.claims.empty() && !translation.batch.claims.empty())
			return sdk::unexpected(mismatch("partition.claims", "empty"));

		auto manifest = sdk::make_partition_manifest(engine, partition);
		if (!manifest)
			return sdk::unexpected(std::move(manifest.error()));
		const sdk::snapshot_partition_binding partition_binding{
			manifest->partition_id,
			partition.relation_descriptor_id,
			partition.scope,
			partition.condition,
			partition.interpretation,
			partition.producer_semantics,
			partition.producer_input_basis_digest,
			partition.precision_profile,
			partition.assumption_set_id,
		};
		auto certificate = sdk::make_partition_certificate_subject(*manifest, partition_binding);
		if (!certificate)
			return sdk::unexpected(std::move(certificate.error()));

		auto binding_digest = materialization_v4_claim_binding_digest(binding);
		if (!binding_digest)
			return sdk::unexpected(std::move(binding_digest.error()));
		auto receipt = make_receipt(translation, *binding_digest, *manifest);
		if (!receipt)
			return sdk::unexpected(std::move(receipt.error()));
		return materialization_v4_claim_sealed{
			std::move(translation), std::move(*manifest), partition_binding, std::move(*receipt)};
	}

	sdk::result<void>
	validate_materialization_v4_claim_receipt(const sdk::relation_engine& engine,
											  const materialization_v4_claim_sealed& sealed)
	{
		if (auto valid = validate_receipt_shape(sealed.receipt); !valid)
			return valid;
		// Re-run the full boundary without trusting any receipt field, then compare the exact
		// derived identity tuple.  This catches post-seal mutation of claims, coverage, or task
		// metadata before the Store receives a partition handle.
		materialization_v4_claim_translation copy{
			sealed.translation.binding,
			sealed.translation.batch,
			sealed.translation.partition,
		};
		auto expected = seal_materialization_v4_claim_translation(engine, std::move(copy));
		if (!expected)
			return sdk::unexpected(std::move(expected.error()));
		if (expected->partition_manifest != sealed.partition_manifest ||
			expected->partition_binding != sealed.partition_binding ||
			expected->receipt != sealed.receipt)
			return sdk::unexpected(mismatch("receipt", "recomputed"));
		return {};
	}
} // namespace cxxlens::detail::clang22::materialization
