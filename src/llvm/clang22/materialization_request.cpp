#include "materialization_request.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_span.hpp>

#include "materialization_identity.hpp"
#include "observation_v2.hpp"
#include "unicode_nfc.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		using sdk::canonical_value;
		using sdk::relation_descriptor;

		constexpr std::string_view authority_registry_digest =
			"sha256:4caf626ec6f198118802f22d9cac62b02b2c3bb392fdc8d68b1a58f8101c342e";
		constexpr std::string_view engine_generation_contract =
			"cxxlens.clang22-materialization-engine.v2";
		constexpr std::string_view interpretation_policy_id =
			"cxxlens.clang22-interpretation-policy.v1";
		constexpr std::string_view interpretation_domain = "cc.clang22-canonical-1";
		constexpr std::string_view trust_policy_id =
			"cxxlens.clang22-installed-native-worker-trust.v1";

		[[nodiscard]] bool lower_hex(const std::string_view value) noexcept
		{
			return std::ranges::all_of(value,
									   [](const char character)
									   {
										   return (character >= '0' && character <= '9') ||
											   (character >= 'a' && character <= 'f');
									   });
		}

		[[nodiscard]] bool has_digest_grammar(const std::string_view value) noexcept
		{
			constexpr std::string_view content_prefix{"sha256:"};
			constexpr std::string_view semantic_prefix{"semantic-v2:sha256:"};
			const auto matches = [&](const std::string_view prefix)
			{
				return value.size() == prefix.size() + 64U && value.starts_with(prefix) &&
					lower_hex(value.substr(prefix.size()));
			};
			return matches(content_prefix) || matches(semantic_prefix);
		}

		[[nodiscard]] bool has_semantic_digest_grammar(const std::string_view value) noexcept
		{
			constexpr std::string_view prefix{"semantic-v2:sha256:"};
			return value.size() == prefix.size() + 64U && value.starts_with(prefix) &&
				lower_hex(value.substr(prefix.size()));
		}

		[[nodiscard]] bool has_revision_grammar(const std::string_view value) noexcept
		{
			return value.size() == 40U && lower_hex(value);
		}

		[[nodiscard]] sdk::error
		request_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error invalid(std::string field, std::string detail = {})
		{
			return request_error(
				"materialization.request-invalid", std::move(field), std::move(detail));
		}

		[[nodiscard]] sdk::error mismatch(std::string field, std::string detail = {})
		{
			return request_error(
				"materialization.identity-mismatch", std::move(field), std::move(detail));
		}

		[[nodiscard]] sdk::error unsupported_version(std::string field)
		{
			return request_error(
				"materialization.version-unsupported", std::move(field), "unsupported");
		}

		template <std::size_t Size>
		[[nodiscard]] sdk::result<void>
		exact_members(const json_value& value,
					  const std::array<std::string_view, Size>& names,
					  const std::string_view field)
		{
			if (!value.has_exact_members(names))
				return sdk::unexpected(invalid(std::string{field}, "member-set"));
			return {};
		}

		[[nodiscard]] sdk::result<const json_value*> required_member(const json_value& value,
																	 const std::string_view name,
																	 const std::string_view field)
		{
			const auto* member = value.member(name);
			if (member == nullptr)
				return sdk::unexpected(invalid(std::string{field}, std::string{name}));
			return member;
		}

		[[nodiscard]] sdk::result<std::string> text(const json_value& value,
													const std::string_view field)
		{
			const auto* result = value.as_string();
			if (result == nullptr)
				return sdk::unexpected(invalid(std::string{field}, "string"));
			return *result;
		}

		[[nodiscard]] sdk::result<std::string> member_text(const json_value& value,
														   const std::string_view name,
														   const std::string_view field)
		{
			auto member = required_member(value, name, field);
			if (!member)
				return sdk::unexpected(std::move(member.error()));
			return text(**member, field);
		}

		[[nodiscard]] sdk::result<bool> member_boolean(const json_value& value,
													   const std::string_view name,
													   const std::string_view field)
		{
			auto member = required_member(value, name, field);
			if (!member)
				return sdk::unexpected(std::move(member.error()));
			const auto* result = (*member)->as_boolean();
			if (result == nullptr)
				return sdk::unexpected(invalid(std::string{field}, "boolean"));
			return *result;
		}

		[[nodiscard]] sdk::result<std::uint64_t>
		unsigned_integer(const json_value& value,
						 const std::string_view field,
						 const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max())
		{
			std::uint64_t output{};
			if (const auto* positive = value.as_unsigned_integer(); positive != nullptr)
				output = *positive;
			else if (const auto* signed_value = value.as_signed_integer();
					 signed_value != nullptr && *signed_value >= 0)
				output = static_cast<std::uint64_t>(*signed_value);
			else
				return sdk::unexpected(invalid(std::string{field}, "unsigned-integer"));
			if (output > maximum)
				return sdk::unexpected(invalid(std::string{field}, "integer-range"));
			return output;
		}

		[[nodiscard]] sdk::result<std::uint64_t>
		member_unsigned(const json_value& value,
						const std::string_view name,
						const std::string_view field,
						const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max())
		{
			auto member = required_member(value, name, field);
			if (!member)
				return sdk::unexpected(std::move(member.error()));
			return unsigned_integer(**member, field, maximum);
		}

		[[nodiscard]] sdk::result<std::optional<std::string>> nullable_text(
			const json_value& value, const std::string_view name, const std::string_view field)
		{
			auto member = required_member(value, name, field);
			if (!member)
				return sdk::unexpected(std::move(member.error()));
			if ((*member)->is_null())
				return std::optional<std::string>{};
			auto decoded = text(**member, field);
			if (!decoded)
				return sdk::unexpected(std::move(decoded.error()));
			return std::optional<std::string>{std::move(*decoded)};
		}

		[[nodiscard]] sdk::result<std::vector<std::string>>
		string_array(const json_value& value, const std::string_view field)
		{
			const auto* array = value.as_array();
			if (array == nullptr)
				return sdk::unexpected(invalid(std::string{field}, "array"));
			std::vector<std::string> output;
			output.reserve(array->size());
			for (const auto& item : *array)
			{
				auto decoded = text(item, field);
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				output.push_back(std::move(*decoded));
			}
			return output;
		}

		[[nodiscard]] sdk::result<std::vector<std::string>> member_string_array(
			const json_value& value, const std::string_view name, const std::string_view field)
		{
			auto member = required_member(value, name, field);
			if (!member)
				return sdk::unexpected(std::move(member.error()));
			return string_array(**member, field);
		}

		[[nodiscard]] sdk::result<std::string> semantic_tuple_digest(const std::string_view domain,
																	 canonical_value projection)
		{
			auto encoded = sdk::canonical_binary(projection);
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			return sdk::semantic_digest(
				domain,
				std::string_view{reinterpret_cast<const char*>(encoded->data()), encoded->size()});
		}

		[[nodiscard]] sdk::result<std::string> identity(const std::string_view kind,
														std::vector<canonical_value> fields)
		{
			return sdk::canonical_identity_digest(kind, fields);
		}

		[[nodiscard]] sdk::detached_cell text_cell(const sdk::value_type& type, std::string value)
		{
			return {type, sdk::cell_state::present, sdk::scalar_value{std::move(value)}, {}};
		}

		[[nodiscard]] sdk::result<void> set_cell(sdk::detached_row& row,
												 const relation_descriptor& descriptor,
												 const std::string_view name,
												 sdk::detached_cell value)
		{
			auto column = descriptor.column(name);
			if (!column)
				return sdk::unexpected(std::move(column.error()));
			if (value.type != column->type)
				return sdk::unexpected(mismatch(descriptor.id + "." + std::string{name}, "type"));
			row.cells.emplace(column->id, std::move(value));
			return {};
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		finish_identity_row(const relation_descriptor& descriptor,
							std::string_view result_name,
							sdk::detached_row row)
		{
			auto result_column = descriptor.column(result_name);
			if (!result_column)
				return sdk::unexpected(std::move(result_column.error()));
			if (auto inserted = set_cell(
					row, descriptor, result_name, text_cell(result_column->type, "pending"));
				!inserted)
				return sdk::unexpected(std::move(inserted.error()));
			auto derived = sdk::derive_domain_identity(descriptor, row);
			if (!derived)
				return sdk::unexpected(std::move(derived.error()));
			row.cells.at(result_column->id) = text_cell(result_column->type, std::move(*derived));
			if (auto valid = sdk::validate_row(descriptor, row); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = sdk::validate_domain_identity(descriptor, row); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return row;
		}

		[[nodiscard]] sdk::result<std::string> row_string(const sdk::detached_row& row,
														  const relation_descriptor& descriptor,
														  const std::string_view name)
		{
			auto column = descriptor.column(name);
			if (!column)
				return sdk::unexpected(std::move(column.error()));
			const auto found = row.cells.find(column->id);
			if (found == row.cells.end() || !found->second.value)
				return sdk::unexpected(mismatch(descriptor.id + "." + std::string{name}));
			const auto* output = std::get_if<std::string>(&*found->second.value);
			if (output == nullptr)
				return sdk::unexpected(mismatch(descriptor.id + "." + std::string{name}, "value"));
			return *output;
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		project_row(const sdk::project_catalog& catalog)
		{
			const auto descriptor = cxxlens::build::relations::project::descriptor();
			sdk::detached_row row{descriptor.id, {}};
			for (auto item : {
					 std::pair{std::string_view{"catalog"}, std::string_view{catalog.catalog_id}},
					 std::pair{std::string_view{"catalog_digest"},
							   std::string_view{catalog.catalog_digest}},
					 std::pair{std::string_view{"logical_root"},
							   std::string_view{catalog.logical_root}},
					 std::pair{std::string_view{"environment_digest"},
							   std::string_view{catalog.environment_digest}},
				 })
			{
				auto column = descriptor.column(item.first);
				if (!column)
					return sdk::unexpected(std::move(column.error()));
				if (auto set = set_cell(row,
										descriptor,
										item.first,
										text_cell(column->type, std::string{item.second}));
					!set)
					return sdk::unexpected(std::move(set.error()));
			}
			return finish_identity_row(descriptor, "project", std::move(row));
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		decode_base64(const std::string_view encoded, const std::size_t maximum_output)
		{
			if (encoded.size() % 4U != 0U)
				return sdk::unexpected(invalid("source.content_base64", "length"));
			const auto value = [](const char character) -> int
			{
				if (character >= 'A' && character <= 'Z')
					return character - 'A';
				if (character >= 'a' && character <= 'z')
					return character - 'a' + 26;
				if (character >= '0' && character <= '9')
					return character - '0' + 52;
				if (character == '+')
					return 62;
				if (character == '/')
					return 63;
				return -1;
			};
			std::vector<std::byte> output;
			output.reserve(std::min(maximum_output, encoded.size() / 4U * 3U));
			for (std::size_t offset = 0; offset < encoded.size(); offset += 4U)
			{
				const bool last = offset + 4U == encoded.size();
				const bool pad_two = encoded[offset + 2U] == '=';
				const bool pad_three = encoded[offset + 3U] == '=';
				if ((!last && (pad_two || pad_three)) || (pad_two && !pad_three))
					return sdk::unexpected(invalid("source.content_base64", "padding"));
				const int a = value(encoded[offset]);
				const int b = value(encoded[offset + 1U]);
				const int c = pad_two ? 0 : value(encoded[offset + 2U]);
				const int d = pad_three ? 0 : value(encoded[offset + 3U]);
				if (a < 0 || b < 0 || c < 0 || d < 0)
					return sdk::unexpected(invalid("source.content_base64", "alphabet"));
				const auto append = [&](const unsigned integer) -> sdk::result<void>
				{
					if (output.size() >= maximum_output)
						return sdk::unexpected(invalid("source.content_base64", "decoded-limit"));
					output.push_back(static_cast<std::byte>(integer));
					return {};
				};
				if (auto added = append(static_cast<unsigned>((a << 2) | (b >> 4))); !added)
					return sdk::unexpected(std::move(added.error()));
				if (!pad_two)
					if (auto added = append(static_cast<unsigned>(((b & 0x0f) << 4) | (c >> 2)));
						!added)
						return sdk::unexpected(std::move(added.error()));
				if (!pad_three)
					if (auto added = append(static_cast<unsigned>(((c & 0x03) << 6) | d)); !added)
						return sdk::unexpected(std::move(added.error()));
			}
			return output;
		}

		[[nodiscard]] sdk::result<std::pair<std::string, std::string>>
		normalized_project_path(const std::string_view value)
		{
			constexpr std::string_view prefix{"project://"};
			auto normalized = is_nfc_utf8(value);
			const bool has_control =
				std::ranges::any_of(value,
									[](const unsigned char character)
									{
										return character < 0x20U || character == 0x7fU;
									});
			if (!normalized || !*normalized || has_control || !value.starts_with(prefix))
				return sdk::unexpected(mismatch("source.logical_path", "domain"));
			const auto relative = value.substr(prefix.size());
			if (relative.empty() || relative.contains('\\') || relative.contains('?') ||
				relative.contains('#'))
				return sdk::unexpected(mismatch("source.logical_path", "canonical"));
			std::size_t begin{};
			while (begin <= relative.size())
			{
				const auto end = relative.find('/', begin);
				const auto segment = relative.substr(
					begin, end == std::string_view::npos ? relative.size() - begin : end - begin);
				if (segment.empty() || segment == "." || segment == "..")
					return sdk::unexpected(mismatch("source.logical_path", "segment"));
				if (end == std::string_view::npos)
					break;
				begin = end + 1U;
			}
			return std::pair{std::string{"project"}, std::string{relative}};
		}

		[[nodiscard]] sdk::result<std::string> file_id(const std::string_view logical_path)
		{
			auto normalized = normalized_project_path(logical_path);
			if (!normalized)
				return sdk::unexpected(std::move(normalized.error()));
			return identity("file",
							{canonical_value::from_string(std::move(normalized->first)),
							 canonical_value::from_string(std::move(normalized->second)),
							 canonical_value::from_string("cxxlens.logical-path.v1")});
		}

		[[nodiscard]] sdk::result<std::string>
		line_index_id(const std::span<const std::byte> source)
		{
			std::vector<canonical_value> offsets;
			offsets.push_back(canonical_value::from_integer(0));
			for (std::size_t index = 0; index < source.size(); ++index)
				if (source[index] == std::byte{'\n'})
					offsets.push_back(
						canonical_value::from_integer(static_cast<std::int64_t>(index + 1U)));
			return identity(
				"line-index",
				{canonical_value::from_string("cxxlens.byte-line-index.v1"),
				 canonical_value::from_string(sdk::content_digest(source)),
				 canonical_value::from_integer(static_cast<std::int64_t>(source.size())),
				 canonical_value::from_tuple(std::move(offsets))});
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		toolchain_row(const clang22_task_input::toolchain_fields& value)
		{
			const auto descriptor = cxxlens::build::relations::toolchain_context::descriptor();
			sdk::detached_row row{descriptor.id, {}};
			for (const auto& [name, item] : {
					 std::pair{std::string_view{"family"}, std::string_view{value.family}},
					 std::pair{std::string_view{"exact_version"},
							   std::string_view{value.exact_version}},
					 std::pair{std::string_view{"target_triple"},
							   std::string_view{value.target_triple}},
					 std::pair{std::string_view{"builtin_headers_digest"},
							   std::string_view{value.builtin_headers_digest}},
					 std::pair{std::string_view{"abi_digest"}, std::string_view{value.abi_digest}},
					 std::pair{std::string_view{"plugin_spec_digest"},
							   std::string_view{value.plugin_spec_digest}},
				 })
			{
				auto column = descriptor.column(name);
				if (!column)
					return sdk::unexpected(std::move(column.error()));
				if (auto set =
						set_cell(row, descriptor, name, text_cell(column->type, std::string{item}));
					!set)
					return sdk::unexpected(std::move(set.error()));
			}
			auto sysroot = descriptor.column("sysroot");
			if (!sysroot)
				return sdk::unexpected(std::move(sysroot.error()));
			if (value.sysroot)
			{
				if (auto set = set_cell(
						row, descriptor, "sysroot", text_cell(sysroot->type, *value.sysroot));
					!set)
					return sdk::unexpected(std::move(set.error()));
			}
			else if (auto set = set_cell(
						 row, descriptor, "sysroot", sdk::detached_cell::absent(sysroot->type));
					 !set)
				return sdk::unexpected(std::move(set.error()));
			return finish_identity_row(descriptor, "toolchain", std::move(row));
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		variant_row(const clang22_task_input::variant_fields& value,
					const std::string_view project,
					const std::string_view toolchain)
		{
			const auto descriptor = cxxlens::build::relations::variant::descriptor();
			sdk::detached_row row{descriptor.id, {}};
			for (const auto& [name, item] : {
					 std::pair{std::string_view{"project"}, project},
					 std::pair{std::string_view{"toolchain"}, toolchain},
					 std::pair{std::string_view{"language"}, std::string_view{value.language}},
					 std::pair{std::string_view{"language_standard"},
							   std::string_view{value.language_standard}},
					 std::pair{std::string_view{"target_triple"},
							   std::string_view{value.target_triple}},
					 std::pair{std::string_view{"predefined_macros_digest"},
							   std::string_view{value.predefined_macros_digest}},
					 std::pair{std::string_view{"include_search_digest"},
							   std::string_view{value.include_search_digest}},
					 std::pair{std::string_view{"semantic_flags_digest"},
							   std::string_view{value.semantic_flags_digest}},
				 })
			{
				auto column = descriptor.column(name);
				if (!column)
					return sdk::unexpected(std::move(column.error()));
				if (auto set =
						set_cell(row, descriptor, name, text_cell(column->type, std::string{item}));
					!set)
					return sdk::unexpected(std::move(set.error()));
			}
			return finish_identity_row(descriptor, "variant", std::move(row));
		}

		[[nodiscard]] sdk::result<sdk::detached_row> source_file_row(const clang22_task_input& task,
																	 const std::string_view project)
		{
			const auto descriptor = cxxlens::source::relations::file::descriptor();
			sdk::detached_row row{descriptor.id, {}};
			for (const auto& [name, item] : {
					 std::pair{std::string_view{"file"}, std::string_view{task.file}},
					 std::pair{std::string_view{"project"}, project},
					 std::pair{std::string_view{"logical_path"},
							   std::string_view{task.logical_path}},
					 std::pair{std::string_view{"content"},
							   std::string_view{task.source_content_digest}},
					 std::pair{std::string_view{"encoding"},
							   std::string_view{task.source_encoding}},
					 std::pair{std::string_view{"line_index"}, std::string_view{task.line_index}},
				 })
			{
				auto column = descriptor.column(name);
				if (!column)
					return sdk::unexpected(std::move(column.error()));
				if (auto set =
						set_cell(row, descriptor, name, text_cell(column->type, std::string{item}));
					!set)
					return sdk::unexpected(std::move(set.error()));
			}
			if (auto set = set_cell(row,
									descriptor,
									"size",
									sdk::detached_cell::unsigned_integer(task.source_size_bytes));
				!set)
				return sdk::unexpected(std::move(set.error()));
			if (auto set = set_cell(row,
									descriptor,
									"read_only",
									sdk::detached_cell::boolean(task.source_read_only));
				!set)
				return sdk::unexpected(std::move(set.error()));
			return finish_identity_row(descriptor, "snapshot", std::move(row));
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		compile_unit_row(const clang22_task_input& task)
		{
			const auto descriptor = cxxlens::build::relations::compile_unit::descriptor();
			sdk::detached_row row{descriptor.id, {}};
			for (const auto& [name, item] : {
					 std::pair{std::string_view{"project"}, std::string_view{task.project}},
					 std::pair{std::string_view{"main_source"},
							   std::string_view{task.source_snapshot}},
					 std::pair{std::string_view{"variant"}, std::string_view{task.variant}},
					 std::pair{std::string_view{"toolchain"},
							   std::string_view{task.toolchain_context}},
					 std::pair{std::string_view{"effective_invocation_digest"},
							   std::string_view{task.normalized_invocation_digest}},
					 std::pair{std::string_view{"language"}, std::string_view{task.language}},
					 std::pair{std::string_view{"working_directory"},
							   std::string_view{task.working_directory}},
				 })
			{
				auto column = descriptor.column(name);
				if (!column)
					return sdk::unexpected(std::move(column.error()));
				if (auto set =
						set_cell(row, descriptor, name, text_cell(column->type, std::string{item}));
					!set)
					return sdk::unexpected(std::move(set.error()));
			}
			return finish_identity_row(descriptor, "compile_unit", std::move(row));
		}

		[[nodiscard]] sdk::result<std::string> base_row_digest(const std::string_view descriptor_id,
															   json_value row)
		{
			auto descriptor = json_string(std::string{descriptor_id});
			if (!descriptor)
				return sdk::unexpected(std::move(descriptor.error()));
			auto wrapper =
				json_object({{"descriptor_id", std::move(*descriptor)}, {"row", std::move(row)}});
			if (!wrapper)
				return sdk::unexpected(std::move(wrapper.error()));
			return projection_digest("cxxlens.base-claim-row.v1", *wrapper);
		}

		[[nodiscard]] sdk::result<void> validate_tool_worker_topology(const json_value& root)
		{
			auto tool_value = required_member(root, "tool", "tool");
			auto worker_value = required_member(root, "worker", "worker");
			auto topology_value = required_member(root, "group_topology", "group_topology");
			if (!tool_value || !worker_value || !topology_value)
				return sdk::unexpected(invalid("request.machine-binding"));
			constexpr std::array tool_members{
				std::string_view{"executable"},
				std::string_view{"interface_version"},
				std::string_view{"distribution_version"},
				std::string_view{"source_revision"},
				std::string_view{"source_tree"},
				std::string_view{"installed_executable_digest"},
				std::string_view{"package_configuration"},
				std::string_view{"prefix_manifest_digest"},
				std::string_view{"relocated_prefix_digest"},
			};
			constexpr std::array worker_members{
				std::string_view{"executable"},
				std::string_view{"provider_id"},
				std::string_view{"provider_version"},
				std::string_view{"installed_binary_digest"},
				std::string_view{"semantic_contract_digest"},
				std::string_view{"protocol_major"},
				std::string_view{"protocol_minor"},
				std::string_view{"sandbox_policy_digest"},
			};
			constexpr std::array topology_members{
				std::string_view{"dependency_groups"},
				std::string_view{"atomic_output_group"},
				std::string_view{"partial_policy"},
			};
			if (!(*tool_value)->has_exact_members(tool_members) ||
				!(*worker_value)->has_exact_members(worker_members) ||
				!(*topology_value)->has_exact_members(topology_members))
				return sdk::unexpected(invalid("request.machine-binding", "member-set"));
			for (const auto& [object, name, expected] : {
					 std::tuple{*tool_value, "executable", "cxxlens-clang22-materialize"},
					 std::tuple{*tool_value, "interface_version", "2.0.0"},
					 std::tuple{*tool_value, "distribution_version", "1.0.0"},
					 std::tuple{*worker_value, "executable", "cxxlens-clang-worker-22"},
					 std::tuple{*worker_value, "provider_id", "cxxlens.clang22.reference"},
					 std::tuple{*worker_value, "provider_version", "1.0.0"},
					 std::tuple{*topology_value, "atomic_output_group", "clang22-atomic"},
					 std::tuple{*topology_value, "partial_policy", "forbid"},
				 })
			{
				auto supplied = member_text(*object, name, name);
				if (!supplied || *supplied != expected)
					return sdk::unexpected(invalid(name, "constant"));
			}
			auto configuration =
				member_text(**tool_value, "package_configuration", "tool.package_configuration");
			if (!configuration || (*configuration != "static" && *configuration != "shared"))
				return sdk::unexpected(invalid("tool.package_configuration", "closed-enum"));
			auto protocol_major =
				member_unsigned(**worker_value, "protocol_major", "worker.protocol_major");
			auto protocol_minor =
				member_unsigned(**worker_value, "protocol_minor", "worker.protocol_minor");
			if (!protocol_major || !protocol_minor || *protocol_major != 1U ||
				*protocol_minor != 0U)
				return sdk::unexpected(invalid("worker.protocol", "version"));
			auto groups = member_string_array(
				**topology_value, "dependency_groups", "group_topology.dependency_groups");
			if (!groups || *groups != std::vector<std::string>{"canonical", "observation"})
				return sdk::unexpected(invalid("group_topology.dependency_groups", "exact-order"));

			for (const auto name :
				 {std::string_view{"source_revision"}, std::string_view{"source_tree"}})
			{
				auto revision =
					member_text(**tool_value, name, std::string{"tool."} + std::string{name});
				if (!revision || !has_revision_grammar(*revision))
					return sdk::unexpected(
						invalid(std::string{"tool."} + std::string{name}, "revision"));
			}
			for (const auto& [object, prefix, names] : {
					 std::tuple{*tool_value,
								std::string_view{"tool."},
								std::array{std::string_view{"installed_executable_digest"},
										   std::string_view{"prefix_manifest_digest"},
										   std::string_view{"relocated_prefix_digest"}}},
					 std::tuple{*worker_value,
								std::string_view{"worker."},
								std::array{std::string_view{"installed_binary_digest"},
										   std::string_view{"semantic_contract_digest"},
										   std::string_view{"sandbox_policy_digest"}}},
				 })
				for (const auto name : names)
				{
					auto digest =
						member_text(*object, name, std::string{prefix} + std::string{name});
					if (!digest || !has_digest_grammar(*digest))
						return sdk::unexpected(
							invalid(std::string{prefix} + std::string{name}, "digest"));
				}
			return {};
		}

		[[nodiscard]] sdk::result<validated_task_request>
		parse_task(const json_value& value,
				   const sdk::project_catalog& catalog,
				   const std::string_view project_id,
				   const std::span<const relation_descriptor> outputs,
				   const json_value& worker)
		{
			constexpr std::array members{
				std::string_view{"provider_task_id"},
				std::string_view{"provider_execution_id"},
				std::string_view{"task_input_digest"},
				std::string_view{"project_id"},
				std::string_view{"catalog_id"},
				std::string_view{"catalog_digest"},
				std::string_view{"selected_catalog_compile_unit_id"},
				std::string_view{"compile_unit_id"},
				std::string_view{"build_variant_id"},
				std::string_view{"toolchain_context_id"},
				std::string_view{"toolchain_digest"},
				std::string_view{"toolchain"},
				std::string_view{"variant"},
				std::string_view{"normalized_invocation_digest"},
				std::string_view{"environment_digest"},
				std::string_view{"language"},
				std::string_view{"working_directory"},
				std::string_view{"condition_universe_id"},
				std::string_view{"condition_id"},
				std::string_view{"interpretation_domain"},
				std::string_view{"source"},
				std::string_view{"effective_argv"},
				std::string_view{"requested_descriptor_ids"},
				std::string_view{"dependency_groups"},
				std::string_view{"budget"},
				std::string_view{"sandbox"},
			};
			if (auto exact = exact_members(value, members, "task"); !exact)
				return sdk::unexpected(std::move(exact.error()));

			clang22_task_input task;
			task.project_catalog = catalog;
			auto assign = [&](std::string& target, const std::string_view name) -> sdk::result<void>
			{
				auto supplied = member_text(value, name, std::string{"task."} + std::string{name});
				if (!supplied)
					return sdk::unexpected(std::move(supplied.error()));
				target = std::move(*supplied);
				return {};
			};
			for (auto item : {
					 std::pair{&task.selected_catalog_compile_unit,
							   std::string_view{"selected_catalog_compile_unit_id"}},
					 std::pair{&task.compile_unit, std::string_view{"compile_unit_id"}},
					 std::pair{&task.project, std::string_view{"project_id"}},
					 std::pair{&task.variant, std::string_view{"build_variant_id"}},
					 std::pair{&task.toolchain_context, std::string_view{"toolchain_context_id"}},
					 std::pair{&task.toolchain_digest, std::string_view{"toolchain_digest"}},
					 std::pair{&task.normalized_invocation_digest,
							   std::string_view{"normalized_invocation_digest"}},
					 std::pair{&task.environment_digest, std::string_view{"environment_digest"}},
					 std::pair{&task.language, std::string_view{"language"}},
					 std::pair{&task.working_directory, std::string_view{"working_directory"}},
					 std::pair{&task.condition_universe, std::string_view{"condition_universe_id"}},
					 std::pair{&task.condition, std::string_view{"condition_id"}},
					 std::pair{&task.interpretation, std::string_view{"interpretation_domain"}},
				 })
				if (auto assigned = assign(*item.first, item.second); !assigned)
					return sdk::unexpected(std::move(assigned.error()));
			if (task.project != project_id)
				return sdk::unexpected(mismatch("task.project_id"));
			for (const auto& [field, expected] : {
					 std::pair{std::string_view{"catalog_id"},
							   std::string_view{catalog.catalog_id}},
					 std::pair{std::string_view{"catalog_digest"},
							   std::string_view{catalog.catalog_digest}},
				 })
			{
				auto supplied = member_text(value, field, field);
				if (!supplied || *supplied != expected)
					return sdk::unexpected(mismatch(std::string{"task."} + std::string{field}));
			}

			auto toolchain_value = required_member(value, "toolchain", "task.toolchain");
			auto variant_value = required_member(value, "variant", "task.variant");
			auto source_value = required_member(value, "source", "task.source");
			auto budget_value = required_member(value, "budget", "task.budget");
			auto sandbox_value = required_member(value, "sandbox", "task.sandbox");
			if (!toolchain_value || !variant_value || !source_value || !budget_value ||
				!sandbox_value)
				return sdk::unexpected(invalid("task", "nested-object"));
			constexpr std::array toolchain_members{
				std::string_view{"family"},
				std::string_view{"exact_version"},
				std::string_view{"target_triple"},
				std::string_view{"builtin_headers_digest"},
				std::string_view{"sysroot"},
				std::string_view{"abi_digest"},
				std::string_view{"plugin_spec_digest"},
			};
			constexpr std::array variant_members{
				std::string_view{"language"},
				std::string_view{"language_standard"},
				std::string_view{"target_triple"},
				std::string_view{"predefined_macros_digest"},
				std::string_view{"include_search_digest"},
				std::string_view{"semantic_flags_digest"},
			};
			constexpr std::array source_members{
				std::string_view{"source_snapshot_id"},
				std::string_view{"file_id"},
				std::string_view{"logical_path"},
				std::string_view{"content_digest"},
				std::string_view{"size_bytes"},
				std::string_view{"encoding"},
				std::string_view{"line_index_id"},
				std::string_view{"read_only"},
				std::string_view{"content_base64"},
			};
			constexpr std::array budget_members{
				std::string_view{"output_bytes"},
				std::string_view{"rows"},
				std::string_view{"diagnostics"},
				std::string_view{"wall_ms"},
				std::string_view{"cpu_ms"},
				std::string_view{"address_space_bytes"},
				std::string_view{"transport_bytes"},
				std::string_view{"open_files"},
				std::string_view{"subprocesses"},
			};
			constexpr std::array sandbox_members{std::string_view{"minimum"},
												 std::string_view{"policy_digest"}};
			if (!(**toolchain_value).has_exact_members(toolchain_members) ||
				!(**variant_value).has_exact_members(variant_members) ||
				!(**source_value).has_exact_members(source_members) ||
				!(**budget_value).has_exact_members(budget_members) ||
				!(**sandbox_value).has_exact_members(sandbox_members))
				return sdk::unexpected(invalid("task", "nested-member-set"));

			auto assign_nested = [&](std::string& target,
									 const json_value& object,
									 const std::string_view name) -> sdk::result<void>
			{
				auto supplied = member_text(object, name, name);
				if (!supplied)
					return sdk::unexpected(std::move(supplied.error()));
				target = std::move(*supplied);
				return {};
			};
			for (auto item : {
					 std::pair{&task.toolchain.family, std::string_view{"family"}},
					 std::pair{&task.toolchain.exact_version, std::string_view{"exact_version"}},
					 std::pair{&task.toolchain.target_triple, std::string_view{"target_triple"}},
					 std::pair{&task.toolchain.builtin_headers_digest,
							   std::string_view{"builtin_headers_digest"}},
					 std::pair{&task.toolchain.abi_digest, std::string_view{"abi_digest"}},
					 std::pair{&task.toolchain.plugin_spec_digest,
							   std::string_view{"plugin_spec_digest"}},
				 })
				if (auto assigned = assign_nested(*item.first, **toolchain_value, item.second);
					!assigned)
					return sdk::unexpected(std::move(assigned.error()));
			auto sysroot = nullable_text(**toolchain_value, "sysroot", "toolchain.sysroot");
			if (!sysroot)
				return sdk::unexpected(std::move(sysroot.error()));
			task.toolchain.sysroot = std::move(*sysroot);
			for (auto item : {
					 std::pair{&task.variant_authority.language, std::string_view{"language"}},
					 std::pair{&task.variant_authority.language_standard,
							   std::string_view{"language_standard"}},
					 std::pair{&task.variant_authority.target_triple,
							   std::string_view{"target_triple"}},
					 std::pair{&task.variant_authority.predefined_macros_digest,
							   std::string_view{"predefined_macros_digest"}},
					 std::pair{&task.variant_authority.include_search_digest,
							   std::string_view{"include_search_digest"}},
					 std::pair{&task.variant_authority.semantic_flags_digest,
							   std::string_view{"semantic_flags_digest"}},
				 })
				if (auto assigned = assign_nested(*item.first, **variant_value, item.second);
					!assigned)
					return sdk::unexpected(std::move(assigned.error()));

			for (auto item : {
					 std::pair{&task.source_snapshot, std::string_view{"source_snapshot_id"}},
					 std::pair{&task.file, std::string_view{"file_id"}},
					 std::pair{&task.logical_path, std::string_view{"logical_path"}},
					 std::pair{&task.source_content_digest, std::string_view{"content_digest"}},
					 std::pair{&task.source_encoding, std::string_view{"encoding"}},
					 std::pair{&task.line_index, std::string_view{"line_index_id"}},
					 std::pair{&task.source_content_base64, std::string_view{"content_base64"}},
				 })
				if (auto assigned = assign_nested(*item.first, **source_value, item.second);
					!assigned)
					return sdk::unexpected(std::move(assigned.error()));
			auto source_size = member_unsigned(
				**source_value, "size_bytes", "source.size_bytes", 16U * 1024U * 1024U);
			auto source_read_only = member_boolean(**source_value, "read_only", "source.read_only");
			if (!source_size || !source_read_only)
				return sdk::unexpected(!source_size ? std::move(source_size.error())
													: std::move(source_read_only.error()));
			task.source_size_bytes = *source_size;
			task.source_read_only = *source_read_only;
			auto decoded = decode_base64(task.source_content_base64, 16U * 1024U * 1024U);
			if (!decoded || decoded->size() != task.source_size_bytes ||
				sdk::content_digest(*decoded) != task.source_content_digest)
				return sdk::unexpected(mismatch("task.source", "bytes-size-digest"));
			if (decoded->empty())
				task.source.clear();
			else
				task.source.assign(reinterpret_cast<const char*>(decoded->data()), decoded->size());
			auto expected_file = file_id(task.logical_path);
			auto expected_lines = line_index_id(*decoded);
			if (!expected_file)
				return sdk::unexpected(std::move(expected_file.error()));
			if (!expected_lines)
				return sdk::unexpected(std::move(expected_lines.error()));
			if (task.file != *expected_file)
				return sdk::unexpected(mismatch("task.source.file_id"));
			if (task.line_index != *expected_lines)
				return sdk::unexpected(mismatch("task.source.line_index_id"));

			auto arguments_value = required_member(value, "effective_argv", "effective_argv");
			if (!arguments_value)
				return sdk::unexpected(std::move(arguments_value.error()));
			auto arguments = string_array(**arguments_value, "effective_argv");
			auto descriptors =
				member_string_array(value, "requested_descriptor_ids", "requested_descriptor_ids");
			auto groups = member_string_array(value, "dependency_groups", "dependency_groups");
			if (!arguments || !descriptors || !groups)
				return sdk::unexpected(invalid("task.topology"));
			task.arguments = std::move(*arguments);
			task.requested_descriptors = std::move(*descriptors);
			task.dependency_groups = std::move(*groups);

			constexpr auto signed_max =
				static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
			auto budget_field = [&](const std::string_view name) -> sdk::result<std::uint64_t>
			{
				auto result = member_unsigned(**budget_value, name, name, signed_max);
				if (result && *result == 0U)
					return sdk::unexpected(invalid(std::string{name}, "positive"));
				return result;
			};
			for (auto item : {
					 std::pair{&task.budget.output_bytes, std::string_view{"output_bytes"}},
					 std::pair{&task.budget.rows, std::string_view{"rows"}},
					 std::pair{&task.budget.diagnostics, std::string_view{"diagnostics"}},
					 std::pair{&task.budget.wall_ms, std::string_view{"wall_ms"}},
					 std::pair{&task.budget.cpu_ms, std::string_view{"cpu_ms"}},
					 std::pair{&task.budget.address_space_bytes,
							   std::string_view{"address_space_bytes"}},
					 std::pair{&task.budget.transport_bytes, std::string_view{"transport_bytes"}},
					 std::pair{&task.budget.open_files, std::string_view{"open_files"}},
					 std::pair{&task.budget.subprocesses, std::string_view{"subprocesses"}},
				 })
			{
				auto supplied = budget_field(item.second);
				if (!supplied)
					return sdk::unexpected(std::move(supplied.error()));
				*item.first = *supplied;
			}

			auto sandbox_minimum = member_text(**sandbox_value, "minimum", "sandbox.minimum");
			auto sandbox_digest =
				member_text(**sandbox_value, "policy_digest", "sandbox.policy_digest");
			if (!sandbox_minimum || !sandbox_digest ||
				(*sandbox_minimum != "enforced" && *sandbox_minimum != "certified"))
				return sdk::unexpected(invalid("task.sandbox"));
			task.sandbox = {*sandbox_minimum, *sandbox_digest};
			sdk::provider::sandbox_requirement sandbox{
				*sandbox_minimum == "certified" ? sdk::provider::sandbox_assurance::certified
												: sdk::provider::sandbox_assurance::enforced,
				*sandbox_digest,
			};
			if (auto valid = sandbox.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));

			auto derived_toolchain = toolchain_row(task.toolchain);
			if (!derived_toolchain)
				return sdk::unexpected(std::move(derived_toolchain.error()));
			auto toolchain_id =
				row_string(*derived_toolchain,
						   cxxlens::build::relations::toolchain_context::descriptor(),
						   "toolchain");
			if (!toolchain_id || task.toolchain_context != *toolchain_id)
				return sdk::unexpected(mismatch("task.toolchain_context_id"));
			auto toolchain_object = **toolchain_value;
			auto toolchain_map = *toolchain_object.as_object();
			auto toolchain_id_json = json_string(*toolchain_id);
			if (!toolchain_id_json)
				return sdk::unexpected(std::move(toolchain_id_json.error()));
			toolchain_map.emplace("toolchain", std::move(*toolchain_id_json));
			auto toolchain_row_json = json_object(std::move(toolchain_map));
			if (!toolchain_row_json)
				return sdk::unexpected(std::move(toolchain_row_json.error()));
			auto expected_toolchain_digest =
				base_row_digest("build.toolchain_context.v1", std::move(*toolchain_row_json));
			if (!expected_toolchain_digest || task.toolchain_digest != *expected_toolchain_digest)
				return sdk::unexpected(mismatch("task.toolchain_digest"));

			auto derived_variant =
				variant_row(task.variant_authority, task.project, task.toolchain_context);
			if (!derived_variant)
				return sdk::unexpected(std::move(derived_variant.error()));
			auto variant_id = row_string(
				*derived_variant, cxxlens::build::relations::variant::descriptor(), "variant");
			if (!variant_id || task.variant != *variant_id)
				return sdk::unexpected(mismatch("task.build_variant_id"));
			auto derived_source = source_file_row(task, task.project);
			if (!derived_source)
				return sdk::unexpected(std::move(derived_source.error()));
			auto snapshot_id = row_string(
				*derived_source, cxxlens::source::relations::file::descriptor(), "snapshot");
			if (!snapshot_id || task.source_snapshot != *snapshot_id)
				return sdk::unexpected(mismatch("task.source_snapshot_id"));
			auto derived_compile_unit = compile_unit_row(task);
			if (!derived_compile_unit)
				return sdk::unexpected(std::move(derived_compile_unit.error()));
			auto compile_unit_id = row_string(*derived_compile_unit,
											  cxxlens::build::relations::compile_unit::descriptor(),
											  "compile_unit");
			if (!compile_unit_id || task.compile_unit != *compile_unit_id)
				return sdk::unexpected(mismatch("task.compile_unit_id"));

			auto payload = encode_task_input(task);
			if (!payload)
				return sdk::unexpected(request_error(
					"materialization.task-binding-mismatch", "task.v3", payload.error().detail));
			auto supplied_input_digest =
				member_text(value, "task_input_digest", "task.task_input_digest");
			if (!supplied_input_digest || sdk::content_digest(*payload) != *supplied_input_digest)
				return sdk::unexpected(mismatch("task.task_input_digest"));
			auto semantic_contract =
				member_text(worker, "semantic_contract_digest", "worker.semantic_contract_digest");
			if (!semantic_contract)
				return sdk::unexpected(std::move(semantic_contract.error()));
			auto generic_task = reconstruct_provider_task(
				task, {outputs.begin(), outputs.end()}, *semantic_contract);
			auto supplied_task_id = member_text(value, "provider_task_id", "task.provider_task_id");
			if (!generic_task)
				return sdk::unexpected(
					request_error("materialization.task-binding-mismatch",
								  "task.provider_task_id",
								  generic_task.error().code + ":" + generic_task.error().detail));
			if (!supplied_task_id)
				return sdk::unexpected(std::move(supplied_task_id.error()));
			if (generic_task->task_id != *supplied_task_id)
				return sdk::unexpected(request_error("materialization.task-binding-mismatch",
													 "task.provider_task_id",
													 "expected=" + generic_task->task_id +
														 ";supplied=" + *supplied_task_id));

			auto installed_binary =
				member_text(worker, "installed_binary_digest", "worker.installed_binary_digest");
			if (!installed_binary || !supplied_task_id->starts_with("task:"))
				return sdk::unexpected(mismatch("task.provider_execution_id"));
			auto provider_id = member_text(worker, "provider_id", "worker.provider_id");
			if (!provider_id)
				return sdk::unexpected(std::move(provider_id.error()));
			auto expected_execution =
				identity("provider-execution",
						 {canonical_value::from_string(*provider_id),
						  canonical_value::from_string(*installed_binary),
						  canonical_value::from_string(supplied_task_id->substr(5U)),
						  canonical_value::from_string(*supplied_input_digest)});
			auto supplied_execution =
				member_text(value, "provider_execution_id", "task.provider_execution_id");
			if (!expected_execution || !supplied_execution ||
				*expected_execution != *supplied_execution)
				return sdk::unexpected(mismatch("task.provider_execution_id"));

			return validated_task_request{std::move(task),
										  std::move(*supplied_task_id),
										  std::move(*supplied_execution),
										  std::move(*supplied_input_digest),
										  std::move(sandbox),
										  std::move(*payload)};
		}

		[[nodiscard]] sdk::result<sdk::project_catalog>
		validate_project_catalog(const json_value& project)
		{
			constexpr std::array members{
				std::string_view{"project_id"},
				std::string_view{"catalog_id"},
				std::string_view{"catalog_digest"},
				std::string_view{"logical_root"},
				std::string_view{"catalog_environment_digest"},
				std::string_view{"catalog_compile_unit_census_digest"},
				std::string_view{"catalog_compile_units"},
			};
			if (auto exact = exact_members(project, members, "project"); !exact)
				return sdk::unexpected(std::move(exact.error()));
			auto logical_root = member_text(project, "logical_root", "project.logical_root");
			auto environment = member_text(
				project, "catalog_environment_digest", "project.catalog_environment_digest");
			auto entries_value =
				required_member(project, "catalog_compile_units", "project.catalog_compile_units");
			if (!logical_root || !environment || !entries_value)
				return sdk::unexpected(!logical_root	  ? std::move(logical_root.error())
										   : !environment ? std::move(environment.error())
														  : std::move(entries_value.error()));
			const auto* entries = (*entries_value)->as_array();
			if (entries == nullptr || entries->empty() || entries->size() > 4096U)
				return sdk::unexpected(invalid("project.catalog_compile_units", "cardinality"));

			std::vector<sdk::catalog_compile_unit> units;
			units.reserve(entries->size());
			for (const auto& entry : *entries)
			{
				constexpr std::array entry_members{
					std::string_view{"catalog_compile_unit_id"},
					std::string_view{"effective_invocation_digest"},
					std::string_view{"source_digest"},
					std::string_view{"environment_digest"},
				};
				if (auto exact =
						exact_members(entry, entry_members, "project.catalog_compile_unit");
					!exact)
					return sdk::unexpected(std::move(exact.error()));
				auto id = member_text(entry, "catalog_compile_unit_id", "catalog_compile_unit_id");
				auto invocation = member_text(
					entry, "effective_invocation_digest", "effective_invocation_digest");
				auto source = member_text(entry, "source_digest", "source_digest");
				auto unit_environment =
					member_text(entry, "environment_digest", "environment_digest");
				if (!id || !invocation || !source || !unit_environment)
					return sdk::unexpected(!id				 ? std::move(id.error())
											   : !invocation ? std::move(invocation.error())
											   : !source	 ? std::move(source.error())
															 : std::move(unit_environment.error()));
				units.push_back({std::move(*id),
								 std::move(*invocation),
								 std::move(*source),
								 std::move(*unit_environment)});
			}
			auto catalog = sdk::project_catalog::make(
				std::move(*logical_root), std::move(*environment), units);
			if (!catalog)
				return sdk::unexpected(std::move(catalog.error()));
			if (catalog->compile_units != units)
				return sdk::unexpected(request_error("materialization.catalog-census-mismatch",
													 "catalog_compile_units",
													 "canonical-order-or-duplicate"));

			auto supplied_id = member_text(project, "catalog_id", "project.catalog_id");
			auto supplied_digest = member_text(project, "catalog_digest", "project.catalog_digest");
			if (!supplied_id || !supplied_digest)
				return sdk::unexpected(!supplied_id ? std::move(supplied_id.error())
													: std::move(supplied_digest.error()));
			if (*supplied_id != catalog->catalog_id || *supplied_digest != catalog->catalog_digest)
				return sdk::unexpected(mismatch("project.catalog", "bottom-up-digest"));

			std::vector<canonical_value> census_values;
			census_values.reserve(catalog->compile_units.size());
			for (const auto& unit : catalog->compile_units)
				census_values.push_back(canonical_value::from_string(unit.compile_unit_id));
			auto census =
				semantic_tuple_digest("cxxlens.clang22-catalog-compile-unit-census.v1",
									  canonical_value::from_tuple(std::move(census_values)));
			auto supplied_census = member_text(project,
											   "catalog_compile_unit_census_digest",
											   "project.catalog_compile_unit_census_digest");
			if (!census || !supplied_census)
				return sdk::unexpected(!census ? std::move(census.error())
											   : std::move(supplied_census.error()));
			if (*census != *supplied_census)
				return sdk::unexpected(request_error("materialization.catalog-census-mismatch",
													 "catalog_compile_unit_census_digest"));
			return catalog;
		}

		[[nodiscard]] std::vector<relation_descriptor> exact_output_descriptors()
		{
			return {
				cxxlens::cc::relations::call_direct_target::descriptor(),
				cxxlens::cc::relations::call_site::descriptor(),
				cxxlens::cc::relations::entity::descriptor(),
				call_observation_v2_descriptor(),
				entity_observation_v2_descriptor(),
				type_observation_v2_descriptor(),
			};
		}

		[[nodiscard]] std::vector<relation_descriptor> exact_base_descriptors()
		{
			return {
				cxxlens::build::relations::project::descriptor(),
				cxxlens::build::relations::toolchain_context::descriptor(),
				cxxlens::build::relations::variant::descriptor(),
				cxxlens::source::relations::file::descriptor(),
				cxxlens::build::relations::compile_unit::descriptor(),
				cxxlens::source::relations::span::descriptor(),
			};
		}

		[[nodiscard]] sdk::result<void>
		validate_descriptor_bindings(const json_value& registry_value,
									 const std::vector<relation_descriptor>& base,
									 const std::vector<relation_descriptor>& outputs)
		{
			constexpr std::array registry_members{
				std::string_view{"path"},
				std::string_view{"authority_registry_digest"},
				std::string_view{"base_descriptors"},
				std::string_view{"descriptors"},
			};
			if (auto exact = exact_members(registry_value, registry_members, "registry"); !exact)
				return exact;
			auto path = member_text(registry_value, "path", "registry.path");
			auto authority = member_text(
				registry_value, "authority_registry_digest", "registry.authority_registry_digest");
			if (!path || !authority)
				return sdk::unexpected(!path ? std::move(path.error())
											 : std::move(authority.error()));
			if (*path != "schemas/cxxlens_ng_relation_registry.yaml" ||
				*authority != authority_registry_digest)
				return sdk::unexpected(request_error("materialization.descriptor-binding-mismatch",
													 "registry.authority"));

			const auto validate_array = [&](const std::string_view member_name,
											const std::span<const relation_descriptor> descriptors,
											const bool base_bindings) -> sdk::result<void>
			{
				auto binding_value = required_member(registry_value, member_name, "registry");
				if (!binding_value)
					return sdk::unexpected(std::move(binding_value.error()));
				const auto* bindings = (*binding_value)->as_array();
				if (bindings == nullptr || bindings->size() != descriptors.size())
					return sdk::unexpected(request_error(
						"materialization.descriptor-binding-mismatch", std::string{member_name}));
				for (std::size_t index = 0; index < descriptors.size(); ++index)
				{
					const auto& binding = bindings->at(index);
					constexpr std::array base_members{
						std::string_view{"descriptor_id"},
						std::string_view{"descriptor_version"},
						std::string_view{"contract_digest"},
						std::string_view{"runtime_descriptor_digest"},
						std::string_view{"stage_order"},
						std::string_view{"output_stage"},
						std::string_view{"owner"},
					};
					constexpr std::array output_members{
						std::string_view{"descriptor_id"},
						std::string_view{"descriptor_version"},
						std::string_view{"contract_digest"},
						std::string_view{"runtime_descriptor_digest"},
						std::string_view{"dependency_group_id"},
						std::string_view{"atomic_output_group_id"},
						std::string_view{"batch_id"},
						std::string_view{"output_stage"},
					};
					if ((base_bindings && !binding.has_exact_members(base_members)) ||
						(!base_bindings && !binding.has_exact_members(output_members)))
						return sdk::unexpected(request_error(
							"materialization.descriptor-binding-mismatch", descriptors[index].id));
					auto id = member_text(binding, "descriptor_id", "descriptor_id");
					auto version = member_text(binding, "descriptor_version", "descriptor_version");
					auto contract = member_text(binding, "contract_digest", "contract_digest");
					auto runtime = member_text(
						binding, "runtime_descriptor_digest", "runtime_descriptor_digest");
					if (!id || !version || !contract || !runtime)
						return sdk::unexpected(request_error(
							"materialization.descriptor-binding-mismatch", descriptors[index].id));
					const auto& descriptor = descriptors[index];
					if (*id != descriptor.id || *version != descriptor.version.string() ||
						*contract != descriptor.contract_digest ||
						*runtime != descriptor.descriptor_digest)
						return sdk::unexpected(request_error(
							"materialization.descriptor-binding-mismatch", descriptor.id));
					if (base_bindings)
					{
						auto stage = member_unsigned(binding, "stage_order", "stage_order");
						auto output = member_text(binding, "output_stage", "output_stage");
						auto owner = member_text(binding, "owner", "owner");
						if (!stage || !output || !owner || *stage != index ||
							*output != "canonical_claim" || *owner != "installed-tool")
							return sdk::unexpected(request_error(
								"materialization.descriptor-binding-mismatch", descriptor.id));
					}
					else
					{
						const auto group = index < 3U ? "canonical" : "observation";
						const auto stage = index < 3U ? "canonical_claim" : "assertion";
						auto supplied_group =
							member_text(binding, "dependency_group_id", "dependency_group_id");
						auto atomic = member_text(
							binding, "atomic_output_group_id", "atomic_output_group_id");
						auto batch = member_text(binding, "batch_id", "batch_id");
						auto output = member_text(binding, "output_stage", "output_stage");
						if (!supplied_group || !atomic || !batch || !output ||
							*supplied_group != group || *atomic != "clang22-atomic" ||
							*batch != descriptor.id + "-batch" || *output != stage)
							return sdk::unexpected(request_error(
								"materialization.descriptor-binding-mismatch", descriptor.id));
					}
				}
				return {};
			};

			if (auto valid = validate_array("base_descriptors", base, true); !valid)
				return valid;
			return validate_array("descriptors", outputs, false);
		}

		[[nodiscard]] sdk::result<sdk::relation_engine>
		validate_engine(const json_value& engine_value,
						const json_value& worker,
						const std::vector<relation_descriptor>& base,
						const std::vector<relation_descriptor>& outputs)
		{
			constexpr std::array members{
				std::string_view{"generation_contract"},
				std::string_view{"admitted_descriptors"},
				std::string_view{"engine_registry_digest"},
				std::string_view{"engine_generation_id"},
			};
			if (auto exact = exact_members(engine_value, members, "engine"); !exact)
				return sdk::unexpected(std::move(exact.error()));
			auto generation_contract =
				member_text(engine_value, "generation_contract", "engine.generation_contract");
			auto registry_digest = member_text(
				engine_value, "engine_registry_digest", "engine.engine_registry_digest");
			auto generation_id =
				member_text(engine_value, "engine_generation_id", "engine.engine_generation_id");
			if (!generation_contract || !registry_digest || !generation_id)
				return sdk::unexpected(!generation_contract ? std::move(generation_contract.error())
										   : !registry_digest ? std::move(registry_digest.error())
															  : std::move(generation_id.error()));
			if (*generation_contract != engine_generation_contract)
				return sdk::unexpected(mismatch("engine.generation_contract"));

			sdk::relation_registry registry;
			std::vector<relation_descriptor> all = base;
			all.insert(all.end(), outputs.begin(), outputs.end());
			for (const auto& descriptor : all)
				if (auto added = registry.add(descriptor); !added)
					return sdk::unexpected(std::move(added.error()));
			auto engine = registry.build(*generation_id);
			if (!engine)
				return sdk::unexpected(std::move(engine.error()));
			if (*registry_digest != engine->registry_digest())
				return sdk::unexpected(mismatch("engine.engine_registry_digest"));

			auto admitted_value = required_member(
				engine_value, "admitted_descriptors", "engine.admitted_descriptors");
			if (!admitted_value)
				return sdk::unexpected(std::move(admitted_value.error()));
			const auto* admitted = (*admitted_value)->as_array();
			std::ranges::sort(all, {}, &relation_descriptor::id);
			if (admitted == nullptr || admitted->size() != all.size())
				return sdk::unexpected(mismatch("engine.admitted_descriptors", "cardinality"));
			for (std::size_t index = 0; index < all.size(); ++index)
			{
				constexpr std::array binding_members{
					std::string_view{"descriptor_id"},
					std::string_view{"runtime_descriptor_digest"},
				};
				const auto& binding = admitted->at(index);
				auto id = member_text(binding, "descriptor_id", "admitted.descriptor_id");
				auto digest = member_text(
					binding, "runtime_descriptor_digest", "admitted.runtime_descriptor_digest");
				if (!binding.has_exact_members(binding_members) || !id || !digest ||
					*id != all[index].id || *digest != all[index].descriptor_digest)
					return sdk::unexpected(mismatch("engine.admitted_descriptors", all[index].id));
			}

			auto provider_id = member_text(worker, "provider_id", "worker.provider_id");
			auto provider_version =
				member_text(worker, "provider_version", "worker.provider_version");
			auto semantic_contract =
				member_text(worker, "semantic_contract_digest", "worker.semantic_contract_digest");
			if (!provider_id || !provider_version || !semantic_contract)
				return sdk::unexpected(mismatch("worker"));
			auto expected_generation =
				identity("engine-generation",
						 {canonical_value::from_string(std::string{engine_generation_contract}),
						  canonical_value::from_string(*provider_id),
						  canonical_value::from_string(*provider_version),
						  canonical_value::from_string(*semantic_contract),
						  canonical_value::from_string(*registry_digest)});
			if (!expected_generation || *expected_generation != *generation_id)
				return sdk::unexpected(mismatch("engine.engine_generation_id"));
			return engine;
		}

		[[nodiscard]] sdk::result<std::string>
		validate_interpretation_policy(const json_value& value)
		{
			constexpr std::array members{
				std::string_view{"policy_id"},
				std::string_view{"selected_domain"},
				std::string_view{"interpretation_policy_digest"},
			};
			if (auto exact = exact_members(value, members, "interpretation_policy"); !exact)
				return sdk::unexpected(std::move(exact.error()));
			auto policy = member_text(value, "policy_id", "interpretation_policy.policy_id");
			auto domain =
				member_text(value, "selected_domain", "interpretation_policy.selected_domain");
			auto supplied =
				member_text(value, "interpretation_policy_digest", "interpretation_policy.digest");
			if (!policy || !domain || !supplied || *policy != interpretation_policy_id ||
				*domain != interpretation_domain || !has_semantic_digest_grammar(*supplied))
				return sdk::unexpected(mismatch("interpretation_policy", "binding"));
			auto expected = semantic_tuple_digest(
				interpretation_policy_id,
				canonical_value::from_tuple({canonical_value::from_string(*policy),
											 canonical_value::from_string(*domain)}));
			if (!expected || *expected != *supplied)
				return sdk::unexpected(mismatch("interpretation_policy.digest"));
			return supplied;
		}

		[[nodiscard]] sdk::result<std::string>
		validate_trust_policy(const json_value& value,
							  const json_value& worker,
							  const std::span<const validated_task_request> tasks)
		{
			constexpr std::array members{
				std::string_view{"policy_id"},
				std::string_view{"execution_profile"},
				std::string_view{"provider_id"},
				std::string_view{"provider_version"},
				std::string_view{"semantic_contract_digest"},
				std::string_view{"protocol_major"},
				std::string_view{"protocol_minor"},
				std::string_view{"required_qualification"},
				std::string_view{"worker_sandbox_policy_digest"},
				std::string_view{"task_sandbox_requirements"},
				std::string_view{"trust_policy_digest"},
			};
			if (auto exact = exact_members(value, members, "trust_policy"); !exact)
				return sdk::unexpected(std::move(exact.error()));

			auto policy = member_text(value, "policy_id", "trust_policy.policy_id");
			auto profile =
				member_text(value, "execution_profile", "trust_policy.execution_profile");
			auto provider = member_text(value, "provider_id", "trust_policy.provider_id");
			auto provider_version =
				member_text(value, "provider_version", "trust_policy.provider_version");
			auto semantic = member_text(
				value, "semantic_contract_digest", "trust_policy.semantic_contract_digest");
			auto qualification =
				member_text(value, "required_qualification", "trust_policy.required_qualification");
			auto sandbox_policy = member_text(
				value, "worker_sandbox_policy_digest", "trust_policy.worker_sandbox_policy_digest");
			auto supplied_digest =
				member_text(value, "trust_policy_digest", "trust_policy.trust_policy_digest");
			auto major = member_unsigned(value, "protocol_major", "trust_policy.protocol_major");
			auto minor = member_unsigned(value, "protocol_minor", "trust_policy.protocol_minor");
			if (!policy || !profile || !provider || !provider_version || !semantic ||
				!qualification || !sandbox_policy || !supplied_digest || !major || !minor)
				return sdk::unexpected(invalid("trust_policy", "shape"));

			auto worker_provider = member_text(worker, "provider_id", "worker.provider_id");
			auto worker_version =
				member_text(worker, "provider_version", "worker.provider_version");
			auto worker_semantic =
				member_text(worker, "semantic_contract_digest", "worker.semantic_contract_digest");
			auto worker_sandbox =
				member_text(worker, "sandbox_policy_digest", "worker.sandbox_policy_digest");
			if (!worker_provider || !worker_version || !worker_semantic || !worker_sandbox ||
				*policy != trust_policy_id || *profile != "trust.native-worker" ||
				*provider != *worker_provider || *provider_version != *worker_version ||
				*semantic != *worker_semantic || *major != 1U || *minor != 0U ||
				*qualification != "canonical-semantic-qualified" ||
				*sandbox_policy != *worker_sandbox ||
				!has_semantic_digest_grammar(*supplied_digest))
				return sdk::unexpected(mismatch("trust_policy", "worker-binding"));

			std::set<std::pair<std::string, std::string>> expected_requirements;
			for (const auto& task : tasks)
				expected_requirements.emplace(task.worker_input.sandbox.minimum,
											  task.worker_input.sandbox.policy_digest);
			auto requirements_value = required_member(
				value, "task_sandbox_requirements", "trust_policy.task_sandbox_requirements");
			if (!requirements_value)
				return sdk::unexpected(std::move(requirements_value.error()));
			const auto* requirements = (*requirements_value)->as_array();
			if (requirements == nullptr || requirements->size() != expected_requirements.size() ||
				requirements->empty())
				return sdk::unexpected(mismatch("trust_policy.task_sandbox_requirements"));

			std::vector<canonical_value> requirement_projection;
			requirement_projection.reserve(requirements->size());
			auto expected = expected_requirements.begin();
			constexpr std::array requirement_members{std::string_view{"minimum"},
													 std::string_view{"policy_digest"}};
			for (const auto& requirement : *requirements)
			{
				if (!requirement.has_exact_members(requirement_members))
					return sdk::unexpected(mismatch("trust_policy.task_sandbox_requirements"));
				auto minimum = member_text(requirement, "minimum", "sandbox.minimum");
				auto digest = member_text(requirement, "policy_digest", "sandbox.policy_digest");
				if (!minimum || !digest || expected == expected_requirements.end() ||
					std::pair{*minimum, *digest} != *expected)
					return sdk::unexpected(mismatch("trust_policy.task_sandbox_requirements"));
				requirement_projection.push_back(
					canonical_value::from_tuple({canonical_value::from_string(*minimum),
												 canonical_value::from_string(*digest)}));
				++expected;
			}

			auto expected_digest = semantic_tuple_digest(
				trust_policy_id,
				canonical_value::from_tuple({
					canonical_value::from_string(*policy),
					canonical_value::from_string(*profile),
					canonical_value::from_string(*provider),
					canonical_value::from_string(*provider_version),
					canonical_value::from_string(*semantic),
					canonical_value::from_integer(static_cast<std::int64_t>(*major)),
					canonical_value::from_integer(static_cast<std::int64_t>(*minor)),
					canonical_value::from_string(*qualification),
					canonical_value::from_string(*sandbox_policy),
					canonical_value::from_tuple(std::move(requirement_projection)),
				}));
			if (!expected_digest || *expected_digest != *supplied_digest)
				return sdk::unexpected(mismatch("trust_policy.trust_policy_digest"));
			return supplied_digest;
		}

		[[nodiscard]] bool valid_sqlite_path(const std::string_view value) noexcept
		{
			if (value.empty() || value.front() == '/' || value.contains('\0'))
				return false;
			std::size_t begin{};
			while (begin <= value.size())
			{
				const auto end = value.find('/', begin);
				const auto part = value.substr(
					begin, end == std::string_view::npos ? value.size() - begin : end - begin);
				if (part == "..")
					return false;
				if (end == std::string_view::npos)
					break;
				begin = end + 1U;
			}
			return true;
		}

		[[nodiscard]] sdk::result<validated_publication_request>
		validate_publication(const json_value& value,
							 const sdk::project_catalog& catalog,
							 const std::string_view engine_generation,
							 const std::string_view engine_registry_digest,
							 const std::string_view interpretation_digest,
							 const std::string_view trust_digest,
							 const std::span<const validated_task_request> tasks)
		{
			constexpr std::array members{
				std::string_view{"backend"},
				std::string_view{"selector"},
				std::string_view{"series_id"},
				std::string_view{"genesis"},
				std::string_view{"expected_parent_publication"},
				std::string_view{"sqlite_path"},
				std::string_view{"partial_policy"},
				std::string_view{"transaction_count"},
				std::string_view{"reopen_before_success"},
			};
			if (auto exact = exact_members(value, members, "publication"); !exact)
				return sdk::unexpected(std::move(exact.error()));
			auto backend = member_text(value, "backend", "publication.backend");
			auto series = member_text(value, "series_id", "publication.series_id");
			auto genesis = member_boolean(value, "genesis", "publication.genesis");
			auto parent = nullable_text(
				value, "expected_parent_publication", "publication.expected_parent_publication");
			auto sqlite = nullable_text(value, "sqlite_path", "publication.sqlite_path");
			auto partial = member_text(value, "partial_policy", "publication.partial_policy");
			auto transactions =
				member_unsigned(value, "transaction_count", "publication.transaction_count");
			auto reopen =
				member_boolean(value, "reopen_before_success", "publication.reopen_before_success");
			if (!backend || !series || !genesis || !parent || !sqlite || !partial ||
				!transactions || !reopen || (*backend != "memory" && *backend != "sqlite") ||
				*partial != "forbid" || *transactions != 1U || !*reopen)
				return sdk::unexpected(invalid("publication", "policy"));
			if ((*genesis) != !parent->has_value())
				return sdk::unexpected(request_error("materialization.stale-parent",
													 "publication.expected_parent_publication"));
			if (parent->has_value() && !sdk::validate_strong_id(**parent))
				return sdk::unexpected(
					invalid("publication.expected_parent_publication", "strong-id"));
			if (*backend == "memory")
			{
				if (!*genesis || parent->has_value() || sqlite->has_value())
					return sdk::unexpected(request_error(
						"materialization.store-failure", "publication", "memory-fresh-genesis"));
			}
			else if (!sqlite->has_value() || !valid_sqlite_path(**sqlite))
				return sdk::unexpected(request_error(
					"materialization.store-failure", "publication.sqlite_path", "relative-path"));

			if (tasks.empty())
				return sdk::unexpected(invalid("tasks", "empty"));
			const auto& condition_universe = tasks.front().worker_input.condition_universe;
			if (std::ranges::any_of(tasks,
									[&](const validated_task_request& task)
									{
										return task.worker_input.condition_universe !=
											condition_universe;
									}))
				return sdk::unexpected(mismatch("publication.selector.condition_universe_id"));

			auto selector_value = required_member(value, "selector", "publication.selector");
			if (!selector_value)
				return sdk::unexpected(std::move(selector_value.error()));
			constexpr std::array selector_members{
				std::string_view{"catalog_id"},
				std::string_view{"channel_id"},
				std::string_view{"engine_generation_id"},
				std::string_view{"condition_universe_id"},
				std::string_view{"relation_registry_digest"},
				std::string_view{"interpretation_policy_digest"},
				std::string_view{"trust_policy_digest"},
			};
			if (!(**selector_value).has_exact_members(selector_members))
				return sdk::unexpected(invalid("publication.selector", "member-set"));
			sdk::snapshot_series_selector selector;
			for (auto [target, name] : {
					 std::pair{&selector.catalog_id, std::string_view{"catalog_id"}},
					 std::pair{&selector.channel_id, std::string_view{"channel_id"}},
					 std::pair{&selector.engine_generation_id,
							   std::string_view{"engine_generation_id"}},
					 std::pair{&selector.condition_universe_id,
							   std::string_view{"condition_universe_id"}},
					 std::pair{&selector.relation_registry_digest,
							   std::string_view{"relation_registry_digest"}},
					 std::pair{&selector.interpretation_policy_digest,
							   std::string_view{"interpretation_policy_digest"}},
					 std::pair{&selector.trust_policy_digest,
							   std::string_view{"trust_policy_digest"}},
				 })
			{
				auto supplied = member_text(
					**selector_value, name, std::string{"selector."} + std::string{name});
				if (!supplied)
					return sdk::unexpected(std::move(supplied.error()));
				*target = std::move(*supplied);
			}
			if (auto valid = selector.validate();
				!valid || !sdk::validate_strong_id(selector.channel_id))
				return sdk::unexpected(invalid("publication.selector", "authority"));
			if (selector.catalog_id != catalog.catalog_id ||
				selector.engine_generation_id != engine_generation ||
				selector.condition_universe_id != condition_universe ||
				selector.relation_registry_digest != engine_registry_digest ||
				selector.interpretation_policy_digest != interpretation_digest ||
				selector.trust_policy_digest != trust_digest || selector.id() != *series)
				return sdk::unexpected(mismatch("publication.selector", "cross-binding"));

			return validated_publication_request{std::move(*backend),
												 std::move(selector),
												 std::move(*series),
												 *genesis,
												 std::move(*parent),
												 std::move(*sqlite)};
		}

		[[nodiscard]] sdk::result<void> validate_request_identity(const json_value& root)
		{
			constexpr std::array identity_members{
				std::string_view{"materialization_request_id"},
				std::string_view{"request_digest"},
				std::string_view{"semantic_request_digest"},
			};
			auto request_projection = object_without(root, identity_members);
			if (!request_projection)
				return sdk::unexpected(std::move(request_projection.error()));
			auto expected_request = projection_digest("cxxlens.clang22-materialization-request.v2",
													  *request_projection);

			auto semantic_map = *request_projection->as_object();
			auto publication = semantic_map.find("publication");
			if (publication == semantic_map.end() || publication->second.as_object() == nullptr)
				return sdk::unexpected(invalid("publication", "object"));
			auto semantic_publication = *publication->second.as_object();
			for (const auto name : {std::string_view{"backend"},
									std::string_view{"series_id"},
									std::string_view{"expected_parent_publication"},
									std::string_view{"sqlite_path"}})
				semantic_publication.erase(std::string{name});
			auto semantic_publication_value = json_object(std::move(semantic_publication));
			if (!semantic_publication_value)
				return sdk::unexpected(std::move(semantic_publication_value.error()));
			publication->second = std::move(*semantic_publication_value);
			auto semantic_projection = json_object(std::move(semantic_map));
			if (!semantic_projection)
				return sdk::unexpected(std::move(semantic_projection.error()));
			auto expected_semantic =
				projection_digest("cxxlens.clang22-semantic-request.v2", *semantic_projection);
			auto request_digest = member_text(root, "request_digest", "request.request_digest");
			auto semantic_digest_value =
				member_text(root, "semantic_request_digest", "request.semantic_request_digest");
			auto request_id = member_text(
				root, "materialization_request_id", "request.materialization_request_id");
			if (!expected_request || !expected_semantic || !request_digest ||
				!semantic_digest_value || !request_id || *request_digest != *expected_request ||
				*semantic_digest_value != *expected_semantic ||
				*request_id != "materialization:" + *expected_request)
				return sdk::unexpected(mismatch("request.identity"));
			return {};
		}
	} // namespace

	sdk::result<validated_materialization_request>
	validate_materialization_request(json_document document)
	{
		const auto& root = document.root();
		auto schema = member_text(root, "schema", "request.schema");
		auto version = member_text(root, "request_version", "request.request_version");
		if (!schema || !version)
			return sdk::unexpected(invalid("request.envelope"));
		if (*schema != "cxxlens.clang22-materialization-request.v2")
			return sdk::unexpected(unsupported_version("request.schema"));
		if (*version != "2.0.0")
			return sdk::unexpected(unsupported_version("request.request_version"));

		constexpr std::array root_members{
			std::string_view{"schema"},
			std::string_view{"request_version"},
			std::string_view{"materialization_request_id"},
			std::string_view{"request_digest"},
			std::string_view{"semantic_request_digest"},
			std::string_view{"tool"},
			std::string_view{"worker"},
			std::string_view{"project"},
			std::string_view{"registry"},
			std::string_view{"engine"},
			std::string_view{"interpretation_policy"},
			std::string_view{"trust_policy"},
			std::string_view{"group_topology"},
			std::string_view{"tasks"},
			std::string_view{"publication"},
		};
		if (auto exact = exact_members(root, root_members, "request"); !exact)
			return sdk::unexpected(std::move(exact.error()));
		if (auto machine = validate_tool_worker_topology(root); !machine)
			return sdk::unexpected(std::move(machine.error()));

		auto worker_value = required_member(root, "worker", "worker");
		auto project_value = required_member(root, "project", "project");
		auto registry_value = required_member(root, "registry", "registry");
		auto engine_value = required_member(root, "engine", "engine");
		auto interpretation_value =
			required_member(root, "interpretation_policy", "interpretation_policy");
		auto trust_value = required_member(root, "trust_policy", "trust_policy");
		auto tasks_value = required_member(root, "tasks", "tasks");
		auto publication_value = required_member(root, "publication", "publication");
		if (!worker_value || !project_value || !registry_value || !engine_value ||
			!interpretation_value || !trust_value || !tasks_value || !publication_value)
			return sdk::unexpected(invalid("request"));

		auto catalog = validate_project_catalog(**project_value);
		if (!catalog)
			return sdk::unexpected(std::move(catalog.error()));
		auto derived_project_row = project_row(*catalog);
		auto supplied_project = member_text(**project_value, "project_id", "project.project_id");
		if (!derived_project_row || !supplied_project)
			return sdk::unexpected(!derived_project_row ? std::move(derived_project_row.error())
														: std::move(supplied_project.error()));
		auto derived_project = row_string(
			*derived_project_row, cxxlens::build::relations::project::descriptor(), "project");
		if (!derived_project || *derived_project != *supplied_project)
			return sdk::unexpected(mismatch("project.project_id"));

		auto base = exact_base_descriptors();
		auto outputs = exact_output_descriptors();
		if (auto bindings = validate_descriptor_bindings(**registry_value, base, outputs);
			!bindings)
			return sdk::unexpected(std::move(bindings.error()));
		auto engine = validate_engine(**engine_value, **worker_value, base, outputs);
		if (!engine)
			return sdk::unexpected(std::move(engine.error()));

		const auto* task_array = (*tasks_value)->as_array();
		if (task_array == nullptr || task_array->empty() || task_array->size() > 4096U ||
			task_array->size() != catalog->compile_units.size())
			return sdk::unexpected(
				request_error("materialization.catalog-census-mismatch", "tasks", "cardinality"));

		auto worker_sandbox =
			member_text(**worker_value, "sandbox_policy_digest", "worker.sandbox_policy_digest");
		if (!worker_sandbox)
			return sdk::unexpected(std::move(worker_sandbox.error()));
		std::vector<validated_task_request> tasks;
		tasks.reserve(task_array->size());
		std::set<std::tuple<std::string, std::string, std::string>> execution_keys;
		std::uint64_t aggregate_source_bytes{};
		constexpr std::uint64_t maximum_aggregate_source_bytes = 512U * 1024U * 1024U;
		for (std::size_t index{}; index < task_array->size(); ++index)
		{
			auto task = parse_task(
				task_array->at(index), *catalog, *supplied_project, outputs, **worker_value);
			if (!task)
				return sdk::unexpected(std::move(task.error()));
			if (task->worker_input.selected_catalog_compile_unit !=
				catalog->compile_units[index].compile_unit_id)
				return sdk::unexpected(request_error(
					"materialization.catalog-census-mismatch", "tasks", "selection-order"));
			if (task->worker_input.sandbox.policy_digest != *worker_sandbox)
				return sdk::unexpected(mismatch("task.sandbox.policy_digest"));
			if (aggregate_source_bytes >
				maximum_aggregate_source_bytes - task->worker_input.source_size_bytes)
				return sdk::unexpected(invalid("tasks.source", "aggregate-decoded-limit"));
			aggregate_source_bytes += task->worker_input.source_size_bytes;
			if (!execution_keys
					 .emplace(task->provider_task_id,
							  task->task_input_digest,
							  task->provider_execution_id)
					 .second)
				return sdk::unexpected(mismatch("tasks", "duplicate-execution-tuple"));
			tasks.push_back(std::move(*task));
		}

		auto interpretation_digest = validate_interpretation_policy(**interpretation_value);
		if (!interpretation_digest)
			return sdk::unexpected(std::move(interpretation_digest.error()));
		if (std::ranges::any_of(tasks,
								[](const validated_task_request& task)
								{
									return task.worker_input.interpretation !=
										interpretation_domain;
								}))
			return sdk::unexpected(mismatch("tasks.interpretation_domain"));
		auto trust_digest = validate_trust_policy(**trust_value, **worker_value, tasks);
		if (!trust_digest)
			return sdk::unexpected(std::move(trust_digest.error()));

		auto engine_generation =
			member_text(**engine_value, "engine_generation_id", "engine.engine_generation_id");
		auto engine_registry_digest =
			member_text(**engine_value, "engine_registry_digest", "engine.engine_registry_digest");
		if (!engine_generation || !engine_registry_digest)
			return sdk::unexpected(!engine_generation ? std::move(engine_generation.error())
													  : std::move(engine_registry_digest.error()));
		auto publication = validate_publication(**publication_value,
												*catalog,
												*engine_generation,
												*engine_registry_digest,
												*interpretation_digest,
												*trust_digest,
												tasks);
		if (!publication)
			return sdk::unexpected(std::move(publication.error()));
		if (auto identity_valid = validate_request_identity(root); !identity_valid)
			return sdk::unexpected(std::move(identity_valid.error()));

		return validated_materialization_request{std::move(document),
												 std::move(*engine),
												 std::move(outputs),
												 std::move(tasks),
												 std::move(*publication)};
	}
} // namespace cxxlens::detail::clang22::materialization
