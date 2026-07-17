#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <cxxlens/sdk/query.hpp>

#include "json_internal.hpp"

namespace cxxlens::sdk::query
{
	namespace
	{
		[[nodiscard]] error decode_error(std::string field, std::string detail)
		{
			return {"sdk.query-argument-invalid", std::move(field), std::move(detail)};
		}

		struct json_value
		{
			using array = std::vector<json_value>;
			using object = std::map<std::string, json_value, std::less<>>;
			std::variant<std::monostate,
						 bool,
						 std::int64_t,
						 std::uint64_t,
						 std::string,
						 array,
						 object>
				value;
		};

		void append_utf8(std::string& output, const std::uint32_t code_point)
		{
			if (code_point <= 0x7fU)
				output.push_back(static_cast<char>(code_point));
			else if (code_point <= 0x7ffU)
			{
				output.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
				output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
			}
			else if (code_point <= 0xffffU)
			{
				output.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
				output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
				output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
			}
			else
			{
				output.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
				output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
				output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
				output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
			}
		}

		class json_parser
		{
		  public:
			explicit json_parser(const std::string_view input) : input_{input} {}

			[[nodiscard]] result<json_value> parse()
			{
				if (input_.size() > std::size_t{4U} * 1024U * 1024U)
					return unexpected(decode_error("arguments", "size-limit"));
				auto output = value(0U);
				space();
				if (!output || position_ != input_.size())
					return unexpected(decode_error("arguments", "trailing-data"));
				return output;
			}

		  private:
			[[nodiscard]] result<json_value> value(const std::size_t depth)
			{
				if (depth > 64U)
					return unexpected(decode_error("arguments", "depth-limit"));
				space();
				if (position_ >= input_.size())
					return unexpected(decode_error("arguments", "value-missing"));
				switch (input_[position_])
				{
					case '{':
						return object(depth);
					case '[':
						return array(depth);
					case '"':
					{
						auto decoded = string();
						if (!decoded)
							return unexpected(std::move(decoded.error()));
						return json_value{std::move(*decoded)};
					}
					case 't':
						return literal("true", json_value{true});
					case 'f':
						return literal("false", json_value{false});
					case 'n':
						return literal("null", json_value{});
					default:
						return number();
				}
			}

			[[nodiscard]] result<json_value> object(const std::size_t depth)
			{
				++position_;
				json_value::object output;
				space();
				if (consume('}'))
					return json_value{std::move(output)};
				while (true)
				{
					auto key = string();
					space();
					if (!key || !consume(':'))
						return unexpected(decode_error("arguments", "object-key"));
					auto child = value(depth + 1U);
					if (!child)
						return unexpected(std::move(child.error()));
					if (!output.emplace(std::move(*key), std::move(*child)).second)
						return unexpected(decode_error("arguments", "duplicate-key"));
					space();
					if (consume('}'))
						break;
					if (!consume(','))
						return unexpected(decode_error("arguments", "object-comma"));
					space();
				}
				return json_value{std::move(output)};
			}

			[[nodiscard]] result<json_value> array(const std::size_t depth)
			{
				++position_;
				json_value::array output;
				space();
				if (consume(']'))
					return json_value{std::move(output)};
				while (true)
				{
					auto child = value(depth + 1U);
					if (!child)
						return unexpected(std::move(child.error()));
					output.push_back(std::move(*child));
					space();
					if (consume(']'))
						break;
					if (!consume(','))
						return unexpected(decode_error("arguments", "array-comma"));
				}
				return json_value{std::move(output)};
			}

