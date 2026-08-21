#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <span>
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

namespace
{
	/**
	 * @brief Build a registry over every relation descriptor this SDK build ships.
	 *
	 * This is the exact set of generated relation tags under `include/cxxlens/relations`; it is
	 * the real, already-shipped relation catalog, not an invented capability list.
	 */
	[[nodiscard]] cxxlens::sdk::result<cxxlens::sdk::relation_registry> known_relation_registry()
	{
		cxxlens::sdk::relation_registry registry;
		const std::array descriptors{
			cxxlens::build::relations::compile_unit::descriptor(),
			cxxlens::build::relations::project::descriptor(),
			cxxlens::build::relations::toolchain_context::descriptor(),
			cxxlens::build::relations::variant::descriptor(),
			cxxlens::cc::relations::call_direct_target::descriptor(),
			cxxlens::cc::relations::call_site::descriptor(),
			cxxlens::cc::relations::declaration::descriptor(),
			cxxlens::cc::relations::entity::descriptor(),
			cxxlens::cc::relations::type::descriptor(),
			cxxlens::cc::relations::type_component::descriptor(),
			cxxlens::company::relations::lock_acquire::descriptor(),
			cxxlens::core::relations::claim_conflict::descriptor(),
			cxxlens::core::relations::differential_disagreement::descriptor(),
			cxxlens::core::relations::provider_execution::descriptor(),
			cxxlens::core::relations::unresolved::descriptor(),
			cxxlens::source::relations::file::descriptor(),
			cxxlens::source::relations::origin::descriptor(),
			cxxlens::source::relations::span::descriptor(),
		};
		for (const auto& descriptor : descriptors)
			if (auto added = registry.add(descriptor); !added)
				return added.error();
		return registry;
	}

	/**
	 * @brief Split a canonical `"<dotted.lowercase.name>.v<major>"` relation ID.
	 * @return The name and major version, or nothing when `id` is not that exact shape.
	 */
	[[nodiscard]] std::optional<std::pair<std::string_view, std::uint32_t>>
	split_relation_id(const std::string_view id)
	{
		const auto canonical_character = [](const char byte)
		{
			return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '.' ||
				byte == '_';
		};
		if (id.empty() || !std::ranges::all_of(id, canonical_character))
			return std::nullopt;
		const auto separator = id.rfind(".v");
		if (separator == std::string_view::npos || separator == 0U)
			return std::nullopt;
		const auto name = id.substr(0U, separator);
		const auto version_digits = id.substr(separator + 2U);
		if (name.empty() || version_digits.empty())
			return std::nullopt;
		std::uint32_t major{};
		const auto parsed = std::from_chars(
			version_digits.data(), version_digits.data() + version_digits.size(), major);
		if (parsed.ec != std::errc{} || parsed.ptr != version_digits.data() + version_digits.size())
			return std::nullopt;
		return std::pair{name, major};
	}

	// This deliberately small JSON value/parser is used only for the installed doctor
	// control plane.  It accepts the canonical JSON projection emitted by the agent
	// capability engine and never interprets diagnostic prose as a capability fact.
	struct json_value
	{
		using array = std::vector<json_value>;
		using object = std::map<std::string, json_value, std::less<>>;
		using storage = std::
			variant<std::monostate, bool, std::int64_t, std::uint64_t, std::string, array, object>;

		storage value{};

		json_value() = default;
		explicit json_value(bool input) : value(input) {}
		explicit json_value(std::int64_t input) : value(input) {}
		explicit json_value(std::uint64_t input) : value(input) {}
		explicit json_value(std::string input) : value(std::move(input)) {}
		explicit json_value(array input) : value(std::move(input)) {}
		explicit json_value(object input) : value(std::move(input)) {}

		[[nodiscard]] const object* as_object() const noexcept
		{
			return std::get_if<object>(&value);
		}
		[[nodiscard]] const array* as_array() const noexcept
		{
			return std::get_if<array>(&value);
		}
		[[nodiscard]] const std::string* as_string() const noexcept
		{
			return std::get_if<std::string>(&value);
		}
		[[nodiscard]] const bool* as_boolean() const noexcept
		{
			return std::get_if<bool>(&value);
		}
		[[nodiscard]] const std::uint64_t* as_unsigned() const noexcept
		{
			return std::get_if<std::uint64_t>(&value);
		}
		[[nodiscard]] const std::int64_t* as_signed() const noexcept
		{
			return std::get_if<std::int64_t>(&value);
		}
	};

	class json_parser
	{
	  public:
		explicit json_parser(std::string_view input) : input_(input) {}

		[[nodiscard]] std::optional<json_value> parse(std::string& error)
		{
			skip_space();
			auto result = parse_value(error, 0U);
			if (!result)
				return std::nullopt;
			skip_space();
			if (position_ != input_.size())
			{
				error = "trailing-json-bytes";
				return std::nullopt;
			}
			return result;
		}

	  private:
		[[nodiscard]] bool consume(const char expected)
		{
			if (position_ >= input_.size() || input_[position_] != expected)
				return false;
			++position_;
			return true;
		}

		void skip_space()
		{
			while (position_ < input_.size() &&
				   std::isspace(static_cast<unsigned char>(input_[position_])) != 0)
				++position_;
		}

