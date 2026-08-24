#pragma once

/**
 * @file materialization_store_v6_typed_ingress.hpp
 * @brief Lossless task-v4 semantic authority for the bounded Store v6 pipeline.
 *
 * This boundary deliberately stops before a Store backend or snapshot payload codec.  The
 * task cursor yields value-owned SDK semantics one record at a time; it never exposes a raw
 * CXLPEV01 frame, a snapshot_writer, or a Store pointer.  A separately sealed expected cursor is
 * generated from the request/journal side and is suitable for the v6 comparator only after the
 * production coordinator consumes the opaque authority returned here.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <cxxlens/sdk/store.hpp>

#include "materialization_claims.hpp"
#include "materialization_request_v2_2.hpp"
#include "materialization_v4_execution_journal.hpp"
#include "materialization_v4_store_source.hpp"
#include "provider_task_v4_authority.hpp"
#include "provider_worker_v4.hpp"
#include "provider_worker_v4_output_normalizer.hpp"
#include "sdk/bounded_store_v6_internal.hpp"
#include "sdk/provider_runtime_internal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	inline constexpr std::string_view materialization_store_v6_typed_ingress_schema =
		"cxxlens.clang22.materialization-store-v6-typed-ingress.v1";
	inline constexpr std::uint64_t materialization_store_v6_max_tasks =
		sdk::detail::bounded_store_v6_max_tasks;

	/** Full partition identity observed before any member event. */
	struct materialization_store_v6_partition_begin
	{
		std::uint64_t task_index{};
		std::string task_id;
		std::string task_authority_digest;
		sdk::snapshot_partition_binding binding;
	};

	/** One metadata-preserving claim occurrence; semantic-content deduplication is separate. */
	struct materialization_store_v6_claim_occurrence
	{
		std::uint64_t task_index{};
		std::string task_id;
		std::string partition_id;
		std::string claim_ref;
		sdk::claim value;
	};

	/** One unique detached row inside a partition. */
	struct materialization_store_v6_detached_row
	{
		std::uint64_t task_index{};
		std::string task_id;
		std::string partition_id;
		std::string row_digest;
		sdk::detached_row value;
	};

	/** One origin association. Multiple origins for one claim remain distinct records. */
	struct materialization_store_v6_claim_annotation
	{
		std::uint64_t task_index{};
		std::string task_id;
		std::string partition_id;
		materialization_origin_association association;
		sdk::claim occurrence;
	};

	/**
	 * Exact hidden-precursor to stored-final edge retained outside the Store event grammar.
	 *
	 * The Store v6 physical projection has no canonicalization-edge record kind.  The task token
	 * therefore replays this sideband separately, while its sealed digest remains part of the same
	 * request authority.  Consumers must not replace it with a claim provenance root.
	 */
	struct materialization_store_v6_canonicalization_edge_event
	{
		std::uint64_t task_index{};
		std::string task_id;
		std::uint64_t batch_index{};
		std::uint64_t row_index{};
		materialization_canonicalization_edge edge;
		sdk::claim hidden_precursor;
	};

	struct materialization_store_v6_coverage
	{
		std::uint64_t task_index{};
		std::string task_id;
		std::string partition_id;
		sdk::snapshot_coverage_unit value;
	};

	struct materialization_store_v6_unresolved
	{
		std::uint64_t task_index{};
		std::string task_id;
		std::string partition_id;
		sdk::unresolved_reference value;
	};

	/** Closed partition identity and independently counted semantic classes. */
	struct materialization_store_v6_partition_end
	{
		std::uint64_t task_index{};
		std::string task_id;
		sdk::partition_manifest manifest;
		std::uint64_t claim_occurrence_count{};
		std::uint64_t unique_claim_content_count{};
		std::uint64_t unique_row_count{};
		std::uint64_t annotation_count{};
		std::uint64_t coverage_count{};
		std::uint64_t unresolved_count{};
		std::string semantic_partition_digest;

		[[nodiscard]] bool
		operator==(const materialization_store_v6_partition_end&) const = default;
	};

	using materialization_store_v6_typed_event =
		std::variant<materialization_store_v6_partition_begin,
					 materialization_store_v6_claim_occurrence,
					 materialization_store_v6_detached_row,
					 materialization_store_v6_claim_annotation,
					 materialization_store_v6_coverage,
					 materialization_store_v6_unresolved,
					 materialization_store_v6_partition_end>;

	/** Structural counts over the source-private typed stream, before backend framing. */
	struct materialization_store_v6_structural_census
	{
		std::uint64_t task_count{};
		std::uint64_t partition_count{};
		std::uint64_t event_count{};
		std::uint64_t claim_occurrence_count{};
		std::uint64_t unique_row_count{};
		std::uint64_t annotation_count{};
		std::uint64_t coverage_count{};
		std::uint64_t unresolved_count{};
		std::uint64_t canonical_source_bytes{};
		std::array<std::byte, 32U> canonical_source_sha256{};
		std::uint64_t canonicalization_edge_count{};
		std::uint64_t canonicalization_edge_bytes{};
		std::array<std::byte, 32U> canonicalization_edge_sha256{};

		[[nodiscard]] bool
		operator==(const materialization_store_v6_structural_census&) const = default;
	};

	/** Independent semantic census. C, occurrence, row, and annotation counts are never aliased. */
	struct materialization_store_v6_semantic_census
	{
		std::uint64_t unique_claim_content_count{};
		std::uint64_t normalized_descriptor_batch_count{};
		std::uint64_t normalized_row_count{};
		std::uint64_t closure_candidate_count{};
		std::uint64_t canonicalization_edge_count{};
		std::string request_digest;
		std::string task_authority_set_digest;
		std::string provider_output_digest;
		std::string claim_occurrence_digest;
		std::string unique_claim_content_digest;
		std::string unique_row_digest;
		std::string origin_association_digest;
		std::string canonicalization_edge_digest;
		std::string coverage_digest;
		std::string unresolved_digest;
		std::string closure_digest;
		std::string journal_digest;
		std::string snapshot_authority_digest;
		std::string semantic_input_digest;

		[[nodiscard]] bool
		operator==(const materialization_store_v6_semantic_census&) const = default;
	};

	/** Closed task receipt over the canonical typed spool, not a Store payload/frame receipt. */
	struct materialization_store_v6_typed_task_receipt
	{
		std::string task_id;
		std::uint64_t ordinal{};
		std::uint64_t partition_count{};
		std::uint64_t event_count{};
		std::uint64_t claim_occurrence_count{};
		std::uint64_t unique_claim_content_count{};
		std::uint64_t unique_row_count{};
		std::uint64_t annotation_count{};
		std::uint64_t coverage_count{};
		std::uint64_t unresolved_count{};
		std::uint64_t canonical_source_bytes{};
		std::array<std::byte, 32U> canonical_source_sha256{};
		std::uint64_t canonicalization_edge_count{};
		std::uint64_t canonicalization_edge_bytes{};
		std::array<std::byte, 32U> canonicalization_edge_sha256{};
		std::string immutable_authority_binding;

		[[nodiscard]] bool
		operator==(const materialization_store_v6_typed_task_receipt&) const = default;
	};

	/** One exact canonical-batch row to hidden/final edge mapping supplied by claim sealing. */
	struct materialization_store_v6_canonicalization_input
	{
		std::uint64_t batch_index{};
		std::uint64_t row_index{};
		materialization_canonicalization_edge edge;
		sdk::claim hidden_precursor;
	};

	/** One exact worker-row to stored-claim provenance association. */
	struct materialization_store_v6_origin_input
	{
		std::uint64_t batch_index{};
		std::uint64_t row_index{};
		materialization_origin_association association;
	};

	/**
	 * All per-task authorities are move-owned. The factory consumes the sole task capability and
	 * revalidates the runtime transcript, six normalized batches, claim seal, and provenance.
	 */
	struct materialization_store_v6_task_input
	{
		provider_task_v4_authority authority;
		provider_worker_v4_receipt worker;
		provider_worker_v4_normalized_output normalized;
		sdk::provider::detail::sealed_provider_transcript transcript;
		sdk::provider::detail::provider_runtime_receipt runtime;
		materialization_v4_claim_sealed claims;
		std::vector<materialization_store_v6_canonicalization_input> canonicalization_edges;
		std::vector<materialization_store_v6_origin_input> origins;

		materialization_store_v6_task_input(
			provider_task_v4_authority authority,
			provider_worker_v4_receipt worker,
			provider_worker_v4_normalized_output normalized,
			sdk::provider::detail::sealed_provider_transcript transcript,
			sdk::provider::detail::provider_runtime_receipt runtime,
			materialization_v4_claim_sealed claims,
			std::vector<materialization_store_v6_canonicalization_input> canonicalization_edges,
			std::vector<materialization_store_v6_origin_input> origins)
			: authority{std::move(authority)}, worker{std::move(worker)},
			  normalized{std::move(normalized)}, transcript{std::move(transcript)},
			  runtime{std::move(runtime)}, claims{std::move(claims)},
			  canonicalization_edges{std::move(canonicalization_edges)}, origins{std::move(origins)}
		{
		}

		materialization_store_v6_task_input(const materialization_store_v6_task_input&) = delete;
		materialization_store_v6_task_input&
		operator=(const materialization_store_v6_task_input&) = delete;
		materialization_store_v6_task_input(materialization_store_v6_task_input&&) noexcept =
			default;
		materialization_store_v6_task_input&
		operator=(materialization_store_v6_task_input&&) noexcept = default;
	};

	struct materialization_store_v6_ingress_input
	{
		materialization_request_v2_2 request;
		std::vector<std::string> advertised_features;
		materialization_v4_incremental_receipt incremental;
		materialization_v4_execution_receipt journal;
		materialization_v4_provider_output_authority output;
		std::vector<materialization_store_v6_task_input> tasks;
		std::stop_token cancellation{};
	};

	/** Opaque one-task pull cursor. It owns no Store and never exposes its sealed spool. */
	class materialization_store_v6_typed_task final
	{
	  public:
		materialization_store_v6_typed_task(materialization_store_v6_typed_task&&) noexcept;
		materialization_store_v6_typed_task&
		operator=(materialization_store_v6_typed_task&&) noexcept;
		materialization_store_v6_typed_task(const materialization_store_v6_typed_task&) = delete;
		materialization_store_v6_typed_task&
		operator=(const materialization_store_v6_typed_task&) = delete;
		~materialization_store_v6_typed_task();

		[[nodiscard]] const materialization_store_v6_typed_task_receipt& receipt() const noexcept;
		[[nodiscard]] sdk::result<std::optional<materialization_store_v6_typed_event>> next();
		[[nodiscard]] sdk::result<
			std::optional<materialization_store_v6_canonicalization_edge_event>>
		next_canonicalization_edge();
		[[nodiscard]] sdk::result<bool> authority_complete() const;

	  private:
		struct state;
		explicit materialization_store_v6_typed_task(std::unique_ptr<state> state) noexcept;
		std::unique_ptr<state> state_;
		friend class materialization_store_v6_typed_ingress;
	};

	/** Factory-only expected authority. It reads a distinct sealed semantic spool. */
	class materialization_store_v6_expected_authority final
		: public sdk::detail::bounded_store_v6_expected_semantic_cursor
	{
	  public:
		materialization_store_v6_expected_authority(
			materialization_store_v6_expected_authority&&) noexcept;
		materialization_store_v6_expected_authority&
		operator=(materialization_store_v6_expected_authority&&) noexcept;
		materialization_store_v6_expected_authority(
			const materialization_store_v6_expected_authority&) = delete;
		materialization_store_v6_expected_authority&
		operator=(const materialization_store_v6_expected_authority&) = delete;
		~materialization_store_v6_expected_authority() override;

		[[nodiscard]] sdk::result<std::optional<sdk::detail::bounded_store_v6_semantic_record>>
		next_semantic_record() override;
		[[nodiscard]] sdk::result<bool> authority_complete() const override;

	  private:
		struct state;
		explicit materialization_store_v6_expected_authority(std::unique_ptr<state> state) noexcept;
		std::unique_ptr<state> state_;
		friend class materialization_store_v6_typed_ingress;
	};

	/** Opaque request-wide authority; task and expected cursors are both one-shot. */
	class materialization_store_v6_typed_ingress final
	{
	  public:
		materialization_store_v6_typed_ingress(materialization_store_v6_typed_ingress&&) noexcept;
		materialization_store_v6_typed_ingress&
		operator=(materialization_store_v6_typed_ingress&&) noexcept;
		materialization_store_v6_typed_ingress(const materialization_store_v6_typed_ingress&) =
			delete;
		materialization_store_v6_typed_ingress&
		operator=(const materialization_store_v6_typed_ingress&) = delete;
		~materialization_store_v6_typed_ingress();

		[[nodiscard]] const materialization_store_v6_structural_census&
		structural_census() const noexcept;
		[[nodiscard]] const materialization_store_v6_semantic_census&
		semantic_census() const noexcept;
		[[nodiscard]] std::string_view immutable_authority_binding() const noexcept;
		[[nodiscard]] std::uint64_t task_count() const noexcept;
		[[nodiscard]] sdk::result<materialization_store_v6_typed_task>
		take_task(std::uint64_t canonical_ordinal);
		[[nodiscard]] sdk::result<materialization_store_v6_expected_authority>
		take_expected_authority();

	  private:
		struct state;
		explicit materialization_store_v6_typed_ingress(std::unique_ptr<state> state) noexcept;
		std::unique_ptr<state> state_;
		friend sdk::result<materialization_store_v6_typed_ingress>
		make_materialization_store_v6_typed_ingress(const sdk::relation_engine&,
													materialization_store_v6_ingress_input);
		friend sdk::result<materialization_store_v6_typed_ingress>
		materialization_store_v6_typed_ingress_factory_impl(const sdk::relation_engine&,
															materialization_store_v6_ingress_input);
	};

	/** Derive the exact occurrence reference expected by origin associations. */
	[[nodiscard]] sdk::result<std::string>
	derive_materialization_store_v6_claim_ref(const sdk::claim& value);

	/** Explicitly test the hard task bound without constructing a request-sized fixture. */
	[[nodiscard]] sdk::result<void>
	validate_materialization_store_v6_task_count(std::uint64_t task_count);

	/** Shared typed/expected/edge spool charge, checked before every append. */
	[[nodiscard]] sdk::result<std::uint64_t>
	checked_materialization_store_v6_spool_charge(std::uint64_t aggregate_bytes,
												  std::uint64_t task_aggregate_begin,
												  std::uint64_t next_bytes);

	/** Fixed record-source bound checked before canonical value/byte allocation. */
	[[nodiscard]] sdk::result<void>
	validate_materialization_store_v6_record_source_bytes(std::uint64_t source_bytes);

	/** Fixed sort-arena charge checked before every vector/key/node allocation. */
	[[nodiscard]] sdk::result<std::uint64_t>
	checked_materialization_store_v6_sort_arena_charge(std::uint64_t used_bytes,
													   std::uint64_t next_bytes);

	/** Complete, effect-free ingress validation and spool sealing factory. */
	[[nodiscard]] sdk::result<materialization_store_v6_typed_ingress>
	make_materialization_store_v6_typed_ingress(const sdk::relation_engine& engine,
												materialization_store_v6_ingress_input input);
} // namespace cxxlens::detail::clang22::materialization
