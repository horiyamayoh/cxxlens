#include "source_closure_invocation.hpp"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "source_closure.hpp"
#include "source_closure_vfs.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		constexpr std::size_t maximum_argument_count = 4096U;
		constexpr std::size_t maximum_argument_bytes = 4096U;
		constexpr std::string_view project_prefix{"project://"};

		enum class path_authority : unsigned char
		{
			project_or_qualified,
			qualified_only,
		};

		struct separate_path_option
		{
			std::string_view spelling;
			path_authority authority;
		};

		constexpr separate_path_option separate_options[]{
			{"-I", path_authority::project_or_qualified},
			{"-isystem", path_authority::project_or_qualified},
			{"-iquote", path_authority::project_or_qualified},
			{"-idirafter", path_authority::project_or_qualified},
			{"-include", path_authority::project_or_qualified},
			{"-imacros", path_authority::project_or_qualified},
			{"-resource-dir", path_authority::qualified_only},
			{"--sysroot", path_authority::qualified_only},
			{"-isysroot", path_authority::qualified_only},
			{"--gcc-toolchain", path_authority::qualified_only},
			{"-gcc-toolchain", path_authority::qualified_only},
		};

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool control_free(const std::string_view value) noexcept
		{
			return std::ranges::none_of(value,
										[](const unsigned char character)
										{
											return character < 0x20U || character == 0x7fU;
										});
		}

		[[nodiscard]] sdk::result<std::string>
		canonical_qualified_root(const std::string_view value)
		{
			if (value.empty() || value.size() > maximum_argument_bytes || value.front() != '/' ||
				value.find('\0') != std::string_view::npos || value.contains('\\') ||
				!control_free(value) || (value.size() > 1U && value.back() == '/'))
				return sdk::unexpected(failure("source-closure.toolchain-input-unqualified",
											   "qualified-read-root",
											   std::string{value}));

			std::size_t begin{1U};
			while (begin <= value.size())
			{
				const auto end = value.find('/', begin);
				const auto segment = value.substr(
					begin, end == std::string_view::npos ? value.size() - begin : end - begin);
				if (segment.empty() || segment == "." || segment == "..")
					return sdk::unexpected(failure("source-closure.toolchain-input-unqualified",
												   "qualified-read-root",
												   std::string{value}));
				if (end == std::string_view::npos)
					break;
				begin = end + 1U;
			}
			return std::string{value};
		}

		[[nodiscard]] bool beneath_root(const std::string_view path,
										const std::string_view root) noexcept
		{
			if (root == "/")
				return path.starts_with('/');
			return path == root ||
				(path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/');
		}

		[[nodiscard]] sdk::result<std::string>
		qualified_path(const std::string_view value, const std::span<const std::string> roots)
		{
			auto canonical = canonical_qualified_root(value);
			if (!canonical)
				return sdk::unexpected(std::move(canonical.error()));
			if (std::ranges::none_of(roots,
									 [&canonical](const std::string& root)
									 {
										 return beneath_root(*canonical, root);
									 }))
				return sdk::unexpected(failure("source-closure.toolchain-input-unqualified",
											   "compiler-path",
											   std::move(*canonical)));
			return canonical;
		}

		[[nodiscard]] sdk::result<std::string> project_path(const std::string_view value)
		{
			auto relative = source_closure_relative_path(value);
			if (!relative)
				return sdk::unexpected(std::move(relative.error()));
			return std::string{source_closure_vfs::synthetic_root()} + "/" + *relative;
		}

		[[nodiscard]] sdk::result<std::string>
		rewrite_path(const std::string_view value,
					 const path_authority authority,
					 const std::span<const std::string> qualified_roots)
		{
			if (value.starts_with(project_prefix))
			{
				if (authority == path_authority::qualified_only)
					return sdk::unexpected(failure("source-closure.toolchain-input-unqualified",
												   "compiler-path",
												   std::string{value}));
				return project_path(value);
			}
			if (!value.empty() && value.front() == '/')
				return qualified_path(value, qualified_roots);
			return sdk::unexpected(failure(
				"source-closure.ambient-fallback-denied", "compiler-path", std::string{value}));
		}

		[[nodiscard]] bool unsupported_path_option(const std::string_view argument) noexcept
		{
			constexpr std::string_view exact[]{"-include-pch", "-ivfsoverlay"};
			if (std::ranges::find(exact, argument) != std::end(exact))
				return true;
			constexpr std::string_view prefixes[]{
				"-include-pch=",
				"-ivfsoverlay=",
				"-fmodule-file=",
				"-fmodule-map-file=",
				"-fmodules-cache-path=",
				"-fprebuilt-module-path=",
			};
			return std::ranges::any_of(prefixes,
									   [argument](const std::string_view prefix)
									   {
										   return argument.starts_with(prefix);
									   });
		}

		[[nodiscard]] sdk::result<void> validate_argument(const std::string_view argument)
		{
			if (argument.empty() || argument.size() > maximum_argument_bytes ||
				argument.find('\0') != std::string_view::npos || !control_free(argument))
				return sdk::unexpected(failure(
					"source-closure.path-invalid", "compiler-argument", std::string{argument}));
			if (argument.front() == '@')
				return sdk::unexpected(failure("source-closure.ambient-fallback-denied",
											   "response-file",
											   std::string{argument}));
			if (unsupported_path_option(argument))
				return sdk::unexpected(failure("source-closure.toolchain-input-unqualified",
											   "unsupported-input-kind",
											   std::string{argument}));
			return {};
		}

		[[nodiscard]] const separate_path_option*
		find_separate_option(const std::string_view argument) noexcept
		{
			const auto found =
				std::ranges::find(separate_options, argument, &separate_path_option::spelling);
			return found == std::end(separate_options) ? nullptr : &*found;
		}

		struct joined_option
		{
			std::string_view prefix;
			std::string_view separator;
			path_authority authority;
		};

		[[nodiscard]] sdk::result<std::string>
		rewrite_joined_option(const std::string_view argument,
							  const std::span<const std::string> qualified_roots)
		{
			constexpr joined_option joined[]{
				{"-resource-dir=", "", path_authority::qualified_only},
				{"--sysroot=", "", path_authority::qualified_only},
				{"--gcc-toolchain=", "", path_authority::qualified_only},
				{"-gcc-toolchain=", "", path_authority::qualified_only},
				{"-isystem", "", path_authority::project_or_qualified},
				{"-iquote", "", path_authority::project_or_qualified},
				{"-idirafter", "", path_authority::project_or_qualified},
				{"-isysroot", "", path_authority::qualified_only},
				{"-imacros", "", path_authority::project_or_qualified},
				{"-include", "", path_authority::project_or_qualified},
				{"-I", "", path_authority::project_or_qualified},
			};
			for (const auto& option : joined)
			{
				if (argument.size() <= option.prefix.size() || !argument.starts_with(option.prefix))
					continue;
				auto rewritten = rewrite_path(
					argument.substr(option.prefix.size()), option.authority, qualified_roots);
				if (!rewritten)
					return sdk::unexpected(std::move(rewritten.error()));
				return std::string{option.prefix} + std::string{option.separator} + *rewritten;
			}
			if (argument.contains(project_prefix))
				return sdk::unexpected(failure(
					"source-closure.path-invalid", "hidden-project-path", std::string{argument}));
			return std::string{argument};
		}
	} // namespace

	sdk::result<source_closure_invocation>
	prepare_source_closure_invocation(const std::span<const std::string> effective_arguments,
									  const std::string_view main_logical_path,
									  const std::string_view logical_working_directory,
									  const std::span<const std::string> qualified_read_roots)
	{
		if (effective_arguments.size() < 2U || effective_arguments.size() > maximum_argument_count)
			return sdk::unexpected(failure("source-closure.limit-exceeded", "effective-arguments"));

		auto compiler_filename = project_path(main_logical_path);
		if (!compiler_filename)
			return sdk::unexpected(std::move(compiler_filename.error()));
		auto working_directory = project_path(logical_working_directory);
		if (!working_directory)
			return sdk::unexpected(std::move(working_directory.error()));
		if (effective_arguments.back() != main_logical_path)
			return sdk::unexpected(failure(
				"source-closure.main-invalid", "effective-arguments", effective_arguments.back()));

		source_closure_invocation output;
		output.compiler_filename = std::move(*compiler_filename);
		output.working_directory = std::move(*working_directory);
		output.qualified_read_roots.reserve(qualified_read_roots.size());
		for (const auto& root : qualified_read_roots)
		{
			auto canonical = canonical_qualified_root(root);
			if (!canonical)
				return sdk::unexpected(std::move(canonical.error()));
			output.qualified_read_roots.push_back(std::move(*canonical));
		}
		std::ranges::sort(output.qualified_read_roots);
		if (std::ranges::adjacent_find(output.qualified_read_roots) !=
			output.qualified_read_roots.end())
			return sdk::unexpected(
				failure("source-closure.toolchain-input-unqualified", "duplicate-qualified-root"));

		if (auto argv0 = validate_argument(effective_arguments.front()); !argv0)
			return sdk::unexpected(std::move(argv0.error()));
		auto tool_name = qualified_path(effective_arguments.front(), output.qualified_read_roots);
		if (!tool_name)
			return sdk::unexpected(std::move(tool_name.error()));
		output.tool_name = std::move(*tool_name);
		output.compiler_arguments.reserve(effective_arguments.size() - 2U);
		for (std::size_t index = 1U; index + 1U < effective_arguments.size(); ++index)
		{
			const auto& argument = effective_arguments[index];
			if (auto valid = validate_argument(argument); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (const auto* option = find_separate_option(argument); option != nullptr)
			{
				if (index + 2U >= effective_arguments.size())
					return sdk::unexpected(
						failure("source-closure.path-invalid", "missing-option-value", argument));
				const auto& value = effective_arguments[++index];
				if (auto valid = validate_argument(value); !valid)
					return sdk::unexpected(std::move(valid.error()));
				auto rewritten =
					rewrite_path(value, option->authority, output.qualified_read_roots);
				if (!rewritten)
					return sdk::unexpected(std::move(rewritten.error()));
				output.compiler_arguments.push_back(argument);
				output.compiler_arguments.push_back(std::move(*rewritten));
				continue;
			}
			auto rewritten = rewrite_joined_option(argument, output.qualified_read_roots);
			if (!rewritten)
				return sdk::unexpected(std::move(rewritten.error()));
			output.compiler_arguments.push_back(std::move(*rewritten));
		}
		return output;
	}
} // namespace cxxlens::detail::clang22
