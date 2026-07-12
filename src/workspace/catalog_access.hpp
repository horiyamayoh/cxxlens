#pragma once

#include <memory>
#include <vector>

#include <cxxlens/workspace.hpp>

#include "../llvm/common/frontend_port.hpp"
#include "../runtime/filesystem_port.hpp"

namespace cxxlens::detail
{
	struct workspace_catalog_access
	{
		[[nodiscard]] static result<workspace>
		open(workspace_options options,
			 const execution_context& context,
			 const std::shared_ptr<runtime::filesystem_port>& files,
			 std::vector<frontend::virtual_source_file> virtual_files = {});
	};
} // namespace cxxlens::detail
