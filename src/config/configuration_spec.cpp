#include "configuration_spec.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace cxxlens::detail::config
{
	namespace
	{
		[[nodiscard]] configuration_spec text(std::string value,
											  std::vector<std::string> allowed = {},
											  const bool path = false,
											  const bool secret = false)
		{
			return {configuration_value_kind::string,
					std::move(value),
					path,
					secret,
					0,
					0,
					std::move(allowed)};
		}

		[[nodiscard]] configuration_spec
		integer(const std::int64_t value, const std::int64_t minimum, const std::int64_t maximum)
		{
			return {configuration_value_kind::integer, value, false, false, minimum, maximum, {}};
		}

		[[nodiscard]] error invalid(std::string key, std::string reason)
		{
			error failure;
			failure.code.value = "config.invalid-value";
			failure.message = "configuration value is invalid";
			failure.attributes.emplace("key", std::move(key));
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}
	} // namespace

	const std::map<std::string, configuration_spec, std::less<>>& configuration_specs()
	{
		static const std::map<std::string, configuration_spec, std::less<>> values{
			{"analysis.default_precision",
			 text("workspace_semantic", {"lexical", "tu_semantic", "workspace_semantic"})},
			{"analysis.macro_policy",
			 text("include_with_origin", {"exclude", "include_with_origin"})},
			{"analysis.template_policy",
			 text("patterns_and_observed_instantiations",
				  {"patterns", "patterns_and_observed_instantiations"})},
			{"cache.backend", text("sqlite", {"memory", "sqlite"})},
			{"cache.directory", text(".cxxlens/cache", {}, true)},
			{"cache.mode", text("read_write", {"off", "read_only", "read_write"})},
			{"execution.memory_budget_mb", integer(4096, 64, 1048576)},
			{"execution.parallelism", text("auto", {"auto"})},
			{"execution.per_tu_timeout_seconds", integer(120, 1, 86400)},
			{"models.files",
			 {configuration_value_kind::string_list,
			  std::vector<std::string>{},
			  true,
			  false,
			  0,
			  0,
			  {}}},
			{"output.deterministic",
			 {configuration_value_kind::boolean, true, false, false, 0, 0, {}}},
			{"output.path_style", text("project_relative", {"project_relative", "absolute"})},
			{"review.baseline", text(".cxxlens/baseline.json", {}, true)},
			{"review.changed_lines_only",
			 {configuration_value_kind::boolean, true, false, false, 0, 0, {}}},
			{"review.gate.minimum_confidence",
			 text("probable", {"speculative", "possible", "probable", "certain"})},
			{"review.gate.minimum_severity",
			 text("warning", {"note", "warning", "error", "critical"})},
			{"review.gate.no_new_only",
			 {configuration_value_kind::boolean, true, false, false, 0, 0, {}}},
			{"secrets.token", text("", {}, false, true)},
			{"transform.apply", text("dry_run", {"dry_run", "apply"})},
			{"transform.formatting", text("changed_ranges", {"none", "changed_ranges"})},
			{"transform.macro_policy", text("reject", {"reject"})},
			{"transform.reparse", text("affected_variants", {"affected_variants", "all_variants"})},
			{"workspace.compilation_database", text("build", {}, true)},
			{"workspace.compile_command_policy", text("require_exact", {"require_exact"})},
			{"workspace.generated_code.default", text("exclude", {"exclude", "include"})},
			{"workspace.generated_code.patterns",
			 {configuration_value_kind::string_list,
			  std::vector<std::string>{},
			  true,
			  false,
			  0,
			  0,
			  {}}},
			{"workspace.root", text(".", {}, true)},
			{"workspace.system_headers", text("exclude", {"exclude", "include"})},
			{"workspace.variants", text("all", {"all"})},
		};
		return values;
	}

	result<void> validate_configuration_value(const std::string_view key,
											  const configuration_value& value)
	{
		const auto found = configuration_specs().find(key);
		if (found == configuration_specs().end())
			return invalid(std::string{key}, "unknown-key");
		const auto& spec = found->second;
		const bool type_matches = (spec.kind == configuration_value_kind::boolean &&
								   std::holds_alternative<bool>(value)) ||
			(spec.kind == configuration_value_kind::integer &&
			 std::holds_alternative<std::int64_t>(value)) ||
			(spec.kind == configuration_value_kind::string &&
			 std::holds_alternative<std::string>(value)) ||
			(spec.kind == configuration_value_kind::string_list &&
			 std::holds_alternative<std::vector<std::string>>(value));
		if (!type_matches)
			return invalid(std::string{key}, "wrong-type");
		if (const auto* number = std::get_if<std::int64_t>(&value);
			number != nullptr && (*number < spec.minimum || *number > spec.maximum))
			return invalid(std::string{key}, "out-of-range");
		if (const auto* string = std::get_if<std::string>(&value); string != nullptr &&
			!spec.allowed_values.empty() && !std::ranges::contains(spec.allowed_values, *string))
			return invalid(std::string{key}, "unsupported-enum-value");
		return {};
	}

	bool is_secret_key(const std::string_view key)
	{
		const auto found = configuration_specs().find(key);
		return found != configuration_specs().end() && found->second.secret;
	}

	std::string_view configuration_layer_name(const configuration_layer layer) noexcept
	{
		switch (layer)
		{
			case configuration_layer::built_in_default:
				return "built_in_default";
			case configuration_layer::config_default:
				return "config_default";
			case configuration_layer::named_profile:
				return "named_profile";
			case configuration_layer::cli:
				return "cli";
			case configuration_layer::api_option:
				return "api_option";
		}
		return "unknown";
	}
} // namespace cxxlens::detail::config
