#include "materialization_request_v2_1.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
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
#include "materialization_request_identity.hpp"
#include "materialization_task_spool.hpp"
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

		/** One bounded task window whose global catalog remains external and immutable. */
		struct validated_task_metadata
		{
			clang22_task_input input;
			const sdk::project_catalog* catalog_owner{};
			std::string provider_task_id;
			std::string provider_execution_id;
			std::string task_input_digest;
			sdk::provider::sandbox_requirement sandbox;
		};

		enum class task_metadata_phase : std::uint8_t
		{
			shape,
			source_independent_binding,
		};

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

		[[nodiscard]] bool has_content_digest_grammar(const std::string_view value) noexcept
		{
			constexpr std::string_view prefix{"sha256:"};
			return value.size() == prefix.size() + 64U && value.starts_with(prefix) &&
				lower_hex(value.substr(prefix.size()));
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

		[[nodiscard]] bool infrastructure_failure(const std::string_view code) noexcept
		{
			return code == "materialization.spool-failure" ||
				code == materialization_admission_no_response_code;
		}

		[[nodiscard]] sdk::error task_index_phase_error(sdk::error error,
														const std::string_view phase)
		{
			if (error.code == "materialization.spool-failure")
				return normalize_materialization_admission_spool_failure(
					std::move(error), phase, "task-index");
			return error;
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

		[[nodiscard]] bool strong_id_schema(const std::string_view value) noexcept
		{
			return value.size() <= 2048U && sdk::validate_strong_id(value).has_value();
		}

		[[nodiscard]] bool exact_prefixed_hex(const std::string_view value,
											  const std::string_view prefix) noexcept
		{
			return value.size() == prefix.size() + 64U && value.starts_with(prefix) &&
				lower_hex(value.substr(prefix.size()));
		}

		template <typename Predicate>
		[[nodiscard]] sdk::result<void> schema_text_member(const json_value& value,
														   const std::string_view name,
														   const std::string_view field,
														   Predicate&& predicate)
		{
			auto supplied = member_text(value, name, field);
			if (!supplied || !predicate(std::string_view{*supplied}))
				return sdk::unexpected(!supplied ? std::move(supplied.error())
												 : invalid(std::string{field}, "schema"));
			return {};
		}

		[[nodiscard]] bool unique_json_items(const json_value::array_type& values) noexcept
		{
			for (std::size_t left{}; left < values.size(); ++left)
				for (std::size_t right = left + 1U; right < values.size(); ++right)
					if (values[left] == values[right])
						return false;
			return true;
		}

		[[nodiscard]] sdk::result<const json_value::array_type*>
		schema_array_member(const json_value& value,
							const std::string_view name,
							const std::string_view field,
							const std::size_t minimum,
							const std::size_t maximum,
							const bool unique)
		{
			auto member = required_member(value, name, field);
			if (!member)
				return sdk::unexpected(std::move(member.error()));
			const auto* array = (*member)->as_array();
			if (array == nullptr || array->size() < minimum || array->size() > maximum ||
				(unique && !unique_json_items(*array)))
				return sdk::unexpected(invalid(std::string{field}, "array-cardinality-or-unique"));
			return array;
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

		[[nodiscard]] bool schema_logical_path(const std::string_view value) noexcept
		{
			if (value.empty() || value.size() > 4096U ||
				std::ranges::any_of(value,
									[](const unsigned char byte)
									{
										return byte < 0x20U || byte == 0x7fU;
									}))
				return false;
			for (const auto prefix : {std::string_view{"project://"},
									  std::string_view{"build://"},
									  std::string_view{"toolchain://"},
									  std::string_view{"sysroot://"},
									  std::string_view{"generated://"},
									  std::string_view{"provider://"},
									  std::string_view{"external://"}})
				if (value.starts_with(prefix) && value.size() != prefix.size())
					return true;
			return false;
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
				std::string_view{"occurrence_manifest_digest"},
			};
			constexpr std::array worker_members{
				std::string_view{"executable"},
				std::string_view{"provider_id"},
				std::string_view{"provider_version"},
				std::string_view{"installed_binary_digest"},
				std::string_view{"semantic_contract_digest"},
				std::string_view{"protocol_major"},
				std::string_view{"protocol_minor"},
				std::string_view{"required_features"},
				std::string_view{"sandbox_policy_digest"},
			};
			constexpr std::array topology_members{
				std::string_view{"dependency_groups"},
				std::string_view{"atomic_output_group"},
				std::string_view{"partial_policy"},
			};
			if (!(**tool_value).has_exact_members(tool_members) ||
				!(**worker_value).has_exact_members(worker_members) ||
				!(**topology_value).has_exact_members(topology_members))
				return sdk::unexpected(invalid("request.machine-binding", "member-set"));
			for (const auto& [object, name, expected] : {
					 std::tuple{*tool_value, "executable", "cxxlens-clang22-materialize"},
					 std::tuple{*tool_value, "interface_version", "2.1.0"},
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
			auto required_features = member_string_array(
				**worker_value, "required_features", "worker.required_features");
			if (!protocol_major || !protocol_minor || !required_features || *protocol_major != 1U ||
				*protocol_minor != 1U ||
				*required_features != std::vector<std::string>{"task-input-chunks-v1"})
				return sdk::unexpected(invalid("worker.protocol", "version-or-features"));
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
										   std::string_view{"occurrence_manifest_digest"}}},
					 std::tuple{*worker_value,
								std::string_view{"worker."},
								std::array{std::string_view{"installed_binary_digest"},
										   std::string_view{"semantic_contract_digest"}}},
				 })
				for (const auto name : names)
				{
					auto digest =
						member_text(*object, name, std::string{prefix} + std::string{name});
					if (!digest || !has_digest_grammar(*digest))
						return sdk::unexpected(
							invalid(std::string{prefix} + std::string{name}, "digest"));
				}
			auto sandbox = member_text(
				**worker_value, "sandbox_policy_digest", "worker.sandbox_policy_digest");
			if (!sandbox || !has_digest_grammar(*sandbox))
				return sdk::unexpected(invalid("worker.sandbox_policy_digest", "digest"));
			return {};
		}

		[[nodiscard]] bool valid_sqlite_path(std::string_view value) noexcept;

		[[nodiscard]] sdk::result<void> validate_project_schema(const json_value& project)
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
				return exact;
			for (const auto name : {std::string_view{"project_id"}, std::string_view{"catalog_id"}})
				if (auto valid = schema_text_member(project,
													name,
													std::string{"project."} + std::string{name},
													strong_id_schema);
					!valid)
					return valid;
			for (const auto name : {std::string_view{"catalog_digest"},
									std::string_view{"catalog_environment_digest"}})
				if (auto valid = schema_text_member(project,
													name,
													std::string{"project."} + std::string{name},
													has_digest_grammar);
					!valid)
					return valid;
			if (auto valid = schema_text_member(project,
												"catalog_compile_unit_census_digest",
												"project.catalog_compile_unit_census_digest",
												has_semantic_digest_grammar);
				!valid)
				return valid;
			if (auto valid = schema_text_member(project,
												"logical_root",
												"project.logical_root",
												[](const std::string_view value)
												{
													return value.starts_with("project://") &&
														schema_logical_path(value);
												});
				!valid)
				return valid;

			auto entries = schema_array_member(project,
											   "catalog_compile_units",
											   "project.catalog_compile_units",
											   1U,
											   4096U,
											   false);
			if (!entries)
				return sdk::unexpected(std::move(entries.error()));
			using catalog_entry_key =
				std::tuple<std::string_view, std::string_view, std::string_view, std::string_view>;
			std::vector<catalog_entry_key> exact_catalog_entries;
			exact_catalog_entries.reserve((*entries)->size());
			constexpr std::array entry_members{
				std::string_view{"catalog_compile_unit_id"},
				std::string_view{"effective_invocation_digest"},
				std::string_view{"source_digest"},
				std::string_view{"environment_digest"},
			};
			for (const auto& entry : **entries)
			{
				if (auto exact =
						exact_members(entry, entry_members, "project.catalog_compile_unit");
					!exact)
					return exact;
				if (auto valid = schema_text_member(entry,
													"catalog_compile_unit_id",
													"project.catalog_compile_unit.id",
													strong_id_schema);
					!valid)
					return valid;
				for (const auto name : {std::string_view{"effective_invocation_digest"},
										std::string_view{"source_digest"},
										std::string_view{"environment_digest"}})
					if (auto valid = schema_text_member(
							entry, name, "project.catalog_compile_unit.digest", has_digest_grammar);
						!valid)
						return valid;
				const auto* id = entry.member("catalog_compile_unit_id")->as_string();
				const auto* invocation = entry.member("effective_invocation_digest")->as_string();
				const auto* source = entry.member("source_digest")->as_string();
				const auto* environment = entry.member("environment_digest")->as_string();
				exact_catalog_entries.emplace_back(*id, *invocation, *source, *environment);
			}
			std::ranges::sort(exact_catalog_entries);
			if (std::ranges::adjacent_find(exact_catalog_entries) != exact_catalog_entries.end())
				return sdk::unexpected(
					invalid("project.catalog_compile_units", "array-cardinality-or-unique"));
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_registry_schema(const json_value& registry)
		{
			constexpr std::array members{std::string_view{"path"},
										 std::string_view{"authority_registry_digest"},
										 std::string_view{"base_descriptors"},
										 std::string_view{"descriptors"}};
			if (auto exact = exact_members(registry, members, "registry"); !exact)
				return exact;
			if (auto valid = schema_text_member(registry,
												"path",
												"registry.path",
												[](const std::string_view value)
												{
													return value ==
														"schemas/cxxlens_ng_relation_registry.yaml";
												});
				!valid)
				return valid;
			if (auto valid = schema_text_member(registry,
												"authority_registry_digest",
												"registry.authority_registry_digest",
												has_content_digest_grammar);
				!valid)
				return valid;

			constexpr std::array base_members{
				std::string_view{"descriptor_id"},
				std::string_view{"descriptor_version"},
				std::string_view{"contract_digest"},
				std::string_view{"runtime_descriptor_digest"},
				std::string_view{"stage_order"},
				std::string_view{"output_stage"},
				std::string_view{"owner"},
			};
			constexpr std::array base_ids{
				std::string_view{"build.project.v1"},
				std::string_view{"build.toolchain_context.v1"},
				std::string_view{"build.variant.v1"},
				std::string_view{"source.file.v1"},
				std::string_view{"build.compile_unit.v1"},
				std::string_view{"source.span.v1"},
			};
			auto bases = schema_array_member(
				registry, "base_descriptors", "registry.base_descriptors", 6U, 6U, true);
			if (!bases)
				return sdk::unexpected(std::move(bases.error()));
			for (const auto& binding : **bases)
			{
				if (auto exact = exact_members(binding, base_members, "registry.base_descriptor");
					!exact)
					return exact;
				auto id = member_text(binding, "descriptor_id", "registry.base_descriptor.id");
				auto version =
					member_text(binding, "descriptor_version", "registry.base_descriptor.version");
				auto stage = member_unsigned(
					binding, "stage_order", "registry.base_descriptor.stage_order", 5U);
				auto output =
					member_text(binding, "output_stage", "registry.base_descriptor.output_stage");
				auto owner = member_text(binding, "owner", "registry.base_descriptor.owner");
				if (!id || !version || !stage || !output || !owner ||
					!std::ranges::contains(base_ids, std::string_view{*id}) ||
					*version != "1.0.0" || *output != "canonical_claim" ||
					*owner != "installed-tool")
					return sdk::unexpected(invalid("registry.base_descriptor", "schema"));
				if (auto valid = schema_text_member(binding,
													"contract_digest",
													"registry.base_descriptor.contract_digest",
													has_content_digest_grammar);
					!valid)
					return valid;
				if (auto valid =
						schema_text_member(binding,
										   "runtime_descriptor_digest",
										   "registry.base_descriptor.runtime_descriptor_digest",
										   has_semantic_digest_grammar);
					!valid)
					return valid;
			}

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
			constexpr std::array output_ids{
				std::string_view{"cc.call_direct_target.v1"},
				std::string_view{"cc.call_site.v1"},
				std::string_view{"cc.entity.v1"},
				std::string_view{"frontend.clang22.call_observation.v2"},
				std::string_view{"frontend.clang22.entity_observation.v2"},
				std::string_view{"frontend.clang22.type_observation.v2"},
			};
			auto outputs =
				schema_array_member(registry, "descriptors", "registry.descriptors", 6U, 6U, true);
			if (!outputs)
				return sdk::unexpected(std::move(outputs.error()));
			for (const auto& binding : **outputs)
			{
				if (auto exact = exact_members(binding, output_members, "registry.descriptor");
					!exact)
					return exact;
				auto id = member_text(binding, "descriptor_id", "registry.descriptor.id");
				auto version =
					member_text(binding, "descriptor_version", "registry.descriptor.version");
				auto group =
					member_text(binding, "dependency_group_id", "registry.descriptor.group");
				auto atomic =
					member_text(binding, "atomic_output_group_id", "registry.descriptor.atomic");
				auto batch = member_text(binding, "batch_id", "registry.descriptor.batch");
				auto stage = member_text(binding, "output_stage", "registry.descriptor.stage");
				if (!id || !version || !group || !atomic || !batch || !stage ||
					!std::ranges::contains(output_ids, std::string_view{*id}) ||
					(*version != "1.0.0" && *version != "2.0.0") ||
					(*group != "canonical" && *group != "observation") ||
					*atomic != "clang22-atomic" ||
					!std::ranges::contains(
						output_ids,
						std::string_view{*batch}.substr(
							0U, batch->ends_with("-batch") ? batch->size() - 6U : batch->size())) ||
					!batch->ends_with("-batch") ||
					(*stage != "assertion" && *stage != "canonical_claim"))
					return sdk::unexpected(invalid("registry.descriptor", "schema"));
				if (auto valid = schema_text_member(binding,
													"contract_digest",
													"registry.descriptor.contract_digest",
													has_content_digest_grammar);
					!valid)
					return valid;
				if (auto valid = schema_text_member(binding,
													"runtime_descriptor_digest",
													"registry.descriptor.runtime_descriptor_digest",
													has_semantic_digest_grammar);
					!valid)
					return valid;
			}
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_engine_schema(const json_value& engine)
		{
			constexpr std::array members{std::string_view{"generation_contract"},
										 std::string_view{"admitted_descriptors"},
										 std::string_view{"engine_registry_digest"},
										 std::string_view{"engine_generation_id"}};
			if (auto exact = exact_members(engine, members, "engine"); !exact)
				return exact;
			if (auto valid = schema_text_member(engine,
												"generation_contract",
												"engine.generation_contract",
												[](const std::string_view value)
												{
													return value == engine_generation_contract;
												});
				!valid)
				return valid;
			if (auto valid = schema_text_member(engine,
												"engine_registry_digest",
												"engine.engine_registry_digest",
												has_semantic_digest_grammar);
				!valid)
				return valid;
			if (auto valid = schema_text_member(engine,
												"engine_generation_id",
												"engine.engine_generation_id",
												[](const std::string_view value)
												{
													return exact_prefixed_hex(
														value, "engine-generation:sha256:");
												});
				!valid)
				return valid;
			constexpr std::array ids{
				std::string_view{"build.compile_unit.v1"},
				std::string_view{"build.project.v1"},
				std::string_view{"build.toolchain_context.v1"},
				std::string_view{"build.variant.v1"},
				std::string_view{"cc.call_direct_target.v1"},
				std::string_view{"cc.call_site.v1"},
				std::string_view{"cc.entity.v1"},
				std::string_view{"frontend.clang22.call_observation.v2"},
				std::string_view{"frontend.clang22.entity_observation.v2"},
				std::string_view{"frontend.clang22.type_observation.v2"},
				std::string_view{"source.file.v1"},
				std::string_view{"source.span.v1"},
			};
			auto admitted = schema_array_member(engine,
												"admitted_descriptors",
												"engine.admitted_descriptors",
												ids.size(),
												ids.size(),
												true);
			if (!admitted)
				return sdk::unexpected(std::move(admitted.error()));
			constexpr std::array binding_members{std::string_view{"descriptor_id"},
												 std::string_view{"runtime_descriptor_digest"}};
			for (std::size_t index{}; index < ids.size(); ++index)
			{
				const auto& binding = (**admitted)[index];
				if (auto exact =
						exact_members(binding, binding_members, "engine.admitted_descriptor");
					!exact)
					return exact;
				if (auto valid = schema_text_member(binding,
													"descriptor_id",
													"engine.admitted_descriptor.id",
													[&](const std::string_view value)
													{
														return value == ids[index];
													});
					!valid)
					return valid;
				if (auto valid = schema_text_member(binding,
													"runtime_descriptor_digest",
													"engine.admitted_descriptor.digest",
													has_semantic_digest_grammar);
					!valid)
					return valid;
			}
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_interpretation_schema(const json_value& value)
		{
			constexpr std::array members{std::string_view{"policy_id"},
										 std::string_view{"selected_domain"},
										 std::string_view{"interpretation_policy_digest"}};
			if (auto exact = exact_members(value, members, "interpretation_policy"); !exact)
				return exact;
			if (auto valid = schema_text_member(value,
												"policy_id",
												"interpretation_policy.policy_id",
												[](const std::string_view item)
												{
													return item == interpretation_policy_id;
												});
				!valid)
				return valid;
			if (auto valid = schema_text_member(value,
												"selected_domain",
												"interpretation_policy.selected_domain",
												[](const std::string_view item)
												{
													return item == interpretation_domain;
												});
				!valid)
				return valid;
			return schema_text_member(value,
									  "interpretation_policy_digest",
									  "interpretation_policy.digest",
									  has_semantic_digest_grammar);
		}

		[[nodiscard]] sdk::result<void> validate_trust_schema(const json_value& value)
		{
			constexpr std::array members{
				std::string_view{"policy_id"},
				std::string_view{"execution_profile"},
				std::string_view{"provider_id"},
				std::string_view{"provider_version"},
				std::string_view{"semantic_contract_digest"},
				std::string_view{"protocol_major"},
				std::string_view{"protocol_minor"},
				std::string_view{"required_features"},
				std::string_view{"required_qualification"},
				std::string_view{"worker_sandbox_policy_digest"},
				std::string_view{"task_sandbox_requirements"},
				std::string_view{"trust_policy_digest"},
			};
			if (auto exact = exact_members(value, members, "trust_policy"); !exact)
				return exact;
			for (const auto& [name, expected] : {
					 std::pair{std::string_view{"policy_id"}, trust_policy_id},
					 std::pair{std::string_view{"execution_profile"},
							   std::string_view{"trust.native-worker"}},
					 std::pair{std::string_view{"provider_id"},
							   std::string_view{"cxxlens.clang22.reference"}},
					 std::pair{std::string_view{"provider_version"}, std::string_view{"1.0.0"}},
					 std::pair{std::string_view{"required_qualification"},
							   std::string_view{"canonical-semantic-qualified"}},
				 })
				if (auto valid =
						schema_text_member(value,
										   name,
										   std::string{"trust_policy."} + std::string{name},
										   [&](const std::string_view item)
										   {
											   return item == expected;
										   });
					!valid)
					return valid;
			for (const auto name : {std::string_view{"semantic_contract_digest"},
									std::string_view{"worker_sandbox_policy_digest"}})
				if (auto valid =
						schema_text_member(value,
										   name,
										   std::string{"trust_policy."} + std::string{name},
										   has_digest_grammar);
					!valid)
					return valid;
			if (auto valid = schema_text_member(value,
												"trust_policy_digest",
												"trust_policy.trust_policy_digest",
												has_semantic_digest_grammar);
				!valid)
				return valid;
			auto major = member_unsigned(value, "protocol_major", "trust_policy.protocol_major");
			auto minor = member_unsigned(value, "protocol_minor", "trust_policy.protocol_minor");
			auto features =
				member_string_array(value, "required_features", "trust_policy.required_features");
			if (!major || !minor || !features || *major != 1U || *minor != 1U ||
				*features != std::vector<std::string>{"task-input-chunks-v1"})
				return sdk::unexpected(invalid("trust_policy.protocol", "schema"));
			const auto* requirements_value = value.member("task_sandbox_requirements");
			if (requirements_value != nullptr && requirements_value->as_array() != nullptr &&
				requirements_value->as_array()->size() > 4096U)
				return sdk::unexpected(
					invalid("request-schema", "trust_policy.task_sandbox_requirements:maxItems"));
			auto requirements = schema_array_member(value,
													"task_sandbox_requirements",
													"trust_policy.task_sandbox_requirements",
													1U,
													4096U,
													false);
			if (!requirements)
				return sdk::unexpected(std::move(requirements.error()));
			std::vector<std::pair<std::string_view, std::string_view>> exact_requirements;
			exact_requirements.reserve((*requirements)->size());
			constexpr std::array requirement_members{std::string_view{"minimum"},
													 std::string_view{"policy_digest"}};
			for (const auto& requirement : **requirements)
			{
				if (auto exact = exact_members(
						requirement, requirement_members, "trust_policy.task_sandbox_requirement");
					!exact)
					return exact;
				if (auto valid = schema_text_member(requirement,
													"minimum",
													"trust_policy.task_sandbox_requirement.minimum",
													[](const std::string_view item)
													{
														return item == "enforced" ||
															item == "certified";
													});
					!valid)
					return valid;
				if (auto valid =
						schema_text_member(requirement,
										   "policy_digest",
										   "trust_policy.task_sandbox_requirement.policy_digest",
										   has_digest_grammar);
					!valid)
					return valid;
				exact_requirements.emplace_back(*requirement.member("minimum")->as_string(),
												*requirement.member("policy_digest")->as_string());
			}
			std::ranges::sort(exact_requirements);
			if (std::ranges::adjacent_find(exact_requirements) != exact_requirements.end())
				return sdk::unexpected(invalid("trust_policy.task_sandbox_requirements",
											   "array-cardinality-or-unique"));
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_publication_schema(const json_value& value)
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
				return exact;
			auto backend = member_text(value, "backend", "publication.backend");
			auto genesis = member_boolean(value, "genesis", "publication.genesis");
			auto parent = nullable_text(
				value, "expected_parent_publication", "publication.expected_parent_publication");
			auto sqlite = nullable_text(value, "sqlite_path", "publication.sqlite_path");
			auto partial = member_text(value, "partial_policy", "publication.partial_policy");
			auto transactions =
				member_unsigned(value, "transaction_count", "publication.transaction_count");
			auto reopen =
				member_boolean(value, "reopen_before_success", "publication.reopen_before_success");
			if (!backend || !genesis || !parent || !sqlite || !partial || !transactions ||
				!reopen || (*backend != "memory" && *backend != "sqlite") || *partial != "forbid" ||
				*transactions != 1U || !*reopen ||
				(parent->has_value() && !strong_id_schema(**parent)) ||
				(sqlite->has_value() && !valid_sqlite_path(**sqlite)) ||
				(*backend == "memory" &&
				 (!*genesis || parent->has_value() || sqlite->has_value())) ||
				(*backend == "sqlite" && !sqlite->has_value()) ||
				(*genesis && parent->has_value()) || (!*genesis && !parent->has_value()))
				return sdk::unexpected(invalid("publication", "schema"));
			if (auto valid = schema_text_member(value,
												"series_id",
												"publication.series_id",
												[](const std::string_view item)
												{
													return exact_prefixed_hex(
														item, "snapshot-series:sha256:");
												});
				!valid)
				return valid;
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
			if (auto exact =
					exact_members(**selector_value, selector_members, "publication.selector");
				!exact)
				return exact;
			for (const auto name : {std::string_view{"catalog_id"},
									std::string_view{"channel_id"},
									std::string_view{"condition_universe_id"}})
				if (auto valid =
						schema_text_member(**selector_value,
										   name,
										   std::string{"publication.selector."} + std::string{name},
										   strong_id_schema);
					!valid)
					return valid;
			if (auto valid = schema_text_member(**selector_value,
												"engine_generation_id",
												"publication.selector.engine_generation_id",
												[](const std::string_view item)
												{
													return exact_prefixed_hex(
														item, "engine-generation:sha256:");
												});
				!valid)
				return valid;
			for (const auto name : {std::string_view{"relation_registry_digest"},
									std::string_view{"interpretation_policy_digest"},
									std::string_view{"trust_policy_digest"}})
				if (auto valid =
						schema_text_member(**selector_value,
										   name,
										   std::string{"publication.selector."} + std::string{name},
										   has_semantic_digest_grammar);
					!valid)
					return valid;
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_selected_v2_1_global_schema(const json_value& root)
		{
			if (auto valid = validate_tool_worker_topology(root); !valid)
				return valid;
			for (const auto& [name, validator] : {
					 std::pair{std::string_view{"project"}, &validate_project_schema},
					 std::pair{std::string_view{"registry"}, &validate_registry_schema},
					 std::pair{std::string_view{"engine"}, &validate_engine_schema},
					 std::pair{std::string_view{"interpretation_policy"},
							   &validate_interpretation_schema},
					 std::pair{std::string_view{"trust_policy"}, &validate_trust_schema},
					 std::pair{std::string_view{"publication"}, &validate_publication_schema},
				 })
			{
				auto member = required_member(root, name, name);
				if (!member)
					return sdk::unexpected(std::move(member.error()));
				if (auto valid = validator(**member); !valid)
					return valid;
			}
			return {};
		}

		[[nodiscard]] sdk::result<validated_task_metadata>
		parse_task(const json_value& value,
				   const sdk::project_catalog* const catalog,
				   const std::string_view project_id,
				   const std::span<const relation_descriptor> outputs,
				   const task_metadata_phase phase)
		{
			if ((phase == task_metadata_phase::source_independent_binding) != (catalog != nullptr))
				return sdk::unexpected(invalid("task", "metadata-catalog-owner"));
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
			if (phase == task_metadata_phase::source_independent_binding &&
				task.project != project_id)
				return sdk::unexpected(mismatch("task.project_id"));
			for (const auto field : {
					 std::string_view{"catalog_id"},
					 std::string_view{"catalog_digest"},
				 })
			{
				auto supplied = member_text(value, field, field);
				std::string_view expected;
				if (catalog != nullptr)
					expected = field == "catalog_id" ? std::string_view{catalog->catalog_id}
													 : std::string_view{catalog->catalog_digest};
				if (!supplied)
					return sdk::unexpected(std::move(supplied.error()));
				if ((field == "catalog_id" && !strong_id_schema(*supplied)) ||
					(field == "catalog_digest" && !has_digest_grammar(*supplied)))
					return sdk::unexpected(
						invalid(std::string{"task."} + std::string{field}, "schema"));
				if (phase == task_metadata_phase::source_independent_binding &&
					*supplied != expected)
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
			if (!task.source_content_base64.empty() ||
				!has_digest_grammar(task.source_content_digest) ||
				!task.line_index.starts_with("line-index:sha256:") ||
				task.line_index.size() != std::string_view{"line-index:sha256:"}.size() + 64U ||
				!lower_hex(std::string_view{task.line_index}.substr(
					std::string_view{"line-index:sha256:"}.size())))
				return sdk::unexpected(invalid("task.source", "metadata-binding"));
			if (phase == task_metadata_phase::source_independent_binding)
			{
				auto expected_file = file_id(task.logical_path);
				if (!expected_file)
					return sdk::unexpected(std::move(expected_file.error()));
				if (task.file != *expected_file)
					return sdk::unexpected(mismatch("task.source.file_id"));
			}

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
				return sdk::unexpected(invalid("task.sandbox", valid.error().detail));

			auto supplied_input_digest =
				member_text(value, "task_input_digest", "task.task_input_digest");
			auto supplied_task_id = member_text(value, "provider_task_id", "task.provider_task_id");
			auto supplied_execution =
				member_text(value, "provider_execution_id", "task.provider_execution_id");
			constexpr std::string_view task_prefix{"task:semantic-v2:sha256:"};
			constexpr std::string_view file_prefix{"file:sha256:"};
			constexpr std::string_view line_prefix{"line-index:sha256:"};
			const auto strong = [](const std::string_view item)
			{
				return item.size() <= 2048U && sdk::validate_strong_id(item).has_value();
			};
			for (const auto item : {
					 std::string_view{task.selected_catalog_compile_unit},
					 std::string_view{task.compile_unit},
					 std::string_view{task.project},
					 std::string_view{task.variant},
					 std::string_view{task.toolchain_context},
					 std::string_view{task.language},
					 std::string_view{task.condition_universe},
					 std::string_view{task.condition},
					 std::string_view{task.source_snapshot},
					 std::string_view{task.file},
					 std::string_view{task.line_index},
					 std::string_view{task.toolchain.family},
					 std::string_view{task.toolchain.exact_version},
					 std::string_view{task.toolchain.target_triple},
					 std::string_view{task.variant_authority.language},
					 std::string_view{task.variant_authority.language_standard},
					 std::string_view{task.variant_authority.target_triple},
				 })
				if (!strong(item))
					return sdk::unexpected(invalid("task.identity", "strong-id"));
			for (const auto digest : {
					 std::string_view{task.toolchain_digest},
					 std::string_view{task.normalized_invocation_digest},
					 std::string_view{task.environment_digest},
					 std::string_view{task.source_content_digest},
					 std::string_view{task.toolchain.builtin_headers_digest},
					 std::string_view{task.toolchain.abi_digest},
					 std::string_view{task.toolchain.plugin_spec_digest},
					 std::string_view{task.variant_authority.predefined_macros_digest},
					 std::string_view{task.variant_authority.include_search_digest},
					 std::string_view{task.variant_authority.semantic_flags_digest},
					 std::string_view{task.sandbox.policy_digest},
				 })
				if (!has_digest_grammar(digest))
					return sdk::unexpected(invalid("task.digest", "grammar"));
			if (!supplied_input_digest || !has_content_digest_grammar(*supplied_input_digest) ||
				!supplied_task_id || supplied_task_id->size() != task_prefix.size() + 64U ||
				!supplied_task_id->starts_with(task_prefix) ||
				!lower_hex(std::string_view{*supplied_task_id}.substr(task_prefix.size())) ||
				!supplied_execution || !strong(*supplied_execution) ||
				task.file.size() != file_prefix.size() + 64U ||
				!task.file.starts_with(file_prefix) ||
				!lower_hex(std::string_view{task.file}.substr(file_prefix.size())) ||
				task.line_index.size() != line_prefix.size() + 64U ||
				!task.line_index.starts_with(line_prefix) ||
				!lower_hex(std::string_view{task.line_index}.substr(line_prefix.size())))
				return sdk::unexpected(invalid("task.authority", "grammar"));
			if (!task.source_content_base64.empty() ||
				!schema_logical_path(task.working_directory) ||
				!task.logical_path.starts_with("project://") ||
				!schema_logical_path(task.logical_path) ||
				(task.toolchain.sysroot && !schema_logical_path(*task.toolchain.sysroot)) ||
				(task.source_encoding != "utf8" && task.source_encoding != "utf16le" &&
				 task.source_encoding != "utf16be" && task.source_encoding != "locale_dependent" &&
				 task.source_encoding != "binary_or_unknown") ||
				task.interpretation != interpretation_domain || task.arguments.empty() ||
				task.arguments.size() > 4096U ||
				std::ranges::any_of(task.arguments,
									[&](const std::string& argument)
									{
										return !strong(argument);
									}))
				return sdk::unexpected(invalid("task", "schema-domain"));
			std::vector<std::string> expected_output_ids;
			expected_output_ids.reserve(outputs.size());
			for (const auto& output : outputs)
				expected_output_ids.push_back(output.id);
			if (task.requested_descriptors != expected_output_ids ||
				task.dependency_groups != std::vector<std::string>{"canonical", "observation"})
				return sdk::unexpected(invalid("task.topology", "exact-order"));
			if (phase == task_metadata_phase::shape)
				return validated_task_metadata{std::move(task),
											   nullptr,
											   std::move(*supplied_task_id),
											   std::move(*supplied_execution),
											   std::move(*supplied_input_digest),
											   std::move(sandbox)};

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
			std::vector<canonical_value> argument_projection;
			argument_projection.reserve(task.arguments.size());
			for (const auto& argument : task.arguments)
				argument_projection.push_back(canonical_value::from_string(argument));
			auto invocation = semantic_tuple_digest(
				"cxxlens.clang22.effective-invocation.v1",
				canonical_value::from_tuple({
					canonical_value::from_string("cxxlens.clang22.effective-invocation.v1"),
					canonical_value::from_string(task.working_directory),
					canonical_value::from_tuple(std::move(argument_projection)),
				}));
			if (!invocation || *invocation != task.normalized_invocation_digest)
				return sdk::unexpected(mismatch("task.normalized_invocation_digest"));
			const auto selected = std::ranges::find(catalog->compile_units,
													task.selected_catalog_compile_unit,
													&sdk::catalog_compile_unit::compile_unit_id);
			if (selected == catalog->compile_units.end() ||
				selected->effective_invocation_digest != task.normalized_invocation_digest ||
				selected->source_digest != task.source_content_digest ||
				selected->environment_digest != task.environment_digest)
				return sdk::unexpected(
					mismatch("task.selected_catalog_compile_unit_id", "entry-payload"));
			return validated_task_metadata{std::move(task),
										   catalog,
										   std::move(*supplied_task_id),
										   std::move(*supplied_execution),
										   std::move(*supplied_input_digest),
										   std::move(sandbox)};
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
			if (logical_root->size() > 4096U || !logical_root->starts_with("project://") ||
				!schema_logical_path(*logical_root) || !has_digest_grammar(*environment))
				return sdk::unexpected(invalid("project", "schema-domain"));
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
					return sdk::unexpected(!id ? std::move(id.error())
											   : !invocation ? std::move(invocation.error())
											   : !source	 ? std::move(source.error())
															 : std::move(unit_environment.error()));
				if (id->size() > 2048U || !sdk::validate_strong_id(*id) ||
					!has_digest_grammar(*invocation) || !has_digest_grammar(*source) ||
					!has_digest_grammar(*unit_environment))
					return sdk::unexpected(
						invalid("project.catalog_compile_unit", "schema-domain"));
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
			if (!census || !supplied_census || !has_semantic_digest_grammar(*supplied_census))
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

		[[nodiscard]] sdk::result<std::string> validate_trust_policy(
			const json_value& value,
			const materialization_v2_1_worker_authority& worker,
			const std::set<std::pair<std::string, std::string>>& expected_requirements)
		{
			constexpr std::array members{
				std::string_view{"policy_id"},
				std::string_view{"execution_profile"},
				std::string_view{"provider_id"},
				std::string_view{"provider_version"},
				std::string_view{"semantic_contract_digest"},
				std::string_view{"protocol_major"},
				std::string_view{"protocol_minor"},
				std::string_view{"required_features"},
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
			auto features =
				member_string_array(value, "required_features", "trust_policy.required_features");
			if (!policy || !profile || !provider || !provider_version || !semantic ||
				!qualification || !sandbox_policy || !supplied_digest || !major || !minor ||
				!features)
				return sdk::unexpected(invalid("trust_policy", "shape"));

			if (*policy != trust_policy_id || *profile != "trust.native-worker" ||
				*provider != worker.provider_id || *provider_version != worker.provider_version ||
				*semantic != worker.semantic_contract_digest || *major != 1U || *minor != 1U ||
				*features != std::vector<std::string>{"task-input-chunks-v1"} ||
				*features != worker.required_features ||
				*qualification != "canonical-semantic-qualified" ||
				*sandbox_policy != worker.sandbox_policy_digest ||
				!has_semantic_digest_grammar(*supplied_digest))
				return sdk::unexpected(mismatch("trust_policy", "worker-binding"));

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
					canonical_value::from_tuple(
						{canonical_value::from_string("task-input-chunks-v1")}),
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
			if (value.empty() || value.size() > 4095U || value.front() == '/' ||
				value.contains('\0') || value.contains('\\') ||
				(value.size() >= 2U &&
				 ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z')) &&
				 value[1] == ':'))
				return false;
			std::size_t begin{};
			while (begin <= value.size())
			{
				const auto end = value.find('/', begin);
				const auto part = value.substr(
					begin, end == std::string_view::npos ? value.size() - begin : end - begin);
				if (part.empty() || part == "." || part == "..")
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
							 const std::string_view condition_universe)
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

			if (condition_universe.empty())
				return sdk::unexpected(invalid("tasks", "empty-condition-universe"));

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

		[[nodiscard]] sdk::result<materialization_v2_1_tool_authority>
		tool_authority(const json_value& root)
		{
			auto value = required_member(root, "tool", "tool");
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			materialization_v2_1_tool_authority output;
			for (auto item : {
					 std::pair{&output.executable, std::string_view{"executable"}},
					 std::pair{&output.interface_version, std::string_view{"interface_version"}},
					 std::pair{&output.distribution_version,
							   std::string_view{"distribution_version"}},
					 std::pair{&output.source_revision, std::string_view{"source_revision"}},
					 std::pair{&output.source_tree, std::string_view{"source_tree"}},
					 std::pair{&output.installed_executable_digest,
							   std::string_view{"installed_executable_digest"}},
					 std::pair{&output.package_configuration,
							   std::string_view{"package_configuration"}},
					 std::pair{&output.occurrence_manifest_digest,
							   std::string_view{"occurrence_manifest_digest"}},
				 })
			{
				auto decoded = member_text(**value, item.second, "tool");
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				*item.first = std::move(*decoded);
			}
			return output;
		}

		[[nodiscard]] sdk::result<materialization_v2_1_worker_authority>
		worker_authority(const json_value& root)
		{
			auto value = required_member(root, "worker", "worker");
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			materialization_v2_1_worker_authority output;
			for (auto item : {
					 std::pair{&output.executable, std::string_view{"executable"}},
					 std::pair{&output.provider_id, std::string_view{"provider_id"}},
					 std::pair{&output.provider_version, std::string_view{"provider_version"}},
					 std::pair{&output.installed_binary_digest,
							   std::string_view{"installed_binary_digest"}},
					 std::pair{&output.semantic_contract_digest,
							   std::string_view{"semantic_contract_digest"}},
					 std::pair{&output.sandbox_policy_digest,
							   std::string_view{"sandbox_policy_digest"}},
				 })
			{
				auto decoded = member_text(**value, item.second, "worker");
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				*item.first = std::move(*decoded);
			}
			auto major = member_unsigned(**value, "protocol_major", "worker.protocol_major");
			auto minor = member_unsigned(**value, "protocol_minor", "worker.protocol_minor");
			auto features =
				member_string_array(**value, "required_features", "worker.required_features");
			if (!major || !minor || !features)
				return sdk::unexpected(invalid("worker", "protocol"));
			output.protocol_major = *major;
			output.protocol_minor = *minor;
			output.required_features = std::move(*features);
			return output;
		}

		[[nodiscard]] materialization_v2_1_task_metadata_receipt
		metadata_receipt(const validated_task_metadata& task, const std::uint64_t task_index)
		{
			const auto& input = task.input;
			return {
				task_index,
				input.project,
				input.selected_catalog_compile_unit,
				input.compile_unit,
				input.source_snapshot,
				input.file,
				input.logical_path,
				input.source_content_digest,
				input.source_size_bytes,
				input.source_encoding,
				input.line_index,
				input.source_read_only,
				input.condition_universe,
				input.condition,
				input.interpretation,
				task.provider_task_id,
				task.provider_execution_id,
				task.task_input_digest,
				task.sandbox,
			};
		}

		[[nodiscard]] sdk::result<void> verify_replay_range(materialization_replayable_spool& spool,
															const std::uint64_t offset,
															const std::uint64_t size)
		{
			if (!spool.sealed() || offset > spool.size_bytes() ||
				size > spool.size_bytes() - offset)
				return sdk::unexpected(materialization_admission_no_response());
			std::array<std::byte, default_stream_chunk_bytes> buffer{};
			std::uint64_t consumed{};
			while (consumed != size)
			{
				const auto remaining = size - consumed;
				auto destination = std::span{buffer}.first(
					static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size())));
				auto read = spool.read_at(offset + consumed, destination);
				if (!read)
					return sdk::unexpected(materialization_admission_io_failure(
						read.error(), "request-schema", "raw-replay:read"));
				if (*read == 0U || *read > destination.size())
					return sdk::unexpected(materialization_admission_no_response());
				consumed += *read;
			}
			return {};
		}

		[[nodiscard]] sdk::result<validated_task_metadata>
		replay_task_metadata(materialization_replayable_spool& raw_request,
							 materialization_request_task_index& task_index,
							 const std::uint64_t index,
							 const sdk::project_catalog* const catalog,
							 const std::string_view project_id,
							 const std::span<const relation_descriptor> outputs,
							 const task_metadata_phase phase)
		{
			auto span = task_index.at(index);
			if (!span)
				return sdk::unexpected(task_index_phase_error(
					std::move(span.error()),
					phase == task_metadata_phase::shape ? "request-schema" : "request-binding"));
			const auto replay_phase = phase == task_metadata_phase::shape
				? std::string_view{"request-schema"}
				: std::string_view{"request-binding"};
			auto metadata = replay_materialization_task_metadata(
				raw_request,
				*span,
				maximum_materialization_task_metadata_window_bytes,
				replay_phase);
			if (!metadata)
				return sdk::unexpected(std::move(metadata.error()));
			return parse_task(metadata->root(), catalog, project_id, outputs, phase);
		}

		[[nodiscard]] std::array<std::byte, 8U> framed_u64(const std::uint64_t value) noexcept
		{
			std::array<std::byte, 8U> bytes{};
			for (std::size_t index{}; index < bytes.size(); ++index)
				bytes[index] = static_cast<std::byte>(
					(value >> (56U - static_cast<unsigned>(index * 8U))) & 0xffU);
			return bytes;
		}

		[[nodiscard]] constexpr std::string_view
		auxiliary_phase(const materialization_v2_1_auxiliary_spool_purpose purpose) noexcept
		{
			switch (purpose)
			{
				case materialization_v2_1_auxiliary_spool_purpose::task_unique_index:
				case materialization_v2_1_auxiliary_spool_purpose::task_collision_metadata:
					return "request-schema";
				case materialization_v2_1_auxiliary_spool_purpose::execution_unique_index:
				case materialization_v2_1_auxiliary_spool_purpose::task_input_digest:
					return "request-binding";
			}
			return "request-binding";
		}

		[[nodiscard]] constexpr std::string_view
		auxiliary_name(const materialization_v2_1_auxiliary_spool_purpose purpose) noexcept
		{
			switch (purpose)
			{
				case materialization_v2_1_auxiliary_spool_purpose::task_unique_index:
					return "task-unique-index";
				case materialization_v2_1_auxiliary_spool_purpose::task_collision_metadata:
					return "task-collision-metadata";
				case materialization_v2_1_auxiliary_spool_purpose::execution_unique_index:
					return "execution-unique-index";
				case materialization_v2_1_auxiliary_spool_purpose::task_input_digest:
					return "task-input";
			}
			return "auxiliary-index";
		}

		[[nodiscard]] sdk::error
		auxiliary_create_failure(const materialization_v2_1_auxiliary_spool_purpose purpose,
								 const materialization_io_failure& failure)
		{
			return materialization_admission_io_failure(failure,
														std::string{auxiliary_phase(purpose)},
														std::string{auxiliary_name(purpose)} +
															":create");
		}

		[[nodiscard]] sdk::error
		auxiliary_io_failure(const materialization_v2_1_auxiliary_spool_purpose purpose,
							 const std::string_view operation,
							 const materialization_io_failure& failure)
		{
			return materialization_admission_io_failure(failure,
														std::string{auxiliary_phase(purpose)},
														std::string{auxiliary_name(purpose)} + ":" +
															std::string{operation});
		}

		class production_auxiliary_spool_factory final
			: public materialization_v2_1_auxiliary_spool_factory
		{
		  public:
			materialization_io_result<std::unique_ptr<materialization_replayable_spool>>
			create(const materialization_v2_1_auxiliary_spool_purpose) override
			{
				return make_materialization_private_spool();
			}
		};

		[[nodiscard]] sdk::result<void>
		append_record_bytes(materialization_private_spool& spool,
							const std::span<const std::byte> bytes,
							const materialization_v2_1_auxiliary_spool_purpose purpose)
		{
			if (auto appended = spool.append(bytes); !appended)
				return sdk::unexpected(auxiliary_io_failure(purpose, "append", appended.error()));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		update_record_digest(materialization_digest_accumulator& digest,
							 const std::span<const std::byte> bytes,
							 const materialization_v2_1_auxiliary_spool_purpose purpose)
		{
			if (auto updated = digest.update(bytes); !updated)
				return sdk::unexpected(
					auxiliary_io_failure(purpose, "digest-update", updated.error()));
			return {};
		}

		[[nodiscard]] sdk::result<std::string>
		digest_task_schema_record(const std::string_view metadata,
								  clang22_task_source_replay& source,
								  const std::array<std::byte, 8U>& metadata_size,
								  materialization_v2_1_auxiliary_spool_factory& auxiliary_spools)
		{
			constexpr auto purpose =
				materialization_v2_1_auxiliary_spool_purpose::task_unique_index;
			auto digest = auxiliary_spools.make_digest(purpose);
			if (!digest)
				return sdk::unexpected(materialization_admission_no_response());
			if (auto updated = update_record_digest(*digest, metadata_size, purpose); !updated)
				return sdk::unexpected(std::move(updated.error()));
			if (auto updated = update_record_digest(
					*digest, std::as_bytes(std::span{metadata.data(), metadata.size()}), purpose);
				!updated)
				return sdk::unexpected(std::move(updated.error()));
			std::array<std::byte, default_stream_chunk_bytes> buffer{};
			std::uint64_t offset{};
			while (offset < source.size_bytes())
			{
				auto destination = std::span{buffer}.first(static_cast<std::size_t>(
					std::min<std::uint64_t>(source.size_bytes() - offset, buffer.size())));
				auto read = source.read_at(offset, destination);
				if (!read)
					return sdk::unexpected(std::move(read.error()));
				if (*read == 0U || *read > destination.size())
					return sdk::unexpected(materialization_admission_no_response());
				if (auto updated =
						update_record_digest(*digest, std::span{buffer}.first(*read), purpose);
					!updated)
					return sdk::unexpected(std::move(updated.error()));
				offset += static_cast<std::uint64_t>(*read);
			}
			auto finished = digest->finish();
			if (!finished ||
				!has_content_digest_grammar(finished ? std::string_view{*finished}
													 : std::string_view{}))
			{
				if (!finished)
					return sdk::unexpected(
						auxiliary_io_failure(purpose, "digest-finalize", finished.error()));
				return sdk::unexpected(materialization_admission_no_response());
			}
			return std::move(*finished);
		}

		struct compact_unique_record
		{
			std::array<std::byte, 32U> digest{};
			std::uint64_t payload_size{};
			std::uint64_t task_index{};
			std::uint64_t raw_task_offset{};
			std::uint64_t raw_task_size{};
		};

		inline constexpr std::size_t compact_unique_record_bytes = 64U;
		inline constexpr std::size_t maximum_materialization_tasks = 4096U;

		[[nodiscard]] std::optional<std::array<std::byte, 32U>>
		decode_content_digest_bytes(const std::string_view digest)
		{
			if (!has_content_digest_grammar(digest))
				return std::nullopt;
			const auto nibble = [](const char value) -> std::optional<std::uint8_t>
			{
				if (value >= '0' && value <= '9')
					return static_cast<std::uint8_t>(value - '0');
				if (value >= 'a' && value <= 'f')
					return static_cast<std::uint8_t>(static_cast<unsigned int>(value - 'a') + 10U);
				return std::nullopt;
			};
			std::array<std::byte, 32U> output{};
			const auto encoded = digest.substr(7U);
			for (std::size_t index{}; index < output.size(); ++index)
			{
				auto high = nibble(encoded[index * 2U]);
				auto low = nibble(encoded[index * 2U + 1U]);
				if (!high || !low)
					return std::nullopt;
				output[index] = static_cast<std::byte>((*high << 4U) | *low);
			}
			return output;
		}

		[[nodiscard]] sdk::result<void>
		append_compact_unique_record(materialization_private_spool& records,
									 const compact_unique_record& record,
									 const materialization_v2_1_auxiliary_spool_purpose purpose)
		{
			std::array<std::byte, compact_unique_record_bytes> bytes{};
			std::ranges::copy(record.digest, bytes.begin());
			const auto encode = [&](const std::uint64_t value, const std::size_t begin)
			{
				const auto framed = framed_u64(value);
				std::ranges::copy(framed, bytes.begin() + static_cast<std::ptrdiff_t>(begin));
			};
			encode(record.payload_size, 32U);
			encode(record.task_index, 40U);
			encode(record.raw_task_offset, 48U);
			encode(record.raw_task_size, 56U);
			return append_record_bytes(records, bytes, purpose);
		}

		[[nodiscard]] sdk::result<void>
		append_task_schema_record(materialization_private_spool& records,
								  const json_value& metadata,
								  clang22_task_source_replay& source,
								  const std::uint64_t task_index,
								  const materialization_request_task_span& raw_span,
								  materialization_v2_1_auxiliary_spool_factory& auxiliary_spools)
		{
			const auto canonical_metadata = canonical_json(metadata);
			if (canonical_metadata.size() > std::numeric_limits<std::uint64_t>::max() - 8U ||
				source.size_bytes() > std::numeric_limits<std::uint64_t>::max() - 8U -
						static_cast<std::uint64_t>(canonical_metadata.size()))
				return sdk::unexpected(materialization_admission_no_response());
			const auto payload_size =
				8U + static_cast<std::uint64_t>(canonical_metadata.size()) + source.size_bytes();
			const auto metadata_size = framed_u64(canonical_metadata.size());
			auto digest = digest_task_schema_record(
				canonical_metadata, source, metadata_size, auxiliary_spools);
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			auto decoded_digest = decode_content_digest_bytes(*digest);
			if (!decoded_digest)
				return sdk::unexpected(materialization_admission_no_response());
			return append_compact_unique_record(
				records,
				compact_unique_record{*decoded_digest,
									  payload_size,
									  task_index,
									  raw_span.value_offset,
									  raw_span.value_size_bytes},
				materialization_v2_1_auxiliary_spool_purpose::task_unique_index);
		}

		[[nodiscard]] sdk::result<void>
		append_string_record(materialization_private_spool& records,
							 const std::span<const std::string_view> fields,
							 const std::uint64_t task_index,
							 materialization_v2_1_auxiliary_spool_factory& auxiliary_spools)
		{
			constexpr auto purpose =
				materialization_v2_1_auxiliary_spool_purpose::execution_unique_index;
			auto digest = auxiliary_spools.make_digest(purpose);
			if (!digest)
				return sdk::unexpected(materialization_admission_no_response());
			std::uint64_t payload_size{};
			for (const auto field : fields)
			{
				if (field.size() > std::numeric_limits<std::uint64_t>::max() - 8U ||
					8U + static_cast<std::uint64_t>(field.size()) >
						std::numeric_limits<std::uint64_t>::max() - payload_size)
					return sdk::unexpected(materialization_admission_no_response());
				payload_size += 8U + static_cast<std::uint64_t>(field.size());
				const auto size = framed_u64(field.size());
				if (auto updated = update_record_digest(*digest, size, purpose); !updated)
					return updated;
				if (auto updated = update_record_digest(
						*digest, std::as_bytes(std::span{field.data(), field.size()}), purpose);
					!updated)
					return updated;
			}
			auto finished = digest->finish();
			if (!finished)
				return sdk::unexpected(
					auxiliary_io_failure(purpose, "digest-finalize", finished.error()));
			if (!has_content_digest_grammar(*finished))
				return sdk::unexpected(materialization_admission_no_response());
			auto decoded_digest = decode_content_digest_bytes(*finished);
			if (!decoded_digest)
				return sdk::unexpected(materialization_admission_no_response());
			return append_compact_unique_record(
				records,
				compact_unique_record{*decoded_digest, payload_size, task_index, 0U, 0U},
				purpose);
		}

		[[nodiscard]] sdk::result<void>
		read_record_bytes(materialization_replayable_spool& spool,
						  std::uint64_t offset,
						  std::span<std::byte> destination,
						  const materialization_v2_1_auxiliary_spool_purpose purpose)
		{
			std::size_t completed{};
			while (completed < destination.size())
			{
				auto read = spool.read_at(offset + completed, destination.subspan(completed));
				if (!read)
					return sdk::unexpected(auxiliary_io_failure(purpose, "read", read.error()));
				if (*read == 0U || *read > destination.size() - completed)
					return sdk::unexpected(materialization_admission_no_response());
				completed += *read;
			}
			return {};
		}

		[[nodiscard]] sdk::result<compact_unique_record>
		read_compact_unique_record(materialization_replayable_spool& spool,
								   const std::uint64_t index,
								   const materialization_v2_1_auxiliary_spool_purpose purpose)
		{
			if (index > std::numeric_limits<std::uint64_t>::max() / compact_unique_record_bytes)
				return sdk::unexpected(materialization_admission_no_response());
			const auto offset = index * compact_unique_record_bytes;
			if (offset > spool.size_bytes() ||
				compact_unique_record_bytes > spool.size_bytes() - offset)
				return sdk::unexpected(materialization_admission_no_response());
			std::array<std::byte, compact_unique_record_bytes> bytes{};
			if (auto read = read_record_bytes(spool, offset, bytes, purpose); !read)
				return sdk::unexpected(std::move(read.error()));
			compact_unique_record output;
			std::ranges::copy(std::span{bytes}.first(output.digest.size()), output.digest.begin());
			const auto decode = [&](const std::size_t begin)
			{
				std::uint64_t value{};
				for (const auto byte : std::span{bytes}.subspan(begin, 8U))
					value = (value << 8U) | std::to_integer<std::uint8_t>(byte);
				return value;
			};
			output.payload_size = decode(32U);
			output.task_index = decode(40U);
			output.raw_task_offset = decode(48U);
			output.raw_task_size = decode(56U);
			return output;
		}

		template <class ValidateRecord, class ExactEqual>
		[[nodiscard]] sdk::result<void> seal_and_validate_compact_unique_records(
			materialization_replayable_spool& records,
			const std::uint64_t expected_count,
			sdk::error duplicate,
			const materialization_v2_1_auxiliary_spool_purpose purpose,
			ValidateRecord&& validate_record,
			ExactEqual&& exact_equal)
		{
			if (auto sealed = records.seal(); !sealed)
				return sdk::unexpected(auxiliary_io_failure(purpose, "seal", sealed.error()));
			if (!records.sealed())
				return sdk::unexpected(materialization_admission_no_response());
			if (expected_count > maximum_materialization_tasks ||
				expected_count >
					std::numeric_limits<std::uint64_t>::max() / compact_unique_record_bytes ||
				records.size_bytes() != expected_count * compact_unique_record_bytes)
				return sdk::unexpected(materialization_admission_no_response());

			std::array<compact_unique_record, maximum_materialization_tasks> index{};
			for (std::uint64_t record_index{}; record_index < expected_count; ++record_index)
			{
				auto record = read_compact_unique_record(records, record_index, purpose);
				if (!record)
					return sdk::unexpected(std::move(record.error()));
				if (record->task_index != record_index)
					return sdk::unexpected(materialization_admission_no_response());
				if (auto valid = validate_record(*record); !valid)
					return sdk::unexpected(std::move(valid.error()));
				index[static_cast<std::size_t>(record_index)] = *record;
			}

			auto begin = index.begin();
			auto end = begin + static_cast<std::ptrdiff_t>(expected_count);
			std::sort(begin,
					  end,
					  [](const compact_unique_record& left, const compact_unique_record& right)
					  {
						  if (left.payload_size != right.payload_size)
							  return left.payload_size < right.payload_size;
						  return std::ranges::lexicographical_compare(left.digest, right.digest);
					  });
			for (auto group = begin; group != end;)
			{
				auto group_end = group + 1;
				while (group_end != end && group_end->payload_size == group->payload_size &&
					   group_end->digest == group->digest)
					++group_end;
				for (auto left = group; left != group_end; ++left)
					for (auto right = left + 1; right != group_end; ++right)
					{
						auto equal = exact_equal(*left, *right);
						if (!equal)
							return sdk::unexpected(std::move(equal.error()));
						if (*equal)
							return sdk::unexpected(std::move(duplicate));
					}
				group = group_end;
			}
			return {};
		}

		[[nodiscard]] sdk::result<std::uint64_t> validate_selected_task_schema(
			materialization_replayable_spool& raw_request,
			materialization_request_task_index& task_index,
			const std::span<const relation_descriptor> outputs,
			materialization_v2_1_auxiliary_spool_factory& auxiliary_spools)
		{
			constexpr std::uint64_t maximum_aggregate_source_bytes = 512U * 1024U * 1024U;
			auto unique_records = auxiliary_spools.create(
				materialization_v2_1_auxiliary_spool_purpose::task_unique_index);
			if (!unique_records)
				return sdk::unexpected(auxiliary_create_failure(
					materialization_v2_1_auxiliary_spool_purpose::task_unique_index,
					unique_records.error()));
			if (!*unique_records)
				return sdk::unexpected(materialization_admission_no_response());
			std::uint64_t aggregate_source_bytes{};
			for (std::uint64_t index{}; index < task_index.task_count(); ++index)
			{
				auto span = task_index.at(index);
				if (!span)
					return sdk::unexpected(
						task_index_phase_error(std::move(span.error()), "request-schema"));
				auto metadata = replay_materialization_task_metadata(raw_request, *span);
				if (!metadata)
					return sdk::unexpected(std::move(metadata.error()));
				auto shape =
					parse_task(metadata->root(), nullptr, {}, outputs, task_metadata_phase::shape);
				if (!shape)
					return sdk::unexpected(std::move(shape.error()));
				auto source = make_materialization_task_source_spool();
				if (!source)
					return sdk::unexpected(normalize_materialization_admission_spool_failure(
						std::move(source.error()), "request-schema", "source-spool:create"));
				auto receipt = decode_materialization_task_source(
					raw_request, *span, **source, "request-schema");
				if (!receipt)
					return sdk::unexpected(std::move(receipt.error()));
				if (receipt->size_bytes > maximum_aggregate_source_bytes - aggregate_source_bytes)
					return sdk::unexpected(invalid("tasks.source", "decoded-aggregate-limit"));
				aggregate_source_bytes += receipt->size_bytes;
				if (auto appended = append_task_schema_record(
						**unique_records,
						metadata->root(),
						static_cast<clang22_task_source_replay&>(**source),
						index,
						*span,
						auxiliary_spools);
					!appended)
					return sdk::unexpected(std::move(appended.error()));
			}
			if (auto unique = seal_and_validate_compact_unique_records(
					**unique_records,
					task_index.task_count(),
					invalid("tasks", "uniqueItems"),
					materialization_v2_1_auxiliary_spool_purpose::task_unique_index,
					[&](const compact_unique_record& record) -> sdk::result<void>
					{
						auto span = task_index.at(record.task_index);
						if (!span)
							return sdk::unexpected(
								task_index_phase_error(std::move(span.error()), "request-schema"));
						if (span->value_offset != record.raw_task_offset ||
							span->value_size_bytes != record.raw_task_size)
							return sdk::unexpected(materialization_admission_no_response());
						return {};
					},
					[&](const compact_unique_record& left,
						const compact_unique_record& right) -> sdk::result<bool>
					{
						auto left_span = task_index.at(left.task_index);
						auto right_span = task_index.at(right.task_index);
						if (!left_span || !right_span)
							return sdk::unexpected(
								task_index_phase_error(!left_span ? std::move(left_span.error())
																  : std::move(right_span.error()),
													   "request-schema"));
						auto comparison = auxiliary_spools.create(
							materialization_v2_1_auxiliary_spool_purpose::task_collision_metadata);
						if (!comparison)
							return sdk::unexpected(auxiliary_create_failure(
								materialization_v2_1_auxiliary_spool_purpose::
									task_collision_metadata,
								comparison.error()));
						if (!*comparison)
							return sdk::unexpected(materialization_admission_no_response());
						return exact_materialization_task_json_equal(
							raw_request, *left_span, *right_span, **comparison);
					});
				!unique)
				return sdk::unexpected(std::move(unique.error()));
			return aggregate_source_bytes;
		}

		[[nodiscard]] sdk::result<std::string>
		digest_task_input(clang22_task_input_replay& input,
						  materialization_v2_1_auxiliary_spool_factory& auxiliary_spools)
		{
			constexpr auto purpose =
				materialization_v2_1_auxiliary_spool_purpose::task_input_digest;
			if (input.size_bytes() > maximum_clang22_task_input_bytes)
				return sdk::unexpected(materialization_admission_no_response());
			if (!input.sealed())
				return sdk::unexpected(materialization_admission_no_response());
			auto digest = auxiliary_spools.make_digest(purpose);
			if (!digest)
				return sdk::unexpected(materialization_admission_no_response());
			std::array<std::byte, default_stream_chunk_bytes> buffer{};
			std::uint64_t offset{};
			while (offset < input.size_bytes())
			{
				const auto remaining = input.size_bytes() - offset;
				auto destination = std::span{buffer}.first(
					static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size())));
				auto read = input.read_at(offset, destination);
				if (!read)
					return sdk::unexpected(normalize_materialization_admission_spool_failure(
						std::move(read.error()), "request-binding", "task-input:digest-read"));
				if (*read == 0U || *read > destination.size())
					return sdk::unexpected(materialization_admission_no_response());
				auto updated = digest->update(std::span{buffer}.first(*read));
				if (!updated)
					return sdk::unexpected(
						auxiliary_io_failure(purpose, "digest-update", updated.error()));
				offset += static_cast<std::uint64_t>(*read);
			}
			auto finished = digest->finish();
			if (!finished)
				return sdk::unexpected(
					auxiliary_io_failure(purpose, "digest-finalize", finished.error()));
			if (!has_content_digest_grammar(*finished))
				return sdk::unexpected(materialization_admission_no_response());
			return std::move(*finished);
		}

		struct source_dependent_task_binding
		{
			std::string provider_task_id;
			std::string task_input_digest;
			std::string provider_execution_id;
			std::uint64_t source_size_bytes{};
		};

		[[nodiscard]] sdk::result<source_dependent_task_binding> validate_source_dependent_task(
			materialization_replayable_spool& raw_request,
			materialization_request_task_index& task_index,
			const std::uint64_t index,
			const sdk::project_catalog& catalog,
			const std::string_view project_id,
			const std::span<const relation_descriptor> outputs,
			const materialization_v2_1_worker_authority& worker,
			materialization_v2_1_auxiliary_spool_factory& auxiliary_spools)
		{
			auto metadata = replay_task_metadata(raw_request,
												 task_index,
												 index,
												 &catalog,
												 project_id,
												 outputs,
												 task_metadata_phase::source_independent_binding);
			if (!metadata)
				return sdk::unexpected(std::move(metadata.error()));
			if (metadata->catalog_owner != &catalog)
				return sdk::unexpected(invalid("task", "metadata-catalog-owner"));

			auto span = task_index.at(index);
			if (!span)
				return sdk::unexpected(
					task_index_phase_error(std::move(span.error()), "request-binding"));
			auto source = make_materialization_task_source_spool();
			if (!source)
				return sdk::unexpected(normalize_materialization_admission_spool_failure(
					std::move(source.error()), "request-binding", "source-spool:create"));
			auto source_receipt =
				decode_materialization_task_source(raw_request, *span, **source, "request-binding");
			if (!source_receipt)
				return sdk::unexpected(std::move(source_receipt.error()));
			if (source_receipt->size_bytes != metadata->input.source_size_bytes)
				return sdk::unexpected(mismatch("task.source.size_bytes"));
			if (source_receipt->content_digest != metadata->input.source_content_digest)
				return sdk::unexpected(mismatch("task.source.content_digest"));
			if (source_receipt->line_index_id != metadata->input.line_index)
				return sdk::unexpected(mismatch("task.source.line_index_id"));

			if (auto bound = metadata->input.validate_with_catalog(catalog, *source_receipt);
				!bound)
				return sdk::unexpected(
					mismatch("task." + bound.error().field, bound.error().detail));

			auto task_input = make_materialization_task_input_spool();
			if (!task_input)
				return sdk::unexpected(normalize_materialization_admission_spool_failure(
					std::move(task_input.error()), "request-binding", "task-input:create"));
			auto encoded =
				encode_task_input_streaming(metadata->input, catalog, **source, **task_input);
			if (!encoded)
			{
				if (infrastructure_failure(encoded.error().code))
					return sdk::unexpected(normalize_materialization_admission_spool_failure(
						std::move(encoded.error()), "request-binding", "task-input:encode"));
				return sdk::unexpected(materialization_admission_no_response());
			}
			if (auto sealed = (*task_input)->seal(); !sealed)
				return sdk::unexpected(normalize_materialization_admission_spool_failure(
					std::move(sealed.error()), "request-binding", "task-input:seal"));
			if (!(*task_input)->sealed() ||
				(*task_input)->size_bytes() != encoded->task_input_bytes)
				return sdk::unexpected(materialization_admission_no_response());
			auto input_digest = digest_task_input(**task_input, auxiliary_spools);
			if (!input_digest)
				return sdk::unexpected(std::move(input_digest.error()));
			if (*input_digest != metadata->task_input_digest)
				return sdk::unexpected(mismatch("task.task_input_digest"));

			auto provider_task_id = reconstruct_provider_task_id(metadata->input,
																 catalog,
																 *source_receipt,
																 {outputs.begin(), outputs.end()},
																 worker.semantic_contract_digest);
			if (!provider_task_id)
			{
				if (infrastructure_failure(provider_task_id.error().code))
					return sdk::unexpected(normalize_materialization_admission_spool_failure(
						std::move(provider_task_id.error()),
						"request-binding",
						"provider-task-identity"));
				return sdk::unexpected(materialization_admission_no_response());
			}
			if (*provider_task_id != metadata->provider_task_id)
				return sdk::unexpected(mismatch("task.provider_task_id"));
			if (!metadata->provider_task_id.starts_with("task:"))
				return sdk::unexpected(mismatch("task.provider_execution_id"));
			auto execution =
				identity("provider-execution",
						 {canonical_value::from_string(worker.provider_id),
						  canonical_value::from_string(worker.installed_binary_digest),
						  canonical_value::from_string(metadata->provider_task_id.substr(5U)),
						  canonical_value::from_string(metadata->task_input_digest)});
			if (!execution)
				return sdk::unexpected(materialization_admission_no_response());
			if (*execution != metadata->provider_execution_id)
				return sdk::unexpected(mismatch("task.provider_execution_id"));

			return source_dependent_task_binding{std::move(metadata->provider_task_id),
												 std::move(metadata->task_input_digest),
												 std::move(metadata->provider_execution_id),
												 source_receipt->size_bytes};
		}
	} // namespace

	std::unique_ptr<materialization_digest_accumulator>
	materialization_v2_1_auxiliary_spool_factory::make_digest(
		const materialization_v2_1_auxiliary_spool_purpose)
	{
		return make_materialization_sha256_accumulator();
	}

	prevalidated_materialization_request_v2_1::prevalidated_materialization_request_v2_1(
		materialization_v2_1_tool_authority tool,
		materialization_v2_1_worker_authority worker,
		std::string project_id,
		sdk::project_catalog catalog,
		sdk::relation_engine engine,
		std::vector<sdk::relation_descriptor> output_descriptors,
		validated_publication_request publication,
		const std::uint64_t task_count,
		const std::uint64_t declared_source_bytes,
		std::unique_ptr<materialization_replayable_spool> raw_request,
		materialization_request_envelope envelope,
		std::unique_ptr<materialization_request_task_index> task_index)
		: tool_{std::move(tool)}, worker_{std::move(worker)}, project_id_{std::move(project_id)},
		  catalog_{std::move(catalog)}, engine_{std::move(engine)},
		  output_descriptors_{std::move(output_descriptors)}, publication_{std::move(publication)},
		  task_count_{task_count}, declared_source_bytes_{declared_source_bytes},
		  raw_request_{std::move(raw_request)}, envelope_{std::move(envelope)},
		  task_index_{std::move(task_index)}
	{
	}

	prevalidated_materialization_request_v2_1::prevalidated_materialization_request_v2_1(
		prevalidated_materialization_request_v2_1&&) noexcept = default;
	prevalidated_materialization_request_v2_1& prevalidated_materialization_request_v2_1::operator=(
		prevalidated_materialization_request_v2_1&&) noexcept = default;
	prevalidated_materialization_request_v2_1::~prevalidated_materialization_request_v2_1() =
		default;

	const materialization_v2_1_tool_authority&
	prevalidated_materialization_request_v2_1::tool() const noexcept
	{
		return tool_;
	}

	const materialization_v2_1_worker_authority&
	prevalidated_materialization_request_v2_1::worker() const noexcept
	{
		return worker_;
	}

	const std::string& prevalidated_materialization_request_v2_1::project_id() const noexcept
	{
		return project_id_;
	}

	const sdk::project_catalog& prevalidated_materialization_request_v2_1::catalog() const noexcept
	{
		return catalog_;
	}

	const sdk::relation_engine& prevalidated_materialization_request_v2_1::engine() const noexcept
	{
		return engine_;
	}

	const std::vector<sdk::relation_descriptor>&
	prevalidated_materialization_request_v2_1::output_descriptors() const noexcept
	{
		return output_descriptors_;
	}

	const validated_publication_request&
	prevalidated_materialization_request_v2_1::publication() const noexcept
	{
		return publication_;
	}

	std::uint64_t prevalidated_materialization_request_v2_1::task_count() const noexcept
	{
		return task_count_;
	}

	std::uint64_t prevalidated_materialization_request_v2_1::declared_source_bytes() const noexcept
	{
		return declared_source_bytes_;
	}

	sdk::result<materialization_v2_1_task_metadata_receipt>
	prevalidated_materialization_request_v2_1::task_metadata(const std::uint64_t index)
	{
		if (index >= task_count_ || !raw_request_ || !task_index_)
			return sdk::unexpected(invalid("tasks", "metadata-index"));
		auto binding = replay_task_metadata(*raw_request_,
											*task_index_,
											index,
											&catalog_,
											project_id_,
											output_descriptors_,
											task_metadata_phase::source_independent_binding);
		if (!binding)
			return sdk::unexpected(std::move(binding.error()));
		if (binding->catalog_owner != &catalog_)
			return sdk::unexpected(invalid("task", "metadata-catalog-owner"));
		return metadata_receipt(*binding, index);
	}

	sdk::result<prevalidated_materialization_request_v2_1> prevalidate_materialization_request_v2_1(
		std::unique_ptr<materialization_replayable_spool> raw_request,
		materialization_request_envelope envelope,
		std::unique_ptr<materialization_request_task_index> task_index,
		materialization_v2_1_auxiliary_spool_factory& auxiliary_spools)
	{
		if (!raw_request || !task_index || !raw_request->sealed() || !task_index->sealed() ||
			envelope.schema() != materialization_request_schema_v2 ||
			envelope.request_version() != materialization_request_version_v2_1 ||
			envelope.scanned_size_bytes() != raw_request->size_bytes() ||
			task_index->task_count() == 0U || task_index->task_count() > 4096U)
			return sdk::unexpected(invalid("request", "prevalidation-binding"));
		if (auto readable = verify_replay_range(*raw_request, 0U, raw_request->size_bytes());
			!readable)
			return sdk::unexpected(std::move(readable.error()));

		auto globals = replay_materialization_request_globals(*raw_request, envelope);
		if (!globals)
			return sdk::unexpected(std::move(globals.error()));
		const auto& root = globals->root();
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
		auto schema = member_text(root, "schema", "request.schema");
		auto version = member_text(root, "request_version", "request.request_version");
		if (!schema || !version || *schema != materialization_request_schema_v2 ||
			*version != materialization_request_version_v2_1)
			return sdk::unexpected(unsupported_version("request.request_version"));
		auto request_id =
			member_text(root, "materialization_request_id", "request.materialization_request_id");
		auto request_digest = member_text(root, "request_digest", "request.request_digest");
		auto semantic_digest_value =
			member_text(root, "semantic_request_digest", "request.semantic_request_digest");
		if (!request_id || request_id->size() > 2048U || !sdk::validate_strong_id(*request_id) ||
			!request_digest || !has_semantic_digest_grammar(*request_digest) ||
			!semantic_digest_value || !has_semantic_digest_grammar(*semantic_digest_value))
			return sdk::unexpected(invalid("request.identity", "grammar"));
		if (auto schema_valid = validate_selected_v2_1_global_schema(root); !schema_valid)
			return sdk::unexpected(std::move(schema_valid.error()));

		const auto count = task_index->task_count();
		auto outputs = exact_output_descriptors();
		auto schema_source_bytes =
			validate_selected_task_schema(*raw_request, *task_index, outputs, auxiliary_spools);
		if (!schema_source_bytes)
			return sdk::unexpected(std::move(schema_source_bytes.error()));

		auto tool = tool_authority(root);
		auto worker = worker_authority(root);
		if (!tool || !worker)
			return sdk::unexpected(!tool ? std::move(tool.error()) : std::move(worker.error()));

		auto worker_value = required_member(root, "worker", "worker");
		auto project_value = required_member(root, "project", "project");
		auto registry_value = required_member(root, "registry", "registry");
		auto engine_value = required_member(root, "engine", "engine");
		auto interpretation_value =
			required_member(root, "interpretation_policy", "interpretation_policy");
		auto trust_value = required_member(root, "trust_policy", "trust_policy");
		auto publication_value = required_member(root, "publication", "publication");
		if (!worker_value || !project_value || !registry_value || !engine_value ||
			!interpretation_value || !trust_value || !publication_value)
			return sdk::unexpected(invalid("request", "global-member"));
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
		if (auto bindings = validate_descriptor_bindings(**registry_value, base, outputs);
			!bindings)
			return sdk::unexpected(std::move(bindings.error()));
		auto validated_engine = validate_engine(**engine_value, **worker_value, base, outputs);
		if (!validated_engine)
			return sdk::unexpected(std::move(validated_engine.error()));

		if (count != catalog->compile_units.size())
			return sdk::unexpected(
				request_error("materialization.catalog-census-mismatch", "tasks", "cardinality"));

		std::set<std::pair<std::string, std::string>> sandbox_requirements;
		std::string condition_universe;
		std::uint64_t declared_source_bytes{};
		constexpr std::uint64_t maximum_aggregate_source_bytes = 512U * 1024U * 1024U;
		for (std::uint64_t index{}; index < count; ++index)
		{
			auto task = replay_task_metadata(*raw_request,
											 *task_index,
											 index,
											 &*catalog,
											 *supplied_project,
											 outputs,
											 task_metadata_phase::source_independent_binding);
			if (!task)
				return sdk::unexpected(std::move(task.error()));
			if (task->catalog_owner != &*catalog)
				return sdk::unexpected(invalid("task", "metadata-catalog-owner"));
			if (task->input.selected_catalog_compile_unit !=
				catalog->compile_units[static_cast<std::size_t>(index)].compile_unit_id)
				return sdk::unexpected(request_error(
					"materialization.catalog-census-mismatch", "tasks", "selection-order"));
			if (task->input.sandbox.policy_digest != worker->sandbox_policy_digest)
				return sdk::unexpected(mismatch("task.sandbox.policy_digest"));
			if (declared_source_bytes >
				maximum_aggregate_source_bytes - task->input.source_size_bytes)
				return sdk::unexpected(invalid("tasks.source", "declared-aggregate-limit"));
			declared_source_bytes += task->input.source_size_bytes;
			sandbox_requirements.emplace(task->input.sandbox.minimum,
										 task->input.sandbox.policy_digest);
			if (condition_universe.empty())
				condition_universe = task->input.condition_universe;
			else if (condition_universe != task->input.condition_universe)
				return sdk::unexpected(mismatch("publication.selector.condition_universe_id"));
		}
		if (declared_source_bytes != *schema_source_bytes)
			return sdk::unexpected(mismatch("tasks.source", "declared-decoded-census"));

		auto interpretation_digest = validate_interpretation_policy(**interpretation_value);
		if (!interpretation_digest)
			return sdk::unexpected(std::move(interpretation_digest.error()));
		auto trust_digest = validate_trust_policy(**trust_value, *worker, sandbox_requirements);
		if (!trust_digest)
			return sdk::unexpected(std::move(trust_digest.error()));
		worker->trust_policy_digest = *trust_digest;
		for (const auto& [minimum, digest] : sandbox_requirements)
			worker->task_sandbox_requirements.push_back({
				minimum == "certified" ? sdk::provider::sandbox_assurance::certified
									   : sdk::provider::sandbox_assurance::enforced,
				digest,
			});

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
												condition_universe);
		if (!publication)
			return sdk::unexpected(std::move(publication.error()));

		return prevalidated_materialization_request_v2_1{
			std::move(*tool),
			std::move(*worker),
			std::move(*supplied_project),
			std::move(*catalog),
			std::move(*validated_engine),
			std::move(outputs),
			std::move(*publication),
			count,
			declared_source_bytes,
			std::move(raw_request),
			std::move(envelope),
			std::move(task_index),
		};
	}

	sdk::result<prevalidated_materialization_request_v2_1> prevalidate_materialization_request_v2_1(
		std::unique_ptr<materialization_replayable_spool> raw_request,
		materialization_request_envelope envelope,
		std::unique_ptr<materialization_request_task_index> task_index)
	{
		production_auxiliary_spool_factory auxiliary_spools;
		return prevalidate_materialization_request_v2_1(
			std::move(raw_request), std::move(envelope), std::move(task_index), auxiliary_spools);
	}

	validated_materialization_request_v2_1::validated_materialization_request_v2_1(
		prevalidated_materialization_request_v2_1 request,
		streamed_materialization_request_identity identity)
		: request_{std::move(request)}, identity_{std::move(identity)}
	{
	}

	validated_materialization_request_v2_1::validated_materialization_request_v2_1(
		validated_materialization_request_v2_1&&) noexcept = default;
	validated_materialization_request_v2_1& validated_materialization_request_v2_1::operator=(
		validated_materialization_request_v2_1&&) noexcept = default;
	validated_materialization_request_v2_1::~validated_materialization_request_v2_1() = default;

	const prevalidated_materialization_request_v2_1&
	validated_materialization_request_v2_1::request() const noexcept
	{
		return request_;
	}

	const streamed_materialization_request_identity&
	validated_materialization_request_v2_1::identity() const noexcept
	{
		return identity_;
	}

	sdk::result<materialization_v2_1_task_metadata_receipt>
	validated_materialization_request_v2_1::task_metadata(const std::uint64_t index)
	{
		return request_.task_metadata(index);
	}

	sdk::result<validated_materialization_request_v2_1> admit_materialization_request_v2_1(
		prevalidated_materialization_request_v2_1 request,
		materialization_v2_1_auxiliary_spool_factory& auxiliary_spools)
	{
		if (!request.raw_request_ || !request.task_index_ || !request.raw_request_->sealed() ||
			!request.task_index_->sealed() ||
			request.task_count_ != request.task_index_->task_count())
			return sdk::unexpected(invalid("request", "admission-binding"));

		auto execution_keys = auxiliary_spools.create(
			materialization_v2_1_auxiliary_spool_purpose::execution_unique_index);
		if (!execution_keys)
			return sdk::unexpected(auxiliary_create_failure(
				materialization_v2_1_auxiliary_spool_purpose::execution_unique_index,
				execution_keys.error()));
		if (!*execution_keys)
			return sdk::unexpected(materialization_admission_no_response());
		std::uint64_t admitted_source_bytes{};
		for (std::uint64_t index{}; index < request.task_count_; ++index)
		{
			auto task = validate_source_dependent_task(*request.raw_request_,
													   *request.task_index_,
													   index,
													   request.catalog_,
													   request.project_id_,
													   request.output_descriptors_,
													   request.worker_,
													   auxiliary_spools);
			if (!task)
				return sdk::unexpected(std::move(task.error()));
			if (task->source_size_bytes > request.declared_source_bytes_ ||
				admitted_source_bytes > request.declared_source_bytes_ - task->source_size_bytes)
				return sdk::unexpected(mismatch("tasks.source", "admission-census-overflow"));
			admitted_source_bytes += task->source_size_bytes;
			const std::array fields{std::string_view{task->provider_task_id},
									std::string_view{task->task_input_digest},
									std::string_view{task->provider_execution_id}};
			if (auto appended =
					append_string_record(**execution_keys, fields, index, auxiliary_spools);
				!appended)
				return sdk::unexpected(std::move(appended.error()));
		}
		if (admitted_source_bytes != request.declared_source_bytes_)
			return sdk::unexpected(mismatch("tasks.source", "admission-census"));
		if (auto unique = seal_and_validate_compact_unique_records(
				**execution_keys,
				request.task_count_,
				mismatch("tasks", "duplicate-execution-tuple"),
				materialization_v2_1_auxiliary_spool_purpose::execution_unique_index,
				[](const compact_unique_record& record) -> sdk::result<void>
				{
					if (record.raw_task_offset != 0U || record.raw_task_size != 0U)
						return sdk::unexpected(materialization_admission_no_response());
					return {};
				},
				[&](const compact_unique_record& left,
					const compact_unique_record& right) -> sdk::result<bool>
				{
					auto left_metadata = request.task_metadata(left.task_index);
					if (!left_metadata)
						return sdk::unexpected(std::move(left_metadata.error()));
					auto right_metadata = request.task_metadata(right.task_index);
					if (!right_metadata)
						return sdk::unexpected(std::move(right_metadata.error()));
					return left_metadata->provider_task_id == right_metadata->provider_task_id &&
						left_metadata->task_input_digest == right_metadata->task_input_digest &&
						left_metadata->provider_execution_id ==
						right_metadata->provider_execution_id;
				});
			!unique)
			return sdk::unexpected(std::move(unique.error()));

		auto identity = validate_streamed_materialization_request_identity(
			*request.raw_request_, request.envelope_, *request.task_index_);
		if (!identity)
			return sdk::unexpected(std::move(identity.error()));
		return validated_materialization_request_v2_1{std::move(request), std::move(*identity)};
	}

	sdk::result<validated_materialization_request_v2_1>
	admit_materialization_request_v2_1(prevalidated_materialization_request_v2_1 request)
	{
		production_auxiliary_spool_factory auxiliary_spools;
		return admit_materialization_request_v2_1(std::move(request), auxiliary_spools);
	}

	sdk::result<validated_materialization_request_v2_1> validate_materialization_request_v2_1(
		std::unique_ptr<materialization_replayable_spool> raw_request,
		materialization_request_envelope envelope,
		std::unique_ptr<materialization_request_task_index> task_index,
		materialization_v2_1_auxiliary_spool_factory& auxiliary_spools)
	{
		try
		{
			auto request = prevalidate_materialization_request_v2_1(std::move(raw_request),
																	std::move(envelope),
																	std::move(task_index),
																	auxiliary_spools);
			if (!request)
				return sdk::unexpected(std::move(request.error()));
			return admit_materialization_request_v2_1(std::move(*request), auxiliary_spools);
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(materialization_admission_no_response());
		}
	}

	sdk::result<validated_materialization_request_v2_1> validate_materialization_request_v2_1(
		std::unique_ptr<materialization_replayable_spool> raw_request,
		materialization_request_envelope envelope,
		std::unique_ptr<materialization_request_task_index> task_index)
	{
		production_auxiliary_spool_factory auxiliary_spools;
		return validate_materialization_request_v2_1(
			std::move(raw_request), std::move(envelope), std::move(task_index), auxiliary_spools);
	}
} // namespace cxxlens::detail::clang22::materialization
