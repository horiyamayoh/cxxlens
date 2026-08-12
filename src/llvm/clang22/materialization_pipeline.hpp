#pragma once

#include <cxxlens/sdk/common.hpp>

#include "materialization_bounded_claim_source.hpp"
#include "materialization_claims.hpp"
#include "materialization_store.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/**
	 * Replay the typed Store partitions owned by one sealed claim result without creating a
	 * second partition vector. The claims object remains the semantic authority and each callback
	 * receives one independent draft for the current Store staging window.
	 */
	class materialization_claim_partition_replay_source final
		: public materialization_store_partition_replay_source
	{
	  public:
		explicit materialization_claim_partition_replay_source(
			const sealed_materialization_claims& claims) noexcept
			: claims_{&claims}
		{
		}

		[[nodiscard]] sdk::result<void>
		replay(const materialization_store_partition_consumer& consumer) override;

	  private:
		const sealed_materialization_claims* claims_{};
	};

	/** Build the one Store transaction exclusively from validated request and sealed claims. */
	[[nodiscard]] sdk::result<prepared_store_transaction>
	make_materialization_store_transaction(const validated_materialization_request& request,
										   const sealed_materialization_claims& claims);

	/** Build Store metadata without copying all partition drafts into a second vector. */
	[[nodiscard]] sdk::result<streaming_prepared_store_transaction>
	make_materialization_streaming_store_transaction(
		const validated_materialization_request& request,
		const sealed_materialization_claims& claims);

	/** Build Store metadata from the production bounded typed source. */
	[[nodiscard]] sdk::result<streaming_prepared_store_transaction>
	make_materialization_streaming_store_transaction(
		const validated_materialization_request& request,
		const materialization_bounded_claim_source& source);
} // namespace cxxlens::detail::clang22::materialization
