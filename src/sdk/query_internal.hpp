#pragma once

#include <set>
#include <span>
#include <string>
#include <vector>

#include <cxxlens/sdk/query.hpp>

namespace cxxlens::sdk::query::detail
{
	[[nodiscard]] inline std::vector<std::string>
	output_aliases(const std::span<const column_ref> columns)
	{
		std::vector<std::string> aliases;
		aliases.reserve(columns.size());
		std::set<std::string, std::less<>> used;
		for (const auto& column : columns)
		{
			auto separator = column.column_id.rfind('.');
			auto alias =
				column.column_id.substr(separator == std::string::npos ? 0U : separator + 1U);
			if (used.contains(alias))
			{
				alias = column.column_id;
				for (auto& byte : alias)
					if (byte == '.' || byte == '-')
						byte = '_';
			}
			const auto base = alias;
			for (std::uint64_t suffix = 2U; used.contains(alias); ++suffix)
				alias = base + "_" + std::to_string(suffix);
			used.insert(alias);
			aliases.push_back(std::move(alias));
		}
		return aliases;
	}
} // namespace cxxlens::sdk::query::detail
