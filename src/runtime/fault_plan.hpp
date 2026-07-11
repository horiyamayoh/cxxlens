#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "runtime_support.hpp"

namespace cxxlens::detail::runtime
{

	class fault_plan
	{
	  public:
		fault_plan& fail(std::string operation, std::uint64_t call_index, runtime_status status);

		[[nodiscard]] const runtime_failure* match(const request_context& context) const noexcept;

	  private:
		using key = std::pair<std::string, std::uint64_t>;
		std::map<key, runtime_failure> failures_;
	};

} // namespace cxxlens::detail::runtime
