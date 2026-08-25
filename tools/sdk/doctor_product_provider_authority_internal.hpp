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
	inline constexpr std::size_t maximum_authority_node_count = 4096U;

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
		provider_untrusted,
		provider_revoked,
		trust_unknown,
		revocation_unknown,
		conflicting_capability,
		catalog_binding_invalid,
		catalog_unavailable,
		catalog_unverified,
		catalog_rejected,
		catalog_revoked,
		provider_certification_unavailable,
		provider_certification_unverified,
		source_closure_unavailable,
		store_authority_unavailable,
	};

	enum class authority_verdict : std::uint8_t
	{
		verified,
		absent,
		unverified,
		rejected,
		revoked,
	};

	[[nodiscard]] constexpr bool valid_authority_verdict(const authority_verdict value) noexcept
	{
		return value >= authority_verdict::verified && value <= authority_verdict::revoked;
	}

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
			case diagnosis_reason::provider_untrusted:
				return "doctor.provider-untrusted";
			case diagnosis_reason::provider_revoked:
				return "doctor.provider-revoked";
			case diagnosis_reason::trust_unknown:
				return "doctor.trust-unknown";
			case diagnosis_reason::revocation_unknown:
				return "doctor.revocation-unknown";
			case diagnosis_reason::conflicting_capability:
				return "doctor.conflicting-capability";
			case diagnosis_reason::catalog_binding_invalid:
				return "doctor.catalog-binding-invalid";
			case diagnosis_reason::catalog_unavailable:
				return "doctor.catalog-unavailable";
			case diagnosis_reason::catalog_unverified:
				return "doctor.catalog-unverified";
			case diagnosis_reason::catalog_rejected:
				return "doctor.catalog-rejected";
			case diagnosis_reason::catalog_revoked:
				return "doctor.catalog-revoked";
			case diagnosis_reason::provider_certification_unavailable:
				return "doctor.provider-certification-unavailable";
			case diagnosis_reason::provider_certification_unverified:
				return "doctor.provider-certification-unverified";
			case diagnosis_reason::source_closure_unavailable:
				return "doctor.source-closure-unavailable";
			case diagnosis_reason::store_authority_unavailable:
				return "doctor.store-authority-unavailable";
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

	enum class trust_state : std::uint8_t
	{
		verified,
		unknown,
		rejected,
	};

	enum class revocation_state : std::uint8_t
	{
		not_revoked,
		revoked,
		unknown,
	};

	struct revocation_context
	{
		revocation_state state{revocation_state::unknown};
		std::optional<std::uint64_t> effective_sequence;
		std::optional<std::string> reason;
	};

	struct trust_context
	{
		trust_state state{trust_state::unknown};
		std::uint64_t registry_sequence{};
		std::optional<std::string> certificate_id;
		std::optional<std::string> trust_anchor_id;
		std::optional<std::string> signature_digest;
		revocation_context revocation;
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
		trust_context trust;
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

	struct project_catalog_authority
	{
		authority_verdict verdict{authority_verdict::unverified};
		std::string catalog_id;
		std::string catalog_digest;
		std::string logical_root;
		std::string environment_digest;
		std::optional<support_tuple> environment;
	};

	struct certification_qualification
	{
		std::string level;
		std::string relation;
		std::string interpretation;
		std::vector<std::string> toolchains;
		std::vector<std::string> platforms;
	};

	struct provider_certification_authority
	{
		authority_verdict verdict{authority_verdict::unverified};
		authority_verdict execution_verdict{authority_verdict::unverified};
		authority_verdict signature_verdict{authority_verdict::unverified};
		authority_verdict trust_anchor_verdict{authority_verdict::unverified};
		authority_verdict revocation_verdict{authority_verdict::unverified};
		std::string registry_id;
		std::string registry_semantic_identity;
		std::string candidate_id;
		std::string provider_id;
		std::string provider_version;
		std::string package_identity;
		std::string provider_manifest_digest;
		std::string provider_binary_digest;
		std::string provider_semantic_contract_digest;
		std::string execution_semantic_identity;
		std::string selection_semantic_identity;
		std::uint32_t protocol_major{};
		std::uint32_t protocol_minor{};
		std::vector<std::string> features;
		std::vector<std::string> relations;
		std::vector<std::string> interpretations;
		std::vector<std::string> platform_tuples;
		std::vector<certification_qualification> certified_qualifications;
		std::string sandbox_assurance;
		std::string sandbox_policy_digest;
		std::uint64_t registry_sequence{};
		std::optional<std::string> certificate_id;
		std::optional<std::string> trust_anchor_id;
		std::optional<std::string> signature_digest;
	};

	struct certification_trust_anchor
	{
		std::string id;
		std::string public_key_fingerprint;
		std::string scope;
		bool production_use{};
	};

	struct certification_issuer
	{
		std::string id;
		std::string trust_anchor_id;
		std::string public_key_fingerprint;
		bool production_scope{};
		std::vector<std::string> allowed_qualifications;
		std::vector<std::string> namespace_prefixes;
	};

	struct certification_subject
	{
		std::string provider_id;
		std::string provider_version;
		std::string package_identity;
		std::string publisher;
		std::string manifest_digest;
		std::string binary_digest;
		std::string semantic_contract_digest;
	};

	struct provider_certificate
	{
		std::string id;
		std::string issuer_id;
		std::string serial;
		certification_subject subject;
		std::vector<certification_qualification> qualifications;
		std::uint64_t not_before_epoch{};
		std::uint64_t not_after_epoch{};
		std::uint64_t registry_sequence{};
		std::string certificate_signature_digest;
	};

	struct certification_revocation
	{
		std::string certificate_id;
		std::uint64_t effective_sequence{};
		std::string reason;
	};

	struct certification_registry_document
	{
		std::string schema{"cxxlens.provider-certification-registry.v1"};
		std::string document_version{"1.0.0"};
		std::string maturity{"accepted"};
		std::string authority_decision_adr{
			"docs/design/adr/0011-provider-trust-certification-discovery.md"};
		std::string authority_owner{"steward.ng-security"};
		std::string update_source{"explicit-installed-registry"};
		std::string update_signature{"required"};
		std::string update_rollback{"monotonically-increasing-sequence"};
		std::string update_clock{"trusted-time-port"};
		std::uint64_t update_sequence{};
		std::vector<certification_trust_anchor> trust_anchors;
		std::vector<certification_issuer> issuers;
		std::vector<provider_certificate> certificates;
		std::vector<certification_revocation> revocations;
		std::string declared_semantic_identity;
		std::string registry_signature_digest;
	};

	class provider_execution_authority_loader;

	// Immutable provider execution authority.  The private constructor accepts only a report
	// whose success bit was minted by process_provider_runtime after transcript, sealed-image,
	// sandbox-policy, and resource validation.  Project/provider JSON cannot construct it.
	class authenticated_provider_execution final
	{
	  public:
		[[nodiscard]] const sdk::provider::manifest& provider() const noexcept
		{
			return provider_;
		}

		[[nodiscard]] std::string_view measured_executable_digest() const noexcept
		{
			return measured_executable_digest_;
		}

		[[nodiscard]] const sdk::provider::sandbox_report& sandbox() const noexcept
		{
			return sandbox_;
		}

		[[nodiscard]] std::string_view execution_semantic_identity() const noexcept
		{
			return execution_semantic_identity_;
		}

		[[nodiscard]] std::string_view candidate_identity() const noexcept
		{
			return candidate_identity_;
		}

		[[nodiscard]] std::string_view selection_semantic_identity() const noexcept
		{
			return selection_semantic_identity_;
		}

	  private:
		friend class provider_execution_authority_loader;

		authenticated_provider_execution(sdk::provider::process_execution_report report,
										 std::string candidate_identity,
										 std::string selection_semantic_identity)
			: candidate_identity_{std::move(candidate_identity)},
			  selection_semantic_identity_{std::move(selection_semantic_identity)}
		{
			execution_semantic_identity_ = report.semantic_digest();
			provider_ = std::move(report.provider);
			measured_executable_digest_ = std::move(report.measured_executable_digest);
			sandbox_ = std::move(report.sandbox);
		}

		sdk::provider::manifest provider_;
		std::string measured_executable_digest_;
		sdk::provider::sandbox_report sandbox_;
		std::string execution_semantic_identity_;
		std::string candidate_identity_;
		std::string selection_semantic_identity_;
	};

	class provider_installation_artifact final
	{
	  public:
		[[nodiscard]] const sdk::provider::provider_selection& discovery() const noexcept
		{
			return discovery_;
		}

		[[nodiscard]] const authenticated_provider_execution* execution() const noexcept
		{
			return execution_ ? &*execution_ : nullptr;
		}

	  private:
		friend class provider_execution_authority_loader;

		explicit provider_installation_artifact(sdk::provider::provider_selection discovery)
			: discovery_{std::move(discovery)}
		{
		}

		provider_installation_artifact(sdk::provider::provider_selection discovery,
									   authenticated_provider_execution execution)
			: discovery_{std::move(discovery)}, execution_{std::move(execution)}
		{
		}

		sdk::provider::provider_selection discovery_;
		std::optional<authenticated_provider_execution> execution_;
	};

	struct store_publication_artifact
	{
		sdk::snapshot_handle publication;
	};

	struct store_publication_authority
	{
		std::string backend;
		std::string format;
		std::string catalog_digest;
		std::string snapshot_id;
		std::string publication_id;
	};

	struct signature_verification_result
	{
		authority_verdict verdict{authority_verdict::unverified};
		std::string verifier_id;
		std::string key_fingerprint;
		std::string signed_subject_digest;
		std::string signature_digest;
	};

	// ADR 0011 assigns cryptographic operations and production trust-anchor storage
	// to this product port.  Registry/project JSON never supplies its verdict.
	class trusted_signature_verifier
	{
	  public:
		virtual ~trusted_signature_verifier() = default;
		[[nodiscard]] virtual signature_verification_result
		verify(std::string_view signature_scope,
			   std::string_view signer_id,
			   std::string_view signed_subject_digest,
			   std::string_view signature_digest) const = 0;
	};

	class unavailable_signature_verifier final : public trusted_signature_verifier
	{
	  public:
		[[nodiscard]] signature_verification_result verify(std::string_view,
														   std::string_view,
														   std::string_view,
														   std::string_view) const override
		{
			return {};
		}
	};

	enum class registry_sequence_acceptance : std::uint8_t
	{
		accepted,
		rollback,
		unavailable,
	};

	[[nodiscard]] constexpr bool
	valid_registry_sequence_acceptance(const registry_sequence_acceptance value) noexcept
	{
		return value >= registry_sequence_acceptance::accepted &&
			value <= registry_sequence_acceptance::unavailable;
	}

	// Time and rollback state are product ports.  The loader never accepts an epoch or
	// a sequence floor from project/provider JSON.  Implementations must atomically
	// reject a sequence older than the persisted floor and advance that floor on success.
	class trusted_authority_state_port
	{
	  public:
		virtual ~trusted_authority_state_port() = default;
		[[nodiscard]] virtual std::optional<std::uint64_t> trusted_epoch() const = 0;
		[[nodiscard]] virtual std::optional<support_tuple> installed_environment() const = 0;
		[[nodiscard]] virtual registry_sequence_acceptance
		accept_registry_update(std::string_view registry_id,
							   std::uint64_t update_sequence,
							   std::string_view registry_semantic_identity) const = 0;
	};

	class unavailable_authority_state_port final : public trusted_authority_state_port
	{
	  public:
		[[nodiscard]] std::optional<std::uint64_t> trusted_epoch() const override
		{
			return std::nullopt;
		}
		[[nodiscard]] std::optional<support_tuple> installed_environment() const override
		{
			return std::nullopt;
		}
		[[nodiscard]] registry_sequence_acceptance
		accept_registry_update(std::string_view, std::uint64_t, std::string_view) const override
		{
			return registry_sequence_acceptance::unavailable;
		}
	};

	class installed_product_authority_loader;

	// Opaque, non-polymorphic authority value.  Only the concrete loader below can
	// create records, so a caller cannot turn JSON booleans into a proved result by
	// subclassing a verifier or by constructing a "verified" aggregate.
	class installed_product_authority_verifier final
	{
	  public:
		installed_product_authority_verifier() = default;

		[[nodiscard]] project_catalog_authority
		lookup_project_catalog(const std::string_view catalog_id) const
		{
			if (!project_catalog_ || project_catalog_->catalog_id != catalog_id)
				return {authority_verdict::absent, {}, {}, {}, {}, std::nullopt};
			return *project_catalog_;
		}

		[[nodiscard]] provider_certification_authority
		lookup_provider(const std::string_view candidate_id) const
		{
			if (duplicate_provider_ids_.contains(candidate_id))
			{
				provider_certification_authority output;
				output.verdict = authority_verdict::rejected;
				return output;
			}
			const auto found = providers_.find(candidate_id);
			if (found == providers_.end())
			{
				provider_certification_authority output;
				output.verdict = authority_verdict::absent;
				return output;
			}
			return found->second;
		}

		[[nodiscard]] std::optional<store_publication_authority>
		lookup_store(const std::string_view backend,
					 const std::string_view format,
					 const std::string_view catalog_digest) const
		{
			const auto found = stores_.find(
				std::tuple{std::string{backend}, std::string{format}, std::string{catalog_digest}});
			return found == stores_.end()
				? std::optional<store_publication_authority>{}
				: std::optional<store_publication_authority>{found->second};
		}

	  private:
		friend class installed_product_authority_loader;

		installed_product_authority_verifier(
			std::optional<project_catalog_authority> project_catalog,
			std::vector<provider_certification_authority> provider_certifications,
			std::vector<store_publication_authority> store_publications)
			: project_catalog_{std::move(project_catalog)}
		{
			for (auto& certification : provider_certifications)
			{
				auto candidate_id = certification.candidate_id;
				if (!providers_.emplace(candidate_id, std::move(certification)).second)
					duplicate_provider_ids_.insert(std::move(candidate_id));
			}
			for (auto& publication : store_publications)
				stores_.emplace(
					std::tuple{publication.backend, publication.format, publication.catalog_digest},
					std::move(publication));
		}

		std::optional<project_catalog_authority> project_catalog_;
		std::map<std::string, provider_certification_authority, std::less<>> providers_;
		std::set<std::string, std::less<>> duplicate_provider_ids_;
		std::map<std::tuple<std::string, std::string, std::string>, store_publication_authority>
			stores_;
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

	[[nodiscard]] inline std::variant<std::string, product_error>
	provider_selection_authority_identity(const sdk::provider::provider_selection& selection)
	{
		if (auto valid = selection.validate(); !valid)
			return product_error{
				"doctor.authority-invalid", "provider_selection", valid.error().code};
		const auto& request = selection.authority_request();
		const auto fallback = selection.fallback_policy_digest();
		const auto projection = canonical_json(json_value::object_value({
			{"authority_request",
			 json_value::object_value({
				 {"fallback_policy_digest",
				  fallback ? json_value::string_value(*fallback) : json_value::null()},
				 {"provider_binary_digest",
				  json_value::string_value(request.provider_binary_digest)},
				 {"provider_id", json_value::string_value(request.provider_id)},
				 {"provider_semantic_contract_digest",
				  json_value::string_value(request.provider_semantic_contract_digest)},
				 {"provider_version", json_value::string_value(request.provider_version.string())},
				 {"require_certification",
				  json_value::boolean_value(request.require_certification)},
				 {"sandbox_minimum",
				  json_value::unsigned_value(static_cast<std::uint64_t>(request.sandbox.minimum))},
				 {"sandbox_policy_digest", json_value::string_value(request.sandbox.policy_digest)},
			 })},
			{"selection", json_value::string_value(selection.canonical_form())},
		}));
		auto identity = sdk::semantic_digest("cxxlens.provider-selection-authority.v1", projection);
		if (!identity || !semantic_digest_value(*identity))
			return product_error{
				"doctor.authority-invalid", "provider_selection", "semantic-identity"};
		return std::move(*identity);
	}

	class provider_execution_authority_loader final
	{
	  public:
		[[nodiscard]] std::variant<provider_installation_artifact, product_error>
		observe(sdk::provider::provider_selection selection) const
		{
			if (auto valid = selection.validate(); !valid)
				return product_error{
					"doctor.authority-invalid", "provider_selection", valid.error().code};
			return provider_installation_artifact{std::move(selection)};
		}

		[[nodiscard]] std::variant<provider_installation_artifact, product_error>
		load(sdk::provider::process_task_request request) const
		{
			auto selection_identity = provider_selection_authority_identity(request.selection);
			if (std::holds_alternative<product_error>(selection_identity))
				return std::get<product_error>(std::move(selection_identity));
			const auto selected =
				std::ranges::find_if(request.selection.decisions(),
									 [](const sdk::provider::provider_candidate_decision& decision)
									 {
										 return decision.selected;
									 });
			if (selected == request.selection.decisions().end() ||
				!semantic_digest_value(selected->candidate_digest))
				return product_error{
					"doctor.authority-invalid", "provider_selection", "candidate-identity"};
			auto candidate_identity = selected->candidate_digest;
			auto processes = sdk::provider::make_system_provider_process_port();
			if (!processes)
				return product_error{
					"doctor.authority-unavailable", "provider_execution", "system-process-port"};
			const sdk::provider::process_provider_runtime runtime{*processes};
			auto executed = runtime.execute(request);
			if (!executed)
				return product_error{"doctor.authority-unavailable",
									 request.task_id,
									 executed.error().code + ":" + executed.error().field};
			auto report = std::move(*executed);
			const auto invalid_report = [&](const std::string_view detail)
			{
				return product_error{
					"doctor.authority-invalid", "provider_execution", std::string{detail}};
			};
			if (!report.succeeded())
				return invalid_report("validated-success-required");
			if (report.exit_code != 0 || report.termination_signal != 0)
				return invalid_report("clean-exit-required");
			if (report.frames.empty() || report.frames.size() > maximum_json_collection_count)
				return invalid_report("bounded-transcript-required");
			if (!digest_value(report.task_input_digest) ||
				!digest_value(report.normalized_invocation_digest) ||
				!digest_value(report.toolchain_digest) ||
				!digest_value(report.environment_digest) ||
				!digest_value(report.measured_executable_digest) ||
				!semantic_digest_value(report.semantic_digest()))
				return invalid_report("canonical-identity-required");
			if (auto valid = report.provider.validate(); !valid)
				return product_error{
					"doctor.authority-invalid", "provider_execution", "provider-manifest"};
			if (auto valid = report.sandbox.validate(); !valid)
				return product_error{
					"doctor.authority-invalid", "provider_execution", "sandbox-report"};
			auto execution = authenticated_provider_execution{
				std::move(report),
				std::move(candidate_identity),
				std::get<std::string>(std::move(selection_identity))};
			return provider_installation_artifact{std::move(request.selection),
												  std::move(execution)};
		}
	};

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

	struct installed_authority_source
	{
		std::optional<sdk::project_catalog> project_catalog;
		std::optional<certification_registry_document> certification_registry;
		std::vector<provider_installation_artifact> provider_installations;
		std::vector<store_publication_artifact> store_publications;
	};

	[[nodiscard]] inline std::string content_digest_text(const std::string_view value)
	{
		return sdk::content_digest(std::as_bytes(std::span{value.data(), value.size()}));
	}

	[[nodiscard]] inline json_value::array_type
	authority_string_set(std::vector<std::string> values)
	{
		std::ranges::sort(values);
		json_value::array_type output;
		output.reserve(values.size());
		for (auto& value : values)
			output.push_back(json_value::string_value(std::move(value)));
		return output;
	}

	[[nodiscard]] inline bool
	bounded_certification_registry(const certification_registry_document& registry) noexcept
	{
		std::size_t bytes{};
		std::size_t nodes{};
		const auto node = [&](const std::size_t count = 1U)
		{
			if (count > maximum_authority_node_count - nodes)
				return false;
			nodes += count;
			return true;
		};
		const auto text = [&](const std::string& value)
		{
			if (value.size() > maximum_json_string_bytes ||
				value.size() > maximum_project_bytes - bytes || !node())
				return false;
			bytes += value.size();
			return valid_utf8(value);
		};
		const auto texts = [&](const std::vector<std::string>& values)
		{
			return values.size() <= maximum_json_collection_count && node(values.size()) &&
				std::ranges::all_of(values, text);
		};
		if (registry.trust_anchors.size() > maximum_json_collection_count ||
			registry.issuers.size() > maximum_json_collection_count ||
			registry.certificates.size() > maximum_json_collection_count ||
			registry.revocations.size() > maximum_json_collection_count ||
			!node(registry.trust_anchors.size()) || !node(registry.issuers.size()) ||
			!node(registry.certificates.size()) || !node(registry.revocations.size()) ||
			!text(registry.schema) || !text(registry.document_version) ||
			!text(registry.maturity) || !text(registry.authority_decision_adr) ||
			!text(registry.authority_owner) || !text(registry.update_source) ||
			!text(registry.update_signature) || !text(registry.update_rollback) ||
			!text(registry.update_clock) || !text(registry.declared_semantic_identity) ||
			!text(registry.registry_signature_digest))
			return false;
		for (const auto& anchor : registry.trust_anchors)
			if (!text(anchor.id) || !text(anchor.public_key_fingerprint) || !text(anchor.scope))
				return false;
		for (const auto& issuer : registry.issuers)
			if (!text(issuer.id) || !text(issuer.trust_anchor_id) ||
				!text(issuer.public_key_fingerprint) || !texts(issuer.allowed_qualifications) ||
				!texts(issuer.namespace_prefixes))
				return false;
		for (const auto& certificate : registry.certificates)
		{
			if (certificate.qualifications.size() > maximum_json_collection_count ||
				!node(certificate.qualifications.size()) || !text(certificate.id) ||
				!text(certificate.issuer_id) || !text(certificate.serial) ||
				!text(certificate.certificate_signature_digest) ||
				!text(certificate.subject.provider_id) ||
				!text(certificate.subject.provider_version) ||
				!text(certificate.subject.package_identity) ||
				!text(certificate.subject.publisher) ||
				!text(certificate.subject.manifest_digest) ||
				!text(certificate.subject.binary_digest) ||
				!text(certificate.subject.semantic_contract_digest))
				return false;
			for (const auto& qualification : certificate.qualifications)
				if (!node(qualification.toolchains.size()) ||
					!node(qualification.platforms.size()) || !text(qualification.level) ||
					!text(qualification.relation) || !text(qualification.interpretation) ||
					!texts(qualification.toolchains) || !texts(qualification.platforms))
					return false;
		}
		for (const auto& revocation : registry.revocations)
			if (!text(revocation.certificate_id) || !text(revocation.reason))
				return false;
		return true;
	}

	[[nodiscard]] inline bool
	valid_certification_registry_structure(const certification_registry_document& registry);

	[[nodiscard]] inline std::variant<std::string, product_error>
	certification_registry_semantic_identity(const certification_registry_document& input)
	{
		if (!valid_certification_registry_structure(input))
			return product_error{"doctor.authority-invalid", "certification_registry", "bound"};
		auto registry = input;
		std::ranges::sort(registry.trust_anchors, {}, &certification_trust_anchor::id);
		std::ranges::sort(registry.issuers, {}, &certification_issuer::id);
		std::ranges::sort(registry.certificates, {}, &provider_certificate::id);
		std::ranges::sort(
			registry.revocations,
			[](const certification_revocation& left, const certification_revocation& right)
			{
				return std::tie(left.certificate_id, left.effective_sequence, left.reason) <
					std::tie(right.certificate_id, right.effective_sequence, right.reason);
			});
		json_value::array_type anchors;
		for (auto& anchor : registry.trust_anchors)
			anchors.push_back(json_value::object_value({
				{"id", json_value::string_value(std::move(anchor.id))},
				{"production_use",
				 json_value::string_value(anchor.production_use ? "allowed" : "forbidden")},
				{"public_key_fingerprint",
				 json_value::string_value(std::move(anchor.public_key_fingerprint))},
				{"scope", json_value::string_value(std::move(anchor.scope))},
			}));
		json_value::array_type issuers;
		for (auto& issuer : registry.issuers)
			issuers.push_back(json_value::object_value({
				{"allowed_qualifications",
				 json_value::array_value(
					 authority_string_set(std::move(issuer.allowed_qualifications)))},
				{"id", json_value::string_value(std::move(issuer.id))},
				{"namespace_prefixes",
				 json_value::array_value(
					 authority_string_set(std::move(issuer.namespace_prefixes)))},
				{"public_key_fingerprint",
				 json_value::string_value(std::move(issuer.public_key_fingerprint))},
				{"scope",
				 json_value::string_value(issuer.production_scope ? "production"
																  : "conformance-only")},
				{"trust_anchor", json_value::string_value(std::move(issuer.trust_anchor_id))},
			}));
		json_value::array_type certificates;
		for (auto& certificate : registry.certificates)
		{
			for (auto& qualification : certificate.qualifications)
			{
				std::ranges::sort(qualification.toolchains);
				std::ranges::sort(qualification.platforms);
			}
			std::ranges::sort(certificate.qualifications,
							  [](const certification_qualification& left,
								 const certification_qualification& right)
							  {
								  return std::tie(left.level,
												  left.relation,
												  left.interpretation,
												  left.toolchains,
												  left.platforms) < std::tie(right.level,
																			 right.relation,
																			 right.interpretation,
																			 right.toolchains,
																			 right.platforms);
							  });
			json_value::array_type qualifications;
			for (auto& qualification : certificate.qualifications)
				qualifications.push_back(json_value::object_value({
					{"interpretation",
					 json_value::string_value(std::move(qualification.interpretation))},
					{"level", json_value::string_value(std::move(qualification.level))},
					{"platforms",
					 json_value::array_value(
						 authority_string_set(std::move(qualification.platforms)))},
					{"relation", json_value::string_value(std::move(qualification.relation))},
					{"toolchains",
					 json_value::array_value(
						 authority_string_set(std::move(qualification.toolchains)))},
				}));
			certificates.push_back(json_value::object_value({
				{"certificate_signature",
				 json_value::string_value(std::move(certificate.certificate_signature_digest))},
				{"id", json_value::string_value(std::move(certificate.id))},
				{"issuer", json_value::string_value(std::move(certificate.issuer_id))},
				{"qualifications", json_value::array_value(std::move(qualifications))},
				{"registry_sequence", json_value::unsigned_value(certificate.registry_sequence)},
				{"serial", json_value::string_value(std::move(certificate.serial))},
				{"subject",
				 json_value::object_value({
					 {"binary_digest",
					  json_value::string_value(std::move(certificate.subject.binary_digest))},
					 {"manifest_digest",
					  json_value::string_value(std::move(certificate.subject.manifest_digest))},
					 {"package_identity",
					  json_value::string_value(std::move(certificate.subject.package_identity))},
					 {"provider_id",
					  json_value::string_value(std::move(certificate.subject.provider_id))},
					 {"provider_version",
					  json_value::string_value(std::move(certificate.subject.provider_version))},
					 {"publisher",
					  json_value::string_value(std::move(certificate.subject.publisher))},
					 {"semantic_contract_digest",
					  json_value::string_value(
						  std::move(certificate.subject.semantic_contract_digest))},
				 })},
				{"validity",
				 json_value::object_value({
					 {"not_after_epoch", json_value::unsigned_value(certificate.not_after_epoch)},
					 {"not_before_epoch", json_value::unsigned_value(certificate.not_before_epoch)},
				 })},
			}));
		}
		json_value::array_type revocations;
		for (auto& revocation : registry.revocations)
			revocations.push_back(json_value::object_value({
				{"certificate_id", json_value::string_value(std::move(revocation.certificate_id))},
				{"effective_sequence", json_value::unsigned_value(revocation.effective_sequence)},
				{"reason", json_value::string_value(std::move(revocation.reason))},
			}));
		const auto projection = canonical_json(json_value::object_value({
			{"authority",
			 json_value::object_value({
				 {"decision_adr",
				  json_value::string_value(std::move(registry.authority_decision_adr))},
				 {"owner", json_value::string_value(std::move(registry.authority_owner))},
			 })},
			{"certificates", json_value::array_value(std::move(certificates))},
			{"document_version", json_value::string_value(std::move(registry.document_version))},
			{"issuers", json_value::array_value(std::move(issuers))},
			{"maturity", json_value::string_value(std::move(registry.maturity))},
			{"revocations", json_value::array_value(std::move(revocations))},
			{"schema", json_value::string_value(std::move(registry.schema))},
			{"trust_anchors", json_value::array_value(std::move(anchors))},
			{"update_policy",
			 json_value::object_value({
				 {"clock", json_value::string_value(std::move(registry.update_clock))},
				 {"rollback", json_value::string_value(std::move(registry.update_rollback))},
				 {"signature", json_value::string_value(std::move(registry.update_signature))},
				 {"source", json_value::string_value(std::move(registry.update_source))},
			 })},
		}));
		auto digest =
			sdk::semantic_digest("cxxlens.provider-certification-registry.v1", projection);
		if (!digest)
			return product_error{digest.error().code, digest.error().field, digest.error().detail};
		return std::move(*digest);
	}

	[[nodiscard]] inline std::variant<std::string, product_error>
	certification_registry_update_subject(const std::string_view registry_semantic_identity,
										  const std::uint64_t update_sequence)
	{
		if (!semantic_digest_value(registry_semantic_identity))
			return product_error{
				"doctor.authority-invalid", "certification_registry", "semantic-identity"};
		const auto projection = canonical_json(json_value::object_value({
			{"registry_semantic_identity",
			 json_value::string_value(std::string{registry_semantic_identity})},
			{"update_sequence", json_value::unsigned_value(update_sequence)},
		}));
		auto digest =
			sdk::semantic_digest("cxxlens.provider-certification-registry-update.v1", projection);
		if (!digest)
			return product_error{digest.error().code, digest.error().field, digest.error().detail};
		return std::move(*digest);
	}

	[[nodiscard]] inline std::string
	certificate_subject_digest(const certification_subject& subject)
	{
		json_value::array_type ordered;
		const auto field = [&](const std::string_view name, const std::string& value)
		{
			ordered.push_back(json_value::array_value(
				{json_value::string_value(std::string{name}), json_value::string_value(value)}));
		};
		field("provider_id", subject.provider_id);
		field("provider_version", subject.provider_version);
		field("package_identity", subject.package_identity);
		field("publisher", subject.publisher);
		field("manifest_digest", subject.manifest_digest);
		field("binary_digest", subject.binary_digest);
		field("semantic_contract_digest", subject.semantic_contract_digest);
		const auto projection = canonical_json(json_value::array_value(std::move(ordered)));
		std::string domain_bound{"cxxlens-provider-signature-subject-v1"};
		domain_bound.push_back('\0');
		domain_bound += projection;
		return content_digest_text(domain_bound);
	}

	[[nodiscard]] inline std::string_view
	sandbox_assurance_name(const sdk::provider::sandbox_assurance assurance) noexcept
	{
		switch (assurance)
		{
			case sdk::provider::sandbox_assurance::none:
				return "none";
			case sdk::provider::sandbox_assurance::best_effort:
				return "best_effort";
			case sdk::provider::sandbox_assurance::enforced:
				return "enforced";
			case sdk::provider::sandbox_assurance::certified:
				return "certified";
		}
		return "invalid";
	}

	[[nodiscard]] inline bool unique_authority_strings(std::vector<std::string> values)
	{
		std::ranges::sort(values);
		return std::ranges::adjacent_find(values) == values.end();
	}

	[[nodiscard]] inline bool authority_namespace_prefix(const std::string_view value) noexcept
	{
		if (!strict_id(value) || value.size() < 2U || value.back() != '.' || value.front() < 'a' ||
			value.front() > 'z')
			return false;
		return std::ranges::all_of(value,
								   [](const char byte)
								   {
									   return (byte >= 'a' && byte <= 'z') ||
										   (byte >= '0' && byte <= '9') || byte == '_' ||
										   byte == '-' || byte == '.';
								   });
	}

	[[nodiscard]] inline bool
	valid_certification_registry_structure(const certification_registry_document& registry)
	{
		if (!bounded_certification_registry(registry) ||
			registry.schema != "cxxlens.provider-certification-registry.v1" ||
			registry.document_version != "1.0.0" || registry.maturity != "accepted" ||
			registry.authority_decision_adr !=
				"docs/design/adr/0011-provider-trust-certification-discovery.md" ||
			registry.authority_owner != "steward.ng-security" ||
			registry.update_source != "explicit-installed-registry" ||
			registry.update_signature != "required" ||
			registry.update_rollback != "monotonically-increasing-sequence" ||
			registry.update_clock != "trusted-time-port" || registry.trust_anchors.empty() ||
			registry.issuers.empty())
			return false;

		std::set<std::string, std::less<>> anchor_ids;
		for (const auto& anchor : registry.trust_anchors)
			if (!strict_id(anchor.id) || !digest_value(anchor.public_key_fingerprint) ||
				(anchor.scope != "production" && anchor.scope != "conformance-only") ||
				(anchor.production_use && anchor.scope != "production") ||
				!anchor_ids.insert(anchor.id).second)
				return false;

		std::set<std::string, std::less<>> issuer_ids;
		for (const auto& issuer : registry.issuers)
			if (!strict_id(issuer.id) || !anchor_ids.contains(issuer.trust_anchor_id) ||
				!digest_value(issuer.public_key_fingerprint) ||
				issuer.allowed_qualifications.empty() || issuer.namespace_prefixes.empty() ||
				!unique_authority_strings(issuer.allowed_qualifications) ||
				!unique_authority_strings(issuer.namespace_prefixes) ||
				!std::ranges::all_of(issuer.allowed_qualifications, strict_id) ||
				!std::ranges::all_of(issuer.namespace_prefixes, authority_namespace_prefix) ||
				!issuer_ids.insert(issuer.id).second)
				return false;

		std::set<std::string, std::less<>> certificate_ids;
		std::set<std::pair<std::string, std::string>> certificate_subject_ids;
		for (const auto& certificate : registry.certificates)
		{
			const auto certificate_issuer = std::ranges::find(
				registry.issuers, certificate.issuer_id, &certification_issuer::id);
			if (!strict_id(certificate.id) || !issuer_ids.contains(certificate.issuer_id) ||
				!strict_id(certificate.serial) ||
				!semantic_version_value(certificate.subject.provider_version) ||
				!strict_id(certificate.subject.provider_id) ||
				!strict_id(certificate.subject.package_identity) ||
				!strict_id(certificate.subject.publisher) ||
				!digest_value(certificate.subject.manifest_digest) ||
				!digest_value(certificate.subject.binary_digest) ||
				!digest_value(certificate.subject.semantic_contract_digest) ||
				!digest_value(certificate.certificate_signature_digest) ||
				certificate.not_before_epoch > certificate.not_after_epoch ||
				certificate.registry_sequence > registry.update_sequence ||
				certificate.qualifications.empty() ||
				!certificate_ids.insert(certificate.id).second ||
				!certificate_subject_ids
					 .emplace(certificate.subject.provider_id, certificate.subject.provider_version)
					 .second)
				return false;
			std::set<std::tuple<std::string,
								std::string,
								std::string,
								std::vector<std::string>,
								std::vector<std::string>>>
				qualification_ids;
			for (const auto& qualification : certificate.qualifications)
			{
				auto toolchains = qualification.toolchains;
				auto platforms = qualification.platforms;
				std::ranges::sort(toolchains);
				std::ranges::sort(platforms);
				if (certificate_issuer == registry.issuers.end() ||
					std::ranges::find(certificate_issuer->allowed_qualifications,
									  qualification.level) ==
						certificate_issuer->allowed_qualifications.end() ||
					!strict_id(qualification.level) || !strict_id(qualification.relation) ||
					!strict_id(qualification.interpretation) || toolchains.empty() ||
					platforms.empty() || !unique_authority_strings(toolchains) ||
					!unique_authority_strings(platforms) ||
					!std::ranges::all_of(toolchains, strict_id) ||
					!std::ranges::all_of(platforms, strict_id) ||
					!qualification_ids
						 .emplace(qualification.level,
								  qualification.relation,
								  qualification.interpretation,
								  std::move(toolchains),
								  std::move(platforms))
						 .second)
					return false;
			}
		}

		std::set<std::string, std::less<>> revoked_certificates;
		for (const auto& revocation : registry.revocations)
			if (!certificate_ids.contains(revocation.certificate_id) ||
				revocation.effective_sequence > registry.update_sequence ||
				!strict_id(revocation.reason) ||
				!revoked_certificates.insert(revocation.certificate_id).second)
				return false;
		return true;
	}

	class installed_product_authority_loader final
	{
	  public:
		[[nodiscard]] std::variant<installed_product_authority_verifier, product_error>
		load(const installed_authority_source& source,
			 const trusted_signature_verifier& signature_verifier,
			 const trusted_authority_state_port& authority_state) const
		{
			std::optional<project_catalog_authority> project_catalog;
			const auto installed_environment = authority_state.installed_environment();
			if (source.project_catalog)
			{
				const auto catalog_bounded =
					source.project_catalog->compile_units.size() <= maximum_json_collection_count &&
					strict_id(source.project_catalog->catalog_id) &&
					strict_id(source.project_catalog->catalog_digest) &&
					strict_id(source.project_catalog->logical_root) &&
					strict_id(source.project_catalog->environment_digest) &&
					std::ranges::all_of(source.project_catalog->compile_units,
										[](const sdk::catalog_compile_unit& unit)
										{
											return strict_id(unit.compile_unit_id) &&
												strict_id(unit.effective_invocation_digest) &&
												strict_id(unit.source_digest) &&
												strict_id(unit.environment_digest);
										});
				if (!catalog_bounded)
					return product_error{"doctor.authority-invalid", "project_catalog", "bound"};
				if (auto valid = source.project_catalog->validate(); !valid)
					project_catalog =
						project_catalog_authority{authority_verdict::rejected,
												  source.project_catalog->catalog_id,
												  source.project_catalog->catalog_digest,
												  source.project_catalog->logical_root,
												  source.project_catalog->environment_digest,
												  installed_environment};
				else
					project_catalog =
						project_catalog_authority{authority_verdict::verified,
												  source.project_catalog->catalog_id,
												  source.project_catalog->catalog_digest,
												  source.project_catalog->logical_root,
												  source.project_catalog->environment_digest,
												  installed_environment};
			}

			if (source.provider_installations.size() > maximum_json_collection_count ||
				!bounded_provider_installations(source.provider_installations))
				return product_error{
					"doctor.authority-invalid", "provider_installations", "count-limit"};
			if (source.store_publications.size() > maximum_json_collection_count)
				return product_error{
					"doctor.authority-invalid", "store_publications", "count-limit"};
			std::vector<store_publication_authority> stores;
			stores.reserve(source.store_publications.size());
			std::set<std::tuple<std::string, std::string, std::string>> store_keys;
			for (const auto& artifact : source.store_publications)
			{
				if (!source.project_catalog || !project_catalog ||
					project_catalog->verdict != authority_verdict::verified ||
					artifact.publication.empty())
					return product_error{
						"doctor.authority-invalid", "store_publications", "catalog-or-handle"};
				const auto& manifest = artifact.publication.manifest();
				const auto& publication = artifact.publication.publication();
				const auto backend = std::string{artifact.publication.physical_backend()};
				constexpr std::string_view format{"cxxlens.snapshot.v3"};
				if ((backend != "memory" && backend != "sqlite") ||
					manifest.schema != "cxxlens.snapshot-manifest.v1" ||
					manifest.id != artifact.publication.id() ||
					publication.snapshot_id != artifact.publication.id() ||
					publication.state != sdk::publication_state::committed || publication.corrupt ||
					manifest.catalog_semantic_digest != source.project_catalog->catalog_digest)
					return product_error{
						"doctor.authority-invalid", "store_publications", "publication-binding"};
				auto key =
					std::tuple{backend, std::string{format}, manifest.catalog_semantic_digest};
				if (!store_keys.insert(key).second)
					return product_error{
						"doctor.authority-invalid", "store_publications", "duplicate-binding"};
				stores.push_back({backend,
								  std::string{format},
								  manifest.catalog_semantic_digest,
								  std::string{artifact.publication.id()},
								  publication.publication_id});
			}
			if (!source.certification_registry)
				return installed_product_authority_verifier{
					std::move(project_catalog), {}, std::move(stores)};
			const auto& registry = *source.certification_registry;
			if (!valid_certification_registry_structure(registry))
				return product_error{
					"doctor.authority-invalid", "certification_registry", "schema"};
			auto computed_registry_identity = certification_registry_semantic_identity(registry);
			if (std::holds_alternative<product_error>(computed_registry_identity))
				return std::get<product_error>(std::move(computed_registry_identity));
			const auto registry_identity =
				std::get<std::string>(std::move(computed_registry_identity));
			auto computed_update_subject =
				certification_registry_update_subject(registry_identity, registry.update_sequence);
			if (std::holds_alternative<product_error>(computed_update_subject))
				return std::get<product_error>(std::move(computed_update_subject));
			const auto update_subject = std::get<std::string>(std::move(computed_update_subject));
			authority_verdict registry_verdict{authority_verdict::unverified};
			const auto production_anchors =
				std::ranges::count_if(registry.trust_anchors,
									  [](const certification_trust_anchor& anchor)
									  {
										  return anchor.production_use;
									  });
			if (!semantic_digest_value(registry.declared_semantic_identity) ||
				registry.declared_semantic_identity != registry_identity ||
				!digest_value(registry.registry_signature_digest) || production_anchors != 1)
				registry_verdict = authority_verdict::rejected;
			else
			{
				const auto& anchor =
					*std::ranges::find_if(registry.trust_anchors,
										  [](const certification_trust_anchor& value)
										  {
											  return value.production_use;
										  });
				const auto verified = signature_verifier.verify("certification-registry",
																anchor.id,
																update_subject,
																registry.registry_signature_digest);
				registry_verdict = normalized_signature_verdict(verified,
																update_subject,
																registry.registry_signature_digest,
																anchor.public_key_fingerprint);
				if (registry_verdict == authority_verdict::verified)
				{
					const auto accepted = authority_state.accept_registry_update(
						registry.schema, registry.update_sequence, registry_identity);
					if (!valid_registry_sequence_acceptance(accepted) ||
						accepted == registry_sequence_acceptance::rollback)
						registry_verdict = authority_verdict::rejected;
					else if (accepted == registry_sequence_acceptance::unavailable)
						registry_verdict = authority_verdict::unverified;
				}
			}
			const auto trusted_epoch = authority_state.trusted_epoch();

			std::vector<provider_certification_authority> providers;
			providers.reserve(source.provider_installations.size());
			for (const auto& installation : source.provider_installations)
				providers.push_back(load_provider(installation,
												  registry,
												  registry_identity,
												  registry_verdict,
												  trusted_epoch,
												  signature_verifier));
			return installed_product_authority_verifier{
				std::move(project_catalog), std::move(providers), std::move(stores)};
		}

	  private:
		[[nodiscard]] static authority_verdict
		normalized_signature_verdict(const signature_verification_result& result,
									 const std::string_view expected_subject,
									 const std::string_view expected_signature,
									 const std::string_view expected_key) noexcept
		{
			if ((result.verdict == authority_verdict::absent ||
				 result.verdict == authority_verdict::unverified) &&
				result.verifier_id.empty() && result.key_fingerprint.empty() &&
				result.signed_subject_digest.empty() && result.signature_digest.empty())
				return result.verdict;
			if (!valid_authority_verdict(result.verdict))
				return authority_verdict::rejected;
			return exact_signature_result(
					   result, expected_subject, expected_signature, expected_key)
				? result.verdict
				: authority_verdict::rejected;
		}

		[[nodiscard]] static bool
		exact_signature_result(const signature_verification_result& result,
							   const std::string_view expected_subject,
							   const std::string_view expected_signature,
							   const std::string_view expected_key) noexcept
		{
			return strict_id(result.verifier_id) && result.key_fingerprint == expected_key &&
				result.signed_subject_digest == expected_subject &&
				result.signature_digest == expected_signature;
		}

		[[nodiscard]] static authority_verdict
		combine_verdicts(const std::initializer_list<authority_verdict> verdicts) noexcept
		{
			if (!std::ranges::all_of(verdicts, valid_authority_verdict))
				return authority_verdict::rejected;
			if (std::ranges::find(verdicts, authority_verdict::revoked) != verdicts.end())
				return authority_verdict::revoked;
			if (std::ranges::find(verdicts, authority_verdict::rejected) != verdicts.end())
				return authority_verdict::rejected;
			if (std::ranges::find(verdicts, authority_verdict::absent) != verdicts.end())
				return authority_verdict::absent;
			if (std::ranges::find(verdicts, authority_verdict::unverified) != verdicts.end())
				return authority_verdict::unverified;
			return authority_verdict::verified;
		}

		[[nodiscard]] static provider_certification_authority
		load_provider(const provider_installation_artifact& installation,
					  const certification_registry_document& registry,
					  const std::string_view registry_identity,
					  const authority_verdict registry_verdict,
					  const std::optional<std::uint64_t> trusted_epoch,
					  const trusted_signature_verifier& signature_verifier)
		{
			provider_certification_authority output;
			output.registry_id = registry.schema;
			output.registry_semantic_identity = registry_identity;
			if (has_unbounded_installation(installation))
			{
				output.verdict = authority_verdict::rejected;
				return output;
			}
			const auto& discovery = installation.discovery();
			if (auto valid = discovery.validate(); !valid)
			{
				output.verdict = authority_verdict::rejected;
				return output;
			}
			const auto selected_count = std::ranges::count_if(
				discovery.decisions(), &sdk::provider::provider_candidate_decision::selected);
			if (selected_count != 1U || discovery.fallback_used())
			{
				output.verdict = authority_verdict::rejected;
				return output;
			}
			const auto& discovered = discovery.selected_candidate();
			const auto selected_decision =
				std::ranges::find_if(discovery.decisions(),
									 [](const sdk::provider::provider_candidate_decision& decision)
									 {
										 return decision.selected;
									 });
			output.candidate_id = selected_decision->candidate_digest;
			if (!semantic_digest_value(output.candidate_id) || !discovered.authoritative_path ||
				!discovered.validation_error.empty())
			{
				output.verdict = authority_verdict::rejected;
				return output;
			}
			const auto& manifest = discovered.description;
			const auto* execution = installation.execution();
			const auto& sandbox = execution == nullptr ? discovered.sandbox : execution->sandbox();
			const auto manifest_digest = content_digest_text(manifest.canonical_json());
			const auto measured_binary_digest = execution == nullptr
				? manifest.provider_binary_digest
				: std::string{execution->measured_executable_digest()};
			if (execution != nullptr)
			{
				output.execution_semantic_identity = execution->execution_semantic_identity();
				output.selection_semantic_identity = execution->selection_semantic_identity();
			}
			auto selection_identity = provider_selection_authority_identity(discovery);
			output.execution_verdict = execution == nullptr
				? authority_verdict::unverified
				: (!std::holds_alternative<product_error>(selection_identity) &&
						   execution->candidate_identity() == output.candidate_id &&
						   execution->selection_semantic_identity() ==
							   std::get<std::string>(selection_identity) &&
						   semantic_digest_value(execution->execution_semantic_identity()) &&
						   execution->provider().canonical_json() == manifest.canonical_json() &&
						   execution->measured_executable_digest() ==
							   manifest.provider_binary_digest &&
						   execution->sandbox().canonical_form() ==
							   discovered.sandbox.canonical_form()
					   ? authority_verdict::verified
					   : authority_verdict::rejected);
			auto sandbox_policy = sdk::provider::resolve_sandbox_policy(sandbox.policy_digest);
			if (!sandbox_policy)
			{
				output.execution_verdict = authority_verdict::rejected;
				output.verdict = authority_verdict::rejected;
				return output;
			}
			// Certificate, issuer, and revocation entries are authenticated only as members
			// of a verified registry.  An unavailable or rejected registry must not turn an
			// inner JSON revocation claim into an authoritative negative product verdict.
			if (registry_verdict != authority_verdict::verified)
			{
				output.verdict = combine_verdicts({registry_verdict, output.execution_verdict});
				return output;
			}
			const auto matches_identity = [&](const provider_certificate& certificate)
			{
				return certificate.subject.provider_id == manifest.provider_id &&
					certificate.subject.provider_version == manifest.provider_version.string();
			};
			const auto matching_count =
				std::ranges::count_if(registry.certificates, matches_identity);
			if (matching_count == 0)
			{
				output.verdict =
					combine_verdicts({authority_verdict::absent, output.execution_verdict});
				return output;
			}
			if (matching_count != 1)
			{
				output.verdict = authority_verdict::rejected;
				return output;
			}
			const auto& certificate =
				*std::ranges::find_if(registry.certificates, matches_identity);
			output.provider_id = manifest.provider_id;
			output.provider_version = manifest.provider_version.string();
			output.package_identity = manifest.package_identity;
			output.provider_manifest_digest = manifest_digest;
			output.provider_binary_digest = measured_binary_digest;
			output.provider_semantic_contract_digest = manifest.provider_semantic_contract_digest;
			output.protocol_major = manifest.protocol.major;
			output.protocol_minor = manifest.protocol.maximum_minor;
			output.features = manifest.protocol.required_features;
			output.features.insert(output.features.end(),
								   manifest.protocol.optional_features.begin(),
								   manifest.protocol.optional_features.end());
			for (const auto& relation : manifest.offered_relations)
			{
				auto normalized = provider_relation_offer_to_descriptor(relation);
				if (!normalized)
				{
					output.verdict = authority_verdict::rejected;
					return output;
				}
				output.relations.push_back(std::move(*normalized));
			}
			std::ranges::sort(output.relations);
			if (std::ranges::adjacent_find(output.relations) != output.relations.end())
			{
				output.verdict = authority_verdict::rejected;
				return output;
			}
			output.interpretations = manifest.interpretation_domains;
			output.platform_tuples = manifest.platform_tuples;
			output.sandbox_assurance = std::string{sandbox_assurance_name(sandbox.achieved)};
			output.sandbox_policy_digest = content_digest_text(sandbox_policy->canonical_form());
			output.registry_sequence = certificate.registry_sequence;
			output.certificate_id = certificate.id;
			output.signature_digest = certificate.certificate_signature_digest;

			const auto issuer = std::ranges::find(
				registry.issuers, certificate.issuer_id, &certification_issuer::id);
			const certification_trust_anchor* anchor{};
			if (issuer != registry.issuers.end())
			{
				const auto found_anchor = std::ranges::find(registry.trust_anchors,
															issuer->trust_anchor_id,
															&certification_trust_anchor::id);
				if (found_anchor != registry.trust_anchors.end())
					anchor = &*found_anchor;
			}
			output.trust_anchor_id = anchor == nullptr ? std::optional<std::string>{}
													   : std::optional<std::string>{anchor->id};
			output.trust_anchor_verdict = issuer != registry.issuers.end() && anchor != nullptr &&
					issuer->production_scope && anchor->production_use
				? authority_verdict::verified
				: authority_verdict::rejected;
			const auto provider_namespace_valid = issuer != registry.issuers.end() &&
				provider_namespace_authorized(manifest, *issuer);
			if (issuer != registry.issuers.end())
				for (const auto& qualification : certificate.qualifications)
					if (qualification_authorized(manifest, qualification, *issuer))
						output.certified_qualifications.push_back(qualification);

			const auto revoked = std::ranges::any_of(
				registry.revocations,
				[&](const certification_revocation& revocation)
				{
					return revocation.certificate_id == certificate.id &&
						revocation.effective_sequence <= registry.update_sequence;
				});
			output.revocation_verdict = revoked
				? authority_verdict::revoked
				: (certificate.registry_sequence <= registry.update_sequence
					   ? authority_verdict::verified
					   : authority_verdict::rejected);

			const auto exact_subject = certificate.subject.provider_id == manifest.provider_id &&
				certificate.subject.provider_version == manifest.provider_version.string() &&
				certificate.subject.package_identity == manifest.package_identity &&
				certificate.subject.publisher == manifest.publisher &&
				certificate.subject.manifest_digest == manifest_digest &&
				certificate.subject.binary_digest == measured_binary_digest &&
				certificate.subject.binary_digest == manifest.provider_binary_digest &&
				certificate.subject.semantic_contract_digest ==
					manifest.provider_semantic_contract_digest;
			const auto valid_epoch = trusted_epoch &&
				certificate.not_before_epoch <= *trusted_epoch &&
				*trusted_epoch <= certificate.not_after_epoch;
			if (issuer != registry.issuers.end())
			{
				const auto subject_digest = certificate_subject_digest(certificate.subject);
				const auto verified =
					signature_verifier.verify("provider-certificate",
											  issuer->id,
											  subject_digest,
											  certificate.certificate_signature_digest);
				output.signature_verdict =
					normalized_signature_verdict(verified,
												 subject_digest,
												 certificate.certificate_signature_digest,
												 issuer->public_key_fingerprint);
			}
			else
				output.signature_verdict = authority_verdict::rejected;
			const auto subject_verdict = !exact_subject || !provider_namespace_valid
				? authority_verdict::rejected
				: (!trusted_epoch
					   ? authority_verdict::unverified
					   : (valid_epoch ? authority_verdict::verified : authority_verdict::rejected));
			output.verdict = combine_verdicts({registry_verdict,
											   output.execution_verdict,
											   output.signature_verdict,
											   output.trust_anchor_verdict,
											   output.revocation_verdict,
											   subject_verdict});
			return output;
		}

		[[nodiscard]] static bool
		has_unbounded_installation(const provider_installation_artifact& installation,
								   std::size_t* aggregate_bytes = nullptr,
								   std::size_t* aggregate_nodes = nullptr)
		{
			std::size_t bytes = aggregate_bytes == nullptr ? 0U : *aggregate_bytes;
			std::size_t nodes = aggregate_nodes == nullptr ? 0U : *aggregate_nodes;
			const auto bounded = [&](const std::string& value)
			{
				if (!strict_id(value) || value.size() > maximum_project_bytes - bytes ||
					nodes == maximum_authority_node_count)
					return false;
				bytes += value.size();
				++nodes;
				return true;
			};
			const auto bounded_optional_text = [&](const std::string& value)
			{
				return value.empty() || bounded(value);
			};
			const auto& discovery = installation.discovery();
			const auto& discovered = discovery.selected_candidate();
			const auto& manifest = discovered.description;
			const auto& sandbox = discovered.sandbox;
			const auto strings_bounded = [&](const std::vector<std::string>& values)
			{
				if (values.size() > maximum_json_collection_count ||
					values.size() > maximum_authority_node_count - nodes)
					return false;
				nodes += values.size();
				return std::ranges::all_of(values, bounded);
			};
			if (discovery.decisions().size() > maximum_json_collection_count ||
				discovery.decisions().size() > maximum_authority_node_count - nodes)
				return true;
			nodes += discovery.decisions().size();
			for (const auto& decision : discovery.decisions())
				if (!bounded(decision.provider_id) ||
					!bounded(decision.provider_version.string()) ||
					!bounded(decision.binary_digest) || !bounded(decision.candidate_digest) ||
					!bounded(decision.reason))
					return true;
			const auto invalid = !bounded(manifest.provider_id) ||
				!bounded(manifest.provider_version.string()) ||
				!bounded(manifest.package_identity) || !bounded(manifest.publisher) ||
				!bounded(manifest.license) ||
				(manifest.signature && !bounded(*manifest.signature)) ||
				!bounded(manifest.provider_binary_digest) ||
				!bounded(manifest.provider_semantic_contract_digest) ||
				!bounded(manifest.invalidation_contract) ||
				!bounded(manifest.determinism_contract) || !bounded(manifest.resource_class) ||
				!bounded(manifest.sandbox_minimum) || !bounded(manifest.task_input_stage) ||
				!bounded(manifest.task_output_stage) || !bounded(sandbox.platform) ||
				!bounded(sandbox.policy_digest) || !bounded(sandbox.evidence_digest) ||
				!strings_bounded(manifest.protocol.required_features) ||
				!strings_bounded(manifest.protocol.optional_features) ||
				!strings_bounded(manifest.platform_tuples) ||
				!strings_bounded(manifest.offered_relations) ||
				!strings_bounded(manifest.required_relations) ||
				!strings_bounded(manifest.interpretation_domains) ||
				!strings_bounded(manifest.requested_qualifications) ||
				!strings_bounded(manifest.trust_flags) ||
				!strings_bounded(discovered.executable_argv) ||
				!strings_bounded(discovered.certified_qualifications) ||
				!bounded_optional_text(discovered.validation_error) ||
				!strings_bounded(sandbox.mechanisms);
			if (invalid)
				return true;
			if (const auto* execution = installation.execution())
			{
				const auto& execution_sandbox = execution->sandbox();
				if (!bounded(std::string{execution->measured_executable_digest()}) ||
					!bounded(std::string{execution->execution_semantic_identity()}) ||
					!bounded(std::string{execution->candidate_identity()}) ||
					!bounded(std::string{execution->selection_semantic_identity()}) ||
					!bounded(execution_sandbox.platform) ||
					!bounded(execution_sandbox.policy_digest) ||
					!bounded(execution_sandbox.evidence_digest) ||
					!strings_bounded(execution_sandbox.mechanisms))
					return true;
			}
			if (aggregate_bytes != nullptr)
				*aggregate_bytes = bytes;
			if (aggregate_nodes != nullptr)
				*aggregate_nodes = nodes;
			return false;
		}

		[[nodiscard]] static bool bounded_provider_installations(
			const std::vector<provider_installation_artifact>& installations)
		{
			std::size_t bytes{};
			std::size_t nodes{};
			return std::ranges::all_of(installations,
									   [&](const provider_installation_artifact& installation)
									   {
										   return !has_unbounded_installation(
											   installation, &bytes, &nodes);
									   });
		}

		enum class namespace_kind : std::uint8_t
		{
			relation,
			provider,
			interpretation,
		};

		struct namespace_grant
		{
			namespace_kind kind;
			std::string_view prefix;
			std::string_view owner;
			std::string_view authority;
			std::string_view minimum_qualification;
			bool active;
		};

		[[nodiscard]] static std::optional<std::string_view>
		compiled_namespace_grant(const namespace_kind kind,
								 const std::string_view value,
								 const std::string_view publisher,
								 const std::string_view provider_id)
		{
			// Typed projection of cxxlens.namespace-registry.v1.  This is product
			// namespace policy, not a source-byte digest or caller assertion.
			static constexpr std::array grants{
				namespace_grant{namespace_kind::relation,
								"build.",
								"cxxlens.project",
								"standard",
								"canonical-semantic-qualified",
								true},
				namespace_grant{namespace_kind::relation,
								"source.",
								"cxxlens.project",
								"standard",
								"canonical-semantic-qualified",
								true},
				namespace_grant{namespace_kind::relation,
								"cc.",
								"cxxlens.project",
								"standard",
								"canonical-semantic-qualified",
								true},
				namespace_grant{namespace_kind::relation,
								"core.",
								"cxxlens.project",
								"standard",
								"canonical-semantic-qualified",
								true},
				namespace_grant{namespace_kind::relation,
								"frontend.clang22.",
								"cxxlens.clang22.reference",
								"provider-owned",
								"schema-conformant",
								true},
				namespace_grant{namespace_kind::relation,
								"company.",
								"cxxlens.conformance",
								"custom",
								"schema-conformant",
								false},
				namespace_grant{namespace_kind::provider,
								"cxxlens.",
								"cxxlens.project",
								"standard",
								"canonical-semantic-qualified",
								true},
				namespace_grant{namespace_kind::interpretation,
								"cc.",
								"cxxlens.project",
								"standard",
								"canonical-semantic-qualified",
								true},
				namespace_grant{namespace_kind::interpretation,
								"core.",
								"cxxlens.project",
								"standard",
								"canonical-semantic-qualified",
								true},
			};
			const namespace_grant* selected{};
			for (const auto& grant : grants)
				if (grant.kind == kind && value.starts_with(grant.prefix) &&
					(selected == nullptr || grant.prefix.size() > selected->prefix.size()))
					selected = &grant;
			if (selected != nullptr)
			{
				if (!selected->active)
					return std::nullopt;
				if (selected->authority == "standard")
					return selected->owner == publisher
						? std::optional<std::string_view>{selected->minimum_qualification}
						: std::nullopt;
				return selected->owner == provider_id
					? std::optional<std::string_view>{selected->minimum_qualification}
					: std::nullopt;
			}
			if (kind == namespace_kind::relation || kind == namespace_kind::interpretation)
			{
				const auto derived =
					"provider." + std::string{publisher} + "." + std::string{provider_id} + ".";
				return value.starts_with(derived)
					? std::optional<std::string_view>{"schema-conformant"}
					: std::nullopt;
			}
			return std::nullopt;
		}

		[[nodiscard]] static bool issuer_namespace_grant(const certification_issuer& issuer,
														 const std::string_view value)
		{
			return std::ranges::any_of(issuer.namespace_prefixes,
									   [&](const std::string& prefix)
									   {
										   return value.starts_with(prefix);
									   });
		}

		[[nodiscard]] static bool
		provider_namespace_authorized(const sdk::provider::manifest& manifest,
									  const certification_issuer& issuer)
		{
			const auto provider_level = compiled_namespace_grant(namespace_kind::provider,
																 manifest.provider_id,
																 manifest.publisher,
																 manifest.provider_id);
			if (!provider_level ||
				std::ranges::find(issuer.allowed_qualifications, *provider_level) ==
					issuer.allowed_qualifications.end() ||
				!issuer_namespace_grant(issuer, manifest.provider_id))
				return false;
			for (const auto& relation : manifest.offered_relations)
			{
				const auto relation_name = provider_relation_namespace_name(relation);
				if (!compiled_namespace_grant(namespace_kind::relation,
											  relation_name,
											  manifest.publisher,
											  manifest.provider_id) ||
					!issuer_namespace_grant(issuer, relation_name))
					return false;
			}
			return std::ranges::all_of(manifest.interpretation_domains,
									   [&](const std::string& interpretation)
									   {
										   return compiled_namespace_grant(
													  namespace_kind::interpretation,
													  interpretation,
													  manifest.publisher,
													  manifest.provider_id)
													  .has_value() &&
											   issuer_namespace_grant(issuer, interpretation);
									   });
		}

		[[nodiscard]] static bool
		qualification_authorized(const sdk::provider::manifest& manifest,
								 const certification_qualification& qualification,
								 const certification_issuer& issuer)
		{
			if (std::ranges::find(manifest.offered_relations, qualification.relation) ==
					manifest.offered_relations.end() ||
				std::ranges::find(manifest.interpretation_domains, qualification.interpretation) ==
					manifest.interpretation_domains.end())
				return false;
			const auto relation_name = provider_relation_namespace_name(qualification.relation);
			const auto relation_level = compiled_namespace_grant(
				namespace_kind::relation, relation_name, manifest.publisher, manifest.provider_id);
			const auto interpretation_level =
				compiled_namespace_grant(namespace_kind::interpretation,
										 qualification.interpretation,
										 manifest.publisher,
										 manifest.provider_id);
			return relation_level && interpretation_level &&
				qualification.level == *relation_level &&
				std::ranges::find(issuer.allowed_qualifications, qualification.level) !=
				issuer.allowed_qualifications.end() &&
				issuer_namespace_grant(issuer, relation_name) &&
				issuer_namespace_grant(issuer, qualification.interpretation) &&
				!qualification.toolchains.empty() && !qualification.platforms.empty();
		}
	};

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
			static constexpr std::array<std::string_view, 13U> fields{
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
				"sandbox",
				"trust"};
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

			const auto* trust = required_member(candidate_value, "trust", "trust", error);
			static constexpr std::array<std::string_view, 6U> trust_fields{"state",
																		   "registry_sequence",
																		   "certificate_id",
																		   "trust_anchor_id",
																		   "signature_digest",
																		   "revocation"};
			std::string trust_state_text;
			const auto* registry_sequence =
				trust == nullptr ? nullptr : trust->member("registry_sequence");
			if (trust == nullptr || !has_only(*trust, trust_fields, error) ||
				!text_member(*trust, "state", true, trust_state_text, error) ||
				registry_sequence == nullptr ||
				registry_sequence->kind != json_kind::unsigned_integer)
				return product_error{
					"doctor.project-invalid", "provider_candidates", "invalid-trust"};
			candidate.trust.registry_sequence = registry_sequence->unsigned_integer;
			if (trust_state_text == "verified")
				candidate.trust.state = trust_state::verified;
			else if (trust_state_text == "unknown")
				candidate.trust.state = trust_state::unknown;
			else if (trust_state_text == "rejected")
				candidate.trust.state = trust_state::rejected;
			else
				return product_error{
					"doctor.project-invalid", "provider_candidates", "invalid-trust-state"};

			auto nullable_text = [&](const std::string_view name,
									 std::optional<std::string>& target) -> bool
			{
				const auto* value = trust->member(name);
				if (value == nullptr)
				{
					error = "missing-" + std::string{name};
					return false;
				}
				if (value->kind == json_kind::null_value)
				{
					target.reset();
					return true;
				}
				if (value->kind != json_kind::string || !strict_id(value->string))
				{
					error = "invalid-" + std::string{name};
					return false;
				}
				target = value->string;
				return true;
			};
			if (!nullable_text("certificate_id", candidate.trust.certificate_id) ||
				!nullable_text("trust_anchor_id", candidate.trust.trust_anchor_id) ||
				!nullable_text("signature_digest", candidate.trust.signature_digest))
				return product_error{
					"doctor.project-invalid", "provider_candidates", std::move(error)};
			if (candidate.trust.signature_digest &&
				!digest_value(*candidate.trust.signature_digest))
				return product_error{
					"doctor.project-invalid", "provider_candidates", "invalid-signature-digest"};

			const auto* revocation = required_member(*trust, "revocation", "revocation", error);
			static constexpr std::array<std::string_view, 3U> revocation_fields{
				"state", "effective_sequence", "reason"};
			std::string revocation_state_text;
			if (revocation == nullptr || !has_only(*revocation, revocation_fields, error) ||
				!text_member(*revocation, "state", true, revocation_state_text, error))
				return product_error{
					"doctor.project-invalid", "provider_candidates", "invalid-revocation"};
			if (revocation_state_text == "not-revoked")
				candidate.trust.revocation.state = revocation_state::not_revoked;
			else if (revocation_state_text == "revoked")
				candidate.trust.revocation.state = revocation_state::revoked;
			else if (revocation_state_text == "unknown")
				candidate.trust.revocation.state = revocation_state::unknown;
			else
				return product_error{
					"doctor.project-invalid", "provider_candidates", "invalid-revocation-state"};

			const auto* effective_sequence = revocation->member("effective_sequence");
			const auto* revocation_reason = revocation->member("reason");
			if (effective_sequence == nullptr || revocation_reason == nullptr)
				return product_error{
					"doctor.project-invalid", "provider_candidates", "incomplete-revocation"};
			if (effective_sequence->kind == json_kind::unsigned_integer)
				candidate.trust.revocation.effective_sequence =
					effective_sequence->unsigned_integer;
			else if (effective_sequence->kind != json_kind::null_value)
				return product_error{
					"doctor.project-invalid", "provider_candidates", "invalid-revocation-sequence"};
			if (revocation_reason->kind == json_kind::string &&
				strict_id(revocation_reason->string))
				candidate.trust.revocation.reason = revocation_reason->string;
			else if (revocation_reason->kind != json_kind::null_value)
				return product_error{
					"doctor.project-invalid", "provider_candidates", "invalid-revocation-reason"};
			const bool revoked = candidate.trust.revocation.state == revocation_state::revoked;
			const bool has_revocation_values =
				candidate.trust.revocation.effective_sequence.has_value() &&
				candidate.trust.revocation.reason.has_value();
			const bool has_partial_revocation_values =
				candidate.trust.revocation.effective_sequence.has_value() ||
				candidate.trust.revocation.reason.has_value();
			if ((revoked && !has_revocation_values) || (!revoked && has_partial_revocation_values))
				return product_error{
					"doctor.project-invalid", "provider_candidates", "inconsistent-revocation"};
			if (candidate.trust.state == trust_state::verified &&
				(!candidate.trust.certificate_id || !candidate.trust.trust_anchor_id ||
				 !candidate.trust.signature_digest ||
				 candidate.trust.revocation.state != revocation_state::not_revoked))
				return product_error{
					"doctor.project-invalid", "provider_candidates", "inconsistent-verified-trust"};
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
