#pragma once

/**
 * @file materializer_worker_bridge.hpp
 * @brief Installed materializer to Protocol 2.0 worker handoff.
 */

#include <cstdint>
#include <string>
#include <vector>

#include "installed_materializer_source_closure.hpp"
#include "materialization_v4_store_source.hpp"
#include "provider_trust_issuer_internal.hpp"
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
		sdk::provider::detail::provider_process_validation_outcome outcome;
		provider_trust_issuance trust;

		materializer_worker_execution(
			installed_materializer_source_closure_result ingress,
			sdk::provider::detail::provider_process_validation_outcome outcome,
			provider_trust_issuance trust)
			: ingress{std::move(ingress)}, outcome{std::move(outcome)}, trust{std::move(trust)}
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
		materializer_task_output_receipt receipt;
		materialization::materialization_v4_store_publication publication;

		materializer_store_execution(
			materializer_worker_execution worker,
			std::vector<materialization::materialization_v4_claim_sealed> claims,
			materializer_task_output_receipt receipt,
			materialization::materialization_v4_store_publication publication)
			: worker{std::move(worker)}, claims{std::move(claims)}, receipt{std::move(receipt)},
			  publication{std::move(publication)}
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

	/** Test/conformance and future installed-registry entry point with an explicit issuer. */
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
