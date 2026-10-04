#include "application_analyzer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/cc_body.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_cfg_edge.hpp>
#include <cxxlens/relations/cc_cfg_node.hpp>
#include <cxxlens/relations/cc_declaration.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/cc_entity_detail.hpp>
#include <cxxlens/relations/cc_entity_edge.hpp>
#include <cxxlens/relations/cc_flow_fact.hpp>
#include <cxxlens/relations/cc_layout_fact.hpp>
#include <cxxlens/relations/cc_record_surface.hpp>
#include <cxxlens/relations/cc_syntax_node.hpp>
#include <cxxlens/relations/cc_type.hpp>
#include <cxxlens/relations/cc_type_component.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_include.hpp>
#include <cxxlens/relations/source_preprocessor_event.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <cxxlens/relations/source_token.hpp>
#include <cxxlens/relations/source_token_inventory.hpp>
#include <cxxlens/sdk/store.hpp>

#include "observation_v2.hpp"
#include "project_semantic_facts.hpp"
#include "provider_worker_v4_ast_observer.hpp"
#include "provider_worker_v4_output_normalizer.hpp"
#include "runtime/gcc_probe_process_port_internal.hpp"
#include "runtime/monotonic_clock_port_internal.hpp"
#include "runtime/sealed_executable_internal.hpp"
#include "sdk/application_query_export_internal.hpp"
#include "sdk/bounded_json_internal.hpp"
#include "sdk/compile_commands_capture_internal.hpp"
#include "source_closure_native.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		namespace fs = std::filesystem;
		using json = sdk::detail::json_value;

		struct analysis_failure
		{
			sdk::error value;
		};

		[[noreturn]] void fail(std::string field, std::string detail)
		{
			throw analysis_failure{
				{"application-analysis.clang22-invalid", std::move(field), std::move(detail)}};
		}

		template <class T>
		T take(sdk::result<T> value)
		{
			if (!value)
				throw analysis_failure{std::move(value.error())};
			return std::move(*value);
		}
		void take(sdk::result<void> value)
		{
			if (!value)
				throw analysis_failure{std::move(value.error())};
		}

		std::string digest(std::string_view domain, std::string_view value)
		{
			return take(sdk::semantic_digest(domain, value));
		}
		std::string content_digest(std::string_view value)
		{
			return sdk::content_digest(std::as_bytes(std::span{value}));
		}
		json text(std::string value)
		{
			return take(json::string(std::move(value)));
		}
		std::string arguments_json(const std::vector<std::string>& values)
		{
			json::array_type output;
			for (const auto& value : values)
				output.push_back(text(value));
			return sdk::detail::canonical_json(json::array(std::move(output)));
		}
		std::string abi_profile(const std::string_view macros,
								const std::vector<std::string>& invocation)
		{
			std::vector<std::string> parts;
			for (std::size_t begin{}; begin < macros.size();)
			{
				const auto end = macros.find('\n', begin);
				const auto line = macros.substr(
					begin, end == std::string_view::npos ? macros.size() - begin : end - begin);
				for (const auto prefix : {"#define __SIZEOF_",
										  "#define __ALIGNOF_",
										  "#define __BYTE_ORDER__",
										  "#define __ORDER_",
										  "#define __CHAR_UNSIGNED__",
										  "#define __WCHAR_",
										  "#define __WINT_",
										  "#define __LONG_DOUBLE_",
										  "#define __GXX_ABI_VERSION",
										  "#define _GLIBCXX_USE_CXX11_ABI",
										  "#define _GLIBCXX_DEBUG",
										  "#define _LIBCPP_ABI_",
										  "#define _ITERATOR_DEBUG_LEVEL"})
					if (line.starts_with(prefix))
					{
						parts.emplace_back(line);
						break;
					}
				if (end == std::string_view::npos)
					break;
				begin = end + 1U;
			}
			for (const auto& flag : invocation)
				for (const auto prefix : {"-m",
										  "-fabi",
										  "-fpack",
										  "-fshort",
										  "-funsigned-char",
										  "-fsigned-char",
										  "-fms-",
										  "-fno-rtti",
										  "-frtti",
										  "-freg-struct-return",
										  "-fpcc-struct-return"})
					if (flag.starts_with(prefix))
					{
						parts.push_back(flag);
						break;
					}
			return arguments_json(parts);
		}
		void replace_all(std::string& value, std::string_view from, std::string_view to)
		{
			if (from.empty())
				return;
			std::size_t offset{};
			while ((offset = value.find(from, offset)) != std::string::npos)
			{
				value.replace(offset, from.size(), to);
				offset += to.size();
			}
		}

		// Host filesystem port. Source bytes are owned before entering the compiler callback.
		class input_files
		{
		  public:
			fs::path canonical(const fs::path& path) const
			{
				std::error_code error;
				auto result = fs::canonical(path, error);
				if (error)
					fail("path", path.string() + ": " + error.message());
				return result;
			}
			std::string read(const fs::path& path, std::size_t maximum = 16U * 1024U * 1024U) const
			{
				std::error_code error;
				if (!fs::is_regular_file(path, error) || error)
					fail("input", "regular-file-required: " + path.string());
				const auto size = fs::file_size(path, error);
				if (error || size > maximum)
					fail("input", "byte-limit: " + path.string());
				std::ifstream stream{path, std::ios::binary};
				std::string output(static_cast<std::size_t>(size), '\0');
				if (!stream || !stream.read(output.data(), static_cast<std::streamsize>(size)) ||
					stream.peek() != std::char_traits<char>::eof())
					fail("input", "read-or-size-changed: " + path.string());
				return output;
			}
			std::shared_ptr<const std::string> freeze(const fs::path& path) const
			{
				const auto canonical_path = canonical(path);
				const auto found = frozen_.find(canonical_path);
				if (found != frozen_.end())
					return found->second;
				auto content = std::make_shared<const std::string>(read(canonical_path));
				frozen_.emplace(canonical_path, content);
				return content;
			}
			std::vector<fs::path> members(const fs::path& root, bool source_only) const
			{
				std::vector<fs::path> output;
				std::size_t visited{};
				for (fs::recursive_directory_iterator it{root}, end; it != end; ++it)
				{
					if (++visited > 100000U)
						fail("inputs", "directory-entry-limit");
					const auto& path = it->path();
					if (it->is_directory())
					{
						const auto name = path.filename().string();
						if (name == ".git" || name == ".cache" || name == ".codex" ||
							name == "node_modules")
							it.disable_recursion_pending();
						continue;
					}
					if (!it->is_regular_file())
						continue;
					const auto extension = path.extension().string();
					if (source_only && extension != ".cpp" && extension != ".cc" &&
						extension != ".cxx" && extension != ".c" && extension != ".h" &&
						extension != ".hpp" && extension != ".hxx" && extension != ".hh" &&
						extension != ".inc" && extension != ".inl")
						continue;
					if (it->is_symlink())
						fail("inputs", "symlink-source-unsupported: " + path.string());
					if (output.size() >= 4096U)
						fail("inputs", "source-member-limit");
					output.push_back(path);
				}
				std::ranges::sort(output);
				return output;
			}
			fs::path compiler(const std::string& name, const fs::path& directory) const
			{
				if (name.find('/') != std::string::npos)
					return canonical(fs::path{name}.is_absolute() ? fs::path{name}
																  : directory / name);
				const char* environment = std::getenv("PATH");
				std::string_view paths = environment == nullptr ? std::string_view{} : environment;
				while (!paths.empty())
				{
					const auto separator = paths.find(':');
					const auto part = paths.substr(0, separator);
					if (!part.empty())
					{
						const auto candidate = fs::path{part} / name;
						std::error_code error;
						if (candidate.is_absolute() && fs::is_regular_file(candidate, error) &&
							!error)
							return canonical(candidate);
					}
					if (separator == std::string_view::npos)
						break;
					paths.remove_prefix(separator + 1U);
				}
				fail("compiler", "executable-not-found: " + name);
			}

			std::shared_ptr<const sdk::detail::sealed_executable> compiler_image(
				const fs::path& compiler, const fs::path& directory, std::uint64_t deadline) const
			{
				const auto found = compilers_.find(compiler);
				if (found != compilers_.end())
					return found->second;
				if (compilers_.size() >= 32U)
					fail("compiler", "distinct-executable-limit");
				auto image = take(sdk::detail::open_sealed_executable({compiler.string(),
																	   directory.string(),
																	   deadline,
																	   512U * 1024U * 1024U,
																	   4096U,
																	   {}}));
				auto retained =
					std::make_shared<const sdk::detail::sealed_executable>(std::move(image));
				compilers_.emplace(compiler, retained);
				return retained;
			}

		  private:
			mutable std::map<fs::path, std::shared_ptr<const std::string>> frozen_;
			mutable std::map<fs::path, std::shared_ptr<const sdk::detail::sealed_executable>>
				compilers_;
		};

		bool beneath(const fs::path& path, const fs::path& root)
		{
			const auto relative = path.lexically_relative(root);
			return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
		}
		std::string logical_path(const fs::path& path, const fs::path& root)
		{
			if (!beneath(path, root))
				fail("project-root", "input-outside-project: " + path.string());
			const auto relative = path.lexically_relative(root).generic_string();
			return relative == "." ? "project://root" : "project://root/" + relative;
		}
		std::string normalized_path(const fs::path& input)
		{
			auto path = input.lexically_normal();
			if (path.has_relative_path() && path.filename().empty())
				path = path.parent_path();
			return path.string();
		}
		std::string trim(std::string value)
		{
			while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
				value.pop_back();
			return value;
		}
		std::vector<std::string> dependency_paths(std::string_view value)
		{
			const auto colon = value.find(':');
			if (colon == std::string_view::npos)
				fail("dependencies", "invalid-makefile-target");
			value.remove_prefix(colon + 1U);
			std::vector<std::string> paths;
			std::string token;
			for (std::size_t index{}; index < value.size(); ++index)
			{
				const auto character = value[index];
				if (character == '\\' && index + 1U < value.size())
				{
					const auto next = value[++index];
					if (next != '\n')
						token.push_back(next);
				}
				else if (character == '$' && index + 1U < value.size() && value[index + 1U] == '$')
				{
					token.push_back('$');
					++index;
				}
				else if (character == ' ' || character == '\t' || character == '\n' ||
						 character == '\r')
				{
					if (!token.empty())
					{
						paths.push_back(std::move(token));
						token.clear();
					}
				}
				else
					token.push_back(character);
			}
			if (!token.empty())
				paths.push_back(std::move(token));
			return paths;
		}
		sdk::detail::gcc_probe_process_output probe(const input_files& files,
													const fs::path& compiler,
													const fs::path& directory,
													std::vector<std::string> options)
		{
			options.insert(options.begin(), compiler.string());
			const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
								 runtime::monotonic_now().time_since_epoch())
								 .count();
			sdk::detail::gcc_probe_process_request request{
				std::move(options),
				directory.string(),
				{"LC_ALL=C", "LANG=C", "PATH=/usr/bin:/bin"},
				{4096U, 1024U * 1024U, 16U, 4096U, 4U * 1024U * 1024U, 512U * 1024U * 1024U, 4096U},
				static_cast<std::uint64_t>(now) + 30000000000ULL};
			request.executable_image =
				files.compiler_image(compiler, directory, request.absolute_wall_deadline_ns);
			auto result = take(sdk::detail::run_gcc_probe_process(request));
			if (result.terminal != sdk::detail::gcc_probe_process_terminal::exited ||
				result.exit_code != 0)
				fail("compiler-probe",
					 result.failure_stage + ": " + result.failure_detail + result.standard_error);
			return result;
		}
		sdk::detached_cell symbol(std::string domain, std::string value)
		{
			return {{sdk::scalar_kind::open_symbol, std::move(domain), false},
					sdk::cell_state::present,
					sdk::scalar_value{std::move(value)},
					std::nullopt};
		}
		sdk::detached_cell digest_cell(std::string value)
		{
			return {{sdk::scalar_kind::digest, {}, false},
					sdk::cell_state::present,
					sdk::scalar_value{std::move(value)},
					std::nullopt};
		}
		sdk::detached_cell id(std::string type, std::string value)
		{
			return sdk::detached_cell::typed(std::move(type), std::move(value));
		}
		sdk::detached_row row(const sdk::relation_descriptor& descriptor,
							  std::map<std::string, sdk::detached_cell, std::less<>> fields)
		{
			sdk::detached_row result{descriptor.id, {}};
			for (const auto& column : descriptor.columns)
			{
				const auto found = fields.find(column.name);
				if (found != fields.end())
					result.cells.emplace(column.id, std::move(found->second));
				else if (column.type.optional)
					result.cells.emplace(
						column.id,
						sdk::detached_cell{
							column.type, sdk::cell_state::absent, std::nullopt, std::nullopt});
			}
			if (descriptor.domain_identity.result_column)
			{
				const auto column =
					take(descriptor.column(*descriptor.domain_identity.result_column));
				result.cells.insert_or_assign(
					column.id,
					id(column.type.parameter,
					   take(sdk::derive_domain_identity(descriptor, result))));
			}
			take(sdk::validate_row(descriptor, result));
			take(sdk::validate_domain_identity(descriptor, result));
			return result;
		}
		std::string identity(const sdk::detached_row& value, std::string_view field)
		{
			return std::get<std::string>(
				*value.cells.at(value.descriptor_id + "." + std::string{field}).value);
		}

		struct prepared_unit
		{
			std::string main_path;
			std::string working_path;
			std::string compiler_version;
			std::string target;
			std::string language;
			std::string standard;
			std::string invocation;
			std::string toolchain_digest;
			std::string builtins;
			std::string macros;
			std::string search;
			std::string environment;
			fs::path compiler;
			source_closure_snapshot closure;
			std::vector<std::string> semantic;
			std::vector<std::string> effective;
		};

		struct analyzed_unit
		{
			std::vector<sdk::detached_row> rows;
			std::vector<sdk::provider::unresolved_item> unresolved;
			std::vector<std::string> limitations;
			std::string variant;
			std::string compile_unit;
			std::string project;
			std::string basis;
			std::string observed_snapshot;
		};

		prepared_unit prepare_unit(const sdk::detail::compile_command_entry& selected,
								   const fs::path& root,
								   const input_files& files)
		{
			const auto directory = files.canonical(selected.directory);
			const auto source =
				files.canonical(fs::path{selected.file}.is_absolute() ? fs::path{selected.file}
																	  : directory / selected.file);
			const auto main_path = logical_path(source, root);
			const auto working_path = logical_path(directory, root);
			const auto compiler = files.compiler(selected.arguments.front(), directory);
			for (const auto* name :
				 {"CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"})
				if (const auto* value = std::getenv(name); value != nullptr && *value != '\0')
					fail("environment",
						 std::string{name} + ": put include paths in compile_commands.json");
			const std::string compiler_version{compiled_clang22_version()};
			if (compiler_version.empty())
				fail("compiler", "Clang 22 frontend is unavailable in this build");
			const auto version = probe(files, compiler, directory, {"--version"});
			const auto expected_version = "clang version " + compiler_version;
			const auto matches_version = [&](const std::string_view prefix)
			{
				return version.standard_output.starts_with(prefix) &&
					version.standard_output.size() > prefix.size() &&
					(version.standard_output[prefix.size()] == ' ' ||
					 version.standard_output[prefix.size()] == '\n');
			};
			if (!matches_version(expected_version) &&
				!matches_version("Ubuntu " + expected_version))
				fail("compiler",
					 "Clang " + compiler_version + " required to match the analyzer frontend");
			const auto resource = files.canonical(
				trim(probe(files, compiler, directory, {"-print-resource-dir"}).standard_output));
			std::string language = source.extension() == ".c" ? "c" : "c++";
			std::string standard = language == "c" ? "gnu17" : "gnu++17";
			std::vector<std::string> flags;
			for (std::size_t index = 1U; index < selected.arguments.size(); ++index)
			{
				const auto& token = selected.arguments[index];
				if (token == "-c" || token == "-MD" || token == "-MMD" || token == "-MP")
					continue;
				if (token == "-o" || token == "-MF" || token == "-MT" || token == "-MQ")
				{
					if (++index == selected.arguments.size())
						fail("arguments", "missing-option-value");
					continue;
				}
				if (token.starts_with("-o") && token.size() > 2U)
					continue;
				if (token.starts_with("@") || token.starts_with("-fmodule") ||
					token.starts_with("-include-pch") || token.starts_with("-fplugin") ||
					token == "-Xclang" || token == "-ivfsoverlay" ||
					token.starts_with("--config") || token.starts_with("-resource-dir") ||
					token.starts_with("-save-temps"))
					fail("arguments", "unsupported-option: " + token);
				if (!token.empty() && token.front() != '-')
				{
					if (token == selected.file ||
						(directory / token).lexically_normal() == source ||
						fs::path{token} == source)
						continue;
					fail("arguments", "unexpected-positional-input: " + token);
				}
				if (token == "-x")
				{
					if (++index == selected.arguments.size())
						fail("arguments", "missing-language");
					language = selected.arguments[index];
					if (language != "c" && language != "c++")
						fail("arguments", "C-or-C++-required");
					continue;
				}
				if (token.starts_with("-std="))
					standard = token.substr(5U);
				if (token == "-I" || token == "-isystem" || token == "-iquote" ||
					token == "-idirafter" || token == "-include" || token == "-imacros" ||
					token == "--sysroot" || token == "-isysroot" || token == "--target" ||
					token == "-target" || token == "-D" || token == "-U")
				{
					flags.push_back(token);
					if (++index == selected.arguments.size())
						fail("arguments", "missing-option-value");
					const auto& value = selected.arguments[index];
					flags.push_back(
						token == "--target" || token == "-target" || token == "-D" || token == "-U"
							? value
							: normalized_path(fs::path{value}.is_absolute() ? fs::path{value}
																			: directory / value));
				}
				else if (token.starts_with("-I") && token.size() > 2U)
				{
					const auto path = fs::path{token.substr(2U)};
					flags.push_back("-I" +
									normalized_path(path.is_absolute() ? path : directory / path));
				}
				else
					flags.push_back(token);
			}
			flags.insert(flags.end(), {"-x", language, "-resource-dir", resource.string()});
			auto target_arguments = flags;
			target_arguments.push_back("-dumpmachine");
			const auto target = trim(
				probe(files, compiler, directory, std::move(target_arguments)).standard_output);
			if (!target.starts_with("x86_64-") || target.find("linux") == std::string::npos)
				fail("target", "Linux x86_64 required");
			auto macro_arguments = flags;
			macro_arguments.insert(macro_arguments.end(), {"-dM", "-E", "/dev/null"});
			const auto macros =
				probe(files, compiler, directory, std::move(macro_arguments)).standard_output;
			if (macros.find("#define __x86_64__ 1\n") == std::string::npos ||
				macros.find("#define __linux__ 1\n") == std::string::npos ||
				macros.find("#define __SIZEOF_POINTER__ 8\n") == std::string::npos)
				fail("target", "Linux x86_64 with 64-bit pointers required");
			auto search_arguments = flags;
			search_arguments.insert(search_arguments.end(), {"-E", "-v", "/dev/null"});
			auto search =
				probe(files, compiler, directory, std::move(search_arguments)).standard_error;
			std::string builtins;
			std::size_t builtin_bytes{};
			for (const auto& path : files.members(resource / "include", false))
			{
				const auto bytes = files.read(path);
				if (bytes.size() > 48U * 1024U * 1024U - builtin_bytes)
					fail("toolchain", "builtin-header-byte-limit");
				builtin_bytes += bytes.size();
				builtins += arguments_json(
					{path.lexically_relative(resource).generic_string(), content_digest(bytes)});
			}
			std::vector<source_closure_file_input> inputs;
			auto dependency_arguments = flags;
			dependency_arguments.insert(dependency_arguments.end(),
										{"-M", "-MT", "cxxlens-input", source.string()});
			const auto dependencies =
				probe(files, compiler, directory, std::move(dependency_arguments)).standard_output;
			std::set<fs::path> source_paths{source};
			for (const auto& dependency : dependency_paths(dependencies))
			{
				const auto path =
					files.canonical(fs::path{dependency}.is_absolute() ? fs::path{dependency}
																	   : directory / dependency);
				if (beneath(path, root))
					source_paths.insert(path);
			}
			std::size_t source_bytes{};
			for (const auto& path : source_paths)
			{
				auto bytes = files.freeze(path);
				if (bytes->size() > 48U * 1024U * 1024U - source_bytes)
					fail("inputs", "source-byte-limit");
				source_bytes += bytes->size();
				inputs.push_back(
					{logical_path(path, root),
					 path == source ? source_closure_role::main : source_closure_role::header,
					 source_closure_encoding::utf8,
					 std::move(bytes)});
			}
			auto closure = take(make_source_closure_snapshot(std::move(inputs)));
			const auto* main = closure.find_member(main_path);
			if (main == nullptr)
				fail("inputs", "main-member-missing");
			auto effective = flags;
			for (std::size_t index{}; index < effective.size(); ++index)
			{
				const auto token = effective[index];
				if (token == "-I" || token == "-isystem" || token == "-iquote" ||
					token == "-idirafter" || token == "-include" || token == "-imacros")
				{
					++index;
					const auto path = fs::path{effective[index]};
					if (beneath(path, root))
						effective[index] = logical_path(path, root);
				}
				else if (token.starts_with("-I") && token.size() > 2U)
				{
					const auto path = fs::path{token.substr(2U)};
					if (beneath(path, root))
						effective[index] = "-I" + logical_path(path, root);
				}
			}
			effective.insert(effective.begin(), compiler.string());
			effective.push_back(main_path);
			auto semantic = effective;
			semantic.front() = version.executable_digest;
			for (auto& flag : semantic)
				replace_all(flag, resource.string(), "toolchain://resource");
			replace_all(search, root.string(), "project://root");
			replace_all(
				search, compiler.parent_path().parent_path().string(), "toolchain://installation");
			const auto environment =
				content_digest("LC_ALL=C; explicit includes; no ambient include variables");
			const auto invocation =
				digest("cxxlens.local-clang22.invocation.v1", arguments_json(semantic));
			const auto toolchain_digest = digest(
				"cxxlens.local-clang22.toolchain.v1",
				arguments_json({version.executable_digest, compiler_version, target, builtins}));
			return {main_path,
					working_path,
					compiler_version,
					target,
					language,
					standard,
					invocation,
					toolchain_digest,
					builtins,
					macros,
					search,
					environment,
					compiler,
					std::move(closure),
					std::move(semantic),
					std::move(effective)};
		}

		analyzed_unit observe_unit(const prepared_unit& prepared,
								   const sdk::project_catalog& catalog,
								   const std::function<void(std::string_view)>& progress)
		{
			const auto& [main_path,
						 working_path,
						 compiler_version,
						 target,
						 language,
						 standard,
						 invocation,
						 toolchain_digest,
						 builtins,
						 macros,
						 search,
						 environment,
						 compiler,
						 closure,
						 semantic,
						 effective] = prepared;
			const auto* main = closure.find_member(main_path);
			std::vector<sdk::detached_row> rows;
			auto project = row(build::relations::project::descriptor(),
							   {{"catalog", id("catalog_id", catalog.catalog_id)},
								{"catalog_digest", digest_cell(catalog.catalog_digest)},
								{"logical_root", id("logical_path_id", catalog.logical_root)},
								{"environment_digest", digest_cell(environment)}});
			const auto project_id = identity(project, "project");
			rows.push_back(std::move(project));
			auto toolchain = row(
				build::relations::toolchain_context::descriptor(),
				{{"family", symbol("build.toolchain-family/1", "clang")},
				 {"exact_version", sdk::detached_cell::utf8(compiler_version)},
				 {"target_triple", sdk::detached_cell::utf8(target)},
				 {"builtin_headers_digest",
				  digest_cell(digest("cxxlens.local-clang22.builtin-headers.v1", builtins))},
				 {"abi_digest",
				  digest_cell(digest(
					  "cxxlens.local-clang22.abi-profile.v1",
					  arguments_json({toolchain_digest, target, abi_profile(macros, semantic)})))},
				 {"plugin_spec_digest",
				  digest_cell(digest("cxxlens.local-clang22.plugins.v1", "none"))}});
			const auto toolchain_id = identity(toolchain, "toolchain");
			rows.push_back(std::move(toolchain));
			auto variant_flags = semantic;
			variant_flags.pop_back();
			const auto variant_digest =
				digest("cxxlens.local-clang22.variant-flags.v1", arguments_json(variant_flags));
			auto variant =
				row(build::relations::variant::descriptor(),
					{{"project", id("project_id", project_id)},
					 {"toolchain", id("toolchain_context_id", toolchain_id)},
					 {"language", symbol("build.language/1", language)},
					 {"language_standard", symbol("build.language-standard/1", standard)},
					 {"target_triple", sdk::detached_cell::utf8(target)},
					 {"predefined_macros_digest",
					  digest_cell(digest("cxxlens.local-clang22.macros.v1", macros))},
					 {"include_search_digest",
					  digest_cell(digest("cxxlens.local-clang22.include-search.v1", search))},
					 {"semantic_flags_digest", digest_cell(variant_digest)}});
			const auto variant_id = identity(variant, "variant");
			rows.push_back(std::move(variant));
			std::string snapshot_id;
			for (const auto& member : closure.members)
			{
				auto source_row = row(
					source::relations::file::descriptor(),
					{{"file", id("file_id", member.file_id)},
					 {"project", id("project_id", project_id)},
					 {"logical_path", id("logical_path_id", member.logical_path)},
					 {"content", digest_cell(member.content_digest)},
					 {"size", sdk::detached_cell::unsigned_integer(member.size_bytes)},
					 {"encoding", symbol("source.encoding/1", "utf8")},
					 {"line_index",
					  id("line_index_id",
						 take(source_closure_member_line_index_id(closure, member.logical_path)))},
					 {"read_only", sdk::detached_cell::boolean(true)}});
				if (member.file_id == main->file_id)
					snapshot_id = identity(source_row, "snapshot");
				rows.push_back(std::move(source_row));
			}
			auto unit = row(build::relations::compile_unit::descriptor(),
							{{"project", id("project_id", project_id)},
							 {"main_source", id("source_snapshot_id", snapshot_id)},
							 {"variant", id("build_variant_id", variant_id)},
							 {"toolchain", id("toolchain_context_id", toolchain_id)},
							 {"effective_invocation_digest", digest_cell(invocation)},
							 {"language", symbol("build.language/1", language)},
							 {"working_directory", id("logical_path_id", working_path)}});
			const auto unit_id = identity(unit, "compile_unit");
			rows.push_back(std::move(unit));
			json::array_type semantic_arguments;
			for (const auto& argument : semantic)
				semantic_arguments.push_back(text(argument));
			const auto projection = sdk::detail::canonical_json(
				take(json::object({{"invocation", json::array(std::move(semantic_arguments))},
								   {"schema", text("cxxlens.local-clang22-input.v1")}})));
			source_closure_task_v4_input input;
			input.base_provider_task_id = "task:" + invocation;
			const auto bytes = std::as_bytes(std::span{projection});
			input.base_task_projection.assign(bytes.begin(), bytes.end());
			input.task_input_digest = content_digest(projection + closure.closure_digest);
			input.normalized_invocation_digest = invocation;
			input.toolchain_digest = toolchain_digest;
			input.environment_digest = environment;
			input.closure = closure;
			input.main_logical_path = main_path;
			input.logical_working_directory = working_path;
			source_closure_task_v4_decoded metadata{
				input, take(derive_source_closure_task_v4_identity(input))};
			std::vector<std::string> read_roots{compiler.parent_path().parent_path().string(),
												"/usr"};
			std::ranges::sort(read_roots);
			read_roots.erase(std::unique(read_roots.begin(), read_roots.end()), read_roots.end());
			std::optional<provider_worker_v4_ast_observation_batch> observations;
			std::optional<provider_worker_v4_normalized_output> normalized;
			std::optional<project_semantic_facts> facts;
			project_preprocessor_observations preprocessing;
			provider_worker_v4_output_normalizer_options normalization;
			normalization.toolchain_context_id = toolchain_id;
			take(with_source_closure_translation_unit(
				{closure, main_path, working_path, effective, std::move(read_roots), {}},
				[&](provider::clang22::borrowed_translation_unit& borrowed) -> sdk::result<void>
				{
					auto observed = observe_provider_worker_v4_ast(
						borrowed, metadata, unit_id, {}, snapshot_id, true);
					if (!observed)
						return sdk::unexpected(std::move(observed.error()));
					observations = std::move(*observed);
					if (progress)
						progress("normalizing " +
								 std::to_string(observations->observations.size()) +
								 " observations");
					auto canonical =
						normalize_provider_worker_v4_output(*observations, normalization);
					if (!canonical)
						return sdk::unexpected(std::move(canonical.error()));
					normalized = std::move(*canonical);
					auto detached = observe_project_semantics(
						borrowed, closure, *observations, *normalized, preprocessing, progress);
					if (!detached)
						return sdk::unexpected(std::move(detached.error()));
					facts = std::move(*detached);
					return {};
				},
				[&](clang::Preprocessor& preprocessor)
				{
					install_project_preprocessor_observer(preprocessor, closure, preprocessing);
				}));
			if (!observations || !normalized || !facts)
				fail("AST", "observer-not-called");
			std::map<std::string, materialization::observation_v2_primary_span, std::less<>> spans;
			for (const auto& observed : observations->observations)
				if (observed.primary_span)
					spans.emplace(observed.primary_span->span_id, *observed.primary_span);
			for (const auto& [span_id, span] : spans)
			{
				(void)span_id;
				rows.push_back(row(source::relations::span::descriptor(),
								   {{"snapshot", id("source_snapshot_id", span.snapshot)},
									{"file", id("file_id", span.file)},
									{"begin", sdk::detached_cell::unsigned_integer(span.begin)},
									{"end", sdk::detached_cell::unsigned_integer(span.end)},
									{"role", symbol("source.range-role/1", span.role)},
									{"read_only", sdk::detached_cell::boolean(span.read_only)}}));
			}
			for (const auto& batch : normalized->batches)
				rows.insert(rows.end(), batch.rows.begin(), batch.rows.end());
			rows.insert(rows.end(),
						std::make_move_iterator(facts->rows.begin()),
						std::make_move_iterator(facts->rows.end()));
			normalized->unresolved.insert(normalized->unresolved.end(),
										  std::make_move_iterator(facts->unresolved.begin()),
										  std::make_move_iterator(facts->unresolved.end()));
			const auto basis = content_digest(arguments_json(
				{closure.closure_digest, invocation, toolchain_digest, environment}));
			return {std::move(rows),
					std::move(normalized->unresolved),
					std::move(normalized->limitations),
					variant_id,
					unit_id,
					project_id,
					basis,
					closure.snapshot_id};
		}

		std::string analyze(const application_analysis_options& options)
		{
			if (options.project_root.empty() || options.compile_commands.empty())
				fail("options", "project-root-and-compile-commands-required");
			input_files files;
			const auto root = files.canonical(options.project_root);
			const auto database =
				take(sdk::detail::decode_compile_commands(files.read(options.compile_commands)));
			const auto selected_source = options.source_file.empty()
				? fs::path{}
				: files.canonical(fs::path{options.source_file}.is_absolute()
									  ? fs::path{options.source_file}
									  : root / options.source_file);
			std::vector<prepared_unit> prepared;
			std::map<std::string, sdk::error, std::less<>> failed;
			std::vector<sdk::catalog_compile_unit> entries;
			const auto environment =
				content_digest("LC_ALL=C; explicit includes; no ambient include variables");
			std::size_t selected_count{};
			for (const auto& entry : database.entries())
			{
				const auto source = files.canonical(fs::path{entry.file}.is_absolute()
														? fs::path{entry.file}
														: fs::path{entry.directory} / entry.file);
				if (!selected_source.empty() && selected_source != source)
					continue;
				++selected_count;
				try
				{
					if (options.progress)
						options.progress("freezing " + entry.file);
					auto value = prepare_unit(entry, root, files);
					const auto* main = value.closure.find_member(value.main_path);
					entries.push_back({"unit:" + value.invocation,
									   value.invocation,
									   main->content_digest,
									   value.environment});
					prepared.push_back(std::move(value));
				}
				catch (const analysis_failure& failure)
				{
					auto arguments = entry.arguments;
					for (auto& argument : arguments)
						replace_all(argument, root.string(), "project://root");
					const auto invocation =
						digest("cxxlens.local-clang22.unavailable-invocation.v1",
							   arguments_json(arguments));
					const auto key = logical_path(source, root) + ":" + invocation;
					failed.emplace(key, failure.value);
					entries.push_back({"unavailable-unit:" + invocation,
									   invocation,
									   content_digest(files.read(source)),
									   environment});
				}
			}
			if (selected_count == 0U)
				fail("compile-commands", "selected-source-not-found");
			std::ranges::sort(entries, {}, &sdk::catalog_compile_unit::compile_unit_id);
			entries.erase(std::ranges::unique(entries).begin(), entries.end());
			const auto catalog =
				take(sdk::project_catalog::make("project://root", environment, std::move(entries)));
			std::ranges::sort(prepared, {}, &prepared_unit::invocation);
			prepared.erase(std::unique(prepared.begin(),
									   prepared.end(),
									   [](const auto& a, const auto& b)
									   {
										   return a.invocation == b.invocation;
									   }),
						   prepared.end());
			sdk::relation_registry registry;
			const std::array<const sdk::relation_descriptor*, 28U> descriptors{
				&build::relations::project::descriptor(),
				&build::relations::toolchain_context::descriptor(),
				&build::relations::variant::descriptor(),
				&build::relations::compile_unit::descriptor(),
				&source::relations::file::descriptor(),
				&source::relations::span::descriptor(),
				&cc::relations::entity::descriptor(),
				&cc::relations::call_site::descriptor(),
				&cc::relations::call_direct_target::descriptor(),
				&cc::relations::entity_detail::descriptor(),
				&cc::relations::entity_edge::descriptor(),
				&cc::relations::syntax_node::descriptor(),
				&cc::relations::body::descriptor(),
				&cc::relations::cfg_node::descriptor(),
				&cc::relations::cfg_edge::descriptor(),
				&cc::relations::flow_fact::descriptor(),
				&cc::relations::layout_fact::descriptor(),
				&cc::relations::record_surface::descriptor(),
				&cc::relations::declaration::descriptor(),
				&cc::relations::type::descriptor(),
				&cc::relations::type_component::descriptor(),
				&source::relations::include::descriptor(),
				&source::relations::preprocessor_event::descriptor(),
				&source::relations::token::descriptor(),
				&source::relations::token_inventory::descriptor(),
				&materialization::entity_observation_v2_descriptor(),
				&materialization::call_observation_v2_descriptor(),
				&materialization::type_observation_v2_descriptor()};
			for (const auto* descriptor : descriptors)
				take(registry.add(*descriptor));
			auto engine = take(registry.build("cxxlens.local-clang22.v1"));
			const auto semantics = digest(
				"cxxlens.local-clang22.producer.v2",
				"Clang " + std::string{compiled_clang22_version()} +
					" AST observer v4; canonical normalizer v4; raw/expanded token domains v1; all "
					"selected compile units and variants");
			const auto universe = "local-clang22-project:" + catalog.catalog_id;
			const sdk::claim_producer producer{"cxxlens.clang22.local", semantics};
			sdk::snapshot_series_selector selector{
				catalog.catalog_id,
				"application",
				std::string{engine.generation()},
				universe,
				std::string{engine.registry_digest()},
				digest("cxxlens.local-clang22.interpretation.v1", "cc.clang22-canonical-1"),
				digest("cxxlens.local-clang22.execution.v1",
					   "explicit local compiler; in-process AST")};
			auto store = take(sdk::make_in_memory_snapshot_store(engine));
			auto writer =
				take(store.begin({selector, {1U, 0U, 0U}, catalog.catalog_digest, std::nullopt}));
			std::size_t successful{};
			for (const auto& unit : prepared)
			{
				std::optional<analyzed_unit> observed;
				try
				{
					if (options.progress)
						options.progress("parsing " + unit.main_path);
					observed = observe_unit(unit, catalog, options.progress);
				}
				catch (const analysis_failure& failure)
				{
					failed.emplace(unit.main_path + ":" + unit.invocation, failure.value);
					continue;
				}
				auto& value = *observed;
				const sdk::claim_condition condition{universe, {value.variant}};
				const sdk::claim_guarantee guarantee{
					"under_approximation",
					"selected-compile-units",
					"all selected variants; frozen project/header facts; "
					"system headers are live compiler inputs",
					{"static_analysis"}};
				sdk::claim_batch claims;
				for (auto& item : value.rows)
				{
					const bool native_observation = item.descriptor_id.starts_with("frontend.");
					auto asserted = take(sdk::make_assertion(engine,
															 {item,
															  condition,
															  "cc.clang22-canonical-1",
															  producer,
															  {value.basis},
															  value.observed_snapshot,
															  guarantee}));
					if (!native_observation)
						asserted = take(sdk::make_canonical_claim(
							engine, asserted, producer, std::move(item), semantics));
					take(claims.add(std::move(asserted)));
				}
				if (options.progress)
					options.progress("committing " + std::to_string(value.rows.size()) + " rows");
				auto committed = take(std::move(claims).commit(engine));
				if (options.progress)
					options.progress("claim batch committed");
				const auto claim_basis =
					take(sdk::claim_input_basis_digest(sdk::direct_claim_basis{value.basis}));
				std::map<std::string, std::vector<sdk::unresolved_reference>, std::less<>>
					unresolved_by_assertion;
				for (const auto& unresolved : committed.unresolved)
					unresolved_by_assertion[unresolved.source_assertion].push_back(unresolved);
				for (const auto* descriptor : descriptors)
				{
					std::map<std::string, std::vector<sdk::claim>, std::less<>> groups;
					for (const auto& claim : committed.claims)
						if (claim.descriptor == descriptor->id)
							groups[take(sdk::claim_input_basis_digest(claim.input_basis))]
								.push_back(claim);
					// TU-wide diagnostics belong to the frozen input, rather than every
					// independently derived assertion. Preserve them once in that basis.
					groups.try_emplace(claim_basis);
					if (options.progress)
						options.progress("staging " + descriptor->id + " (" +
										 std::to_string(groups.size()) + " partitions)");
					for (auto& [input_basis, group] : groups)
					{
						sdk::partition_draft partition{
							descriptor->id,
							value.compile_unit,
							condition,
							"cc.clang22-canonical-1",
							semantics,
							input_basis,
							"under_approximation",
							"selected-compile-unit",
							std::move(group),
							{{"compile-unit", value.compile_unit, "covered", {}}},
							{}};
						for (const auto& claim : partition.claims)
							if (const auto found = unresolved_by_assertion.find(claim.assertion);
								found != unresolved_by_assertion.end())
								partition.unresolved.insert(partition.unresolved.end(),
															found->second.begin(),
															found->second.end());
						if (input_basis == claim_basis &&
							(descriptor->id.starts_with("cc.") ||
							 descriptor->id == "source.preprocessor_event.v1"))
						{
							for (const auto& unresolved : value.unresolved)
							{
								if (unresolved.code.starts_with("body.") &&
									descriptor->id != "cc.body.v1" &&
									descriptor->id != "cc.syntax_node.v1")
									continue;
								if (unresolved.code.starts_with("flow.") &&
									descriptor->id != "cc.flow_fact.v1")
									continue;
								if (unresolved.code.starts_with("record.") &&
									descriptor->id != "cc.record_surface.v1")
									continue;
								if (unresolved.code.starts_with("declaration.") &&
									descriptor->id != "cc.entity_detail.v1")
									continue;
								if (unresolved.code.starts_with("type.") &&
									descriptor->id != "cc.type.v1" &&
									descriptor->id != "cc.type_component.v1" &&
									descriptor->id != "cc.entity_detail.v1")
									continue;
								if (unresolved.code.starts_with("preprocessor.") &&
									descriptor->id != "source.preprocessor_event.v1")
									continue;
								if (descriptor->id == "source.preprocessor_event.v1" &&
									!unresolved.code.starts_with("preprocessor."))
									continue;
								partition.coverage.push_back(
									{"provider-diagnostic",
									 unresolved.subject + ":" +
										 digest("cxxlens.application-diagnostic.v1",
												arguments_json({unresolved.code,
																unresolved.subject,
																unresolved.detail})),
									 "unresolved",
									 unresolved.code + ": " + unresolved.detail});
							}
							for (const auto& limitation : value.limitations)
								if (descriptor->id.starts_with("cc."))
									partition.coverage.push_back({"extraction-limitation",
																  limitation,
																  "unknown",
																  limitation});
						}
						std::ranges::sort(
							partition.coverage,
							[](const auto& left, const auto& right)
							{
								return std::tie(left.domain, left.key, left.state, left.reason) <
									std::tie(right.domain, right.key, right.state, right.reason);
							});
						partition.coverage.erase(std::ranges::unique(partition.coverage).begin(),
												 partition.coverage.end());
						take(writer.stage(std::move(partition)));
					}
				}
				++successful;
			}
			if (successful == 0U)
				throw analysis_failure{failed.begin()->second};
			for (const auto& [key, error] : failed)
			{
				const auto reason = error.code + ": " + error.field + ": " + error.detail;
				const sdk::claim_condition condition{
					universe,
					{"unavailable-unit:" + digest("cxxlens.local-clang22.failed-unit.v1", key)}};
				const auto basis = take(
					sdk::claim_input_basis_digest(sdk::direct_claim_basis{content_digest(key)}));
				for (const auto* descriptor : descriptors)
					take(writer.stage({descriptor->id,
									   key,
									   condition,
									   "cc.clang22-canonical-1",
									   semantics,
									   basis,
									   "unknown",
									   "failed-compile-unit",
									   {},
									   {{"compile-unit", key, "not_covered", reason}},
									   {}}));
			}
			if (options.progress)
				options.progress("validating snapshot");
			take(writer.validate());
			if (options.progress)
				options.progress("publishing snapshot");
			auto snapshot = take(writer.publish());
			if (options.progress)
				options.progress("exporting public relation scans");
			std::vector<std::string> relation_ids;
			for (const auto* descriptor : descriptors)
				relation_ids.push_back(descriptor->id);
			return take(sdk::detail::encode_application_queries(
				engine, snapshot, relation_ids, options.maximum_output_bytes));
		}

	} // namespace

	sdk::result<std::string> analyze_application(const application_analysis_options& options)
	{
		try
		{
			return analyze(options);
		}
		catch (const analysis_failure& error)
		{
			return sdk::unexpected(error.value);
		}
		catch (const fs::filesystem_error& error)
		{
			return sdk::unexpected(
				sdk::error{"application-analysis.input-unavailable", "filesystem", error.what()});
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				sdk::error{"application-analysis.resource-exhausted", "memory", "allocation"});
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(
				sdk::error{"application-analysis.resource-exhausted", "memory", "length"});
		}
	}
} // namespace cxxlens::detail::clang22