		[[nodiscard]] std::optional<json_value> parse_value(std::string& error,
															const unsigned depth)
		{
			if (depth > 64U)
			{
				error = "json-depth-limit";
				return std::nullopt;
			}
			skip_space();
			if (position_ >= input_.size())
			{
				error = "json-value-missing";
				return std::nullopt;
			}
			switch (input_[position_])
			{
				case '{':
					return parse_object(error, depth + 1U);
				case '[':
					return parse_array(error, depth + 1U);
				case '"':
				{
					auto text = parse_string(error);
					return text ? std::optional<json_value>{json_value{std::move(*text)}}
								: std::nullopt;
				}
				case 't':
					return parse_literal("true", json_value{true}, error);
				case 'f':
					return parse_literal("false", json_value{false}, error);
				case 'n':
					return parse_literal("null", json_value{}, error);
				default:
					if (input_[position_] == '-' ||
						(input_[position_] >= '0' && input_[position_] <= '9'))
						return parse_number(error);
					error = "json-token-invalid";
					return std::nullopt;
			}
		}

		[[nodiscard]] std::optional<json_value>
		parse_literal(const std::string_view literal, json_value result, std::string& error)
		{
			if (input_.substr(position_, literal.size()) != literal)
			{
				error = "json-literal-invalid";
				return std::nullopt;
			}
			position_ += literal.size();
			return result;
		}

		[[nodiscard]] std::optional<std::string> parse_string(std::string& error)
		{
			if (!consume('"'))
			{
				error = "json-string-opening-quote-missing";
				return std::nullopt;
			}
			std::string output;
			while (position_ < input_.size())
			{
				const char byte = input_[position_++];
				if (byte == '"')
					return output;
				if (static_cast<unsigned char>(byte) < 0x20U)
				{
					error = "json-string-control-byte";
					return std::nullopt;
				}
				if (byte != '\\')
				{
					output.push_back(byte);
					continue;
				}
				if (position_ >= input_.size())
				{
					error = "json-string-escape-truncated";
					return std::nullopt;
				}
				const char escaped = input_[position_++];
				switch (escaped)
				{
					case '"':
						output.push_back('"');
						break;
					case '\\':
						output.push_back('\\');
						break;
					case '/':
						output.push_back('/');
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
						if (input_.size() - position_ < 4U)
						{
							error = "json-unicode-escape-truncated";
							return std::nullopt;
						}
						std::uint32_t codepoint{};
						for (unsigned index = 0U; index < 4U; ++index)
						{
							const char hex = input_[position_++];
							codepoint <<= 4U;
							if (hex >= '0' && hex <= '9')
								codepoint += static_cast<unsigned>(hex - '0');
							else if (hex >= 'a' && hex <= 'f')
								codepoint += static_cast<unsigned>(hex - 'a' + 10);
							else if (hex >= 'A' && hex <= 'F')
								codepoint += static_cast<unsigned>(hex - 'A' + 10);
							else
							{
								error = "json-unicode-escape-invalid";
								return std::nullopt;
							}
						}
						if (codepoint <= 0x7fU)
							output.push_back(static_cast<char>(codepoint));
						else if (codepoint <= 0x7ffU)
						{
							output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
							output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
						}
						else
						{
							output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
							output.push_back(
								static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
							output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
						}
						break;
					}
					default:
						error = "json-string-escape-invalid";
						return std::nullopt;
				}
			}
			error = "json-string-closing-quote-missing";
			return std::nullopt;
		}

		[[nodiscard]] std::optional<json_value> parse_number(std::string& error)
		{
			const auto start = position_;
			if (position_ < input_.size() && input_[position_] == '-')
				++position_;
			const auto digits_start = position_;
			while (position_ < input_.size() &&
				   std::isdigit(static_cast<unsigned char>(input_[position_])) != 0)
				++position_;
			if (digits_start == position_ ||
				(position_ < input_.size() &&
				 (input_[position_] == '.' || input_[position_] == 'e' ||
				  input_[position_] == 'E')))
			{
				error = "json-number-not-an-integer";
				return std::nullopt;
			}
			const auto text = input_.substr(start, position_ - start);
			if (text.front() == '-')
			{
				std::int64_t signed_value{};
				const auto parsed =
					std::from_chars(text.data(), text.data() + text.size(), signed_value);
				if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
				{
					error = "json-number-out-of-range";
					return std::nullopt;
				}
				return json_value{signed_value};
			}
			std::uint64_t unsigned_value{};
			const auto parsed =
				std::from_chars(text.data(), text.data() + text.size(), unsigned_value);
			if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
			{
				error = "json-number-out-of-range";
				return std::nullopt;
			}
			return json_value{unsigned_value};
		}

		[[nodiscard]] std::optional<json_value> parse_object(std::string& error,
															 const unsigned depth)
		{
			if (!consume('{'))
			{
				error = "json-object-opening-brace-missing";
				return std::nullopt;
			}
			json_value::object output;
			skip_space();
			if (consume('}'))
				return json_value{std::move(output)};
			while (true)
			{
				auto key = parse_string(error);
				if (!key)
					return std::nullopt;
				skip_space();
				if (!consume(':'))
				{
					error = "json-object-colon-missing";
					return std::nullopt;
				}
				auto child = parse_value(error, depth);
				if (!child || !output.emplace(std::move(*key), std::move(*child)).second)
				{
					if (error.empty())
						error = "json-object-duplicate-key";
					return std::nullopt;
				}
				skip_space();
				if (consume('}'))
					return json_value{std::move(output)};
				if (!consume(','))
				{
					error = "json-object-comma-missing";
					return std::nullopt;
				}
				skip_space();
			}
		}

		[[nodiscard]] std::optional<json_value> parse_array(std::string& error,
															const unsigned depth)
		{
			if (!consume('['))
			{
				error = "json-array-opening-bracket-missing";
				return std::nullopt;
			}
			json_value::array output;
			skip_space();
			if (consume(']'))
				return json_value{std::move(output)};
			while (true)
			{
				auto child = parse_value(error, depth);
				if (!child)
					return std::nullopt;
				output.push_back(std::move(*child));
				skip_space();
				if (consume(']'))
					return json_value{std::move(output)};
				if (!consume(','))
				{
					error = "json-array-comma-missing";
					return std::nullopt;
				}
				skip_space();
			}
		}

