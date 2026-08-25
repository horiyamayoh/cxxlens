#include "materialization_v4_store_source.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error invalid(std::string field, std::string detail = {})
		{
			return {"materialization.v4-store-source-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error mismatch(std::string field, std::string detail = {})
		{
			return {
				"materialization.v4-store-source-mismatch", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error incomplete(std::string field)
		{
			return {"materialization.v4-store-source-incomplete", std::move(field), {}};
		}

		[[nodiscard]] sdk::result<void> strong(std::string_view value, std::string field)
		{
			if (auto valid = sdk::validate_strong_id(value); !valid)
				return sdk::unexpected(invalid(std::move(field), "strong-id"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		add_checked(std::uint64_t& total, const std::uint64_t value, std::string field)
		{
			if (value > std::numeric_limits<std::uint64_t>::max() - total)
				return sdk::unexpected(invalid(std::move(field), "overflow"));
			total += value;
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_authority(const sdk::relation_engine& engine,
						   const materialization_v4_incremental_receipt& receipt,
						   const materialization_v4_provider_output_authority& authority)
		{
			if (authority.schema != materialization_v4_provider_output_authority_schema)
				return sdk::unexpected(invalid("authority.schema", "unsupported"));
			if (auto valid = strong(authority.materialization_request_id,
									"authority.materialization-request-id");
				!valid)
				return valid;
			if (authority.materialization_request_id != receipt.materialization_request_id)
				return sdk::unexpected(mismatch("authority.materialization-request-id", "receipt"));

			const std::array<std::pair<std::string_view, std::string_view>, 3U> publication_ids{{
				{"analysis-recipe", authority.publication.analysis_recipe_digest},
				{"output-plan", authority.publication.output_plan_digest},
				{"publication-target", authority.publication.publication_target},
			}};
			for (const auto& [field, value] : publication_ids)
				if (auto valid = strong(value, "authority.publication." + std::string{field});
					!valid)
					return valid;

			if (auto valid = authority.snapshot.series.validate(); !valid)
				return sdk::unexpected(invalid("authority.snapshot.series", valid.error().code));
			if (auto valid = strong(authority.snapshot.catalog_semantic_digest,
									"authority.snapshot.catalog-semantic-digest");
				!valid)
				return valid;
			if (authority.snapshot.expected_parent_publication)
				if (auto valid = strong(*authority.snapshot.expected_parent_publication,
										"authority.snapshot.expected-parent");
					!valid)
					return valid;
			if (authority.closures.size() > materialization_v4_store_max_closures)
				return sdk::unexpected(invalid("authority.closures", "bound"));
			if (authority.snapshot.series.engine_generation_id != engine.generation())
				return sdk::unexpected(
					mismatch("authority.snapshot.series.engine-generation", "engine"));
			if (authority.snapshot.series.relation_registry_digest != engine.registry_digest())
				return sdk::unexpected(
					mismatch("authority.snapshot.series.registry-digest", "engine"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_receipt_shape(const materialization_v4_incremental_receipt& receipt)
		{
			if (receipt.schema != materialization_v4_incremental_receipt_schema)
				return sdk::unexpected(invalid("receipt.schema", "unsupported"));
			if (receipt.task_count == 0U ||
				receipt.task_count > materialization_v4_incremental_max_tasks)
				return sdk::unexpected(invalid("receipt.task-count", "bound"));
			if (receipt.task_count != receipt.task_receipts.size())
				return sdk::unexpected(mismatch("receipt.task-count", "task-receipts"));
			if (auto valid = strong(receipt.materialization_request_id, "receipt.request-id");
				!valid)
				return valid;
			if (auto valid = strong(receipt.receipt_digest, "receipt.digest"); !valid)
				return valid;
			if (!receipt.complete)
				return sdk::unexpected(incomplete("receipt"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_partition_receipt(const materialization_v4_store_partition& value,
								   const materialization_v4_claim_receipt& aggregate,
								   const std::size_t expected_index)
		{
			if (value.task_index != expected_index || value.receipt.task_index != expected_index)
				return sdk::unexpected(mismatch("partition.task-index", "order"));
			if (value.receipt.materialization_request_id != aggregate.materialization_request_id)
				return sdk::unexpected(mismatch("partition.request-id", "receipt"));
			if (value.receipt.partition_id != value.manifest.partition_id ||
				value.receipt.partition_content_digest != value.manifest.content_digest ||
				value.receipt.coverage_digest != value.manifest.coverage_digest ||
				value.receipt.claim_count != value.manifest.claim_count ||
				value.receipt.complete != value.manifest.complete ||
				value.receipt.unresolved_count != value.draft.unresolved.size())
				return sdk::unexpected(mismatch("partition.receipt", "manifest"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_grouped_output_receipt(const materialization_v4_store_partition& value,
										const materialization_v4_claim_receipt& task_receipt)
		{
			if (!value.has_receipt)
				return {};
			if (value.receipt.materialization_request_id !=
					task_receipt.materialization_request_id ||
				value.receipt.task_index != task_receipt.task_index ||
				value.receipt.task_id != task_receipt.task_id ||
				value.receipt.task_v4_digest != task_receipt.task_v4_digest)
				return sdk::unexpected(mismatch("partition.receipt", "group-task-binding"));
			if (!value.receipt.complete)
				return sdk::unexpected(incomplete("partition.receipt"));
			if (value.receipt.partition_id != value.manifest.partition_id ||
				value.receipt.partition_content_digest != value.manifest.content_digest ||
				value.receipt.coverage_digest != value.manifest.coverage_digest ||
				value.receipt.claim_count != value.manifest.claim_count ||
				value.receipt.complete != value.manifest.complete ||
				value.receipt.unresolved_count != value.draft.unresolved.size())
				return sdk::unexpected(mismatch("partition.receipt", "manifest"));
			return {};
		}
	} // namespace

	sdk::result<void>
	materialization_v4_store_source::validate(const sdk::relation_engine& engine) const
	{
		if (auto valid = validate_receipt_shape(receipt_); !valid)
			return valid;
		if (auto valid = validate_authority(engine, receipt_, authority_); !valid)
			return valid;
		const bool grouped = std::ranges::any_of(partitions_,
												 [](const materialization_v4_store_partition& value)
												 {
													 return !value.has_receipt;
												 });
		if (!grouped && partitions_.size() != receipt_.task_count)
			return sdk::unexpected(mismatch("partitions", "receipt.task-count"));
		if (grouped && (receipt_.task_count != 1U || receipt_.task_receipts.size() != 1U))
			return sdk::unexpected(mismatch("partitions", "grouped-task-receipt"));

		std::uint64_t claim_count{};
		std::uint64_t unresolved_count{};
		std::uint64_t conflict_count{};
		std::uint64_t differential_disagreement_count{};
		std::set<std::string, std::less<>> partition_ids;
		for (std::size_t index{}; index < partitions_.size(); ++index)
		{
			const auto& value = partitions_[index];
			if (grouped)
			{
				if (value.has_receipt)
				{
					if (auto valid =
							validate_grouped_output_receipt(value, receipt_.task_receipts.front());
						!valid)
						return valid;
				}
			}
			else
			{
				if (value.receipt != receipt_.task_receipts[index])
					return sdk::unexpected(mismatch("partition.receipt", "aggregate"));
				if (auto valid =
						validate_partition_receipt(value, receipt_.task_receipts[index], index);
					!valid)
					return valid;
			}
			if (!partition_ids.insert(value.manifest.partition_id).second)
				return sdk::unexpected(mismatch("partition.partition-id", "duplicate"));
			if (value.draft.condition.universe != authority_.snapshot.series.condition_universe_id)
				return sdk::unexpected(mismatch("partition.condition-universe", "snapshot"));

			auto manifest = sdk::make_partition_manifest(engine, value.draft);
			if (!manifest)
				return sdk::unexpected(std::move(manifest.error()));
			if (*manifest != value.manifest)
				return sdk::unexpected(mismatch("partition.manifest", "recomputed"));
			auto subject = sdk::make_partition_certificate_subject(value.manifest, value.binding);
			if (!subject)
				return sdk::unexpected(std::move(subject.error()));
			if (value.has_receipt)
			{
				if (auto valid =
						add_checked(claim_count, value.receipt.claim_count, "receipt.claim-count");
					!valid)
					return valid;
				if (auto valid = add_checked(unresolved_count,
											 value.receipt.unresolved_count,
											 "receipt.unresolved-count");
					!valid)
					return valid;
				if (auto valid = add_checked(
						conflict_count, value.receipt.conflict_count, "receipt.conflict-count");
					!valid)
					return valid;
				if (auto valid = add_checked(differential_disagreement_count,
											 value.receipt.differential_disagreement_count,
											 "receipt.differential-disagreement-count");
					!valid)
					return valid;
			}
		}

		if (!grouped &&
			(claim_count != receipt_.claim_count || unresolved_count != receipt_.unresolved_count ||
			 conflict_count != receipt_.conflict_count ||
			 differential_disagreement_count != receipt_.differential_disagreement_count))
			return sdk::unexpected(mismatch("receipt.counters", "partitions"));

		std::set<std::string, std::less<>> closure_ids;
		for (const auto& closure : authority_.closures)
		{
			const auto partition =
				std::ranges::find(partitions_,
								  closure.subject_partition_id,
								  [](const materialization_v4_store_partition& value)
								  {
									  return value.manifest.partition_id;
								  });
			if (partition == partitions_.end())
				return sdk::unexpected(mismatch("closure.subject-partition", "missing"));
			auto subject =
				sdk::make_partition_certificate_subject(partition->manifest, partition->binding);
			if (!subject)
				return sdk::unexpected(std::move(subject.error()));
			auto certificate = sdk::make_closure_certificate(*subject, closure);
			if (!certificate)
				return sdk::unexpected(std::move(certificate.error()));
			if (!closure_ids.insert(certificate->id).second)
				return sdk::unexpected(mismatch("closure.id", "duplicate"));
		}
		return {};
	}

	sdk::result<materialization_v4_store_source> make_materialization_v4_store_source(
		const sdk::relation_engine& engine,
		const materialization_v4_incremental_receipt& receipt,
		const std::span<const materialization_v4_claim_sealed* const> sealed_tasks,
		materialization_v4_provider_output_authority authority)
	{
		auto admitted = admit_materialization_v4_store_ingress(
			engine, receipt, sealed_tasks, authority.publication);
		if (!admitted)
			return sdk::unexpected(std::move(admitted.error()));
		if (authority.materialization_request_id != receipt.materialization_request_id)
			return sdk::unexpected(mismatch("authority.materialization-request-id", "receipt"));

		std::vector<materialization_v4_store_partition> partitions;
		partitions.reserve(sealed_tasks.size());
		for (std::size_t index{}; index < sealed_tasks.size(); ++index)
		{
			const auto* sealed = sealed_tasks[index];
			if (sealed == nullptr)
				return sdk::unexpected(invalid("sealed-tasks", "null"));
			partitions.push_back({sealed->translation.binding.task_index,
								  sealed->translation.partition,
								  sealed->partition_manifest,
								  sealed->partition_binding,
								  sealed->receipt});
		}

		materialization_v4_store_source source{
			std::move(authority), receipt, std::move(partitions)};
		if (auto valid = source.validate(engine); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return source;
	}

	sdk::result<materialization_v4_store_source> make_materialization_v4_grouped_store_source(
		const sdk::relation_engine& engine,
		const materialization_v4_incremental_receipt& receipt,
		const std::span<const materialization_v4_claim_sealed* const> sealed_tasks,
		const std::span<const sdk::partition_draft> base_partitions,
		const std::span<const sdk::claim> existing,
		materialization_v4_provider_output_authority authority)
	{
		if (sealed_tasks.empty() || base_partitions.empty())
			return sdk::unexpected(invalid("grouped-source", "empty"));
		if (receipt.task_count != 1U || receipt.task_receipts.size() != 1U)
			return sdk::unexpected(mismatch("grouped-source", "task-receipt"));
		const std::array<const materialization_v4_claim_sealed*, 1U> receipt_task{
			sealed_tasks.front()};
		if (auto admitted = admit_materialization_v4_store_ingress(
				engine, receipt, receipt_task, authority.publication, existing);
			!admitted)
			return sdk::unexpected(std::move(admitted.error()));
		if (authority.materialization_request_id != receipt.materialization_request_id)
			return sdk::unexpected(mismatch("authority.materialization-request-id", "receipt"));

		std::vector<materialization_v4_store_partition> partitions;
		partitions.reserve(base_partitions.size() + sealed_tasks.size());
		for (const auto& draft : base_partitions)
		{
			if (draft.claims.empty())
				return sdk::unexpected(invalid("grouped-source.base-partition", "empty"));
			auto manifest = sdk::make_partition_manifest(engine, draft);
			if (!manifest)
				return sdk::unexpected(std::move(manifest.error()));
			// Base partitions are already canonical claims produced by the host-side planner. Their
			// source identity is re-derived here, but they deliberately do not masquerade as worker
			// task receipts.
			sdk::snapshot_partition_binding binding{manifest->partition_id,
													draft.relation_descriptor_id,
													draft.scope,
													draft.condition,
													draft.interpretation,
													draft.producer_semantics,
													draft.producer_input_basis_digest,
													draft.precision_profile,
													draft.assumption_set_id};
			partitions.push_back({0U, draft, *manifest, std::move(binding), {}, false});
		}
		for (const auto* sealed : sealed_tasks)
		{
			if (sealed == nullptr)
				return sdk::unexpected(invalid("grouped-source.sealed-tasks", "null"));
			if (auto valid = validate_materialization_v4_claim_receipt(engine, *sealed, existing);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			if (sealed->receipt.materialization_request_id != receipt.materialization_request_id ||
				sealed->receipt.task_index != receipt.task_receipts.front().task_index ||
				sealed->receipt.task_id != receipt.task_receipts.front().task_id ||
				sealed->receipt.task_v4_digest != receipt.task_receipts.front().task_v4_digest)
				return sdk::unexpected(mismatch("grouped-source.task-binding", "receipt"));
			partitions.push_back({sealed->translation.binding.task_index,
								  sealed->translation.partition,
								  sealed->partition_manifest,
								  sealed->partition_binding,
								  sealed->receipt,
								  true});
		}

		materialization_v4_store_source source{
			std::move(authority), receipt, std::move(partitions)};
		if (auto valid = source.validate(engine); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return source;
	}

	sdk::result<materialization_v4_store_publication>
	publish_materialization_v4_store_source(const sdk::relation_engine& engine,
											sdk::snapshot_store& store,
											materialization_v4_store_source source)
	{
		if (auto valid = source.validate(engine); !valid)
			return sdk::unexpected(std::move(valid.error()));
		const auto output_authority = source.authority_;
		const auto output_receipt = source.receipt_;

		auto writer = store.begin(std::move(source.authority_.snapshot));
		if (!writer)
			return sdk::unexpected(std::move(writer.error()));
		for (auto& partition : source.partitions_)
		{
			if (auto staged = writer->stage(std::move(partition.draft)); !staged)
				return sdk::unexpected(std::move(staged.error()));
		}
		for (auto& closure : source.authority_.closures)
		{
			if (auto staged = writer->add_closure(std::move(closure)); !staged)
				return sdk::unexpected(std::move(staged.error()));
		}
		if (auto valid = writer->validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		auto published = writer->publish();
		if (!published)
			return sdk::unexpected(std::move(published.error()));
		return materialization_v4_store_publication{
			std::move(*published), output_authority, output_receipt, {}, 0U};
	}
} // namespace cxxlens::detail::clang22::materialization
