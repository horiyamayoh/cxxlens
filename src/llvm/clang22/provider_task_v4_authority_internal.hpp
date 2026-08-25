#pragma once

/** Source-private issuer boundary for the task-v4 authority contract. */

#include "provider_task_v4_authority.hpp"

namespace cxxlens::detail::clang22
{
	/**
	 * Only source-private request/receiver/toolchain owners may call this function.  The argument
	 * is moved into opaque state after all cross-bindings and bounds have been checked.
	 */
	[[nodiscard]] sdk::result<provider_task_v4_authority>
	issue_provider_task_v4_authority(provider_task_v4_authority_identity&& identity,
									 provider_task_v4_authority_limits limits = {});
} // namespace cxxlens::detail::clang22
