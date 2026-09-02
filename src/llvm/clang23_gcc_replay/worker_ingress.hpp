#pragma once

/**
 * @file worker_ingress.hpp
 * @brief Bounded Clang 23 GCC replay worker ingress.
 */

#include <istream>
#include <ostream>

#include "sdk/gcc_replay_input_internal.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	/**
	 * Read and revalidate exactly one canonical replay input.
	 *
	 * Success emits only the canonical input digest after the source-closure-only parser succeeds.
	 * Provider output is a later worker stage; this boundary cannot claim relation coverage or
	 * publish Store state.
	 */
	[[nodiscard]] sdk::result<void> validate_worker_ingress(std::istream& input,
															std::ostream& output,
															sdk::import_limits limits = {});
} // namespace cxxlens::detail::clang23_gcc_replay
