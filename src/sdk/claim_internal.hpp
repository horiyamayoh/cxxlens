#pragma once

#include <string>

#include <cxxlens/sdk/relation.hpp>

namespace cxxlens::sdk::detail
{
	[[nodiscard]] result<std::string>
	functional_payload_digest(const relation_descriptor& descriptor, const detached_row& row);
} // namespace cxxlens::sdk::detail
