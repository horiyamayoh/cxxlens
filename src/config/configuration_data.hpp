#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include <cxxlens/configuration.hpp>

namespace cxxlens::detail
{
	using configuration_value =
		std::variant<bool, std::int64_t, std::string, std::vector<std::string>>;

	struct configuration_origin
	{
		configuration_layer layer{configuration_layer::built_in_default};
		configuration_value value;
		std::string source;
	};

	struct configuration_data
	{
		std::map<std::string, std::vector<configuration_origin>, std::less<>> values;
		std::map<std::string, std::map<std::string, configuration_value, std::less<>>, std::less<>>
			profiles;
	};
} // namespace cxxlens::detail
