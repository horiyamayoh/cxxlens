#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "selector_ast.hpp"

namespace cxxlens::select::detail
{
	namespace
	{
		using cxxlens::detail::json::json_value;
		constexpr std::size_t maximum_json_bytes = std::size_t{4U} * 1024U * 1024U;
		constexpr std::size_t maximum_json_depth = 64U;

		[[nodiscard]] error invalid(std::string field, std::string reason)
		{
			error failure;
			failure.code.value = "select.invalid-expression";
			failure.message = "selector JSON is invalid";
			failure.attributes.emplace("field", std::move(field));
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		[[nodiscard]] error mismatch(std::string field, std::string expected, std::string actual)
		{
			error failure;
			failure.code.value = "select.type-mismatch";
			failure.message = "selector expression domain does not match its position";
			failure.attributes.emplace("actual", std::move(actual));
			failure.attributes.emplace("expected", std::move(expected));
			failure.attributes.emplace("field", std::move(field));
			return failure;
		}

		void append_utf8(std::string& output, const std::uint32_t code_point)
		{
			if (code_point <= 0x7FU)
				output.push_back(static_cast<char>(code_point));
			else if (code_point <= 0x7FFU)
			{
				output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
				output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
			}
			else
			{
				output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
				output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
				output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
			}
		}

		class json_parser
		{
		  public:
			explicit json_parser(const std::string_view input) : input_{input} {}

			[[nodiscard]] result<json_value> parse()
			{
				if (input_.size() > maximum_json_bytes)
					return invalid("$", "size-limit");
				auto value = parse_value(0U);
				if (!value)
					return std::move(value.error());
				skip_space();
				if (position_ != input_.size())
					return invalid("$", "trailing-data");
				return std::move(value.value());
			}

		  private:
			[[nodiscard]] result<json_value> parse_value(const std::size_t depth)
			{
				if (depth > maximum_json_depth)
					return invalid("$", "depth-limit");
				skip_space();
				if (position_ >= input_.size())
					return invalid("$", "value-missing");
				switch (input_.at(position_))
				{
					case '{':
						return parse_object(depth);
					case '[':
						return parse_array(depth);
					case '"':
					{
						auto value = parse_string();
						if (!value)
							return std::move(value.error());
						return json_value{std::move(value.value())};
					}
					case 't':
						return literal("true", json_value{true});
					case 'f':
						return literal("false", json_value{false});
					case 'n':
						return literal("null", json_value{});
					default:
						return parse_number();
				}
			}

			[[nodiscard]] result<json_value> parse_object(const std::size_t depth)
			{
				++position_;
				json_value::object object;
				skip_space();
				if (consume('}'))
					return json_value{std::move(object)};
				while (true)
				{
					auto key = parse_string();
					if (!key)
						return std::move(key.error());
					skip_space();
					if (!consume(':'))
						return invalid("$", "colon-missing");
					auto value = parse_value(depth + 1U);
					if (!value)
						return std::move(value.error());
					if (std::ranges::any_of(object,
											[&](const auto& field)
											{
												return field.first == key.value();
											}))
						return invalid("$", "duplicate-key");
					object.emplace_back(std::move(key.value()), std::move(value.value()));
					skip_space();
					if (consume('}'))
						break;
					if (!consume(','))
						return invalid("$", "comma-missing");
					skip_space();
				}
				return json_value{std::move(object)};
			}

			[[nodiscard]] result<json_value> parse_array(const std::size_t depth)
			{
				++position_;
				json_value::array array;
				skip_space();
				if (consume(']'))
					return json_value{std::move(array)};
				while (true)
				{
					auto value = parse_value(depth + 1U);
					if (!value)
						return std::move(value.error());
					array.push_back(std::move(value.value()));
					skip_space();
					if (consume(']'))
						break;
					if (!consume(','))
						return invalid("$", "comma-missing");
				}
				return json_value{std::move(array)};
			}

			[[nodiscard]] result<std::string> parse_string()
			{
				if (!consume('"'))
					return invalid("$", "string-expected");
				std::string output;
				while (position_ < input_.size())
				{
					const auto character = static_cast<unsigned char>(input_.at(position_++));
					if (character == '"')
					{
						if (!cxxlens::detail::json::valid_utf8(output))
							return invalid("$", "invalid-utf8");
						return output;
					}
					if (character < 0x20U)
						return invalid("$", "control-character");
					if (character != '\\')
					{
						output.push_back(static_cast<char>(character));
						continue;
					}
					if (position_ >= input_.size())
						return invalid("$", "escape-missing");
					const char escaped = input_.at(position_++);
					switch (escaped)
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
							if (!code_point ||
								(code_point.value() >= 0xD800U && code_point.value() <= 0xDFFFU))
								return invalid("$", "invalid-unicode-escape");
							append_utf8(output, code_point.value());
							break;
						}
						default:
							return invalid("$", "unsupported-escape");
					}
				}
				return invalid("$", "unterminated-string");
			}

