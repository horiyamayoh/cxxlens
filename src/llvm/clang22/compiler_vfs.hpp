#pragma once

#include "source_closure.hpp"

#include <cstddef>
#include <expected>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cxxlens::detail::clang22::source_closure
{
	struct vfs_error
	{
		std::string code;
		std::string path;
		std::string role;

		[[nodiscard]] bool operator==(const vfs_error&) const = default;
	};

	struct opened_file
	{
		std::string logical_path;
		std::string compiler_path;
		file_role role{file_role::project_header};
		std::span<const std::byte> content;
	};

	/**
	 * Immutable compiler-facing filesystem derived only from one validated snapshot.
	 * It has no path, descriptor, or callback through which an ambient project file may be read.
	 */
	class read_only_compiler_vfs
	{
	  public:
		read_only_compiler_vfs(const read_only_compiler_vfs&) = delete;
		read_only_compiler_vfs& operator=(const read_only_compiler_vfs&) = delete;
		read_only_compiler_vfs(read_only_compiler_vfs&&) noexcept = default;
		read_only_compiler_vfs& operator=(read_only_compiler_vfs&&) noexcept = default;

		[[nodiscard]] static std::expected<read_only_compiler_vfs, vfs_error>
		create(std::shared_ptr<const validated_snapshot> snapshot);

		[[nodiscard]] std::expected<opened_file, vfs_error>
		open_logical(std::string_view logical_path) const;
		[[nodiscard]] std::expected<opened_file, vfs_error>
		open_compiler(std::string_view compiler_path) const;
		[[nodiscard]] std::expected<std::string, vfs_error>
		map_logical_path(std::string_view logical_path) const;
		[[nodiscard]] std::expected<std::vector<std::string>, vfs_error>
		rewrite_invocation(std::span<const std::string> arguments) const;

		[[nodiscard]] bool ambient_project_fallback_allowed() const noexcept;
		[[nodiscard]] const std::string& snapshot_id() const noexcept;

	  private:
		struct entry
		{
			std::size_t file_index{};
			std::size_t blob_index{};
			std::string compiler_path;
		};

		read_only_compiler_vfs(std::shared_ptr<const validated_snapshot> snapshot,
			std::map<std::string, entry, std::less<>> logical,
			std::map<std::string, std::string, std::less<>> compiler_to_logical);

		[[nodiscard]] std::expected<opened_file, vfs_error>
		open_entry(const std::string& logical_path, const entry& selected) const;

		std::shared_ptr<const validated_snapshot> snapshot_;
		std::map<std::string, entry, std::less<>> logical_;
		std::map<std::string, std::string, std::less<>> compiler_to_logical_;
	};
} // namespace cxxlens::detail::clang22::source_closure
