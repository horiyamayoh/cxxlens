#pragma once

#include <iosfwd>
#include <span>
#include <string>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

#include "../common/frontend_port.hpp"

namespace cxxlens::detail::clang22
{
	struct canonicalized_provider_batch
	{
		std::vector<sdk::detached_row> entity_observations;
		std::vector<sdk::detached_row> type_observations;
		std::vector<sdk::detached_row> call_observations;
		std::vector<sdk::detached_row> entities;
		std::vector<sdk::detached_row> call_sites;
		std::vector<sdk::detached_row> direct_targets;
		std::vector<sdk::provider::unresolved_item> unresolved;
		bool exact_equivalence{};
		std::vector<std::string> equivalence_limitations;
	};

	[[nodiscard]] sdk::relation_descriptor entity_observation_descriptor();
	[[nodiscard]] sdk::relation_descriptor type_observation_descriptor();
	[[nodiscard]] sdk::relation_descriptor call_observation_descriptor();

	[[nodiscard]] bool invocation_has_exact_equivalence(const compile_command& command,
														std::vector<std::string>& limitations);

	[[nodiscard]] sdk::result<canonicalized_provider_batch>
	canonicalize_provider_batch(const frontend::observation_batch& batch,
								const std::string& toolchain_digest,
								bool invocation_exact,
								std::vector<std::string> invocation_limitations = {});

	[[nodiscard]] int run_provider_worker(std::span<const std::byte> input, std::ostream& output);
} // namespace cxxlens::detail::clang22
