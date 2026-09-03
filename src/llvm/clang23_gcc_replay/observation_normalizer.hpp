#pragma once

/**
 * @file observation_normalizer.hpp
 * @brief Host-side canonical projection of detached Clang 23 observations.
 */

#include <string>
#include <vector>

#include <cxxlens/sdk/relation.hpp>

#include "sdk/gcc_replay_input_internal.hpp"
#include "worker_observation_codec.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	/**
	 * Canonical relation rows before claim, coverage, trust, or publication authority is attached.
	 */
	struct normalized_observation_candidates
	{
		std::string replay_input_digest;
		std::vector<sdk::detached_row> source_spans;
		std::vector<sdk::detached_row> entities;
		std::vector<sdk::detached_row> declarations;
		std::vector<sdk::detached_row> types;
		std::vector<sdk::detached_row> call_sites;
		std::vector<sdk::detached_row> direct_targets;
		std::vector<sdk::capture_gap> unresolved;

		[[nodiscard]] bool operator==(const normalized_observation_candidates&) const;
	};

	/**
	 * Rebind source authority and project requested observations through existing relation
	 * descriptors. The result remains non-authoritative until provider adoption and publication.
	 */
	[[nodiscard]] sdk::result<normalized_observation_candidates>
	normalize_observation_candidates(const sdk::detail::validated_gcc_replay_input& input,
									 const worker_observation_output& worker);
} // namespace cxxlens::detail::clang23_gcc_replay
