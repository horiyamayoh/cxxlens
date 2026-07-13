#pragma once

#include <optional>
#include <string>

#include <cxxlens/source.hpp>

namespace cxxlens::detail::workspace_paths
{
	using semantic_path_result = std::optional<std::string>;
	struct build_path_roots
	{
		const path& project;
		const path& build;
	};

	[[nodiscard]] inline bool starts_outside(const path& relative)
	{
		const auto first = relative.begin();
		return first != relative.end() && *first == "..";
	}

	[[nodiscard]] inline semantic_path_result semantic_path(const path& root, path value)
	{
		const auto normalized_root = root.lexically_normal();
		if (normalized_root.empty())
			return std::nullopt;
		if (!normalized_root.is_absolute())
			return std::nullopt;
		if (value.is_relative())
			value = normalized_root / value;
		value = value.lexically_normal();
		if (!value.is_absolute())
			return std::nullopt;
		const auto relative = value.lexically_relative(normalized_root);
		if (relative.empty() || starts_outside(relative))
			return std::nullopt;
		return relative.generic_string();
	}

	[[nodiscard]] inline semantic_path_result build_path(const build_path_roots roots, path value)
	{
		if (auto project = semantic_path(roots.project, value))
			return project;
		if (roots.build.empty() || !roots.build.lexically_normal().is_absolute())
			return std::nullopt;
		if (value.is_relative())
			value = roots.build / value;
		value = value.lexically_normal();
		const auto relative = value.lexically_relative(roots.build.lexically_normal());
		if (relative.empty() || starts_outside(relative))
			return std::nullopt;
		if (relative == ".")
			return std::string{"$build"};
		return std::string{"$build/"} + relative.generic_string();
	}
} // namespace cxxlens::detail::workspace_paths
