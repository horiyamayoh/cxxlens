#pragma once

#include <set>

#include <cxxlens/testing.hpp>

namespace cxxlens::testing::detail
{
	struct workspace_fixture_data
	{
		std::string language;
		path main_file;
		std::string main_source;
		std::vector<fixture_file> files;
		std::vector<fixture_variant> variants;
		std::string standard;
		std::string target;
		std::vector<std::string> arguments;
		std::set<path> generated_files;
		std::set<path> system_headers;
	};
} // namespace cxxlens::testing::detail
