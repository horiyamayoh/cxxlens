#pragma once

#include <string>
#include <string_view>

#include <cxxlens/core/failure.hpp>

namespace cxxlens::detail::clang22
{
	[[nodiscard]] result<std::string> run_frontend_worker(std::string_view request_bytes);
} // namespace cxxlens::detail::clang22
