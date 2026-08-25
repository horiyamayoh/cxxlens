#include "store_candidate_projection_internal.hpp"

#include <utility>

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] result<std::vector<std::byte>>
		tuple_bytes(std::vector<canonical_value> values)
		{
			return canonical_binary(canonical_value::from_tuple(std::move(values)));
		}

		[[nodiscard]] canonical_value strings(std::vector<std::string> values)
		{
			std::vector<canonical_value> encoded;
			encoded.reserve(values.size());
			for (auto& value : values)
				encoded.push_back(canonical_value::from_string(std::move(value)));
			return canonical_value::from_tuple(std::move(encoded));
		}
	} // namespace

	result<std::vector<std::byte>>
	encode_snapshot_candidate_partition(const partition_draft& partition)
	{
		const std::span<const claim_conflict> conflicts{};
		const std::span<const differential_disagreement> disagreements{};
		auto claims = claim_batch_content_encoding(
			partition.claims, partition.unresolved, conflicts, disagreements);
		if (!claims)
			return unexpected(std::move(claims.error()));
		std::vector<std::string> coverage;
		coverage.reserve(partition.coverage.size());
		for (const auto& value : partition.coverage)
			coverage.push_back(value.canonical_form());
		return tuple_bytes({canonical_value::from_string(partition.relation_descriptor_id),
							canonical_value::from_string(partition.scope),
							canonical_value::from_string(partition.condition.canonical_form()),
							canonical_value::from_string(partition.interpretation),
							canonical_value::from_string(partition.producer_semantics),
							canonical_value::from_string(partition.producer_input_basis_digest),
							canonical_value::from_string(partition.precision_profile),
							canonical_value::from_string(partition.assumption_set_id),
							canonical_value::from_bytes(std::move(*claims)),
							strings(std::move(coverage))});
	}

	result<std::vector<std::byte>>
	encode_snapshot_candidate_binding(const snapshot_partition_binding& binding)
	{
		return tuple_bytes({canonical_value::from_string(binding.partition_id),
							canonical_value::from_string(binding.relation_descriptor_id),
							canonical_value::from_string(binding.scope),
							canonical_value::from_string(binding.condition.canonical_form()),
							canonical_value::from_string(binding.interpretation),
							canonical_value::from_string(binding.producer_semantics),
							canonical_value::from_string(binding.producer_input_basis_digest),
							canonical_value::from_string(binding.precision_profile),
							canonical_value::from_string(binding.assumption_set_id)});
	}

	result<std::vector<std::byte>>
	encode_snapshot_candidate_closure(const closure_candidate& closure)
	{
		return tuple_bytes({canonical_value::from_string(closure.relation_descriptor_id),
							canonical_value::from_string(closure.subject_partition_id),
							canonical_value::from_string(closure.partition_content_digest),
							canonical_value::from_string(closure.coverage_digest),
							canonical_value::from_string(closure.key_domain_digest),
							canonical_value::from_string(closure.condition.canonical_form()),
							canonical_value::from_string(closure.interpretation),
							canonical_value::from_string(closure.assumption_set_id),
							canonical_value::from_string(closure.closure_kind),
							canonical_value::from_string(closure.producer_semantics),
							canonical_value::from_string(closure.evidence_digest)});
	}

	result<std::vector<std::byte>>
	encode_snapshot_candidate_manifest(const snapshot_manifest& manifest)
	{
		return tuple_bytes(
			{canonical_value::from_string(manifest.schema),
			 canonical_value::from_string(manifest.snapshot_semantics_version.string()),
			 canonical_value::from_string(manifest.catalog_semantic_digest),
			 canonical_value::from_string(manifest.condition_universe_id),
			 canonical_value::from_string(manifest.relation_registry_digest),
			 canonical_value::from_string(manifest.interpretation_policy_digest)});
	}

	result<std::vector<std::byte>>
	encode_snapshot_candidate_unresolved(const unresolved_reference& unresolved)
	{
		return tuple_bytes({canonical_value::from_string(unresolved.source_assertion),
							canonical_value::from_string(unresolved.source_relation),
							canonical_value::from_string(unresolved.target_relation),
							strings(unresolved.source_columns),
							canonical_value::from_string(unresolved.reason)});
	}
} // namespace cxxlens::sdk::detail