			[[nodiscard]] result<std::uint32_t> hexadecimal_quad()
			{
				if (position_ + 4U > input_.size())
					return invalid("$", "short-unicode-escape");
				std::uint32_t value{};
				for (std::size_t index = 0U; index < 4U; ++index)
				{
					const char character = input_.at(position_++);
					value <<= 4U;
					if (character >= '0' && character <= '9')
						value |= static_cast<std::uint32_t>(character - '0');
					else if (character >= 'a' && character <= 'f')
						value |= static_cast<std::uint32_t>(character - 'a' + 10);
					else if (character >= 'A' && character <= 'F')
						value |= static_cast<std::uint32_t>(character - 'A' + 10);
					else
						return invalid("$", "invalid-unicode-hex");
				}
				return value;
			}

			[[nodiscard]] result<json_value> parse_number()
			{
				const auto begin = position_;
				while (position_ < input_.size() &&
					   std::string_view{"-0123456789"}.contains(input_.at(position_)))
					++position_;
				if (begin == position_)
					return invalid("$", "number-expected");
				const auto token = input_.substr(begin, position_ - begin);
				if (token.starts_with('-'))
				{
					std::int64_t value{};
					const auto converted =
						std::from_chars(token.data(), token.data() + token.size(), value);
					if (converted.ec != std::errc{} || converted.ptr != token.data() + token.size())
						return invalid("$", "invalid-integer");
					return json_value{value};
				}
				std::uint64_t value{};
				const auto converted =
					std::from_chars(token.data(), token.data() + token.size(), value);
				if (converted.ec != std::errc{} || converted.ptr != token.data() + token.size())
					return invalid("$", "invalid-integer");
				return json_value{value};
			}

			[[nodiscard]] result<json_value> literal(const std::string_view expected,
													 json_value value)
			{
				if (input_.substr(position_, expected.size()) != expected)
					return invalid("$", "invalid-literal");
				position_ += expected.size();
				return value;
			}
			void skip_space()
			{
				while (position_ < input_.size() &&
					   std::isspace(static_cast<unsigned char>(input_.at(position_))) != 0)
					++position_;
			}
			[[nodiscard]] bool consume(const char expected)
			{
				if (position_ < input_.size() && input_.at(position_) == expected)
				{
					++position_;
					return true;
				}
				return false;
			}

			std::string_view input_;
			std::size_t position_{};
		};

		[[nodiscard]] const json_value* field(const json_value::object& object,
											  const std::string_view key)
		{
			const auto position =
				std::ranges::find(object, key, &std::pair<std::string, json_value>::first);
			return position == object.end() ? nullptr : &position->second;
		}

		[[nodiscard]] bool exact_fields(const json_value::object& object,
										std::initializer_list<std::string_view> expected)
		{
			if (object.size() != expected.size())
				return false;
			return std::ranges::all_of(object,
									   [&](const auto& item)
									   {
										   return std::ranges::find(expected, item.first) !=
											   expected.end();
									   });
		}

