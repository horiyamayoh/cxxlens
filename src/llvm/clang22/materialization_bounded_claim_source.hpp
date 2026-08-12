#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_claims.hpp"
#include "materialization_io.hpp"
#include "materialization_store.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/**
	 * Replayable source-private typed partition ingress for DF-0200.
	 *
	 * A task window is consumed into sealed per-partition claim spools. Only identity and coverage
	 * metadata remain resident; claim payloads are decoded into one partition draft during replay.
	 * The source has no sdk::claim_batch and cannot publish a Store record itself.
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

		/** Consume exactly one bounded task window before the cursor advances. */
		[[nodiscard]] sdk::result<void> consume_task(materialization_bounded_task_claims task);

		/** Seal all private partition spools and return the immutable replay source. */
		[[nodiscard]] sdk::result<materialization_bounded_claim_source> finalize() &&;

		[[nodiscard]] sdk::result<void>
		replay(const materialization_store_partition_consumer& consumer) override;

		[[nodiscard]] std::string_view materialization_request_id() const noexcept
		{
			return materialization_request_id_;
		}

		[[nodiscard]] std::size_t partition_count() const noexcept
		{
			return partitions_.size();
		}

		[[nodiscard]] bool sealed() const noexcept
		{
			return sealed_;
		}

	  private:
		struct partition_state
		{
			sdk::partition_draft identity;
			std::unique_ptr<materialization_replayable_spool> claims;
			std::map<std::string, sdk::snapshot_coverage_unit, std::less<>> coverage;
			std::vector<sdk::unresolved_reference> unresolved;
			bool empty{};
			std::uint64_t appended_claim_count{};
		};

		materialization_bounded_claim_source(std::string materialization_request_id,
											 const sdk::relation_engine& engine)
			: materialization_request_id_{std::move(materialization_request_id)}, engine_{&engine}
		{
		}

		std::string materialization_request_id_;
		const sdk::relation_engine* engine_{};
		std::map<std::string, partition_state, std::less<>> partitions_;
		std::string materializer_semantics_digest_;
		std::string direct_basis_digest_;
		std::string canonical_adoption_transform_digest_;
		std::string base_ingestion_transform_digest_;
		std::string assumption_set_id_;
		bool sealed_{};
	};
} // namespace cxxlens::detail::clang22::materialization
