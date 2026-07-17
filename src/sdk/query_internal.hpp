#pragma once

#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/query.hpp>

namespace cxxlens::sdk::query::detail
{
	inline constexpr std::string_view occurrence_separator{"\x1f"};

	[[nodiscard]] inline std::string occurrence_column_id(const std::string_view source_alias,
														  const std::string_view column_id)
	{
		return std::string{source_alias} + std::string{occurrence_separator} +
			std::string{column_id};
	}

	[[nodiscard]] std::string
	canonical_subtree_form(std::string_view node_id,
						   const std::map<std::string, const ir_node*, std::less<>>& nodes);
	[[nodiscard]] std::string
	canonical_subtree_digest(std::string_view node_id,
							 const std::map<std::string, const ir_node*, std::less<>>& nodes);

	[[nodiscard]] inline std::vector<std::string>
	output_aliases(const std::span<const column_ref> columns)
	{
		std::vector<std::string> aliases;
		aliases.reserve(columns.size());
		std::set<std::string, std::less<>> used;
		std::map<std::string, std::size_t, std::less<>> leaf_counts;
		for (const auto& column : columns)
		{
			const auto separator = column.column_id.rfind('.');
			++leaf_counts[column.column_id.substr(separator == std::string::npos ? 0U
																				 : separator + 1U)];
		}
		for (const auto& column : columns)
		{
			auto separator = column.column_id.rfind('.');
			auto alias =
				column.column_id.substr(separator == std::string::npos ? 0U : separator + 1U);
			if (leaf_counts.at(alias) > 1U && !column.source_alias.empty())
			{
				auto qualified = column.source_alias;
				qualified.push_back('_');
				qualified.append(alias);
				alias = std::move(qualified);
			}
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