		[[nodiscard]] result<const json_value::object*> object_at(const json_value& value,
																  std::string path)
		{
			if (const auto* object = std::get_if<json_value::object>(&value.value))
				return object;
			return invalid(std::move(path), "object-required");
		}

		[[nodiscard]] result<std::string>
		string_at(const json_value::object& object, const std::string_view key, std::string path)
		{
			const auto* value = field(object, key);
			if (value == nullptr)
				return invalid(std::move(path), "required-field-missing");
			if (const auto* string = std::get_if<std::string>(&value->value))
				return *string;
			return invalid(std::move(path), "string-required");
		}

		[[nodiscard]] result<selector_domain> parse_domain(const std::string_view value,
														   std::string path)
		{
			if (value == "file")
				return selector_domain::file;
			if (value == "symbol")
				return selector_domain::symbol;
			if (value == "type")
				return selector_domain::type;
			if (value == "call")
				return selector_domain::call;
			return invalid(std::move(path), "unknown-domain");
		}

		[[nodiscard]] std::string_view domain_name(const selector_domain value)
		{
			constexpr std::array names{"file", "symbol", "type", "call"};
			return names.at(static_cast<std::size_t>(value));
		}

		struct predicate_spec
		{
			selector_domain domain;
			std::vector<std::string_view> arguments;
			std::vector<selector_domain> operands;
			std::optional<std::set<std::string_view>> values;
		};

		[[nodiscard]] std::optional<predicate_spec> spec_for(const std::string_view name)
		{
			using enum_set = std::set<std::string_view>;
			const auto plain =
				[&](const selector_domain domain, std::vector<std::string_view> arguments = {})
			{
				return predicate_spec{domain, std::move(arguments), {}, std::nullopt};
			};
			if (name == "file.any")
				return plain(selector_domain::file);
			if (name == "file.path_exact" || name == "file.path_glob" || name == "file.generated" ||
				name == "file.system")
				return plain(selector_domain::file, {"value"});
			if (name == "symbol.any")
				return plain(selector_domain::symbol);
			if (name == "symbol.kinds")
				return plain(selector_domain::symbol, {"values"});
			if (name == "symbol.name")
				return plain(selector_domain::symbol, {"policy", "value"});
			if (name == "symbol.declared_in")
				return predicate_spec{
					selector_domain::symbol, {}, {selector_domain::file}, std::nullopt};
			if (name == "symbol.member_of" || name == "symbol.derived_from" ||
				name == "symbol.overrides")
				return predicate_spec{
					selector_domain::symbol, {}, {selector_domain::symbol}, std::nullopt};
			if (name == "symbol.defined" || name == "symbol.public_surface")
				return plain(selector_domain::symbol, {"value"});
			if (name == "symbol.macro_policy")
				return predicate_spec{selector_domain::symbol,
									  {"value"},
									  {},
									  enum_set{"exclude",
											   "include_with_origin",
											   "only_macro_arguments",
											   "only_macro_bodies",
											   "only_expansions"}};
			if (name == "symbol.variant_policy")
				return predicate_spec{selector_domain::symbol,
									  {"value"},
									  {},
									  enum_set{"any_variant",
											   "all_variants",
											   "report_per_variant",
											   "reject_disagreement"}};
			if (name == "type.any")
				return plain(selector_domain::type);
			if (name == "type.canonical" || name == "type.spelling" ||
				name == "type.specialization_of" || name == "type.const_qualified" ||
				name == "type.including_derived" || name == "type.any_cvref")
				return plain(selector_domain::type, {"value"});
			if (name == "type.declared_as" || name == "type.derived_from")
				return predicate_spec{
					selector_domain::type, {}, {selector_domain::symbol}, std::nullopt};
			if (name == "type.pointer_to" || name == "type.reference_to" ||
				name == "type.convertible_to")
				return predicate_spec{
					selector_domain::type, {}, {selector_domain::type}, std::nullopt};
			if (name == "call.any")
				return plain(selector_domain::call);
			if (name == "call.kinds")
				return plain(selector_domain::call, {"values"});
			if (name == "call.callee")
				return predicate_spec{
					selector_domain::call, {}, {selector_domain::symbol}, std::nullopt};
			if (name == "call.callee_name" || name == "call.function_name" ||
				name == "call.method_name" || name == "call.include_derived_types" ||
				name == "call.include_virtual_overrides" || name == "call.precision")
				return plain(selector_domain::call, {"value"});
			if (name == "call.receiver_type")
				return predicate_spec{
					selector_domain::call, {}, {selector_domain::type}, std::nullopt};
			if (name == "call.argument_type")
				return predicate_spec{
					selector_domain::call, {"index"}, {selector_domain::type}, std::nullopt};
			if (name == "call.inside")
				return predicate_spec{
					selector_domain::call, {}, {selector_domain::symbol}, std::nullopt};
			if (name == "call.in_file")
				return predicate_spec{
					selector_domain::call, {}, {selector_domain::file}, std::nullopt};
			if (name == "call.dispatch")
				return predicate_spec{selector_domain::call,
									  {"value"},
									  {},
									  enum_set{"direct_only",
											   "static_target",
											   "static_and_virtual_candidates",
											   "include_indirect_candidates"}};
			if (name == "call.implicit_policy")
				return predicate_spec{
					selector_domain::call,
					{"value"},
					{},
					enum_set{"spelled_only", "include_language_implicit", "implicit_only"}};
			if (name == "call.macro_policy")
				return predicate_spec{selector_domain::call,
									  {"value"},
									  {},
									  enum_set{"exclude",
											   "include_with_origin",
											   "only_macro_arguments",
											   "only_macro_bodies",
											   "only_expansions"}};
			if (name == "call.template_policy")
				return predicate_spec{selector_domain::call,
									  {"value"},
									  {},
									  enum_set{"patterns",
											   "observed_instantiations",
											   "patterns_and_observed_instantiations"}};
			if (name == "call.variant_policy")
				return predicate_spec{selector_domain::call,
									  {"value"},
									  {},
									  enum_set{"any_variant",
											   "all_variants",
											   "report_per_variant",
											   "reject_disagreement"}};
			return std::nullopt;
		}

