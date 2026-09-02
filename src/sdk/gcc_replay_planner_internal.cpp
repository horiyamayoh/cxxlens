#include "gcc_replay_planner_internal.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <set>
#include <utility>

#include "gcc_auxiliary_capture_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error invalid(std::string field, std::string detail)
		{
			return {"application-analysis.capture-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] error request_invalid(std::string field, std::string detail)
		{
			return {"application-analysis.request-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] error limit(std::string field, std::string detail)
		{
			return {
				"application-analysis.import-limit-exceeded", std::move(field), std::move(detail)};
		}

		void append_gap(std::vector<capture_gap>& values,
						std::string field,
						const replay_option_mapping& mapping)
		{
			values.push_back(
				{std::move(field), "unavailable", mapping.reason, mapping.completion_action});
		}

		[[nodiscard]] std::optional<std::string>
		logical_path_for(const decoded_capture_projection& capture, const std::string_view physical)
		{
			for (const auto& mapping : capture.path_mappings)
			{
				if (!physical.starts_with(mapping.physical_prefix))
					continue;
				if (physical.size() != mapping.physical_prefix.size() &&
					mapping.physical_prefix.back() != '/' &&
					physical[mapping.physical_prefix.size()] != '/')
					continue;
				auto suffix = physical.substr(mapping.physical_prefix.size());
				if (mapping.logical_prefix.ends_with('/') && suffix.starts_with('/'))
					suffix.remove_prefix(1U);
				return mapping.logical_prefix + std::string{suffix};
			}
			return std::nullopt;
		}

		[[nodiscard]] result<std::string>
		logical_argument_path(const decoded_capture_projection& capture,
							  const decoded_capture_unit& unit,
							  const std::string_view value)
		{
			if (value.empty() || value.contains('\0') || value.contains('\\'))
				return unexpected(request_invalid("response_file.path", "logical-path-required"));
			if (value.starts_with("project://"))
				return std::string{value};
			if (value.starts_with('/'))
			{
				auto mapped = logical_path_for(capture, value);
				if (!mapped)
					return unexpected(
						request_invalid("response_file.path", "outside-project-root"));
				return *mapped;
			}
			if (!unit.logical_working_directory.starts_with("project://"))
				return unexpected(request_invalid("response_file.path", "working-directory"));

			std::string relative =
				unit.logical_working_directory.substr(std::string_view{"project://"}.size());
			if (!relative.empty())
				relative.push_back('/');
			relative += value;
			std::vector<std::string_view> segments;
			std::size_t offset{};
			while (offset < relative.size())
			{
				const auto next = relative.find('/', offset);
				const auto segment = std::string_view{relative}.substr(
					offset, next == std::string::npos ? relative.size() - offset : next - offset);
				if (segment.empty())
					return unexpected(request_invalid("response_file.path", "empty-segment"));
				if (segment == "..")
				{
					if (segments.empty())
						return unexpected(
							request_invalid("response_file.path", "outside-project-root"));
					segments.pop_back();
				}
				else if (segment != ".")
					segments.push_back(segment);
				if (next == std::string::npos)
					break;
				offset = next + 1U;
			}
			std::string output{"project://"};
			for (std::size_t index{}; index < segments.size(); ++index)
			{
				if (index != 0U)
					output.push_back('/');
				output += segments[index];
			}
			return output;
		}

		class gcc_response_expander
		{
		  public:
			gcc_response_expander(const decoded_capture_projection& capture,
								  const decoded_capture_unit& unit,
								  const decoded_capture_source_closure& closure,
								  const import_limits& limits)
				: capture_{capture}, unit_{unit}, closure_{closure}, limits_{limits}
			{
			}

			[[nodiscard]] result<std::vector<std::string>>
			expand(const std::span<const std::string> arguments, const std::size_t depth)
			{
				std::vector<std::string> output;
				for (const auto& argument : arguments)
				{
					if (!argument.starts_with('@') || argument.size() == 1U)
					{
						if (output.size() >= limits_.maximum_arguments_per_unit)
							return unexpected(limit("response_file.arguments", "count"));
						output.push_back(argument);
						continue;
					}
					if (++expansions_ > gcc_16_2_maximum_response_expansions)
						return unexpected(limit("response_files", "gcc-expansion-count"));
					if (depth >= limits_.maximum_nesting_depth)
						return unexpected(limit("response_files", "depth"));
					auto path = logical_argument_path(
						capture_, unit_, std::string_view{argument}.substr(1U));
					if (!path)
						return unexpected(std::move(path.error()));
					const auto metadata = std::ranges::find(
						unit_.response_files, *path, &decoded_capture_auxiliary_file::logical_path);
					if (metadata == unit_.response_files.end() || !metadata->content_digest)
					{
						if (output.size() >= limits_.maximum_arguments_per_unit)
							return unexpected(limit("response_file.arguments", "count"));
						output.push_back(argument);
						continue;
					}
					const auto member = std::ranges::find(
						closure_.members, *path, &decoded_capture_source_member::logical_path);
					if (member == closure_.members.end())
						return unexpected(
							request_invalid("response_files", "source-closure-binding-mismatch"));
					if (!active_.emplace(*path).second)
						return unexpected(request_invalid("response_files", "recursive-reference"));
					auto parsed = parse_gcc_16_2_response_arguments(member->content, limits_);
					if (!parsed)
						return unexpected(std::move(parsed.error()));
					auto nested = expand(*parsed, depth + 1U);
					active_.erase(*path);
					if (!nested)
						return unexpected(std::move(nested.error()));
					if (nested->size() > limits_.maximum_arguments_per_unit - output.size())
						return unexpected(limit("response_file.arguments", "count"));
					output.insert(output.end(), nested->begin(), nested->end());
				}
				return output;
			}

		  private:
			const decoded_capture_projection& capture_;
			const decoded_capture_unit& unit_;
			const decoded_capture_source_closure& closure_;
			const import_limits& limits_;
			std::set<std::string, std::less<>> active_;
			std::size_t expansions_{};
		};

		[[nodiscard]] bool one_of(const std::string_view value,
								  const std::initializer_list<std::string_view> candidates)
		{
			return std::ranges::find(candidates, value) != candidates.end();
		}
	} // namespace

	result<gcc_replay_mapping_result>
	map_gcc_16_2_replay_arguments(const decoded_capture_projection& capture,
								  const decoded_capture_unit& unit,
								  const std::size_t unit_index,
								  const import_limits limits)
	{
		if (!unit.original_arguments || unit.original_arguments->empty())
			return unexpected(error{"application-analysis.target-unavailable",
									"original_argv",
									"recapture the GCC compile unit with its exact argv"});
		if (unit.original_arguments->size() > limits.maximum_arguments_per_unit)
			return unexpected(limit("original_argv", "count"));
		const auto closure = std::ranges::find(
			capture.source_closures, unit.source_closure_id, &decoded_capture_source_closure::id);
		if (closure == capture.source_closures.end())
			return unexpected(request_invalid("source_closure", "missing-validated-binding"));
		auto expanded = gcc_response_expander{capture, unit, *closure, limits}.expand(
			*unit.original_arguments, 0U);
		if (!expanded)
			return unexpected(std::move(expanded.error()));

		gcc_replay_mapping_result output;
		output.effective_arguments.reserve(expanded->size() + 1U);
		output.option_mappings.reserve(expanded->size());
		const auto push_effective = [&](const std::span<const std::string> tokens) -> result<void>
		{
			if (tokens.size() >
				limits.maximum_arguments_per_unit - output.effective_arguments.size())
				return unexpected(limit("replay_plan.effective_arguments", "count"));
			output.effective_arguments.insert(
				output.effective_arguments.end(), tokens.begin(), tokens.end());
			return {};
		};
		const auto field_for = [&](const std::size_t index)
		{
			return "compile_units[" + std::to_string(unit_index) + "].expanded_argv[" +
				std::to_string(index) + "]";
		};
		const auto unresolved = [&](const std::size_t index, replay_option_mapping& mapping)
		{
			mapping.affected_scope = unit.compile_unit_id;
			append_gap(output.unresolved, field_for(index), mapping);
		};
		const auto append_mapping = [&](replay_option_mapping mapping) -> result<void>
		{
			auto added = push_effective(mapping.replay_tokens);
			if (!added)
				return added;
			output.option_mappings.push_back(std::move(mapping));
			return {};
		};

		bool source_bound{};
		for (std::size_t index{}; index < expanded->size(); ++index)
		{
			const auto& token = (*expanded)[index];
			if (token.size() > limits.maximum_string_bytes)
				return unexpected(limit("original_argv", "string-bytes"));
			replay_option_mapping mapping;
			mapping.production_token = token;
			if (index == 0U)
			{
				mapping.replay_tokens = {"clang++", "-fsyntax-only"};
				mapping.fidelity = replay_fidelity::approximation;
				mapping.reason = "analysis-frontend-differs-from-production-compiler";
				mapping.completion_action = "use-a-qualified-gcc-native-gap-provider";
				unresolved(index, mapping);
			}
			else if (token == "-c" || one_of(token, {"-M", "-MM", "-MD", "-MMD", "-MP"}) ||
					 token == "-o" || (token.starts_with("-o") && token.size() > 2U))
			{
				mapping.fidelity = replay_fidelity::nonsemantic;
				if (token == "-o")
				{
					if (index + 1U >= expanded->size())
						return unexpected(invalid("original_argv", "missing-output-path"));
					replay_option_mapping value;
					value.production_token = (*expanded)[++index];
					value.fidelity = replay_fidelity::nonsemantic;
					output.option_mappings.push_back(std::move(mapping));
					output.option_mappings.push_back(std::move(value));
					continue;
				}
			}
			else if (one_of(token, {"-MF", "-MT", "-MQ"}))
			{
				mapping.fidelity = replay_fidelity::nonsemantic;
				if (index + 1U >= expanded->size())
					return unexpected(
						invalid("original_argv", "missing-dependency-option-argument"));
				replay_option_mapping value;
				value.production_token = (*expanded)[++index];
				value.fidelity = replay_fidelity::nonsemantic;
				output.option_mappings.push_back(std::move(mapping));
				output.option_mappings.push_back(std::move(value));
				continue;
			}
			else if ((token.starts_with("-MF") || token.starts_with("-MT") ||
					  token.starts_with("-MQ")) &&
					 token.size() > 3U)
				mapping.fidelity = replay_fidelity::nonsemantic;
			else if (((token.starts_with("-D") || token.starts_with("-U")) && token.size() > 2U) ||
					 one_of(token, {"-nostdinc", "-nostdinc++", "-undef"}))
			{
				mapping.replay_tokens = {token};
				mapping.fidelity = replay_fidelity::exact;
			}
			else if (token == "-D" || token == "-U")
			{
				if (index + 1U >= expanded->size())
					return unexpected(
						invalid("original_argv", "missing-preprocessor-option-argument"));
				mapping.replay_tokens = {token};
				mapping.fidelity = replay_fidelity::exact;
				auto added = append_mapping(std::move(mapping));
				if (!added)
					return unexpected(std::move(added.error()));
				replay_option_mapping value;
				value.production_token = (*expanded)[++index];
				value.replay_tokens = {(*expanded)[index]};
				value.fidelity = replay_fidelity::exact;
				mapping = std::move(value);
			}
			else if (token.starts_with("-std="))
			{
				const auto standard = token.substr(5U);
				const bool matches = unit.language_standard && *unit.language_standard == standard;
				const bool strict = unit.extension_mode && *unit.extension_mode == "strict";
				if (matches && strict && standard == "c++23")
				{
					mapping.replay_tokens = {token};
					mapping.fidelity = replay_fidelity::exact;
				}
				else if (matches && unit.extension_mode && *unit.extension_mode == "gnu" &&
						 standard == "gnu++23")
				{
					mapping.replay_tokens = {token};
					mapping.fidelity = replay_fidelity::approximation;
					mapping.reason = "gcc-extension-fidelity-not-proved-for-clang-replay";
					mapping.completion_action =
						"compare-the-required-extension-or-use-native-gap-provider";
					unresolved(index, mapping);
				}
				else
				{
					mapping.fidelity = replay_fidelity::unsupported;
					mapping.reason = "language-mode-capture-mismatch-or-unsupported";
					mapping.completion_action = "recapture-a-pinned-cxx23-language-mode";
					unresolved(index, mapping);
				}
			}
			else if (token == "-pthread")
			{
				mapping.replay_tokens = {token};
				mapping.fidelity = replay_fidelity::approximation;
				mapping.reason = "gcc-pthread-driver-effects-not-proved-for-clang-replay";
				mapping.completion_action = "compare-pthread-macro-and-target-runtime-effects";
				unresolved(index, mapping);
			}
			else
			{
				struct path_option
				{
					std::string_view production;
					std::string_view replay;
					bool allow_separated;
					bool allow_joined;
				};
				constexpr std::array path_options{
					path_option{"--include-directory-after=", "-idirafter", false, true},
					path_option{"--include-directory=", "-I", false, true},
					path_option{"-idirafter", "-idirafter", true, true},
					path_option{"-isystem", "-isystem", true, true},
					path_option{"-iquote", "-iquote", true, true},
					path_option{"-include", "-include", true, false},
					path_option{"-imacros", "-imacros", true, false},
					path_option{"-I", "-I", true, true},
				};
				const auto option = std::ranges::find_if(
					path_options,
					[&](const path_option& candidate)
					{
						return (candidate.allow_separated && token == candidate.production) ||
							(candidate.allow_joined && token.starts_with(candidate.production) &&
							 token.size() > candidate.production.size());
					});
				if (option != path_options.end())
				{
					const bool separated = token == option->production;
					if (separated && index + 1U >= expanded->size())
						return unexpected(invalid("original_argv", "missing-path-option-argument"));
					const auto raw_path = separated
						? std::string_view{(*expanded)[index + 1U]}
						: std::string_view{token}.substr(option->production.size());
					if (raw_path.empty())
						return unexpected(invalid("original_argv", "empty-path-option-argument"));
					auto logical = logical_argument_path(capture, unit, raw_path);
					if (logical)
					{
						mapping.fidelity = replay_fidelity::semantics_preserving;
						mapping.replay_tokens = {std::string{option->replay}};
						if (!separated)
							mapping.replay_tokens.front() += *logical;
						if (separated)
						{
							auto added = append_mapping(std::move(mapping));
							if (!added)
								return unexpected(std::move(added.error()));
							replay_option_mapping value;
							value.production_token = (*expanded)[++index];
							value.replay_tokens = {*logical};
							value.fidelity = replay_fidelity::semantics_preserving;
							mapping = std::move(value);
						}
					}
					else
					{
						mapping.fidelity = replay_fidelity::unsupported;
						mapping.reason = "gcc-path-option-outside-logical-project";
						mapping.completion_action =
							"capture-and-map-the-external-toolchain-or-source-input";
						unresolved(index, mapping);
						if (separated)
						{
							output.option_mappings.push_back(std::move(mapping));
							replay_option_mapping value;
							value.production_token = (*expanded)[++index];
							value.fidelity = replay_fidelity::unsupported;
							value.affected_scope = unit.compile_unit_id;
							value.reason = "gcc-path-option-outside-logical-project";
							value.completion_action =
								"capture-and-map-the-external-toolchain-or-source-input";
							append_gap(output.unresolved, field_for(index), value);
							output.option_mappings.push_back(std::move(value));
							continue;
						}
					}
				}
				else
				{
					std::optional<std::string> logical = logical_path_for(capture, token);
					if (!logical && !token.starts_with('-') && !token.starts_with('@'))
					{
						auto relative = logical_argument_path(capture, unit, token);
						if (relative)
							logical = std::move(*relative);
					}
					if ((logical && *logical == unit.source_logical_path) ||
						token == unit.source_logical_path)
					{
						const auto replay = logical ? *logical : token;
						mapping.replay_tokens = {replay};
						mapping.fidelity = logical ? replay_fidelity::semantics_preserving
												   : replay_fidelity::exact;
						source_bound = true;
					}
					else
					{
						const bool specification = token == "--specs" ||
							token.starts_with("--specs=") || token.starts_with("-specs=");
						const bool external_root = token == "--sysroot" || token == "-isysroot" ||
							token == "-B" || token.starts_with("--sysroot=") ||
							(token.starts_with("-isysroot") && token.size() > 9U) ||
							(token.starts_with("-B") && token.size() > 2U);
						const bool paired_external = token == "--specs" || token == "--sysroot" ||
							token == "-isysroot" || token == "-B";
						if (paired_external && index + 1U >= expanded->size())
							return unexpected(
								invalid("original_argv", "missing-external-option-argument"));
						if (token == "--specs=" || token == "-specs=" || token == "--sysroot=" ||
							token == "-isysroot" || token == "-B")
						{
							if (!paired_external)
								return unexpected(
									invalid("original_argv", "empty-external-option-argument"));
						}
						mapping.fidelity = replay_fidelity::unsupported;
						mapping.reason = specification ? "gcc-specification-file-not-replayable"
							: external_root			 ? "gcc-external-toolchain-root-not-replayable"
							: token.starts_with('@') ? "response-file-expansion-unavailable"
													 : "gcc-option-not-classified";
						mapping.completion_action = specification
							? "model-the-specification-effects-with-a-versioned-replay-contract"
							: external_root ? "capture-and-bind-the-external-toolchain-root"
							: token.starts_with('@') ? "capture-and-expand-the-response-file"
													 : "add-a-versioned-gcc16-option-mapping";
						unresolved(index, mapping);
						if (paired_external)
						{
							output.option_mappings.push_back(std::move(mapping));
							replay_option_mapping value;
							value.production_token = (*expanded)[++index];
							value.fidelity = replay_fidelity::unsupported;
							value.affected_scope = unit.compile_unit_id;
							value.reason = output.unresolved.back().reason;
							value.completion_action = output.unresolved.back().completion_action;
							append_gap(output.unresolved, field_for(index), value);
							output.option_mappings.push_back(std::move(value));
							continue;
						}
					}
				}
			}
			auto added = append_mapping(std::move(mapping));
			if (!added)
				return unexpected(std::move(added.error()));
		}
		if (!source_bound)
		{
			const std::array source{unit.source_logical_path};
			auto added = push_effective(source);
			if (!added)
				return unexpected(std::move(added.error()));
			output.unresolved.push_back(
				{"compile_units[" + std::to_string(unit_index) + "].original_argv",
				 "unavailable",
				 "source-token-not-bound",
				 "recapture-the-exact-production-source-token"});
		}
		return output;
	}
} // namespace cxxlens::sdk::detail
