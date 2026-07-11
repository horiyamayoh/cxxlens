#pragma once

#include <memory>

#include <cxxlens/configuration.hpp>

#include "../runtime/filesystem_port.hpp"
#include "configuration_data.hpp"
#include "environment_port.hpp"

namespace cxxlens::detail::config
{
	[[nodiscard]] result<std::shared_ptr<const configuration_data>>
	load_document(const path& yaml_file,
				  const runtime::filesystem_port& filesystem,
				  const environment_port& environment);

	[[nodiscard]] result<std::shared_ptr<const configuration_data>>
	load_nearest_document(const path& start,
						  const runtime::filesystem_port& filesystem,
						  const environment_port& environment);
} // namespace cxxlens::detail::config