			[[nodiscard]] result<std::string> string()
			{
				if (!consume('"'))
					return unexpected(decode_error("arguments", "string-expected"));
				std::string output;
				while (position_ < input_.size())
				{
					const auto byte = static_cast<unsigned char>(input_[position_++]);
					if (byte == '"')
					{
						if (!cxxlens::sdk::detail::valid_utf8(output))
							return unexpected(decode_error("arguments", "invalid-utf8"));
						return output;
					}
					if (byte < 0x20U)
						return unexpected(decode_error("arguments", "control-character"));
					if (byte != '\\')
					{
						output.push_back(static_cast<char>(byte));
						continue;
					}
					if (position_ >= input_.size())
						return unexpected(decode_error("arguments", "short-escape"));
					switch (const auto escaped = input_[position_++])
					{
						case '"':
						case '\\':
						case '/':
							output.push_back(escaped);
							break;
						case 'b':
							output.push_back('\b');
							break;
						case 'f':
							output.push_back('\f');
							break;
						case 'n':
							output.push_back('\n');
							break;
						case 'r':
							output.push_back('\r');
							break;
						case 't':
							output.push_back('\t');
							break;
						case 'u':
						{
							auto code_point = hexadecimal_quad();
							if (!code_point)
								return unexpected(decode_error("arguments", "unicode-escape"));
							if (*code_point >= 0xd800U && *code_point <= 0xdbffU)
							{
								if (position_ + 2U > input_.size() || input_[position_] != '\\' ||
									input_[position_ + 1U] != 'u')
									return unexpected(decode_error("arguments", "surrogate-pair"));
								position_ += 2U;
								auto low = hexadecimal_quad();
								if (!low || *low < 0xdc00U || *low > 0xdfffU)
									return unexpected(decode_error("arguments", "surrogate-pair"));
								*code_point =
									0x10000U + ((*code_point - 0xd800U) << 10U) + (*low - 0xdc00U);
							}
							else if (*code_point >= 0xdc00U && *code_point <= 0xdfffU)
								return unexpected(decode_error("arguments", "surrogate-pair"));
							append_utf8(output, *code_point);
							break;
						}
						default:
							return unexpected(decode_error("arguments", "unsupported-escape"));
					}
				}
				return unexpected(decode_error("arguments", "unterminated-string"));
			}

			[[nodiscard]] result<std::uint32_t> hexadecimal_quad()
			{
				if (position_ + 4U > input_.size())
					return unexpected(decode_error("arguments", "short-unicode"));
				std::uint32_t output{};
				for (std::size_t index = 0U; index < 4U; ++index)
				{
					const auto byte = input_[position_++];
					output <<= 4U;
					if (byte >= '0' && byte <= '9')
						output |= static_cast<std::uint32_t>(byte - '0');
					else if (byte >= 'a' && byte <= 'f')
						output |= static_cast<std::uint32_t>(byte - 'a' + 10);
					else if (byte >= 'A' && byte <= 'F')
						output |= static_cast<std::uint32_t>(byte - 'A' + 10);
					else
						return unexpected(decode_error("arguments", "unicode-hex"));
				}
				return output;
			}

			[[nodiscard]] result<json_value> number()
			{
				const auto begin = position_;
				if (position_ < input_.size() && input_[position_] == '-')
					++position_;
				const auto digits_begin = position_;
				while (position_ < input_.size() && input_[position_] >= '0' &&
					   input_[position_] <= '9')
					++position_;
				if (digits_begin == position_ ||
					(position_ - digits_begin > 1U && input_[digits_begin] == '0') ||
					(position_ < input_.size() &&
					 std::string_view{".eE+"}.contains(input_[position_])))
					return unexpected(decode_error("arguments", "integer-expected"));
				const auto token = input_.substr(begin, position_ - begin);
				if (token.starts_with('-'))
				{
					std::int64_t output{};
					const auto converted =
						std::from_chars(token.data(), token.data() + token.size(), output);
					if (converted.ec != std::errc{} || converted.ptr != token.data() + token.size())
						return unexpected(decode_error("arguments", "signed-integer"));
					return json_value{output};
				}
				std::uint64_t output{};
				const auto converted =
					std::from_chars(token.data(), token.data() + token.size(), output);
				if (converted.ec != std::errc{} || converted.ptr != token.data() + token.size())
					return unexpected(decode_error("arguments", "unsigned-integer"));
				return json_value{output};
			}

