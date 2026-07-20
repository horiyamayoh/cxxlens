#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/claim.hpp>
#include <cxxlens/sdk/store.hpp>

#include "materialization_request.hpp"
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

		friend sdk::result<sealed_materialization_claims> construct_materialization_claims(
			const validated_materialization_request& request,
			std::span<const sealed_materialization_result> task_results,
			const materialization_producer_authority& producer_authority,
			const materialization_guarantee_authority& guarantee_authority);
	};

	/** Construct, globally validate, and partition every final occurrence exactly once. */
	[[nodiscard]] sdk::result<sealed_materialization_claims> construct_materialization_claims(
		const validated_materialization_request& request,
		std::span<const sealed_materialization_result> task_results,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority);
} // namespace cxxlens::detail::clang22::materialization
