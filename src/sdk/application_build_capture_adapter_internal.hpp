#pragma once

/**
 * @file application_build_capture_adapter_internal.hpp
 * @brief Adapt one validated application capture/replay unit to the compiler-neutral authority.
 */

#include "application_analysis_internal.hpp"
#include "build_capture_internal.hpp"

namespace cxxlens::sdk::detail
{
	/** Build only from the immutable imported project and its bound replay plan. */
	[[nodiscard]] result<validated_build_capture>
	make_application_build_capture(const imported_project::implementation& project,
								   const replay_plan::implementation& plan,
								   build_capture_limits limits = {});
} // namespace cxxlens::sdk::detail
