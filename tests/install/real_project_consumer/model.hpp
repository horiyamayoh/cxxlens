#pragma once

#include <cxxlens/sdk.hpp>

namespace real_project
{
	[[nodiscard]] cxxlens::sdk::relation_descriptor descriptor();
	[[nodiscard]] int qualify(cxxlens::sdk::snapshot_store& store,
							  const cxxlens::sdk::relation_engine& engine);
} // namespace real_project