		std::string_view input_;
		std::size_t position_{};
	};

	[[nodiscard]] std::string json_escape(const std::string_view text)
	{
		std::string output;
		for (const char raw_byte : text)
		{
			const auto byte = static_cast<unsigned char>(raw_byte);
			switch (byte)
			{
				case '"':
					output += "\\\"";
					break;
				case '\\':
					output += "\\\\";
					break;
				case '\b':
					output += "\\b";
					break;
				case '\f':
					output += "\\f";
					break;
				case '\n':
					output += "\\n";
					break;
				case '\r':
					output += "\\r";
					break;
				case '\t':
					output += "\\t";
					break;
				default:
					if (byte < 0x20U)
					{
						static constexpr char hex[] = "0123456789abcdef";
						output += "\\u00";
						output.push_back(hex[byte >> 4U]);
						output.push_back(hex[byte & 0x0fU]);
					}
					else
						output.push_back(static_cast<char>(byte));
			}
		}
		return output;
	}

	[[nodiscard]] std::string json_dump(const json_value& value)
	{
		if (std::holds_alternative<std::monostate>(value.value))
			return "null";
		if (const auto* boolean = std::get_if<bool>(&value.value))
			return *boolean ? "true" : "false";
		if (const auto* signed_value = std::get_if<std::int64_t>(&value.value))
			return std::to_string(*signed_value);
		if (const auto* unsigned_value = std::get_if<std::uint64_t>(&value.value))
			return std::to_string(*unsigned_value);
		if (const auto* text = value.as_string())
			return "\"" + json_escape(*text) + "\"";
		if (const auto* array = value.as_array())
		{
			std::string output = "[";
			for (std::size_t index = 0U; index < array->size(); ++index)
			{
				if (index != 0U)
					output.push_back(',');
				output += json_dump((*array)[index]);
			}
			return output + "]";
		}
		const auto* object = value.as_object();
		std::string output = "{";
		std::size_t index{};
		for (const auto& [key, child] : *object)
		{
			if (index++ != 0U)
				output.push_back(',');
			output += "\"" + json_escape(key) + "\":" + json_dump(child);
		}
		return output + "}";
	}

	[[nodiscard]] const json_value* member(const json_value::object& object,
										   const std::string_view key)
	{
		const auto found = object.find(key);
		return found == object.end() ? nullptr : &found->second;
	}

	[[nodiscard]] std::string text_member(const json_value::object& object,
										  const std::string_view key,
										  const std::string_view fallback = {})
	{
		const auto* value = member(object, key);
		const auto* text = value == nullptr ? nullptr : value->as_string();
		return text == nullptr ? std::string{fallback} : *text;
	}

	[[nodiscard]] std::optional<json_value> read_json(const std::string_view path,
													  std::string& error)
	{
		if (path.empty())
		{
			error = "json-path-empty";
			return std::nullopt;
		}
		std::ifstream input{std::string{path}, std::ios::binary};
		if (!input)
		{
			error = "json-input-open-failed";
			return std::nullopt;
		}
		std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
		if (bytes.size() > 16U * 1024U * 1024U)
		{
			error = "json-input-too-large";
			return std::nullopt;
		}
		return json_parser{bytes}.parse(error);
	}

	enum class resolution_state
	{
		proved,
		disproved,
		unknown,
		partial,
		conflicting,
	};

	[[nodiscard]] std::string_view state_name(const resolution_state state) noexcept
	{
		switch (state)
		{
			case resolution_state::proved:
				return "proved";
			case resolution_state::disproved:
				return "disproved";
			case resolution_state::unknown:
				return "unknown";
			case resolution_state::partial:
				return "partial";
			case resolution_state::conflicting:
				return "conflicting";
		}
		return "unknown";
	}

	[[nodiscard]] resolution_state parse_state(const std::string_view value) noexcept
	{
		if (value == "proved" || value == "implemented" || value == "available" ||
			value == "satisfied" || value == "complete")
			return resolution_state::proved;
		if (value == "disproved" || value == "rejected" || value == "unsupported")
			return resolution_state::disproved;
		if (value == "partial")
			return resolution_state::partial;
		if (value == "conflicting" || value == "conflict")
			return resolution_state::conflicting;
		return resolution_state::unknown;
	}

	struct capability_node
	{
		std::string id;
		std::string kind;
		std::vector<std::string> dependencies;
		resolution_state declared_state{resolution_state::unknown};
		resolution_state effective_state{resolution_state::unknown};
		std::string reason_code;
		std::string reason_detail;
		std::string owner_issue;
		std::string action;
		std::string upgrade;
	};

	struct resolution_document
	{
		std::string use_case;
		std::string source_path;
		std::string source_schema;
		std::vector<capability_node> nodes;
		json_value::object preserved;
	};

	[[nodiscard]] std::string node_id(const json_value::object& object)
	{
		for (const auto key : {"id", "capability", "capability_id", "name"})
		{
			const auto value = text_member(object, key);
			if (!value.empty())
				return value;
		}
		return {};
	}

	[[nodiscard]] std::vector<std::string> dependency_values(const json_value::object& object)
	{
		for (const auto key : {"requires", "dependencies", "depends_on"})
		{
			const auto* value = member(object, key);
			const auto* array = value == nullptr ? nullptr : value->as_array();
			if (array == nullptr)
				continue;
			std::vector<std::string> output;
			for (const auto& child : *array)
			{
				if (const auto* text = child.as_string())
					output.push_back(*text);
				else if (const auto* child_object = child.as_object())
				{
					const auto id = node_id(*child_object);
					if (!id.empty())
						output.push_back(id);
				}
			}
			return output;
		}
		return {};
	}

