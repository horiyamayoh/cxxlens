#include "worker_observation_codec.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "protocol_v2/cbor.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	namespace
	{
		using namespace protocol_v2;
		constexpr std::string_view schema = "cxxlens.clang23-gcc-replay-observations.v4";

		[[nodiscard]] sdk::error invalid(std::string field, std::string detail)
		{
			return {"application-analysis.replay-observation-codec-invalid",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::error resource(std::string field, std::string detail)
		{
			return {"application-analysis.replay-observation-codec-resource-limit",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] std::size_t wire_items(const worker_observation_codec_limits& limits)
		{
			return limits.maximum_observations * 20U + 64U;
		}

		[[nodiscard]] cbor::limits cbor_limits(const worker_observation_codec_limits& limits)
		{
			return {limits.maximum_bytes, 8U, wire_items(limits), limits.maximum_text_bytes, 0U};
		}

		[[nodiscard]] cbor::scan_limits scan_limits(const worker_observation_codec_limits& limits)
		{
			return {limits.maximum_bytes,
					8U,
					wire_items(limits),
					limits.maximum_observations,
					16U,
					limits.maximum_text_bytes,
					0U,
					true};
		}

		[[nodiscard]] cbor::value text(std::string value)
		{
			return cbor::value{std::move(value)};
		}

		[[nodiscard]] cbor::value unsigned_integer(const std::size_t value)
		{
			return cbor::value{static_cast<std::uint64_t>(value)};
		}

		[[nodiscard]] cbor::value span_value(const observed_source_span& value)
		{
			return cbor::value{cbor::array{text(value.logical_path),
										   cbor::value{value.begin},
										   cbor::value{value.end},
										   text(value.role)}};
		}

		[[nodiscard]] cbor::value origin_value(const observed_source_origin& value)
		{
			return cbor::value{cbor::array{text(value.kind),
										   text(value.logical_path),
										   cbor::value{value.begin},
										   cbor::value{value.end},
										   cbor::value{value.read_only}}};
		}

		[[nodiscard]] cbor::array entity_values(const observation_batch& observations)
		{
			cbor::array output;
			output.reserve(observations.entities.size());
			for (const auto& value : observations.entities)
				output.emplace_back(cbor::array{text(value.provider_local_key),
												text(value.kind),
												text(value.qualified_name),
												text(value.canonical_type),
												span_value(value.source),
												cbor::value{value.definition}});
			return output;
		}

		[[nodiscard]] cbor::array declaration_values(const observation_batch& observations)
		{
			cbor::array output;
			output.reserve(observations.declarations.size());
			for (const auto& value : observations.declarations)
			{
				cbor::array attributes;
				attributes.reserve(value.attributes.size());
				for (const auto& attribute : value.attributes)
					attributes.emplace_back(attribute);
				output.emplace_back(cbor::array{text(value.entity_provider_local_key),
												text(value.kind),
												text(value.storage),
												text(value.linkage),
												cbor::value{std::move(attributes)},
												span_value(value.source),
												cbor::value{value.implicit},
												cbor::value{value.deleted},
												cbor::value{value.defaulted},
												cbor::value{value.friend_declaration},
												cbor::value{value.exported}});
			}
			return output;
		}

		[[nodiscard]] cbor::array type_values(const observation_batch& observations)
		{
			cbor::array output;
			output.reserve(observations.types.size());
			for (const auto& value : observations.types)
			{
				cbor::value structure{nullptr};
				if (value.structure)
				{
					cbor::array qualifiers;
					for (const auto& qualifier : value.structure->qualifiers)
						qualifiers.emplace_back(qualifier);
					cbor::array components;
					for (const auto& component : value.structure->components)
					{
						cbor::array component_qualifiers;
						for (const auto& qualifier : component.qualifiers)
							component_qualifiers.emplace_back(qualifier);
						components.emplace_back(cbor::array{
							text(component.role),
							cbor::value{component.ordinal},
							text(component.constructor),
							cbor::value{std::move(component_qualifiers)},
						});
					}
					structure = cbor::value{cbor::array{
						cbor::value{std::move(qualifiers)},
						cbor::value{std::move(components)},
						text(value.structure->calling_convention),
						text(value.structure->exception_specification),
						text(value.structure->ref_qualifier),
						cbor::value{value.structure->variadic},
					}};
				}
				output.emplace_back(cbor::array{
					text(value.provider_local_key),
					text(value.owning_entity_provider_local_key),
					text(value.constructor),
					text(value.canonical_spelling),
					cbor::value{value.dependent},
					std::move(structure),
					value.unavailable_reason ? text(*value.unavailable_reason)
											 : cbor::value{nullptr},
				});
			}
			return output;
		}

		[[nodiscard]] cbor::array direct_call_values(const observation_batch& observations)
		{
			cbor::array output;
			output.reserve(observations.direct_calls.size());
			for (const auto& value : observations.direct_calls)
			{
				cbor::array origins;
				origins.reserve(value.origins.size());
				for (const auto& origin : value.origins)
					origins.push_back(origin_value(origin));
				output.emplace_back(cbor::array{
					value.caller_provider_local_key ? text(*value.caller_provider_local_key)
													: cbor::value{nullptr},
					text(value.target_provider_local_key),
					text(value.kind),
					span_value(value.source),
					cbor::value{std::move(origins)},
				});
			}
			return output;
		}

		[[nodiscard]] cbor::array limitation_values(const observation_batch& observations)
		{
			cbor::array output;
			output.reserve(observations.limitations.size());
			for (const auto& value : observations.limitations)
				output.emplace_back(value);
			return output;
		}

		[[nodiscard]] sdk::result<void> add_text(const std::string_view value,
												 std::size_t& total,
												 const std::string_view field,
												 const worker_observation_codec_limits& limits)
		{
			if (value.empty() || value.size() > limits.maximum_text_bytes ||
				!cbor::valid_utf8(value))
				return sdk::unexpected(invalid(std::string{field}, "text"));
			if (value.size() > limits.maximum_logical_bytes - total)
				return sdk::unexpected(resource("logical-bytes", std::string{field}));
			total += value.size();
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_span(const observed_source_span& value,
													  std::size_t& total,
													  const worker_observation_codec_limits& limits)
		{
			if (auto valid = add_text(value.logical_path, total, "source.logical_path", limits);
				!valid)
				return valid;
			if (auto valid = add_text(value.role, total, "source.role", limits); !valid)
				return valid;
			if (!value.logical_path.starts_with("project://") || value.end < value.begin ||
				(value.role != "spelling" && value.role != "expansion"))
				return sdk::unexpected(invalid("source", "range"));
			return {};
		}

		[[nodiscard]] bool supported_type_qualifier(const std::string_view value)
		{
			return value == "const" || value == "restrict" || value == "volatile";
		}

		[[nodiscard]] bool supported_builtin_constructor(const std::string_view value)
		{
			return value == "builtin.void" || value == "builtin.bool" ||
				value == "builtin.char-unsigned" || value == "builtin.char-signed" ||
				value == "builtin.unsigned-char" || value == "builtin.signed-char" ||
				value == "builtin.wchar-unsigned" || value == "builtin.wchar-signed" ||
				value == "builtin.char8" || value == "builtin.char16" ||
				value == "builtin.char32" || value == "builtin.unsigned-short" ||
				value == "builtin.signed-short" || value == "builtin.unsigned-int" ||
				value == "builtin.signed-int" || value == "builtin.unsigned-long" ||
				value == "builtin.signed-long" || value == "builtin.unsigned-long-long" ||
				value == "builtin.signed-long-long" || value == "builtin.unsigned-int128" ||
				value == "builtin.signed-int128" || value == "builtin.half" ||
				value == "builtin.float" || value == "builtin.double" ||
				value == "builtin.long-double" || value == "builtin.float16" ||
				value == "builtin.bfloat16" || value == "builtin.float128" ||
				value == "builtin.ibm128" || value == "builtin.nullptr";
		}

		[[nodiscard]] sdk::result<void>
		validate_qualifiers(const std::vector<std::string>& values,
							std::size_t& total,
							const worker_observation_codec_limits& limits)
		{
			if (!std::ranges::is_sorted(values) ||
				std::ranges::adjacent_find(values) != values.end())
				return sdk::unexpected(invalid("type.qualifiers", "canonical-order"));
			for (const auto& value : values)
			{
				if (!supported_type_qualifier(value))
					return sdk::unexpected(invalid("type.qualifiers", "symbol"));
				if (auto valid = add_text(value, total, "type.qualifier", limits); !valid)
					return valid;
			}
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_output_impl(const worker_observation_output& value,
							 const worker_observation_codec_limits& limits)
		{
			if (value.replay_input_digest.size() != 71U ||
				!value.replay_input_digest.starts_with("sha256:") ||
				!std::ranges::all_of(std::string_view{value.replay_input_digest}.substr(7U),
									 [](const char byte)
									 {
										 return (byte >= '0' && byte <= '9') ||
											 (byte >= 'a' && byte <= 'f');
									 }))
				return sdk::unexpected(invalid("replay_input_digest", "sha256"));
			for (const auto [field, count] : {
					 std::pair{std::string_view{"declaration_count"}, value.declaration_count},
					 std::pair{std::string_view{"warning_count"}, value.warning_count},
					 std::pair{std::string_view{"error_count"}, value.error_count},
				 })
				if (count > observer_product_maximum_traversal_entries)
					return sdk::unexpected(resource(std::string{field}, "count"));
			std::size_t count{};
			for (const auto size : {value.observations.entities.size(),
									value.observations.declarations.size(),
									value.observations.types.size(),
									value.observations.direct_calls.size(),
									value.observations.limitations.size()})
			{
				if (size > limits.maximum_observations - count)
					return sdk::unexpected(resource("observations", "count"));
				count += size;
			}
			std::size_t total{};
			for (const auto& item : value.observations.entities)
			{
				for (const auto field : {std::string_view{item.provider_local_key},
										 std::string_view{item.kind},
										 std::string_view{item.qualified_name},
										 std::string_view{item.canonical_type}})
					if (auto valid = add_text(field, total, "entity", limits); !valid)
						return valid;
				if (auto valid = validate_span(item.source, total, limits); !valid)
					return valid;
			}
			for (const auto& item : value.observations.declarations)
			{
				for (const auto field : {std::string_view{item.entity_provider_local_key},
										 std::string_view{item.kind},
										 std::string_view{item.storage},
										 std::string_view{item.linkage}})
					if (auto valid = add_text(field, total, "declaration", limits); !valid)
						return valid;
				if (!std::ranges::is_sorted(item.attributes) ||
					std::ranges::adjacent_find(item.attributes) != item.attributes.end())
					return sdk::unexpected(invalid("declaration.attributes", "canonical-order"));
				for (const auto& attribute : item.attributes)
					if (auto valid = add_text(attribute, total, "declaration.attribute", limits);
						!valid)
						return valid;
				if (auto valid = validate_span(item.source, total, limits); !valid)
					return valid;
			}
			for (const auto& item : value.observations.types)
			{
				for (const auto field : {std::string_view{item.provider_local_key},
										 std::string_view{item.owning_entity_provider_local_key},
										 std::string_view{item.constructor},
										 std::string_view{item.canonical_spelling}})
					if (auto valid = add_text(field, total, "type", limits); !valid)
						return valid;
				if (item.constructor != "function" ||
					item.structure.has_value() == item.unavailable_reason.has_value())
					return sdk::unexpected(invalid("type", "structural-state"));
				if (item.unavailable_reason)
				{
					if (auto valid =
							add_text(*item.unavailable_reason, total, "type.unavailable", limits);
						!valid)
						return valid;
					continue;
				}
				if (item.dependent)
					return sdk::unexpected(invalid("type", "dependent-structure"));
				const auto& structure = *item.structure;
				if (structure.calling_convention != "c" &&
					structure.calling_convention != "x86-stdcall" &&
					structure.calling_convention != "x86-fastcall" &&
					structure.calling_convention != "x86-thiscall" &&
					structure.calling_convention != "x86-vectorcall" &&
					structure.calling_convention != "win64" &&
					structure.calling_convention != "x86-64-sysv")
					return sdk::unexpected(invalid("type.calling_convention", "symbol"));
				if (structure.exception_specification != "none" &&
					structure.exception_specification != "dynamic-none" &&
					structure.exception_specification != "ms-nothrow" &&
					structure.exception_specification != "noexcept" &&
					structure.exception_specification != "noexcept-false" &&
					structure.exception_specification != "noexcept-true")
					return sdk::unexpected(invalid("type.exception_specification", "symbol"));
				if (structure.ref_qualifier != "none" && structure.ref_qualifier != "lvalue" &&
					structure.ref_qualifier != "rvalue")
					return sdk::unexpected(invalid("type.ref_qualifier", "symbol"));
				for (const auto field : {std::string_view{structure.calling_convention},
										 std::string_view{structure.exception_specification},
										 std::string_view{structure.ref_qualifier}})
					if (auto valid = add_text(field, total, "type.structure", limits); !valid)
						return valid;
				if (auto valid = validate_qualifiers(structure.qualifiers, total, limits); !valid)
					return valid;
				if (structure.components.empty() ||
					structure.components.size() > limits.maximum_observations - count)
					return sdk::unexpected(resource("observations", "type-component-count"));
				count += structure.components.size();
				for (std::size_t index{}; index < structure.components.size(); ++index)
				{
					const auto& component = structure.components[index];
					const auto expected_role =
						index == 0U ? std::string_view{"result"} : std::string_view{"parameter"};
					const auto expected_ordinal = index == 0U ? 0U : index - 1U;
					if (component.role != expected_role || component.ordinal != expected_ordinal ||
						!supported_builtin_constructor(component.constructor))
						return sdk::unexpected(invalid("type.component", "canonical-shape"));
					for (const auto field : {std::string_view{component.role},
											 std::string_view{component.constructor}})
						if (auto valid = add_text(field, total, "type.component", limits); !valid)
							return valid;
					if (auto valid = validate_qualifiers(component.qualifiers, total, limits);
						!valid)
						return valid;
				}
			}
			for (const auto& item : value.observations.direct_calls)
			{
				if (item.caller_provider_local_key)
					if (auto valid =
							add_text(*item.caller_provider_local_key, total, "call.caller", limits);
						!valid)
						return valid;
				for (const auto field : {std::string_view{item.target_provider_local_key},
										 std::string_view{item.kind}})
					if (auto valid = add_text(field, total, "call", limits); !valid)
						return valid;
				if (auto valid = validate_span(item.source, total, limits); !valid)
					return valid;
				if (item.origins.size() > limits.maximum_observations - count)
					return sdk::unexpected(resource("observations", "origin-count"));
				count += item.origins.size();
				for (const auto& origin : item.origins)
				{
					for (const auto field :
						 {std::string_view{origin.kind}, std::string_view{origin.logical_path}})
						if (auto valid = add_text(field, total, "call.origin", limits); !valid)
							return valid;
					if (!origin.logical_path.starts_with("project://") ||
						origin.end < origin.begin || !origin.read_only ||
						(origin.kind != "macro-spelling" && origin.kind != "macro-spelling-begin" &&
						 origin.kind != "macro-spelling-end"))
						return sdk::unexpected(invalid("call.origin", "range"));
				}
			}
			for (const auto& item : value.observations.limitations)
				if (auto valid = add_text(item, total, "limitation", limits); !valid)
					return valid;
			if (value.observations.traversal_entries > observer_product_maximum_traversal_entries)
				return sdk::unexpected(resource("traversal_entries", "count"));
			return {};
		}

		template <class value_type>
		[[nodiscard]] sdk::result<const value_type*> required(const cbor::map& fields,
															  const std::string_view field)
		{
			const auto* value = cbor::find(fields, field);
			if (value == nullptr)
				return sdk::unexpected(invalid(std::string{field}, "missing"));
			const auto* typed = std::get_if<value_type>(&value->data);
			if (typed == nullptr)
				return sdk::unexpected(invalid(std::string{field}, "type"));
			return typed;
		}

		[[nodiscard]] sdk::result<std::size_t> size_value(const cbor::map& fields,
														  const std::string_view field)
		{
			auto value = required<std::uint64_t>(fields, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			if (**value > std::numeric_limits<std::size_t>::max())
				return sdk::unexpected(resource(std::string{field}, "size_t"));
			return static_cast<std::size_t>(**value);
		}

		[[nodiscard]] sdk::result<const cbor::array*>
		fixed_array(const cbor::value& value, const std::size_t size, const std::string_view field)
		{
			const auto* array = std::get_if<cbor::array>(&value.data);
			if (array == nullptr || array->size() != size)
				return sdk::unexpected(invalid(std::string{field}, "shape"));
			return array;
		}

		template <class value_type>
		[[nodiscard]] sdk::result<const value_type*>
		item(const cbor::array& values, const std::size_t index, const std::string_view field)
		{
			const auto* typed = std::get_if<value_type>(&values[index].data);
			if (typed == nullptr)
				return sdk::unexpected(invalid(std::string{field}, "type"));
			return typed;
		}

		[[nodiscard]] sdk::result<observed_source_span> decode_span(const cbor::value& value)
		{
			auto fields = fixed_array(value, 4U, "source");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto path = item<std::string>(**fields, 0U, "source.logical_path");
			auto begin = item<std::uint64_t>(**fields, 1U, "source.begin");
			auto end = item<std::uint64_t>(**fields, 2U, "source.end");
			auto role = item<std::string>(**fields, 3U, "source.role");
			if (!path || !begin || !end || !role)
				return sdk::unexpected(invalid("source", "field"));
			return observed_source_span{**path, **begin, **end, **role};
		}

		[[nodiscard]] sdk::result<observed_source_origin> decode_origin(const cbor::value& value)
		{
			auto fields = fixed_array(value, 5U, "call.origin");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto kind = item<std::string>(**fields, 0U, "call.origin.kind");
			auto path = item<std::string>(**fields, 1U, "call.origin.logical_path");
			auto begin = item<std::uint64_t>(**fields, 2U, "call.origin.begin");
			auto end = item<std::uint64_t>(**fields, 3U, "call.origin.end");
			auto read_only = item<bool>(**fields, 4U, "call.origin.read_only");
			if (!kind || !path || !begin || !end || !read_only)
				return sdk::unexpected(invalid("call.origin", "field"));
			return observed_source_origin{**kind, **path, **begin, **end, **read_only};
		}

		[[nodiscard]] sdk::result<std::vector<std::string>>
		decode_text_array(const cbor::value& value, const std::string_view field)
		{
			const auto* encoded = std::get_if<cbor::array>(&value.data);
			if (encoded == nullptr)
				return sdk::unexpected(invalid(std::string{field}, "type"));
			std::vector<std::string> output;
			output.reserve(encoded->size());
			for (const auto& item : *encoded)
			{
				const auto* text = std::get_if<std::string>(&item.data);
				if (text == nullptr)
					return sdk::unexpected(invalid(std::string{field}, "item-type"));
				output.push_back(*text);
			}
			return output;
		}

		[[nodiscard]] sdk::result<observed_type::function_structure>
		decode_type_structure(const cbor::value& value)
		{
			auto fields = fixed_array(value, 6U, "type.structure");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto qualifiers = decode_text_array((**fields)[0U], "type.qualifiers");
			auto components = item<cbor::array>(**fields, 1U, "type.components");
			auto convention = item<std::string>(**fields, 2U, "type.calling_convention");
			auto exceptions = item<std::string>(**fields, 3U, "type.exception_specification");
			auto ref = item<std::string>(**fields, 4U, "type.ref_qualifier");
			auto variadic = item<bool>(**fields, 5U, "type.variadic");
			if (!qualifiers || !components || !convention || !exceptions || !ref || !variadic)
				return sdk::unexpected(invalid("type.structure", "field"));
			observed_type::function_structure output;
			output.qualifiers = std::move(*qualifiers);
			output.calling_convention = **convention;
			output.exception_specification = **exceptions;
			output.ref_qualifier = **ref;
			output.variadic = **variadic;
			output.components.reserve((*components)->size());
			for (const auto& encoded : **components)
			{
				auto component = fixed_array(encoded, 4U, "type.component");
				if (!component)
					return sdk::unexpected(std::move(component.error()));
				auto role = item<std::string>(**component, 0U, "type.component.role");
				auto ordinal = item<std::uint64_t>(**component, 1U, "type.component.ordinal");
				auto constructor = item<std::string>(**component, 2U, "type.component.constructor");
				auto component_qualifiers =
					decode_text_array((**component)[3U], "type.component.qualifiers");
				if (!role || !ordinal || !constructor || !component_qualifiers)
					return sdk::unexpected(invalid("type.component", "field"));
				output.components.push_back(
					{**role, **ordinal, **constructor, std::move(*component_qualifiers)});
			}
			return output;
		}

		[[nodiscard]] sdk::result<observation_batch> decode_batch(const cbor::map& root)
		{
			observation_batch output;
			auto entities = required<cbor::array>(root, "entities");
			auto declarations = required<cbor::array>(root, "declarations");
			auto types = required<cbor::array>(root, "types");
			auto calls = required<cbor::array>(root, "direct_calls");
			auto limitations = required<cbor::array>(root, "limitations");
			auto traversal = size_value(root, "traversal_entries");
			if (!entities || !declarations || !types || !calls || !limitations || !traversal)
				return sdk::unexpected(invalid("observations", "field"));

			output.entities.reserve((*entities)->size());
			for (const auto& encoded : **entities)
			{
				auto fields = fixed_array(encoded, 6U, "entity");
				if (!fields)
					return sdk::unexpected(std::move(fields.error()));
				auto key = item<std::string>(**fields, 0U, "entity.key");
				auto kind = item<std::string>(**fields, 1U, "entity.kind");
				auto name = item<std::string>(**fields, 2U, "entity.name");
				auto type = item<std::string>(**fields, 3U, "entity.type");
				auto source = decode_span((**fields)[4U]);
				auto definition = item<bool>(**fields, 5U, "entity.definition");
				if (!key || !kind || !name || !type || !source || !definition)
					return sdk::unexpected(invalid("entity", "field"));
				output.entities.push_back(
					{**key, **kind, **name, **type, std::move(*source), **definition});
			}

			output.declarations.reserve((*declarations)->size());
			for (const auto& encoded : **declarations)
			{
				auto fields = fixed_array(encoded, 11U, "declaration");
				if (!fields)
					return sdk::unexpected(std::move(fields.error()));
				auto key = item<std::string>(**fields, 0U, "declaration.key");
				auto kind = item<std::string>(**fields, 1U, "declaration.kind");
				auto storage = item<std::string>(**fields, 2U, "declaration.storage");
				auto linkage = item<std::string>(**fields, 3U, "declaration.linkage");
				auto encoded_attributes = item<cbor::array>(**fields, 4U, "declaration.attributes");
				if (!encoded_attributes)
					return sdk::unexpected(std::move(encoded_attributes.error()));
				std::vector<std::string> attributes;
				attributes.reserve((*encoded_attributes)->size());
				for (const auto& encoded_attribute : **encoded_attributes)
				{
					const auto* attribute = std::get_if<std::string>(&encoded_attribute.data);
					if (attribute == nullptr)
						return sdk::unexpected(invalid("declaration.attribute", "type"));
					attributes.push_back(*attribute);
				}
				auto source = decode_span((**fields)[5U]);
				auto implicit = item<bool>(**fields, 6U, "declaration.implicit");
				auto deleted = item<bool>(**fields, 7U, "declaration.deleted");
				auto defaulted = item<bool>(**fields, 8U, "declaration.defaulted");
				auto friend_declaration = item<bool>(**fields, 9U, "declaration.friend");
				auto exported = item<bool>(**fields, 10U, "declaration.exported");
				if (!key || !kind || !storage || !linkage || !source || !implicit || !deleted ||
					!defaulted || !friend_declaration || !exported)
					return sdk::unexpected(invalid("declaration", "field"));
				output.declarations.push_back({**key,
											   **kind,
											   **storage,
											   **linkage,
											   std::move(attributes),
											   std::move(*source),
											   **implicit,
											   **deleted,
											   **defaulted,
											   **friend_declaration,
											   **exported});
			}

			output.types.reserve((*types)->size());
			for (const auto& encoded : **types)
			{
				auto fields = fixed_array(encoded, 7U, "type");
				if (!fields)
					return sdk::unexpected(std::move(fields.error()));
				auto key = item<std::string>(**fields, 0U, "type.key");
				auto owner = item<std::string>(**fields, 1U, "type.owner");
				auto constructor = item<std::string>(**fields, 2U, "type.constructor");
				auto spelling = item<std::string>(**fields, 3U, "type.spelling");
				auto dependent = item<bool>(**fields, 4U, "type.dependent");
				if (!key || !owner || !constructor || !spelling || !dependent)
					return sdk::unexpected(invalid("type", "field"));
				std::optional<observed_type::function_structure> structure;
				if (!std::holds_alternative<std::monostate>((**fields)[5U].data))
				{
					auto decoded = decode_type_structure((**fields)[5U]);
					if (!decoded)
						return sdk::unexpected(std::move(decoded.error()));
					structure = std::move(*decoded);
				}
				std::optional<std::string> unavailable_reason;
				if (const auto* reason = std::get_if<std::string>(&(**fields)[6U].data))
					unavailable_reason = *reason;
				else if (!std::holds_alternative<std::monostate>((**fields)[6U].data))
					return sdk::unexpected(invalid("type.unavailable", "type"));
				output.types.push_back({**key,
										**owner,
										**constructor,
										**spelling,
										**dependent,
										std::move(structure),
										std::move(unavailable_reason)});
			}

			output.direct_calls.reserve((*calls)->size());
			for (const auto& encoded : **calls)
			{
				auto fields = fixed_array(encoded, 5U, "direct_call");
				if (!fields)
					return sdk::unexpected(std::move(fields.error()));
				std::optional<std::string> caller;
				if (const auto* value = std::get_if<std::string>(&(**fields)[0U].data))
					caller = *value;
				else if (!std::holds_alternative<std::monostate>((**fields)[0U].data))
					return sdk::unexpected(invalid("direct_call.caller", "type"));
				auto target = item<std::string>(**fields, 1U, "direct_call.target");
				auto kind = item<std::string>(**fields, 2U, "direct_call.kind");
				auto source = decode_span((**fields)[3U]);
				auto encoded_origins = item<cbor::array>(**fields, 4U, "direct_call.origins");
				if (!target || !kind || !source || !encoded_origins)
					return sdk::unexpected(invalid("direct_call", "field"));
				std::vector<observed_source_origin> origins;
				origins.reserve((*encoded_origins)->size());
				for (const auto& encoded_origin : **encoded_origins)
				{
					auto origin = decode_origin(encoded_origin);
					if (!origin)
						return sdk::unexpected(std::move(origin.error()));
					origins.push_back(std::move(*origin));
				}
				output.direct_calls.push_back(
					{std::move(caller), **target, **kind, std::move(*source), std::move(origins)});
			}

			output.limitations.reserve((*limitations)->size());
			for (const auto& encoded : **limitations)
			{
				const auto* value = std::get_if<std::string>(&encoded.data);
				if (value == nullptr)
					return sdk::unexpected(invalid("limitation", "type"));
				output.limitations.push_back(*value);
			}
			output.traversal_entries = *traversal;
			return output;
		}
	} // namespace

	sdk::result<void> worker_observation_codec_limits::validate() const
	{
		const worker_observation_codec_limits product{};
		if (maximum_bytes == 0U || maximum_bytes > product.maximum_bytes ||
			maximum_text_bytes == 0U || maximum_text_bytes > product.maximum_text_bytes ||
			maximum_logical_bytes == 0U || maximum_logical_bytes > product.maximum_logical_bytes ||
			maximum_observations == 0U || maximum_observations > product.maximum_observations)
			return sdk::unexpected(resource("limits", "outside-product-bound"));
		return {};
	}

	sdk::result<void>
	validate_worker_observation_output(const worker_observation_output& value,
									   const worker_observation_codec_limits limits)
	{
		if (auto valid = limits.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (value.error_count != 0U)
			return sdk::unexpected(invalid("error_count", "nonzero-success-output"));
		return validate_output_impl(value, limits);
	}

	sdk::result<std::vector<std::byte>>
	encode_worker_observations(const std::string_view replay_input_digest,
							   const parse_result& parsed,
							   const worker_observation_codec_limits limits)
	{
		try
		{
			if (auto valid = limits.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (parsed.terminal != parse_terminal::parsed || parsed.error_count != 0U)
				return sdk::unexpected(invalid("parse_terminal", "not-successful"));
			worker_observation_output candidate{std::string{replay_input_digest},
												parsed.declaration_count,
												parsed.warning_count,
												parsed.error_count,
												parsed.observations};
			if (auto valid = validate_worker_observation_output(candidate, limits); !valid)
				return sdk::unexpected(std::move(valid.error()));
			cbor::map root{
				{"schema", text(std::string{schema})},
				{"replay_input_digest", text(candidate.replay_input_digest)},
				{"declaration_count", unsigned_integer(candidate.declaration_count)},
				{"warning_count", unsigned_integer(candidate.warning_count)},
				{"error_count", unsigned_integer(candidate.error_count)},
				{"traversal_entries", unsigned_integer(candidate.observations.traversal_entries)},
				{"entities", cbor::value{entity_values(candidate.observations)}},
				{"declarations", cbor::value{declaration_values(candidate.observations)}},
				{"types", cbor::value{type_values(candidate.observations)}},
				{"direct_calls", cbor::value{direct_call_values(candidate.observations)}},
				{"limitations", cbor::value{limitation_values(candidate.observations)}},
			};
			auto encoded = cbor::encode(cbor::value{std::move(root)}, cbor_limits(limits));
			if (!encoded)
				return sdk::unexpected(resource("output", encoded.error().detail));
			return encoded;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(resource("output", "allocation"));
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(resource("output", "allocation-length"));
		}
	}

	sdk::result<worker_observation_output>
	decode_worker_observations(const std::span<const std::byte> bytes,
							   const worker_observation_codec_limits limits)
	{
		try
		{
			if (auto valid = limits.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (!cbor::scan_canonical(bytes, scan_limits(limits)))
				return sdk::unexpected(invalid("binary", "noncanonical-or-bounded-cbor"));
			auto decoded = cbor::decode(bytes, cbor_limits(limits));
			if (!decoded)
				return sdk::unexpected(invalid("binary", decoded.error().detail));
			const auto* root = std::get_if<cbor::map>(&decoded->data);
			if (root == nullptr)
				return sdk::unexpected(invalid("root", "map"));
			if (auto keys = cbor::require_keys(*root,
											   {"schema",
												"replay_input_digest",
												"declaration_count",
												"warning_count",
												"error_count",
												"traversal_entries",
												"entities",
												"declarations",
												"types",
												"direct_calls",
												"limitations"});
				!keys)
				return sdk::unexpected(invalid("root", keys.error().detail));
			auto schema_value = required<std::string>(*root, "schema");
			auto digest = required<std::string>(*root, "replay_input_digest");
			auto declarations = size_value(*root, "declaration_count");
			auto warnings = size_value(*root, "warning_count");
			auto errors = size_value(*root, "error_count");
			auto observations = decode_batch(*root);
			if (!schema_value || **schema_value != schema || !digest || !declarations ||
				!warnings || !errors || !observations)
				return sdk::unexpected(invalid("root", "field"));
			worker_observation_output output{
				**digest, *declarations, *warnings, *errors, std::move(*observations)};
			if (auto valid = validate_worker_observation_output(output, limits); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(resource("input", "allocation"));
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(resource("input", "allocation-length"));
		}
	}
} // namespace cxxlens::detail::clang23_gcc_replay
