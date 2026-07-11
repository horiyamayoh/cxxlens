#pragma once

#include <string_view>

#include "../core/canonical_json.hpp"

namespace cxxlens::testing::detail
{
	[[nodiscard]] result<cxxlens::detail::json::json_value> parse_json(std::string_view input);
} // namespace cxxlens::testing::detail
