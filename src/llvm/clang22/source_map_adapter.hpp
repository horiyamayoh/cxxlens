#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cxxlens/core/failure.hpp>
#include <cxxlens/source.hpp>
#include <cxxlens/workspace.hpp>

#include "../common/frontend_port.hpp"

namespace clang
{
	class CompilerInstance;
	class SourceLocation;
	class SourceRange;
} // namespace clang

namespace cxxlens::detail::clang22
{
	struct normalized_source_file
	{
		file_id id;
		std::string semantic_path;
		source_digest digest;
		std::uint64_t size{};
		bool system{};
		bool generated{};
		bool builtin{};
		source_span whole_file;
	};

	class source_map_adapter
	{
	  public:
		source_map_adapter(clang::CompilerInstance& compiler,
						   const compile_unit& unit,
						   const std::vector<frontend::virtual_source_file>& virtual_files);
		~source_map_adapter();
		source_map_adapter(const source_map_adapter&) = delete;
		source_map_adapter& operator=(const source_map_adapter&) = delete;
		source_map_adapter(source_map_adapter&&) noexcept;
		source_map_adapter& operator=(source_map_adapter&&) noexcept;

		[[nodiscard]] result<normalized_source_file>
		file_for(const clang::SourceLocation& location) const;
		[[nodiscard]] result<source_span> direct_span(const clang::SourceRange& range) const;
		[[nodiscard]] result<source_span>
		macro_span(const clang::SourceRange& primary,
				   const clang::SourceRange& invocation,
				   const clang::SourceRange* definition,
				   const std::string& macro_name,
				   source_origin origin,
				   std::optional<std::uint32_t> argument_index = std::nullopt) const;
		[[nodiscard]] std::string source_text(const clang::SourceRange& range) const;

	  private:
		struct impl;
		std::unique_ptr<impl> impl_;
	};
} // namespace cxxlens::detail::clang22
