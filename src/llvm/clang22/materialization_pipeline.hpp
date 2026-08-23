#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_bounded_claim_source.hpp"
#include "materialization_claims.hpp"
#include "materialization_request_v2_2.hpp"
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

	/** Build Store metadata from a v2.1 bounded typed source. */
	[[nodiscard]] sdk::result<streaming_prepared_store_transaction>
	make_materialization_streaming_store_transaction(
		const materialization_v2_1_claim_authority& authority,
		const materialization_bounded_claim_source& source);

	/**
	 * The closure-bearing materializer admission.  The request and every transferred manifest are
	 * retained together so a task cannot be accepted using the pre-transfer request-only result.
	 * This object intentionally contains metadata and identities only; source/blob bytes are owned
	 * by the bounded transport and are never read through the process cwd.
	 */
	struct materialization_v2_2_closure_admission
	{
		validated_materialization_request_v2_2 request;
		std::vector<source_closure_manifest> manifests;

		[[nodiscard]] const source_closure_manifest*
		manifest_for(std::string_view closure_id) const noexcept;
	};

	/** Explicit materializer task phases.  No phase may be skipped or downgraded. */
	enum class materialization_v2_2_task_phase : std::uint8_t
	{
		closure_validated,
		task_accepted,
		materialization_started,
	};

	/** A task acceptance token issued only after its complete source closure is validated. */
	struct materialization_v2_2_task_admission
	{
		std::uint64_t task_index{};
		std::string task_id;
		std::string task_v4_digest;
		std::string source_closure_id;
		std::string source_closure_digest;
		std::string manifest_digest;
		materialization_v2_2_task_phase phase{materialization_v2_2_task_phase::task_accepted};
	};

	/**
	 * Admit a request for execution after the source-closure manifest transfer is complete.
	 * An empty manifest span is always rejected, even though request admission itself supports a
	 * pre-transfer validation phase for transport orchestration.
	 */
	[[nodiscard]] sdk::result<materialization_v2_2_closure_admission>
	admit_materialization_request_v2_2_for_execution(
		materialization_request_v2_2 request,
		std::span<const std::string> advertised_features,
		std::span<const source_closure_manifest> manifests,
		materialization_request_v2_2_limits limits = {});

	/**
	 * Issue one task acceptance token.  The token contains no host path and cannot be created until
	 * the request's exact task/manifest binding has passed.
	 */
	[[nodiscard]] sdk::result<materialization_v2_2_task_admission>
	accept_materialization_task_v2_2(const materialization_v2_2_closure_admission& admission,
									 std::uint64_t task_index);

	/** Mark the irreversible materialization phase; only a v2.2 task token may cross this gate. */
	[[nodiscard]] sdk::result<void>
	begin_materialization_task_v2_2(materialization_v2_2_task_admission& task);
} // namespace cxxlens::detail::clang22::materialization
