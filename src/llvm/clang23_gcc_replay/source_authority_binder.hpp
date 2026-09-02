#pragma once

/**
 * @file source_authority_binder.hpp
 * @brief Bind detached Clang 23 locations to capture-owned source identities.
 */

#include <cstdint>
#include <string>
#include <vector>

#include "sdk/gcc_replay_input_internal.hpp"
#include "worker_observer.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	struct bound_source_span
	{
		observed_source_span observed;
		std::string span_id;
		std::string source_snapshot_id;
		std::string file_id;
		std::string role;
		bool read_only{};

		[[nodiscard]] bool operator==(const bound_source_span&) const = default;
	};

	/**
	 * Source identities recomputed from the original host-side capture authority.
	 *
	 * This value contains no relation claim, coverage, guarantee, trust, or publication authority.
	 */
	struct bound_observation_sources
	{
		std::string replay_input_digest;
		std::vector<bound_source_span> spans;

		[[nodiscard]] bool operator==(const bound_observation_sources&) const = default;
	};

	/** Bind every observed source range to an exact capture member or reject atomically. */
	[[nodiscard]] sdk::result<bound_observation_sources>
	bind_observation_sources(const sdk::detail::validated_gcc_replay_input& input,
							 const observation_batch& observations);
} // namespace cxxlens::detail::clang23_gcc_replay
