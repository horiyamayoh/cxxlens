#pragma once

// Product-only capability diagnosis used by cxxlens-sdk-doctor.
//
// It consumes product values supplied by a project document and descriptors compiled into the
// SDK. The implementation is header-only so the installed tool can use the existing target.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_declaration.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/cc_type.hpp>
#include <cxxlens/relations/cc_type_component.hpp>
#include <cxxlens/relations/company_lock_acquire.hpp>
#include <cxxlens/relations/core_claim_conflict.hpp>
#include <cxxlens/relations/core_differential_disagreement.hpp>
#include <cxxlens/relations/core_provider_execution.hpp>
#include <cxxlens/relations/core_unresolved.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_origin.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <cxxlens/sdk.hpp>

namespace cxxlens::sdk::doctor
{
	inline constexpr std::size_t maximum_project_bytes = 1U * 1024U * 1024U;
	inline constexpr std::size_t maximum_json_depth = 64U;
	inline constexpr std::size_t maximum_capability_count = 128U;

	[[nodiscard]] inline bool valid_utf8(const std::string_view value) noexcept
	{
		for (std::size_t index = 0U; index < value.size(); ++index)
		{
			const auto first = static_cast<unsigned char>(value[index]);
			if (first <= 0x7fU)
				continue;
			if (first >= 0xc2U && first <= 0xdfU)
			{
				if (index + 1U >= value.size())
					return false;
				const auto second = static_cast<unsigned char>(value[++index]);
				if (second < 0x80U || second > 0xbfU)
					return false;
				continue;
			}
			if (first == 0xe0U)
			{
				if (index + 2U >= value.size())
					return false;
				const auto second = static_cast<unsigned char>(value[++index]);
				const auto third = static_cast<unsigned char>(value[++index]);
				if (second < 0xa0U || second > 0xbfU || third < 0x80U || third > 0xbfU)
					return false;
				continue;
			}
			if (first >= 0xe1U && first <= 0xecU)
			{
				if (index + 2U >= value.size())
					return false;
				const auto second = static_cast<unsigned char>(value[++index]);
				const auto third = static_cast<unsigned char>(value[++index]);
				if (second < 0x80U || second > 0xbfU || third < 0x80U || third > 0xbfU)
					return false;
				continue;
			}
			if (first == 0xedU)
			{
				if (index + 2U >= value.size())
					return false;
				const auto second = static_cast<unsigned char>(value[++index]);
				const auto third = static_cast<unsigned char>(value[++index]);
				if (second < 0x80U || second > 0x9fU || third < 0x80U || third > 0xbfU)
					return false;
				continue;
			}
			if (first >= 0xeeU && first <= 0xefU)
			{
				if (index + 2U >= value.size())
					return false;
				const auto second = static_cast<unsigned char>(value[++index]);
				const auto third = static_cast<unsigned char>(value[++index]);
				if (second < 0x80U || second > 0xbfU || third < 0x80U || third > 0xbfU)
					return false;
				continue;
			}
			if (first == 0xf0U)
			{
				if (index + 3U >= value.size())
					return false;
				const auto second = static_cast<unsigned char>(value[++index]);
				const auto third = static_cast<unsigned char>(value[++index]);
				const auto fourth = static_cast<unsigned char>(value[++index]);
				if (second < 0x90U || second > 0xbfU || third < 0x80U || third > 0xbfU ||
					fourth < 0x80U || fourth > 0xbfU)
					return false;
				continue;
			}
			if (first >= 0xf1U && first <= 0xf3U)
			{
				if (index + 3U >= value.size())
					return false;
				const auto second = static_cast<unsigned char>(value[++index]);
				const auto third = static_cast<unsigned char>(value[++index]);
				const auto fourth = static_cast<unsigned char>(value[++index]);
				if (second < 0x80U || second > 0xbfU || third < 0x80U || third > 0xbfU ||
					fourth < 0x80U || fourth > 0xbfU)
					return false;
				continue;
			}
			if (first == 0xf4U)
			{
				if (index + 3U >= value.size())
					return false;
				const auto second = static_cast<unsigned char>(value[++index]);
				const auto third = static_cast<unsigned char>(value[++index]);
				const auto fourth = static_cast<unsigned char>(value[++index]);
				if (second < 0x80U || second > 0x8fU || third < 0x80U || third > 0xbfU ||
					fourth < 0x80U || fourth > 0xbfU)
					return false;
				continue;
			}
			return false;
		}
		return true;
	}

	enum class json_kind : std::uint8_t
	{
		null_value,
		boolean,
		unsigned_integer,
		string,
		array,
		object,
	};

	struct json_value
	{
		using array_type = std::vector<json_value>;
		using object_type = std::map<std::string, json_value, std::less<>>;

		json_kind kind{json_kind::null_value};
		bool boolean{};
		std::uint64_t unsigned_integer{};
		std::string string;
		array_type array;
		object_type object;

		[[nodiscard]] static json_value null()
		{
			return {};
		}
		[[nodiscard]] static json_value boolean_value(const bool value)
		{
			json_value output;
			output.kind = json_kind::boolean;
			output.boolean = value;
			return output;
		}
		[[nodiscard]] static json_value unsigned_value(const std::uint64_t value)
		{
			json_value output;
			output.kind = json_kind::unsigned_integer;
			output.unsigned_integer = value;
			return output;
		}
		[[nodiscard]] static json_value string_value(std::string value)
		{
			json_value output;
			output.kind = json_kind::string;
			output.string = std::move(value);
			return output;
		}
		[[nodiscard]] static json_value array_value(array_type value)
		{
			json_value output;
			output.kind = json_kind::array;
			output.array = std::move(value);
			return output;
		}
		[[nodiscard]] static json_value object_value(object_type value)
		{
			json_value output;
			output.kind = json_kind::object;
			output.object = std::move(value);
			return output;
		}

