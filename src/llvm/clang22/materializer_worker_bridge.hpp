#pragma once

/**
 * @file materializer_worker_bridge.hpp
 * @brief Installed materializer to Protocol 2.0 worker handoff.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "installed_materializer_source_closure.hpp"
#include "materialization_rooted_vfs.hpp"
#include "materialization_v4_claim_binding.hpp"
#include "provider_trust_issuer_internal.hpp"
#include "sdk/materialization_task_internal.hpp"
#include "sdk/materialization_writer_internal.hpp"
#include "sdk/provider_runtime_internal.hpp"

namespace cxxlens::detail::clang22
{
	/**
	 * Receipt for the complete output of one task-v4 compile unit.
	 *
	 * The existing incremental receipt is task-partition shaped.  Installed Clang 22 has a
	 * stricter six-descriptor atomic output group, so this bridge-level value retains every batch
	 * receipt in authority order instead of collapsing the group to one relation.
	 */
	struct materializer_task_output_receipt
	{
		std::string schema{"cxxlens.clang22.materializer-task-output-receipt.v4"};
		std::string materialization_request_id;
		std::uint64_t task_index{};
		std::string task_id;
		std::string task_v4_digest;
		std::vector<materialization::materialization_v4_claim_receipt> batch_receipts;
		std::uint64_t claim_count{};
		std::uint64_t unresolved_count{};
		std::uint64_t conflict_count{};
		std::uint64_t differential_disagreement_count{};
		bool complete{};
		std::string receipt_digest;

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/** The authenticated request/closure remain alive until the worker has terminated. */
	struct materializer_worker_execution
	{
		installed_materializer_source_closure_result ingress;
		sdk::detail::validated_materialization_task task;
		sdk::provider::detail::provider_process_validation_outcome outcome;
		provider_trust_issuance trust;

		materializer_worker_execution(
			installed_materializer_source_closure_result ingress,
			sdk::detail::validated_materialization_task task,
			sdk::provider::detail::provider_process_validation_outcome outcome,
			provider_trust_issuance trust)
			: ingress{std::move(ingress)}, task{std::move(task)}, outcome{std::move(outcome)},
			  trust{std::move(trust)}
		{
		}

		materializer_worker_execution(const materializer_worker_execution&) = delete;
		materializer_worker_execution& operator=(const materializer_worker_execution&) = delete;
		materializer_worker_execution(materializer_worker_execution&&) noexcept = default;
		materializer_worker_execution&
		operator=(materializer_worker_execution&&) noexcept = default;
	};

	/**
	 * One authenticated worker result after the Store publication boundary has been crossed.
	 *
	 * The claim and incremental receipt are retained alongside the published handle so callers
	 * cannot accidentally report a committed snapshot without its exact input identities.
	 */
	struct materializer_store_execution
	{
		materializer_worker_execution worker;
		std::vector<materialization::materialization_v4_claim_sealed> claims;
		/** Host-side canonical base partitions retained for the detailed report projection. */
		std::vector<sdk::partition_draft> base_partitions;
		/** All validated reference occurrences used by canonical adoption and Store staging. */
		std::vector<sdk::claim> reference_claims;
		materializer_task_output_receipt receipt;
		sdk::detail::validated_materialization_result result;
		sdk::detail::materialization_store_publication publication;
		/** Exact committed parent observed before the invocation publication, when non-genesis. */
		std::optional<sdk::publication_record> observed_parent_record;
		/** SDK canonical export identity captured before the backend lifetime is released. */
		std::string canonical_export_digest;
		/** Rooted SQLite observation captured by the production opener, when SQLite is selected. */
		std::optional<materialization::materialization_rooted_vfs_receipt>
			sqlite_effect_root_receipt;

		materializer_store_execution(
			materializer_worker_execution worker,
			std::vector<materialization::materialization_v4_claim_sealed> claims,
			std::vector<sdk::partition_draft> base_partitions,
			std::vector<sdk::claim> reference_claims,
			materializer_task_output_receipt receipt,
			sdk::detail::validated_materialization_result result,
			sdk::detail::materialization_store_publication publication,
			std::optional<sdk::publication_record> observed_parent_record,
			std::string canonical_export_digest,
			std::optional<materialization::materialization_rooted_vfs_receipt>
				sqlite_effect_root_receipt)
			: worker{std::move(worker)}, claims{std::move(claims)},
			  base_partitions{std::move(base_partitions)},
			  reference_claims{std::move(reference_claims)}, receipt{std::move(receipt)},
			  result{std::move(result)}, publication{std::move(publication)},
			  observed_parent_record{std::move(observed_parent_record)},
			  canonical_export_digest{std::move(canonical_export_digest)},
			  sqlite_effect_root_receipt{std::move(sqlite_effect_root_receipt)}
		{
		}

		materializer_store_execution(const materializer_store_execution&) = delete;
		materializer_store_execution& operator=(const materializer_store_execution&) = delete;
		materializer_store_execution(materializer_store_execution&&) noexcept = default;
		materializer_store_execution& operator=(materializer_store_execution&&) noexcept = default;
	};

	/**
	 * Launch the exact worker named by the request authority and relay the authenticated closure.
	 * The bridge owns no Store effect; the returned sealed transcript is the only worker output
	 * eligible for the subsequent claim/materialization boundary.
	 */
	[[nodiscard]] sdk::result<materializer_worker_execution>
	run_materializer_worker(installed_materializer_source_closure_result ingress);

	/** Conformance and embedding entry point with an explicit trust issuer. */
	[[nodiscard]] sdk::result<materializer_worker_execution>
	run_materializer_worker(installed_materializer_source_closure_result ingress,
							provider_trust_issuer_port& issuer);

	/**
	 * Convert the sealed six-batch transcript into a typed claim and publish it through the
	 * request-authorized memory/SQLite Store.  No task or provider identity is inferred from rows;
	 * all publication fields come from the decoded v2.2 authority.
	 */
	[[nodiscard]] sdk::result<materializer_store_execution>
	publish_materializer_worker(materializer_worker_execution execution);
} // namespace cxxlens::detail::clang22
