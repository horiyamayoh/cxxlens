#include "compiler_vfs.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <utility>

namespace cxxlens::detail::clang22::source_closure
{
	namespace
	{
		constexpr std::string_view project_scheme{"project://"};
		constexpr std::string_view generated_scheme{"generated://"};
		constexpr std::string_view project_root{"/__cxxlens/project/"};
		constexpr std::string_view generated_root{"/__cxxlens/generated/"};

		[[nodiscard]] vfs_error make_error(
			std::string code, std::string path, std::string role = {})
		{
			return {std::move(code), std::move(path), std::move(role)};
		}

		[[nodiscard]] std::expected<std::string, vfs_error>
		map_path(const std::string_view logical_path)
		{
			if (logical_path.starts_with(project_scheme))
				return std::string{project_root} + std::string{logical_path.substr(project_scheme.size())};
			if (logical_path.starts_with(generated_scheme))
				return std::string{generated_root} +
					std::string{logical_path.substr(generated_scheme.size())};
			return std::unexpected(make_error(
				"source-closure.vfs-path-not-bound", std::string{logical_path}));
		}

		[[nodiscard]] bool is_path_option(const std::string_view argument)
		{
			constexpr std::array<std::string_view, 5U> options{
				"-I", "-isystem", "-iquote", "-include", "-imacros"};
			return std::ranges::find(options, argument) != options.end();
		}

		[[nodiscard]] std::optional<std::pair<std::string_view, std::string_view>>
		split_joined_path_option(const std::string_view argument)
		{
			constexpr std::array<std::string_view, 3U> prefixes{"-I", "-isystem", "-iquote"};
			for (const auto prefix : prefixes)
				if (argument.starts_with(prefix) && argument.size() > prefix.size())
					return std::pair{prefix, argument.substr(prefix.size())};
			return std::nullopt;
		}
	} // namespace

	read_only_compiler_vfs::read_only_compiler_vfs(
		std::shared_ptr<const validated_snapshot> snapshot,
		std::map<std::string, entry, std::less<>> logical,
		std::map<std::string, std::string, std::less<>> compiler_to_logical)
		: snapshot_{std::move(snapshot)}, logical_{std::move(logical)},
		  compiler_to_logical_{std::move(compiler_to_logical)}
	{
	}

	std::expected<read_only_compiler_vfs, vfs_error>
	read_only_compiler_vfs::create(std::shared_ptr<const validated_snapshot> snapshot)
	{
		if (!snapshot || snapshot->snapshot_id.empty() || snapshot->files.empty() ||
			snapshot->blobs.empty())
			return std::unexpected(make_error("source-closure.vfs-invalid-snapshot", "snapshot"));
		std::map<std::string, std::size_t, std::less<>> blobs;
		for (std::size_t index = 0U; index < snapshot->blobs.size(); ++index)
			if (!blobs.emplace(snapshot->blobs[index].content_digest, index).second)
				return std::unexpected(make_error(
					"source-closure.vfs-invalid-snapshot", snapshot->blobs[index].content_digest));
		std::map<std::string, entry, std::less<>> logical;
		std::map<std::string, std::string, std::less<>> compiler_to_logical;
		for (std::size_t index = 0U; index < snapshot->files.size(); ++index)
		{
			const auto& item = snapshot->files[index];
			const auto blob = blobs.find(item.content_digest);
			if (blob == blobs.end())
				return std::unexpected(make_error(
					"source-closure.vfs-invalid-snapshot", item.logical_path, "missing-blob"));
			auto compiler = map_path(item.logical_path);
			if (!compiler)
				return std::unexpected(std::move(compiler.error()));
			entry selected{index, blob->second, *compiler};
			if (!logical.emplace(item.logical_path, selected).second ||
				!compiler_to_logical.emplace(*compiler, item.logical_path).second)
				return std::unexpected(make_error(
					"source-closure.vfs-path-collision", item.logical_path));
		}
		return read_only_compiler_vfs{
			std::move(snapshot), std::move(logical), std::move(compiler_to_logical)};
	}

	std::expected<opened_file, vfs_error>
	read_only_compiler_vfs::open_entry(
		const std::string& logical_path, const entry& selected) const
	{
		if (!snapshot_ || selected.file_index >= snapshot_->files.size() ||
			selected.blob_index >= snapshot_->blobs.size())
			return std::unexpected(make_error(
				"source-closure.vfs-invalid-snapshot", logical_path));
		const auto& file = snapshot_->files[selected.file_index];
		const auto& blob = snapshot_->blobs[selected.blob_index];
		if (file.logical_path != logical_path || file.content_digest != blob.content_digest ||
			file.size_bytes != blob.content.size())
			return std::unexpected(make_error(
				"source-closure.vfs-invalid-snapshot", logical_path));
		return opened_file{logical_path, selected.compiler_path, file.role, blob.content};
	}

	std::expected<opened_file, vfs_error>
	read_only_compiler_vfs::open_logical(const std::string_view logical_path) const
	{
		const auto found = logical_.find(logical_path);
		if (found == logical_.end())
			return std::unexpected(make_error(
				"source-closure.vfs-missing-input", std::string{logical_path}, "project-or-generated"));
		return open_entry(found->first, found->second);
	}

	std::expected<opened_file, vfs_error>
	read_only_compiler_vfs::open_compiler(const std::string_view compiler_path) const
	{
		const auto found = compiler_to_logical_.find(compiler_path);
		if (found == compiler_to_logical_.end())
			return std::unexpected(make_error(
				"source-closure.vfs-missing-input", std::string{compiler_path}, "project-or-generated"));
		return open_logical(found->second);
	}

	std::expected<std::string, vfs_error>
	read_only_compiler_vfs::map_logical_path(const std::string_view logical_path) const
	{
		const auto found = logical_.find(logical_path);
		if (found == logical_.end())
			return std::unexpected(make_error(
				"source-closure.vfs-missing-input", std::string{logical_path}, "project-or-generated"));
		return found->second.compiler_path;
	}

	std::expected<std::vector<std::string>, vfs_error>
	read_only_compiler_vfs::rewrite_invocation(const std::span<const std::string> arguments) const
	{
		std::vector<std::string> output;
		output.reserve(arguments.size());
		for (std::size_t index = 0U; index < arguments.size(); ++index)
		{
			const auto& argument = arguments[index];
			if (is_path_option(argument))
			{
				if (index + 1U >= arguments.size())
					return std::unexpected(make_error(
						"source-closure.vfs-invocation-invalid", argument, "missing-operand"));
				output.push_back(argument);
				auto mapped = map_path(arguments[++index]);
				if (!mapped)
					return std::unexpected(std::move(mapped.error()));
				output.push_back(std::move(*mapped));
				continue;
			}
			if (const auto joined = split_joined_path_option(argument))
			{
				auto mapped = map_path(joined->second);
				if (!mapped)
					return std::unexpected(std::move(mapped.error()));
				output.push_back(std::string{joined->first} + *mapped);
				continue;
			}
			if (argument.starts_with(project_scheme) || argument.starts_with(generated_scheme))
			{
				auto mapped = map_path(argument);
				if (!mapped)
					return std::unexpected(std::move(mapped.error()));
				output.push_back(std::move(*mapped));
				continue;
			}
			output.push_back(argument);
		}
		return output;
	}

	bool read_only_compiler_vfs::ambient_project_fallback_allowed() const noexcept
	{
		return false;
	}

	const std::string& read_only_compiler_vfs::snapshot_id() const noexcept
	{
		return snapshot_->snapshot_id;
	}
} // namespace cxxlens::detail::clang22::source_closure