		[[nodiscard]] const json_value* member(const std::string_view name) const noexcept
		{
			if (kind != json_kind::object)
				return nullptr;
			const auto found = object.find(name);
			return found == object.end() ? nullptr : &found->second;
		}
	};

	struct parse_error
	{
		std::string code;
		std::string field;
		std::string detail;
	};

	class json_parser final
	{
	  public:
		explicit json_parser(std::string_view input) : input_{input} {}

		[[nodiscard]] std::variant<json_value, parse_error> parse()
		{
			skip_space();
			auto value = parse_value(0U);
			if (std::holds_alternative<parse_error>(value))
				return value;
			skip_space();
			if (position_ != input_.size())
				return failure("doctor.project-invalid", "json", "trailing-bytes");
			return value;
		}

	  private:
		[[nodiscard]] parse_error
		failure(std::string code, std::string field, std::string detail) const
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		void skip_space() noexcept
		{
			while (position_ < input_.size())
			{
				const auto byte = static_cast<unsigned char>(input_[position_]);
				if (byte != ' ' && byte != '\n' && byte != '\r' && byte != '\t')
					break;
				++position_;
			}
		}

		[[nodiscard]] std::variant<json_value, parse_error> parse_value(const std::size_t depth)
		{
			if (depth > maximum_json_depth)
				return failure("doctor.project-invalid", "json", "depth-limit");
			skip_space();
			if (position_ == input_.size())
				return failure("doctor.project-invalid", "json", "unexpected-end");
			switch (input_[position_])
			{
				case '{':
					return parse_object(depth + 1U);
				case '[':
					return parse_array(depth + 1U);
				case '"':
					return parse_string_value();
				case 't':
					return parse_literal("true", json_value::boolean_value(true));
				case 'f':
					return parse_literal("false", json_value::boolean_value(false));
				case 'n':
					return parse_literal("null", json_value::null());
				default:
					if (input_[position_] >= '0' && input_[position_] <= '9')
						return parse_unsigned();
					return failure("doctor.project-invalid", "json", "unexpected-token");
			}
		}

		[[nodiscard]] std::variant<json_value, parse_error>
		parse_literal(const std::string_view literal, json_value value)
		{
			if (input_.substr(position_, literal.size()) != literal)
				return failure("doctor.project-invalid", "json", "invalid-literal");
			position_ += literal.size();
			return value;
		}

		[[nodiscard]] std::variant<json_value, parse_error> parse_unsigned()
		{
			const auto begin = position_;
			while (position_ < input_.size() && input_[position_] >= '0' &&
				   input_[position_] <= '9')
				++position_;
			std::uint64_t value{};
			const auto parsed =
				std::from_chars(input_.data() + begin, input_.data() + position_, value);
			if (parsed.ec != std::errc{} || parsed.ptr != input_.data() + position_)
				return failure("doctor.project-invalid", "json", "integer-overflow");
			if (position_ < input_.size() &&
				(input_[position_] == '.' || input_[position_] == 'e' || input_[position_] == 'E'))
				return failure("doctor.project-invalid", "json", "floating-point-forbidden");
			return json_value::unsigned_value(value);
		}

		[[nodiscard]] std::variant<json_value, parse_error> parse_string_value()
		{
			auto value = parse_string();
			if (std::holds_alternative<parse_error>(value))
				return std::get<parse_error>(std::move(value));
			return json_value::string_value(std::move(std::get<std::string>(value)));
		}

		[[nodiscard]] std::variant<std::uint32_t, parse_error> parse_hex_quad()
		{
			if (position_ + 4U > input_.size())
				return failure("doctor.project-invalid", "json", "unicode-escape-end");
			std::uint32_t value{};
			for (std::size_t index = 0U; index < 4U; ++index)
			{
				const auto byte = static_cast<unsigned char>(input_[position_++]);
				value <<= 4U;
				if (byte >= '0' && byte <= '9')
					value += byte - '0';
				else if (byte >= 'a' && byte <= 'f')
					value += byte - 'a' + 10U;
				else if (byte >= 'A' && byte <= 'F')
					value += byte - 'A' + 10U;
				else
					return failure("doctor.project-invalid", "json", "unicode-escape-digit");
			}
			return value;
		}

