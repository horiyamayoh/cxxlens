#pragma once

/**
 * @file worker_ingress.hpp
 * @brief Bounded Clang 23 GCC replay worker ingress.
 */

#include <istream>
#include <ostream>

#include "sdk/compiler_replay_input_internal.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	/**
	 * Read, revalidate, and execute exactly one canonical replay input.
	 *
	 * Success emits one bounded canonical detached-observation value. This boundary cannot claim
	 * relation coverage or publish Store state.
	 */
	[[nodiscard]] sdk::result<void> execute_worker_ingress(std::istream& input,
														   std::ostream& output,
														   sdk::import_limits limits = {});
} // namespace cxxlens::detail::clang23_gcc_replay
