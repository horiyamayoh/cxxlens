#pragma once

// Product-only capability diagnosis used by cxxlens-sdk-doctor.
//
// It consumes product values supplied by a project document and descriptors compiled into the
// SDK. The implementation is header-only so the installed tool can use the existing target.

#include <algorithm>
#include <array>
#include <charconv>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
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
	inline constexpr std::size_t maximum_json_string_bytes = 512U;
	inline constexpr std::size_t maximum_json_collection_count = 128U;
	inline constexpr std::size_t maximum_capability_count = 128U;
	inline constexpr std::size_t maximum_project_node_count = 4096U;

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
			if (position_ - begin > 1U && input_[begin] == '0')
				return failure("doctor.project-invalid", "json", "leading-zero-number");
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
				if (output.size() >= maximum_json_string_bytes)
					return failure("doctor.project-invalid", "json", "string-byte-limit");
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
						const std::size_t encoded_bytes = value <= 0x7fU ? 1U
							: value <= 0x7ffU							 ? 2U
							: value <= 0xffffU							 ? 3U
																		 : 4U;
						if (output.size() > maximum_json_string_bytes - encoded_bytes)
							return failure("doctor.project-invalid", "json", "string-byte-limit");
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
				if (values.size() >= maximum_json_collection_count)
					return failure("doctor.project-invalid", "json", "array-count-limit");
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
				if (values.size() >= maximum_json_collection_count)
					return failure("doctor.project-invalid", "json", "object-count-limit");
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

	enum class resolution_state : std::uint8_t
	{
		proved,
		disproved,
		unknown,
		partial,
		conflicting,
	};

	enum class diagnosis_reason : std::uint8_t
	{
		none,
		missing_input,
		missing_provider,
		missing_capability,
		unsupported_tuple,
		disproved_dependency,
		unknown_dependency,
		conflicting_capability,
		source_closure_unavailable,
	};

	[[nodiscard]] inline std::string_view state_name(const resolution_state state) noexcept
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

	[[nodiscard]] inline std::string_view reason_name(const diagnosis_reason reason) noexcept
	{
		switch (reason)
		{
			case diagnosis_reason::none:
				return "doctor.none";
			case diagnosis_reason::missing_input:
				return "doctor.missing-input";
			case diagnosis_reason::missing_provider:
				return "doctor.missing-provider";
			case diagnosis_reason::missing_capability:
				return "doctor.missing-capability";
			case diagnosis_reason::unsupported_tuple:
				return "doctor.unsupported-tuple";
			case diagnosis_reason::disproved_dependency:
				return "doctor.disproved-dependency";
			case diagnosis_reason::unknown_dependency:
				return "doctor.unknown-dependency";
			case diagnosis_reason::conflicting_capability:
				return "doctor.conflicting-capability";
			case diagnosis_reason::source_closure_unavailable:
				return "doctor.source-closure-unavailable";
		}
		return "doctor.missing-capability";
	}

	struct support_tuple
	{
		std::string release_version;
		std::string surface;
		std::string os;
		std::string architecture;
		std::string compiler_provider_major;
		std::string linkage;

		auto operator<=>(const support_tuple&) const = default;
	};

	struct provider_candidate
	{
		std::string candidate_id;
		std::string provider_id;
		std::string provider_version;
		std::string package_identity;
		std::string provider_manifest_digest;
		std::string provider_binary_digest;
		std::string provider_semantic_contract_digest;
		std::uint32_t protocol_major{};
		std::uint32_t protocol_minor{};
		std::vector<std::string> features;
		std::vector<std::string> relations;
		std::vector<std::string> interpretations;
		std::string sandbox_minimum;
		std::string sandbox_policy_digest;
	};

	struct project_context
	{
		std::string project_id;
		std::string catalog_id;
		std::string catalog_digest;
		std::string logical_root;
		std::string environment_digest;
		support_tuple environment;
		bool source_input{};
		std::string source_snapshot_id;
		std::string compilation_database_id;
		bool store_input{};
		std::vector<provider_candidate> provider_candidates;
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
		if (value.empty() || value.size() > 512U || !valid_utf8(value))
			return false;
		for (const auto byte : value)
			if (static_cast<unsigned char>(byte) < 0x20U || byte == '\x7f')
				return false;
		return true;
	}

	[[nodiscard]] inline bool lowercase_hex(const std::string_view value) noexcept
	{
		return std::ranges::all_of(value,
								   [](const char byte)
								   {
									   return (byte >= '0' && byte <= '9') ||
										   (byte >= 'a' && byte <= 'f');
								   });
	}

	[[nodiscard]] inline bool digest_value(const std::string_view value) noexcept
	{
		return value.size() == 71U && value.starts_with("sha256:") &&
			lowercase_hex(value.substr(7U));
	}

	[[nodiscard]] inline bool semantic_digest_value(const std::string_view value) noexcept
	{
		return value.size() == 83U && value.starts_with("semantic-v2:sha256:") &&
			lowercase_hex(value.substr(19U));
	}

	[[nodiscard]] inline bool semantic_version_value(const std::string_view value) noexcept
	{
		std::size_t begin{};
		for (std::size_t component = 0U; component < 3U; ++component)
		{
			const auto end = component == 2U ? value.size() : value.find('.', begin);
			if (end == std::string_view::npos || end == begin)
				return false;
			const auto digits = value.substr(begin, end - begin);
			if ((component == 0U && digits == "0") ||
				(digits.size() > 1U && digits.front() == '0') ||
				!std::ranges::all_of(digits,
									 [](const char byte)
									 {
										 return byte >= '0' && byte <= '9';
									 }))
				return false;
			begin = end + 1U;
		}
		return begin == value.size() + 1U;
	}

	[[nodiscard]] inline bool relation_name_value(const std::string_view value) noexcept
	{
		if (value.empty() || value.front() == '.' || value.back() == '.' ||
			value.find("..") != std::string_view::npos || value.find('.') == std::string_view::npos)
			return false;
		return std::ranges::all_of(value,
								   [](const char byte)
								   {
									   return (byte >= 'a' && byte <= 'z') ||
										   (byte >= '0' && byte <= '9') || byte == '.' ||
										   byte == '_';
								   });
	}

	[[nodiscard]] inline std::optional<std::string>
	provider_relation_offer_to_descriptor(const std::string_view offer)
	{
		const auto separator = offer.rfind('@');
		if (separator == std::string_view::npos || separator == 0U ||
			separator + 1U == offer.size())
			return std::nullopt;
		const auto name = offer.substr(0U, separator);
		const auto major_text = offer.substr(separator + 1U);
		if (!relation_name_value(name) || (major_text.size() > 1U && major_text.front() == '0') ||
			!std::ranges::all_of(major_text,
								 [](const char byte)
								 {
									 return byte >= '0' && byte <= '9';
								 }))
			return std::nullopt;
		std::uint32_t major{};
		const auto parsed =
			std::from_chars(major_text.data(), major_text.data() + major_text.size(), major);
		if (parsed.ec != std::errc{} || parsed.ptr != major_text.data() + major_text.size())
			return std::nullopt;
		return std::string{name} + ".v" + std::to_string(major);
	}

	[[nodiscard]] inline std::optional<std::string>
	descriptor_relation_to_provider_offer(const std::string_view descriptor)
	{
		const auto marker = descriptor.rfind(".v");
		if (marker == std::string_view::npos || marker == 0U || marker + 2U == descriptor.size())
			return std::nullopt;
		const auto name = descriptor.substr(0U, marker);
		const auto major_text = descriptor.substr(marker + 2U);
		if (!relation_name_value(name) || (major_text.size() > 1U && major_text.front() == '0') ||
			!std::ranges::all_of(major_text,
								 [](const char byte)
								 {
									 return byte >= '0' && byte <= '9';
								 }))
			return std::nullopt;
		std::uint32_t major{};
		const auto parsed =
			std::from_chars(major_text.data(), major_text.data() + major_text.size(), major);
		if (parsed.ec != std::errc{} || parsed.ptr != major_text.data() + major_text.size())
			return std::nullopt;
		return std::string{name} + "@" + std::to_string(major);
	}

	[[nodiscard]] inline std::string_view
	provider_relation_namespace_name(const std::string_view offer) noexcept
	{
		const auto separator = offer.rfind('@');
		return separator == std::string_view::npos ? offer : offer.substr(0U, separator);
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
		if (value->array.size() > maximum_json_collection_count)
		{
			error = "doctor.project-invalid:" + std::string{name} + ":count-limit";
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
		std::ranges::sort(output);
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
		const auto* schema_value = required_member(root, "schema", "schema", error);
		if (schema_value == nullptr || schema_value->kind != json_kind::string ||
			schema_value->string != "cxxlens.sdk-doctor-project.v2")
			return product_error{
				"doctor.project-invalid", "schema", "expected-cxxlens-sdk-doctor-project-v2"};
		const auto* version_value =
			required_member(root, "document_version", "document_version", error);
		if (version_value == nullptr || version_value->kind != json_kind::string ||
			version_value->string != "2.0.0")
			return product_error{"doctor.project-invalid", "document_version", "expected-2.0.0"};
		const auto* project = required_member(root, "project", "project", error);
		if (project == nullptr)
			return product_error{"doctor.project-invalid", "project", std::move(error)};
		static constexpr std::array<std::string_view, 9U> project_fields{"project_id",
																		 "catalog_id",
																		 "catalog_digest",
																		 "logical_root",
																		 "environment_digest",
																		 "environment",
																		 "source_input",
																		 "provider_candidates",
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
		constexpr std::string_view logical_root_prefix{"project://"};
		if (!output.logical_root.starts_with(logical_root_prefix) ||
			output.logical_root.size() == logical_root_prefix.size())
			return product_error{"doctor.project-invalid", "logical_root", "project-uri-required"};
		if (!semantic_digest_value(output.catalog_digest) ||
			output.catalog_id != "catalog:" + output.catalog_digest)
			return product_error{
				"doctor.project-invalid", "catalog_id", "catalog-id-digest-mismatch"};
		if (!digest_value(output.environment_digest))
			return product_error{"doctor.project-invalid", "environment_digest", "sha256-required"};

		const auto* environment = required_member(*project, "environment", "environment", error);
		static constexpr std::array<std::string_view, 6U> environment_fields{
			"release_version",
			"surface",
			"os",
			"architecture",
			"compiler_provider_major",
			"linkage"};
		if (environment == nullptr || !has_only(*environment, environment_fields, error) ||
			!text_member(
				*environment, "release_version", true, output.environment.release_version, error) ||
			!text_member(*environment, "surface", true, output.environment.surface, error) ||
			!text_member(*environment, "os", true, output.environment.os, error) ||
			!text_member(
				*environment, "architecture", true, output.environment.architecture, error) ||
			!text_member(*environment,
						 "compiler_provider_major",
						 true,
						 output.environment.compiler_provider_major,
						 error) ||
			!text_member(*environment, "linkage", true, output.environment.linkage, error))
			return product_error{"doctor.project-invalid", "environment", std::move(error)};
		if (!semantic_version_value(output.environment.release_version) ||
			output.environment.release_version.starts_with("0.") ||
			(output.environment.linkage != "static" && output.environment.linkage != "shared"))
			return product_error{"doctor.project-invalid", "environment", "invalid-support-tuple"};

		if (const auto* source = project->member("source_input"); source != nullptr)
		{
			static constexpr std::array<std::string_view, 2U> fields{"source_snapshot_id",
																	 "compilation_database_id"};
			if (!has_only(*source, fields, error) ||
				!text_member(
					*source, "source_snapshot_id", true, output.source_snapshot_id, error) ||
				!text_member(*source,
							 "compilation_database_id",
							 true,
							 output.compilation_database_id,
							 error))
				return product_error{"doctor.project-invalid", "source_input", std::move(error)};
			output.source_input = true;
		}

		const auto* candidates =
			required_member(*project, "provider_candidates", "provider_candidates", error);
		if (candidates == nullptr || candidates->kind != json_kind::array)
			return product_error{"doctor.project-invalid", "provider_candidates", "array-required"};
		if (candidates->array.size() > maximum_capability_count)
			return product_error{"doctor.project-invalid", "provider_candidates", "count-limit"};
		std::set<std::string, std::less<>> candidate_ids;
		for (const auto& candidate_value : candidates->array)
		{
			static constexpr std::array<std::string_view, 12U> fields{
				"candidate_id",
				"provider_id",
				"provider_version",
				"package_identity",
				"provider_manifest_digest",
				"provider_binary_digest",
				"provider_semantic_contract_digest",
				"protocol",
				"features",
				"relations",
				"interpretations",
				"sandbox"};
			provider_candidate candidate;
			if (!has_only(candidate_value, fields, error) ||
				!text_member(
					candidate_value, "candidate_id", true, candidate.candidate_id, error) ||
				!text_member(candidate_value, "provider_id", true, candidate.provider_id, error) ||
				!text_member(
					candidate_value, "provider_version", true, candidate.provider_version, error) ||
				!text_member(
					candidate_value, "package_identity", true, candidate.package_identity, error) ||
				!text_member(candidate_value,
							 "provider_manifest_digest",
							 true,
							 candidate.provider_manifest_digest,
							 error) ||
				!text_member(candidate_value,
							 "provider_binary_digest",
							 true,
							 candidate.provider_binary_digest,
							 error) ||
				!text_member(candidate_value,
							 "provider_semantic_contract_digest",
							 true,
							 candidate.provider_semantic_contract_digest,
							 error) ||
				!string_array_member(
					candidate_value, "features", true, candidate.features, error) ||
				!string_array_member(
					candidate_value, "relations", true, candidate.relations, error) ||
				!string_array_member(
					candidate_value, "interpretations", true, candidate.interpretations, error))
				return product_error{
					"doctor.project-invalid", "provider_candidates", std::move(error)};
			if (!semantic_version_value(candidate.provider_version) ||
				candidate.provider_version.starts_with("0.") ||
				!semantic_digest_value(candidate.candidate_id) ||
				!digest_value(candidate.provider_manifest_digest) ||
				!digest_value(candidate.provider_binary_digest) ||
				!digest_value(candidate.provider_semantic_contract_digest))
				return product_error{
					"doctor.project-invalid", "provider_candidates", "invalid-provider-identity"};

			const auto* protocol = required_member(candidate_value, "protocol", "protocol", error);
			static constexpr std::array<std::string_view, 2U> protocol_fields{"major", "minor"};
			if (protocol == nullptr || !has_only(*protocol, protocol_fields, error) ||
				!unsigned_member(*protocol, "major", true, candidate.protocol_major, error) ||
				!unsigned_member(*protocol, "minor", true, candidate.protocol_minor, error) ||
				candidate.protocol_major == 0U)
				return product_error{
					"doctor.project-invalid", "provider_candidates", "invalid-protocol"};

			const auto* sandbox = required_member(candidate_value, "sandbox", "sandbox", error);
			static constexpr std::array<std::string_view, 2U> sandbox_fields{"minimum",
																			 "policy_digest"};
			if (sandbox == nullptr || !has_only(*sandbox, sandbox_fields, error) ||
				!text_member(*sandbox, "minimum", true, candidate.sandbox_minimum, error) ||
				!text_member(
					*sandbox, "policy_digest", true, candidate.sandbox_policy_digest, error) ||
				!digest_value(candidate.sandbox_policy_digest))
				return product_error{
					"doctor.project-invalid", "provider_candidates", "invalid-sandbox"};

			if (!candidate_ids.insert(candidate.candidate_id).second)
				return product_error{
					"doctor.project-invalid", "provider_candidates", "duplicate-candidate-id"};
			output.provider_candidates.push_back(std::move(candidate));
		}
		std::ranges::sort(output.provider_candidates,
						  [](const provider_candidate& left, const provider_candidate& right)
						  {
							  return left.candidate_id < right.candidate_id;
						  });

		if (const auto* store = project->member("store"); store != nullptr)
		{
			static constexpr std::array<std::string_view, 2U> fields{"backend", "format"};
			if (!has_only(*store, fields, error) ||
				!text_member(*store, "backend", true, output.store_backend, error) ||
				!text_member(*store, "format", true, output.store_format, error))
				return product_error{"doctor.project-invalid", "store", std::move(error)};
			output.store_input = true;
			if ((output.store_backend != "memory" && output.store_backend != "sqlite") ||
				output.store_format != "cxxlens.snapshot.v3")
				return product_error{"doctor.project-invalid", "store", "unsupported-schema-value"};
		}
		return output;
	}

} // namespace cxxlens::sdk::doctor
