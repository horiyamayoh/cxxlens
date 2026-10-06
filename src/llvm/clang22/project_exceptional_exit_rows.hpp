#pragma once

#include <map>
#include <string>

#include <cxxlens/sdk/relation.hpp>

#include "project_exceptional_exits.hpp"

namespace cxxlens::detail::clang22
{
	struct exceptional_exit_row_bindings
	{
		std::string compile_unit;
		std::function<std::string(exceptional_source)> source;
	};
	struct exceptional_scope_facets
	{
		std::string detail, body;
		std::map<std::string, sdk::detached_cell, std::less<>> fields;
	};
	struct project_exceptional_exit_rows
	{
		std::vector<sdk::detached_row> rows;
		std::vector<exceptional_scope_facets> scopes;
		std::vector<std::string> unbound_scopes;
	};
	[[nodiscard]] sdk::result<project_exceptional_exit_rows>
	detach_project_exceptional_exits(const project_exceptional_exit_observations&,
									 const exceptional_exit_row_bindings&,
									 exceptional_exit_limits = {});
} // namespace cxxlens::detail::clang22
