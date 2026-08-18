#pragma once

#include "source_closure.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cxxlens::detail::clang22
{
	/** One immutable compiler-visible file resolved exclusively from a source closure. */
	struct source_closure_vfs_file
	{
		std::string logical_path;
		std::string synthetic_path;
		source_closure_role role{source_closure_role::header};
		source_closure_encoding encoding{source_closure_encoding::utf8};
		std::shared_ptr<const std::string> content;
	};

	/**
	 * Read-only project filesystem rooted at `/__cxxlens_project__`. No API in this value can
	 * delegate a project/generated lookup to the process filesystem.
	 */
	class source_closure_vfs
	{
	  public:
		[[nodiscard]] static sdk::result<source_closure_vfs>
		mount(source_closure_snapshot closure);

		/** Open one canonical `project://` or synthetic-root absolute compiler path. */
		[[nodiscard]] sdk::result<source_closure_vfs_file>
		open(std::string_view compiler_path) const;
		/** Resolve one include spelling relative to an authenticated including file. */
		[[nodiscard]] sdk::result<source_closure_vfs_file>
		open_relative(std::string_view include_path,
					  std::string_view including_logical_path) const;
		/** Return canonical synthetic children for one project directory. */
		[[nodiscard]] sdk::result<std::vector<std::string>>
		list_directory(std::string_view compiler_path) const;
		/** Every write-like operation is rejected; the closure remains immutable. */
		[[nodiscard]] sdk::result<void> deny_write(std::string_view compiler_path) const;

		[[nodiscard]] const source_closure_snapshot& closure() const noexcept;
		[[nodiscard]] static constexpr std::string_view synthetic_root() noexcept
		{
			return "/__cxxlens_project__";
		}

	  private:
		explicit source_closure_vfs(source_closure_snapshot closure);

		[[nodiscard]] sdk::result<std::string>
		logical_path(std::string_view compiler_path) const;
		[[nodiscard]] sdk::result<source_closure_vfs_file>
		open_logical(std::string logical_path) const;

		source_closure_snapshot closure_;
	};
} // namespace cxxlens::detail::clang22
