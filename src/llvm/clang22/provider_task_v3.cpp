#include "provider_task_v3.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/source_file.hpp>

#include "unicode_nfc.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		constexpr std::string_view provider_id = "cxxlens.clang22.reference";
		constexpr std::string_view task_magic = "cxxlens.clang22.task.v3";
		const sdk::semantic_version provider_version{1U, 0U, 0U};
		constexpr std::uint32_t maximum_arguments = 4096U;
		constexpr std::uint64_t maximum_source_bytes = 16U * 1024U * 1024U;
		constexpr std::array<std::string_view, 6U> exact_descriptor_ids{
			"cc.call_direct_target.v1",
			"cc.call_site.v1",
			"cc.entity.v1",
			"frontend.clang22.call_observation.v2",
			"frontend.clang22.entity_observation.v2",
			"frontend.clang22.type_observation.v2",
		};
		constexpr std::array<std::string_view, 2U> exact_dependency_groups{"canonical",
																		   "observation"};

		[[nodiscard]] sdk::error
		provider_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::detached_cell symbol_cell(const sdk::scalar_kind kind,
													 std::string parameter,
													 std::string value,
													 const bool optional = false)
		{
			return {{kind, std::move(parameter), optional},
					sdk::cell_state::present,
					sdk::scalar_value{std::move(value)},
					std::nullopt};
		}

		[[nodiscard]] sdk::detached_cell optional_typed(std::string parameter, std::string value)
		{
			auto output = sdk::detached_cell::typed(std::move(parameter), std::move(value));
			output.type.optional = true;
			return output;
		}

		[[nodiscard]] bool canonical_digest(const std::string_view value)
		{
			const auto hex = value.starts_with("sha256:")  ? value.substr(7U)
				: value.starts_with("semantic-v2:sha256:") ? value.substr(19U)
														   : std::string_view{};
			return hex.size() == 64U &&
				std::ranges::all_of(hex,
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] bool schema_logical_path(const std::string_view value)
		{
			static constexpr std::array<std::string_view, 7U> prefixes{
				"project://",
				"build://",
				"toolchain://",
				"sysroot://",
				"generated://",
				"provider://",
				"external://",
			};
			if (!sdk::validate_utf8_text(value) ||
				std::ranges::any_of(value,
									[](const unsigned char byte)
									{
										return byte <= 0x1fU || byte == 0x7fU;
									}))
				return false;
			const auto prefix = std::ranges::find_if(prefixes,
													 [&](const std::string_view candidate)
													 {
														 return value.starts_with(candidate);
													 });
			return prefix != prefixes.end() && value.size() > prefix->size();
		}

		[[nodiscard]] sdk::canonical_value string_tuple(const std::span<const std::string> values)
		{
			std::vector<sdk::canonical_value> output;
			output.reserve(values.size());
			for (const auto& value : values)
				output.push_back(sdk::canonical_value::from_string(value));
			return sdk::canonical_value::from_tuple(std::move(output));
		}

		[[nodiscard]] sdk::canonical_value
		canonical_object(std::vector<std::pair<std::string, sdk::canonical_value>> fields)
		{
			std::ranges::sort(fields, {}, &std::pair<std::string, sdk::canonical_value>::first);
			std::vector<sdk::canonical_value> output;
			output.reserve(fields.size());
			for (auto& [key, value] : fields)
				output.push_back(sdk::canonical_value::from_tuple(
					{sdk::canonical_value::from_string(std::move(key)), std::move(value)}));
			return sdk::canonical_value::from_tuple(std::move(output));
		}

		[[nodiscard]] sdk::result<std::string> base64_decode(const std::string_view input)
		{
			if (input.size() % 4U != 0U)
				return sdk::unexpected(
					provider_error("provider.frontend-request-invalid", "source.content_base64"));
			const auto decode = [](const char value) -> std::optional<std::uint32_t>
			{
				if (value >= 'A' && value <= 'Z')
					return static_cast<std::uint32_t>(value - 'A');
				if (value >= 'a' && value <= 'z')
					return static_cast<std::uint32_t>(value - 'a' + 26);
				if (value >= '0' && value <= '9')
					return static_cast<std::uint32_t>(value - '0' + 52);
				if (value == '+')
					return 62U;
				if (value == '/')
					return 63U;
				return std::nullopt;
			};
			std::string output;
			output.reserve((input.size() / 4U) * 3U);
			for (std::size_t offset{}; offset < input.size(); offset += 4U)
			{
				const bool final = offset + 4U == input.size();
				const bool padding_two = input[offset + 2U] == '=';
				const bool padding_one = input[offset + 3U] == '=';
				if ((!final && (padding_two || padding_one)) || (padding_two && !padding_one))
					return sdk::unexpected(provider_error("provider.frontend-request-invalid",
														  "source.content_base64"));
				auto first = decode(input[offset]);
				auto second = decode(input[offset + 1U]);
				auto third =
					padding_two ? std::optional<std::uint32_t>{0U} : decode(input[offset + 2U]);
				auto fourth =
					padding_one ? std::optional<std::uint32_t>{0U} : decode(input[offset + 3U]);
				if (!first || !second || !third || !fourth)
					return sdk::unexpected(provider_error("provider.frontend-request-invalid",
														  "source.content_base64"));
				const auto word = (*first << 18U) | (*second << 12U) | (*third << 6U) | *fourth;
				output.push_back(static_cast<char>((word >> 16U) & 0xffU));
				if (!padding_two)
					output.push_back(static_cast<char>((word >> 8U) & 0xffU));
				if (!padding_one)
					output.push_back(static_cast<char>(word & 0xffU));
			}
			return output;
		}

		[[nodiscard]] sdk::canonical_value catalog_projection(const clang22_task_input& input)
		{
			std::vector<sdk::canonical_value> entries;
			entries.reserve(input.project_catalog.compile_units.size());
			for (const auto& unit : input.project_catalog.compile_units)
				entries.push_back(canonical_object({
					{"catalog_compile_unit_id",
					 sdk::canonical_value::from_string(unit.compile_unit_id)},
					{"effective_invocation_digest",
					 sdk::canonical_value::from_string(unit.effective_invocation_digest)},
					{"source_digest", sdk::canonical_value::from_string(unit.source_digest)},
					{"environment_digest",
					 sdk::canonical_value::from_string(unit.environment_digest)},
				}));
			return canonical_object({
				{"catalog_id", sdk::canonical_value::from_string(input.project_catalog.catalog_id)},
				{"catalog_digest",
				 sdk::canonical_value::from_string(input.project_catalog.catalog_digest)},
				{"logical_root",
				 sdk::canonical_value::from_string(input.project_catalog.logical_root)},
				{"catalog_environment_digest",
				 sdk::canonical_value::from_string(input.project_catalog.environment_digest)},
				{"catalog_compile_units", sdk::canonical_value::from_tuple(std::move(entries))},
			});
		}

		[[nodiscard]] sdk::canonical_value task_payload_projection(const clang22_task_input& input)
		{
			const auto integer = [](const std::uint64_t value)
			{
				return sdk::canonical_value::from_integer(static_cast<std::int64_t>(value));
			};
			return canonical_object({
				{"project_id", sdk::canonical_value::from_string(input.project)},
				{"catalog_id", sdk::canonical_value::from_string(input.project_catalog.catalog_id)},
				{"catalog_digest",
				 sdk::canonical_value::from_string(input.project_catalog.catalog_digest)},
				{"build_variant_id", sdk::canonical_value::from_string(input.variant)},
				{"toolchain_context_id",
				 sdk::canonical_value::from_string(input.toolchain_context)},
				{"toolchain_digest", sdk::canonical_value::from_string(input.toolchain_digest)},
				{"toolchain",
				 canonical_object({
					 {"family", sdk::canonical_value::from_string(input.toolchain.family)},
					 {"exact_version",
					  sdk::canonical_value::from_string(input.toolchain.exact_version)},
					 {"target_triple",
					  sdk::canonical_value::from_string(input.toolchain.target_triple)},
					 {"builtin_headers_digest",
					  sdk::canonical_value::from_string(input.toolchain.builtin_headers_digest)},
					 {"sysroot",
					  input.toolchain.sysroot
						  ? sdk::canonical_value::from_string(*input.toolchain.sysroot)
						  : sdk::canonical_value::null()},
					 {"abi_digest", sdk::canonical_value::from_string(input.toolchain.abi_digest)},
					 {"plugin_spec_digest",
					  sdk::canonical_value::from_string(input.toolchain.plugin_spec_digest)},
				 })},
				{"variant",
				 canonical_object({
					 {"language",
					  sdk::canonical_value::from_string(input.variant_authority.language)},
					 {"language_standard",
					  sdk::canonical_value::from_string(input.variant_authority.language_standard)},
					 {"target_triple",
					  sdk::canonical_value::from_string(input.variant_authority.target_triple)},
					 {"predefined_macros_digest",
					  sdk::canonical_value::from_string(
						  input.variant_authority.predefined_macros_digest)},
					 {"include_search_digest",
					  sdk::canonical_value::from_string(
						  input.variant_authority.include_search_digest)},
					 {"semantic_flags_digest",
					  sdk::canonical_value::from_string(
						  input.variant_authority.semantic_flags_digest)},
				 })},
				{"normalized_invocation_digest",
				 sdk::canonical_value::from_string(input.normalized_invocation_digest)},
				{"environment_digest", sdk::canonical_value::from_string(input.environment_digest)},
				{"language", sdk::canonical_value::from_string(input.language)},
				{"working_directory", sdk::canonical_value::from_string(input.working_directory)},
				{"condition_universe_id",
				 sdk::canonical_value::from_string(input.condition_universe)},
				{"condition_id", sdk::canonical_value::from_string(input.condition)},
				{"interpretation_domain", sdk::canonical_value::from_string(input.interpretation)},
				{"source",
				 canonical_object({
					 {"source_snapshot_id",
					  sdk::canonical_value::from_string(input.source_snapshot)},
					 {"file_id", sdk::canonical_value::from_string(input.file)},
					 {"logical_path", sdk::canonical_value::from_string(input.logical_path)},
					 {"content_digest",
					  sdk::canonical_value::from_string(input.source_content_digest)},
					 {"size_bytes", integer(input.source_size_bytes)},
					 {"encoding", sdk::canonical_value::from_string(input.source_encoding)},
					 {"line_index_id", sdk::canonical_value::from_string(input.line_index)},
					 {"read_only", sdk::canonical_value::from_boolean(input.source_read_only)},
					 {"content_base64",
					  sdk::canonical_value::from_string(input.source_content_base64)},
				 })},
				{"effective_argv", string_tuple(input.arguments)},
				{"requested_descriptor_ids", string_tuple(input.requested_descriptors)},
				{"dependency_groups", string_tuple(input.dependency_groups)},
				{"budget",
				 canonical_object({
					 {"output_bytes", integer(input.budget.output_bytes)},
					 {"rows", integer(input.budget.rows)},
					 {"diagnostics", integer(input.budget.diagnostics)},
					 {"wall_ms", integer(input.budget.wall_ms)},
					 {"cpu_ms", integer(input.budget.cpu_ms)},
					 {"address_space_bytes", integer(input.budget.address_space_bytes)},
					 {"transport_bytes", integer(input.budget.transport_bytes)},
					 {"open_files", integer(input.budget.open_files)},
					 {"subprocesses", integer(input.budget.subprocesses)},
				 })},
				{"sandbox",
				 canonical_object({
					 {"minimum", sdk::canonical_value::from_string(input.sandbox.minimum)},
					 {"policy_digest",
					  sdk::canonical_value::from_string(input.sandbox.policy_digest)},
				 })},
			});
		}

		[[nodiscard]] sdk::canonical_value task_v3_projection(const clang22_task_input& input)
		{
			return sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_string(std::string{task_magic}),
				catalog_projection(input),
				sdk::canonical_value::from_string(input.selected_catalog_compile_unit),
				sdk::canonical_value::from_string(input.compile_unit),
				task_payload_projection(input),
			});
		}

		[[nodiscard]] sdk::result<std::string>
		semantic_digest_projection(const std::string_view domain,
								   const sdk::canonical_value& projection)
		{
			auto encoded = sdk::canonical_binary(projection);
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			const std::string bytes_value{reinterpret_cast<const char*>(encoded->data()),
										  encoded->size()};
			return sdk::semantic_digest(domain, bytes_value);
		}

		using canonical_fields =
			std::map<std::string_view, const sdk::canonical_value*, std::less<>>;

		[[nodiscard]] sdk::result<canonical_fields>
		parse_object(const sdk::canonical_value& value,
					 const std::span<const std::string_view> expected,
					 const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::ordered_tuple ||
				value.tuple.size() != expected.size())
				return sdk::unexpected(provider_error(
					"provider.frontend-request-invalid", std::string{field}, "object-shape"));
			canonical_fields output;
			for (std::size_t index{}; index < expected.size(); ++index)
			{
				const auto& entry = value.tuple[index];
				if (entry.type != sdk::canonical_value::kind::ordered_tuple ||
					entry.tuple.size() != 2U ||
					entry.tuple[0U].type != sdk::canonical_value::kind::utf8_string ||
					entry.tuple[0U].text != expected[index])
					return sdk::unexpected(provider_error(
						"provider.frontend-request-invalid", std::string{field}, "object-key"));
				output.emplace(entry.tuple[0U].text, &entry.tuple[1U]);
			}
			return output;
		}

		[[nodiscard]] sdk::result<std::string> parse_string(const sdk::canonical_value& value,
															const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::utf8_string)
				return sdk::unexpected(provider_error(
					"provider.frontend-request-invalid", std::string{field}, "string"));
			return value.text;
		}

		[[nodiscard]] sdk::result<std::uint64_t>
		parse_positive_integer(const sdk::canonical_value& value, const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::signed_integer || value.integer <= 0)
				return sdk::unexpected(provider_error(
					"provider.frontend-request-invalid", std::string{field}, "positive-int64"));
			return static_cast<std::uint64_t>(value.integer);
		}

		[[nodiscard]] sdk::result<std::uint64_t>
		parse_nonnegative_integer(const sdk::canonical_value& value, const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::signed_integer || value.integer < 0)
				return sdk::unexpected(provider_error(
					"provider.frontend-request-invalid", std::string{field}, "nonnegative-int64"));
			return static_cast<std::uint64_t>(value.integer);
		}

		[[nodiscard]] sdk::result<std::vector<std::string>>
		parse_string_tuple(const sdk::canonical_value& value, const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::ordered_tuple)
				return sdk::unexpected(provider_error(
					"provider.frontend-request-invalid", std::string{field}, "array"));
			std::vector<std::string> output;
			output.reserve(value.tuple.size());
			for (const auto& item : value.tuple)
			{
				auto parsed = parse_string(item, field);
				if (!parsed)
					return sdk::unexpected(std::move(parsed.error()));
				output.push_back(std::move(*parsed));
			}
			return output;
		}

		[[nodiscard]] sdk::result<void>
		validate_base_identity_bindings(const clang22_task_input& input)
		{
			const auto mismatch = [](const std::string_view field)
			{
				return sdk::unexpected(provider_error(
					"provider.frontend-request-invalid", std::string{field}, "identity-mismatch"));
			};
			const auto derive = [](const sdk::relation_descriptor& descriptor,
								   sdk::result<sdk::detached_row> row) -> sdk::result<std::string>
			{
				if (!row)
					return sdk::unexpected(std::move(row.error()));
				return sdk::derive_domain_identity(descriptor, *row);
			};

			constexpr std::string_view project_prefix = "project://";
			auto normalized_path = is_nfc_utf8(input.logical_path);
			if (!normalized_path || !*normalized_path ||
				!input.logical_path.starts_with(project_prefix) ||
				std::ranges::any_of(input.logical_path,
									[](const unsigned char value)
									{
										return value <= 0x1fU || value == 0x7fU;
									}))
				return mismatch("source.logical_path");
			const auto relative =
				std::string_view{input.logical_path}.substr(project_prefix.size());
			if (relative.empty() || relative.contains('\\') || relative.contains('?') ||
				relative.contains('#'))
				return mismatch("source.logical_path");
			std::size_t begin{};
			while (begin <= relative.size())
			{
				const auto end = relative.find('/', begin);
				const auto segment = relative.substr(
					begin, end == std::string_view::npos ? relative.size() - begin : end - begin);
				if (segment.empty() || segment == "." || segment == "..")
					return mismatch("source.logical_path");
				if (end == std::string_view::npos)
					break;
				begin = end + 1U;
			}
			const std::array file_fields{
				sdk::canonical_value::from_string("project"),
				sdk::canonical_value::from_string(std::string{relative}),
				sdk::canonical_value::from_string("cxxlens.logical-path.v1"),
			};
			auto expected_file = sdk::canonical_identity_digest("file", file_fields);
			if (!expected_file || *expected_file != input.file)
				return mismatch("source.file_id");

			std::vector<sdk::canonical_value> offset_values{sdk::canonical_value::from_integer(0)};
			for (std::size_t index{}; index < input.source.size(); ++index)
				if (input.source[index] == '\n')
					offset_values.push_back(
						sdk::canonical_value::from_integer(static_cast<std::int64_t>(index + 1U)));
			const std::array line_index_fields{
				sdk::canonical_value::from_string("cxxlens.byte-line-index.v1"),
				sdk::canonical_value::from_string(input.source_content_digest),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(input.source_size_bytes)),
				sdk::canonical_value::from_tuple(std::move(offset_values)),
			};
			auto expected_line_index =
				sdk::canonical_identity_digest("line-index", line_index_fields);
			if (!expected_line_index || *expected_line_index != input.line_index)
				return mismatch("source.line_index_id");

			using project_relation = build::relations::project;
			project_relation::builder project_builder;
			for (auto result : {
					 project_builder.set<project_relation::project_column>(
						 sdk::detached_cell::typed("project_id", "pending")),
					 project_builder.set<project_relation::catalog>(
						 sdk::detached_cell::typed("catalog_id", input.project_catalog.catalog_id)),
					 project_builder.set<project_relation::catalog_digest>(symbol_cell(
						 sdk::scalar_kind::digest, {}, input.project_catalog.catalog_digest)),
					 project_builder.set<project_relation::logical_root>(sdk::detached_cell::typed(
						 "logical_path_id", input.project_catalog.logical_root)),
					 project_builder.set<project_relation::environment_digest>(symbol_cell(
						 sdk::scalar_kind::digest, {}, input.project_catalog.environment_digest)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			auto expected_project =
				derive(project_relation::descriptor(), std::move(project_builder).finish());
			if (!expected_project || *expected_project != input.project)
				return mismatch("project_id");

			using toolchain_relation = build::relations::toolchain_context;
			toolchain_relation::builder toolchain_builder;
			for (auto result : {
					 toolchain_builder.set<toolchain_relation::toolchain>(
						 sdk::detached_cell::typed("toolchain_context_id", "pending")),
					 toolchain_builder.set<toolchain_relation::family>(
						 symbol_cell(sdk::scalar_kind::open_symbol,
									 "build.toolchain-family/1",
									 input.toolchain.family)),
					 toolchain_builder.set<toolchain_relation::exact_version>(
						 sdk::detached_cell::utf8(input.toolchain.exact_version)),
					 toolchain_builder.set<toolchain_relation::target_triple>(
						 sdk::detached_cell::utf8(input.toolchain.target_triple)),
					 toolchain_builder.set<toolchain_relation::builtin_headers_digest>(symbol_cell(
						 sdk::scalar_kind::digest, {}, input.toolchain.builtin_headers_digest)),
					 toolchain_builder.set<toolchain_relation::abi_digest>(
						 symbol_cell(sdk::scalar_kind::digest, {}, input.toolchain.abi_digest)),
					 toolchain_builder.set<toolchain_relation::plugin_spec_digest>(symbol_cell(
						 sdk::scalar_kind::digest, {}, input.toolchain.plugin_spec_digest)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			if (input.toolchain.sysroot)
			{
				auto result = toolchain_builder.set<toolchain_relation::sysroot>(
					optional_typed("logical_path_id", *input.toolchain.sysroot));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			auto toolchain_row = std::move(toolchain_builder).finish();
			if (!toolchain_row)
				return sdk::unexpected(std::move(toolchain_row.error()));
			auto expected_toolchain =
				sdk::derive_domain_identity(toolchain_relation::descriptor(), *toolchain_row);
			if (!expected_toolchain || *expected_toolchain != input.toolchain_context)
				return mismatch("toolchain_context_id");
			auto expected_toolchain_digest = semantic_digest_projection(
				"cxxlens.base-claim-row.v1",
				canonical_object({
					{"descriptor_id",
					 sdk::canonical_value::from_string("build.toolchain_context.v1")},
					{"row",
					 canonical_object({
						 {"toolchain", sdk::canonical_value::from_string(*expected_toolchain)},
						 {"family", sdk::canonical_value::from_string(input.toolchain.family)},
						 {"exact_version",
						  sdk::canonical_value::from_string(input.toolchain.exact_version)},
						 {"target_triple",
						  sdk::canonical_value::from_string(input.toolchain.target_triple)},
						 {"builtin_headers_digest",
						  sdk::canonical_value::from_string(
							  input.toolchain.builtin_headers_digest)},
						 {"sysroot",
						  input.toolchain.sysroot
							  ? sdk::canonical_value::from_string(*input.toolchain.sysroot)
							  : sdk::canonical_value::null()},
						 {"abi_digest",
						  sdk::canonical_value::from_string(input.toolchain.abi_digest)},
						 {"plugin_spec_digest",
						  sdk::canonical_value::from_string(input.toolchain.plugin_spec_digest)},
					 })},
				}));
			if (!expected_toolchain_digest || *expected_toolchain_digest != input.toolchain_digest)
				return mismatch("toolchain_digest");

			using variant_relation = build::relations::variant;
			variant_relation::builder variant_builder;
			for (auto result : {
					 variant_builder.set<variant_relation::variant_column>(
						 sdk::detached_cell::typed("build_variant_id", "pending")),
					 variant_builder.set<variant_relation::project>(
						 sdk::detached_cell::typed("project_id", input.project)),
					 variant_builder.set<variant_relation::toolchain>(sdk::detached_cell::typed(
						 "toolchain_context_id", input.toolchain_context)),
					 variant_builder.set<variant_relation::language>(
						 symbol_cell(sdk::scalar_kind::open_symbol,
									 "build.language/1",
									 input.variant_authority.language)),
					 variant_builder.set<variant_relation::language_standard>(
						 symbol_cell(sdk::scalar_kind::open_symbol,
									 "build.language-standard/1",
									 input.variant_authority.language_standard)),
					 variant_builder.set<variant_relation::target_triple>(
						 sdk::detached_cell::utf8(input.variant_authority.target_triple)),
					 variant_builder.set<variant_relation::predefined_macros_digest>(
						 symbol_cell(sdk::scalar_kind::digest,
									 {},
									 input.variant_authority.predefined_macros_digest)),
					 variant_builder.set<variant_relation::include_search_digest>(
						 symbol_cell(sdk::scalar_kind::digest,
									 {},
									 input.variant_authority.include_search_digest)),
					 variant_builder.set<variant_relation::semantic_flags_digest>(
						 symbol_cell(sdk::scalar_kind::digest,
									 {},
									 input.variant_authority.semantic_flags_digest)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			auto expected_variant =
				derive(variant_relation::descriptor(), std::move(variant_builder).finish());
			if (!expected_variant || *expected_variant != input.variant)
				return mismatch("build_variant_id");

			using source_relation = source::relations::file;
			source_relation::builder source_builder;
			for (auto result : {
					 source_builder.set<source_relation::snapshot>(
						 sdk::detached_cell::typed("source_snapshot_id", "pending")),
					 source_builder.set<source_relation::file_column>(
						 sdk::detached_cell::typed("file_id", input.file)),
					 source_builder.set<source_relation::project>(
						 sdk::detached_cell::typed("project_id", input.project)),
					 source_builder.set<source_relation::logical_path>(
						 sdk::detached_cell::typed("logical_path_id", input.logical_path)),
					 source_builder.set<source_relation::content>(
						 symbol_cell(sdk::scalar_kind::digest, {}, input.source_content_digest)),
					 source_builder.set<source_relation::size>(
						 sdk::detached_cell::unsigned_integer(input.source_size_bytes)),
					 source_builder.set<source_relation::encoding>(
						 symbol_cell(sdk::scalar_kind::open_symbol,
									 "source.encoding/1",
									 input.source_encoding)),
					 source_builder.set<source_relation::line_index>(
						 sdk::detached_cell::typed("line_index_id", input.line_index)),
					 source_builder.set<source_relation::read_only>(
						 sdk::detached_cell::boolean(input.source_read_only)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			auto expected_source =
				derive(source_relation::descriptor(), std::move(source_builder).finish());
			if (!expected_source || *expected_source != input.source_snapshot)
				return mismatch("source.source_snapshot_id");

			using compile_unit_relation = build::relations::compile_unit;
			compile_unit_relation::builder compile_unit_builder;
			for (auto result : {
					 compile_unit_builder.set<compile_unit_relation::compile_unit_column>(
						 sdk::detached_cell::typed("compile_unit_id", "pending")),
					 compile_unit_builder.set<compile_unit_relation::project>(
						 sdk::detached_cell::typed("project_id", input.project)),
					 compile_unit_builder.set<compile_unit_relation::main_source>(
						 sdk::detached_cell::typed("source_snapshot_id", input.source_snapshot)),
					 compile_unit_builder.set<compile_unit_relation::variant>(
						 sdk::detached_cell::typed("build_variant_id", input.variant)),
					 compile_unit_builder.set<compile_unit_relation::toolchain>(
						 sdk::detached_cell::typed("toolchain_context_id",
												   input.toolchain_context)),
					 compile_unit_builder.set<compile_unit_relation::effective_invocation_digest>(
						 symbol_cell(
							 sdk::scalar_kind::digest, {}, input.normalized_invocation_digest)),
					 compile_unit_builder.set<compile_unit_relation::language>(symbol_cell(
						 sdk::scalar_kind::open_symbol, "build.language/1", input.language)),
					 compile_unit_builder.set<compile_unit_relation::working_directory>(
						 sdk::detached_cell::typed("logical_path_id", input.working_directory)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			auto expected_compile_unit = derive(compile_unit_relation::descriptor(),
												std::move(compile_unit_builder).finish());
			if (!expected_compile_unit || *expected_compile_unit != input.compile_unit)
				return mismatch("compile_unit_id");
			return {};
		}
	} // namespace

	sdk::result<void> clang22_task_input::validate() const
	{
		const auto invalid = [](const std::string_view field, std::string detail = {})
		{
			return sdk::unexpected(provider_error(
				"provider.frontend-request-invalid", std::string{field}, std::move(detail)));
		};
		if (auto valid = project_catalog.validate(); !valid)
			return invalid("project_catalog", valid.error().code);
		if (project_catalog.compile_units.empty() || project_catalog.compile_units.size() > 4096U)
			return invalid("project_catalog", "compile-unit-count");
		for (const auto value : {
				 std::string_view{selected_catalog_compile_unit},
				 std::string_view{compile_unit},
				 std::string_view{project},
				 std::string_view{variant},
				 std::string_view{toolchain_context},
				 std::string_view{language},
				 std::string_view{condition_universe},
				 std::string_view{condition},
				 std::string_view{source_snapshot},
				 std::string_view{file},
				 std::string_view{line_index},
			 })
			if (auto valid = sdk::validate_strong_id(value); !valid)
				return invalid("identity", valid.error().detail);
		if (selected_catalog_compile_unit == compile_unit)
			return invalid("compile_unit", "catalog-local-final-alias");
		if (interpretation != "cc.clang22-canonical-1")
			return invalid("interpretation_domain", "unsupported");
		if (!schema_logical_path(project_catalog.logical_root) ||
			!schema_logical_path(working_directory) || logical_path.empty() ||
			!logical_path.starts_with("project://") || source_size_bytes > maximum_source_bytes ||
			source_size_bytes != source.size())
			return invalid("source", "path-or-size");
		if (source_encoding != "utf8" && source_encoding != "utf16le" &&
			source_encoding != "utf16be" && source_encoding != "locale_dependent" &&
			source_encoding != "binary_or_unknown")
			return invalid("source.encoding", "closed-enum");
		if (arguments.empty() || arguments.size() > maximum_arguments ||
			std::ranges::any_of(arguments,
								[](const std::string& argument)
								{
									return !sdk::validate_strong_id(argument);
								}))
			return invalid("effective_argv", "shape");
		if (!std::ranges::equal(requested_descriptors, exact_descriptor_ids) ||
			!std::ranges::equal(dependency_groups, exact_dependency_groups))
			return invalid("task-topology", "exact-set-or-order");
		if (sandbox.minimum != "enforced" && sandbox.minimum != "certified")
			return invalid("sandbox.minimum", "closed-enum");
		for (const auto digest : {
				 std::string_view{toolchain_digest},
				 std::string_view{normalized_invocation_digest},
				 std::string_view{environment_digest},
				 std::string_view{source_content_digest},
				 std::string_view{toolchain.builtin_headers_digest},
				 std::string_view{toolchain.abi_digest},
				 std::string_view{toolchain.plugin_spec_digest},
				 std::string_view{variant_authority.predefined_macros_digest},
				 std::string_view{variant_authority.include_search_digest},
				 std::string_view{variant_authority.semantic_flags_digest},
				 std::string_view{sandbox.policy_digest},
			 })
			if (!canonical_digest(digest))
				return invalid("digest", "grammar");
		for (const auto value : {
				 std::string_view{toolchain.family},
				 std::string_view{toolchain.exact_version},
				 std::string_view{toolchain.target_triple},
				 std::string_view{variant_authority.language},
				 std::string_view{variant_authority.language_standard},
				 std::string_view{variant_authority.target_triple},
			 })
			if (auto valid = sdk::validate_strong_id(value); !valid)
				return invalid("task-authority", valid.error().detail);
		if (toolchain.sysroot && !schema_logical_path(*toolchain.sysroot))
			return invalid("toolchain.sysroot", "logical-path");
		if (language != variant_authority.language ||
			toolchain.target_triple != variant_authority.target_triple)
			return invalid("task-authority", "language-or-target-drift");
		if (auto valid = budget.validate(); !valid)
			return invalid("budget", valid.error().detail);
		constexpr auto signed_max =
			static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
		for (const auto value : {
				 budget.output_bytes,
				 budget.rows,
				 budget.diagnostics,
				 budget.wall_ms,
				 budget.cpu_ms,
				 budget.address_space_bytes,
				 budget.transport_bytes,
				 budget.open_files,
				 budget.subprocesses,
			 })
			if (value > signed_max)
				return invalid("budget", "signed-int64-overflow");
		if (source_content_digest != sdk::content_digest(std::as_bytes(std::span{source})))
			return invalid("source.content_digest", "mismatch");
		auto decoded_source = base64_decode(source_content_base64);
		if (!decoded_source || *decoded_source != source)
			return invalid("source.content_base64", "decoded-bytes-mismatch");
		auto invocation_digest = semantic_digest_projection(
			"cxxlens.clang22.effective-invocation.v1",
			sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_string("cxxlens.clang22.effective-invocation.v1"),
				sdk::canonical_value::from_string(working_directory),
				string_tuple(arguments),
			}));
		if (!invocation_digest || normalized_invocation_digest != *invocation_digest)
			return invalid("normalized_invocation_digest", "mismatch");
		if (auto valid = validate_base_identity_bindings(*this); !valid)
			return valid;
		const auto selected = std::ranges::find(project_catalog.compile_units,
												selected_catalog_compile_unit,
												&sdk::catalog_compile_unit::compile_unit_id);
		if (selected == project_catalog.compile_units.end())
			return invalid("selected_catalog_compile_unit_id", "missing");
		if (selected->effective_invocation_digest != normalized_invocation_digest ||
			selected->source_digest != source_content_digest ||
			selected->environment_digest != environment_digest)
			return invalid("selected_catalog_compile_unit_id", "entry-payload-mismatch");
		return {};
	}

	sdk::result<std::vector<std::byte>> encode_task_input(const clang22_task_input& input)
	{
		if (auto valid = input.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		auto encoded = sdk::canonical_binary(task_v3_projection(input));
		if (!encoded)
			return sdk::unexpected(provider_error(
				"provider.frontend-request-invalid", "payload", encoded.error().detail));
		return encoded;
	}

	sdk::result<clang22_task_input> decode_task_input(const std::span<const std::byte> input)
	{
		auto decoded = sdk::canonical_binary_decode(input);
		if (!decoded || decoded->type != sdk::canonical_value::kind::ordered_tuple ||
			decoded->tuple.size() != 5U ||
			decoded->tuple[0U].type != sdk::canonical_value::kind::utf8_string ||
			decoded->tuple[0U].text != task_magic ||
			decoded->tuple[2U].type != sdk::canonical_value::kind::utf8_string ||
			decoded->tuple[3U].type != sdk::canonical_value::kind::utf8_string)
			return sdk::unexpected(provider_error("provider.frontend-request-invalid", "payload"));
		static constexpr std::array<std::string_view, 5U> catalog_keys{
			"catalog_compile_units",
			"catalog_digest",
			"catalog_environment_digest",
			"catalog_id",
			"logical_root",
		};
		static constexpr std::array<std::string_view, 4U> catalog_unit_keys{
			"catalog_compile_unit_id",
			"effective_invocation_digest",
			"environment_digest",
			"source_digest",
		};
		static constexpr std::array<std::string_view, 21U> task_keys{
			"budget",
			"build_variant_id",
			"catalog_digest",
			"catalog_id",
			"condition_id",
			"condition_universe_id",
			"dependency_groups",
			"effective_argv",
			"environment_digest",
			"interpretation_domain",
			"language",
			"normalized_invocation_digest",
			"project_id",
			"requested_descriptor_ids",
			"sandbox",
			"source",
			"toolchain",
			"toolchain_context_id",
			"toolchain_digest",
			"variant",
			"working_directory",
		};
		static constexpr std::array<std::string_view, 9U> source_keys{
			"content_base64",
			"content_digest",
			"encoding",
			"file_id",
			"line_index_id",
			"logical_path",
			"read_only",
			"size_bytes",
			"source_snapshot_id",
		};
		static constexpr std::array<std::string_view, 7U> toolchain_keys{
			"abi_digest",
			"builtin_headers_digest",
			"exact_version",
			"family",
			"plugin_spec_digest",
			"sysroot",
			"target_triple",
		};
		static constexpr std::array<std::string_view, 6U> variant_keys{
			"include_search_digest",
			"language",
			"language_standard",
			"predefined_macros_digest",
			"semantic_flags_digest",
			"target_triple",
		};
		static constexpr std::array<std::string_view, 9U> budget_keys{
			"address_space_bytes",
			"cpu_ms",
			"diagnostics",
			"open_files",
			"output_bytes",
			"rows",
			"subprocesses",
			"transport_bytes",
			"wall_ms",
		};
		static constexpr std::array<std::string_view, 2U> sandbox_keys{"minimum", "policy_digest"};

		auto catalog_fields = parse_object(decoded->tuple[1U], catalog_keys, "project_catalog");
		auto task_fields = parse_object(decoded->tuple[4U], task_keys, "task");
		if (!catalog_fields || !task_fields)
			return sdk::unexpected(!catalog_fields ? std::move(catalog_fields.error())
												   : std::move(task_fields.error()));
		const auto text = [](const canonical_fields& fields,
							 const std::string_view key) -> sdk::result<std::string>
		{
			return parse_string(*fields.at(key), key);
		};
		auto catalog_id = text(*catalog_fields, "catalog_id");
		auto catalog_digest_value = text(*catalog_fields, "catalog_digest");
		auto logical_root = text(*catalog_fields, "logical_root");
		auto catalog_environment = text(*catalog_fields, "catalog_environment_digest");
		if (!catalog_id || !catalog_digest_value || !logical_root || !catalog_environment)
			return sdk::unexpected(provider_error(
				"provider.frontend-request-invalid", "project_catalog", "string-field"));
		const auto& unit_values = *catalog_fields->at("catalog_compile_units");
		if (unit_values.type != sdk::canonical_value::kind::ordered_tuple ||
			unit_values.tuple.empty() || unit_values.tuple.size() > 4096U)
			return sdk::unexpected(provider_error(
				"provider.frontend-request-invalid", "catalog_compile_units", "shape"));
		std::vector<sdk::catalog_compile_unit> units;
		units.reserve(unit_values.tuple.size());
		for (const auto& value : unit_values.tuple)
		{
			auto fields = parse_object(value, catalog_unit_keys, "catalog_compile_unit");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto id = text(*fields, "catalog_compile_unit_id");
			auto invocation = text(*fields, "effective_invocation_digest");
			auto source_digest_value = text(*fields, "source_digest");
			auto environment = text(*fields, "environment_digest");
			if (!id || !invocation || !source_digest_value || !environment)
				return sdk::unexpected(provider_error(
					"provider.frontend-request-invalid", "catalog_compile_unit", "field"));
			units.push_back({std::move(*id),
							 std::move(*invocation),
							 std::move(*source_digest_value),
							 std::move(*environment)});
		}
		if (!std::ranges::is_sorted(units, {}, &sdk::catalog_compile_unit::compile_unit_id))
			return sdk::unexpected(provider_error(
				"provider.frontend-request-invalid", "catalog_compile_units", "canonical-order"));
		auto catalog = sdk::project_catalog::make(
			std::move(*logical_root), std::move(*catalog_environment), std::move(units));
		if (!catalog || catalog->catalog_id != *catalog_id ||
			catalog->catalog_digest != *catalog_digest_value)
			return sdk::unexpected(provider_error(
				"provider.frontend-request-invalid", "project_catalog", "identity-mismatch"));

		auto source_fields = parse_object(*task_fields->at("source"), source_keys, "source");
		auto toolchain_fields_value =
			parse_object(*task_fields->at("toolchain"), toolchain_keys, "toolchain");
		auto variant_fields_value =
			parse_object(*task_fields->at("variant"), variant_keys, "variant");
		auto budget_fields_value = parse_object(*task_fields->at("budget"), budget_keys, "budget");
		auto sandbox_fields_value =
			parse_object(*task_fields->at("sandbox"), sandbox_keys, "sandbox");
		if (!source_fields || !toolchain_fields_value || !variant_fields_value ||
			!budget_fields_value || !sandbox_fields_value)
			return sdk::unexpected(
				provider_error("provider.frontend-request-invalid", "task", "nested-object"));

		clang22_task_input output;
		output.project_catalog = std::move(*catalog);
		output.selected_catalog_compile_unit = decoded->tuple[2U].text;
		output.compile_unit = decoded->tuple[3U].text;
		auto assign = [&](std::string& target,
						  const canonical_fields& fields,
						  const std::string_view key) -> bool
		{
			auto value = text(fields, key);
			if (!value)
				return false;
			target = std::move(*value);
			return true;
		};
		if (!assign(output.project, *task_fields, "project_id") ||
			!assign(output.variant, *task_fields, "build_variant_id") ||
			!assign(output.toolchain_context, *task_fields, "toolchain_context_id") ||
			!assign(output.toolchain_digest, *task_fields, "toolchain_digest") ||
			!assign(output.normalized_invocation_digest,
					*task_fields,
					"normalized_invocation_digest") ||
			!assign(output.environment_digest, *task_fields, "environment_digest") ||
			!assign(output.language, *task_fields, "language") ||
			!assign(output.working_directory, *task_fields, "working_directory") ||
			!assign(output.condition_universe, *task_fields, "condition_universe_id") ||
			!assign(output.condition, *task_fields, "condition_id") ||
			!assign(output.interpretation, *task_fields, "interpretation_domain") ||
			!assign(output.source_snapshot, *source_fields, "source_snapshot_id") ||
			!assign(output.file, *source_fields, "file_id") ||
			!assign(output.logical_path, *source_fields, "logical_path") ||
			!assign(output.source_content_digest, *source_fields, "content_digest") ||
			!assign(output.source_encoding, *source_fields, "encoding") ||
			!assign(output.line_index, *source_fields, "line_index_id") ||
			!assign(output.toolchain.family, *toolchain_fields_value, "family") ||
			!assign(output.toolchain.exact_version, *toolchain_fields_value, "exact_version") ||
			!assign(output.toolchain.target_triple, *toolchain_fields_value, "target_triple") ||
			!assign(output.toolchain.builtin_headers_digest,
					*toolchain_fields_value,
					"builtin_headers_digest") ||
			!assign(output.toolchain.abi_digest, *toolchain_fields_value, "abi_digest") ||
			!assign(output.toolchain.plugin_spec_digest,
					*toolchain_fields_value,
					"plugin_spec_digest") ||
			!assign(output.variant_authority.language, *variant_fields_value, "language") ||
			!assign(output.variant_authority.language_standard,
					*variant_fields_value,
					"language_standard") ||
			!assign(
				output.variant_authority.target_triple, *variant_fields_value, "target_triple") ||
			!assign(output.variant_authority.predefined_macros_digest,
					*variant_fields_value,
					"predefined_macros_digest") ||
			!assign(output.variant_authority.include_search_digest,
					*variant_fields_value,
					"include_search_digest") ||
			!assign(output.variant_authority.semantic_flags_digest,
					*variant_fields_value,
					"semantic_flags_digest") ||
			!assign(output.sandbox.minimum, *sandbox_fields_value, "minimum") ||
			!assign(output.sandbox.policy_digest, *sandbox_fields_value, "policy_digest"))
			return sdk::unexpected(
				provider_error("provider.frontend-request-invalid", "task", "string-field"));
		const auto& sysroot = *toolchain_fields_value->at("sysroot");
		if (sysroot.type == sdk::canonical_value::kind::utf8_string)
			output.toolchain.sysroot = sysroot.text;
		else if (sysroot.type != sdk::canonical_value::kind::null_value)
			return sdk::unexpected(provider_error(
				"provider.frontend-request-invalid", "toolchain.sysroot", "nullable-string"));
		const auto& read_only = *source_fields->at("read_only");
		if (read_only.type != sdk::canonical_value::kind::boolean)
			return sdk::unexpected(
				provider_error("provider.frontend-request-invalid", "source.read_only", "boolean"));
		output.source_read_only = read_only.boolean;
		auto source_size =
			parse_nonnegative_integer(*source_fields->at("size_bytes"), "source.size_bytes");
		auto source_base64 = text(*source_fields, "content_base64");
		if (!source_size || !source_base64)
			return sdk::unexpected(
				provider_error("provider.frontend-request-invalid", "source", "content"));
		output.source_size_bytes = *source_size;
		output.source_content_base64 = *source_base64;
		auto source = base64_decode(*source_base64);
		if (!source)
			return sdk::unexpected(std::move(source.error()));
		output.source = std::move(*source);
		auto arguments = parse_string_tuple(*task_fields->at("effective_argv"), "effective_argv");
		auto descriptors = parse_string_tuple(*task_fields->at("requested_descriptor_ids"),
											  "requested_descriptor_ids");
		auto groups =
			parse_string_tuple(*task_fields->at("dependency_groups"), "dependency_groups");
		if (!arguments || !descriptors || !groups)
			return sdk::unexpected(
				provider_error("provider.frontend-request-invalid", "task", "string-array"));
		output.arguments = std::move(*arguments);
		output.requested_descriptors = std::move(*descriptors);
		output.dependency_groups = std::move(*groups);
		const auto budget_value = [&](const std::string_view key) -> sdk::result<std::uint64_t>
		{
			return parse_positive_integer(*budget_fields_value->at(key), key);
		};
		auto output_bytes = budget_value("output_bytes");
		auto rows = budget_value("rows");
		auto diagnostics = budget_value("diagnostics");
		auto wall = budget_value("wall_ms");
		auto cpu = budget_value("cpu_ms");
		auto address_space = budget_value("address_space_bytes");
		auto transport = budget_value("transport_bytes");
		auto open_files = budget_value("open_files");
		auto subprocesses = budget_value("subprocesses");
		if (!output_bytes || !rows || !diagnostics || !wall || !cpu || !address_space ||
			!transport || !open_files || !subprocesses)
			return sdk::unexpected(
				provider_error("provider.frontend-request-invalid", "budget", "field"));
		output.budget.output_bytes = *output_bytes;
		output.budget.rows = *rows;
		output.budget.diagnostics = *diagnostics;
		output.budget.wall_ms = *wall;
		output.budget.cpu_ms = *cpu;
		output.budget.address_space_bytes = *address_space;
		output.budget.transport_bytes = *transport;
		output.budget.open_files = *open_files;
		output.budget.subprocesses = *subprocesses;
		if (*task_fields->at("catalog_id") !=
				sdk::canonical_value::from_string(output.project_catalog.catalog_id) ||
			*task_fields->at("catalog_digest") !=
				sdk::canonical_value::from_string(output.project_catalog.catalog_digest))
			return sdk::unexpected(provider_error(
				"provider.frontend-request-invalid", "task.catalog", "global-mismatch"));
		if (auto valid = output.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		auto reencoded = encode_task_input(output);
		if (!reencoded || !std::ranges::equal(*reencoded, input))
			return sdk::unexpected(provider_error(
				"provider.frontend-request-invalid", "payload", "noncanonical-projection"));
		return output;
	}

	sdk::result<std::string> provider_condition_ref_id(const clang22_task_input& input)
	{
		if (auto valid = input.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		auto digest = semantic_digest_projection(
			"cxxlens.clang22.condition-ref.v1",
			sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_string("cxxlens.clang22.condition-ref.v1"),
				sdk::canonical_value::from_string(input.condition_universe),
				sdk::canonical_value::from_string(input.condition),
			}));
		if (!digest)
			return sdk::unexpected(std::move(digest.error()));
		return "condition-ref:" + *digest;
	}

	sdk::result<sdk::provider::task>
	reconstruct_provider_task(const clang22_task_input& input,
							  std::vector<sdk::relation_descriptor> exact_outputs,
							  std::string provider_semantic_contract_digest)
	{
		if (auto valid = input.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (!canonical_digest(provider_semantic_contract_digest))
			return sdk::unexpected(provider_error("provider.frontend-request-invalid",
												  "provider_semantic_contract_digest",
												  "digest"));
		std::ranges::sort(
			exact_outputs,
			[](const sdk::relation_descriptor& left, const sdk::relation_descriptor& right)
			{
				return std::tie(left.id, left.descriptor_digest) <
					std::tie(right.id, right.descriptor_digest);
			});
		if (exact_outputs.size() != input.requested_descriptors.size())
			return sdk::unexpected(provider_error(
				"provider.frontend-request-invalid", "requested_descriptor_ids", "count"));
		for (std::size_t index{}; index < exact_outputs.size(); ++index)
			if (exact_outputs[index].id != input.requested_descriptors[index])
				return sdk::unexpected(provider_error(
					"provider.frontend-request-invalid", "requested_descriptor_ids", "binding"));
		auto condition_ref = provider_condition_ref_id(input);
		if (!condition_ref)
			return sdk::unexpected(std::move(condition_ref.error()));
		auto session = sdk::provider::provider_session{
			std::string{provider_id},
			provider_version,
			std::move(provider_semantic_contract_digest),
			exact_outputs,
			{},
			{"cc.clang22-canonical-1"},
			"observation",
			"assertion",
		};
		return sdk::provider::task::make(std::move(session),
										 input.project_catalog,
										 std::move(exact_outputs),
										 std::move(*condition_ref),
										 input.interpretation,
										 input.dependency_groups);
	}
} // namespace cxxlens::detail::clang22