			[[nodiscard]] result<json_value> literal(const std::string_view expected,
													 json_value value)
			{
				if (input_.substr(position_, expected.size()) != expected)
					return unexpected(decode_error("arguments", "literal"));
				position_ += expected.size();
				return value;
			}

			void space()
			{
				while (position_ < input_.size() &&
					   (input_[position_] == ' ' || input_[position_] == '\t' ||
						input_[position_] == '\n' || input_[position_] == '\r'))
					++position_;
			}

			[[nodiscard]] bool consume(const char expected)
			{
				if (position_ < input_.size() && input_[position_] == expected)
				{
					++position_;
					return true;
				}
				return false;
			}

			std::string_view input_;
			std::size_t position_{};
		};

		[[nodiscard]] result<const json_value::object*> as_object(const json_value& value,
																  const std::string_view field)
		{
			if (const auto* output = std::get_if<json_value::object>(&value.value))
				return output;
			return unexpected(decode_error(std::string{field}, "object-required"));
		}

		[[nodiscard]] result<const json_value::array*> as_array(const json_value& value,
																const std::string_view field)
		{
			if (const auto* output = std::get_if<json_value::array>(&value.value))
				return output;
			return unexpected(decode_error(std::string{field}, "array-required"));
		}

		[[nodiscard]] result<std::string> as_string(const json_value& value,
													const std::string_view field)
		{
			if (const auto* output = std::get_if<std::string>(&value.value))
				return *output;
			return unexpected(decode_error(std::string{field}, "string-required"));
		}

		[[nodiscard]] result<std::uint64_t> as_unsigned(const json_value& value,
														const std::string_view field)
		{
			if (const auto* output = std::get_if<std::uint64_t>(&value.value))
				return *output;
			return unexpected(decode_error(std::string{field}, "unsigned-required"));
		}

		[[nodiscard]] result<const json_value*> required(const json_value::object& object,
														 const std::string_view name)
		{
			const auto found = object.find(name);
			if (found == object.end())
				return unexpected(decode_error(std::string{name}, "missing"));
			return &found->second;
		}

		[[nodiscard]] result<void> exact_keys(const json_value::object& object,
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
			if (!object || !exact_keys(**object, {"availability", "column_id"}, "column"))
				return unexpected(decode_error("column", "shape"));
			auto identifier_value = required(**object, "column_id");
			auto availability_value = required(**object, "availability");
			if (!identifier_value || !availability_value)
				return unexpected(decode_error("column", "missing"));
			auto identifier = as_string(**identifier_value, "column_id");
			auto availability = as_string(**availability_value, "availability");
			if (!identifier || identifier->empty() || !availability)
				return unexpected(decode_error("column", "value"));
			if (*availability == "require")
				return ir_column_ref{std::move(*identifier), column_availability::require};
			if (*availability == "absent_if_schema_missing")
				return ir_column_ref{std::move(*identifier),
									 column_availability::absent_if_schema_missing};
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
				const auto* boolean = std::get_if<bool>(&(*scalar)->value);
				if (boolean == nullptr)
					return unexpected(decode_error("literal.value", "bool"));
				decoded = *boolean;
			}
			else if (*type == "int64")
			{
				if (const auto* signed_value = std::get_if<std::int64_t>(&(*scalar)->value))
					decoded = *signed_value;
				else if (const auto* unsigned_value = std::get_if<std::uint64_t>(&(*scalar)->value);
						 unsigned_value != nullptr &&
						 *unsigned_value <=
							 static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
					decoded = static_cast<std::int64_t>(*unsigned_value);
				else
					return unexpected(decode_error("literal.value", "int64"));
			}
			else if (*type == "uint64")
			{
				const auto* unsigned_value = std::get_if<std::uint64_t>(&(*scalar)->value);
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

		[[nodiscard]] result<json_value::object> parse_object(const ir_node& node)
		{
			auto parsed = json_parser{node.arguments}.parse();
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
			node.operator_id == "query.semi_join.v1")
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
