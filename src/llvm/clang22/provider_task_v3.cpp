#include "provider_task_v3.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <new>
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
				if ((padding_two && ((*second & 0x0fU) != 0U)) ||
					(padding_one && !padding_two && ((*third & 0x03U) != 0U)))
					return sdk::unexpected(provider_error("provider.frontend-request-invalid",
														  "source.content_base64",
														  "nonzero-padding-bits"));
				const auto word = (*first << 18U) | (*second << 12U) | (*third << 6U) | *fourth;
				output.push_back(static_cast<char>((word >> 16U) & 0xffU));
				if (!padding_two)
					output.push_back(static_cast<char>((word >> 8U) & 0xffU));
				if (!padding_one)
					output.push_back(static_cast<char>(word & 0xffU));
			}
			return output;
		}

		[[nodiscard]] sdk::canonical_value catalog_projection(const sdk::project_catalog& catalog)
		{
			std::vector<sdk::canonical_value> entries;
			entries.reserve(catalog.compile_units.size());
			for (const auto& unit : catalog.compile_units)
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
				{"catalog_id", sdk::canonical_value::from_string(catalog.catalog_id)},
				{"catalog_digest", sdk::canonical_value::from_string(catalog.catalog_digest)},
				{"logical_root", sdk::canonical_value::from_string(catalog.logical_root)},
				{"catalog_environment_digest",
				 sdk::canonical_value::from_string(catalog.environment_digest)},
				{"catalog_compile_units", sdk::canonical_value::from_tuple(std::move(entries))},
			});
		}

		[[nodiscard]] sdk::canonical_value
		task_payload_projection(const clang22_task_input& input,
								const sdk::project_catalog& catalog)
		{
			const auto integer = [](const std::uint64_t value)
			{
				return sdk::canonical_value::from_integer(static_cast<std::int64_t>(value));
			};
			return canonical_object({
				{"project_id", sdk::canonical_value::from_string(input.project)},
				{"catalog_id", sdk::canonical_value::from_string(catalog.catalog_id)},
				{"catalog_digest", sdk::canonical_value::from_string(catalog.catalog_digest)},
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

		[[nodiscard]] sdk::canonical_value task_v3_projection(const clang22_task_input& input,
															  const sdk::project_catalog& catalog)
		{
			return sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_string(std::string{task_magic}),
				catalog_projection(catalog),
				sdk::canonical_value::from_string(input.selected_catalog_compile_unit),
				sdk::canonical_value::from_string(input.compile_unit),
				task_payload_projection(input, catalog),
			});
		}

		[[nodiscard]] sdk::canonical_value task_v3_projection(const clang22_task_input& input)
		{
			return task_v3_projection(input, input.project_catalog);
		}

		[[nodiscard]] sdk::canonical_value* object_member(sdk::canonical_value& object,
														  const std::string_view key)
		{
			if (object.type != sdk::canonical_value::kind::ordered_tuple)
				return nullptr;
			for (auto& entry : object.tuple)
				if (entry.type == sdk::canonical_value::kind::ordered_tuple &&
					entry.tuple.size() == 2U &&
					entry.tuple[0U].type == sdk::canonical_value::kind::utf8_string &&
					entry.tuple[0U].text == key)
					return &entry.tuple[1U];
			return nullptr;
		}

		[[nodiscard]] sdk::result<std::uint64_t>
		canonical_stream_size(const sdk::canonical_value& value,
							  const sdk::canonical_value* external_string,
							  const std::uint64_t external_string_bytes)
		{
			constexpr std::uint64_t tag_bytes = 1U;
			constexpr std::uint64_t length_bytes = 8U;
			const auto checked_add = [](const std::uint64_t left,
										const std::uint64_t right) -> sdk::result<std::uint64_t>
			{
				if (right > std::numeric_limits<std::uint64_t>::max() - left)
					return sdk::unexpected(provider_error(
						"provider.frontend-request-invalid", "task.v3", "length-overflow"));
				return left + right;
			};
			using kind = sdk::canonical_value::kind;
			switch (value.type)
			{
				case kind::null_value:
					return tag_bytes;
				case kind::boolean:
					return tag_bytes + 1U;
				case kind::signed_integer:
				{
					const auto magnitude = value.integer < 0
						? static_cast<std::uint64_t>(-(value.integer + 1)) + 1U
						: static_cast<std::uint64_t>(value.integer);
					std::uint64_t width = 1U;
					for (auto remaining = magnitude; remaining > 0xffU; remaining >>= 8U)
						++width;
					return tag_bytes + 1U + length_bytes + width;
				}
				case kind::bytes:
					return tag_bytes + length_bytes + value.byte_string.size();
				case kind::utf8_string:
					return tag_bytes + length_bytes +
						(&value == external_string ? external_string_bytes : value.text.size());
				case kind::ordered_tuple:
				{
					std::uint64_t total = tag_bytes + length_bytes;
					for (const auto& child : value.tuple)
					{
						auto child_size =
							canonical_stream_size(child, external_string, external_string_bytes);
						if (!child_size)
							return sdk::unexpected(std::move(child_size.error()));
						auto framed = checked_add(length_bytes, *child_size);
						if (!framed)
							return sdk::unexpected(std::move(framed.error()));
						auto next = checked_add(total, *framed);
						if (!next)
							return sdk::unexpected(std::move(next.error()));
						total = *next;
					}
					return total;
				}
			}
			return sdk::unexpected(
				provider_error("provider.frontend-request-invalid", "task.v3", "unknown-kind"));
		}

		[[nodiscard]] sdk::result<void> append_stream(clang22_task_input_sink& output,
													  const std::span<const std::byte> bytes)
		{
			if (auto appended = output.append(bytes); !appended)
				return sdk::unexpected(std::move(appended.error()));
			return {};
		}

		[[nodiscard]] sdk::result<void> append_octet(clang22_task_input_sink& output,
													 const std::uint8_t value)
		{
			const std::array byte{static_cast<std::byte>(value)};
			return append_stream(output, byte);
		}

		[[nodiscard]] sdk::result<void> append_length(clang22_task_input_sink& output,
													  const std::uint64_t value)
		{
			std::array<std::byte, 8U> bytes{};
			for (std::size_t index{}; index < bytes.size(); ++index)
				bytes[index] = static_cast<std::byte>(
					(value >> (56U - static_cast<unsigned>(index * 8U))) & 0xffU);
			return append_stream(output, bytes);
		}

		[[nodiscard]] sdk::result<void> stream_canonical_base64(clang22_task_source_replay& source,
																clang22_task_input_sink& output)
		{
			constexpr std::string_view alphabet{
				"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
			std::array<std::byte, 64U * 1024U> input{};
			std::array<std::byte, 64U * 1024U> encoded{};
			std::array<std::uint8_t, 3U> pending{};
			std::size_t pending_size{};
			std::size_t encoded_size{};
			const auto emit = [&](const char value) -> sdk::result<void>
			{
				encoded[encoded_size++] = static_cast<std::byte>(static_cast<unsigned char>(value));
				if (encoded_size == encoded.size())
				{
					if (auto appended = append_stream(output, encoded); !appended)
						return appended;
					encoded_size = 0U;
				}
				return {};
			};
			const auto emit_group = [&](const std::array<std::uint8_t, 3U>& group,
										const std::size_t size) -> sdk::result<void>
			{
				const auto word = (static_cast<std::uint32_t>(group[0U]) << 16U) |
					(static_cast<std::uint32_t>(group[1U]) << 8U) |
					static_cast<std::uint32_t>(group[2U]);
				for (const auto character : {
						 alphabet[(word >> 18U) & 0x3fU],
						 alphabet[(word >> 12U) & 0x3fU],
						 size >= 2U ? alphabet[(word >> 6U) & 0x3fU] : '=',
						 size == 3U ? alphabet[word & 0x3fU] : '=',
					 })
					if (auto emitted = emit(character); !emitted)
						return emitted;
				return {};
			};

			std::uint64_t offset{};
			while (offset < source.size_bytes())
			{
				auto read = source.read_at(offset, input);
				if (!read)
					return sdk::unexpected(std::move(read.error()));
				if (*read == 0U || *read > input.size())
					return sdk::unexpected(provider_error(
						"provider.frontend-request-invalid", "source", "truncated-replay"));
				for (const auto byte : std::span{input}.first(*read))
				{
					pending[pending_size++] = std::to_integer<std::uint8_t>(byte);
					if (pending_size == pending.size())
					{
						if (auto emitted = emit_group(pending, pending.size()); !emitted)
							return emitted;
						pending = {};
						pending_size = 0U;
					}
				}
				offset += static_cast<std::uint64_t>(*read);
			}
			if (pending_size != 0U)
				if (auto emitted = emit_group(pending, pending_size); !emitted)
					return emitted;
			if (encoded_size != 0U)
				return append_stream(output, std::span{encoded}.first(encoded_size));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		stream_canonical_value(const sdk::canonical_value& value,
							   const sdk::canonical_value* external_string,
							   const std::uint64_t external_string_bytes,
							   clang22_task_source_replay& source,
							   clang22_task_input_sink& output)
		{
			using kind = sdk::canonical_value::kind;
			const auto tag = static_cast<std::uint8_t>(value.type);
			if (auto appended = append_octet(output, tag); !appended)
				return appended;
			switch (value.type)
			{
				case kind::null_value:
					return {};
				case kind::boolean:
					return append_octet(output, value.boolean ? 1U : 0U);
				case kind::signed_integer:
				{
					if (auto sign = append_octet(output, value.integer < 0 ? 1U : 0U); !sign)
						return sign;
					const auto magnitude = value.integer < 0
						? static_cast<std::uint64_t>(-(value.integer + 1)) + 1U
						: static_cast<std::uint64_t>(value.integer);
					std::uint64_t width = 1U;
					for (auto remaining = magnitude; remaining > 0xffU; remaining >>= 8U)
						++width;
					if (auto length = append_length(output, width); !length)
						return length;
					std::array<std::byte, 8U> bytes{};
					for (std::uint64_t index{}; index < width; ++index)
						bytes[static_cast<std::size_t>(index)] = static_cast<std::byte>(
							(magnitude >> ((width - index - 1U) * 8U)) & 0xffU);
					return append_stream(output,
										 std::span{bytes}.first(static_cast<std::size_t>(width)));
				}
				case kind::bytes:
					if (auto length = append_length(output, value.byte_string.size()); !length)
						return length;
					return append_stream(output, value.byte_string);
				case kind::utf8_string:
				{
					const auto length =
						&value == external_string ? external_string_bytes : value.text.size();
					if (auto framed = append_length(output, length); !framed)
						return framed;
					if (&value == external_string)
						return stream_canonical_base64(source, output);
					return append_stream(output, std::as_bytes(std::span{value.text}));
				}
				case kind::ordered_tuple:
					if (auto count = append_length(output, value.tuple.size()); !count)
						return count;
					for (const auto& child : value.tuple)
					{
						auto size =
							canonical_stream_size(child, external_string, external_string_bytes);
						if (!size)
							return sdk::unexpected(std::move(size.error()));
						if (auto framed = append_length(output, *size); !framed)
							return framed;
						if (auto streamed = stream_canonical_value(
								child, external_string, external_string_bytes, source, output);
							!streamed)
							return streamed;
					}
					return {};
			}
			return sdk::unexpected(
				provider_error("provider.frontend-request-invalid", "task.v3", "unknown-kind"));
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
		validate_base_identity_bindings(const clang22_task_input& input,
										const sdk::project_catalog& catalog,
										const clang22_task_source_receipt* source_receipt)
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

			if (source_receipt != nullptr)
			{
				if (source_receipt->line_index_id != input.line_index)
					return mismatch("source.line_index_id");
			}
			else
			{
				std::vector<sdk::canonical_value> offset_values{
					sdk::canonical_value::from_integer(0)};
				for (std::size_t index{}; index < input.source.size(); ++index)
					if (input.source[index] == '\n')
						offset_values.push_back(sdk::canonical_value::from_integer(
							static_cast<std::int64_t>(index + 1U)));
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
			}

			using project_relation = build::relations::project;
			project_relation::builder project_builder;
			for (auto result : {
					 project_builder.set<project_relation::project_column>(
						 sdk::detached_cell::typed("project_id", "pending")),
					 project_builder.set<project_relation::catalog>(
						 sdk::detached_cell::typed("catalog_id", catalog.catalog_id)),
					 project_builder.set<project_relation::catalog_digest>(
						 symbol_cell(sdk::scalar_kind::digest, {}, catalog.catalog_digest)),
					 project_builder.set<project_relation::logical_root>(
						 sdk::detached_cell::typed("logical_path_id", catalog.logical_root)),
					 project_builder.set<project_relation::environment_digest>(
						 symbol_cell(sdk::scalar_kind::digest, {}, catalog.environment_digest)),
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

	sdk::result<void>
	clang22_task_input::validate_impl(const sdk::project_catalog& catalog,
									  const clang22_task_source_receipt* source_receipt) const
	{
		const auto invalid = [](const std::string_view field, std::string detail = {})
		{
			return sdk::unexpected(provider_error(
				"provider.frontend-request-invalid", std::string{field}, std::move(detail)));
		};
		if (auto valid = catalog.validate(); !valid)
			return invalid("project_catalog", valid.error().code);
		if (catalog.compile_units.empty() || catalog.compile_units.size() > 4096U)
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
		if (!schema_logical_path(catalog.logical_root) || !schema_logical_path(working_directory) ||
			logical_path.empty() || !logical_path.starts_with("project://") ||
			source_size_bytes > maximum_source_bytes ||
			(source_receipt == nullptr && source_size_bytes != source.size()))
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
		if (source_receipt == nullptr)
		{
			if (source_content_digest != sdk::content_digest(std::as_bytes(std::span{source})))
				return invalid("source.content_digest", "mismatch");
			auto decoded_source = base64_decode(source_content_base64);
			if (!decoded_source || *decoded_source != source)
				return invalid("source.content_base64", "decoded-bytes-mismatch");
		}
		else if (!source.empty() || !source_content_base64.empty() ||
				 source_receipt->size_bytes != source_size_bytes ||
				 source_receipt->content_digest != source_content_digest ||
				 source_receipt->line_index_id != line_index)
			return invalid("source", "sealed-receipt-mismatch");
		auto invocation_digest = semantic_digest_projection(
			"cxxlens.clang22.effective-invocation.v1",
			sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_string("cxxlens.clang22.effective-invocation.v1"),
				sdk::canonical_value::from_string(working_directory),
				string_tuple(arguments),
			}));
		if (!invocation_digest || normalized_invocation_digest != *invocation_digest)
			return invalid("normalized_invocation_digest", "mismatch");
		if (auto valid = validate_base_identity_bindings(*this, catalog, source_receipt); !valid)
			return valid;
		const auto selected = std::ranges::find(catalog.compile_units,
												selected_catalog_compile_unit,
												&sdk::catalog_compile_unit::compile_unit_id);
		if (selected == catalog.compile_units.end())
			return invalid("selected_catalog_compile_unit_id", "missing");
		if (selected->effective_invocation_digest != normalized_invocation_digest ||
			selected->source_digest != source_content_digest ||
			selected->environment_digest != environment_digest)
			return invalid("selected_catalog_compile_unit_id", "entry-payload-mismatch");
		return {};
	}

	sdk::result<void> clang22_task_input::validate() const
	{
		return validate_impl(project_catalog, nullptr);
	}

	sdk::result<void> clang22_task_input::validate_with_source_receipt(
		const clang22_task_source_receipt& source_receipt) const
	{
		return validate_impl(project_catalog, &source_receipt);
	}

	sdk::result<void> clang22_task_input::validate_with_catalog(
		const sdk::project_catalog& catalog,
		const clang22_task_source_receipt& source_receipt) const
	{
		return validate_impl(catalog, &source_receipt);
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

	sdk::result<clang22_task_v3_stream_receipt>
	encode_task_input_streaming(const clang22_task_input& input,
								const sdk::project_catalog& catalog,
								clang22_task_source_replay& source,
								clang22_task_input_sink& output)
	{
		if (!source.sealed())
			return sdk::unexpected(
				provider_error("provider.frontend-request-invalid", "source", "unsealed-replay"));
		if (auto valid = input.validate_with_catalog(catalog, source.receipt()); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (source.size_bytes() != source.receipt().size_bytes ||
			source.size_bytes() != input.source_size_bytes ||
			source.size_bytes() > maximum_source_bytes)
			return sdk::unexpected(provider_error(
				"provider.frontend-request-invalid", "source", "replay-size-mismatch"));

		const auto full_groups = source.size_bytes() / 3U;
		const auto remainder = source.size_bytes() % 3U;
		if (full_groups >
			(std::numeric_limits<std::uint64_t>::max() / 4U) - (remainder == 0U ? 0U : 1U))
			return sdk::unexpected(provider_error(
				"provider.frontend-request-invalid", "source", "base64-length-overflow"));
		const auto base64_bytes = (full_groups + (remainder == 0U ? 0U : 1U)) * 4U;

		auto projection = task_v3_projection(input, catalog);
		if (projection.tuple.size() != 5U)
			return sdk::unexpected(
				provider_error("provider.frontend-request-invalid", "task.v3", "projection-shape"));
		auto* source_object = object_member(projection.tuple[4U], "source");
		auto* content =
			source_object != nullptr ? object_member(*source_object, "content_base64") : nullptr;
		if (content == nullptr || content->type != sdk::canonical_value::kind::utf8_string ||
			!content->text.empty())
			return sdk::unexpected(
				provider_error("provider.frontend-request-invalid", "task.v3", "source-marker"));

		auto task_bytes = canonical_stream_size(projection, content, base64_bytes);
		if (!task_bytes)
			return sdk::unexpected(std::move(task_bytes.error()));
		constexpr std::uint64_t maximum_task_input_bytes = 64U * 1024U * 1024U;
		if (*task_bytes > maximum_task_input_bytes)
			return sdk::unexpected(
				provider_error("provider.frontend-request-invalid", "task.v3", "maximum-bytes"));
		if (auto streamed =
				stream_canonical_value(projection, content, base64_bytes, source, output);
			!streamed)
			return sdk::unexpected(std::move(streamed.error()));
		return clang22_task_v3_stream_receipt{source.size_bytes(), base64_bytes, *task_bytes};
	}

	sdk::result<clang22_task_v3_stream_receipt>
	encode_task_input_streaming(const clang22_task_input& input,
								clang22_task_source_replay& source,
								clang22_task_input_sink& output)
	{
		return encode_task_input_streaming(input, input.project_catalog, source, output);
	}

	namespace
	{
		constexpr std::uint64_t maximum_task_input_bytes = 64U * 1024U * 1024U;
		constexpr std::uint64_t maximum_source_base64_bytes =
			((maximum_source_bytes + 2U) / 3U) * 4U;
		constexpr std::size_t maximum_canonical_depth = 64U;
		constexpr std::array<std::size_t, 5U> source_base64_value_path{4U, 15U, 1U, 0U, 1U};

		class streaming_task_v3_decoder
		{
		  public:
			streaming_task_v3_decoder(clang22_task_input_replay& input,
									  clang22_task_source_spool& source)
				: input_{input}, source_{source}
			{
			}

			[[nodiscard]] sdk::result<sdk::canonical_value> decode()
			{
				if (!input_.sealed())
					return failure("task.v3", "unsealed-replay");
				if (input_.size_bytes() == 0U || input_.size_bytes() > maximum_task_input_bytes)
					return failure("task.v3", "maximum-bytes");
				if (source_.sealed() || source_.size_bytes() != 0U)
					return failure("source", "spool-not-empty");
				auto value = decode_value(input_.size_bytes(), 0U);
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				if (offset_ != input_.size_bytes() || !source_seen_)
					return failure("task.v3", "source-or-trailing-bytes");
				if (auto valid = value->validate(); !valid)
					return failure("task.v3", valid.error().detail);
				return value;
			}

			[[nodiscard]] std::uint64_t canonical_base64_bytes() const noexcept
			{
				return base64_bytes_;
			}

		  private:
			[[nodiscard]] static sdk::result<sdk::canonical_value>
			failure(const std::string_view field, const std::string_view detail)
			{
				return sdk::unexpected(provider_error(
					"provider.frontend-request-invalid", std::string{field}, std::string{detail}));
			}

			[[nodiscard]] sdk::result<void> read_exact(const std::span<std::byte> destination)
			{
				if (destination.size() > input_.size_bytes() - offset_)
					return sdk::unexpected(provider_error(
						"provider.frontend-request-invalid", "task.v3", "truncated-replay"));
				std::size_t consumed{};
				while (consumed < destination.size())
				{
					auto read = input_.read_at(offset_, destination.subspan(consumed));
					if (!read)
						return sdk::unexpected(std::move(read.error()));
					if (*read == 0U || *read > destination.size() - consumed)
						return sdk::unexpected(provider_error(
							"provider.frontend-request-invalid", "task.v3", "truncated-replay"));
					consumed += *read;
					offset_ += static_cast<std::uint64_t>(*read);
				}
				return {};
			}

			[[nodiscard]] sdk::result<std::uint8_t> read_octet()
			{
				std::array<std::byte, 1U> value{};
				if (auto read = read_exact(value); !read)
					return sdk::unexpected(std::move(read.error()));
				return std::to_integer<std::uint8_t>(value.front());
			}

			[[nodiscard]] sdk::result<std::uint64_t> read_length()
			{
				std::array<std::byte, 8U> bytes{};
				if (auto read = read_exact(bytes); !read)
					return sdk::unexpected(std::move(read.error()));
				std::uint64_t value{};
				for (const auto byte : bytes)
					value = (value << 8U) | std::to_integer<std::uint8_t>(byte);
				return value;
			}

			[[nodiscard]] bool external_source_value() const noexcept
			{
				return path_size_ == source_base64_value_path.size() &&
					std::ranges::equal(std::span{path_}.first(path_size_),
									   source_base64_value_path);
			}

			[[nodiscard]] sdk::result<void> decode_base64(const std::uint64_t length)
			{
				if (source_seen_ || length > maximum_source_base64_bytes || length % 4U != 0U)
					return sdk::unexpected(provider_error(
						"provider.frontend-request-invalid", "source.content_base64", "shape"));
				source_seen_ = true;
				base64_bytes_ = length;
				const auto decode = [](const std::uint8_t value) -> std::optional<std::uint32_t>
				{
					if (value >= static_cast<std::uint8_t>('A') &&
						value <= static_cast<std::uint8_t>('Z'))
						return value - static_cast<std::uint8_t>('A');
					if (value >= static_cast<std::uint8_t>('a') &&
						value <= static_cast<std::uint8_t>('z'))
						return value - static_cast<std::uint8_t>('a') + 26U;
					if (value >= static_cast<std::uint8_t>('0') &&
						value <= static_cast<std::uint8_t>('9'))
						return value - static_cast<std::uint8_t>('0') + 52U;
					if (value == static_cast<std::uint8_t>('+'))
						return 62U;
					if (value == static_cast<std::uint8_t>('/'))
						return 63U;
					return std::nullopt;
				};

				std::array<std::byte, 64U * 1024U> encoded{};
				std::array<std::byte, 48U * 1024U> decoded{};
				std::uint64_t consumed{};
				std::uint64_t decoded_total{};
				while (consumed < length)
				{
					const auto count = static_cast<std::size_t>(
						std::min<std::uint64_t>(encoded.size(), length - consumed));
					if (auto read = read_exact(std::span{encoded}.first(count)); !read)
						return read;
					std::size_t decoded_size{};
					for (std::size_t index{}; index < count; index += 4U)
					{
						const bool final = consumed + index + 4U == length;
						const auto third_byte = std::to_integer<std::uint8_t>(encoded[index + 2U]);
						const auto fourth_byte = std::to_integer<std::uint8_t>(encoded[index + 3U]);
						const bool padding_two = third_byte == static_cast<std::uint8_t>('=');
						const bool padding_one = fourth_byte == static_cast<std::uint8_t>('=');
						if ((!final && (padding_two || padding_one)) ||
							(padding_two && !padding_one))
							return sdk::unexpected(
								provider_error("provider.frontend-request-invalid",
											   "source.content_base64",
											   "padding"));
						auto first = decode(std::to_integer<std::uint8_t>(encoded[index]));
						auto second = decode(std::to_integer<std::uint8_t>(encoded[index + 1U]));
						auto third =
							padding_two ? std::optional<std::uint32_t>{0U} : decode(third_byte);
						auto fourth =
							padding_one ? std::optional<std::uint32_t>{0U} : decode(fourth_byte);
						if (!first || !second || !third || !fourth)
							return sdk::unexpected(
								provider_error("provider.frontend-request-invalid",
											   "source.content_base64",
											   "alphabet"));
						if ((padding_two && ((*second & 0x0fU) != 0U)) ||
							(padding_one && !padding_two && ((*third & 0x03U) != 0U)))
							return sdk::unexpected(
								provider_error("provider.frontend-request-invalid",
											   "source.content_base64",
											   "nonzero-padding-bits"));
						const auto word =
							(*first << 18U) | (*second << 12U) | (*third << 6U) | *fourth;
						decoded[decoded_size++] = static_cast<std::byte>((word >> 16U) & 0xffU);
						if (!padding_two)
							decoded[decoded_size++] = static_cast<std::byte>((word >> 8U) & 0xffU);
						if (!padding_one)
							decoded[decoded_size++] = static_cast<std::byte>(word & 0xffU);
					}
					if (decoded_size > maximum_source_bytes - decoded_total)
						return sdk::unexpected(provider_error(
							"provider.frontend-request-invalid", "source", "maximum-bytes"));
					if (decoded_size != 0U)
						if (auto appended = source_.append(std::span{decoded}.first(decoded_size));
							!appended)
							return sdk::unexpected(std::move(appended.error()));
					decoded_total += static_cast<std::uint64_t>(decoded_size);
					consumed += count;
				}
				return {};
			}

			[[nodiscard]] sdk::result<sdk::canonical_value> decode_value(const std::uint64_t end,
																		 const std::size_t depth)
			{
				if (depth > maximum_canonical_depth || offset_ >= end || end > input_.size_bytes())
					return failure("task.v3", "depth-or-boundary");
				auto tag = read_octet();
				if (!tag)
					return sdk::unexpected(std::move(tag.error()));
				sdk::canonical_value output;
				switch (*tag)
				{
					case 0x00U:
						output = sdk::canonical_value::null();
						break;
					case 0x01U:
					{
						auto value = read_octet();
						if (!value || *value > 1U)
							return failure("task.v3", "invalid-boolean");
						output = sdk::canonical_value::from_boolean(*value == 1U);
						break;
					}
					case 0x02U:
					{
						auto sign = read_octet();
						auto width = read_length();
						if (!sign || !width || *sign > 1U || *width == 0U || *width > 8U ||
							*width > end - offset_)
							return failure("task.v3", "invalid-integer");
						std::array<std::byte, 8U> bytes{};
						if (auto read = read_exact(
								std::span{bytes}.first(static_cast<std::size_t>(*width)));
							!read)
							return sdk::unexpected(std::move(read.error()));
						if (*width > 1U && bytes.front() == std::byte{0})
							return failure("task.v3", "noncanonical-integer");
						std::uint64_t magnitude{};
						for (std::uint64_t index{}; index < *width; ++index)
							magnitude = (magnitude << 8U) |
								std::to_integer<std::uint8_t>(
											bytes[static_cast<std::size_t>(index)]);
						constexpr auto signed_max =
							static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
						if ((*sign == 0U && magnitude > signed_max) ||
							(*sign == 1U && (magnitude == 0U || magnitude > signed_max + 1U)))
							return failure("task.v3", "integer-out-of-range");
						const auto integer = *sign == 1U
							? (magnitude == signed_max + 1U
								   ? std::numeric_limits<std::int64_t>::min()
								   : -static_cast<std::int64_t>(magnitude))
							: static_cast<std::int64_t>(magnitude);
						output = sdk::canonical_value::from_integer(integer);
						break;
					}
					case 0x03U:
					case 0x04U:
					{
						auto length = read_length();
						if (!length || *length > end - offset_ ||
							*length >
								static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
							return failure("task.v3", "truncated-payload");
						if (*tag == 0x04U && external_source_value())
						{
							if (auto decoded = decode_base64(*length); !decoded)
								return sdk::unexpected(std::move(decoded.error()));
							output = sdk::canonical_value::from_string({});
						}
						else if (*tag == 0x03U)
						{
							std::vector<std::byte> bytes(static_cast<std::size_t>(*length));
							if (auto read = read_exact(bytes); !read)
								return sdk::unexpected(std::move(read.error()));
							output = sdk::canonical_value::from_bytes(std::move(bytes));
						}
						else
						{
							std::string text(static_cast<std::size_t>(*length), '\0');
							if (auto read = read_exact(std::as_writable_bytes(std::span{text}));
								!read)
								return sdk::unexpected(std::move(read.error()));
							output = sdk::canonical_value::from_string(std::move(text));
						}
						break;
					}
					case 0x05U:
					{
						auto count = read_length();
						if (!count || *count > (end - offset_) / 9U ||
							*count >
								static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
							return failure("task.v3", "invalid-tuple-count");
						std::vector<sdk::canonical_value> values;
						values.reserve(static_cast<std::size_t>(*count));
						for (std::uint64_t index{}; index < *count; ++index)
						{
							auto length = read_length();
							if (!length || *length == 0U || *length > end - offset_ ||
								path_size_ == path_.size())
								return failure("task.v3", "invalid-tuple-item-length");
							const auto child_end = offset_ + *length;
							path_[path_size_++] = static_cast<std::size_t>(index);
							auto child = decode_value(child_end, depth + 1U);
							--path_size_;
							if (!child)
								return sdk::unexpected(std::move(child.error()));
							values.push_back(std::move(*child));
						}
						output = sdk::canonical_value::from_tuple(std::move(values));
						break;
					}
					default:
						return failure("task.v3", "unknown-tag");
				}
				if (offset_ != end)
					return failure("task.v3", "trailing-bytes");
				return output;
			}

			clang22_task_input_replay& input_;
			clang22_task_source_spool& source_;
			std::uint64_t offset_{};
			std::uint64_t base64_bytes_{};
			std::array<std::size_t, maximum_canonical_depth> path_{};
			std::size_t path_size_{};
			bool source_seen_{};
		};

		sdk::result<clang22_task_input>
		decode_task_projection(sdk::canonical_value decoded,
							   const clang22_task_source_receipt* source_receipt,
							   const std::optional<std::span<const std::byte>> original_bytes)
		{
			if (decoded.type != sdk::canonical_value::kind::ordered_tuple ||
				decoded.tuple.size() != 5U ||
				decoded.tuple[0U].type != sdk::canonical_value::kind::utf8_string ||
				decoded.tuple[0U].text != task_magic ||
				decoded.tuple[2U].type != sdk::canonical_value::kind::utf8_string ||
				decoded.tuple[3U].type != sdk::canonical_value::kind::utf8_string)
				return sdk::unexpected(
					provider_error("provider.frontend-request-invalid", "payload"));
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
			static constexpr std::array<std::string_view, 2U> sandbox_keys{"minimum",
																		   "policy_digest"};

			auto catalog_fields = parse_object(decoded.tuple[1U], catalog_keys, "project_catalog");
			auto task_fields = parse_object(decoded.tuple[4U], task_keys, "task");
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
				return sdk::unexpected(provider_error("provider.frontend-request-invalid",
													  "catalog_compile_units",
													  "canonical-order"));
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
			auto budget_fields_value =
				parse_object(*task_fields->at("budget"), budget_keys, "budget");
			auto sandbox_fields_value =
				parse_object(*task_fields->at("sandbox"), sandbox_keys, "sandbox");
			if (!source_fields || !toolchain_fields_value || !variant_fields_value ||
				!budget_fields_value || !sandbox_fields_value)
				return sdk::unexpected(
					provider_error("provider.frontend-request-invalid", "task", "nested-object"));

			clang22_task_input output;
			output.project_catalog = std::move(*catalog);
			output.selected_catalog_compile_unit = decoded.tuple[2U].text;
			output.compile_unit = decoded.tuple[3U].text;
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
				!assign(output.variant_authority.target_triple,
						*variant_fields_value,
						"target_triple") ||
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
				return sdk::unexpected(provider_error(
					"provider.frontend-request-invalid", "source.read_only", "boolean"));
			output.source_read_only = read_only.boolean;
			auto source_size =
				parse_nonnegative_integer(*source_fields->at("size_bytes"), "source.size_bytes");
			auto source_base64 = text(*source_fields, "content_base64");
			if (!source_size || !source_base64)
				return sdk::unexpected(
					provider_error("provider.frontend-request-invalid", "source", "content"));
			output.source_size_bytes = *source_size;
			if (source_receipt == nullptr)
			{
				output.source_content_base64 = *source_base64;
				auto source = base64_decode(*source_base64);
				if (!source)
					return sdk::unexpected(std::move(source.error()));
				output.source = std::move(*source);
			}
			else if (!source_base64->empty())
				return sdk::unexpected(provider_error(
					"provider.frontend-request-invalid", "source.content_base64", "stream-marker"));
			auto arguments =
				parse_string_tuple(*task_fields->at("effective_argv"), "effective_argv");
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
			auto valid = source_receipt == nullptr
				? output.validate()
				: output.validate_with_source_receipt(*source_receipt);
			if (!valid)
				return sdk::unexpected(std::move(valid.error()));
			if (original_bytes)
			{
				auto reencoded = encode_task_input(output);
				if (!reencoded || !std::ranges::equal(*reencoded, *original_bytes))
					return sdk::unexpected(provider_error(
						"provider.frontend-request-invalid", "payload", "noncanonical-projection"));
			}
			return output;
		}
	} // namespace

	sdk::result<clang22_task_input> decode_task_input(const std::span<const std::byte> input)
	{
		auto decoded = sdk::canonical_binary_decode(input);
		if (!decoded)
			return sdk::unexpected(provider_error(
				"provider.frontend-request-invalid", "payload", decoded.error().detail));
		return decode_task_projection(std::move(*decoded), nullptr, input);
	}

	sdk::result<clang22_task_v3_stream_decoded>
	decode_task_input_streaming(clang22_task_input_replay& input, clang22_task_source_spool& source)
	{
		try
		{
			streaming_task_v3_decoder decoder{input, source};
			auto projection = decoder.decode();
			if (!projection)
				return sdk::unexpected(std::move(projection.error()));
			auto sealed = source.seal();
			if (!sealed)
				return sdk::unexpected(std::move(sealed.error()));
			if (!source.sealed() || source.size_bytes() != sealed->size_bytes ||
				source.receipt() != *sealed)
				return sdk::unexpected(provider_error(
					"provider.frontend-request-invalid", "source", "invalid-spool-seal"));
			auto decoded = decode_task_projection(std::move(*projection), &*sealed, std::nullopt);
			if (!decoded)
				return sdk::unexpected(std::move(decoded.error()));
			return clang22_task_v3_stream_decoded{std::move(*decoded),
												  std::move(*sealed),
												  decoder.canonical_base64_bytes(),
												  input.size_bytes()};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				provider_error("provider.frontend-request-invalid", "task.v3", "allocation"));
		}
	}

	namespace
	{
		[[nodiscard]] sdk::result<std::string>
		condition_ref_from_validated_input(const clang22_task_input& input)
		{
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

		[[nodiscard]] sdk::result<std::string>
		derive_validated_provider_task_id(const clang22_task_input& input,
										  const sdk::project_catalog& catalog,
										  std::vector<sdk::relation_descriptor> exact_outputs,
										  std::string provider_semantic_contract_digest)
		{
			if (!canonical_digest(provider_semantic_contract_digest))
				return sdk::unexpected(provider_error("provider.frontend-request-invalid",
													  "provider_semantic_contract_digest",
													  "digest"));
			const auto descriptor_order =
				[](const sdk::relation_descriptor& left, const sdk::relation_descriptor& right)
			{
				return std::tie(left.id, left.descriptor_digest) <
					std::tie(right.id, right.descriptor_digest);
			};
			std::ranges::sort(exact_outputs, descriptor_order);
			if (exact_outputs.size() != input.requested_descriptors.size())
				return sdk::unexpected(provider_error(
					"provider.frontend-request-invalid", "requested_descriptor_ids", "count"));
			for (std::size_t index{}; index < exact_outputs.size(); ++index)
				if (exact_outputs[index].id != input.requested_descriptors[index])
					return sdk::unexpected(provider_error("provider.frontend-request-invalid",
														  "requested_descriptor_ids",
														  "binding"));
			auto condition_ref = condition_ref_from_validated_input(input);
			if (!condition_ref)
				return sdk::unexpected(std::move(condition_ref.error()));
			const auto descriptor_values =
				[](const std::span<const sdk::relation_descriptor> values)
			{
				std::vector<sdk::canonical_value> projection;
				projection.reserve(values.size());
				for (const auto& descriptor : values)
					projection.push_back(sdk::canonical_value::from_tuple({
						sdk::canonical_value::from_string(descriptor.id),
						sdk::canonical_value::from_string(descriptor.descriptor_digest),
					}));
				return sdk::canonical_value::from_tuple(std::move(projection));
			};
			const auto string_values = [](const std::span<const std::string> values)
			{
				std::vector<sdk::canonical_value> projection;
				projection.reserve(values.size());
				for (const auto& value : values)
					projection.push_back(sdk::canonical_value::from_string(value));
				return sdk::canonical_value::from_tuple(std::move(projection));
			};
			const std::vector<std::string> interpretations{"cc.clang22-canonical-1"};
			auto projection = sdk::canonical_binary(sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_string("cxxlens.provider-task.v1"),
				sdk::canonical_value::from_string(std::string{provider_id}),
				sdk::canonical_value::from_string(provider_version.string()),
				sdk::canonical_value::from_string(provider_semantic_contract_digest),
				sdk::canonical_value::from_string(catalog.catalog_id),
				sdk::canonical_value::from_string(catalog.catalog_digest),
				descriptor_values(exact_outputs),
				sdk::canonical_value::from_string(*condition_ref),
				sdk::canonical_value::from_string(input.interpretation),
				descriptor_values(exact_outputs),
				descriptor_values(std::span<const sdk::relation_descriptor>{}),
				string_values(interpretations),
				sdk::canonical_value::from_string("observation"),
				sdk::canonical_value::from_string("assertion"),
				string_values(input.dependency_groups),
			}));
			if (!projection)
				return sdk::unexpected(std::move(projection.error()));
			std::string projection_bytes;
			projection_bytes.reserve(projection->size());
			for (const auto byte : *projection)
				projection_bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
			auto digest = sdk::semantic_digest("cxxlens.provider-task.v1", projection_bytes);
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			return "task:" + *digest;
		}

		[[nodiscard]] sdk::result<sdk::provider::task>
		reconstruct_validated_provider_task(const clang22_task_input& input,
											std::vector<sdk::relation_descriptor> exact_outputs,
											std::string provider_semantic_contract_digest)
		{
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
					return sdk::unexpected(provider_error("provider.frontend-request-invalid",
														  "requested_descriptor_ids",
														  "binding"));
			auto condition_ref = condition_ref_from_validated_input(input);
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
	} // namespace

	sdk::result<std::string> provider_condition_ref_id(const clang22_task_input& input)
	{
		if (auto valid = input.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return condition_ref_from_validated_input(input);
	}

	sdk::result<std::string> provider_condition_ref_id(const clang22_task_input& input,
													   const clang22_task_source_receipt& source)
	{
		if (auto valid = input.validate_with_source_receipt(source); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return condition_ref_from_validated_input(input);
	}

	sdk::result<sdk::provider::task>
	reconstruct_provider_task(const clang22_task_input& input,
							  std::vector<sdk::relation_descriptor> exact_outputs,
							  std::string provider_semantic_contract_digest)
	{
		if (auto valid = input.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return reconstruct_validated_provider_task(
			input, std::move(exact_outputs), std::move(provider_semantic_contract_digest));
	}

	sdk::result<sdk::provider::task>
	reconstruct_provider_task(const clang22_task_input& input,
							  const clang22_task_source_receipt& source,
							  std::vector<sdk::relation_descriptor> exact_outputs,
							  std::string provider_semantic_contract_digest)
	{
		if (auto valid = input.validate_with_source_receipt(source); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return reconstruct_validated_provider_task(
			input, std::move(exact_outputs), std::move(provider_semantic_contract_digest));
	}

	sdk::result<std::string>
	reconstruct_provider_task_id(const clang22_task_input& input,
								 const sdk::project_catalog& catalog,
								 const clang22_task_source_receipt& source,
								 std::vector<sdk::relation_descriptor> exact_outputs,
								 std::string provider_semantic_contract_digest)
	{
		if (auto valid = input.validate_with_catalog(catalog, source); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return derive_validated_provider_task_id(
			input, catalog, std::move(exact_outputs), std::move(provider_semantic_contract_digest));
	}
} // namespace cxxlens::detail::clang22
