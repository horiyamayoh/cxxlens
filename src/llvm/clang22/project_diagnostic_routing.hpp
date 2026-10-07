#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cxxlens::detail::clang22
{
	struct project_diagnostic_scope_less
	{
		using is_transparent = void;
		template <class Left, class Right>
		bool operator()(const Left& left, const Right& right) const noexcept
		{
			return std::pair<std::string_view, std::string_view>{left.first, left.second} <
				std::pair<std::string_view, std::string_view>{right.first, right.second};
		}
	};
	using project_diagnostic_relations = std::map<std::pair<std::string, std::string>,
												  std::vector<std::string>,
												  project_diagnostic_scope_less>;

	// The collector charges ownership before registering an original affected relation.
	inline void add_project_diagnostic_relation(project_diagnostic_relations& routes,
												const std::string_view code,
												const std::string_view subject,
												const std::string_view relation)
	{
		auto& affected = routes[{std::string{code}, std::string{subject}}];
		if (std::ranges::find(affected, relation) == affected.end())
		{
			affected.emplace_back(relation);
			std::ranges::sort(affected);
		}
	}

	[[nodiscard]] inline bool project_diagnostic_applies(const project_diagnostic_relations& routes,
														 const std::string_view code,
														 const std::string_view subject,
														 const std::string_view relation) noexcept
	{
		const auto found = routes.find(std::pair{code, subject});
		// Unrouted diagnostics and missing scope metadata retain the conservative
		// translation-unit frontier. Diagnostic prose never selects a relation.
		return found == routes.end() || found->second.empty() ||
			std::ranges::find(found->second, relation) != found->second.end();
	}
} // namespace cxxlens::detail::clang22
