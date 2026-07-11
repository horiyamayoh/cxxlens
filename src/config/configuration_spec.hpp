#pragma once

#include <map>
#include <string>
#include <string_view>

#include <cxxlens/core/failure.hpp>

#include "configuration_data.hpp"

namespace cxxlens::detail::config
{
	enum class configuration_value_kind : unsigned char
	{
		boolean,
		integer,
		string,
		string_list,
	};

	struct configuration_spec
	{
		configuration_value_kind kind{};
		configuration_value default_value;
		bool path_placeholder{};
		bool secret{};
		std::int64_t minimum{};
		std::int64_t maximum{};
		std::vector<std::string> allowed_values;
	};

	[[nodiscard]] const std::map<std::string, configuration_spec, std::less<>>&
	configuration_specs();
	[[nodiscard]] result<void> validate_configuration_value(std::string_view key,
															const configuration_value& value);
	[[nodiscard]] bool is_secret_key(std::string_view key);
	[[nodiscard]] std::string_view configuration_layer_name(configuration_layer layer) noexcept;
} // namespace cxxlens::detail::config