	[[nodiscard]] std::optional<capability_node> parse_node(const json_value& value)
	{
		capability_node node;
		if (const auto* text = value.as_string())
		{
			node.id = *text;
			node.kind = "capability";
			node.reason_code = "capability-unresolved";
			return node;
		}
		const auto* object = value.as_object();
		if (object == nullptr)
			return std::nullopt;
		node.id = node_id(*object);
		if (node.id.empty())
			return std::nullopt;
		node.kind = text_member(*object, "kind", "capability");
		node.dependencies = dependency_values(*object);
		const auto state = text_member(
			*object,
			"state",
			text_member(*object,
						"result_state",
						text_member(*object, "disposition", text_member(*object, "status"))));
		node.declared_state = parse_state(state);
		node.reason_code =
			text_member(*object, "reason_code", text_member(*object, "missing_reason_code"));
		if (node.reason_code.empty())
		{
			if (const auto* reason = member(*object, "reason"); reason != nullptr)
			{
				if (const auto* reason_text = reason->as_string())
					node.reason_detail = *reason_text;
				else if (const auto* reason_object = reason->as_object())
					node.reason_code = text_member(*reason_object, "code");
			}
			if (node.reason_code.empty())
			{
				if (const auto* reason = member(*object, "missing_reason"); reason != nullptr)
				{
					if (const auto* reason_text = reason->as_string())
						node.reason_code = *reason_text;
					else if (const auto* reason_object = reason->as_object())
						node.reason_code = text_member(*reason_object, "code");
				}
			}
		}
		node.reason_detail = node.reason_detail.empty()
			? text_member(*object, "reason_detail", text_member(*object, "detail"))
			: node.reason_detail;
		node.owner_issue = text_member(*object, "owner_issue", text_member(*object, "owner"));
		node.action = text_member(*object, "action", text_member(*object, "completion_action"));
		node.upgrade =
			text_member(*object, "upgrade", text_member(*object, "reevaluation_trigger"));
		return node;
	}

	[[nodiscard]] std::vector<const json_value*> path_entries(const json_value::object& object)
	{
		for (const auto key :
			 {"capability_path", "capabilities", "required_capabilities", "requirements", "nodes"})
		{
			const auto* value = member(object, key);
			if (value == nullptr)
				continue;
			if (const auto* array = value->as_array())
			{
				std::vector<const json_value*> output;
				output.reserve(array->size());
				for (const auto& child : *array)
					output.push_back(&child);
				return output;
			}
			if (const auto* children = value->as_object())
			{
				std::vector<const json_value*> output;
				output.reserve(children->size());
				for (const auto& [id, child] : *children)
				{
					(void)id;
					output.push_back(&child);
				}
				return output;
			}
		}
		return {};
	}

	[[nodiscard]] bool parse_resolution_document(const json_value& root,
												 const std::string_view requested_use_case,
												 resolution_document& output,
												 std::string& error)
	{
		const auto* root_object = root.as_object();
		if (root_object == nullptr)
		{
			error = "capability-input-object-required";
			return false;
		}
		output.source_schema = text_member(*root_object, "schema", "unknown");
		output.use_case =
			text_member(*root_object, "use_case", text_member(*root_object, "use_case_id"));
		const json_value::object* selected = root_object;
		if (const auto* use_cases = member(*root_object, "use_cases"); use_cases != nullptr)
		{
			const auto* array = use_cases->as_array();
			if (array == nullptr)
			{
				error = "capability-use-cases-array-required";
				return false;
			}
			const json_value::object* first = nullptr;
			bool matched = requested_use_case.empty();
			for (const auto& candidate : *array)
			{
				const auto* candidate_object = candidate.as_object();
				if (candidate_object == nullptr)
					continue;
				if (first == nullptr)
					first = candidate_object;
				const auto candidate_id = text_member(
					*candidate_object, "id", text_member(*candidate_object, "use_case_id"));
				if (!requested_use_case.empty() && candidate_id == requested_use_case)
				{
					selected = candidate_object;
					output.use_case = candidate_id;
					matched = true;
					break;
				}
			}
			if (!matched)
			{
				error = "capability-use-case-not-found";
				return false;
			}
			if (selected == root_object && first != nullptr)
			{
				selected = first;
				if (output.use_case.empty())
					output.use_case = text_member(*first, "id", text_member(*first, "use_case_id"));
			}
		}
		if (!requested_use_case.empty())
			output.use_case = requested_use_case;
		if (output.use_case.empty())
		{
			error = "capability-use-case-missing";
			return false;
		}
		for (const auto& key : {"coverage",
								"closure",
								"unresolved",
								"conflict",
								"differential_disagreement",
								"guarantee",
								"provenance",
								"evidence",
								"logical_explain",
								"physical_explain"})
			if (const auto* value = member(*selected, key); value != nullptr)
				output.preserved.emplace(key, *value);
		const auto entries = path_entries(*selected);
		for (const auto* entry : entries)
		{
			auto node = parse_node(*entry);
			if (!node)
			{
				error = "capability-entry-invalid";
				return false;
			}
			if (std::ranges::any_of(output.nodes,
									[&](const capability_node& existing)
									{
										return existing.id == node->id;
									}))
			{
				error = "capability-entry-duplicate";
				return false;
			}
			output.nodes.push_back(std::move(*node));
		}
		if (output.nodes.empty())
		{
			capability_node missing;
			missing.id = "input.project";
			missing.kind = "input";
			missing.declared_state = resolution_state::unknown;
			missing.reason_code = "input.project-missing";
			missing.reason_detail = "an explicit project capability projection is required";
			output.nodes.push_back(std::move(missing));
		}
		return true;
	}

