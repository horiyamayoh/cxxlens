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

#include <clang/Basic/Version.h>
#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <cxxlens/sdk/store.hpp>

#include "observation_v2.hpp"
#include "provider_worker_v4_output_normalizer.hpp"
#include "runtime/gcc_probe_process_port_internal.hpp"
#include "runtime/monotonic_clock_port_internal.hpp"
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
		sdk::detail::gcc_probe_process_output
		probe(const fs::path& compiler, const fs::path& directory, std::vector<std::string> options)
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

		std::string analyze(const application_analysis_options& options)
		{
			if (options.project_root.empty() || options.compile_commands.empty())
				fail("options", "project-root-and-compile-commands-required");
			input_files files;
			const auto root = files.canonical(options.project_root);
			const auto database =
				take(sdk::detail::decode_compile_commands(files.read(options.compile_commands)));
			const sdk::detail::compile_command_entry* selected{};
			for (const auto& entry : database.entries())
			{
				const auto source = files.canonical(fs::path{entry.file}.is_absolute()
														? fs::path{entry.file}
														: fs::path{entry.directory} / entry.file);
				if (!options.source_file.empty() &&
					source !=
						files.canonical(fs::path{options.source_file}.is_absolute()
											? fs::path{options.source_file}
											: root / options.source_file))
					continue;
				if (selected != nullptr)
					fail("compile-commands",
						 "ambiguous-variants: select --file and keep one command for that file");
				selected = &entry;
			}
			if (selected == nullptr)
				fail("compile-commands", "selected-source-not-found");
			const auto directory = files.canonical(selected->directory);
			const auto source = files.canonical(fs::path{selected->file}.is_absolute()
													? fs::path{selected->file}
													: directory / selected->file);
			const auto main_path = logical_path(source, root);
			const auto working_path = logical_path(directory, root);
			const auto compiler = files.compiler(selected->arguments.front(), directory);
			for (const auto* name :
				 {"CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"})
				if (const auto* value = std::getenv(name); value != nullptr && *value != '\0')
					fail("environment",
						 std::string{name} + ": put include paths in compile_commands.json");
			const auto version = probe(compiler, directory, {"--version"});
			const std::string compiler_version{CLANG_VERSION_STRING};
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
				trim(probe(compiler, directory, {"-print-resource-dir"}).standard_output));
			std::string language = source.extension() == ".c" ? "c" : "c++";
			std::string standard = language == "c" ? "gnu17" : "gnu++17";
			std::vector<std::string> flags;
			for (std::size_t index = 1U; index < selected->arguments.size(); ++index)
			{
				const auto& token = selected->arguments[index];
				if (token == "-c" || token == "-MD" || token == "-MMD" || token == "-MP")
					continue;
				if (token == "-o" || token == "-MF" || token == "-MT" || token == "-MQ")
				{
					if (++index == selected->arguments.size())
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
					if (token == selected->file ||
						(directory / token).lexically_normal() == source ||
						fs::path{token} == source)
						continue;
					fail("arguments", "unexpected-positional-input: " + token);
				}
				if (token == "-x")
				{
					if (++index == selected->arguments.size())
						fail("arguments", "missing-language");
					language = selected->arguments[index];
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
					if (++index == selected->arguments.size())
						fail("arguments", "missing-option-value");
					const auto& value = selected->arguments[index];
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
			const auto target =
				trim(probe(compiler, directory, std::move(target_arguments)).standard_output);
			if (!target.starts_with("x86_64-") || target.find("linux") == std::string::npos)
				fail("target", "Linux x86_64 required");
			auto macro_arguments = flags;
			macro_arguments.insert(macro_arguments.end(), {"-dM", "-E", "/dev/null"});
			const auto macros =
				probe(compiler, directory, std::move(macro_arguments)).standard_output;
			if (macros.find("#define __x86_64__ 1\n") == std::string::npos ||
				macros.find("#define __linux__ 1\n") == std::string::npos ||
				macros.find("#define __SIZEOF_POINTER__ 8\n") == std::string::npos)
				fail("target", "Linux x86_64 with 64-bit pointers required");
			auto search_arguments = flags;
			search_arguments.insert(search_arguments.end(), {"-E", "-v", "/dev/null"});
			auto search = probe(compiler, directory, std::move(search_arguments)).standard_error;
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
			auto source_paths = files.members(root, true);
			if (std::ranges::find(source_paths, source) == source_paths.end())
				source_paths.push_back(source);
			std::size_t source_bytes{};
			for (const auto& path : source_paths)
			{
				auto bytes = files.read(path);
				if (bytes.size() > 48U * 1024U * 1024U - source_bytes)
					fail("inputs", "source-byte-limit");
				source_bytes += bytes.size();
				inputs.push_back(
					{logical_path(path, root),
					 path == source ? source_closure_role::main : source_closure_role::header,
					 source_closure_encoding::utf8,
					 std::make_shared<const std::string>(std::move(bytes))});
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
			const auto catalog = take(sdk::project_catalog::make(
				"project://root",
				environment,
				{{"unit:" + invocation, invocation, main->content_digest, environment}}));
			std::vector<sdk::detached_row> rows;
			auto project = row(build::relations::project::descriptor(),
							   {{"catalog", id("catalog_id", catalog.catalog_id)},
								{"catalog_digest", digest_cell(catalog.catalog_digest)},
								{"logical_root", id("logical_path_id", catalog.logical_root)},
								{"environment_digest", digest_cell(environment)}});
			const auto project_id = identity(project, "project");
			rows.push_back(std::move(project));
			auto toolchain =
				row(build::relations::toolchain_context::descriptor(),
					{{"family", symbol("build.toolchain-family/1", "clang")},
					 {"exact_version", sdk::detached_cell::utf8(compiler_version)},
					 {"target_triple", sdk::detached_cell::utf8(target)},
					 {"builtin_headers_digest",
					  digest_cell(digest("cxxlens.local-clang22.builtin-headers.v1", builtins))},
					 {"abi_digest",
					  digest_cell(digest("cxxlens.local-clang22.abi-profile.v1",
										 arguments_json({toolchain_digest, target, macros})))},
					 {"plugin_spec_digest",
					  digest_cell(digest("cxxlens.local-clang22.plugins.v1", "none"))}});
			const auto toolchain_id = identity(toolchain, "toolchain");
			rows.push_back(std::move(toolchain));
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
					 {"semantic_flags_digest", digest_cell(invocation)}});
			const auto variant_id = identity(variant, "variant");
			rows.push_back(std::move(variant));
			auto source_row =
				row(source::relations::file::descriptor(),
					{{"file", id("file_id", main->file_id)},
					 {"project", id("project_id", project_id)},
					 {"logical_path", id("logical_path_id", main_path)},
					 {"content", digest_cell(main->content_digest)},
					 {"size", sdk::detached_cell::unsigned_integer(main->size_bytes)},
					 {"encoding", symbol("source.encoding/1", "utf8")},
					 {"line_index",
					  id("line_index_id", take(source_closure_main_line_index_id(closure)))},
					 {"read_only", sdk::detached_cell::boolean(true)}});
			const auto snapshot_id = identity(source_row, "snapshot");
			rows.push_back(std::move(source_row));
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
			take(with_source_closure_translation_unit(
				{closure, main_path, working_path, std::move(effective), std::move(read_roots), {}},
				[&](provider::clang22::borrowed_translation_unit& borrowed) -> sdk::result<void>
				{
					auto observed = observe_provider_worker_v4_ast(
						borrowed, metadata, unit_id, {}, snapshot_id);
					if (!observed)
						return sdk::unexpected(std::move(observed.error()));
					observations = std::move(*observed);
					return {};
				}));
			if (!observations)
				fail("AST", "observer-not-called");
			provider_worker_v4_output_normalizer_options normalization;
			normalization.toolchain_context_id = toolchain_id;
			const auto normalized =
				take(normalize_provider_worker_v4_output(*observations, normalization));
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
			for (const auto& batch : normalized.batches)
				rows.insert(rows.end(), batch.rows.begin(), batch.rows.end());
			sdk::relation_registry registry;
			const std::array<const sdk::relation_descriptor*, 12U> descriptors{
				&build::relations::project::descriptor(),
				&build::relations::toolchain_context::descriptor(),
				&build::relations::variant::descriptor(),
				&build::relations::compile_unit::descriptor(),
				&source::relations::file::descriptor(),
				&source::relations::span::descriptor(),
				&cc::relations::entity::descriptor(),
				&cc::relations::call_site::descriptor(),
				&cc::relations::call_direct_target::descriptor(),
				&materialization::entity_observation_v2_descriptor(),
				&materialization::call_observation_v2_descriptor(),
				&materialization::type_observation_v2_descriptor()};
			for (const auto* descriptor : descriptors)
				take(registry.add(*descriptor));
			auto engine = take(registry.build("cxxlens.local-clang22.v1"));
			const auto semantics =
				digest("cxxlens.local-clang22.producer.v1",
					   "Clang " + compiler_version +
						   " AST observer v4; canonical normalizer v4; single main source");
			const auto basis = content_digest(arguments_json(
				{closure.closure_digest, invocation, toolchain_digest, environment}));
			const sdk::claim_condition condition{"local-clang22-variant:" + variant_id,
												 {variant_id}};
			const sdk::claim_producer producer{"cxxlens.clang22.local", semantics};
			const sdk::claim_guarantee guarantee{
				"under_approximation",
				"selected-compile-unit",
				"single variant; main-source facts; system headers are live compiler inputs",
				{"static_analysis"}};
			sdk::claim_batch claims;
			for (auto& value : rows)
			{
				const bool native_observation = value.descriptor_id.starts_with("frontend.");
				auto asserted = take(sdk::make_assertion(engine,
														 {value,
														  condition,
														  "cc.clang22-canonical-1",
														  producer,
														  {basis},
														  closure.snapshot_id,
														  guarantee}));
				if (!native_observation)
					asserted = take(sdk::make_canonical_claim(
						engine, asserted, producer, std::move(value), semantics));
				take(claims.add(std::move(asserted)));
			}
			auto committed = take(std::move(claims).commit(engine));
			const auto claim_basis =
				take(sdk::claim_input_basis_digest(sdk::direct_claim_basis{basis}));
			auto store = take(sdk::make_in_memory_snapshot_store(engine));
			sdk::snapshot_series_selector selector{
				catalog.catalog_id,
				"application",
				std::string{engine.generation()},
				condition.universe,
				std::string{engine.registry_digest()},
				digest("cxxlens.local-clang22.interpretation.v1", "cc.clang22-canonical-1"),
				digest("cxxlens.local-clang22.execution.v1",
					   "explicit local compiler; in-process AST")};
			auto writer =
				take(store.begin({selector, {1U, 0U, 0U}, catalog.catalog_digest, std::nullopt}));
			std::vector<std::string> relation_ids;
			for (const auto* descriptor : descriptors)
			{
				std::map<std::string, std::vector<sdk::claim>, std::less<>> groups;
				for (const auto& claim : committed.claims)
					if (claim.descriptor == descriptor->id)
						groups[take(sdk::claim_input_basis_digest(claim.input_basis))].push_back(
							claim);
				if (groups.empty())
					groups.emplace(claim_basis, std::vector<sdk::claim>{});
				for (auto& [input_basis, group] : groups)
				{
					sdk::partition_draft partition{descriptor->id,
												   project_id,
												   condition,
												   "cc.clang22-canonical-1",
												   semantics,
												   input_basis,
												   "under_approximation",
												   "single-main-source",
												   std::move(group),
												   {{"compile-unit", unit_id, "covered", {}}},
												   {}};
					for (const auto& unresolved : committed.unresolved)
						if ((unresolved.source_relation == descriptor->name ||
							 unresolved.source_relation == descriptor->id) &&
							std::ranges::any_of(partition.claims,
												[&](const auto& claim)
												{
													return claim.assertion ==
														unresolved.source_assertion;
												}))
							partition.unresolved.push_back(unresolved);
					if (descriptor->id.starts_with("cc."))
					{
						for (const auto& unresolved : normalized.unresolved)
							partition.coverage.push_back(
								{"provider-diagnostic",
								 unresolved.subject,
								 "unresolved",
								 unresolved.code + ": " + unresolved.detail});
						for (const auto& limitation : normalized.limitations)
							partition.coverage.push_back(
								{"extraction-limitation", limitation, "unknown", limitation});
					}
					take(writer.stage(std::move(partition)));
				}
				relation_ids.push_back(descriptor->id);
			}
			take(writer.validate());
			auto snapshot = take(writer.publish());
			return take(sdk::detail::encode_application_queries(engine, snapshot, relation_ids));
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
