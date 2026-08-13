#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <cxxlens/sdk/incremental.hpp>
#include <cxxlens/sdk/store.hpp>

#include "materialization_incremental_coordinator.hpp"
#include "materialization_report.hpp"
#include "materialization_seal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	class materialization_effect_root;

	/** Bounds applied before a durable prior-artifact envelope is admitted. */
	struct materialization_prior_artifact_limits
	{
		std::size_t max_bytes{detailed_report_limits::maximum_report_bytes};
		std::size_t max_tasks{4096U};
		std::size_t max_capture_bytes{detailed_report_limits::maximum_report_bytes};
		std::size_t max_total_capture_bytes{detailed_report_limits::maximum_report_bytes};
		std::size_t max_batches_per_task{6U};
		std::size_t max_chunks_per_batch{65536U};
		std::size_t max_side_channel_records{65536U};
		std::size_t max_string_bytes{16U * 1024U * 1024U};
	};

	/** One task result retained with its exact invalidation state and planner identity. */
	struct materialization_prior_artifact_task
	{
		materialization_incremental_task_identity identity;
		sdk::incremental::partition_state state;
		std::string sealed_artifact_digest;
		detailed_task_report_capture capture;

		[[nodiscard]] bool operator==(const materialization_prior_artifact_task&) const;
	};

	/** Publication pointer and immutable task blobs committed by one exact Store generation. */
	struct materialization_prior_artifact_bundle
	{
		std::string schema{"cxxlens.clang22.incremental-artifact.v1"};
		std::uint32_t version{1U};
		sdk::snapshot_series_selector selector;
		std::string series_id;
		std::string publication_id;
		std::string snapshot_id;
		std::uint64_t sequence{};
		std::uint64_t physical_generation{};
		std::optional<std::string> parent_publication;
		sdk::publication_state publication_state{sdk::publication_state::committed};
		bool publication_corrupt{};
		std::vector<materialization_prior_artifact_task> tasks;

		[[nodiscard]] bool operator==(const materialization_prior_artifact_bundle&) const = default;
	};

	/** Canonical, versioned envelope used by the source-private durable sidecar. */
	[[nodiscard]] sdk::result<std::vector<std::byte>>
	encode_materialization_prior_artifact(const materialization_prior_artifact_bundle& bundle,
										  const materialization_prior_artifact_limits& limits = {});

	/** Strict decoder: canonical shape, digest, bounds, ordering, and publication state. */
	[[nodiscard]] sdk::result<materialization_prior_artifact_bundle>
	decode_materialization_prior_artifact(std::span<const std::byte> bytes,
										  const materialization_prior_artifact_limits& limits = {});

	/** Load the exact parent publication blob; an absent blob is the only cold-start result. */
	[[nodiscard]] sdk::result<std::optional<materialization_prior_artifact_bundle>>
	load_materialization_prior_artifact(const materialization_effect_root& root,
										const sdk::relation_engine& engine,
										const validated_publication_request& publication,
										const materialization_prior_artifact_limits& limits = {});

	/** Persist an immutable publication-keyed blob only after Store commit verification. */
	[[nodiscard]] sdk::result<void> persist_materialization_prior_artifact(
		const materialization_effect_root& root,
		const validated_publication_request& publication,
		const sdk::publication_record& committed_record,
		const materialization_store_observation& observation,
		std::vector<materialization_prior_artifact_task> tasks,
		const materialization_prior_artifact_limits& limits = {});

	/** Rehydrate only after the current task identity and output descriptors are supplied. */
	[[nodiscard]] sdk::result<sealed_materialization_result>
	rehydrate_materialization_prior_artifact(
		const materialization_prior_artifact_task& artifact,
		std::size_t request_task_index,
		const validated_task_request& current_task,
		const sdk::provider::manifest& provider_manifest,
		std::span<const sdk::relation_descriptor> output_descriptors,
		sdk::provider::protocol_credit output_credit,
		sdk::provider::protocol_limits protocol_limits,
		const detailed_report_limits& report_limits = {});
} // namespace cxxlens::detail::clang22::materialization
