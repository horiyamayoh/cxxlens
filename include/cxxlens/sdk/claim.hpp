#pragma once

/** @file claim.hpp @brief Claim identity, stage validation, reference staging, and merge laws. */

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <cxxlens/sdk/relation.hpp>

namespace cxxlens::sdk
{
	/** @brief Canonical finite condition fragments within one explicit universe. */
	struct claim_condition
	{
		std::string universe;
		std::vector<std::string> fragments;
		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string canonical_form() const;
		[[nodiscard]] std::string id() const;
		[[nodiscard]] result<std::vector<std::string>> overlap(const claim_condition& other) const;
		[[nodiscard]] bool operator==(const claim_condition&) const = default;
	};

	/** @brief Claim processing stage; observation deliberately remains outside this enum. */
	enum class claim_stage : std::uint8_t
	{
		assertion,
		canonical_claim,
		derived_claim,
	};
	/** @brief Test exact membership in the closed claim stage enum. */
	[[nodiscard]] constexpr bool is_valid(const claim_stage value) noexcept
	{
		return value >= claim_stage::assertion && value <= claim_stage::derived_claim;
	}

	/** @brief Producer identity split from its semantic contract version. */
	struct claim_producer
	{
		std::string id;
		std::string semantic_contract;
		[[nodiscard]] bool operator==(const claim_producer&) const = default;
	};

	/** @brief Direct observation basis, independent from snapshot identity. */
	struct direct_claim_basis
	{
		std::string basis_digest;
		[[nodiscard]] bool operator==(const direct_claim_basis&) const = default;
	};

	/** @brief Explicit basis for a claim derived from immutable partition content. */
	struct derived_claim_basis
	{
		std::string input_snapshot;
		std::vector<std::string> consumed_partition_content_digests;
		std::string transform_semantics;
		[[nodiscard]] bool operator==(const derived_claim_basis&) const = default;
	};

	/** @brief Tagged direct/derived input basis from claim-envelope v2. */
	using claim_input_basis = std::variant<direct_claim_basis, derived_claim_basis>;
	/** @brief Derive the exact producer-input basis digest used by partition identity. */
	[[nodiscard]] result<std::string> claim_input_basis_digest(const claim_input_basis& basis);

	/** @brief Machine-readable semantic guarantee that must survive ingestion. */
	struct claim_guarantee
	{
		std::string approximation;
		std::string scope;
		std::string assumptions;
		std::vector<std::string> verification_modalities;
		[[nodiscard]] result<void> validate() const;
	};

	/** @brief Provider-owned observation before claim identity is assigned. */
	struct observation
	{
		detached_row row;
		claim_condition presence;
		std::string interpretation;
		claim_producer producer;
		direct_claim_basis input_basis;
		std::string provenance_root;
		claim_guarantee guarantee;
	};

	/**
	 * @brief System claim envelope v2 paired with its detached semantic row.
	 *
	 * The complete envelope is one evidence occurrence and its semantic subject. There is no
	 * detached evidence-ID reference collection to reconcile.
	 */
	struct claim
	{
		detached_row row;
		std::string descriptor;
		std::string semantic_key;
		std::string assertion;
		std::string content;
		claim_condition presence;
		std::string interpretation;
		claim_stage stage{claim_stage::assertion};
		claim_producer producer;
		claim_input_basis input_basis;
		std::string provenance_root;
		claim_guarantee guarantee;
	};

	/** @brief Validate an observation and create an order-independent assertion identity. */
	[[nodiscard]] result<claim> make_assertion(const relation_engine& engine, observation value);
	/** @brief Independently revalidate and promote one assertion through a canonicalizer. */
	[[nodiscard]] result<claim> make_canonical_claim(const relation_engine& engine,
													 const claim& input,
													 claim_producer canonicalizer,
													 detached_row canonical_row,
													 const std::string& transform_semantics);
	/** @brief Validate and construct a derived claim from explicit immutable inputs. */
	[[nodiscard]] result<claim>
	make_derived_claim(const relation_engine& engine,
					   std::span<const claim> inputs,
					   observation output,
					   std::string input_snapshot,
					   std::vector<std::string> consumed_partition_content_digests,
					   std::string transform_semantics);
	/** @brief Independently revalidate stage, row, basis, and all three identities. */
	[[nodiscard]] result<void> validate_claim(const relation_engine& engine, const claim& value);

	/** @brief Preserved soft-reference failure, never collapsed into an empty result. */
	struct unresolved_reference
	{
		std::string source_assertion;
		std::string source_relation;
		std::string target_relation;
		std::vector<std::string> source_columns;
		std::string reason;
	};

	/** @brief Same-domain functional disagreement over only the overlapping condition. */
	struct claim_conflict
	{
		std::string relation;
		std::string semantic_key;
		std::string interpretation;
		std::vector<std::string> overlap_fragments;
		std::vector<std::string> assertions;
		std::vector<std::string> contents;
	};

	/** @brief Cross-domain disagreement, intentionally distinct from a claim conflict. */
	struct differential_disagreement
	{
		std::string relation;
		std::string semantic_key;
		std::string left_interpretation;
		std::string right_interpretation;
		std::string left_content;
		std::string right_content;
		std::vector<std::string> overlap_fragments;
	};

	/** @brief Atomic batch result with accepted claims and explicit partiality records. */
	struct claim_batch_result
	{
		/**
		 * @brief Canonical semantic claims with every distinct self-contained occurrence preserved.
		 */
		std::vector<claim> claims;
		std::vector<unresolved_reference> unresolved;
		std::vector<claim_conflict> conflicts;
		std::vector<differential_disagreement> differential_disagreements;
		std::string content_digest;
	};

	/**
	 * @brief Encode all canonical batch result records as one typed, self-delimiting tuple.
	 * @return `sdk.canonical-value-invalid` if any nested semantic string is invalid UTF-8.
	 */
	[[nodiscard]] result<std::vector<std::byte>> claim_batch_content_encoding(
		std::span<const claim> claims,
		std::span<const unresolved_reference> unresolved,
		std::span<const claim_conflict> conflicts,
		std::span<const differential_disagreement> differential_disagreements);

	/**
	 * @brief Derive the typed, self-delimiting identity of all canonical batch result records.
	 * @return `sdk.canonical-value-invalid` if any nested semantic string is invalid UTF-8.
	 */
	[[nodiscard]] result<std::string> claim_batch_content_digest(
		std::span<const claim> claims,
		std::span<const unresolved_reference> unresolved,
		std::span<const claim_conflict> conflicts,
		std::span<const differential_disagreement> differential_disagreements);

	/** @brief Provider-facing atomic batch ingestor with independent stage/reference validation. */
	class claim_batch
	{
	  public:
		/** @brief Validate and ingest one provider observation through the assertion stage. */
		[[nodiscard]] result<void> add_observation(const relation_engine& engine,
												   observation value);
		/** @brief Add an already validated or derived claim for independent commit validation. */
		[[nodiscard]] result<void> add(claim value);
		/**
		 * @brief Validate references/merge laws against new and existing claims and produce the
		 * batch.
		 */
		[[nodiscard]] result<claim_batch_result> commit(const relation_engine& engine,
														std::span<const claim> existing = {}) &&;

	  private:
		std::vector<claim> claims_;
	};
} // namespace cxxlens::sdk
