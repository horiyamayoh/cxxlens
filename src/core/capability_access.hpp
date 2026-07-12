#pragma once

#include <algorithm>
#include <ranges>
#include <utility>
#include <vector>

#include <cxxlens/core.hpp>

namespace cxxlens::detail
{
	struct capability_set_access
	{
		[[nodiscard]] static capability_set make(std::vector<capability> values)
		{
			std::ranges::sort(values, {}, &capability::id);
			values.erase(std::ranges::unique(values, {}, &capability::id).begin(), values.end());
			capability_set output;
			output.values_ = std::move(values);
			return output;
		}
	};
} // namespace cxxlens::detail
