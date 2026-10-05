#pragma once
#include <map>
#include <string_view>

#include <cxxlens/sdk/relation.hpp>

#include "project_template_events.hpp"
namespace cxxlens::detail::clang22
{
	struct template_event_bindings
	{
		std::string compile_unit;
		std::function<std::string(std::string_view)> entity;
		std::function<std::string(template_event_source)> source;
	};
	struct project_template_event_rows
	{
		std::vector<sdk::detached_row> rows;
		// Qualified original inventory column IDs to merge before validation.
		std::map<std::string, sdk::detached_cell, std::less<>> inventory_facets;
	};
	[[nodiscard]] sdk::result<project_template_event_rows>
	detach_project_template_events(const project_template_event_observations&,
								   const template_event_bindings&,
								   template_event_limits = {});
} // namespace cxxlens::detail::clang22
