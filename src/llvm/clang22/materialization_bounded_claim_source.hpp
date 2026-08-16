#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_claims.hpp"
#include "materialization_io.hpp"
#include "materialization_store.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/** Bounded report metadata for one replayable Store partition. */
	struct materialization_bounded_partition_metadata
	{
		std::vector<std::string> stored_claim_refs;
		std::vector<std::string> claim_content_ids;
		std::uint64_t sdk_claim_occurrence_count{};
		std::uint64_t origin_association_count{};
		bool empty_partition{};
	};

	using materialization_claim_envelope_consumer =
		std::function<sdk::result<void>(const materialization_claim_envelope&)>;
	using materialization_canonicalization_edge_consumer =
		std::function<sdk::result<void>(const materialization_canonicalization_edge&)>;
	using materialization_origin_association_consumer =
		std::function<sdk::result<void>(const materialization_origin_association&)>;

	/** Source-private canonical claim-batch census produced without a request-wide claim vector. */
	struct materialization_bounded_claim_batch_status
	{
		std::string content_digest;
		std::uint64_t claim_count{};
		std::uint64_t unresolved_count{};
		std::uint64_t conflict_count{};
		std::uint64_t differential_disagreement_count{};
		std::size_t partition_count{};
	};

	/**
	 * Replayable source-private typed partition ingress for DF-0200.
	 *
	 * A task window is consumed into sealed per-partition claim spools plus source-private report
	 * metadata spools. Only identity/census metadata remain resident; claim payloads are decoded
	 * into one partition draft during replay. The source has no sdk::claim_batch and cannot publish
	 * a Store record itself.
	 */
	class materialization_bounded_claim_source final
		: public materialization_store_partition_replay_source
	{
	  public:
		materialization_bounded_claim_source(const materialization_bounded_claim_source&) = delete;
		materialization_bounded_claim_source&
		operator=(const materialization_bounded_claim_source&) = delete;
		materialization_bounded_claim_source(materialization_bounded_claim_source&&) noexcept =
			default;
		materialization_bounded_claim_source&
		operator=(materialization_bounded_claim_source&&) noexcept = default;
		~materialization_bounded_claim_source() override = default;

		[[nodiscard]] static sdk::result<materialization_bounded_claim_source>
		begin(const validated_materialization_request& request);

		/** Begin a bounded source using v2.1 request authority without a legacy task vector. */
		[[nodiscard]] static sdk::result<materialization_bounded_claim_source>
		begin(const materialization_v2_1_claim_authority& authority);

		/** Consume exactly one bounded task window before the cursor advances. */
		[[nodiscard]] sdk::result<void> consume_task(materialization_bounded_task_claims task);

		/** Seal all private partition spools and return the immutable replay source. */
		[[nodiscard]] sdk::result<materialization_bounded_claim_source> finalize() &&;

		[[nodiscard]] sdk::result<void>
		replay(const materialization_store_partition_consumer& consumer) override;

		/** Replay report metadata without retaining the request-wide claim graph. */
		[[nodiscard]] sdk::result<void>
		replay_claim_envelopes(const materialization_claim_envelope_consumer& consumer);
		[[nodiscard]] sdk::result<void> replay_canonicalization_edges(
			const materialization_canonicalization_edge_consumer& consumer);
		[[nodiscard]] sdk::result<void>
		replay_origin_associations(const materialization_origin_association_consumer& consumer);

		/**
		 * Recompute the public claim-batch v2 digest from bounded partition runs. The
		 * implementation retains at most one decoded occurrence per partition while merging; it
		 * never constructs a request-wide sdk::claim vector.
		 */
		[[nodiscard]] sdk::result<materialization_bounded_claim_batch_status> claim_batch_status();

		/** Return one partition's bounded report census and identity metadata. */
		[[nodiscard]] sdk::result<materialization_bounded_partition_metadata>
		partition_metadata(std::string_view partition_id) const;

		[[nodiscard]] std::string_view materializer_semantics_digest() const noexcept
		{
			return materializer_semantics_digest_;
		}
		[[nodiscard]] std::string_view direct_basis_digest() const noexcept
		{
			return direct_basis_digest_;
		}
		[[nodiscard]] std::string_view canonical_adoption_transform_digest() const noexcept
		{
			return canonical_adoption_transform_digest_;
		}
		[[nodiscard]] std::string_view base_ingestion_transform_digest() const noexcept
		{
			return base_ingestion_transform_digest_;
		}

		[[nodiscard]] std::string_view materialization_request_id() const noexcept
		{
			return materialization_request_id_;
		}

		/** Return the complete source-private request binding sealed at source admission. */
		[[nodiscard]] const materialization_claim_request_binding& request_binding() const noexcept
		{
			return request_binding_;
		}

		/** Return the exact task census sealed into this source's request authority. */
		[[nodiscard]] std::uint64_t task_count() const noexcept
		{
			return expected_task_count_;
		}

		[[nodiscard]] std::size_t partition_count() const noexcept
		{
			return partitions_.size();
		}

		[[nodiscard]] bool sealed() const noexcept
		{
			return sealed_;
		}

		/** Exact installed publication remains closed while any partial guarantee is retained. */
		[[nodiscard]] bool exact_publication_ready() const noexcept
		{
			return sealed_ && !failed_ && exact_publication_ready_;
		}

	  private:
		struct partition_state
		{
			sdk::partition_draft identity;
			std::unique_ptr<materialization_replayable_spool> claims;
			std::map<std::string, sdk::snapshot_coverage_unit, std::less<>> coverage;
			std::vector<sdk::unresolved_reference> unresolved;
			std::set<std::string, std::less<>> stored_claim_refs;
			std::set<std::string, std::less<>> claim_content_ids;
			std::uint64_t origin_association_count{};
			bool empty{};
			std::uint64_t appended_claim_count{};
		};

		materialization_bounded_claim_source(const validated_materialization_request* request,
											 materialization_claim_request_binding request_binding,
											 const sdk::relation_engine& engine,
											 std::uint64_t expected_task_count,
											 std::function<sdk::result<std::string>(std::size_t)>
												 selected_request_entry_binding_resolver)
			: request_{request}, request_binding_{std::move(request_binding)},
			  materialization_request_id_{request_binding_.materialization_request_id},
			  engine_{&engine}, expected_task_count_{expected_task_count},
			  selected_request_entry_binding_resolver_{
				  std::move(selected_request_entry_binding_resolver)}
		{
		}

		/** Validate every retained coverage/guarantee field before opening exact publication. */
		[[nodiscard]] sdk::result<bool> assess_exact_publication_state();
		const validated_materialization_request* request_{};
		materialization_claim_request_binding request_binding_;
		std::string materialization_request_id_;
		const sdk::relation_engine* engine_{};
		std::uint64_t expected_task_count_{};
		std::uint64_t consumed_task_count_{};
		std::function<sdk::result<std::string>(std::size_t)>
			selected_request_entry_binding_resolver_;
		std::map<std::string, partition_state, std::less<>> partitions_;
		std::string materializer_semantics_digest_;
		std::string direct_basis_digest_;
		std::string canonical_adoption_transform_digest_;
		std::string base_ingestion_transform_digest_;
		std::string assumption_set_id_;
		std::unique_ptr<materialization_replayable_spool> claim_envelopes_;
		std::unique_ptr<materialization_replayable_spool> canonicalization_edges_;
		std::unique_ptr<materialization_replayable_spool> origin_associations_;
		bool sealed_{};
		bool failed_{};
		bool exact_publication_ready_{};
		std::uint64_t conflict_count_{};
		std::uint64_t differential_disagreement_count_{};
	};
} // namespace cxxlens::detail::clang22::materialization
