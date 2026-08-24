#pragma once

#include <memory>
#include <string_view>

#include <cxxlens/sdk/common.hpp>

#include "materialization_io.hpp"
#include "materialization_prior_artifact.hpp"
#include "materialization_rooted_vfs.hpp"

namespace cxxlens::detail::clang22::materialization
{
	[[nodiscard]] bool
	prior_artifact_limits_valid(const materialization_prior_artifact_limits& limits) noexcept;

	[[nodiscard]] bool prior_artifact_sidecar_missing(const sdk::error& error) noexcept;

	[[nodiscard]] sdk::result<std::unique_ptr<materialization_replayable_spool>>
	spool_prior_artifact_sidecar(const materialization_owned_fd& file,
								 const materialization_prior_artifact_limits& limits);

	[[nodiscard]] sdk::result<void>
	install_prior_artifact_sidecar(const materialization_effect_root& root,
								   std::string_view path,
								   materialization_replayable_spool& spool,
								   const materialization_prior_artifact_limits& limits);
} // namespace cxxlens::detail::clang22::materialization
