#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace cxxlens::detail::config
{
	class environment_port
	{
	  public:
		virtual ~environment_port() = default;
		[[nodiscard]] virtual std::optional<std::string> get(std::string_view name) const = 0;
	};

	class standard_environment_adapter final : public environment_port
	{
	  public:
		[[nodiscard]] std::optional<std::string> get(std::string_view name) const override;
	};

	class memory_environment_adapter final : public environment_port
	{
	  public:
		memory_environment_adapter& set(std::string name, std::string value);
		[[nodiscard]] std::optional<std::string> get(std::string_view name) const override;

	  private:
		std::map<std::string, std::string, std::less<>> values_;
	};
} // namespace cxxlens::detail::config
