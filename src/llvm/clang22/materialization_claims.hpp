#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/claim.hpp>
#include <cxxlens/sdk/store.hpp>

#include "materialization_request.hpp"
#include "materialization_request_v2_1.hpp"
#include "materialization_seal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/** One installed authority file measured at the active, possibly relocated, prefix. */
	struct materialization_authority_binding
	{
		std::string path;
		std::string content_digest;
		[[nodiscard]] bool operator==(const materialization_authority_binding&) const = default;
	};

	/**
	 * Explicit semantic producer authority supplied by the installation-binding phase.
	 *
	 * Physical executable digests, package configuration, and prefix occurrence fields are
	 * intentionally not representable here. They remain report correlation only.
	 */
	struct materialization_producer_authority
	{
		std::string executable;
		std::string interface_version;
		std::string distribution_version;
		std::string source_revision;
		std::string source_tree;
		std::vector<materialization_authority_binding> authority_bindings;
	};

	/** Report-owned semantic guarantee inputs; no value is inferred from terminal prose. */
	struct materialization_guarantee_authority
	{
		std::vector<std::string> assumptions;
		std::vector<std::string> verification_modalities;
	};

	/**
	 * Source-private request authority for bounded v2.1 claim adoption.
	 *
	 * This value owns only request-wide semantic digests and typed authority.  It deliberately
	 * contains no task vector, source receipt, task.v3 payload, result map, or claim occurrence.
	 * Its shared lifetime token follows the move-only admitted request and is cleared on request
	 * destruction, so every one-task adoption call fails closed after owner invalidation.  Catalog
	 * and engine views are derived from the current owner; no raw owner pointer is retained.
	 */
	class materialization_v2_1_claim_authority
	{
	  public:
		[[nodiscard]] validated_materialization_request_v2_1* request() const noexcept;
		[[nodiscard]] const sdk::project_catalog* catalog() const noexcept;
		[[nodiscard]] const sdk::relation_engine* engine() const noexcept;
		[[nodiscard]] const std::string& materialization_request_id() const noexcept;
		[[nodiscard]] std::uint64_t task_count() const noexcept;
		[[nodiscard]] const std::string& worker_provider_id() const noexcept;
		[[nodiscard]] const std::string& worker_semantic_contract_digest() const noexcept;
		[[nodiscard]] const std::string& materializer_semantics_digest() const noexcept;
		[[nodiscard]] const std::string& direct_basis_digest() const noexcept;
		[[nodiscard]] const std::string& canonical_adoption_transform_digest() const noexcept;
		[[nodiscard]] const std::string& base_ingestion_transform_digest() const noexcept;
		[[nodiscard]] const sdk::claim_guarantee& guarantee() const noexcept;
		[[nodiscard]] const std::string& assumption_set_id() const noexcept;

	  private:
		materialization_v2_1_claim_authority(
			std::shared_ptr<materialization_v2_1_request_lifetime> lifetime,
			std::string materialization_request_id,
			std::uint64_t task_count,
			std::string worker_provider_id,
			std::string worker_semantic_contract_digest,
			std::string materializer_semantics_digest,
			std::string direct_basis_digest,
			std::string canonical_adoption_transform_digest,
			std::string base_ingestion_transform_digest,
			sdk::claim_guarantee guarantee,
			std::string assumption_set_id);

		std::shared_ptr<materialization_v2_1_request_lifetime> lifetime_;
		std::string materialization_request_id_;
		std::uint64_t task_count_{};
		std::string worker_provider_id_;
		std::string worker_semantic_contract_digest_;
		std::string materializer_semantics_digest_;
		std::string direct_basis_digest_;
		std::string canonical_adoption_transform_digest_;
		std::string base_ingestion_transform_digest_;
		sdk::claim_guarantee guarantee_;
		std::string assumption_set_id_;

		friend sdk::result<materialization_v2_1_claim_authority>
		make_materialization_v2_1_claim_authority(
			validated_materialization_request_v2_1& request,
			const materialization_producer_authority& producer_authority,
			const materialization_guarantee_authority& guarantee_authority);
	};

	/** Exact seven-field task context with physical provider execution deliberately absent. */
	struct materialization_semantic_task_context
	{
		std::string provider_task_id;
		std::string task_input_digest;
		std::string selected_catalog_compile_unit_id;
		std::string compile_unit_id;
		std::string condition_universe_id;
		std::string condition_id;
		std::string interpretation_domain;
		[[nodiscard]] bool operator==(const materialization_semantic_task_context&) const = default;
	};

	/** One hidden precursor or Store-visible final SDK claim occurrence. */
	struct materialization_claim_envelope
	{
		std::string role;
		std::string row_ref;
		std::string claim_ref;
		std::string sdk_singleton_claim_batch_digest;
		sdk::claim value;
	};

	/** Exact hidden-precursor to stored-final canonicalization edge. */
	struct materialization_canonicalization_edge
	{
		std::string precursor_claim_ref;
		std::string final_claim_ref;
		std::string transform_semantics;
		[[nodiscard]] bool operator==(const materialization_canonicalization_edge&) const = default;
	};

	/** Lossless task/row/evidence association kept separate from SDK occurrence identity. */
	struct materialization_origin_association
	{
		std::string association_id;
		std::string stored_claim_ref;
		materialization_semantic_task_context originating_task;
		std::string sealed_row_digest;
		std::optional<std::string> source_evidence_digest;
		[[nodiscard]] bool operator==(const materialization_origin_association&) const = default;
	};

	/** One exact eight-field Store partition and its report-facing occurrence census. */
	struct materialization_claim_partition
	{
		sdk::partition_draft draft;
		sdk::partition_manifest manifest;
		sdk::snapshot_partition_binding binding;
		std::vector<std::string> stored_claim_refs;
		std::vector<std::string> claim_content_ids;
		std::uint64_t sdk_claim_occurrence_count{};
		std::uint64_t origin_association_count{};
		bool empty_partition{};
	};

	/**
	 * Source-private request authority retained by sealed claims.
	 *
	 * The request ID binds the validated semantic request projection, while the catalog identity
	 * and exact task count keep Store adoption from pairing this claim set with a coherent but
	 * different request that happens to have compatible mutable projections.
	 */
	struct materialization_claim_request_binding
	{
		std::string materialization_request_id;
		std::string catalog_id;
		std::string catalog_digest;
		std::uint64_t task_count{};

		[[nodiscard]] bool operator==(const materialization_claim_request_binding&) const = default;
	};

	/**
	 * One bounded task adoption result. The contained drafts are limited to the currently consumed
	 * sealed task; callers must transfer them to a replayable source before advancing the task
	 * cursor. This type deliberately has no claim_batch or request-wide result-set member.
	 */
	struct materialization_bounded_task_claims
	{
		/** Caller-visible ordinal; adoption also checks the sealed source-private copy below. */
		std::uint64_t canonical_task_index{};
		std::string materializer_semantics_digest;
		std::string direct_basis_digest;
		std::string canonical_adoption_transform_digest;
		std::string base_ingestion_transform_digest;
		std::string assumption_set_id;
		std::vector<materialization_claim_envelope> claim_envelopes;
		std::vector<materialization_canonicalization_edge> canonicalization_edges;
		std::vector<materialization_origin_association> origin_associations;
		std::vector<materialization_claim_partition> partitions;

		/** Source-private request-entry seal issued with the validated task/result pair. */
		[[nodiscard]] std::uint64_t sealed_canonical_task_index() const noexcept
		{
			return sealed_canonical_task_index_;
		}
		[[nodiscard]] std::string_view sealed_request_entry_binding_digest() const noexcept
		{
			return sealed_request_entry_binding_digest_;
		}

		materialization_bounded_task_claims(
			const std::uint64_t canonical_task_index,
			std::string sealed_request_entry_binding_digest,
			std::string materializer_semantics_digest,
			std::string direct_basis_digest,
			std::string canonical_adoption_transform_digest,
			std::string base_ingestion_transform_digest,
			std::string assumption_set_id,
			std::vector<materialization_claim_envelope> claim_envelopes,
			std::vector<materialization_canonicalization_edge> canonicalization_edges,
			std::vector<materialization_origin_association> origin_associations,
			std::vector<materialization_claim_partition> partitions)
			: canonical_task_index{canonical_task_index},
			  materializer_semantics_digest{std::move(materializer_semantics_digest)},
			  direct_basis_digest{std::move(direct_basis_digest)},
			  canonical_adoption_transform_digest{std::move(canonical_adoption_transform_digest)},
			  base_ingestion_transform_digest{std::move(base_ingestion_transform_digest)},
			  assumption_set_id{std::move(assumption_set_id)},
			  claim_envelopes{std::move(claim_envelopes)},
			  canonicalization_edges{std::move(canonicalization_edges)},
			  origin_associations{std::move(origin_associations)},
			  partitions{std::move(partitions)}, sealed_canonical_task_index_{canonical_task_index},
			  sealed_request_entry_binding_digest_{std::move(sealed_request_entry_binding_digest)}
		{
		}

		materialization_bounded_task_claims(const materialization_bounded_task_claims&) = delete;
		materialization_bounded_task_claims&
		operator=(const materialization_bounded_task_claims&) = delete;
		materialization_bounded_task_claims(materialization_bounded_task_claims&&) noexcept =
			default;
		materialization_bounded_task_claims&
		operator=(materialization_bounded_task_claims&&) noexcept = default;

	  private:
		std::uint64_t sealed_canonical_task_index_{};
		std::string sealed_request_entry_binding_digest_;
	};

	/**
	 * Source-private streaming adoption boundary. The loader exposes at most one live sealed
	 * result; the coordinator resets that owner before requesting the next task and after claim
	 * adoption.
	 */
	using materialization_task_result_loader =
		std::function<sdk::result<std::reference_wrapper<const sealed_materialization_result>>(
			std::size_t)>;

	/**
	 * Immutable, move-only output of complete claim construction and one atomic batch commit.
	 * Partitions are in deterministic manifest order and contain final claims only.
	 */
	class sealed_materialization_claims
	{
	  public:
		sealed_materialization_claims(const sealed_materialization_claims&) = delete;
		sealed_materialization_claims& operator=(const sealed_materialization_claims&) = delete;
		sealed_materialization_claims(sealed_materialization_claims&&) noexcept = default;
		sealed_materialization_claims&
		operator=(sealed_materialization_claims&&) noexcept = default;
		~sealed_materialization_claims() = default;

		[[nodiscard]] std::string_view materializer_semantics_digest() const noexcept;
		[[nodiscard]] std::string_view direct_basis_digest() const noexcept;
		[[nodiscard]] std::string_view canonical_adoption_transform_digest() const noexcept;
		[[nodiscard]] std::string_view base_ingestion_transform_digest() const noexcept;
		[[nodiscard]] std::string_view assumption_set_id() const noexcept;
		[[nodiscard]] const sdk::claim_batch_result& final_claim_batch() const noexcept;
		[[nodiscard]] std::span<const materialization_claim_envelope>
		claim_envelopes() const noexcept;
		[[nodiscard]] std::span<const materialization_canonicalization_edge>
		canonicalization_edges() const noexcept;
		[[nodiscard]] std::span<const materialization_origin_association>
		origin_associations() const noexcept;
		[[nodiscard]] std::span<const materialization_claim_partition> partitions() const noexcept;

	  private:
		sealed_materialization_claims(
			materialization_claim_request_binding request_binding,
			std::string materializer_semantics_digest,
			std::string direct_basis_digest,
			std::string canonical_adoption_transform_digest,
			std::string base_ingestion_transform_digest,
			std::string assumption_set_id,
			sdk::claim_batch_result final_claim_batch,
			std::vector<materialization_claim_envelope> claim_envelopes,
			std::vector<materialization_canonicalization_edge> canonicalization_edges,
			std::vector<materialization_origin_association> origin_associations,
			std::vector<materialization_claim_partition> partitions);

		materialization_claim_request_binding request_binding_;
		std::string materializer_semantics_digest_;
		std::string direct_basis_digest_;
		std::string canonical_adoption_transform_digest_;
		std::string base_ingestion_transform_digest_;
		std::string assumption_set_id_;
		sdk::claim_batch_result final_claim_batch_;
		std::vector<materialization_claim_envelope> claim_envelopes_;
		std::vector<materialization_canonicalization_edge> canonicalization_edges_;
		std::vector<materialization_origin_association> origin_associations_;
		std::vector<materialization_claim_partition> partitions_;

		friend sdk::result<void> validate_materialization_claim_request_binding(
			const validated_materialization_request& request,
			const sealed_materialization_claims& claims);

		friend sdk::result<sealed_materialization_claims> construct_materialization_claims(
			const validated_materialization_request& request,
			std::span<const sealed_materialization_result> task_results,
			const materialization_producer_authority& producer_authority,
			const materialization_guarantee_authority& guarantee_authority);
		friend sdk::result<sealed_materialization_claims>
		construct_materialization_claims_from_loader(
			const validated_materialization_request& request,
			const materialization_task_result_loader& load,
			const materialization_producer_authority& producer_authority,
			const materialization_guarantee_authority& guarantee_authority);
	};

	/** Derive the exact source-private request binding retained by sealed claims and bounded
	 * sources. */
	[[nodiscard]] sdk::result<materialization_claim_request_binding>
	make_materialization_claim_request_binding(const validated_materialization_request& request);

	/** Validate a sealed claim set against the candidate request before Store adoption. */
	[[nodiscard]] sdk::result<void>
	validate_materialization_claim_request_binding(const validated_materialization_request& request,
												   const sealed_materialization_claims& claims);

	/** Construct, globally validate, and partition every final occurrence exactly once. */
	[[nodiscard]] sdk::result<sealed_materialization_claims> construct_materialization_claims(
		const validated_materialization_request& request,
		std::span<const sealed_materialization_result> task_results,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority);

	[[nodiscard]] sdk::result<sealed_materialization_claims>
	construct_materialization_claims_from_loader(
		const validated_materialization_request& request,
		const materialization_task_result_loader& load,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority);

	/**
	 * Independently construct final typed claims for exactly one sealed task.
	 *
	 * This is the production adoption primitive. It performs field/identity/reference/conflict
	 * validation without constructing or committing an sdk::claim_batch. The returned value is a
	 * task-window object and must be spooled or consumed before the next task is loaded.
	 */
	[[nodiscard]] sdk::result<materialization_bounded_task_claims>
	construct_materialization_bounded_task_claims(
		const validated_materialization_request& request,
		std::size_t task_index,
		const sealed_materialization_result& result,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority);

	/** Build one request-wide v2.1 claim authority without retaining task/source occurrences. */
	[[nodiscard]] sdk::result<materialization_v2_1_claim_authority>
	make_materialization_v2_1_claim_authority(
		validated_materialization_request_v2_1& request,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority);

	/** Adopt exactly one cursor-owned v2.1 task window into the bounded claim stream. */
	[[nodiscard]] sdk::result<materialization_bounded_task_claims>
	construct_materialization_bounded_task_claims(
		const materialization_v2_1_claim_authority& authority,
		std::size_t task_index,
		const materialization_v2_1_task_execution& task,
		const sealed_materialization_result& result);
} // namespace cxxlens::detail::clang22::materialization
