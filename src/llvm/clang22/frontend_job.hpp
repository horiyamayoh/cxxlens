#pragma once

#include <cxxlens/interop/clang.hpp>

#include "../common/frontend_port.hpp"

namespace cxxlens::detail::clang22
{
	[[nodiscard]] frontend::adapter_capability capability();
	[[nodiscard]] result<frontend::observation_batch>
	execute(frontend::parse_task task,
			execution_context context = {},
			interop::clang_tu_callback callback = {});
} // namespace cxxlens::detail::clang22