		static void append_codepoint(std::string& output, const std::uint32_t value)
		{
			if (value <= 0x7fU)
				output.push_back(static_cast<char>(value));
			else if (value <= 0x7ffU)
			{
				output.push_back(static_cast<char>(0xc0U | (value >> 6U)));
				output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
			}
			else if (value <= 0xffffU)
			{
				output.push_back(static_cast<char>(0xe0U | (value >> 12U)));
				output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
				output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
			}
			else
			{
				output.push_back(static_cast<char>(0xf0U | (value >> 18U)));
				output.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3fU)));
				output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
				output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
			}
		}

		[[nodiscard]] std::variant<std::string, parse_error> parse_string()
		{
			if (position_ >= input_.size() || input_[position_] != '"')
				return failure("doctor.project-invalid", "json", "string-required");
			++position_;
			std::string output;
			while (position_ < input_.size())
			{
				const char byte = input_[position_++];
				if (byte == '"')
				{
					if (!valid_utf8(output))
						return failure("doctor.project-invalid", "json", "invalid-utf8");
					return output;
				}
				if (static_cast<unsigned char>(byte) < 0x20U)
					return failure("doctor.project-invalid", "json", "control-character");
				if (byte != '\\')
				{
					output += byte;
					continue;
				}
				if (position_ >= input_.size())
					return failure("doctor.project-invalid", "json", "escape-end");
				const char escaped = input_[position_++];
				switch (escaped)
				{
					case '"':
					case '\\':
					case '/':
						output += escaped;
						break;
					case 'b':
						output += '\b';
						break;
					case 'f':
						output += '\f';
						break;
					case 'n':
						output += '\n';
						break;
					case 'r':
						output += '\r';
						break;
					case 't':
						output += '\t';
						break;
					case 'u':
					{
						auto codepoint = parse_hex_quad();
						if (std::holds_alternative<parse_error>(codepoint))
							return std::get<parse_error>(std::move(codepoint));
						auto value = std::get<std::uint32_t>(codepoint);
						if (value >= 0xd800U && value <= 0xdbffU)
						{
							if (position_ + 6U > input_.size() || input_[position_] != '\\' ||
								input_[position_ + 1U] != 'u')
								return failure(
									"doctor.project-invalid", "json", "unicode-surrogate-pair");
							position_ += 2U;
							auto low = parse_hex_quad();
							if (std::holds_alternative<parse_error>(low))
								return std::get<parse_error>(std::move(low));
							const auto low_value = std::get<std::uint32_t>(low);
							if (low_value < 0xdc00U || low_value > 0xdfffU)
								return failure(
									"doctor.project-invalid", "json", "unicode-surrogate-pair");
							value = 0x10000U + ((value - 0xd800U) << 10U) + (low_value - 0xdc00U);
						}
						else if (value >= 0xdc00U && value <= 0xdfffU)
							return failure(
								"doctor.project-invalid", "json", "unicode-surrogate-pair");
						append_codepoint(output, value);
						break;
					}
					default:
						return failure("doctor.project-invalid", "json", "unsupported-escape");
				}
			}
			return failure("doctor.project-invalid", "json", "unterminated-string");
		}

		[[nodiscard]] std::variant<json_value, parse_error> parse_array(const std::size_t depth)
		{
			++position_;
			json_value::array_type values;
			skip_space();
			if (position_ < input_.size() && input_[position_] == ']')
			{
				++position_;
				return json_value::array_value(std::move(values));
			}
			for (;;)
			{
				auto value = parse_value(depth);
				if (std::holds_alternative<parse_error>(value))
					return value;
				values.push_back(std::move(std::get<json_value>(value)));
				skip_space();
				if (position_ >= input_.size())
					return failure("doctor.project-invalid", "json", "array-end");
				if (input_[position_] == ']')
				{
					++position_;
					return json_value::array_value(std::move(values));
				}
				if (input_[position_] != ',')
					return failure("doctor.project-invalid", "json", "array-separator");
				++position_;
			}
		}

		[[nodiscard]] std::variant<json_value, parse_error> parse_object(const std::size_t depth)
		{
			++position_;
			json_value::object_type values;
			skip_space();
			if (position_ < input_.size() && input_[position_] == '}')
			{
				++position_;
				return json_value::object_value(std::move(values));
			}
			for (;;)
			{
				auto key = parse_string();
				if (std::holds_alternative<parse_error>(key))
					return std::get<parse_error>(std::move(key));
				skip_space();
				if (position_ >= input_.size() || input_[position_] != ':')
					return failure("doctor.project-invalid", "json", "object-colon");
				++position_;
				auto value = parse_value(depth);
				if (std::holds_alternative<parse_error>(value))
					return value;
				const auto [iterator, inserted] = values.emplace(
					std::get<std::string>(std::move(key)), std::move(std::get<json_value>(value)));
				(void)iterator;
				if (!inserted)
					return failure("doctor.project-invalid", "json", "duplicate-key");
				skip_space();
				if (position_ >= input_.size())
					return failure("doctor.project-invalid", "json", "object-end");
				if (input_[position_] == '}')
				{
					++position_;
					return json_value::object_value(std::move(values));
				}
				if (input_[position_] != ',')
					return failure("doctor.project-invalid", "json", "object-separator");
				++position_;
				skip_space();
			}
		}

		std::string_view input_;
		std::size_t position_{};
	};

	[[nodiscard]] inline std::string json_escape(const std::string_view value)
	{
		std::ostringstream output;
		for (const auto byte : value)
		{
			switch (byte)
			{
				case '"':
					output << "\\\"";
					break;
				case '\\':
					output << "\\\\";
					break;
				case '\b':
					output << "\\b";
					break;
				case '\f':
					output << "\\f";
					break;
				case '\n':
					output << "\\n";
					break;
				case '\r':
					output << "\\r";
					break;
				case '\t':
					output << "\\t";
					break;
				default:
					if (static_cast<unsigned char>(byte) < 0x20U)
					{
						static constexpr char digits[] = "0123456789abcdef";
						output << "\\u00"
							   << digits[(static_cast<unsigned char>(byte) >> 4U) & 0x0fU]
							   << digits[static_cast<unsigned char>(byte) & 0x0fU];
					}
					else
						output << byte;
			}
		}
		return output.str();
	}

	[[nodiscard]] inline std::string canonical_json(const json_value& value)
	{
		std::ostringstream output;
		switch (value.kind)
		{
			case json_kind::null_value:
				return "null";
			case json_kind::boolean:
				return value.boolean ? "true" : "false";
			case json_kind::unsigned_integer:
				return std::to_string(value.unsigned_integer);
			case json_kind::string:
				return "\"" + json_escape(value.string) + "\"";
			case json_kind::array:
				output << '[';
				for (std::size_t index = 0U; index < value.array.size(); ++index)
				{
					if (index != 0U)
						output << ',';
					output << canonical_json(value.array[index]);
				}
				output << ']';
				return output.str();
			case json_kind::object:
				output << '{';
				{
					bool first = true;
					for (const auto& [key, member] : value.object)
					{
						if (!first)
							output << ',';
						first = false;
						output << '"' << json_escape(key) << "\":" << canonical_json(member);
					}
				}
				output << '}';
				return output.str();
		}
		return {};
	}

	[[nodiscard]] inline std::string state_name(const std::string_view value)
	{
		return std::string{value};
	}

	inline constexpr std::array<std::string_view, 5U> result_states{
		"proved", "disproved", "unknown", "partial", "conflicting"};

	struct project_context
	{
		std::string project_id;
		std::string catalog_id;
		std::string catalog_digest;
		std::string logical_root;
		std::string environment_digest;
		bool source_input{};
		bool provider_input{};
		bool store_input{};
		std::string provider_id;
		std::string provider_version;
		std::uint32_t protocol_major{};
		std::uint32_t protocol_minor{};
		std::vector<std::string> offered_relations;
		std::vector<std::string> provider_features;
		std::vector<std::string> interpretation_domains;
		std::string store_backend;
		std::string store_format;
	};

	struct product_error
	{
		std::string code;
		std::string field;
		std::string detail;
	};

	[[nodiscard]] inline const json_value* required_member(const json_value& object,
														   const std::string_view name,
														   const std::string_view field,
														   std::string& error)
	{
		if (object.kind != json_kind::object)
		{
			error = "doctor.project-invalid:" + std::string{field};
			return nullptr;
		}
		const auto* value = object.member(name);
		if (value == nullptr)
		{
			error = "doctor.project-invalid:" + std::string{field} + ":missing";
			return nullptr;
		}
		return value;
	}

	[[nodiscard]] inline bool strict_id(const std::string_view value)
	{
		if (value.empty() || !valid_utf8(value))
			return false;
		for (const auto byte : value)
			if (static_cast<unsigned char>(byte) < 0x20U || byte == '\x7f')
				return false;
		return true;
	}

	[[nodiscard]] inline bool has_only(const json_value& object,
									   const std::span<const std::string_view> names,
									   std::string& error)
	{
		if (object.kind != json_kind::object)
		{
			error = "doctor.project-invalid:object-required";
			return false;
		}
		for (const auto& [key, unused] : object.object)
		{
			(void)unused;
			if (std::ranges::find(names, key) == names.end())
			{
				error = "doctor.project-invalid:unknown-field:" + key;
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] inline bool text_member(const json_value& object,
										  const std::string_view name,
										  const bool required,
										  std::string& output,
										  std::string& error)
	{
		const auto* value = object.member(name);
		if (value == nullptr)
		{
			if (required)
				error = "doctor.project-invalid:" + std::string{name} + ":missing";
			return !required;
		}
		if (value->kind != json_kind::string || !strict_id(value->string))
		{
			error = "doctor.project-invalid:" + std::string{name} + ":string-required";
			return false;
		}
		output = value->string;
		return true;
	}

	[[nodiscard]] inline bool unsigned_member(const json_value& object,
											  const std::string_view name,
											  const bool required,
											  std::uint32_t& output,
											  std::string& error)
	{
		const auto* value = object.member(name);
		if (value == nullptr)
		{
			if (required)
				error = "doctor.project-invalid:" + std::string{name} + ":missing";
			return !required;
		}
		if (value->kind != json_kind::unsigned_integer ||
			value->unsigned_integer > std::numeric_limits<std::uint32_t>::max())
		{
			error = "doctor.project-invalid:" + std::string{name} + ":u32-required";
			return false;
		}
		output = static_cast<std::uint32_t>(value->unsigned_integer);
		return true;
	}

	[[nodiscard]] inline bool string_array_member(const json_value& object,
												  const std::string_view name,
												  const bool required,
												  std::vector<std::string>& output,
												  std::string& error)
	{
		const auto* value = object.member(name);
		if (value == nullptr)
		{
			if (required)
				error = "doctor.project-invalid:" + std::string{name} + ":missing";
			return !required;
		}
		if (value->kind != json_kind::array)
		{
			error = "doctor.project-invalid:" + std::string{name} + ":array-required";
			return false;
		}
		std::set<std::string, std::less<>> unique;
		for (const auto& item : value->array)
		{
			if (item.kind != json_kind::string || !strict_id(item.string) ||
				!unique.insert(item.string).second)
			{
				error =
					"doctor.project-invalid:" + std::string{name} + ":unique-string-array-required";
				return false;
			}
			output.push_back(item.string);
		}
		return true;
	}

	[[nodiscard]] inline std::variant<project_context, product_error>
	parse_project_document(const std::string_view raw)
	{
		if (raw.empty() || raw.size() > maximum_project_bytes)
			return product_error{"doctor.project-invalid", "project", "byte-limit"};
		json_parser parser{raw};
		auto parsed = parser.parse();
		if (std::holds_alternative<parse_error>(parsed))
		{
			auto error = std::get<parse_error>(std::move(parsed));
			return product_error{
				std::move(error.code), std::move(error.field), std::move(error.detail)};
		}
		const auto& root = std::get<json_value>(parsed);
		std::string error;
		static constexpr std::array<std::string_view, 3U> root_fields{
			"schema", "document_version", "project"};
		if (!has_only(root, root_fields, error))
			return product_error{"doctor.project-invalid", "project", std::move(error)};
		std::string schema;
		std::string version;
		const auto* schema_value = required_member(root, "schema", "schema", error);
		if (schema_value == nullptr || schema_value->kind != json_kind::string ||
			schema_value->string != "cxxlens.sdk-doctor-project.v1")
			return product_error{
				"doctor.project-invalid", "schema", "expected-cxxlens-sdk-doctor-project-v1"};
		const auto* version_value =
			required_member(root, "document_version", "document_version", error);
		if (version_value == nullptr || version_value->kind != json_kind::string ||
			version_value->string != "1.0.0")
			return product_error{"doctor.project-invalid", "document_version", "expected-1.0.0"};
		const auto* project = required_member(root, "project", "project", error);
		if (project == nullptr)
			return product_error{"doctor.project-invalid", "project", std::move(error)};
		static constexpr std::array<std::string_view, 8U> project_fields{"project_id",
																		 "catalog_id",
																		 "catalog_digest",
																		 "logical_root",
																		 "environment_digest",
																		 "source_input",
																		 "provider",
																		 "store"};
		if (!has_only(*project, project_fields, error))
			return product_error{"doctor.project-invalid", "project", std::move(error)};
		project_context output;
		if (!text_member(*project, "project_id", true, output.project_id, error) ||
			!text_member(*project, "catalog_id", true, output.catalog_id, error) ||
			!text_member(*project, "catalog_digest", true, output.catalog_digest, error) ||
			!text_member(*project, "logical_root", true, output.logical_root, error) ||
			!text_member(*project, "environment_digest", true, output.environment_digest, error))
			return product_error{"doctor.project-invalid", "project", std::move(error)};
		if (output.logical_root.rfind("project://", 0U) != 0U)
			return product_error{"doctor.project-invalid", "logical_root", "project-uri-required"};

		if (const auto* source = project->member("source_input"); source != nullptr)
		{
			static constexpr std::array<std::string_view, 2U> fields{"source_snapshot_id",
																	 "compilation_database_id"};
			if (!has_only(*source, fields, error) ||
				!text_member(*source, "source_snapshot_id", true, schema, error) ||
				!text_member(*source, "compilation_database_id", true, version, error))
				return product_error{"doctor.project-invalid", "source_input", std::move(error)};
			output.source_input = true;
		}

		if (const auto* provider = project->member("provider"); provider != nullptr)
		{
			static constexpr std::array<std::string_view, 8U> fields{"provider_id",
																	 "provider_version",
																	 "protocol_major",
																	 "protocol_minor",
																	 "offered_relations",
																	 "features",
																	 "interpretation_domains",
																	 "sandbox_minimum"};
			if (!has_only(*provider, fields, error) ||
				!text_member(*provider, "provider_id", true, output.provider_id, error) ||
				!text_member(*provider, "provider_version", true, output.provider_version, error) ||
				!unsigned_member(*provider, "protocol_major", true, output.protocol_major, error) ||
				!unsigned_member(*provider, "protocol_minor", true, output.protocol_minor, error) ||
				!string_array_member(
					*provider, "offered_relations", true, output.offered_relations, error) ||
				!string_array_member(
					*provider, "features", true, output.provider_features, error) ||
				!string_array_member(*provider,
									 "interpretation_domains",
									 true,
									 output.interpretation_domains,
									 error) ||
				!text_member(*provider, "sandbox_minimum", true, schema, error))
				return product_error{"doctor.project-invalid", "provider", std::move(error)};
			output.provider_input = true;
		}

		if (const auto* store = project->member("store"); store != nullptr)
		{
			static constexpr std::array<std::string_view, 2U> fields{"backend", "format"};
			if (!has_only(*store, fields, error) ||
				!text_member(*store, "backend", true, output.store_backend, error) ||
				!text_member(*store, "format", true, output.store_format, error))
				return product_error{"doctor.project-invalid", "store", std::move(error)};
			output.store_input = true;
		}
		return output;
	}

	struct relation_check
	{
		std::string id;
		std::string state;
		std::string reason_code;
	};

	[[nodiscard]] inline result<relation_registry> known_relation_registry()
	{
		relation_registry registry;
		const std::array descriptors{
			build::relations::compile_unit::descriptor(),
			build::relations::project::descriptor(),
			build::relations::toolchain_context::descriptor(),
			build::relations::variant::descriptor(),
			cc::relations::call_direct_target::descriptor(),
			cc::relations::call_site::descriptor(),
			cc::relations::declaration::descriptor(),
			cc::relations::entity::descriptor(),
			cc::relations::type::descriptor(),
			cc::relations::type_component::descriptor(),
			company::relations::lock_acquire::descriptor(),
			core::relations::claim_conflict::descriptor(),
			core::relations::differential_disagreement::descriptor(),
			core::relations::provider_execution::descriptor(),
			core::relations::unresolved::descriptor(),
			source::relations::file::descriptor(),
			source::relations::origin::descriptor(),
			source::relations::span::descriptor(),
		};
		for (const auto& descriptor : descriptors)
		{
			if (auto added = registry.add(descriptor); !added)
				return added.error();
		}
		return registry;
	}

	[[nodiscard]] inline std::optional<std::pair<std::string_view, std::uint32_t>>
	split_relation_id(const std::string_view id)
	{
		if (id.empty())
			return std::nullopt;
		const auto marker = id.rfind(".v");
		if (marker == std::string_view::npos || marker == 0U || marker + 2U == id.size())
			return std::nullopt;
		const auto name = id.substr(0U, marker);
		if (name.front() == '.' || name.back() == '.' || name.find("..") != std::string_view::npos)
			return std::nullopt;
		for (const auto byte : name)
			if ((byte < 'a' || byte > 'z') && (byte < '0' || byte > '9') && byte != '.' &&
				byte != '_')
				return std::nullopt;
		std::uint32_t major{};
		const auto digits = id.substr(marker + 2U);
		const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), major);
		if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
			return std::nullopt;
		return std::pair{name, major};
	}

	[[nodiscard]] inline std::variant<std::vector<relation_check>, product_error>
	check_relations(const std::span<const std::string_view> relation_ids)
	{
		if (relation_ids.empty())
			return product_error{"doctor.relation-request-invalid", "relation", "empty"};
		auto registry = known_relation_registry();
		if (!registry)
			return product_error{registry.error().code, "relation", registry.error().detail};
		std::vector<relation_check> output;
		output.reserve(relation_ids.size());
		for (const auto id : relation_ids)
		{
			const auto parsed = split_relation_id(id);
			if (!parsed)
				return product_error{"doctor.relation-request-invalid", "relation", "malformed-id"};
			auto found = registry->require(parsed->first, parsed->second);
			if (found)
				output.push_back({std::string{id}, "proved", "none"});
			else
				output.push_back({std::string{id}, "unknown", found.error().code});
		}
		return output;
	}

	enum class capability_kind : std::uint8_t
	{
		input,
		provider,
		relation,
		query,
		store,
		recipe,
	};

	struct capability_spec
	{
		std::string id;
		capability_kind kind;
		std::vector<std::string> dependencies;
		std::vector<std::string> relation_ids;
	};

	struct use_case_spec
	{
		std::string id;
		std::string consumer;
		std::string question;
		std::vector<capability_spec> capabilities;
	};

	[[nodiscard]] inline use_case_spec materialize_and_query_use_case()
	{
		return {"cxxlens.clang22.materialize-and-query.v1",
				"semantic-query-consumer",
				"Can this project be materialized and queried with semantic partiality preserved?",
				{
					{"input.project-catalog.v1", capability_kind::input, {}, {}},
					{"input.source-closure.v1",
					 capability_kind::input,
					 {"input.project-catalog.v1"},
					 {}},
					{"provider.protocol.v2",
					 capability_kind::provider,
					 {"input.project-catalog.v1"},
					 {}},
					{"provider.source-closure.v1",
					 capability_kind::provider,
					 {"provider.protocol.v2", "input.source-closure.v1"},
					 {}},
					{"relation.cc-entity.v1",
					 capability_kind::relation,
					 {"provider.protocol.v2"},
					 {"cc.entity.v1"}},
					{"relation.cc-call-site.v1",
					 capability_kind::relation,
					 {"provider.protocol.v2"},
					 {"cc.call_site.v1"}},
					{"query.logical-ir.v1",
					 capability_kind::query,
					 {"relation.cc-entity.v1", "relation.cc-call-site.v1"},
					 {}},
					{"store.snapshot.v3", capability_kind::store, {"input.project-catalog.v1"}, {}},
					{"recipe.calls-to-function.v1",
					 capability_kind::recipe,
					 {"query.logical-ir.v1", "store.snapshot.v3", "provider.source-closure.v1"},
					 {}},
				}};
	}

	struct capability_result
	{
		std::string id;
		std::string kind;
		std::string state;
		std::string reason_code;
		std::vector<std::string> dependencies;
	};

	struct missing_result
	{
		std::string capability_id;
		std::string reason_code;
		std::string explanation;
	};

	struct plan_step
	{
		std::string id;
		std::vector<std::string> dependencies;
		std::string action;
		std::string unlocks;
	};

	struct resolution
	{
		std::string use_case_id;
		std::string consumer;
		std::string question;
		std::string state;
		std::string reason_code;
		std::vector<capability_result> capability_path;
		std::vector<missing_result> missing;
		std::vector<plan_step> completion_plan;
		std::vector<std::string> unresolved;
		std::vector<std::string> conflicts;
	};

	[[nodiscard]] inline std::string kind_name(const capability_kind kind)
	{
		switch (kind)
		{
			case capability_kind::input:
				return "input";
			case capability_kind::provider:
				return "provider";
			case capability_kind::relation:
				return "relation";
			case capability_kind::query:
				return "query";
			case capability_kind::store:
				return "store";
			case capability_kind::recipe:
				return "recipe";
		}
		return "unknown";
	}

	[[nodiscard]] inline std::pair<std::string, std::string>
	evaluate_capability(const capability_spec& capability,
						const project_context& project,
						const std::map<std::string, capability_result, std::less<>>& prior,
						const relation_registry& registry)
	{
		for (const auto& dependency : capability.dependencies)
		{
			const auto found = prior.find(dependency);
			if (found == prior.end() || found->second.state == "conflicting")
				return {"unknown", "doctor.missing-capability"};
			if (found->second.state == "disproved")
				return {"disproved", "doctor.disproved-dependency"};
			if (found->second.state != "proved")
				return {"partial", "doctor.missing-capability"};
		}
		switch (capability.kind)
		{
			case capability_kind::input:
				if (capability.id == "input.project-catalog.v1")
					return {"proved", "doctor.none"};
				if (capability.id == "input.source-closure.v1")
					return {project.source_input ? "proved" : "unknown",
							project.source_input ? "doctor.none" : "doctor.missing-input"};
				return {"unknown", "doctor.missing-input"};
			case capability_kind::provider:
				if (!project.provider_input)
					return {"unknown", "doctor.missing-capability"};
				if (capability.id == "provider.protocol.v2")
					return {project.protocol_major == 2U ? "proved" : "disproved",
							project.protocol_major == 2U ? "doctor.none"
														 : "doctor.unsupported-tuple"};
				if (capability.id == "provider.source-closure.v1")
				{
					const auto found =
						std::ranges::find(project.provider_features, "task-source-closure-v2");
					return {found != project.provider_features.end() ? "proved" : "disproved",
							found != project.provider_features.end() ? "doctor.none"
																	 : "doctor.unsupported-tuple"};
				}
				return {"unknown", "doctor.missing-capability"};
			case capability_kind::relation:
				for (const auto& relation_id : capability.relation_ids)
				{
					const auto parsed = split_relation_id(relation_id);
					if (!parsed || !registry.require(parsed->first, parsed->second))
						return {"unknown", "doctor.missing-capability"};
					if (!project.provider_input ||
						std::ranges::find(project.offered_relations, relation_id) ==
							project.offered_relations.end())
						return {"partial", "doctor.missing-capability"};
				}
				return {"proved", "doctor.none"};
			case capability_kind::query:
				return {"proved", "doctor.none"};
			case capability_kind::store:
				if (!project.store_input)
					return {"unknown", "doctor.missing-input"};
				if ((project.store_backend == "memory" || project.store_backend == "sqlite") &&
					project.store_format == "cxxlens.snapshot.v3")
					return {"proved", "doctor.none"};
				return {"disproved", "doctor.unsupported-tuple"};
			case capability_kind::recipe:
				return {"proved", "doctor.none"};
		}
		return {"unknown", "doctor.missing-capability"};
	}

	[[nodiscard]] inline std::variant<resolution, product_error>
	resolve(const std::string_view use_case_id, const project_context& project)
	{
		if (use_case_id != "cxxlens.clang22.materialize-and-query.v1")
			return product_error{"doctor.unknown-use-case", "use_case_id", "not-admitted"};
		auto registry = known_relation_registry();
		if (!registry)
			return product_error{registry.error().code, "relation", registry.error().detail};
		const auto use_case = materialize_and_query_use_case();
		resolution output;
		output.use_case_id = use_case.id;
		output.consumer = use_case.consumer;
		output.question = use_case.question;
		output.state = "unknown";
		output.reason_code = "doctor.none";
		std::map<std::string, capability_result, std::less<>> by_id;
		for (const auto& capability : use_case.capabilities)
		{
			if (by_id.size() >= maximum_capability_count)
				return product_error{"doctor.resolution-limit", "capability_path", "count"};
			const auto [state, reason] = evaluate_capability(capability, project, by_id, *registry);
			capability_result result{
				capability.id, kind_name(capability.kind), state, reason, capability.dependencies};
			by_id.emplace(capability.id, result);
			output.capability_path.push_back(std::move(result));
			if (state == "unknown" || state == "partial")
				output.unresolved.push_back(capability.id);
			if (state == "conflicting")
				output.conflicts.push_back(capability.id);
			if (state != "proved")
			{
				const auto action = state == "disproved"
					? "Select a product-supported provider or store tuple for this capability."
					: capability.kind == capability_kind::input
					? "Supply the missing product input in the project context."
					: "Provide the required product capability and its contract values.";
				output.missing.push_back(
					{capability.id,
					 reason,
					 "The capability cannot be proved for this project context."});
				output.completion_plan.push_back({"completion." + capability.id,
												  capability.dependencies,
												  action,
												  capability.id});
			}
		}
		const bool any_conflict = std::ranges::any_of(output.capability_path,
													  [](const capability_result& item)
													  {
														  return item.state == "conflicting";
													  });
		const bool any_disproved = std::ranges::any_of(output.capability_path,
													   [](const capability_result& item)
													   {
														   return item.state == "disproved";
													   });
		const bool any_proved = std::ranges::any_of(output.capability_path,
													[](const capability_result& item)
													{
														return item.state == "proved";
													});
		const bool any_unresolved =
			std::ranges::any_of(output.capability_path,
								[](const capability_result& item)
								{
									return item.state == "unknown" || item.state == "partial";
								});
		if (any_conflict)
			output.state = "conflicting", output.reason_code = "doctor.conflicting-capability";
		else if (any_unresolved && any_proved)
			output.state = "partial", output.reason_code = output.missing.front().reason_code;
		else if (any_unresolved)
			output.state = "unknown", output.reason_code = output.missing.front().reason_code;
		else if (any_disproved)
			output.state = "disproved", output.reason_code = output.missing.front().reason_code;
		else
			output.state = "proved", output.reason_code = "doctor.none";
		return output;
	}

	[[nodiscard]] inline json_value to_json(const resolution& value)
	{
		json_value::array_type path;
		for (const auto& item : value.capability_path)
		{
			json_value::array_type dependencies;
			for (const auto& dependency : item.dependencies)
				dependencies.push_back(json_value::string_value(dependency));
			path.push_back(json_value::object_value({
				{"id", json_value::string_value(item.id)},
				{"kind", json_value::string_value(item.kind)},
				{"requires", json_value::array_value(std::move(dependencies))},
				{"reason_code", json_value::string_value(item.reason_code)},
				{"state", json_value::string_value(item.state)},
			}));
		}
		json_value::array_type missing;
		for (const auto& item : value.missing)
			missing.push_back(json_value::object_value({
				{"capability_id", json_value::string_value(item.capability_id)},
				{"explanation", json_value::string_value(item.explanation)},
				{"reason_code", json_value::string_value(item.reason_code)},
			}));
		json_value::array_type plan;
		for (const auto& item : value.completion_plan)
		{
			json_value::array_type dependencies;
			for (const auto& dependency : item.dependencies)
				dependencies.push_back(json_value::string_value(dependency));
			plan.push_back(json_value::object_value({
				{"action", json_value::string_value(item.action)},
				{"id", json_value::string_value(item.id)},
				{"requires", json_value::array_value(std::move(dependencies))},
				{"unlocks", json_value::string_value(item.unlocks)},
			}));
		}
		json_value::array_type unresolved;
		for (const auto& item : value.unresolved)
			unresolved.push_back(json_value::string_value(item));
		json_value::array_type conflicts;
		for (const auto& item : value.conflicts)
			conflicts.push_back(json_value::string_value(item));
		return json_value::object_value({
			{"capability_path", json_value::array_value(std::move(path))},
			{"completion_plan", json_value::array_value(std::move(plan))},
			{"missing", json_value::array_value(std::move(missing))},
			{"preserved_semantics",
			 json_value::object_value({
				 {"closure",
				  json_value::array_value(
					  {json_value::string_value(value.missing.empty() ? "dependency-graph-closed"
																	  : "dependency-graph-open")})},
				 {"conflict", json_value::array_value(std::move(conflicts))},
				 {"coverage",
				  json_value::array_value({json_value::string_value("capability-path")})},
				 {"differential_disagreement", json_value::array_value({})},
				 {"guarantee",
				  json_value::array_value(
					  {json_value::string_value("unknown-not-collapsed-to-empty-success"),
					   json_value::string_value("product-only-diagnosis")})},
				 {"logical_explain", json_value::array_value(unresolved)},
				 {"physical_explain", json_value::array_value({})},
				 {"provenance",
				  json_value::array_value(
					  {json_value::string_value("cxxlens.sdk-doctor-project.v1"),
					   json_value::string_value("cxxlens.sdk-doctor-use-case-catalog.v1")})},
				 {"unresolved", json_value::array_value(std::move(unresolved))},
			 })},
			{"question", json_value::string_value(value.question)},
			{"result",
			 json_value::object_value({
				 {"explanation",
				  json_value::string_value("Capability resolution is derived from product context "
										   "and explicit dependencies.")},
				 {"guarantee",
				  json_value::string_value(
					  "Product-only values are evaluated; unknown remains explicit.")},
				 {"reason_code", json_value::string_value(value.reason_code)},
				 {"state", json_value::string_value(value.state)},
			 })},
			{"schema", json_value::string_value("cxxlens.sdk-doctor-resolution.v1")},
			{"use_case_id", json_value::string_value(value.use_case_id)},
			{"consumer", json_value::string_value(value.consumer)},
			{"document_version", json_value::string_value("1.0.0")},
		});
	}

	[[nodiscard]] inline json_value to_json(const std::vector<relation_check>& checks)
	{
		json_value::array_type components;
		std::size_t missing{};
		for (const auto& item : checks)
		{
			if (item.state != "proved")
				++missing;
			json_value::object_type component{
				{"id", json_value::string_value(item.id)},
				{"reason_code", json_value::string_value(item.reason_code)},
				{"state", json_value::string_value(item.state)},
			};
			components.push_back(json_value::object_value(std::move(component)));
		}
		return json_value::object_value({
			{"components", json_value::array_value(std::move(components))},
			{"mode", json_value::string_value("relation-presence")},
			{"missing", json_value::unsigned_value(missing)},
			{"requested", json_value::unsigned_value(checks.size())},
			{"schema", json_value::string_value("cxxlens.sdk-doctor-relation-presence.v1")},
			{"state", json_value::string_value(missing == 0U ? "proved" : "unknown")},
			{"document_version", json_value::string_value("1.0.0")},
		});
	}

	[[nodiscard]] inline std::string markdown_projection(const std::string_view json)
	{
		return "# cxxlens SDK capability diagnosis\n\n```json\n" + std::string{json} + "\n```\n";
	}

	[[nodiscard]] inline std::string read_file(const std::string_view path, std::string& error)
	{
		std::ifstream input{std::string{path}, std::ios::binary};
		if (!input)
		{
			error = "doctor.project-invalid:project:open";
			return {};
		}
		std::ostringstream output;
		output << input.rdbuf();
		return output.str();
	}
} // namespace cxxlens::sdk::doctor
