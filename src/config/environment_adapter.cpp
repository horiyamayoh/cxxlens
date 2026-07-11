#include <cstdlib>

#include "environment_port.hpp"

namespace cxxlens::detail::config
{
	std::optional<std::string> standard_environment_adapter::get(const std::string_view name) const
	{
		const std::string terminated{name};
		if (const auto* value = std::getenv(terminated.c_str()); value != nullptr)
			return std::string{value};
		return std::nullopt;
	}

	memory_environment_adapter& memory_environment_adapter::set(std::string name, std::string value)
	{
		values_.insert_or_assign(std::move(name), std::move(value));
		return *this;
	}

	std::optional<std::string> memory_environment_adapter::get(const std::string_view name) const
	{
		if (const auto found = values_.find(name); found != values_.end())
			return found->second;
		return std::nullopt;
	}
} // namespace cxxlens::detail::config
