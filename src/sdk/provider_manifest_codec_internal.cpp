#include "provider_manifest_codec_internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bounded_json_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		constexpr std::size_t maximum_manifest_bytes = std::size_t{64U} * 1024U;
		constexpr std::size_t maximum_manifest_items = 4096U;
		constexpr std::size_t maximum_id_bytes = 512U;

		[[nodiscard]] error invalid(std::string field, std::string detail)
		{
			return {"provider.manifest-decode-invalid", std::move(field), std::move(detail)};
		}

		template <std::size_t size>
		[[nodiscard]] result<void> exact_members(const json_value& value,
												 const std::array<std::string_view, size>& fields,
												 const std::string_view field)
		{
			if (!value.has_exact_members(fields))
				return unexpected(invalid(std::string{field}, "member-set"));
			return {};
		}

		struct member_request
		{
			std::string_view name;
			std::string_view field;
		};

		[[nodiscard]] result<const json_value*> member(const json_value& value,
													   const member_request request)
		{
			const auto* found = value.member(request.name);
			if (found == nullptr)
				return unexpected(invalid(std::string{request.field}, "missing"));
			return found;
		}

		[[nodiscard]] result<std::string>
		text(const json_value& value, const std::string_view field, const std::size_t maximum_bytes)
		{
			const auto* found = value.as_string();
			if (found == nullptr)
				return unexpected(invalid(std::string{field}, "string"));
			if (found->empty() || found->size() > maximum_bytes)
				return unexpected(invalid(std::string{field}, "string-size"));
			return *found;
		}

		[[nodiscard]] result<std::string>
		member_text(const json_value& value,
					const std::string_view name,
					const std::string_view field,
					const std::size_t maximum_bytes = maximum_id_bytes)
		{
			auto found = member(value, {.name = name, .field = field});
			if (!found)
				return unexpected(std::move(found.error()));
			return text(**found, field, maximum_bytes);
		}

		[[nodiscard]] result<std::vector<std::string>> string_array(const json_value& value,
																	const std::string_view field)
		{
			const auto* values = value.as_array();
			if (values == nullptr)
				return unexpected(invalid(std::string{field}, "array"));
			if (values->size() > maximum_manifest_items)
				return unexpected(invalid(std::string{field}, "array-size"));
			std::vector<std::string> output;
			output.reserve(values->size());
			for (const auto& value_item : *values)
			{
				auto item = text(value_item, field, maximum_id_bytes);
				if (!item)
					return unexpected(std::move(item.error()));
				output.push_back(std::move(*item));
			}
			return output;
		}

		[[nodiscard]] result<std::vector<std::string>> member_string_array(
			const json_value& value, const std::string_view name, const std::string_view field)
		{
			auto found = member(value, {.name = name, .field = field});
			if (!found)
				return unexpected(std::move(found.error()));
			return string_array(**found, field);
		}

		[[nodiscard]] result<void> assign_member_text(std::string& output,
													  const json_value& value,
													  const std::string_view field)
		{
			auto decoded = member_text(value, field, field);
			if (!decoded)
				return unexpected(std::move(decoded.error()));
			output = std::move(*decoded);
			return {};
		}

		[[nodiscard]] bool canonical_sha256_digest(const std::string_view value)
		{
			return value.size() == 71U && value.starts_with("sha256:") &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] result<void> assign_member_string_array(std::vector<std::string>& output,
															  const json_value& value,
															  const std::string_view field)
		{
			auto decoded = member_string_array(value, field, field);
			if (!decoded)
				return unexpected(std::move(decoded.error()));
			output = std::move(*decoded);
			return {};
		}

		[[nodiscard]] result<std::uint32_t> unsigned_component(const json_value& value,
															   const std::string_view field)
		{
			const auto* number = value.as_unsigned_integer();
			if (number == nullptr || *number > std::numeric_limits<std::uint32_t>::max())
				return unexpected(invalid(std::string{field}, "uint32"));
			return static_cast<std::uint32_t>(*number);
		}

		[[nodiscard]] result<std::uint32_t> member_unsigned_component(const json_value& value,
																	  const std::string_view name,
																	  const std::string_view field)
		{
			auto found = member(value, {.name = name, .field = field});
			if (!found)
				return unexpected(std::move(found.error()));
			return unsigned_component(**found, field);
		}

		[[nodiscard]] result<void> assign_member_unsigned_component(std::uint32_t& output,
																	const json_value& value,
																	const std::string_view field)
		{
			auto decoded = member_unsigned_component(value, field, field);
			if (!decoded)
				return unexpected(std::move(decoded.error()));
			output = *decoded;
			return {};
		}

		[[nodiscard]] result<semantic_version> semantic_version_value(const json_value& value,
																	  const std::string_view field)
		{
			auto encoded = text(value, field, 32U);
			if (!encoded)
				return unexpected(std::move(encoded.error()));
			std::array<std::uint32_t, 3U> components{};
			std::size_t begin{};
			std::size_t index{};
			for (auto& component : components)
			{
				const auto end =
					index + 1U == components.size() ? encoded->size() : encoded->find('.', begin);
				if (end == std::string::npos || end == begin ||
					(end - begin > 1U && (*encoded)[begin] == '0'))
					return unexpected(invalid(std::string{field}, "semantic-version"));
				const auto converted =
					std::from_chars(encoded->data() + begin, encoded->data() + end, component);
				if (converted.ec != std::errc{} || converted.ptr != encoded->data() + end)
					return unexpected(invalid(std::string{field}, "semantic-version"));
				begin = end + 1U;
				++index;
			}
			if (begin != encoded->size() + 1U || components[0U] == 0U)
				return unexpected(invalid(std::string{field}, "semantic-version"));
			return semantic_version{components[0U], components[1U], components[2U]};
		}

		[[nodiscard]] result<std::optional<std::string>> optional_signature(const json_value& value)
		{
			auto found = member(value, {.name = "signature", .field = "signature"});
			if (!found)
				return unexpected(std::move(found.error()));
			if ((*found)->is_null())
				return std::optional<std::string>{};
			auto value_text = text(**found, "signature", maximum_id_bytes);
			if (!value_text)
				return unexpected(std::move(value_text.error()));
			return std::optional<std::string>{std::move(*value_text)};
		}
	} // namespace

	result<provider::manifest> decode_provider_manifest(const std::string_view canonical_bytes)
	{
		try
		{
			const json_limits limits{
				.max_input_bytes = maximum_manifest_bytes,
				.max_depth = 4U,
				.max_array_elements = maximum_manifest_items,
				.max_object_members = 64U,
				.max_string_bytes = 8192U,
				.max_total_string_bytes = maximum_manifest_bytes,
				.max_total_values = maximum_manifest_items * 12U,
			};
			const json_parse_contract contract{
				.error_code = "provider.manifest-decode-invalid",
				.error_field = "manifest",
				.include_byte_offset = true,
				.require_top_level_object = true,
				.reject_utf8_bom = true,
				.numbers = json_number_syntax::integer_tokens,
				.depth = json_depth_semantics::containers,
			};
			auto root = parse_json_value(canonical_bytes, limits, contract);
			if (!root)
				return unexpected(std::move(root.error()));

			constexpr std::array<std::string_view, 21U> root_fields{
				"determinism_contract",
				"interpretation_domains",
				"invalidation_contract",
				"license",
				"offered_relations",
				"package_identity",
				"platform_tuples",
				"protocol_range",
				"provider_binary_digest",
				"provider_id",
				"provider_semantic_contract_digest",
				"provider_version",
				"publisher",
				"requested_qualifications",
				"required_relations",
				"resource_class",
				"sandbox_minimum",
				"schema",
				"signature",
				"task_stage",
				"trust_flags"};
			if (auto valid = exact_members(*root, root_fields, "manifest"); !valid)
				return unexpected(std::move(valid.error()));
			if (canonical_json(*root) != canonical_bytes)
				return unexpected(invalid("manifest", "noncanonical-json"));

			auto schema = member_text(*root, "schema", "schema");
			if (!schema)
				return unexpected(std::move(schema.error()));
			if (*schema != "cxxlens.provider-manifest.v1")
				return unexpected(invalid("schema", "unsupported"));

			provider::manifest output;
			for (auto binding :
				 {std::pair{&output.provider_id, std::string_view{"provider_id"}},
				  std::pair{&output.package_identity, std::string_view{"package_identity"}},
				  std::pair{&output.publisher, std::string_view{"publisher"}},
				  std::pair{&output.provider_binary_digest,
							std::string_view{"provider_binary_digest"}},
				  std::pair{&output.provider_semantic_contract_digest,
							std::string_view{"provider_semantic_contract_digest"}},
				  std::pair{&output.invalidation_contract,
							std::string_view{"invalidation_contract"}},
				  std::pair{&output.determinism_contract, std::string_view{"determinism_contract"}},
				  std::pair{&output.resource_class, std::string_view{"resource_class"}},
				  std::pair{&output.sandbox_minimum, std::string_view{"sandbox_minimum"}}})
				if (auto decoded = assign_member_text(*binding.first, *root, binding.second);
					!decoded)
					return unexpected(std::move(decoded.error()));
			for (const auto& [value, field] :
				 {std::pair{&output.provider_binary_digest,
							std::string_view{"provider_binary_digest"}},
				  std::pair{&output.provider_semantic_contract_digest,
							std::string_view{"provider_semantic_contract_digest"}},
				  std::pair{&output.invalidation_contract,
							std::string_view{"invalidation_contract"}},
				  std::pair{&output.determinism_contract,
							std::string_view{"determinism_contract"}}})
				if (!canonical_sha256_digest(*value))
					return unexpected(invalid(std::string{field}, "sha256-digest"));
			auto license = member_text(*root, "license", "license", 8192U);
			if (!license)
				return unexpected(std::move(license.error()));
			output.license = std::move(*license);

			auto provider_version_member =
				member(*root, {.name = "provider_version", .field = "provider_version"});
			if (!provider_version_member)
				return unexpected(std::move(provider_version_member.error()));
			auto provider_version =
				semantic_version_value(**provider_version_member, "provider_version");
			if (!provider_version)
				return unexpected(std::move(provider_version.error()));
			output.provider_version = *provider_version;
			auto signature = optional_signature(*root);
			if (!signature)
				return unexpected(std::move(signature.error()));
			output.signature = std::move(*signature);

			auto protocol = member(*root, {.name = "protocol_range", .field = "protocol_range"});
			if (!protocol)
				return unexpected(std::move(protocol.error()));
			constexpr std::array<std::string_view, 5U> protocol_fields{"major",
																	   "maximum_minor",
																	   "minimum_minor",
																	   "optional_features",
																	   "required_features"};
			if (auto valid = exact_members(**protocol, protocol_fields, "protocol_range"); !valid)
				return unexpected(std::move(valid.error()));
			for (auto binding :
				 {std::pair{&output.protocol.major, std::string_view{"major"}},
				  std::pair{&output.protocol.minimum_minor, std::string_view{"minimum_minor"}},
				  std::pair{&output.protocol.maximum_minor, std::string_view{"maximum_minor"}}})
				if (auto decoded = assign_member_unsigned_component(
						*binding.first, **protocol, binding.second);
					!decoded)
					return unexpected(std::move(decoded.error()));

			for (auto binding :
				 {std::pair{&output.platform_tuples, std::string_view{"platform_tuples"}},
				  std::pair{&output.offered_relations, std::string_view{"offered_relations"}},
				  std::pair{&output.required_relations, std::string_view{"required_relations"}},
				  std::pair{&output.interpretation_domains,
							std::string_view{"interpretation_domains"}},
				  std::pair{&output.requested_qualifications,
							std::string_view{"requested_qualifications"}},
				  std::pair{&output.trust_flags, std::string_view{"trust_flags"}}})
				if (auto decoded =
						assign_member_string_array(*binding.first, *root, binding.second);
					!decoded)
					return unexpected(std::move(decoded.error()));
			auto required_features =
				member_string_array(**protocol, "required_features", "required_features");
			if (!required_features)
				return unexpected(std::move(required_features.error()));
			output.protocol.required_features = std::move(*required_features);
			auto optional_features =
				member_string_array(**protocol, "optional_features", "optional_features");
			if (!optional_features)
				return unexpected(std::move(optional_features.error()));
			output.protocol.optional_features = std::move(*optional_features);

			auto task_stage = member(*root, {.name = "task_stage", .field = "task_stage"});
			if (!task_stage)
				return unexpected(std::move(task_stage.error()));
			constexpr std::array<std::string_view, 2U> task_fields{"input", "output"};
			if (auto valid = exact_members(**task_stage, task_fields, "task_stage"); !valid)
				return unexpected(std::move(valid.error()));
			auto input_stage = member_text(**task_stage, "input", "task_stage.input");
			if (!input_stage)
				return unexpected(std::move(input_stage.error()));
			output.task_input_stage = std::move(*input_stage);
			auto output_stage = member_text(**task_stage, "output", "task_stage.output");
			if (!output_stage)
				return unexpected(std::move(output_stage.error()));
			output.task_output_stage = std::move(*output_stage);

			if (auto valid = output.validate(); !valid)
				return unexpected(invalid(valid.error().field, valid.error().detail));
			if (output.canonical_json() != canonical_bytes)
				return unexpected(invalid("manifest", "noncanonical-value"));
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(invalid("manifest", "allocation-failed"));
		}
		catch (const std::length_error&)
		{
			return unexpected(invalid("manifest", "allocation-length"));
		}
	}
} // namespace cxxlens::sdk::detail
