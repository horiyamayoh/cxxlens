#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

namespace cxxlens::sdk::detail
{
	/** Sort detached values without reencoding their full projection for every comparison. */
	template <class Value, class Projection>
	void sort_canonical_projection(std::vector<Value>& values, Projection projection)
	{
		using key_type = std::remove_cvref_t<std::invoke_result_t<Projection, const Value&>>;
		struct entry
		{
			key_type key;
			std::size_t index;
		};
		std::vector<entry> entries;
		entries.reserve(values.size());
		for (std::size_t index = 0U; index < values.size(); ++index)
			entries.push_back({std::invoke(projection, values[index]), index});
		std::ranges::sort(entries, {}, &entry::key);
		std::vector<Value> sorted;
		sorted.reserve(values.size());
		for (const auto& value : entries)
			sorted.push_back(std::move(values[value.index]));
		values = std::move(sorted);
	}
} // namespace cxxlens::sdk::detail
