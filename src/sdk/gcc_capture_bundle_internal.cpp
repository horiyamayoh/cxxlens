#include "gcc_capture_bundle_internal.hpp"

#include <algorithm>
#include <new>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error invalid(std::string field, std::string detail)
		{
			return {
				"application-analysis.capture-input-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] error limit(std::string field, std::string detail)
		{
			return {
				"application-analysis.import-limit-exceeded", std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::string_view state_name(const capture_observation_state state) noexcept
		{
			switch (state)
			{
				case capture_observation_state::observed:
					return "observed";
				case capture_observation_state::derived:
					return "derived";
				case capture_observation_state::redacted:
					return "redacted";
				case capture_observation_state::unavailable:
					return "unavailable";
			}
			return {};
		}

		[[nodiscard]] canonical_value captured_text(const captured_text_observation& observation,
													const std::string_view field,
													std::vector<capture_gap>& gaps)
		{
			const auto state = state_name(observation.state);
			const bool present = observation.state == capture_observation_state::observed ||
				observation.state == capture_observation_state::derived;
			if (!present)
				gaps.push_back({std::string{field},
								std::string{state},
								observation.reason,
								observation.completion_action});
			return canonical_value::from_tuple({
				canonical_value::from_string(std::string{state}),
				present && observation.value ? canonical_value::from_string(*observation.value)
											 : canonical_value::null(),
				canonical_value::from_string(observation.reason),
				canonical_value::from_string(observation.completion_action),
			});
		}

		[[nodiscard]] canonical_value present(const capture_observation_state state,
											  canonical_value value)
		{
			return canonical_value::from_tuple({
				canonical_value::from_string(std::string{state_name(state)}),
				std::move(value),
				canonical_value::from_string({}),
				canonical_value::from_string({}),
			});
		}

		struct unavailable_field
		{
			std::string_view reason;
			std::string_view action;
			std::string_view field;
		};

		[[nodiscard]] canonical_value unavailable(const unavailable_field description,
												  std::vector<capture_gap>& gaps)
		{
			captured_text_observation observation;
			observation.reason = description.reason;
			observation.completion_action = description.action;
			return captured_text(observation, description.field, gaps);
		}

		[[nodiscard]] bool at_or_below(const std::string_view path,
									   const std::string_view root) noexcept
		{
			return path == root ||
				(path.size() > root.size() && path.starts_with(root) &&
				 (root == "/" || path[root.size()] == '/'));
		}

		struct path_field
		{
			std::string_view value;
			std::string_view field;
		};

		[[nodiscard]] result<std::string> canonical_posix_path(const path_field input)
		{
			const auto value = input.value;
			if (value.empty() || !value.starts_with('/') || value.contains('\0') ||
				value.contains('\\'))
				return unexpected(
					invalid(std::string{input.field}, "canonical-absolute-path-required"));
			if (value.size() > 1U && value.ends_with('/'))
				return unexpected(
					invalid(std::string{input.field}, "canonical-absolute-path-required"));
			std::size_t offset{1U};
			while (offset < value.size())
			{
				const auto next = value.find('/', offset);
				const auto segment = value.substr(
					offset, next == std::string_view::npos ? value.size() - offset : next - offset);
				if (segment.empty() || segment == "." || segment == "..")
					return unexpected(
						invalid(std::string{input.field}, "canonical-absolute-path-required"));
				for (const char byte : segment)
					if (static_cast<unsigned char>(byte) <= 0x1fU || byte == 0x7f)
						return unexpected(
							invalid(std::string{input.field}, "canonical-absolute-path-required"));
				if (next == std::string_view::npos)
					break;
				offset = next + 1U;
			}
			return std::string{value};
		}

		struct path_resolution
		{
			std::string_view value;
			std::string_view directory;
			std::string_view field;
		};

		[[nodiscard]] result<std::string> resolve_path(const path_resolution input)
		{
			const auto value = input.value;
			if (value.empty() || value.contains('\0') || value.contains('\\'))
				return unexpected(invalid(std::string{input.field}, "posix-path-required"));
			std::vector<std::string_view> segments;
			const auto append = [&](const std::string_view path,
									const std::size_t begin) -> result<void>
			{
				std::size_t offset{begin};
				while (offset < path.size())
				{
					const auto next = path.find('/', offset);
					const auto segment = path.substr(
						offset,
						next == std::string_view::npos ? path.size() - offset : next - offset);
					if (segment.empty())
						return unexpected(invalid(std::string{input.field}, "empty-path-segment"));
					if (segment == "..")
					{
						if (segments.empty())
							return unexpected(
								invalid(std::string{input.field}, "path-escapes-root"));
						segments.pop_back();
					}
					else if (segment != ".")
						segments.push_back(segment);
					if (next == std::string_view::npos)
						break;
					offset = next + 1U;
				}
				return {};
			};
			if (!value.starts_with('/'))
				if (auto added = append(input.directory, 1U); !added)
					return unexpected(std::move(added.error()));
			if (auto added = append(value, value.starts_with('/') ? 1U : 0U); !added)
				return unexpected(std::move(added.error()));
			std::string normalized{"/"};
			for (std::size_t index{}; index < segments.size(); ++index)
			{
				if (index != 0U)
					normalized.push_back('/');
				normalized += segments[index];
			}
			return canonical_posix_path({normalized, input.field});
		}

		[[nodiscard]] std::string logical_path_for(const std::string_view physical,
												   const std::string_view root)
		{
			if (physical == root)
				return "project://";
			const auto offset = root == "/" ? 1U : root.size() + 1U;
			return "project://" + std::string{physical.substr(offset)};
		}

		struct language_projection
		{
			std::string language;
			std::optional<std::string> standard;
			std::optional<std::string> extension_mode;
		};

		[[nodiscard]] result<language_projection>
		project_language(const compile_command_entry& entry, const std::string_view field)
		{
			language_projection output;
			for (std::size_t index{1U}; index < entry.arguments.size(); ++index)
			{
				const auto& token = entry.arguments[index];
				if (token == "-x")
				{
					++index;
					if (index >= entry.arguments.size() || entry.arguments[index].empty())
						return unexpected(invalid(std::string{field}, "missing-language-after-x"));
					output.language = entry.arguments[index];
				}
				else if (token.starts_with("-x") && token.size() > 2U)
					output.language = token.substr(2U);
				else if (token == "-std")
				{
					++index;
					if (index >= entry.arguments.size() || entry.arguments[index].empty())
						return unexpected(invalid(std::string{field}, "missing-value-after-std"));
					output.standard = entry.arguments[index];
				}
				else if (token.starts_with("-std=") && token.size() > 5U)
					output.standard = token.substr(5U);
			}

			if (output.language.empty())
			{
				const auto slash = entry.file.find_last_of('/');
				const auto dot = entry.file.find_last_of('.');
				const auto extension =
					dot != std::string::npos && dot > (slash == std::string::npos ? 0U : slash)
					? std::string_view{entry.file}.substr(dot)
					: std::string_view{};
				if (extension == ".c")
					output.language = "c";
				else if (extension == ".cc" || extension == ".cp" || extension == ".cpp" ||
						 extension == ".cxx" || extension == ".c++" || extension == ".C")
					output.language = "c++";
				else
					return unexpected(invalid(std::string{field}, "language-unobserved"));
			}
			if (output.language != "c" && output.language != "c++")
				return unexpected(invalid(std::string{field}, "unsupported-language"));
			if (output.standard)
			{
				if (output.standard->starts_with("gnu"))
					output.extension_mode = "gnu";
				else if (output.standard->starts_with("c"))
					output.extension_mode = "strict";
			}
			return output;
		}

		struct logical_argument_context
		{
			std::string_view directory;
			std::string_view physical_root;
		};

		[[nodiscard]] std::optional<std::string>
		semantic_argument_path(const std::string_view value, const logical_argument_context context)
		{
			if (value.empty() || value.contains('\0') || value.contains('\\'))
				return std::nullopt;
			auto normalized = resolve_path({value, context.directory, "semantic_argument"});
			if (!normalized)
				return std::nullopt;
			if (!at_or_below(*normalized, context.physical_root))
				return "$external-path";
			return logical_path_for(*normalized, context.physical_root);
		}

		[[nodiscard]] std::vector<canonical_value>
		semantic_arguments(const compile_command_entry& entry,
						   const logical_argument_context context)
		{
			std::vector<canonical_value> output;
			output.reserve(entry.arguments.size());
			output.push_back(canonical_value::from_string("$production-compiler"));
			bool output_path{};
			bool separated_include_path{};
			for (std::size_t index{1U}; index < entry.arguments.size(); ++index)
			{
				const auto& token = entry.arguments[index];
				if (output_path)
				{
					output_path = false;
					continue;
				}
				if (token == "-o")
				{
					output_path = true;
					continue;
				}
				if (token == "-c")
					continue;
				if (separated_include_path)
				{
					separated_include_path = false;
					if (auto logical = semantic_argument_path(token, context))
						output.push_back(canonical_value::from_string(std::move(*logical)));
					else
						output.push_back(canonical_value::from_string(token));
					continue;
				}
				if (token == "-I" || token == "-iquote" || token == "-isystem" ||
					token == "-isysroot" || token == "-include" || token == "-imacros")
				{
					output.push_back(canonical_value::from_string(token));
					separated_include_path = true;
					continue;
				}
				if (token.starts_with("-I") && token.size() > 2U)
				{
					if (auto logical = semantic_argument_path(token.substr(2U), context))
						output.push_back(canonical_value::from_string("-I" + *logical));
					else
						output.push_back(canonical_value::from_string(token));
					continue;
				}
				if (token.starts_with("--sysroot=") && token.size() > 10U)
				{
					output.push_back(canonical_value::from_string("--sysroot=$captured-sysroot"));
					continue;
				}
				if (token.starts_with('@') && token.size() > 1U)
				{
					if (auto logical = semantic_argument_path(token.substr(1U), context))
						output.push_back(canonical_value::from_string("@" + *logical));
					else
						output.push_back(canonical_value::from_string(token));
					continue;
				}
				if (!token.starts_with('-'))
					if (auto logical = semantic_argument_path(token, context))
					{
						output.push_back(canonical_value::from_string(std::move(*logical)));
						continue;
					}
				output.push_back(canonical_value::from_string(token));
			}
			return output;
		}

		struct prepared_unit
		{
			std::string compile_unit_id;
			std::string source_snapshot_id;
			std::string source_file_id;
			std::string source_logical_path;
			std::string source_digest;
			std::uint64_t source_size{};
			std::string logical_working_directory;
			std::string captured_working_directory;
			language_projection language;
			std::vector<std::string> arguments;
			std::string source_closure_id;
			canonical_value closure;
		};

		[[nodiscard]] result<std::string> identity(const std::string_view kind,
												   std::vector<canonical_value> fields)
		{
			return canonical_identity_digest(kind, fields);
		}
	} // namespace

	result<std::vector<std::byte>>
	encode_gcc_compile_commands_bundle(const compile_commands_capture& capture,
									   const gcc_compile_commands_bundle_input& input,
									   const import_limits limits)
	{
		if (auto valid = limits.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (capture.entries().empty() || capture.entries().size() > limits.maximum_compile_units ||
			capture.entries().size() != input.sources.size())
			return unexpected(invalid("capture.sources", "compile-unit-count-mismatch"));
		if (input.toolchain.exact_version != "16.2.0")
			return unexpected(invalid("production_toolchain.exact_version", "not-pinned"));

		try
		{
			std::size_t metadata_bytes{};
			const auto add_metadata = [&](const path_field value) -> result<void>
			{
				if (value.value.size() > limits.maximum_string_bytes)
					return unexpected(limit(std::string{value.field}, "string-bytes"));
				if (value.value.size() > limits.maximum_total_metadata_bytes - metadata_bytes)
					return unexpected(limit("capture_bundle", "metadata-bytes"));
				metadata_bytes += value.value.size();
				return {};
			};
			for (const auto value : {
					 path_field{input.project_id, "project_id"},
					 path_field{input.physical_project_root, "physical_project_root"},
					 path_field{input.toolchain.exact_version,
								"production_toolchain.exact_version"},
					 path_field{input.toolchain.target_triple,
								"production_toolchain.target_triple"},
				 })
				if (auto bounded = add_metadata(value); !bounded)
					return unexpected(std::move(bounded.error()));
			for (const auto* observation : {
					 &input.toolchain.canonical_binary_path,
					 &input.toolchain.binary_digest,
					 &input.toolchain.sysroot,
					 &input.toolchain.abi_digest,
					 &input.toolchain.builtin_headers_digest,
					 &input.toolchain.builtin_macros_digest,
					 &input.toolchain.include_search_digest,
				 })
			{
				if (observation->value)
					if (auto bounded = add_metadata({*observation->value, "toolchain.observation"});
						!bounded)
						return unexpected(std::move(bounded.error()));
				if (auto bounded = add_metadata({observation->reason, "toolchain.reason"});
					!bounded)
					return unexpected(std::move(bounded.error()));
				if (auto bounded = add_metadata(
						{observation->completion_action, "toolchain.completion_action"});
					!bounded)
					return unexpected(std::move(bounded.error()));
			}
			auto physical_root =
				canonical_posix_path({input.physical_project_root, "physical_project_root"});
			if (!physical_root)
				return unexpected(std::move(physical_root.error()));

			std::uint64_t source_bytes{};
			std::vector<prepared_unit> prepared;
			prepared.reserve(capture.entries().size());
			for (std::size_t index{}; index < capture.entries().size(); ++index)
			{
				const auto prefix = "compile_commands[" + std::to_string(index) + "]";
				const auto& entry = capture.entries()[index];
				const auto& source = input.sources[index];
				if (auto bounded = add_metadata({entry.directory, prefix + ".directory"}); !bounded)
					return unexpected(std::move(bounded.error()));
				if (auto bounded = add_metadata({entry.file, prefix + ".file"}); !bounded)
					return unexpected(std::move(bounded.error()));
				if (auto bounded = add_metadata({source.encoding, prefix + ".source.encoding"});
					!bounded)
					return unexpected(std::move(bounded.error()));
				if (source.content.size() > limits.maximum_source_closure_bytes - source_bytes)
					return unexpected(limit("sources", "byte-count"));
				source_bytes += source.content.size();
				if (entry.arguments.size() > limits.maximum_arguments_per_unit)
					return unexpected(limit(prefix + ".arguments", "count"));
				for (const auto& argument : entry.arguments)
				{
					if (argument.size() > limits.maximum_string_bytes ||
						argument.size() > limits.maximum_total_metadata_bytes - metadata_bytes)
						return unexpected(limit(prefix + ".arguments", "metadata-bytes"));
					metadata_bytes += argument.size();
				}

				auto directory = canonical_posix_path({entry.directory, prefix + ".directory"});
				if (!directory)
					return unexpected(std::move(directory.error()));
				auto source_path = resolve_path({entry.file, *directory, prefix + ".file"});
				if (!source_path)
					return unexpected(std::move(source_path.error()));
				if (!at_or_below(*directory, *physical_root) ||
					!at_or_below(*source_path, *physical_root))
					return unexpected(invalid(prefix, "path-outside-project-root"));

				auto language = project_language(entry, prefix + ".language");
				if (!language)
					return unexpected(std::move(language.error()));
				const auto logical_source = logical_path_for(*source_path, *physical_root);
				const auto logical_working = logical_path_for(*directory, *physical_root);
				const auto source_digest = content_digest(source.content);
				auto source_file =
					identity("file",
							 {canonical_value::from_string("project"),
							  canonical_value::from_string(
								  logical_source.substr(std::string{"project://"}.size())),
							  canonical_value::from_string("cxxlens.logical-path.v1")});
				if (!source_file)
					return unexpected(std::move(source_file.error()));
				auto source_snapshot = identity("source-snapshot",
												{canonical_value::from_string(*source_file),
												 canonical_value::from_string(source_digest),
												 canonical_value::from_string(source.encoding)});
				if (!source_snapshot)
					return unexpected(std::move(source_snapshot.error()));

				auto member = canonical_value::from_tuple({
					canonical_value::from_string(*source_file),
					canonical_value::from_string(logical_source),
					present(capture_observation_state::derived,
							canonical_value::from_string(source_digest)),
					present(capture_observation_state::observed,
							canonical_value::from_bytes(source.content)),
					canonical_value::from_integer(static_cast<std::int64_t>(source.content.size())),
					present(capture_observation_state::derived,
							canonical_value::from_string("main")),
					present(capture_observation_state::observed,
							canonical_value::from_string(source.encoding)),
					canonical_value::from_boolean(true),
				});
				auto members = canonical_value::from_tuple({std::move(member)});
				auto encoded_members = canonical_binary(members);
				if (!encoded_members)
					return unexpected(std::move(encoded_members.error()));
				const auto manifest_digest = content_digest(*encoded_members);
				std::vector<capture_gap> ignored_gaps;
				auto membership =
					unavailable({"dependency-output-unobserved",
								 "recapture-with-shell-free-wrapper-or-run-dependency-probe",
								 "membership_coverage"},
								ignored_gaps);
				auto closure_digest =
					identity("application-source-closure",
							 {canonical_value::from_string(manifest_digest),
							  canonical_value::from_integer(1),
							  canonical_value::from_integer(1),
							  canonical_value::from_integer(
								  static_cast<std::int64_t>(source.content.size())),
							  membership});
				if (!closure_digest)
					return unexpected(std::move(closure_digest.error()));
				const auto closure_id = "source-closure:" + *closure_digest;
				auto closure = canonical_value::from_tuple({
					canonical_value::from_string(closure_id),
					canonical_value::from_string(*closure_digest),
					canonical_value::from_string(manifest_digest),
					canonical_value::from_integer(1),
					canonical_value::from_integer(1),
					canonical_value::from_integer(static_cast<std::int64_t>(source.content.size())),
					std::move(members),
					std::move(membership),
				});

				auto semantic_argv = canonical_value::from_tuple(
					semantic_arguments(entry, {*directory, *physical_root}));
				std::vector<capture_gap> identity_gaps;
				auto sysroot_identity_observation = input.toolchain.sysroot;
				if (sysroot_identity_observation.value)
					sysroot_identity_observation.value = "$captured-sysroot";
				std::vector<canonical_value> compile_unit_fields{
					canonical_value::from_string(input.project_id),
					canonical_value::from_string(*source_snapshot),
					canonical_value::from_string(logical_working),
					std::move(semantic_argv),
					canonical_value::from_string(language->language),
					canonical_value::from_string(input.toolchain.exact_version),
					canonical_value::from_string(input.toolchain.target_triple),
					captured_text(input.toolchain.binary_digest,
								  "production_toolchain.binary_digest",
								  identity_gaps),
					captured_text(sysroot_identity_observation,
								  "production_toolchain.sysroot",
								  identity_gaps),
					captured_text(input.toolchain.abi_digest,
								  "production_toolchain.abi_digest",
								  identity_gaps),
					captured_text(input.toolchain.builtin_headers_digest,
								  "production_toolchain.builtin_headers_digest",
								  identity_gaps),
					captured_text(input.toolchain.builtin_macros_digest,
								  "production_toolchain.builtin_macros_digest",
								  identity_gaps),
					captured_text(input.toolchain.include_search_digest,
								  "production_toolchain.include_search_digest",
								  identity_gaps),
				};
				auto compile_unit_digest =
					identity("application-compile-unit", std::move(compile_unit_fields));
				if (!compile_unit_digest)
					return unexpected(std::move(compile_unit_digest.error()));

				prepared.push_back({"compile-unit:" + *compile_unit_digest,
									*source_snapshot,
									*source_file,
									logical_source,
									source_digest,
									static_cast<std::uint64_t>(source.content.size()),
									logical_working,
									*directory,
									std::move(*language),
									entry.arguments,
									closure_id,
									std::move(closure)});
			}

			std::ranges::sort(prepared, {}, &prepared_unit::compile_unit_id);
			for (std::size_t index{1U}; index < prepared.size(); ++index)
				if (prepared[index - 1U].compile_unit_id == prepared[index].compile_unit_id)
					return unexpected(invalid("compile_units", "duplicate-semantic-variant"));

			std::vector<capture_gap> gaps;
			auto toolchain = canonical_value::from_tuple({
				canonical_value::from_string("gcc"),
				canonical_value::from_string(input.toolchain.exact_version),
				captured_text(input.toolchain.canonical_binary_path,
							  "production_toolchain.canonical_binary_path",
							  gaps),
				captured_text(
					input.toolchain.binary_digest, "production_toolchain.binary_digest", gaps),
				canonical_value::from_string(input.toolchain.target_triple),
				captured_text(input.toolchain.sysroot, "production_toolchain.sysroot", gaps),
				captured_text(input.toolchain.abi_digest, "production_toolchain.abi_digest", gaps),
				captured_text(input.toolchain.builtin_headers_digest,
							  "production_toolchain.builtin_headers_digest",
							  gaps),
				captured_text(input.toolchain.builtin_macros_digest,
							  "production_toolchain.builtin_macros_digest",
							  gaps),
				captured_text(input.toolchain.include_search_digest,
							  "production_toolchain.include_search_digest",
							  gaps),
			});

			std::vector<canonical_value> units;
			std::vector<canonical_value> closures;
			units.reserve(prepared.size());
			closures.reserve(prepared.size());
			for (std::size_t index{}; index < prepared.size(); ++index)
			{
				const auto prefix = "compile_units[" + std::to_string(index) + "]";
				auto& unit = prepared[index];
				std::vector<canonical_value> argv;
				argv.reserve(unit.arguments.size());
				for (const auto& argument : unit.arguments)
					argv.push_back(canonical_value::from_string(argument));
				auto language_standard = unit.language.standard
					? present(capture_observation_state::derived,
							  canonical_value::from_string(*unit.language.standard))
					: unavailable({"language-standard-unobserved",
								   "recapture-effective-language-standard",
								   prefix + ".language_standard"},
								  gaps);
				auto extension_mode = unit.language.extension_mode
					? present(capture_observation_state::derived,
							  canonical_value::from_string(*unit.language.extension_mode))
					: unavailable({"extension-mode-unobserved",
								   "recapture-effective-extension-mode",
								   prefix + ".extension_mode"},
								  gaps);
				units.push_back(canonical_value::from_tuple({
					canonical_value::from_string(unit.compile_unit_id),
					present(capture_observation_state::derived,
							canonical_value::from_string(unit.source_snapshot_id)),
					canonical_value::from_string(unit.source_file_id),
					canonical_value::from_string(unit.source_logical_path),
					canonical_value::from_string(unit.source_digest),
					canonical_value::from_integer(static_cast<std::int64_t>(unit.source_size)),
					canonical_value::from_string(unit.logical_working_directory),
					canonical_value::from_string(unit.language.language),
					present(capture_observation_state::observed,
							canonical_value::from_tuple(std::move(argv))),
					unavailable({"response-files-unobserved",
								 "recapture-with-shell-free-wrapper",
								 prefix + ".response_files"},
								gaps),
					unavailable({"config-files-unobserved",
								 "recapture-with-shell-free-wrapper",
								 prefix + ".config_files"},
								gaps),
					unavailable({"environment-effects-unobserved",
								 "recapture-with-shell-free-wrapper",
								 prefix + ".environment_effects"},
								gaps),
					present(capture_observation_state::observed,
							canonical_value::from_string(unit.captured_working_directory)),
					std::move(language_standard),
					std::move(extension_mode),
					canonical_value::from_string(unit.source_closure_id),
				}));
				closures.push_back(std::move(unit.closure));
			}
			std::ranges::sort(closures,
							  [](const canonical_value& left, const canonical_value& right)
							  {
								  return left.tuple.front().text < right.tuple.front().text;
							  });
			closures.erase(
				std::ranges::unique(closures,
									[](const canonical_value& left, const canonical_value& right)
									{
										return left.tuple.front().text == right.tuple.front().text;
									})
					.begin(),
				closures.end());
			for (std::size_t index{}; index < closures.size(); ++index)
				gaps.push_back(
					{"source_closures[" + std::to_string(index) + "].membership_coverage",
					 "unavailable",
					 "dependency-output-unobserved",
					 "recapture-with-shell-free-wrapper-or-run-dependency-probe"});

			std::ranges::sort(
				gaps,
				[](const capture_gap& left, const capture_gap& right)
				{
					return std::tie(left.field, left.state, left.reason, left.completion_action) <
						std::tie(right.field, right.state, right.reason, right.completion_action);
				});
			std::vector<canonical_value> gap_values;
			gap_values.reserve(gaps.size());
			for (const auto& gap : gaps)
				gap_values.push_back(canonical_value::from_tuple({
					canonical_value::from_string(gap.field),
					canonical_value::from_string(gap.state),
					canonical_value::from_string(gap.reason),
					canonical_value::from_string(gap.completion_action),
				}));

			auto root = canonical_value::from_tuple({
				canonical_value::from_string("cxxlens.build-capture-bundle.v1"),
				std::move(toolchain),
				canonical_value::from_string("compile-commands"),
				canonical_value::from_string("x86_64-linux-gnu"),
				canonical_value::from_string(input.project_id),
				canonical_value::from_tuple(std::move(units)),
				canonical_value::from_tuple(std::move(closures)),
				canonical_value::from_tuple(std::move(gap_values)),
				canonical_value::from_string("project://"),
				present(capture_observation_state::derived,
						canonical_value::from_tuple({canonical_value::from_tuple({
							canonical_value::from_string(*physical_root),
							canonical_value::from_string("project://"),
						})})),
			});
			auto encoded = canonical_binary(root);
			if (!encoded)
				return unexpected(std::move(encoded.error()));
			if (encoded->size() > limits.maximum_bundle_bytes)
				return unexpected(limit("capture_bundle", "encoded-bytes"));
			if (auto admitted = decode_capture_bundle(*encoded, limits); !admitted)
				return unexpected(std::move(admitted.error()));
			return encoded;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(limit("capture_bundle", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(limit("capture_bundle", "allocation-length"));
		}
	}
} // namespace cxxlens::sdk::detail