	[[nodiscard]] bool topological_order(std::vector<capability_node>& nodes,
										 std::vector<std::size_t>& order,
										 std::string& error)
	{
		std::map<std::string, std::size_t, std::less<>> index;
		for (std::size_t position = 0U; position < nodes.size(); ++position)
			index.emplace(nodes[position].id, position);
		for (std::size_t position = 0U; position < nodes.size(); ++position)
		{
			for (const auto& dependency : nodes[position].dependencies)
				if (!index.contains(dependency))
				{
					capability_node missing;
					missing.id = dependency;
					missing.kind = "capability";
					missing.reason_code = "capability-missing";
					nodes.push_back(std::move(missing));
					index.emplace(dependency, nodes.size() - 1U);
				}
		}
		std::map<std::string, unsigned, std::less<>> colour;
		std::function<bool(std::size_t)> visit = [&](const std::size_t position)
		{
			const auto& id = nodes[position].id;
			if (colour[id] == 1U)
			{
				error = "agent.capability-cycle";
				return false;
			}
			if (colour[id] == 2U)
				return true;
			colour[id] = 1U;
			std::vector<std::string> dependencies = nodes[position].dependencies;
			std::ranges::sort(dependencies);
			for (const auto& dependency : dependencies)
				if (!visit(index.at(dependency)))
					return false;
			colour[id] = 2U;
			order.push_back(position);
			return true;
		};
		std::vector<std::size_t> positions(nodes.size());
		std::ranges::iota(positions, 0U);
		std::ranges::sort(positions,
						  [&](const std::size_t left, const std::size_t right)
						  {
							  return nodes[left].id < nodes[right].id;
						  });
		for (const auto position : positions)
			if (!visit(position))
				return false;
		return true;
	}

	[[nodiscard]] resolution_state
	effective_state(const capability_node& node,
					const std::map<std::string, capability_node, std::less<>>& by_id)
	{
		if (node.declared_state == resolution_state::conflicting ||
			node.declared_state == resolution_state::disproved)
			return node.declared_state;
		bool dependency_unresolved = false;
		for (const auto& dependency : node.dependencies)
		{
			const auto found = by_id.find(dependency);
			if (found == by_id.end())
			{
				dependency_unresolved = true;
				continue;
			}
			const auto dependency_state = found->second.effective_state;
			if (dependency_state == resolution_state::conflicting)
				return resolution_state::conflicting;
			if (dependency_state == resolution_state::disproved)
				return resolution_state::disproved;
			if (dependency_state != resolution_state::proved)
				dependency_unresolved = true;
		}
		if (dependency_unresolved)
			return node.declared_state == resolution_state::partial ? resolution_state::partial
																	: resolution_state::unknown;
		return node.declared_state;
	}

	[[nodiscard]] resolution_state aggregate_state(const std::vector<capability_node>& nodes)
	{
		if (nodes.empty())
			return resolution_state::unknown;
		bool any_proved = false;
		bool any_unresolved = false;
		bool any_disproved = false;
		bool any_conflicting = false;
		for (const auto& node : nodes)
		{
			any_proved |= node.effective_state == resolution_state::proved;
			any_unresolved |= node.effective_state == resolution_state::unknown ||
				node.effective_state == resolution_state::partial;
			any_disproved |= node.effective_state == resolution_state::disproved;
			any_conflicting |= node.effective_state == resolution_state::conflicting;
		}
		if (any_conflicting)
			return resolution_state::conflicting;
		if (any_unresolved && any_proved)
			return resolution_state::partial;
		if (any_unresolved)
			return resolution_state::unknown;
		if (any_disproved)
			return resolution_state::disproved;
		return resolution_state::proved;
	}

	struct resolved_document
	{
		resolution_document source;
		std::vector<std::size_t> order;
		resolution_state state{resolution_state::unknown};
		std::string error;
	};

	[[nodiscard]] std::optional<resolved_document> resolve(resolution_document document)
	{
		resolved_document output{std::move(document), {}, resolution_state::unknown, {}};
		if (!topological_order(output.source.nodes, output.order, output.error))
			return std::nullopt;
		std::map<std::string, capability_node, std::less<>> by_id;
		for (const auto position : output.order)
			by_id.emplace(output.source.nodes[position].id, output.source.nodes[position]);
		for (const auto position : output.order)
		{
			auto& node = output.source.nodes[position];
			const auto found = by_id.find(node.id);
			if (found != by_id.end())
				node.effective_state = effective_state(node, by_id);
			by_id[node.id].effective_state = node.effective_state;
			if (node.effective_state == resolution_state::unknown && node.reason_code.empty())
				node.reason_code = "capability-unresolved";
			if (node.effective_state == resolution_state::partial && node.reason_code.empty())
				node.reason_code = "coverage-incomplete";
			if (node.effective_state == resolution_state::disproved && node.reason_code.empty())
				node.reason_code = "capability-disproved";
			if (node.effective_state == resolution_state::conflicting && node.reason_code.empty())
				node.reason_code = "capability-conflict";
		}
		output.state = aggregate_state(output.source.nodes);
		return output;
	}

	[[nodiscard]] json_value json_string(const std::string_view text)
	{
		return json_value{std::string{text}};
	}

	[[nodiscard]] json_value json_string_array(const std::vector<std::string>& values)
	{
		json_value::array output;
		for (const auto& value : values)
			output.emplace_back(json_string(value));
		return json_value{std::move(output)};
	}

