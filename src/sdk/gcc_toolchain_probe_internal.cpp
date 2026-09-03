#include "gcc_toolchain_probe_internal.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "gcc_capture_file_port_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		constexpr std::string_view pinned_version{"16.2.0"};
		constexpr std::array pinned_target_triples{
			std::string_view{"x86_64-linux-gnu"},
			std::string_view{"x86_64-pc-linux-gnu"},
		};

		[[nodiscard]] bool pinned_target_abi(const std::string_view value) noexcept
		{
			return std::ranges::find(pinned_target_triples, value) != pinned_target_triples.end();
		}

		[[nodiscard]] error unavailable(std::string field, std::string detail)
		{
			return {"application-analysis.gcc-toolchain-unavailable",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] error invalid(std::string field, std::string detail)
		{
			return {"application-analysis.gcc-toolchain-observation-invalid",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] std::string_view
		terminal_name(const gcc_probe_process_terminal terminal) noexcept
		{
			switch (terminal)
			{
				case gcc_probe_process_terminal::exited:
					return "exited";
				case gcc_probe_process_terminal::crashed:
					return "crashed";
				case gcc_probe_process_terminal::timed_out:
					return "timed-out";
				case gcc_probe_process_terminal::cancelled:
					return "cancelled";
				case gcc_probe_process_terminal::output_limit:
					return "output-limit";
				case gcc_probe_process_terminal::launch_failed:
					return "launch-failed";
				case gcc_probe_process_terminal::unavailable:
					return "unavailable";
			}
			return "invalid-terminal";
		}

		[[nodiscard]] bool sha256_digest(const std::string_view value) noexcept
		{
			if (value.size() != 71U || !value.starts_with("sha256:"))
				return false;
			return std::ranges::all_of(value.substr(7U),
									   [](const char byte)
									   {
										   return (byte >= '0' && byte <= '9') ||
											   (byte >= 'a' && byte <= 'f');
									   });
		}

		[[nodiscard]] result<std::string> single_line(std::string_view value,
													  const std::string_view field,
													  const bool allow_empty = false)
		{
			if (value.ends_with('\n'))
				value.remove_suffix(1U);
			if (value.ends_with('\r'))
				value.remove_suffix(1U);
			if ((!allow_empty && value.empty()) || value.contains('\n') || value.contains('\r') ||
				value.contains('\0'))
				return unexpected(invalid(std::string{field}, "single-line"));
			return std::string{value};
		}

		struct executable_identity
		{
			std::string path;
			std::string digest;
			std::uint64_t bytes{};
		};

		[[nodiscard]] result<gcc_probe_process_output>
		execute_probe(gcc_probe_process_port& processes,
					  const gcc_toolchain_probe_request& request,
					  std::vector<std::string> arguments,
					  const std::string_view field,
					  std::optional<executable_identity>& identity,
					  const std::stop_token& cancellation)
		{
			gcc_probe_process_request process_request;
			process_request.argv.reserve(arguments.size() + 1U);
			process_request.argv.push_back(request.compiler_path);
			process_request.argv.insert(process_request.argv.end(),
										std::make_move_iterator(arguments.begin()),
										std::make_move_iterator(arguments.end()));
			process_request.working_directory = request.working_directory;
			process_request.environment.reserve(request.execution_environment.size() + 1U);
			for (const auto& entry : request.execution_environment)
			{
				const auto separator = entry.find('=');
				if (separator != std::string::npos &&
					std::string_view{entry.data(), separator} == "LC_ALL")
					continue;
				process_request.environment.push_back(entry);
			}
			process_request.environment.emplace_back("LC_ALL=C");
			process_request.limits = request.process_limits;
			process_request.absolute_wall_deadline_ns = request.absolute_wall_deadline_ns;
			auto executed = processes.run(process_request, cancellation);
			if (!executed)
				return unexpected(std::move(executed.error()));
			if (executed->terminal != gcc_probe_process_terminal::exited ||
				executed->exit_code != 0)
			{
				auto detail = std::string{terminal_name(executed->terminal)};
				if (executed->terminal == gcc_probe_process_terminal::exited)
					detail += ":exit=" + std::to_string(executed->exit_code);
				else if (!executed->failure_stage.empty())
					detail += ":" + executed->failure_stage + ":" + executed->failure_detail;
				return unexpected(unavailable(std::string{field}, std::move(detail)));
			}
			if (executed->executable_path.empty() || executed->executable_path.front() != '/' ||
				executed->executable_path.contains('\0') ||
				!sha256_digest(executed->executable_digest) || executed->executable_bytes == 0U)
				return unexpected(invalid(std::string{field}, "executable-identity"));
			if (!identity)
				identity = executable_identity{executed->executable_path,
											   executed->executable_digest,
											   executed->executable_bytes};
			else if (identity->path != executed->executable_path ||
					 identity->digest != executed->executable_digest ||
					 identity->bytes != executed->executable_bytes)
				return unexpected(invalid("compiler", "changed-during-probe"));
			return std::move(*executed);
		}

		[[nodiscard]] captured_text_observation observed(std::string value)
		{
			return {capture_observation_state::observed, std::move(value), {}, {}};
		}

		[[nodiscard]] captured_text_observation missing(std::string reason, std::string action)
		{
			return {capture_observation_state::unavailable,
					std::nullopt,
					std::move(reason),
					std::move(action)};
		}

		[[nodiscard]] result<std::string>
		observation_digest(const std::string_view domain,
						   const std::span<const canonical_value> fields)
		{
			auto encoded = canonical_binary(canonical_value::from_tuple(
				std::vector<canonical_value>{fields.begin(), fields.end()}));
			if (!encoded)
				return unexpected(std::move(encoded.error()));
			return semantic_digest(
				domain, {reinterpret_cast<const char*>(encoded->data()), encoded->size()});
		}

		[[nodiscard]] bool path_prefix(const std::string_view path,
									   const std::string_view prefix) noexcept
		{
			return path == prefix ||
				(path.size() > prefix.size() && path.starts_with(prefix) &&
				 (prefix == "/" || path[prefix.size()] == '/'));
		}

		[[nodiscard]] std::string parent_path(const std::string_view path)
		{
			const auto separator = path.find_last_of('/');
			if (separator == 0U)
				return "/";
			if (separator == std::string_view::npos)
				return {};
			return std::string{path.substr(0U, separator)};
		}

		[[nodiscard]] std::string normalize_search_path(const std::string_view path,
														const std::string_view compiler_prefix,
														const std::string_view sysroot)
		{
			if (!sysroot.empty() && path_prefix(path, sysroot))
				return "$sysroot" + std::string{path.substr(sysroot.size())};
			if (compiler_prefix != "/" && path_prefix(path, compiler_prefix))
				return "$compiler-prefix" + std::string{path.substr(compiler_prefix.size())};
			return "$host-root" + std::string{path};
		}

		struct include_search_input
		{
			std::string_view diagnostics;
			std::string_view compiler_path;
			std::string_view sysroot;
		};

		[[nodiscard]] result<std::string>
		normalized_include_search(const include_search_input input)
		{
			const auto diagnostics = input.diagnostics;
			constexpr std::array markers{std::string_view{"#include \"...\" search starts here:"},
										 std::string_view{"#include <...> search starts here:"}};
			std::size_t begin = std::string_view::npos;
			for (const auto marker : markers)
			{
				const auto found = diagnostics.find(marker);
				if (found != std::string_view::npos)
					begin = std::min(begin, found);
			}
			if (begin == std::string_view::npos)
				return unexpected(invalid("include-search", "missing-start-marker"));
			begin = diagnostics.find('\n', begin);
			if (begin == std::string_view::npos)
				return unexpected(invalid("include-search", "truncated-start-marker"));
			++begin;
			const auto end = diagnostics.find("End of search list.", begin);
			if (end == std::string_view::npos)
				return unexpected(invalid("include-search", "missing-end-marker"));
			const auto compiler_prefix = parent_path(parent_path(input.compiler_path));
			std::string normalized;
			std::size_t offset = begin;
			while (offset < end)
			{
				const auto next = diagnostics.find('\n', offset);
				auto line = diagnostics.substr(
					offset, std::min(next == std::string_view::npos ? end : next, end) - offset);
				if (line.ends_with('\r'))
					line.remove_suffix(1U);
				while (!line.empty() && line.front() == ' ')
					line.remove_prefix(1U);
				while (!line.empty() && line.back() == ' ')
					line.remove_suffix(1U);
				if (!line.empty() && !line.starts_with("#include"))
				{
					if (line.front() != '/' || line.contains('\0'))
						return unexpected(invalid("include-search", "non-absolute-entry"));
					normalized += normalize_search_path(line, compiler_prefix, input.sysroot);
					normalized.push_back('\n');
				}
				if (next == std::string_view::npos || next >= end)
					break;
				offset = next + 1U;
			}
			if (normalized.empty())
				return unexpected(invalid("include-search", "empty"));
			return normalized;
		}
	} // namespace

	result<gcc_toolchain_observation>
	probe_gcc_toolchain(gcc_probe_process_port& processes,
						const gcc_toolchain_probe_request& request,
						const std::stop_token& cancellation,
						gcc_capture_file_port* files)
	{
		try
		{
			std::optional<executable_identity> identity;
			auto version_output = execute_probe(processes,
												request,
												{"-dumpfullversion", "-dumpversion"},
												"exact-version",
												identity,
												cancellation);
			if (!version_output)
				return unexpected(std::move(version_output.error()));
			auto version = single_line(version_output->standard_output, "exact-version");
			if (!version)
				return unexpected(std::move(version.error()));
			if (*version != pinned_version)
				return unexpected(
					unavailable("exact-version", "expected-16.2.0:observed-" + *version));

			auto target_output = execute_probe(
				processes, request, {"-dumpmachine"}, "target-triple", identity, cancellation);
			if (!target_output)
				return unexpected(std::move(target_output.error()));
			auto target = single_line(target_output->standard_output, "target-triple");
			if (!target)
				return unexpected(std::move(target.error()));
			if (!pinned_target_abi(*target))
				return unexpected(unavailable("target-triple",
											  "expected-x86_64-linux-gnu-abi:observed-" + *target));

			auto sysroot_output = execute_probe(
				processes, request, {"--print-sysroot"}, "sysroot", identity, cancellation);
			if (!sysroot_output)
				return unexpected(std::move(sysroot_output.error()));
			auto sysroot = single_line(sysroot_output->standard_output, "sysroot", true);
			if (!sysroot)
				return unexpected(std::move(sysroot.error()));
			if (!sysroot->empty() && sysroot->front() != '/')
				return unexpected(invalid("sysroot", "non-absolute"));

			auto macros_output = execute_probe(processes,
											   request,
											   {"-dM", "-E", "-x", "c++", "-std=gnu++23", "-"},
											   "builtin-macros",
											   identity,
											   cancellation);
			if (!macros_output)
				return unexpected(std::move(macros_output.error()));
			if (macros_output->standard_output.empty() ||
				macros_output->standard_output.contains('\0'))
				return unexpected(invalid("builtin-macros", "empty-or-binary"));
			const std::array macro_fields{
				canonical_value::from_string(macros_output->standard_output)};
			auto macros_digest = observation_digest("gcc-builtin-macros-v1", macro_fields);
			if (!macros_digest)
				return unexpected(std::move(macros_digest.error()));

			auto include_output = execute_probe(processes,
												request,
												{"-E", "-x", "c++", "-std=gnu++23", "-v", "-"},
												"include-search",
												identity,
												cancellation);
			if (!include_output)
				return unexpected(std::move(include_output.error()));
			auto include_search = normalized_include_search(
				{include_output->standard_error, identity->path, *sysroot});
			if (!include_search)
				return unexpected(std::move(include_search.error()));
			const std::array include_fields{canonical_value::from_string(*include_search)};
			auto include_digest = observation_digest("gcc-include-search-v1", include_fields);
			if (!include_digest)
				return unexpected(std::move(include_digest.error()));
			captured_text_observation builtin_headers = missing(
				"builtin-header-content-unobserved", "capture-builtin-header-source-closure");
			if (files != nullptr)
			{
				auto builtin_directory_output = execute_probe(processes,
															  request,
															  {"-print-file-name=include"},
															  "builtin-header-directory",
															  identity,
															  cancellation);
				if (!builtin_directory_output)
					return unexpected(std::move(builtin_directory_output.error()));
				auto builtin_directory = single_line(builtin_directory_output->standard_output,
													 "builtin-header-directory");
				if (!builtin_directory || builtin_directory->front() != '/')
					return unexpected(!builtin_directory
										  ? std::move(builtin_directory.error())
										  : invalid("builtin-header-directory", "non-absolute"));
				auto tree_digest = files->digest_regular_tree(*builtin_directory,
															  {request.maximum_builtin_header_bytes,
															   request.maximum_builtin_header_files,
															   request.maximum_path_bytes});
				if (!tree_digest)
					return unexpected(std::move(tree_digest.error()));
				builtin_headers = observed(std::move(*tree_digest));
			}
			const std::array abi_fields{canonical_value::from_string(*version),
										canonical_value::from_string(*target),
										canonical_value::from_string(*macros_digest)};
			auto abi_digest = observation_digest("gcc-abi-observation-v1", abi_fields);
			if (!abi_digest)
				return unexpected(std::move(abi_digest.error()));

			gcc_toolchain_observation observation;
			observation.exact_version = std::move(*version);
			observation.canonical_binary_path = observed(identity->path);
			observation.binary_digest = observed(identity->digest);
			observation.target_triple = std::move(*target);
			observation.sysroot = sysroot->empty()
				? missing("compiler-reported-empty-sysroot", "capture-effective-system-roots")
				: observed(std::move(*sysroot));
			observation.abi_digest = observed(std::move(*abi_digest));
			observation.builtin_headers_digest = std::move(builtin_headers);
			observation.builtin_macros_digest = observed(std::move(*macros_digest));
			observation.include_search_digest = observed(std::move(*include_digest));
			return observation;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(invalid("probe", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(invalid("probe", "allocation"));
		}
	}
} // namespace cxxlens::sdk::detail
