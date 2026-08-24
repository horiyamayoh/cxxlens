#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/incremental.hpp>
#include <cxxlens/sdk/store.hpp>

#include "materialization_prior_artifact_limits.hpp"
#include "materialization_incremental_coordinator.hpp"
#include "materialization_report.hpp"
#include "materialization_seal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	class materialization_effect_root;

	/** Task metadata retained while the detailed capture remains in a sealed replay spool. */
	struct materialization_prior_artifact_task_metadata
	{
		materialization_incremental_task_identity identity;
		sdk::incremental::partition_state state;
		std::string sealed_artifact_digest;
		std::string provider_execution_id;
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

	/**
	 * Source-private loaded view: publication/task metadata stays resident while detailed captures
	 * remain in one sealed replay spool and are decoded only for the task being reused.
	 */
	struct materialization_prior_artifact_replay_bundle
	{
		materialization_prior_artifact_bundle publication;
		std::vector<materialization_prior_artifact_task_metadata> tasks;
		std::optional<detailed_task_report_replayable_spool> captures;
	};

	/** Canonical, versioned envelope used by the source-private durable sidecar. */
	[[nodiscard]] sdk::result<std::vector<std::byte>>
	encode_materialization_prior_artifact(const materialization_prior_artifact_bundle& bundle,
										  const materialization_prior_artifact_limits& limits = {});

	/** Strict decoder: canonical shape, digest, bounds, ordering, and publication state. */
	[[nodiscard]] sdk::result<materialization_prior_artifact_bundle>
	decode_materialization_prior_artifact(std::span<const std::byte> bytes,
										  const materialization_prior_artifact_limits& limits = {});

	/** Load exact parent metadata and a sealed capture spool; an absent blob is cold start. */
	[[nodiscard]] sdk::result<std::optional<materialization_prior_artifact_replay_bundle>>
	load_materialization_prior_artifact(const materialization_effect_root& root,
										const sdk::relation_engine& engine,
										const validated_publication_request& publication,
										const materialization_prior_artifact_limits& limits = {});

	/** Persist task metadata while replaying detailed captures one task at a time. */
	[[nodiscard]] sdk::result<void> persist_materialization_prior_artifact(
		const materialization_effect_root& root,
		const validated_publication_request& publication,
		const sdk::publication_record& committed_record,
		const materialization_store_observation& observation,
		const detailed_task_report_replayable_spool& captures,
		std::vector<materialization_prior_artifact_task_metadata> tasks,
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
