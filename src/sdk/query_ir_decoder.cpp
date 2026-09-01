#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/query.hpp>

#include "bounded_json_internal.hpp"

namespace cxxlens::sdk::query
{
	namespace
	{
		using json_value = cxxlens::sdk::detail::json_value;

		[[nodiscard]] error decode_error(std::string field, std::string detail)
		{
			return {"sdk.query-argument-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] result<const json_value::object_type*> as_object(const json_value& value,
																	   const std::string_view field)
		{
			if (const auto* output = value.as_object())
				return output;
			return unexpected(decode_error(std::string{field}, "object-required"));
		}

		[[nodiscard]] result<const json_value::array_type*> as_array(const json_value& value,
																	 const std::string_view field)
		{
			if (const auto* output = value.as_array())
				return output;
			return unexpected(decode_error(std::string{field}, "array-required"));
		}

		[[nodiscard]] result<std::string> as_string(const json_value& value,
													const std::string_view field)
		{
			if (const auto* output = value.as_string())
				return *output;
			return unexpected(decode_error(std::string{field}, "string-required"));
		}

		[[nodiscard]] result<std::uint64_t> as_unsigned(const json_value& value,
														const std::string_view field)
		{
			if (const auto* output = value.as_unsigned_integer())
				return *output;
			return unexpected(decode_error(std::string{field}, "unsigned-required"));
		}

		[[nodiscard]] result<const json_value*> required(const json_value::object_type& object,
														 const std::string_view name)
		{
			const auto found = object.find(name);
			if (found == object.end())
				return unexpected(decode_error(std::string{name}, "missing"));
			return &found->second;
		}

		[[nodiscard]] result<void> exact_keys(const json_value::object_type& object,
											  const std::initializer_list<std::string_view> keys,
											  const std::string_view field)
		{
			std::set<std::string, std::less<>> expected;
			for (const auto key : keys)
				expected.emplace(key);
			std::set<std::string, std::less<>> actual;
			for (const auto& [key, value] : object)
			{
				(void)value;
				actual.insert(key);
			}
			if (actual != expected)
				return unexpected(decode_error(std::string{field}, "key-set"));
			return {};
		}

		[[nodiscard]] result<ir_column_ref> column(const json_value& value)
		{
			auto object = as_object(value, "column");
			if (!object ||
				!exact_keys(**object, {"availability", "column_id", "source_alias"}, "column"))
				return unexpected(decode_error("column", "shape"));
			auto identifier_value = required(**object, "column_id");
			auto availability_value = required(**object, "availability");
			auto alias_value = required(**object, "source_alias");
			if (!identifier_value || !availability_value || !alias_value)
				return unexpected(decode_error("column", "missing"));
			auto identifier = as_string(**identifier_value, "column_id");
			auto availability = as_string(**availability_value, "availability");
			auto alias = as_string(**alias_value, "source_alias");
			if (!identifier || identifier->empty() || !availability || !alias)
				return unexpected(decode_error("column", "value"));
			if (*availability == "require")
				return ir_column_ref{
					std::move(*identifier), column_availability::require, std::move(*alias)};
			if (*availability == "absent_if_schema_missing")
				return ir_column_ref{std::move(*identifier),
									 column_availability::absent_if_schema_missing,
									 std::move(*alias)};
			return unexpected(decode_error("availability", *availability));
		}

		[[nodiscard]] result<std::vector<std::byte>> hex_bytes(const std::string_view value)
		{
			if (value.size() % 2U != 0U)
				return unexpected(decode_error("literal.value", "odd-hex"));
			auto nibble = [](const char byte) -> int
			{
				if (byte >= '0' && byte <= '9')
					return byte - '0';
				if (byte >= 'a' && byte <= 'f')
					return byte - 'a' + 10;
				return -1;
			};
			std::vector<std::byte> output;
			output.reserve(value.size() / 2U);
			for (std::size_t index = 0U; index < value.size(); index += 2U)
			{
				const auto high = nibble(value[index]);
				const auto low = nibble(value[index + 1U]);
				if (high < 0 || low < 0)
					return unexpected(decode_error("literal.value", "hex"));
				output.push_back(static_cast<std::byte>((high << 4) | low));
			}
			return output;
		}

		[[nodiscard]] result<ir_literal> typed_literal(const json_value& value)
		{
			auto object = as_object(value, "literal");
			if (!object || !exact_keys(**object, {"type", "value"}, "literal"))
				return unexpected(decode_error("literal", "shape"));
			auto type_value = required(**object, "type");
			auto scalar = required(**object, "value");
			if (!type_value || !scalar)
				return unexpected(decode_error("literal", "missing"));
			auto type = as_string(**type_value, "literal.type");
			if (!type || type->empty() || type->starts_with("optional<"))
				return unexpected(decode_error("literal.type", "present-type-required"));
			scalar_value decoded;
			if (*type == "bool")
			{
				const auto* boolean = (*scalar)->as_boolean();
				if (boolean == nullptr)
					return unexpected(decode_error("literal.value", "bool"));
				decoded = *boolean;
			}
			else if (*type == "int64")
			{
				if (const auto* signed_value = (*scalar)->as_signed_integer())
					decoded = *signed_value;
				else if (const auto* unsigned_value = (*scalar)->as_unsigned_integer();
						 unsigned_value != nullptr &&
						 *unsigned_value <=
							 static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
					decoded = static_cast<std::int64_t>(*unsigned_value);
				else
					return unexpected(decode_error("literal.value", "int64"));
			}
			else if (*type == "uint64")
			{
				const auto* unsigned_value = (*scalar)->as_unsigned_integer();
				if (unsigned_value == nullptr)
					return unexpected(decode_error("literal.value", "uint64"));
				decoded = *unsigned_value;
			}
			else
			{
				auto string_value = as_string(**scalar, "literal.value");
				if (!string_value)
					return unexpected(std::move(string_value.error()));
				const auto byte_backed = *type == "bytes" || *type == "set" ||
					(type->starts_with("set<") && type->ends_with('>'));
				if (byte_backed)
				{
					auto bytes = hex_bytes(*string_value);
					if (!bytes)
						return unexpected(std::move(bytes.error()));
					decoded = std::move(*bytes);
				}
				else
					decoded = std::move(*string_value);
			}
			return ir_literal{std::move(*type), std::move(decoded)};
		}

		[[nodiscard]] result<decoded_predicate> predicate(const json_value& value)
		{
			auto object = as_object(value, "predicate");
			if (!object)
				return unexpected(std::move(object.error()));
			auto kind_value = required(**object, "kind");
			if (!kind_value)
				return unexpected(std::move(kind_value.error()));
			auto kind = as_string(**kind_value, "predicate.kind");
			if (!kind)
				return unexpected(std::move(kind.error()));
			decoded_predicate output;
			if (*kind == "equals_present")
			{
				if (!exact_keys(**object, {"column", "kind", "literal"}, "predicate"))
					return unexpected(decode_error("predicate", "equals-shape"));
				auto column_value = required(**object, "column");
				auto literal_value = required(**object, "literal");
				if (!column_value || !literal_value)
					return unexpected(decode_error("predicate", "equals-missing"));
				auto decoded_column = column(**column_value);
				auto decoded_literal = typed_literal(**literal_value);
				if (!decoded_column || !decoded_literal)
					return unexpected(decode_error("predicate", "equals-value"));
				output.kind = predicate_kind::equals_present;
				output.column = std::move(*decoded_column);
				output.literal_value = std::move(*decoded_literal);
				return output;
			}
			if (*kind == "column_equals_present")
			{
				if (!exact_keys(**object, {"kind", "left", "right"}, "predicate"))
					return unexpected(decode_error("predicate", "column-equals-shape"));
				auto left_value = required(**object, "left");
				auto right_value = required(**object, "right");
				if (!left_value || !right_value)
					return unexpected(decode_error("predicate", "column-equals-missing"));
				auto left = column(**left_value);
				auto right = column(**right_value);
				if (!left || !right)
					return unexpected(decode_error("predicate", "column-equals-value"));
				output.kind = predicate_kind::column_equals_present;
				output.left = std::move(*left);
				output.right = std::move(*right);
				return output;
			}
			if (*kind == "and" || *kind == "or")
			{
				if (!exact_keys(**object, {"kind", "operands"}, "predicate"))
					return unexpected(decode_error("predicate", "boolean-shape"));
				auto operands_value = required(**object, "operands");
				if (!operands_value)
					return unexpected(std::move(operands_value.error()));
				auto operands = as_array(**operands_value, "predicate.operands");
				if (!operands || (*operands)->size() < 2U)
					return unexpected(decode_error("predicate.operands", "minimum-two"));
				output.kind = *kind == "and" ? predicate_kind::all : predicate_kind::any;
				for (const auto& operand : **operands)
				{
					auto child = predicate(operand);
					if (!child)
						return unexpected(std::move(child.error()));
					output.operands.push_back(std::move(*child));
				}
				return output;
			}
			if (!exact_keys(**object, {"column", "kind"}, "predicate"))
				return unexpected(decode_error("predicate", "state-shape"));
			auto column_value = required(**object, "column");
			if (!column_value)
				return unexpected(std::move(column_value.error()));
			auto decoded_column = column(**column_value);
			if (!decoded_column)
				return unexpected(std::move(decoded_column.error()));
			if (*kind == "is_present")
				output.kind = predicate_kind::is_present;
			else if (*kind == "is_absent")
				output.kind = predicate_kind::is_absent;
			else if (*kind == "is_unknown")
				output.kind = predicate_kind::is_unknown;
			else
				return unexpected(decode_error("predicate.kind", *kind));
			output.column = std::move(*decoded_column);
			return output;
		}

		[[nodiscard]] result<json_value::object_type> parse_object(const ir_node& node)
		{
			constexpr std::size_t maximum_arguments_bytes = std::size_t{4U} * 1024U * 1024U;
			const cxxlens::sdk::detail::json_limits limits{
				.max_input_bytes = maximum_arguments_bytes,
				.max_depth = 64U,
				.max_array_elements = maximum_arguments_bytes,
				.max_object_members = maximum_arguments_bytes,
				.max_string_bytes = maximum_arguments_bytes,
				.max_total_string_bytes = maximum_arguments_bytes,
				.max_total_values = maximum_arguments_bytes,
			};
			const cxxlens::sdk::detail::json_parse_contract contract{
				.error_code = "sdk.query-argument-invalid",
				.error_field = "arguments",
				.include_byte_offset = false,
				.require_top_level_object = false,
				.reject_utf8_bom = true,
				.numbers = cxxlens::sdk::detail::json_number_syntax::integer_tokens,
				.depth = cxxlens::sdk::detail::json_depth_semantics::all_values,
			};
			auto parsed = cxxlens::sdk::detail::parse_json_value(node.arguments, limits, contract);
			if (!parsed)
				return unexpected(std::move(parsed.error()));
			auto object = as_object(*parsed, "arguments");
			if (!object)
				return unexpected(std::move(object.error()));
			return **object;
		}
	} // namespace

	result<operator_arguments> decode_arguments(const ir_node& node)
	{
		auto parsed = parse_object(node);
		if (!parsed)
			return unexpected(std::move(parsed.error()));
		const auto& object = *parsed;
		if (node.operator_id == "query.scan.v1")
		{
			if (!exact_keys(object, {"alias", "descriptor_id"}, "scan"))
				return unexpected(decode_error("scan", "shape"));
			auto descriptor_value = required(object, "descriptor_id");
			auto alias_value = required(object, "alias");
			if (!descriptor_value || !alias_value)
				return unexpected(decode_error("scan", "missing"));
			auto descriptor = as_string(**descriptor_value, "descriptor_id");
			auto alias = as_string(**alias_value, "alias");
			if (!descriptor || descriptor->empty() || !alias || alias->empty())
				return unexpected(decode_error("scan", "value"));
			return operator_arguments{scan_arguments{std::move(*descriptor), std::move(*alias)}};
		}
		if (node.operator_id == "query.filter.v1" || node.operator_id == "query.inner_join.v1" ||
			node.operator_id == "query.semi_join.v1" || node.operator_id == "query.anti_join.v1")
		{
			if (!exact_keys(object, {"predicate"}, "predicate-arguments"))
				return unexpected(decode_error("predicate", "shape"));
			auto value = required(object, "predicate");
			if (!value)
				return unexpected(std::move(value.error()));
			auto decoded = predicate(**value);
			if (!decoded)
				return unexpected(std::move(decoded.error()));
			return operator_arguments{predicate_arguments{std::move(*decoded)}};
		}
		if (node.operator_id == "query.project.v1")
		{
			if (!exact_keys(object, {"columns"}, "project"))
				return unexpected(decode_error("project", "shape"));
			auto value = required(object, "columns");
			if (!value)
				return unexpected(std::move(value.error()));
			auto columns = as_array(**value, "project.columns");
			if (!columns || (*columns)->empty())
				return unexpected(decode_error("project.columns", "empty"));
			project_arguments output;
			std::set<std::string, std::less<>> outputs;
			for (const auto& item_value : **columns)
			{
				auto item = as_object(item_value, "projection");
				if (!item || !exact_keys(**item, {"column", "output"}, "projection"))
					return unexpected(decode_error("projection", "shape"));
				auto column_value = required(**item, "column");
				auto output_value = required(**item, "output");
				if (!column_value || !output_value)
					return unexpected(decode_error("projection", "missing"));
				auto decoded_column = column(**column_value);
				auto output_name = as_string(**output_value, "projection.output");
				if (!decoded_column || !output_name || output_name->empty() ||
					!outputs.insert(*output_name).second)
					return unexpected(decode_error("projection", "value"));
				output.columns.push_back({std::move(*decoded_column), std::move(*output_name)});
			}
			return operator_arguments{std::move(output)};
		}
		if (node.operator_id == "query.union.v1" || node.operator_id == "query.distinct.v1")
		{
			if (!object.empty())
				return unexpected(decode_error("arguments", "empty-required"));
			return operator_arguments{empty_arguments{}};
		}
		if (node.operator_id == "query.order_by.v1")
		{
			if (!exact_keys(object, {"keys"}, "order"))
				return unexpected(decode_error("order", "shape"));
			auto value = required(object, "keys");
			if (!value)
				return unexpected(std::move(value.error()));
			auto keys = as_array(**value, "order.keys");
			if (!keys || (*keys)->empty())
				return unexpected(decode_error("order.keys", "empty"));
			order_arguments output;
			for (const auto& key_value : **keys)
			{
				auto key = as_object(key_value, "order.key");
				if (!key ||
					!exact_keys(**key, {"cell_state_order", "column", "direction"}, "order.key"))
					return unexpected(decode_error("order.key", "shape"));
				auto column_value = required(**key, "column");
				auto direction_value = required(**key, "direction");
				auto states_value = required(**key, "cell_state_order");
				if (!column_value || !direction_value || !states_value)
					return unexpected(decode_error("order.key", "missing"));
				auto decoded_column = column(**column_value);
				auto direction = as_string(**direction_value, "order.direction");
				auto states = as_array(**states_value, "order.cell_state_order");
				if (!decoded_column || !direction ||
					(*direction != "ascending" && *direction != "descending") || !states ||
					(*states)->size() != 3U)
					return unexpected(decode_error("order.key", "value"));
				std::vector<cell_state> decoded_states;
				for (const auto& state_value : **states)
				{
					auto state = as_string(state_value, "order.cell-state");
					if (!state)
						return unexpected(std::move(state.error()));
					if (*state == "present")
						decoded_states.push_back(cell_state::present);
					else if (*state == "absent")
						decoded_states.push_back(cell_state::absent);
					else if (*state == "unknown")
						decoded_states.push_back(cell_state::unknown);
					else
						return unexpected(decode_error("order.cell-state", *state));
				}
				auto sorted = decoded_states;
				std::ranges::sort(sorted);
				if (std::ranges::adjacent_find(sorted) != sorted.end())
					return unexpected(decode_error("order.cell-state", "duplicate"));
				output.keys.push_back({std::move(*decoded_column),
									   *direction == "ascending",
									   std::move(decoded_states)});
			}
			return operator_arguments{std::move(output)};
		}
		if (node.operator_id == "query.limit.v1")
		{
			if (!exact_keys(object, {"count"}, "limit"))
				return unexpected(decode_error("limit", "shape"));
			auto value = required(object, "count");
			if (!value)
				return unexpected(std::move(value.error()));
			auto count = as_unsigned(**value, "limit.count");
			if (!count)
				return unexpected(std::move(count.error()));
			return operator_arguments{limit_arguments{*count}};
		}
		if (node.operator_id == "query.condition_restrict.v1")
		{
			if (!exact_keys(object, {"alternatives", "universe"}, "condition"))
				return unexpected(decode_error("condition", "shape"));
			auto universe_value = required(object, "universe");
			auto alternatives_value = required(object, "alternatives");
			if (!universe_value || !alternatives_value)
				return unexpected(decode_error("condition", "missing"));
			auto universe = as_string(**universe_value, "condition.universe");
			auto alternatives = as_array(**alternatives_value, "condition.alternatives");
			if (!universe || universe->empty() || !alternatives || (*alternatives)->empty())
				return unexpected(decode_error("condition", "value"));
			condition_arguments output;
			output.universe = std::move(*universe);
			for (const auto& alternative_value : **alternatives)
			{
				auto alternative = as_string(alternative_value, "condition.alternative");
				if (!alternative || alternative->empty())
					return unexpected(decode_error("condition.alternative", "value"));
				output.alternatives.push_back(std::move(*alternative));
			}
			std::ranges::sort(output.alternatives);
			output.alternatives.erase(std::ranges::unique(output.alternatives).begin(),
									  output.alternatives.end());
			return operator_arguments{std::move(output)};
		}
		if (node.operator_id == "query.interpretation_restrict.v1")
		{
			if (!exact_keys(object, {"interpretation"}, "interpretation"))
				return unexpected(decode_error("interpretation", "shape"));
			auto value = required(object, "interpretation");
			if (!value)
				return unexpected(std::move(value.error()));
			auto interpretation = as_string(**value, "interpretation");
			if (!interpretation || interpretation->empty())
				return unexpected(decode_error("interpretation", "value"));
			return operator_arguments{interpretation_arguments{std::move(*interpretation)}};
		}
		return unexpected(decode_error("operator", node.operator_id));
	}
} // namespace cxxlens::sdk::query
