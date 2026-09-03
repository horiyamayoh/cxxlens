#include "msvc_replay_planner_internal.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <utility>

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error invalid(std::string field, std::string detail)
		{
			return {"application-analysis.request-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] error limit(std::string field, std::string detail)
		{
			return {
				"application-analysis.import-limit-exceeded", std::move(field), std::move(detail)};
		}

		[[nodiscard]] char fold(const char value) noexcept
		{
			return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
		}

		[[nodiscard]] bool starts_with_ci(const std::string_view value,
										  const std::string_view prefix) noexcept
		{
			return value.size() >= prefix.size() &&
				std::ranges::equal(value.substr(0U, prefix.size()), prefix, {}, fold, fold);
		}

		[[nodiscard]] bool equals_ci(const std::string_view left,
									 const std::string_view right) noexcept
		{
			return left.size() == right.size() && std::ranges::equal(left, right, {}, fold, fold);
		}

		[[nodiscard]] std::optional<std::string>
		logical_path_for(const decoded_capture_projection& capture, const std::string_view physical)
		{
			for (const auto& mapping : capture.path_mappings)
			{
				if (physical.size() < mapping.physical_prefix.size() ||
					!std::ranges::equal(physical.substr(0U, mapping.physical_prefix.size()),
										mapping.physical_prefix,
										{},
										fold,
										fold))
					continue;
				if (physical.size() != mapping.physical_prefix.size() &&
					mapping.physical_prefix.back() != '\\' &&
					physical[mapping.physical_prefix.size()] != '\\')
					continue;
				auto suffix = physical.substr(mapping.physical_prefix.size());
				std::string normalized;
				normalized.reserve(suffix.size());
				for (const auto byte : suffix)
					normalized.push_back(byte == '\\' ? '/' : byte);
				if (mapping.logical_prefix.ends_with('/') && normalized.starts_with('/'))
					normalized.erase(normalized.begin());
				return mapping.logical_prefix + normalized;
			}
			return std::nullopt;
		}

		[[nodiscard]] std::optional<std::string>
		logical_argument_path(const decoded_capture_projection& capture,
							  const decoded_capture_unit& unit,
							  const std::string_view input)
		{
			if (input.empty() || input.contains('\0'))
				return std::nullopt;
			std::string value{input};
			if (value.size() >= 2U && value.front() == '"' && value.back() == '"')
				value = value.substr(1U, value.size() - 2U);
			if (value.starts_with("project://"))
				return value;
			if (value.size() >= 3U && value[1U] == ':' && value[2U] == '\\')
				return logical_path_for(capture, value);

			std::string relative =
				unit.logical_working_directory.substr(std::string_view{"project://"}.size());
			if (!relative.empty())
				relative.push_back('/');
			for (const auto byte : value)
				relative.push_back(byte == '\\' ? '/' : byte);
			std::vector<std::string_view> segments;
			std::size_t offset{};
			while (offset < relative.size())
			{
				const auto next = relative.find('/', offset);
				const auto segment = std::string_view{relative}.substr(
					offset, next == std::string::npos ? relative.size() - offset : next - offset);
				if (segment.empty())
					return std::nullopt;
				if (segment == "..")
				{
					if (segments.empty())
						return std::nullopt;
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

		[[nodiscard]] result<std::vector<std::string>>
		parse_response(const std::span<const std::byte> bytes, const import_limits& limits)
		{
			std::string text;
			text.reserve(bytes.size());
			for (const auto byte : bytes)
			{
				const auto value = std::to_integer<unsigned char>(byte);
				if (value == 0U)
					return unexpected(invalid("response_files", "embedded-nul"));
				text.push_back(static_cast<char>(value));
			}
			std::vector<std::string> output;
			std::string token;
			bool quoted{};
			std::size_t slashes{};
			const auto flush = [&]() -> result<void>
			{
				if (token.empty())
					return {};
				if (output.size() >= limits.maximum_arguments_per_unit)
					return unexpected(limit("response_files", "argument-count"));
				output.push_back(std::exchange(token, {}));
				return {};
			};
			for (const char byte : text)
			{
				if (byte == '\\')
				{
					++slashes;
					continue;
				}
				if (byte == '"')
				{
					token.append(slashes / 2U, '\\');
					if (slashes % 2U != 0U)
						token.push_back('"');
					else
						quoted = !quoted;
					slashes = 0U;
					continue;
				}
				token.append(slashes, '\\');
				slashes = 0U;
				if (!quoted && std::isspace(static_cast<unsigned char>(byte)) != 0)
				{
					if (auto added = flush(); !added)
						return unexpected(std::move(added.error()));
				}
				else
					token.push_back(byte);
			}
			token.append(slashes, '\\');
			if (quoted)
				return unexpected(invalid("response_files", "unterminated-quote"));
			if (auto added = flush(); !added)
				return unexpected(std::move(added.error()));
			return output;
		}

		class response_expander
		{
		  public:
			response_expander(const decoded_capture_projection& capture,
							  const decoded_capture_unit& unit,
							  const decoded_capture_source_closure& closure,
							  const import_limits& limits)
				: capture_{capture}, unit_{unit}, closure_{closure}, limits_{limits}
			{
			}

			[[nodiscard]] result<std::vector<std::string>>
			expand(const std::span<const std::string> input, const std::size_t depth)
			{
				std::vector<std::string> output;
				for (const auto& argument : input)
				{
					if (!argument.starts_with('@') || argument.size() == 1U)
					{
						if (output.size() >= limits_.maximum_arguments_per_unit)
							return unexpected(limit("response_files", "argument-count"));
						output.push_back(argument);
						continue;
					}
					if (depth >= limits_.maximum_nesting_depth)
						return unexpected(limit("response_files", "depth"));
					auto path = logical_argument_path(
						capture_, unit_, std::string_view{argument}.substr(1U));
					if (!path)
						return unexpected(invalid("response_files", "outside-project-root"));
					const auto metadata = std::ranges::find(
						unit_.response_files, *path, &decoded_capture_auxiliary_file::logical_path);
					if (metadata == unit_.response_files.end() || !metadata->content_digest)
					{
						output.push_back(argument);
						continue;
					}
					const auto member = std::ranges::find(
						closure_.members, *path, &decoded_capture_source_member::logical_path);
					if (member == closure_.members.end() || !active_.emplace(*path).second)
						return unexpected(
							invalid("response_files", "missing-or-recursive-binding"));
					auto parsed = parse_response(member->content, limits_);
					if (!parsed)
						return unexpected(std::move(parsed.error()));
					auto nested = expand(*parsed, depth + 1U);
					active_.erase(*path);
					if (!nested)
						return unexpected(std::move(nested.error()));
					if (nested->size() > limits_.maximum_arguments_per_unit - output.size())
						return unexpected(limit("response_files", "argument-count"));
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
		};

		struct unresolved_description
		{
			std::string field;
			std::string scope;
			std::string reason;
			std::string action;
		};

		void mark_unresolved(msvc_replay_mapping_result& output,
							 replay_option_mapping& mapping,
							 unresolved_description description)
		{
			mapping.fidelity = replay_fidelity::unsupported;
			mapping.affected_scope = std::move(description.scope);
			mapping.reason = std::move(description.reason);
			mapping.completion_action = std::move(description.action);
			output.unresolved.push_back({std::move(description.field),
										 "unavailable",
										 mapping.reason,
										 mapping.completion_action});
		}
	} // namespace

	result<msvc_replay_mapping_result>
	map_msvc_19_51_replay_arguments(const decoded_capture_projection& capture,
									const decoded_capture_unit& unit,
									const std::size_t unit_index,
									const import_limits limits)
	{
		if (capture.toolchain_family != "msvc" || capture.toolchain_version != "19.51.36247")
			return unexpected(invalid("production_toolchain", "not-pinned-msvc-19.51"));
		if (!unit.original_arguments || unit.original_arguments->empty())
			return unexpected(invalid("original_argv", "recapture-with-msbuild-cltool-proxy"));
		const auto closure = std::ranges::find(
			capture.source_closures, unit.source_closure_id, &decoded_capture_source_closure::id);
		if (closure == capture.source_closures.end())
			return unexpected(invalid("source_closure", "missing-validated-binding"));
		auto expanded =
			response_expander{capture, unit, *closure, limits}.expand(*unit.original_arguments, 0U);
		if (!expanded)
			return unexpected(std::move(expanded.error()));

		msvc_replay_mapping_result output;
		output.effective_arguments.reserve(expanded->size() + 2U);
		output.option_mappings.reserve(expanded->size());
		bool source_bound{};
		for (std::size_t index{}; index < expanded->size(); ++index)
		{
			const auto& token = (*expanded)[index];
			if (token.size() > limits.maximum_string_bytes)
				return unexpected(limit("original_argv", "string-bytes"));
			const auto field = "compile_units[" + std::to_string(unit_index) + "].expanded_argv[" +
				std::to_string(index) + "]";
			replay_option_mapping mapping;
			mapping.production_token = token;
			if (index == 0U)
			{
				mapping.replay_tokens = {"clang-cl", "/Zs"};
				mapping.fidelity = replay_fidelity::approximation;
				mapping.affected_scope = unit.compile_unit_id;
				mapping.reason = "analysis-frontend-differs-from-production-compiler";
				mapping.completion_action = "use-a-qualified-msvc-native-gap-provider";
				output.unresolved.push_back(
					{field, "unavailable", mapping.reason, mapping.completion_action});
			}
			else if (equals_ci(token, "/c") || equals_ci(token, "/nologo") ||
					 equals_ci(token, "/showIncludes") || starts_with_ci(token, "/Fo") ||
					 starts_with_ci(token, "/Fd") || starts_with_ci(token, "/Fe") ||
					 starts_with_ci(token, "/sourceDependencies"))
			{
				const auto separated = equals_ci(token, "/Fo") || equals_ci(token, "/Fd") ||
					equals_ci(token, "/Fe") || equals_ci(token, "/sourceDependencies");
				if (separated && index + 1U >= expanded->size())
					mark_unresolved(output,
									mapping,
									{field,
									 unit.compile_unit_id,
									 "msvc-option-operand-missing",
									 "recapture-the-complete-production-invocation"});
				else
				{
					mapping.fidelity = replay_fidelity::nonsemantic;
					if (separated)
						++index;
				}
			}
			else if (equals_ci(token, "/D") || equals_ci(token, "/U"))
			{
				if (index + 1U >= expanded->size())
					mark_unresolved(output,
									mapping,
									{field,
									 unit.compile_unit_id,
									 "msvc-option-operand-missing",
									 "recapture-the-complete-production-invocation"});
				else
				{
					mapping.replay_tokens = {token, (*expanded)[++index]};
					mapping.fidelity = replay_fidelity::semantics_preserving;
				}
			}
			else if (starts_with_ci(token, "/D") || starts_with_ci(token, "/U") ||
					 starts_with_ci(token, "/std:") || starts_with_ci(token, "/Zc:") ||
					 starts_with_ci(token, "/permissive-") || starts_with_ci(token, "/EH") ||
					 equals_ci(token, "/GR") || equals_ci(token, "/GR-") ||
					 equals_ci(token, "/MD") || equals_ci(token, "/MDd") ||
					 equals_ci(token, "/MT") || equals_ci(token, "/MTd"))
			{
				mapping.replay_tokens = {token};
				mapping.fidelity = replay_fidelity::semantics_preserving;
			}
			else if (equals_ci(token, "/I") || equals_ci(token, "/FI") ||
					 equals_ci(token, "/external:I"))
			{
				if (index + 1U >= expanded->size())
					mark_unresolved(output,
									mapping,
									{field,
									 unit.compile_unit_id,
									 "msvc-option-operand-missing",
									 "recapture-the-complete-production-invocation"});
				else
				{
					auto logical = logical_argument_path(capture, unit, (*expanded)[++index]);
					if (!logical)
						mark_unresolved(output,
										mapping,
										{field,
										 unit.compile_unit_id,
										 "msvc-include-path-outside-captured-authority",
										 "capture-and-map-the-sdk-or-project-include-root"});
					else
					{
						mapping.replay_tokens = {token, *logical};
						mapping.fidelity = replay_fidelity::semantics_preserving;
					}
				}
			}
			else if (starts_with_ci(token, "/I") || starts_with_ci(token, "/FI") ||
					 starts_with_ci(token, "/external:I"))
			{
				const auto prefix = starts_with_ci(token, "/external:I") ? std::size_t{11U}
					: starts_with_ci(token, "/FI")						 ? std::size_t{3U}
																		 : std::size_t{2U};
				auto logical =
					logical_argument_path(capture, unit, std::string_view{token}.substr(prefix));
				if (!logical)
					mark_unresolved(output,
									mapping,
									{field,
									 unit.compile_unit_id,
									 "msvc-include-path-outside-captured-authority",
									 "capture-and-map-the-sdk-or-project-include-root"});
				else
				{
					mapping.replay_tokens = {token.substr(0U, prefix) + *logical};
					mapping.fidelity = replay_fidelity::semantics_preserving;
				}
			}
			else if (starts_with_ci(token, "/Yu") || starts_with_ci(token, "/Yc") ||
					 starts_with_ci(token, "/Fp") || starts_with_ci(token, "/ifc") ||
					 starts_with_ci(token, "/reference") || starts_with_ci(token, "/headerUnit"))
				mark_unresolved(output,
								mapping,
								{field,
								 unit.compile_unit_id,
								 "msvc-pch-or-module-input-not-replayable",
								 "capture-and-bind-the-exact-pch-or-module-input"});
			else if (!token.starts_with('/') && !token.starts_with('-'))
			{
				auto logical = logical_argument_path(capture, unit, token);
				if (logical && *logical == unit.source_logical_path)
				{
					mapping.replay_tokens = {*logical};
					mapping.fidelity = replay_fidelity::semantics_preserving;
					source_bound = true;
				}
				else
					mark_unresolved(output,
									mapping,
									{field,
									 unit.compile_unit_id,
									 "msvc-input-path-not-bound",
									 "capture-the-input-in-the-source-closure"});
			}
			else
				mark_unresolved(output,
								mapping,
								{field,
								 unit.compile_unit_id,
								 "msvc-option-not-classified",
								 "add-a-versioned-msvc19.51-option-mapping"});

			if (mapping.replay_tokens.size() >
				limits.maximum_arguments_per_unit - output.effective_arguments.size())
				return unexpected(limit("replay_plan.effective_arguments", "count"));
			output.effective_arguments.insert(output.effective_arguments.end(),
											  mapping.replay_tokens.begin(),
											  mapping.replay_tokens.end());
			output.option_mappings.push_back(std::move(mapping));
		}
		if (!source_bound)
		{
			if (output.effective_arguments.size() >= limits.maximum_arguments_per_unit)
				return unexpected(limit("replay_plan.effective_arguments", "count"));
			output.effective_arguments.push_back(unit.source_logical_path);
			output.unresolved.push_back(
				{"compile_units[" + std::to_string(unit_index) + "].original_argv",
				 "unavailable",
				 "source-token-not-bound",
				 "recapture-the-exact-production-source-token"});
		}
		return output;
	}
} // namespace cxxlens::sdk::detail
