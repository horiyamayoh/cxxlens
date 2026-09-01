#include "build_capture_internal.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error capture_error(std::string field,
										  std::string detail,
										  std::string code = "sdk.build-capture-invalid")
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::string_view state_name(const capture_field_state state) noexcept
		{
			switch (state)
			{
				case capture_field_state::observed:
					return "observed";
				case capture_field_state::derived:
					return "derived";
				case capture_field_state::redacted:
					return "redacted";
				case capture_field_state::unavailable:
					return "unavailable";
			}
			return "invalid";
		}

		[[nodiscard]] bool digest_like(const std::string_view value)
		{
			std::size_t offset{};
			if (value.starts_with("sha256:"))
				offset = 7U;
			else
			{
				const auto marker = value.rfind(":sha256:");
				if (marker == std::string_view::npos || marker == 0U)
					return false;
				const auto domain = value.substr(0U, marker);
				if (!std::ranges::all_of(domain,
										 [](const char byte)
										 {
											 return (byte >= 'a' && byte <= 'z') ||
												 (byte >= '0' && byte <= '9') || byte == '.' ||
												 byte == '-' || byte == '_';
										 }))
					return false;
				offset = marker + 8U;
			}
			const auto hex = value.substr(offset);
			return hex.size() == 64U &&
				std::ranges::all_of(hex,
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		class metadata_budget
		{
		  public:
			explicit metadata_budget(const build_capture_limits& limits) : limits_{limits} {}

			[[nodiscard]] result<void> add(const std::string_view value, std::string field)
			{
				if (value.empty() || value.size() > limits_.maximum_string_bytes)
					return unexpected(capture_error(std::move(field), "empty-or-oversized"));
				if (auto valid = validate_utf8_text(value); !valid)
					return unexpected(capture_error(std::move(field), "invalid-utf8"));
				if (value.size() > limits_.maximum_total_metadata_bytes - total_)
					return unexpected(capture_error(
						"metadata", "byte-limit", "sdk.build-capture-limit-exceeded"));
				total_ += value.size();
				return {};
			}

			[[nodiscard]] result<void> add_optional(const std::optional<std::string>& value,
													std::string field)
			{
				return value ? add(*value, std::move(field)) : result<void>{};
			}

		  private:
			const build_capture_limits& limits_;
			std::size_t total_{};
		};

		template <class T>
		[[nodiscard]] result<void> validate_capture_shape(const captured_value<T>& value,
														  const std::string_view field,
														  metadata_budget& budget)
		{
			const bool present = value.value.has_value();
			const bool value_state = value.state == capture_field_state::observed ||
				value.state == capture_field_state::derived;
			if (present != value_state)
				return unexpected(capture_error(std::string{field}, "state-value-mismatch"));
			if (value_state && (!value.reason.empty() || !value.completion_action.empty()))
				return unexpected(capture_error(std::string{field}, "unexpected-gap-metadata"));
			if (!value_state && (value.reason.empty() || value.completion_action.empty()))
				return unexpected(capture_error(std::string{field}, "missing-gap-metadata"));
			if (!value.reason.empty())
				if (auto valid = budget.add(value.reason, std::string{field} + ".reason"); !valid)
					return valid;
			if (!value.completion_action.empty())
				if (auto valid = budget.add(value.completion_action,
											std::string{field} + ".completion_action");
					!valid)
					return valid;
			return {};
		}

		[[nodiscard]] result<void>
		validate_id(const std::string_view value, std::string field, metadata_budget& budget)
		{
			if (auto bounded = budget.add(value, field); !bounded)
				return bounded;
			if (auto valid = validate_strong_id(value); !valid)
				return unexpected(capture_error(std::move(field), "strong-id"));
			return {};
		}

		[[nodiscard]] result<void>
		validate_digest(const std::string_view value, std::string field, metadata_budget& budget)
		{
			if (auto bounded = budget.add(value, field); !bounded)
				return bounded;
			if (!digest_like(value))
				return unexpected(capture_error(std::move(field), "digest"));
			return {};
		}

		[[nodiscard]] result<void>
		validate_string_list(const captured_value<std::vector<std::string>>& value,
							 const std::string_view field,
							 const std::size_t maximum,
							 metadata_budget& budget,
							 const bool require_nonempty)
		{
			if (auto shape = validate_capture_shape(value, field, budget); !shape)
				return shape;
			if (!value.value)
				return {};
			if (value.value->size() > maximum || (require_nonempty && value.value->empty()))
				return unexpected(
					capture_error(std::string{field}, "count", "sdk.build-capture-limit-exceeded"));
			for (std::size_t index{}; index < value.value->size(); ++index)
				if (auto valid = budget.add((*value.value)[index],
											std::string{field} + "[" + std::to_string(index) + "]");
					!valid)
					return valid;
			return {};
		}

		[[nodiscard]] bool semantic_option_requires_gap(const replay_option_class value) noexcept
		{
			return value == replay_option_class::approximation ||
				value == replay_option_class::unsupported;
		}

		[[nodiscard]] result<void>
		validate_options(const captured_value<std::vector<normalized_build_option>>& value,
						 const build_capture_limits& limits,
						 metadata_budget& budget)
		{
			constexpr std::string_view field{"invocation.normalized_semantic_options"};
			if (auto shape = validate_capture_shape(value, field, budget); !shape)
				return shape;
			if (!value.value)
				return {};
			if (value.value->size() > limits.maximum_options)
				return unexpected(
					capture_error(std::string{field}, "count", "sdk.build-capture-limit-exceeded"));
			for (std::size_t index{}; index < value.value->size(); ++index)
			{
				const auto& option = (*value.value)[index];
				const auto prefix = std::string{field} + "[" + std::to_string(index) + "]";
				if (auto valid = budget.add(option.token, prefix + ".token"); !valid)
					return valid;
				const bool gap = semantic_option_requires_gap(option.classification);
				if (gap != (!option.reason.empty() && !option.completion_action.empty()) ||
					(!gap && (!option.reason.empty() || !option.completion_action.empty())))
					return unexpected(capture_error(prefix, "classification-gap-mismatch"));
				if (gap)
				{
					if (auto valid = budget.add(option.reason, prefix + ".reason"); !valid)
						return valid;
					if (auto valid =
							budget.add(option.completion_action, prefix + ".completion_action");
						!valid)
						return valid;
				}
			}
			return {};
		}

		[[nodiscard]] bool logical_path(const std::string_view value) noexcept
		{
			return !value.empty() && value.front() != '/' && !value.contains('\\') &&
				!(value.size() > 1U && value[1U] == ':');
		}

		[[nodiscard]] result<void> validate_auxiliary_files(
			const captured_value<std::vector<build_capture_auxiliary_file>>& value,
			const std::string_view field,
			const build_capture_limits& limits,
			metadata_budget& budget)
		{
			if (auto shape = validate_capture_shape(value, field, budget); !shape)
				return shape;
			if (!value.value)
				return {};
			if (value.value->size() > limits.maximum_auxiliary_files)
				return unexpected(
					capture_error(std::string{field}, "count", "sdk.build-capture-limit-exceeded"));
			std::set<std::string, std::less<>> paths;
			for (std::size_t index{}; index < value.value->size(); ++index)
			{
				const auto& item = (*value.value)[index];
				const auto prefix = std::string{field} + "[" + std::to_string(index) + "]";
				if (!logical_path(item.logical_path))
					return unexpected(capture_error(prefix + ".logical_path", "machine-path"));
				if (auto valid = validate_id(item.logical_path, prefix + ".logical_path", budget);
					!valid)
					return valid;
				if (!paths.insert(item.logical_path).second)
					return unexpected(capture_error(std::string{field}, "duplicate-path"));
				if (auto shape = validate_capture_shape(
						item.content_digest, prefix + ".content_digest", budget);
					!shape)
					return shape;
				if (item.content_digest.value)
					if (auto digest = validate_digest(
							*item.content_digest.value, prefix + ".content_digest", budget);
						!digest)
						return digest;
				if (item.parent_index && *item.parent_index >= value.value->size())
					return unexpected(capture_error(prefix + ".parent_index", "out-of-range"));
				if (item.size_bytes >
					static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
					return unexpected(capture_error(prefix + ".size_bytes", "overflow"));
			}

			for (std::size_t index{}; index < value.value->size(); ++index)
			{
				std::set<std::size_t> visited;
				auto cursor = std::optional<std::size_t>{index};
				std::size_t depth{};
				while (cursor)
				{
					if (!visited.insert(*cursor).second)
						return unexpected(capture_error(std::string{field}, "recursive-reference"));
					if (++depth > limits.maximum_auxiliary_depth)
						return unexpected(capture_error(
							std::string{field}, "depth", "sdk.build-capture-limit-exceeded"));
					cursor = (*value.value)[*cursor].parent_index;
				}
			}
			return {};
		}

		[[nodiscard]] result<void> validate_environment(
			const captured_value<std::vector<build_capture_environment_effect>>& value,
			const build_capture_limits& limits,
			metadata_budget& budget)
		{
			constexpr std::string_view field{"invocation.environment_effects"};
			if (auto shape = validate_capture_shape(value, field, budget); !shape)
				return shape;
			if (!value.value)
				return {};
			if (value.value->size() > limits.maximum_environment_effects)
				return unexpected(
					capture_error(std::string{field}, "count", "sdk.build-capture-limit-exceeded"));
			std::string previous;
			for (std::size_t index{}; index < value.value->size(); ++index)
			{
				const auto& effect = (*value.value)[index];
				const auto prefix = std::string{field} + "[" + std::to_string(index) + "]";
				if (auto valid = validate_id(effect.name, prefix + ".name", budget); !valid)
					return valid;
				if (!previous.empty() && effect.name <= previous)
					return unexpected(capture_error(std::string{field}, "noncanonical-order"));
				previous = effect.name;
				if (auto shape = validate_capture_shape(
						effect.semantic_value, prefix + ".semantic_value", budget);
					!shape)
					return shape;
				if (effect.semantic_value.value)
					if (auto valid =
							budget.add(*effect.semantic_value.value, prefix + ".semantic_value");
						!valid)
						return valid;
			}
			return {};
		}

		[[nodiscard]] canonical_value text(std::string_view value)
		{
			return canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] canonical_value captured_state(const capture_field_state state,
													 std::string_view reason)
		{
			return canonical_value::from_tuple({text(state_name(state)), text(reason)});
		}

		[[nodiscard]] canonical_value option_projection(const normalized_build_option& option)
		{
			return canonical_value::from_tuple({
				text(option.token),
				canonical_value::from_integer(static_cast<std::int64_t>(option.classification)),
				text(option.reason),
				text(option.completion_action),
			});
		}

		[[nodiscard]] canonical_value
		auxiliary_projection(const std::vector<build_capture_auxiliary_file>& values)
		{
			std::vector<canonical_value> output;
			output.reserve(values.size());
			for (std::size_t index{}; index < values.size(); ++index)
			{
				const auto& item = values[index];
				const auto parent =
					item.parent_index ? values[*item.parent_index].logical_path : std::string{};
				output.push_back(canonical_value::from_tuple({
					text(item.logical_path),
					captured_state(item.content_digest.state, item.content_digest.reason),
					text(item.content_digest.value.value_or("")),
					canonical_value::from_integer(static_cast<std::int64_t>(item.size_bytes)),
					text(parent),
				}));
			}
			std::ranges::sort(output,
							  [](const auto& left, const auto& right)
							  {
								  return canonical_binary(left).value() <
									  canonical_binary(right).value();
							  });
			return canonical_value::from_tuple(std::move(output));
		}

		[[nodiscard]] canonical_value
		environment_projection(const std::vector<build_capture_environment_effect>& values)
		{
			std::vector<canonical_value> output;
			output.reserve(values.size());
			for (const auto& effect : values)
				output.push_back(canonical_value::from_tuple({
					text(effect.name),
					captured_state(effect.semantic_value.state, effect.semantic_value.reason),
					text(effect.semantic_value.value.value_or("")),
				}));
			return canonical_value::from_tuple(std::move(output));
		}

		[[nodiscard]] result<std::string> capture_identity(const build_capture_draft& value)
		{
			std::vector<canonical_value> options;
			if (value.invocation.normalized_semantic_options.value)
				for (const auto& option : *value.invocation.normalized_semantic_options.value)
					if (option.classification != replay_option_class::nonsemantic)
						options.push_back(option_projection(option));

			const auto response = value.invocation.response_files.value
				? auxiliary_projection(*value.invocation.response_files.value)
				: canonical_value::from_tuple({});
			const auto config = value.invocation.config_files.value
				? auxiliary_projection(*value.invocation.config_files.value)
				: canonical_value::from_tuple({});
			const auto environment = value.invocation.environment_effects.value
				? environment_projection(*value.invocation.environment_effects.value)
				: canonical_value::from_tuple({});

			std::vector<canonical_value> fields{
				text("cxxlens.build-capture.v1"),
				text(value.project_id),
				text(value.catalog.logical_root),
				text(value.selected_catalog_compile_unit_id),
				text(value.compile_unit_id),
				text(value.build_variant_id),
				text(value.toolchain_context_id),
				canonical_value::from_tuple(
					{text(value.toolchain.family),
					 text(value.toolchain.exact_version),
					 text(value.toolchain.target_triple),
					 text(value.toolchain.builtin_headers_digest),
					 text(value.toolchain.abi_digest),
					 text(value.toolchain.plugin_spec_digest),
					 captured_state(value.toolchain.production_compiler_binary_digest.state,
									value.toolchain.production_compiler_binary_digest.reason),
					 text(value.toolchain.production_compiler_binary_digest.value.value_or(""))}),
				canonical_value::from_tuple({text(value.variant.language),
											 text(value.variant.language_standard),
											 text(value.variant.target_triple),
											 text(value.variant.predefined_macros_digest),
											 text(value.variant.include_search_digest),
											 text(value.variant.semantic_flags_digest)}),
				canonical_value::from_tuple({
					captured_state(value.invocation.normalized_semantic_options.state,
								   value.invocation.normalized_semantic_options.reason),
					canonical_value::from_tuple(std::move(options)),
					captured_state(value.invocation.response_files.state,
								   value.invocation.response_files.reason),
					response,
					captured_state(value.invocation.config_files.state,
								   value.invocation.config_files.reason),
					config,
					captured_state(value.invocation.environment_effects.state,
								   value.invocation.environment_effects.reason),
					environment,
					text(value.invocation.environment_digest),
					text(value.invocation.language),
					text(value.invocation.logical_working_directory),
				}),
				canonical_value::from_tuple(
					{text(value.source.source_snapshot_id),
					 text(value.source.file_id),
					 text(value.source.logical_path),
					 text(value.source.content_digest),
					 canonical_value::from_integer(
						 static_cast<std::int64_t>(value.source.size_bytes)),
					 text(value.source.encoding),
					 text(value.source.line_index_id),
					 canonical_value::from_boolean(value.source.read_only)}),
				canonical_value::from_tuple({
					text(value.source_closure.closure_id),
					text(value.source_closure.closure_digest),
					text(value.source_closure.manifest_digest),
					canonical_value::from_integer(
						static_cast<std::int64_t>(value.source_closure.member_count)),
					canonical_value::from_integer(
						static_cast<std::int64_t>(value.source_closure.blob_count)),
					canonical_value::from_integer(
						static_cast<std::int64_t>(value.source_closure.unique_blob_bytes)),
				}),
			};
			return canonical_identity_digest("build-capture", fields);
		}

		[[nodiscard]] bool same_catalog(const project_catalog& left,
										const project_catalog& right) noexcept
		{
			return left.catalog_id == right.catalog_id &&
				left.catalog_digest == right.catalog_digest &&
				left.logical_root == right.logical_root &&
				left.environment_digest == right.environment_digest &&
				left.compile_units == right.compile_units;
		}

		template <class T>
		void append_gap(std::vector<build_capture_gap>& output,
						const std::string_view field,
						const captured_value<T>& value)
		{
			if (value.state == capture_field_state::redacted ||
				value.state == capture_field_state::unavailable)
				output.push_back(
					{std::string{field}, value.state, value.reason, value.completion_action});
		}

		[[nodiscard]] std::vector<build_capture_gap> collect_gaps(const build_capture_draft& value)
		{
			std::vector<build_capture_gap> output;
			append_gap(
				output, "invocation.original_arguments", value.invocation.original_arguments);
			append_gap(output,
					   "invocation.normalized_semantic_options",
					   value.invocation.normalized_semantic_options);
			append_gap(output,
					   "invocation.effective_replay_arguments",
					   value.invocation.effective_replay_arguments);
			append_gap(output, "invocation.response_files", value.invocation.response_files);
			append_gap(output, "invocation.config_files", value.invocation.config_files);
			append_gap(
				output, "invocation.environment_effects", value.invocation.environment_effects);
			append_gap(output,
					   "toolchain.production_compiler_path",
					   value.toolchain.production_compiler_path);
			append_gap(output,
					   "toolchain.production_compiler_binary_digest",
					   value.toolchain.production_compiler_binary_digest);
			if (value.invocation.normalized_semantic_options.value)
				for (std::size_t index{};
					 index < value.invocation.normalized_semantic_options.value->size();
					 ++index)
				{
					const auto& option =
						(*value.invocation.normalized_semantic_options.value)[index];
					if (semantic_option_requires_gap(option.classification))
						output.push_back({"invocation.normalized_semantic_options[" +
											  std::to_string(index) + "]",
										  capture_field_state::unavailable,
										  option.reason,
										  option.completion_action});
				}
			if (value.invocation.environment_effects.value)
				for (const auto& effect : *value.invocation.environment_effects.value)
					append_gap(output,
							   "invocation.environment_effects." + effect.name,
							   effect.semantic_value);
			for (const auto* files :
				 {&value.invocation.response_files, &value.invocation.config_files})
				if (files->value)
					for (const auto& file : *files->value)
						append_gap(output,
								   "invocation.auxiliary_file." + file.logical_path,
								   file.content_digest);
			std::ranges::sort(
				output,
				[](const auto& left, const auto& right)
				{
					return std::tie(left.field, left.state, left.reason, left.completion_action) <
						std::tie(right.field, right.state, right.reason, right.completion_action);
				});
			return output;
		}
	} // namespace

	result<validated_build_capture> validate_build_capture(build_capture_draft draft,
														   const build_capture_limits limits)
	{
		if (limits.maximum_catalog_compile_units == 0U || limits.maximum_arguments == 0U ||
			limits.maximum_options == 0U || limits.maximum_auxiliary_files == 0U ||
			limits.maximum_environment_effects == 0U || limits.maximum_auxiliary_depth == 0U ||
			limits.maximum_string_bytes == 0U || limits.maximum_total_metadata_bytes == 0U ||
			limits.maximum_source_closure_members == 0U ||
			limits.maximum_source_closure_blobs == 0U || limits.maximum_source_closure_bytes == 0U)
			return unexpected(capture_error("limits", "zero"));
		metadata_budget budget{limits};

		if (auto valid = validate_id(draft.project_id, "project_id", budget); !valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = draft.catalog.validate(); !valid)
			return unexpected(
				capture_error("catalog", valid.error().code + ":" + valid.error().field));
		if (draft.catalog.compile_units.size() > limits.maximum_catalog_compile_units)
			return unexpected(capture_error(
				"catalog.compile_units", "count", "sdk.build-capture-limit-exceeded"));
		for (const auto& value : {
				 std::pair{std::string_view{draft.catalog.catalog_id}, "catalog.catalog_id"},
				 std::pair{std::string_view{draft.catalog.catalog_digest},
						   "catalog.catalog_digest"},
				 std::pair{std::string_view{draft.catalog.logical_root}, "catalog.logical_root"},
				 std::pair{std::string_view{draft.catalog.environment_digest},
						   "catalog.environment_digest"},
			 })
			if (auto valid = budget.add(value.first, value.second); !valid)
				return unexpected(std::move(valid.error()));
		for (std::size_t index{}; index < draft.catalog.compile_units.size(); ++index)
		{
			const auto& unit = draft.catalog.compile_units[index];
			const auto prefix = "catalog.compile_units[" + std::to_string(index) + "]";
			for (const auto& value : {
					 std::pair{std::string_view{unit.compile_unit_id}, ".compile_unit_id"},
					 std::pair{std::string_view{unit.effective_invocation_digest},
							   ".effective_invocation_digest"},
					 std::pair{std::string_view{unit.source_digest}, ".source_digest"},
					 std::pair{std::string_view{unit.environment_digest}, ".environment_digest"},
				 })
				if (auto valid = budget.add(value.first, prefix + value.second); !valid)
					return unexpected(std::move(valid.error()));
		}
		for (const auto& value :
			 {std::pair{std::string_view{draft.selected_catalog_compile_unit_id},
						"selected_catalog_compile_unit_id"},
			  std::pair{std::string_view{draft.compile_unit_id}, "compile_unit_id"},
			  std::pair{std::string_view{draft.build_variant_id}, "build_variant_id"},
			  std::pair{std::string_view{draft.toolchain_context_id}, "toolchain_context_id"}})
			if (auto valid = validate_id(value.first, value.second, budget); !valid)
				return unexpected(std::move(valid.error()));
		if (auto valid = validate_digest(draft.toolchain_digest, "toolchain_digest", budget);
			!valid)
			return unexpected(std::move(valid.error()));

		const auto selected = std::ranges::find(draft.catalog.compile_units,
												draft.selected_catalog_compile_unit_id,
												&catalog_compile_unit::compile_unit_id);
		if (selected == draft.catalog.compile_units.end())
			return unexpected(capture_error("selected_catalog_compile_unit_id", "not-in-catalog"));

		for (const auto& value : {
				 std::pair{std::string_view{draft.toolchain.family}, "toolchain.family"},
				 std::pair{std::string_view{draft.toolchain.exact_version},
						   "toolchain.exact_version"},
				 std::pair{std::string_view{draft.toolchain.target_triple},
						   "toolchain.target_triple"},
				 std::pair{std::string_view{draft.variant.language}, "variant.language"},
				 std::pair{std::string_view{draft.variant.language_standard},
						   "variant.language_standard"},
				 std::pair{std::string_view{draft.variant.target_triple}, "variant.target_triple"},
				 std::pair{std::string_view{draft.invocation.language}, "invocation.language"},
			 })
			if (auto valid = validate_id(value.first, value.second, budget); !valid)
				return unexpected(std::move(valid.error()));
		for (const auto& value : {
				 std::pair{std::string_view{draft.toolchain.builtin_headers_digest},
						   "toolchain.builtin_headers_digest"},
				 std::pair{std::string_view{draft.toolchain.abi_digest}, "toolchain.abi_digest"},
				 std::pair{std::string_view{draft.toolchain.plugin_spec_digest},
						   "toolchain.plugin_spec_digest"},
				 std::pair{std::string_view{draft.variant.predefined_macros_digest},
						   "variant.predefined_macros_digest"},
				 std::pair{std::string_view{draft.variant.include_search_digest},
						   "variant.include_search_digest"},
				 std::pair{std::string_view{draft.variant.semantic_flags_digest},
						   "variant.semantic_flags_digest"},
				 std::pair{std::string_view{draft.invocation.effective_invocation_digest},
						   "invocation.effective_invocation_digest"},
				 std::pair{std::string_view{draft.invocation.environment_digest},
						   "invocation.environment_digest"},
			 })
			if (auto valid = validate_digest(value.first, value.second, budget); !valid)
				return unexpected(std::move(valid.error()));
		if (draft.toolchain.target_triple != draft.variant.target_triple ||
			draft.variant.language != draft.invocation.language)
			return unexpected(capture_error("variant", "toolchain-or-language-mismatch"));
		if (auto valid = budget.add_optional(draft.toolchain.sysroot, "toolchain.sysroot"); !valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = validate_capture_shape(draft.toolchain.production_compiler_path,
												"toolchain.production_compiler_path",
												budget);
			!valid)
			return unexpected(std::move(valid.error()));
		if (draft.toolchain.production_compiler_path.value)
			if (auto valid = budget.add(*draft.toolchain.production_compiler_path.value,
										"toolchain.production_compiler_path");
				!valid)
				return unexpected(std::move(valid.error()));
		if (auto valid = validate_capture_shape(draft.toolchain.production_compiler_binary_digest,
												"toolchain.production_compiler_binary_digest",
												budget);
			!valid)
			return unexpected(std::move(valid.error()));
		if (draft.toolchain.production_compiler_binary_digest.value)
			if (auto valid =
					validate_digest(*draft.toolchain.production_compiler_binary_digest.value,
									"toolchain.production_compiler_binary_digest",
									budget);
				!valid)
				return unexpected(std::move(valid.error()));

		if (auto valid = validate_string_list(draft.invocation.original_arguments,
											  "invocation.original_arguments",
											  limits.maximum_arguments,
											  budget,
											  true);
			!valid)
			return unexpected(std::move(valid.error()));
		if (auto valid =
				validate_options(draft.invocation.normalized_semantic_options, limits, budget);
			!valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = validate_string_list(draft.invocation.effective_replay_arguments,
											  "invocation.effective_replay_arguments",
											  limits.maximum_arguments,
											  budget,
											  true);
			!valid)
			return unexpected(std::move(valid.error()));
		if (!draft.invocation.effective_replay_arguments.value)
			return unexpected(capture_error("invocation.effective_replay_arguments", "required"));
		if (auto valid = validate_auxiliary_files(
				draft.invocation.response_files, "invocation.response_files", limits, budget);
			!valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = validate_auxiliary_files(
				draft.invocation.config_files, "invocation.config_files", limits, budget);
			!valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = validate_environment(draft.invocation.environment_effects, limits, budget);
			!valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = validate_id(draft.invocation.logical_working_directory,
									 "invocation.logical_working_directory",
									 budget);
			!valid)
			return unexpected(std::move(valid.error()));
		if (!std::ranges::is_sorted(draft.invocation.qualified_read_roots) ||
			std::ranges::adjacent_find(draft.invocation.qualified_read_roots) !=
				draft.invocation.qualified_read_roots.end())
			return unexpected(capture_error("invocation.qualified_read_roots", "noncanonical"));
		for (const auto& root : draft.invocation.qualified_read_roots)
		{
			if (root.empty() || root.front() != '/')
				return unexpected(capture_error("invocation.qualified_read_roots", "not-absolute"));
			if (auto valid = budget.add(root, "invocation.qualified_read_roots"); !valid)
				return unexpected(std::move(valid.error()));
		}

		for (const auto& value : {
				 std::pair{std::string_view{draft.source.source_snapshot_id},
						   "source.source_snapshot_id"},
				 std::pair{std::string_view{draft.source.file_id}, "source.file_id"},
				 std::pair{std::string_view{draft.source.logical_path}, "source.logical_path"},
				 std::pair{std::string_view{draft.source.encoding}, "source.encoding"},
				 std::pair{std::string_view{draft.source.line_index_id}, "source.line_index_id"},
			 })
			if (auto valid = validate_id(value.first, value.second, budget); !valid)
				return unexpected(std::move(valid.error()));
		if (auto valid =
				validate_digest(draft.source.content_digest, "source.content_digest", budget);
			!valid)
			return unexpected(std::move(valid.error()));
		if (draft.source.content_digest != selected->source_digest ||
			draft.invocation.effective_invocation_digest != selected->effective_invocation_digest ||
			draft.invocation.environment_digest != selected->environment_digest)
			return unexpected(capture_error("catalog.compile_unit", "input-binding-mismatch"));

		for (const auto& value : {
				 std::pair{std::string_view{draft.source_closure.closure_id},
						   "source_closure.closure_id"},
				 std::pair{std::string_view{draft.source_closure.closure_digest},
						   "source_closure.closure_digest"},
				 std::pair{std::string_view{draft.source_closure.manifest_digest},
						   "source_closure.manifest_digest"},
			 })
			if (value.second == std::string_view{"source_closure.closure_id"})
			{
				if (auto valid = validate_id(value.first, value.second, budget); !valid)
					return unexpected(std::move(valid.error()));
			}
			else if (auto valid = validate_digest(value.first, value.second, budget); !valid)
				return unexpected(std::move(valid.error()));
		if (draft.source_closure.member_count == 0U || draft.source_closure.blob_count == 0U)
			return unexpected(capture_error("source_closure", "empty"));
		if (draft.source_closure.member_count > limits.maximum_source_closure_members ||
			draft.source_closure.blob_count > limits.maximum_source_closure_blobs ||
			draft.source_closure.unique_blob_bytes > limits.maximum_source_closure_bytes)
			return unexpected(
				capture_error("source_closure", "bounds", "sdk.build-capture-limit-exceeded"));
		if (draft.source.size_bytes > draft.source_closure.unique_blob_bytes)
			return unexpected(capture_error("source_closure", "main-source-size-mismatch"));
		if (draft.source.size_bytes >
				static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
			draft.source_closure.member_count >
				static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
			draft.source_closure.blob_count >
				static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
			draft.source_closure.unique_blob_bytes >
				static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
			return unexpected(capture_error("source_closure", "overflow"));

		auto identity = capture_identity(draft);
		if (!identity)
			return unexpected(std::move(identity.error()));
		auto gaps = collect_gaps(draft);
		return validated_build_capture{std::move(draft), std::move(*identity), std::move(gaps)};
	}

	result<std::vector<validated_build_capture>>
	validate_build_capture_set(std::vector<build_capture_draft> drafts,
							   const build_capture_limits limits)
	{
		if (drafts.empty())
			return unexpected(capture_error("captures", "empty"));
		std::vector<validated_build_capture> output;
		output.reserve(drafts.size());
		std::set<std::string, std::less<>> compile_units;
		std::set<std::string, std::less<>> identities;
		std::map<std::string, build_capture_variant, std::less<>> variants;
		std::map<std::string, std::pair<std::string, build_capture_toolchain>, std::less<>>
			toolchains;
		std::map<std::string, project_catalog, std::less<>> projects;
		for (auto& draft : drafts)
		{
			auto validated = validate_build_capture(std::move(draft), limits);
			if (!validated)
				return unexpected(std::move(validated.error()));
			const auto& value = validated->value();
			if (!compile_units.insert(value.compile_unit_id).second)
				return unexpected(capture_error("captures", "duplicate-compile-unit"));
			if (!identities.insert(std::string{validated->semantic_identity()}).second)
				return unexpected(capture_error("captures", "duplicate-semantic-identity"));
			if (const auto [found, inserted] =
					variants.emplace(value.build_variant_id, value.variant);
				!inserted && found->second != value.variant)
				return unexpected(capture_error("captures", "conflicting-build-variant"));
			if (const auto [found, inserted] = toolchains.emplace(
					value.toolchain_context_id, std::pair{value.toolchain_digest, value.toolchain});
				!inserted && found->second != std::pair{value.toolchain_digest, value.toolchain})
				return unexpected(capture_error("captures", "conflicting-toolchain-context"));
			if (const auto [found, inserted] = projects.emplace(value.project_id, value.catalog);
				!inserted && !same_catalog(found->second, value.catalog))
				return unexpected(capture_error("captures", "conflicting-project"));
			output.push_back(std::move(*validated));
		}
		return output;
	}
} // namespace cxxlens::sdk::detail
