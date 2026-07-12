#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "runtime_support.hpp"

namespace cxxlens::detail::runtime
{
	class dynamic_library
	{
	  public:
		virtual ~dynamic_library() = default;
		[[nodiscard]] virtual runtime_result<void*> symbol(std::string_view name) const = 0;
	};

	class dynamic_library_port
	{
	  public:
		virtual ~dynamic_library_port() = default;
		[[nodiscard]] virtual runtime_result<std::shared_ptr<const dynamic_library>>
		open(const std::vector<std::string>& candidates, const request_context& context) const = 0;
	};

	class system_dynamic_library_adapter final : public dynamic_library_port
	{
	  public:
		[[nodiscard]] runtime_result<std::shared_ptr<const dynamic_library>>
		open(const std::vector<std::string>& candidates,
			 const request_context& context) const override;
	};
} // namespace cxxlens::detail::runtime
