#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>
#include <cxxlens/sdk/store.hpp>

#include "materialization_prior_artifact_limits.hpp"
#include "materialization_v4_incremental_ingress.hpp"

namespace cxxlens::detail::clang22::materialization
{
	class materialization_effect_root;

	/** Durable identity of a committed task-v4 Store publication. */
	struct materialization_v4_prior_publication_identity
	{
		std::string series_id;
		std::string publication_id;
		std::string snapshot_id;
		std::optional<std::string> parent_publication;
		std::uint64_t sequence{};
		std::uint64_t physical_generation{};
		bool committed{};
		bool corrupt{};

		[[nodiscard]] bool
		operator==(const materialization_v4_prior_publication_identity&) const = default;
	};

	/**
	 * Identity-only task-v4 prior artifact.
	 *
	 * The claim/coverage payload remains in the committed Store snapshot. This sidecar records
	 * only the complete v4 receipt and the independently supplied publication/recipe authority;
	 * it cannot be used as a substitute payload or as a path-derived publication claim.
	 */
	struct materialization_v4_prior_artifact
	{
		inline static constexpr std::string_view schema =
			"cxxlens.clang22.materialization-v4-prior-artifact.v1";
		std::uint32_t version{1U};
		std::string materialization_request_id;
		materialization_v4_store_publication_authority authority;
		materialization_v4_prior_publication_identity publication;
		materialization_v4_incremental_receipt receipt;
		std::string artifact_digest;

		[[nodiscard]] bool operator==(const materialization_v4_prior_artifact&) const = default;
	};

	/** Successful exact reuse decision; no provider process is launched for these tasks. */
	struct materialization_v4_prior_artifact_reuse
	{
		materialization_v4_incremental_receipt receipt;
		std::uint64_t provider_call_count{};
		std::uint64_t reused_task_count{};

		[[nodiscard]] bool
		operator==(const materialization_v4_prior_artifact_reuse&) const = default;
	};

	/** Canonical envelope codec for the v4 identity-only prior artifact. */
	[[nodiscard]] sdk::result<std::vector<std::byte>> encode_materialization_v4_prior_artifact(
		const materialization_v4_prior_artifact& artifact,
		const materialization_prior_artifact_limits& limits = {});

	[[nodiscard]] sdk::result<materialization_v4_prior_artifact>
	decode_materialization_v4_prior_artifact(
		std::span<const std::byte> bytes, const materialization_prior_artifact_limits& limits = {});

	/**
	 * Exact identity gate used before a provider launch. Every per-task receipt is compared in
	 * canonical index order; stale publication/authority or a reordered/tampered receipt fails.
	 */
	[[nodiscard]] sdk::result<materialization_v4_prior_artifact_reuse>
	match_materialization_v4_prior_artifact(
		const materialization_v4_prior_artifact& artifact,
		std::string_view materialization_request_id,
		const materialization_v4_store_publication_authority& authority,
		const materialization_v4_prior_publication_identity& publication,
		std::span<const materialization_v4_claim_receipt> current_task_receipts);

	/** Atomically install a rooted, immutable v4 sidecar for one committed publication. */
	[[nodiscard]] sdk::result<void> persist_materialization_v4_prior_artifact(
		const materialization_effect_root& root,
		std::string_view sqlite_path,
		std::string_view expected_parent_publication,
		const materialization_v4_prior_artifact& artifact,
		const materialization_prior_artifact_limits& limits = {});

	/** Read and validate a rooted v4 sidecar; absent sidecars are a cold start. */
	[[nodiscard]] sdk::result<std::optional<materialization_v4_prior_artifact>>
	load_materialization_v4_prior_artifact(
		const materialization_effect_root& root,
		std::string_view sqlite_path,
		std::string_view expected_parent_publication,
		std::string_view materialization_request_id,
		const materialization_v4_store_publication_authority& authority,
		const materialization_v4_prior_publication_identity& publication,
		const materialization_prior_artifact_limits& limits = {});
} // namespace cxxlens::detail::clang22::materialization