	[[nodiscard]] json_value node_json(const capability_node& node)
	{
		json_value::object output;
		output.emplace("id", json_string(node.id));
		output.emplace("kind", json_string(node.kind));
		output.emplace("state", json_string(state_name(node.effective_state)));
		output.emplace("requires", json_string_array(node.dependencies));
		if (!node.reason_code.empty())
			output.emplace("reason_code", json_string(node.reason_code));
		if (!node.reason_detail.empty())
			output.emplace("reason_detail", json_string(node.reason_detail));
		if (!node.owner_issue.empty())
			output.emplace("owner_issue", json_string(node.owner_issue));
		if (!node.action.empty())
			output.emplace("action", json_string(node.action));
		if (!node.upgrade.empty())
			output.emplace("upgrade", json_string(node.upgrade));
		return json_value{std::move(output)};
	}

	[[nodiscard]] json_value resolved_json(const resolved_document& resolved,
										   const std::string_view mode)
	{
		const auto& source = resolved.source;
		json_value::array capabilities;
		json_value::array missing;
		json_value::array plan;
		std::size_t proved{};
		for (const auto position : resolved.order)
		{
			const auto& node = source.nodes[position];
			capabilities.emplace_back(node_json(node));
			if (node.effective_state == resolution_state::proved)
				++proved;
			else
			{
				missing.emplace_back(node_json(node));
				json_value::object action;
				action.emplace("capability", json_string(node.id));
				action.emplace(
					"action",
					json_string(node.action.empty() ? "supply-capability-evidence" : node.action));
				action.emplace("reason_code",
							   json_string(node.reason_code.empty() ? "capability-unresolved"
																	: node.reason_code));
				action.emplace("depends_on", json_string_array(node.dependencies));
				if (!node.owner_issue.empty())
					action.emplace("owner_issue", json_string(node.owner_issue));
				if (!node.upgrade.empty())
					action.emplace("reevaluation_trigger", json_string(node.upgrade));
				plan.emplace_back(json_value{std::move(action)});
			}
		}
		json_value::object coverage;
		coverage.emplace("required", json_value{static_cast<std::uint64_t>(source.nodes.size())});
		coverage.emplace("proved", json_value{static_cast<std::uint64_t>(proved)});
		coverage.emplace("unresolved",
						 json_value{static_cast<std::uint64_t>(source.nodes.size() - proved)});
		json_value::object result;
		result.emplace("state", json_string(state_name(resolved.state)));
		result.emplace("result_state", json_string(state_name(resolved.state)));
		result.emplace("capabilities", json_value{std::move(capabilities)});
		result.emplace("missing", json_value{std::move(missing)});
		result.emplace("coverage", json_value{std::move(coverage)});
		result.emplace("completion_plan", json_value{std::move(plan)});
		for (const auto& [key, value] : source.preserved)
			result.emplace(key, value);
		json_value::object provenance;
		provenance.emplace(
			"source",
			json_string(source.source_path.empty() ? "stdin-or-inline" : source.source_path));
		provenance.emplace("source_schema", json_string(source.source_schema));
		provenance.emplace("engine", json_string("cxxlens.agent-capability-resolution.v1"));
		result.emplace("provenance", json_value{std::move(provenance)});
		json_value::object output;
		output.emplace("schema", json_string("cxxlens.agent-capability-resolution.v1"));
		output.emplace("document_version", json_string("1.0.0"));
		output.emplace("role", json_string("sdk-doctor-capability-resolution"));
		output.emplace("mode", json_string(mode));
		output.emplace("use_case", json_string(source.use_case));
		output.emplace("state", json_string(state_name(resolved.state)));
		output.emplace("result_state", json_string(state_name(resolved.state)));
		output.emplace("result", json_value{std::move(result)});
		return json_value{std::move(output)};
	}

	[[nodiscard]] std::string markdown_report(const resolved_document& resolved,
											  const std::string_view mode)
	{
		const auto& source = resolved.source;
		std::string output = "# cxxlens SDK doctor\n\n";
		output += "- mode: `" + std::string{mode} + "`\n";
		output += "- use case: `" + source.use_case + "`\n";
		output += "- state: `" + std::string{state_name(resolved.state)} + "`\n\n";
		output += "## Capability resolution\n\n| Capability | State | Reason |\n|---|---|---|\n";
		for (const auto position : resolved.order)
		{
			const auto& node = source.nodes[position];
			output += "| `" + node.id + "` | `" + std::string{state_name(node.effective_state)} +
				"` | `" + (node.reason_code.empty() ? std::string{""} : node.reason_code) + "` |\n";
		}
		output += "\n## Dependency-ordered completion plan\n\n";
		std::size_t step{};
		for (const auto position : resolved.order)
		{
			const auto& node = source.nodes[position];
			if (node.effective_state == resolution_state::proved)
				continue;
			output += std::to_string(++step) + ". Resolve `" + node.id + "` (`" +
				(node.action.empty() ? std::string{"supply-capability-evidence"} : node.action) +
				"`)";
			if (!node.owner_issue.empty())
				output += "; owner " + node.owner_issue;
			output += ".\n";
		}
		if (step == 0U)
			output += "Complete: no missing capability evidence.\n";
		return output;
	}

	/** @brief One requested relation's presence outcome against `known_relation_registry()`. */
	struct component_check
	{
		std::string_view id;
		bool present{};
		std::string reason_code;
	};