		[[nodiscard]] bool comma_values_are(const std::string_view input,
											const std::set<std::string_view>& allowed)
		{
			if (input.empty())
				return false;
			std::size_t begin = 0U;
			while (begin < input.size())
			{
				const auto end = input.find(',', begin);
				if (!allowed.contains(input.substr(begin, end - begin)))
					return false;
				if (end == std::string_view::npos)
					return true;
				begin = end + 1U;
			}
			return false;
		}

		[[nodiscard]] bool
		valid_arguments(const std::string_view name,
						const std::vector<std::pair<std::string, std::string>>& arguments)
		{
			const auto value = [&](const std::string_view key) -> std::string_view
			{
				const auto position =
					std::ranges::find(arguments, key, &std::pair<std::string, std::string>::first);
				return position == arguments.end() ? std::string_view{}
												   : std::string_view{position->second};
			};
			if (name.ends_with(".generated") || name.ends_with(".system") ||
				name.ends_with(".defined") || name.ends_with(".public_surface") ||
				name.ends_with(".const_qualified") || name.ends_with(".including_derived") ||
				name.ends_with(".any_cvref") || name.ends_with(".include_derived_types") ||
				name.ends_with(".include_virtual_overrides"))
				return value("value") == "true" || value("value") == "false";
			if (name == "symbol.name")
				return !value("value").empty() &&
					std::set<std::string_view>{
						"exact", "qualified_exact", "unqualified_exact", "glob"}
						.contains(value("policy"));
			if (name == "symbol.kinds")
				return comma_values_are(value("values"),
										{"namespace",	  "record",	  "class",		"struct",
										 "union",		  "function", "method",		"constructor",
										 "destructor",	  "variable", "field",		"enum_type",
										 "enum_constant", "typedef",  "type_alias", "template",
										 "concept",		  "macro",	  "module",		"parameter",
										 "unknown"});
			if (name == "call.kinds")
				return comma_values_are(value("values"),
										{"direct_function",
										 "member",
										 "virtual_member",
										 "constructor",
										 "destructor",
										 "overloaded_operator",
										 "builtin_operator",
										 "function_pointer",
										 "callback",
										 "modeled",
										 "unknown"});
			if (name == "call.precision")
				return value("value").size() == 1U && value("value").front() >= '0' &&
					value("value").front() <= '6';
			if (name == "call.argument_type")
				return !value("index").empty() &&
					std::ranges::all_of(value("index"),
										[](const char character)
										{
											return character >= '0' && character <= '9';
										});
			if (name.ends_with(".any") || name.ends_with("_policy") || name == "call.dispatch")
				return true;
			return arguments.empty() || !value("value").empty();
		}

