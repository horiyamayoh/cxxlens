#pragma once

#include <memory>
#include <vector>

#include <cxxlens/core/failure.hpp>
#include <cxxlens/workspace.hpp>

#include "../common/frontend_port.hpp"

namespace clang
{
	class CompilerInstance;
} // namespace clang

namespace cxxlens::detail::clang22
{
	class preprocessor_extraction_session
	{
	  public:
		virtual ~preprocessor_extraction_session() = default;
		[[nodiscard]] virtual result<std::vector<facts::observation_record>> take() = 0;
	};

	[[nodiscard]] result<std::unique_ptr<preprocessor_extraction_session>>
	attach_preprocessor_extractor(clang::CompilerInstance& compiler,
								  const compile_unit& unit,
								  const std::vector<frontend::virtual_source_file>& virtual_files);
} // namespace cxxlens::detail::clang22