	/**
	 * @brief Report, for each requested relation ID, whether this SDK build's registry has it.
	 *
	 * `relation_ids` names the relations a consumer configuration (a provider or query author)
	 * depends on, e.g. a manifest's `offered_relations`/`required_relations` entries. Absence is
	 * derived from the real `cxxlens::sdk::relation_registry::require` outcome, reusing its exact
	 * `sdk.relation-not-found` / `sdk.relation-major-mismatch` reason codes rather than inventing
	 * new ones.
	 */
	int run_relation_presence(const std::span<char*> relation_ids)
	{
		if (relation_ids.empty())
		{
			std::cerr << "usage: cxxlens-sdk-doctor missing <relation-id> [<relation-id> ...]\n";
			return 2;
		}
		auto registry = known_relation_registry();
		if (!registry)
		{
			std::cerr << "sdk.doctor-contract-invalid\n";
			return 1;
		}
		std::vector<std::pair<std::string_view, std::uint32_t>> parsed;
		parsed.reserve(relation_ids.size());
		for (const auto* argument : relation_ids)
		{
			const auto split = split_relation_id(std::string_view{argument});
			if (!split)
			{
				std::cerr << "sdk.relation-id-malformed: " << argument << '\n';
				return 2;
			}
			parsed.push_back(*split);
		}
		std::vector<component_check> checks;
		checks.reserve(relation_ids.size());
		std::size_t missing_count{};
		for (std::size_t index = 0U; index < relation_ids.size(); ++index)
		{
			const auto [name, major] = parsed[index];
			auto found = registry->require(name, major);
			if (found)
			{
				checks.push_back({std::string_view{relation_ids[index]}, true, {}});
				continue;
			}
			++missing_count;
			checks.push_back({std::string_view{relation_ids[index]}, false, found.error().code});
		}
		std::cout << R"({"schema":"cxxlens.sdk-doctor-missing.v1","mode":"missing","requested":)"
				  << checks.size() << R"(,"missing":)" << missing_count << R"(,"status":")"
				  << (missing_count == 0U ? "complete" : "incomplete") << R"(","components":[)";
		for (std::size_t index = 0U; index < checks.size(); ++index)
		{
			if (index != 0U)
				std::cout << ',';
			std::cout << R"({"id":")" << checks[index].id << R"(","status":")"
					  << (checks[index].present ? "present" : "missing") << '"';
			if (!checks[index].present)
				std::cout << R"(,"reason_code":")" << checks[index].reason_code << '"';
			std::cout << '}';
		}
		std::cout << "]}\n";
		return missing_count == 0U ? 0 : 1;
	}

	struct doctor_options
	{
		std::string project;
		std::string use_case;
		std::string format{"json"};
	};

	[[nodiscard]] bool parse_doctor_options(
		const int argc, char** argv, const int start, doctor_options& output, std::string& error)
	{
		for (int index = start; index < argc; ++index)
		{
			const std::string_view argument{argv[index]};
			if (argument == "--project" || argument == "--use-case")
			{
				if (index + 1 >= argc)
				{
					error = "doctor-option-value-missing";
					return false;
				}
				if (argument == "--project")
					output.project = argv[++index];
				else
					output.use_case = argv[++index];
				continue;
			}
			if (argument == "--format")
			{
				if (index + 1 >= argc)
				{
					error = "doctor-format-value-missing";
					return false;
				}
				output.format = argv[++index];
				if (output.format != "json" && output.format != "markdown")
				{
					error = "doctor-format-invalid";
					return false;
				}
				continue;
			}
			error = "doctor-option-invalid";
			return false;
		}
		return true;
	}

	[[nodiscard]] std::optional<resolved_document>
	load_and_resolve(const doctor_options& options,
					 const std::string_view requested_use_case,
					 std::string& error)
	{
		resolution_document document;
		document.source_path = options.project;
		if (options.project.empty())
		{
			document.use_case = requested_use_case;
			document.source_schema = "missing-input";
			capability_node missing;
			missing.id = "input.project";
			missing.kind = "input";
			missing.reason_code = "input.project-missing";
			missing.reason_detail = "--project is required for an installed capability diagnosis";
			document.nodes.push_back(std::move(missing));
			auto resolved = resolve(std::move(document));
			if (!resolved && error.empty())
				error = "agent.capability-cycle";
			return resolved;
		}
		auto root = read_json(options.project, error);
		if (!root)
			return std::nullopt;
		if (!parse_resolution_document(*root, requested_use_case, document, error))
			return std::nullopt;
		auto resolved = resolve(std::move(document));
		if (!resolved && error.empty())
			error = "agent.capability-cycle";
		return resolved;
	}

	int print_resolved(const resolved_document& resolved,
					   const std::string_view mode,
					   const std::string_view format)
	{
		if (format == "markdown")
			std::cout << markdown_report(resolved, mode);
		else
			std::cout << json_dump(resolved_json(resolved, mode)) << '\n';
		return 0;
	}

	int run_capability(const int argc, char** argv)
	{
		if (argc < 3)
		{
			std::cerr << "usage: cxxlens-sdk-doctor capability <use-case-id> [--project "
						 "<project.json>] [--format json|markdown]\n";
			return 2;
		}
		doctor_options options;
		std::string error;
		if (!parse_doctor_options(argc, argv, 3, options, error))
		{
			std::cerr << "sdk.capability-option-invalid: " << error << '\n';
			return 2;
		}
		auto resolved = load_and_resolve(options, argv[2], error);
		if (!resolved)
		{
			std::cerr << "sdk.capability-resolution-failed: " << error << '\n';
			return 1;
		}
		return print_resolved(*resolved, "capability", options.format);
	}

	int run_project_missing(const int argc, char** argv)
	{
		doctor_options options;
		std::string error;
		if (!parse_doctor_options(argc, argv, 2, options, error) || options.project.empty())
		{
			std::cerr << "usage: cxxlens-sdk-doctor missing --project <project.json> [--use-case "
						 "<id>] [--format json|markdown]\n";
			return 2;
		}
		auto resolved = load_and_resolve(options, options.use_case, error);
		if (!resolved)
		{
			std::cerr << "sdk.capability-resolution-failed: " << error << '\n';
			return 1;
		}
		return print_resolved(*resolved, "missing", options.format);
	}

	int run_explain(const int argc, char** argv)
	{
		if (argc < 3)
		{
			std::cerr
				<< "usage: cxxlens-sdk-doctor explain <report.json> [--format json|markdown]\n";
			return 2;
		}
		doctor_options options;
		std::string error;
		if (!parse_doctor_options(argc, argv, 3, options, error))
		{
			std::cerr << "sdk.explain-option-invalid: " << error << '\n';
			return 2;
		}
		auto root = read_json(argv[2], error);
		if (!root)
		{
			std::cerr << "sdk.explain-input-invalid: " << error << '\n';
			return 1;
		}
		resolution_document document;
		if (!parse_resolution_document(*root, {}, document, error))
		{
			std::cerr << "sdk.explain-input-invalid: " << error << '\n';
			return 1;
		}
		if (const auto* root_object = root->as_object())
			if (const auto* result = member(*root_object, "result"); result != nullptr)
			{
				resolution_document result_document;
				if (parse_resolution_document(*result, document.use_case, result_document, error) &&
					!result_document.nodes.empty())
				{
					result_document.source_path = argv[2];
					result_document.source_schema = document.source_schema;
					document = std::move(result_document);
				}
			}
		document.source_path = argv[2];
		auto resolved = resolve(std::move(document));
		if (!resolved)
		{
			if (error.empty())
				error = "agent.capability-cycle";
			std::cerr << "sdk.explain-input-invalid: " << error << '\n';
			return 1;
		}
		return print_resolved(*resolved, "explain", options.format);
	}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cerr << "usage: cxxlens-sdk-doctor "
					 "inspect|doctor|query-ir|provider-manifest|capability|explain|missing\n";
		return 2;
	}
	const std::string_view mode{argv[1]};
	if (mode == "capability")
		return run_capability(argc, argv);
	if (mode == "explain")
		return run_explain(argc, argv);
	if (mode == "missing")
	{
		if (argc >= 3 && std::string_view{argv[2]} == "--project")
			return run_project_missing(argc, argv);
		return run_relation_presence(
			std::span<char*>{argv + 2, static_cast<std::size_t>(argc - 2)});
	}
	if (argc != 2 ||
		(mode != "inspect" && mode != "doctor" && mode != "query-ir" &&
		 mode != "provider-manifest"))
	{
		std::cerr << "usage: cxxlens-sdk-doctor "
					 "inspect|doctor|query-ir|provider-manifest|capability|explain|missing\n";
		return 2;
	}
	using relation = cxxlens::cc::relations::call_site;
	auto typed = cxxlens::sdk::query::from<relation>();
	cxxlens::sdk::relation_registry registry;
	auto added = registry.add(relation::descriptor());
	auto dynamic = registry.require("cc.call_site", 1U);
	if (!typed || !added || !dynamic)
	{
		std::cerr << "sdk.doctor-contract-invalid\n";
		return 1;
	}
	auto dynamic_query = cxxlens::sdk::query::dynamic_query::from(*dynamic);
	if (!dynamic_query || typed->ir().digest() != dynamic_query->ir().digest())
	{
		std::cerr << "sdk.static-dynamic-ir-mismatch\n";
		return 1;
	}
	if (mode == "query-ir")
	{
		auto predicate =
			cxxlens::sdk::query::equals_present(cxxlens::sdk::query::col<relation::ordinal>(),
												cxxlens::sdk::query::literal::unsigned_integer(0U));
		if (!predicate)
			return 1;
		auto filtered = std::move(*typed).where(std::move(*predicate));
		if (!filtered)
			return 1;
		const std::array keys{cxxlens::sdk::query::col<relation::call>()};
		auto ordered = std::move(*filtered).order_by(keys);
		if (!ordered)
			return 1;
		const std::array output{cxxlens::sdk::query::col<relation::call>(),
								cxxlens::sdk::query::col<relation::source>()};
		auto projected = std::move(*ordered).project(output);
		if (!projected || !projected->ir().validate())
			return 1;
		std::cout << projected->ir().canonical_form() << '\n';
		return 0;
	}
	if (mode == "provider-manifest")
	{
		const auto zero_digest = "sha256:" + std::string(64U, '0');
		cxxlens::sdk::provider::manifest manifest;
		manifest.provider_id = "company.example.doctor";
		manifest.provider_version = {1U, 0U, 0U};
		manifest.package_identity = "company.example.doctor-package";
		manifest.publisher = "company.example";
		manifest.license = "Apache-2.0";
		manifest.platform_tuples = {"linux-x86_64"};
		manifest.provider_binary_digest = zero_digest;
		manifest.provider_semantic_contract_digest = zero_digest;
		manifest.offered_relations = {"cc.call_site.v1"};
		manifest.interpretation_domains = {"cc.canonical-1"};
		manifest.invalidation_contract = zero_digest;
		manifest.determinism_contract = zero_digest;
		manifest.resource_class = "provider.standard";
		manifest.requested_qualifications = {"schema-conformant"};
		if (!manifest.validate())
			return 1;
		std::cout << manifest.canonical_json() << '\n';
		return 0;
	}
	std::cout << "{\"descriptor\":\"" << relation::descriptor().descriptor_digest
			  << "\",\"mode\":\"" << mode << "\",\"ordinary_llvm_dependency\":false,"
			  << "\"query_ir\":\"" << typed->ir().digest() << "\",\"status\":\"accepted\"}\n";
	return 0;
}