		[[nodiscard]] std::string expected_reason(const std::string_view predicate)
		{
			std::string result{"select."};
			for (const char value : predicate)
				result.push_back(value == '_' ? '-' : value);
			return result;
		}

		[[nodiscard]] result<node_ptr> parse_expression(const json_value& input,
														const selector_domain domain,
														std::string path,
														const std::size_t depth)
		{
			if (depth > maximum_json_depth)
				return invalid(std::move(path), "depth-limit");
			auto object_result = object_at(input, path);
			if (!object_result)
				return std::move(object_result.error());
			const auto& object = *object_result.value();
			auto operation_result = string_at(object, "op", path + ".op");
			if (!operation_result)
				return std::move(operation_result.error());
			const auto& operation = operation_result.value();
			if (operation == "constant")
			{
				if (!exact_fields(object, {"op", "value"}))
					return invalid(path, "unknown-field");
				const auto* value = field(object, "value");
				if (value == nullptr || !std::holds_alternative<bool>(value->value))
					return invalid(path + ".value", "boolean-required");
				return std::get<bool>(value->value) ? all_node(domain, {}) : any_node(domain, {});
			}
			if (operation == "all" || operation == "any")
			{
				if (!exact_fields(object, {"op", "operands"}))
					return invalid(path, "unknown-field");
				const auto* value = field(object, "operands");
				if (value == nullptr || !std::holds_alternative<json_value::array>(value->value))
					return invalid(path + ".operands", "array-required");
				std::vector<node_ptr> operands;
				const auto& array = std::get<json_value::array>(value->value);
				for (std::size_t index = 0U; index < array.size(); ++index)
				{
					auto operand =
						parse_expression(array.at(index),
										 domain,
										 path + ".operands[" + std::to_string(index) + "]",
										 depth + 1U);
					if (!operand)
						return std::move(operand.error());
					operands.push_back(std::move(operand.value()));
				}
				return operation == "all" ? all_node(domain, std::move(operands))
										  : any_node(domain, std::move(operands));
			}
			if (operation == "negate")
			{
				if (!exact_fields(object, {"op", "operand"}))
					return invalid(path, "unknown-field");
				const auto* operand_value = field(object, "operand");
				if (operand_value == nullptr)
					return invalid(path + ".operand", "required-field-missing");
				auto operand =
					parse_expression(*operand_value, domain, path + ".operand", depth + 1U);
				if (!operand)
					return std::move(operand.error());
				return negate_node(domain, std::move(operand.value()));
			}
			if (operation != "predicate")
				return invalid(path + ".op", "unknown-operation");
			if (!exact_fields(object, {"op", "name", "reason_code", "arguments", "operands"}))
				return invalid(path, "unknown-field");

			auto name_result = string_at(object, "name", path + ".name");
			if (!name_result)
				return std::move(name_result.error());
			const auto spec = spec_for(name_result.value());
			if (!spec)
				return invalid(path + ".name", "unknown-predicate");
			if (spec->domain != domain)
				return mismatch(path + ".name",
								std::string{domain_name(domain)},
								std::string{domain_name(spec->domain)});
			auto reason_result = string_at(object, "reason_code", path + ".reason_code");
			if (!reason_result)
				return std::move(reason_result.error());
			if (reason_result.value() != expected_reason(name_result.value()))
				return invalid(path + ".reason_code", "reason-code-mismatch");

			const auto* arguments_value = field(object, "arguments");
			if (arguments_value == nullptr ||
				!std::holds_alternative<json_value::object>(arguments_value->value))
				return invalid(path + ".arguments", "object-required");
			const auto& arguments_object = std::get<json_value::object>(arguments_value->value);
			std::vector<std::pair<std::string, std::string>> arguments;
			for (const auto expected : spec->arguments)
			{
				auto value_result = string_at(
					arguments_object, expected, path + ".arguments." + std::string{expected});
				if (!value_result)
					return std::move(value_result.error());
				arguments.emplace_back(expected, std::move(value_result.value()));
			}
			if (arguments_object.size() != spec->arguments.size())
				return invalid(path + ".arguments", "unknown-argument");
			if (spec->values && !arguments.empty() &&
				!spec->values->contains(arguments.front().second))
				return invalid(path + ".arguments.value", "unknown-policy");
			if (!valid_arguments(name_result.value(), arguments))
				return invalid(path + ".arguments", "invalid-argument");

			const auto* operands_value = field(object, "operands");
			if (operands_value == nullptr ||
				!std::holds_alternative<json_value::array>(operands_value->value))
				return invalid(path + ".operands", "array-required");
			const auto& operand_array = std::get<json_value::array>(operands_value->value);
			if (operand_array.size() != spec->operands.size())
				return invalid(path + ".operands", "operand-count-mismatch");
			std::vector<node_ptr> operands;
			for (std::size_t index = 0U; index < operand_array.size(); ++index)
			{
				auto operand = parse_expression(operand_array.at(index),
												spec->operands.at(index),
												path + ".operands[" + std::to_string(index) + "]",
												depth + 1U);
				if (!operand)
					return std::move(operand.error());
				operands.push_back(std::move(operand.value()));
			}
			return predicate_node(
				domain, std::move(name_result.value()), std::move(arguments), std::move(operands));
		}
	} // namespace

	result<node_ptr> parse_selector_json(const std::string_view input)
	{
		auto parsed = json_parser{input}.parse();
		if (!parsed)
			return std::move(parsed.error());
		auto object_result = object_at(parsed.value(), "$");
		if (!object_result)
			return std::move(object_result.error());
		const auto& object = *object_result.value();
		auto schema_result = string_at(object, "schema", "$.schema");
		if (!schema_result)
			return std::move(schema_result.error());
		const bool legacy = schema_result.value() == "cxxlens.selector.v0";
		if (!legacy && schema_result.value() != "cxxlens.selector.v1")
			return invalid("$.schema", "unknown-schema");
		if (!legacy &&
			!exact_fields(object,
						  {"schema",
						   "semantics_version",
						   "library_version",
						   "normalization_version",
						   "domain",
						   "expression"}))
			return invalid("$", "unknown-field");
		if (!legacy)
		{
			auto semantics = string_at(object, "semantics_version", "$.semantics_version");
			if (!semantics || semantics.value() != "1.0.0")
				return invalid("$.semantics_version", "unsupported-semantics-version");
			auto library = string_at(object, "library_version", "$.library_version");
			if (!library)
				return std::move(library.error());
		}
		auto domain_result = string_at(object, "domain", "$.domain");
		if (!domain_result)
			return std::move(domain_result.error());
		auto domain = parse_domain(domain_result.value(), "$.domain");
		if (!domain)
			return std::move(domain.error());
		const auto* expression = field(object, "expression");
		if (expression == nullptr)
			return invalid("$.expression", "required-field-missing");
		if (!legacy)
		{
			const auto* version = field(object, "normalization_version");
			if (version == nullptr || !std::holds_alternative<std::uint64_t>(version->value) ||
				std::get<std::uint64_t>(version->value) != 1U)
				return invalid("$.normalization_version", "unsupported-normalization-version");
		}
		return parse_expression(*expression, domain.value(), "$.expression", 0U);
	}
} // namespace cxxlens::select::detail
