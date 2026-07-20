#include "materialization_json.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <utility>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		constexpr std::string_view json_error_code = "materialization.json-invalid";
		constexpr std::size_t contract_maximum_depth = 64U;

		[[nodiscard]] std::optional<std::size_t>
		invalid_utf8_offset(const std::string_view input) noexcept
		{
			std::size_t index{};
			while (index < input.size())
			{
				const auto first = static_cast<unsigned char>(input[index]);
				if (first <= 0x7fU)
				{
					++index;
					continue;
				}

				std::size_t width{};
				std::uint32_t code_point{};
				std::uint32_t minimum{};
				if (first >= 0xc2U && first <= 0xdfU)
				{
					width = 2U;
					code_point = first & 0x1fU;
					minimum = 0x80U;
				}
				else if (first >= 0xe0U && first <= 0xefU)
				{
					width = 3U;
					code_point = first & 0x0fU;
					minimum = 0x800U;
				}
				else if (first >= 0xf0U && first <= 0xf4U)
				{
					width = 4U;
					code_point = first & 0x07U;
					minimum = 0x10000U;
				}
				else
					return index;

				if (width > input.size() - index)
					return index;
				for (std::size_t offset = 1U; offset < width; ++offset)
				{
					const auto continuation = static_cast<unsigned char>(input[index + offset]);
					if ((continuation & 0xc0U) != 0x80U)
						return index + offset;
					code_point = (code_point << 6U) | (continuation & 0x3fU);
				}
				if (code_point < minimum || code_point > 0x10ffffU ||
					(code_point >= 0xd800U && code_point <= 0xdfffU))
					return index;
				index += width;
			}
			return std::nullopt;
		}

		[[nodiscard]] sdk::error json_error(const std::string_view reason, const std::size_t offset)
		{
			return {std::string{json_error_code},
					"input",
					std::string{reason} + ":byte=" + std::to_string(offset)};
		}

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

		class strict_json_parser
		{
		  public:
			strict_json_parser(const std::string_view input, const json_limits& limits)
				: input_{input}, limits_{limits}
			{
			}

			[[nodiscard]] sdk::result<json_value> parse()
			{
				auto root = parse_value(0U);
				if (!root)
					return root;
				space();
				if (position_ != input_.size())
					return sdk::unexpected(json_error("trailing-data", position_));
				if (root->as_object() == nullptr)
					return sdk::unexpected(json_error("top-level-object-required", 0U));
				return root;
			}

		  private:
			[[nodiscard]] sdk::result<json_value> parse_value(const std::size_t depth)
			{
				space();
				if (position_ >= input_.size())
					return sdk::unexpected(json_error("value-missing", position_));
				if (value_count_ >= limits_.max_total_values)
					return sdk::unexpected(json_error("value-count-limit", position_));
				++value_count_;

				switch (input_[position_])
				{
					case '{':
						if (depth >= std::min(limits_.max_depth, contract_maximum_depth))
							return sdk::unexpected(json_error("depth-limit", position_));
						return parse_object(depth + 1U);
					case '[':
						if (depth >= std::min(limits_.max_depth, contract_maximum_depth))
							return sdk::unexpected(json_error("depth-limit", position_));
						return parse_array(depth + 1U);
					case '"':
					{
						auto decoded = parse_string();
						if (!decoded)
							return sdk::unexpected(std::move(decoded.error()));
						return json_value::string(std::move(*decoded));
					}
					case 't':
						return literal("true", json_value::boolean(true));
					case 'f':
						return literal("false", json_value::boolean(false));
					case 'n':
						return literal("null", json_value::null());
					default:
						return parse_number();
				}
			}

			[[nodiscard]] sdk::result<json_value> parse_object(const std::size_t depth)
			{
				++position_;
				json_value::object_type output;
				space();
				if (consume('}'))
					return json_value::object(std::move(output));

				while (true)
				{
					const auto key_offset = position_;
					auto key = parse_string();
					if (!key)
						return sdk::unexpected(std::move(key.error()));
					if (output.contains(*key))
						return sdk::unexpected(json_error("duplicate-member", key_offset));
					if (output.size() >= limits_.max_object_members)
						return sdk::unexpected(json_error("object-member-limit", key_offset));
					space();
					if (!consume(':'))
						return sdk::unexpected(json_error("object-colon", position_));
					auto child = parse_value(depth);
					if (!child)
						return sdk::unexpected(std::move(child.error()));
					output.emplace(std::move(*key), std::move(*child));
					space();
					if (consume('}'))
						break;
					if (!consume(','))
						return sdk::unexpected(json_error("object-comma", position_));
					space();
				}
				return json_value::object(std::move(output));
			}

			[[nodiscard]] sdk::result<json_value> parse_array(const std::size_t depth)
			{
				++position_;
				json_value::array_type output;
				space();
				if (consume(']'))
					return json_value::array(std::move(output));

				while (true)
				{
					if (output.size() >= limits_.max_array_elements)
						return sdk::unexpected(json_error("array-element-limit", position_));
					auto child = parse_value(depth);
					if (!child)
						return sdk::unexpected(std::move(child.error()));
					output.push_back(std::move(*child));
					space();
					if (consume(']'))
						break;
					if (!consume(','))
						return sdk::unexpected(json_error("array-comma", position_));
				}
				return json_value::array(std::move(output));
			}

			[[nodiscard]] bool can_append(const std::size_t current,
										  const std::size_t amount) const noexcept
			{
				return amount <= limits_.max_string_bytes &&
					current <= limits_.max_string_bytes - amount &&
					amount <= limits_.max_total_string_bytes &&
					total_string_bytes_ <= limits_.max_total_string_bytes - amount &&
					current <= limits_.max_total_string_bytes - amount - total_string_bytes_;
			}

			[[nodiscard]] sdk::result<std::string> parse_string()
			{
				const auto begin = position_;
				if (!consume('"'))
					return sdk::unexpected(json_error("string-required", position_));
				std::string output;
				while (position_ < input_.size())
				{
					const auto byte = static_cast<unsigned char>(input_[position_++]);
					if (byte == '"')
					{
						total_string_bytes_ += output.size();
						return output;
					}
					if (byte < 0x20U)
						return sdk::unexpected(json_error("raw-control-character", position_ - 1U));
					if (byte != '\\')
					{
						if (!can_append(output.size(), 1U))
							return sdk::unexpected(json_error("string-byte-limit", begin));
						output.push_back(static_cast<char>(byte));
						continue;
					}

					if (position_ >= input_.size())
						return sdk::unexpected(json_error("short-escape", position_));
					switch (const auto escaped = input_[position_++])
					{
						case '"':
						case '\\':
						case '/':
							if (!append_ascii(output, escaped, begin))
								return sdk::unexpected(json_error("string-byte-limit", begin));
							break;
						case 'b':
							if (!append_ascii(output, '\b', begin))
								return sdk::unexpected(json_error("string-byte-limit", begin));
							break;
						case 'f':
							if (!append_ascii(output, '\f', begin))
								return sdk::unexpected(json_error("string-byte-limit", begin));
							break;
						case 'n':
							if (!append_ascii(output, '\n', begin))
								return sdk::unexpected(json_error("string-byte-limit", begin));
							break;
						case 'r':
							if (!append_ascii(output, '\r', begin))
								return sdk::unexpected(json_error("string-byte-limit", begin));
							break;
						case 't':
							if (!append_ascii(output, '\t', begin))
								return sdk::unexpected(json_error("string-byte-limit", begin));
							break;
						case 'u':
						{
							auto code_point = hexadecimal_quad();
							if (!code_point)
								return sdk::unexpected(std::move(code_point.error()));
							if (*code_point >= 0xd800U && *code_point <= 0xdbffU)
							{
								if (position_ + 2U > input_.size() || input_[position_] != '\\' ||
									input_[position_ + 1U] != 'u')
									return sdk::unexpected(json_error("surrogate-pair", position_));
								position_ += 2U;
								auto low = hexadecimal_quad();
								if (!low || *low < 0xdc00U || *low > 0xdfffU)
									return sdk::unexpected(json_error("surrogate-pair", position_));
								*code_point =
									0x10000U + ((*code_point - 0xd800U) << 10U) + (*low - 0xdc00U);
							}
							else if (*code_point >= 0xdc00U && *code_point <= 0xdfffU)
								return sdk::unexpected(json_error("surrogate-pair", position_));
							std::string encoded;
							append_utf8(encoded, *code_point);
							if (!can_append(output.size(), encoded.size()))
								return sdk::unexpected(json_error("string-byte-limit", begin));
							output += encoded;
							break;
						}
						default:
							return sdk::unexpected(
								json_error("unsupported-escape", position_ - 1U));
					}
				}
				return sdk::unexpected(json_error("unterminated-string", begin));
			}

			[[nodiscard]] bool
			append_ascii(std::string& output, const char value, const std::size_t) const
			{
				if (!can_append(output.size(), 1U))
					return false;
				output.push_back(value);
				return true;
			}

			[[nodiscard]] sdk::result<std::uint32_t> hexadecimal_quad()
			{
				const auto begin = position_;
				if (position_ + 4U > input_.size())
					return sdk::unexpected(json_error("short-unicode-escape", begin));
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
						return sdk::unexpected(json_error("unicode-escape", position_ - 1U));
				}
				return output;
			}

			[[nodiscard]] sdk::result<json_value> parse_number()
			{
				const auto begin = position_;
				const bool negative = consume('-');
				const auto integer_begin = position_;
				while (position_ < input_.size() && input_[position_] >= '0' &&
					   input_[position_] <= '9')
					++position_;
				const auto integer_end = position_;
				if (integer_begin == integer_end ||
					(integer_end - integer_begin > 1U && input_[integer_begin] == '0'))
					return sdk::unexpected(json_error("number-syntax", begin));

				std::size_t fraction_begin = position_;
				std::size_t fraction_end = position_;
				if (consume('.'))
				{
					fraction_begin = position_;
					while (position_ < input_.size() && input_[position_] >= '0' &&
						   input_[position_] <= '9')
						++position_;
					fraction_end = position_;
					if (fraction_begin == fraction_end)
						return sdk::unexpected(json_error("number-syntax", begin));
				}

				bool exponent_negative{};
				bool exponent_overflow{};
				std::uint64_t exponent_magnitude{};
				if (position_ < input_.size() &&
					(input_[position_] == 'e' || input_[position_] == 'E'))
				{
					++position_;
					if (position_ < input_.size() &&
						(input_[position_] == '+' || input_[position_] == '-'))
						exponent_negative = input_[position_++] == '-';
					const auto exponent_begin = position_;
					while (position_ < input_.size() && input_[position_] >= '0' &&
						   input_[position_] <= '9')
						++position_;
					if (exponent_begin == position_)
						return sdk::unexpected(json_error("number-syntax", begin));
					const auto converted = std::from_chars(input_.data() + exponent_begin,
														   input_.data() + position_,
														   exponent_magnitude);
					exponent_overflow = converted.ec == std::errc::result_out_of_range;
					if (converted.ec != std::errc{} && !exponent_overflow)
						return sdk::unexpected(json_error("number-syntax", begin));
				}

				const auto integer_count = integer_end - integer_begin;
				const auto fraction_count = fraction_end - fraction_begin;
				const auto base_digit_count = integer_count + fraction_count;
				const auto digit_at = [&](const std::size_t index) -> char
				{
					return index < integer_count ? input_[integer_begin + index]
												 : input_[fraction_begin + index - integer_count];
				};
				bool all_zero = true;
				for (std::size_t index = 0U; index < base_digit_count; ++index)
					all_zero = all_zero && digit_at(index) == '0';
				if (all_zero)
					return json_value::unsigned_integer(0U);

				const auto overflow_reason =
					negative ? "signed-integer-overflow" : "unsigned-integer-overflow";
				if (exponent_overflow)
					return sdk::unexpected(json_error(
						exponent_negative ? "non-integer-number" : overflow_reason, begin));

				std::size_t retained_base_digits = base_digit_count;
				std::uint64_t appended_zero_count{};
				if (exponent_negative)
				{
					if (exponent_magnitude > base_digit_count ||
						fraction_count >
							base_digit_count - static_cast<std::size_t>(exponent_magnitude))
						return sdk::unexpected(json_error("non-integer-number", begin));
					const auto removed =
						fraction_count + static_cast<std::size_t>(exponent_magnitude);
					retained_base_digits -= removed;
				}
				else if (exponent_magnitude >= fraction_count)
					appended_zero_count = exponent_magnitude - fraction_count;
				else
					retained_base_digits -=
						fraction_count - static_cast<std::size_t>(exponent_magnitude);

				for (std::size_t index = retained_base_digits; index < base_digit_count; ++index)
					if (digit_at(index) != '0')
						return sdk::unexpected(json_error("non-integer-number", begin));

				std::size_t first_nonzero{};
				while (first_nonzero < retained_base_digits && digit_at(first_nonzero) == '0')
					++first_nonzero;
				if (first_nonzero == retained_base_digits)
					return json_value::unsigned_integer(0U);
				const auto retained_significant = retained_base_digits - first_nonzero;
				if (appended_zero_count > 20U ||
					retained_significant > 20U - static_cast<std::size_t>(appended_zero_count))
					return sdk::unexpected(json_error(overflow_reason, begin));

				const auto limit = negative
					? std::uint64_t{std::numeric_limits<std::int64_t>::max()} + 1U
					: std::numeric_limits<std::uint64_t>::max();
				std::uint64_t magnitude{};
				const auto accumulate = [&](const unsigned int digit) -> bool
				{
					if (magnitude > (limit - digit) / 10U)
						return false;
					magnitude = magnitude * 10U + digit;
					return true;
				};
				for (std::size_t index = first_nonzero; index < retained_base_digits; ++index)
					if (!accumulate(static_cast<unsigned int>(digit_at(index) - '0')))
						return sdk::unexpected(json_error(overflow_reason, begin));
				for (std::uint64_t index = 0U; index < appended_zero_count; ++index)
					if (!accumulate(0U))
						return sdk::unexpected(json_error(overflow_reason, begin));

				if (!negative)
					return json_value::unsigned_integer(magnitude);
				if (magnitude == std::uint64_t{std::numeric_limits<std::int64_t>::max()} + 1U)
					return json_value::signed_integer(std::numeric_limits<std::int64_t>::min());
				return json_value::signed_integer(-static_cast<std::int64_t>(magnitude));
			}

			[[nodiscard]] sdk::result<json_value> literal(const std::string_view expected,
														  json_value value)
			{
				if (input_.substr(position_, expected.size()) != expected)
					return sdk::unexpected(json_error("literal", position_));
				position_ += expected.size();
				return value;
			}

			void space() noexcept
			{
				while (position_ < input_.size() &&
					   (input_[position_] == ' ' || input_[position_] == '\t' ||
						input_[position_] == '\n' || input_[position_] == '\r'))
					++position_;
			}

			[[nodiscard]] bool consume(const char expected) noexcept
			{
				if (position_ < input_.size() && input_[position_] == expected)
				{
					++position_;
					return true;
				}
				return false;
			}

			std::string_view input_;
			const json_limits& limits_;
			std::size_t position_{};
			std::size_t value_count_{};
			std::size_t total_string_bytes_{};
		};

		void append_hex_escape(std::string& output, const unsigned char byte)
		{
			constexpr char digits[] = "0123456789abcdef";
			output += "\\u00";
			output.push_back(digits[(byte >> 4U) & 0x0fU]);
			output.push_back(digits[byte & 0x0fU]);
		}

		void append_json_string(std::string& output, const std::string_view value)
		{
			output.push_back('"');
			for (const auto character : value)
			{
				const auto byte = static_cast<unsigned char>(character);
				switch (character)
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
							append_hex_escape(output, byte);
						else
							output.push_back(character);
				}
			}
			output.push_back('"');
		}

		template <class Integer>
		void append_integer(std::string& output, const Integer value)
		{
			char buffer[std::numeric_limits<Integer>::digits10 + 4U]{};
			const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), value);
			output.append(buffer, converted.ptr);
		}

		void append_canonical_json(std::string& output, const json_value& value)
		{
			switch (value.type())
			{
				case json_value::kind::null_value:
					output += "null";
					break;
				case json_value::kind::boolean:
					output += *value.as_boolean() ? "true" : "false";
					break;
				case json_value::kind::signed_integer:
					append_integer(output, *value.as_signed_integer());
					break;
				case json_value::kind::unsigned_integer:
					append_integer(output, *value.as_unsigned_integer());
					break;
				case json_value::kind::string:
					append_json_string(output, *value.as_string());
					break;
				case json_value::kind::array:
				{
					output.push_back('[');
					bool first = true;
					for (const auto& child : *value.as_array())
					{
						if (!first)
							output.push_back(',');
						first = false;
						append_canonical_json(output, child);
					}
					output.push_back(']');
					break;
				}
				case json_value::kind::object:
				{
					output.push_back('{');
					bool first = true;
					for (const auto& [key, child] : *value.as_object())
					{
						if (!first)
							output.push_back(',');
						first = false;
						append_json_string(output, key);
						output.push_back(':');
						append_canonical_json(output, child);
					}
					output.push_back('}');
					break;
				}
			}
		}
	} // namespace

	bool utf8_byte_less::operator()(const std::string_view left,
									const std::string_view right) const noexcept
	{
		const auto common = std::min(left.size(), right.size());
		for (std::size_t index = 0U; index < common; ++index)
		{
			const auto left_byte = static_cast<unsigned char>(left[index]);
			const auto right_byte = static_cast<unsigned char>(right[index]);
			if (left_byte != right_byte)
				return left_byte < right_byte;
		}
		return left.size() < right.size();
	}

	json_value::json_value(storage_type value) : value_{std::move(value)} {}

	json_value json_value::null()
	{
		return json_value{storage_type{std::in_place_type<std::monostate>}};
	}

	json_value json_value::boolean(const bool value)
	{
		return json_value{storage_type{std::in_place_type<bool>, value}};
	}

	json_value json_value::signed_integer(const std::int64_t value)
	{
		return json_value{storage_type{std::in_place_type<std::int64_t>, value}};
	}

	json_value json_value::unsigned_integer(const std::uint64_t value)
	{
		return json_value{storage_type{std::in_place_type<std::uint64_t>, value}};
	}

	sdk::result<json_value> json_value::string(std::string value)
	{
		if (const auto invalid = invalid_utf8_offset(value))
			return sdk::unexpected(json_error("invalid-utf8", *invalid));
		return json_value{storage_type{std::in_place_type<std::string>, std::move(value)}};
	}

	json_value json_value::array(array_type value)
	{
		return json_value{storage_type{std::in_place_type<array_type>, std::move(value)}};
	}

	sdk::result<json_value> json_value::object(object_type value)
	{
		for (const auto& [key, child] : value)
		{
			(void)child;
			if (const auto invalid = invalid_utf8_offset(key))
				return sdk::unexpected(json_error("invalid-utf8-object-key", *invalid));
		}
		return json_value{storage_type{std::in_place_type<object_type>, std::move(value)}};
	}

	json_value::kind json_value::type() const noexcept
	{
		return static_cast<kind>(value_.index());
	}

	bool json_value::is_null() const noexcept
	{
		return std::holds_alternative<std::monostate>(value_);
	}

	const bool* json_value::as_boolean() const noexcept
	{
		return std::get_if<bool>(&value_);
	}

	const std::int64_t* json_value::as_signed_integer() const noexcept
	{
		return std::get_if<std::int64_t>(&value_);
	}

	const std::uint64_t* json_value::as_unsigned_integer() const noexcept
	{
		return std::get_if<std::uint64_t>(&value_);
	}

	const std::string* json_value::as_string() const noexcept
	{
		return std::get_if<std::string>(&value_);
	}

	const json_value::array_type* json_value::as_array() const noexcept
	{
		return std::get_if<array_type>(&value_);
	}

	const json_value::object_type* json_value::as_object() const noexcept
	{
		return std::get_if<object_type>(&value_);
	}

	const json_value* json_value::member(const std::string_view name) const noexcept
	{
		const auto* object_value = as_object();
		if (object_value == nullptr)
			return nullptr;
		const auto found = object_value->find(name);
		return found == object_value->end() ? nullptr : &found->second;
	}

	bool json_value::has_exact_members(const std::span<const std::string_view> names) const noexcept
	{
		const auto* object_value = as_object();
		if (object_value == nullptr || object_value->size() != names.size())
			return false;
		for (std::size_t index = 0U; index < names.size(); ++index)
		{
			if (!object_value->contains(names[index]))
				return false;
			for (std::size_t prior = 0U; prior < index; ++prior)
				if (names[prior] == names[index])
					return false;
		}
		return true;
	}

	json_document::json_document(std::string raw_bytes, json_value root)
		: raw_bytes_{std::move(raw_bytes)}, root_{std::move(root)}
	{
	}

	const std::string& json_document::raw_bytes() const noexcept
	{
		return raw_bytes_;
	}

	const json_value& json_document::root() const noexcept
	{
		return root_;
	}

	sdk::result<json_document> parse_json_object(std::string raw_bytes, const json_limits& limits)
	{
		if (raw_bytes.size() > limits.max_input_bytes)
			return sdk::unexpected(json_error("input-byte-limit", limits.max_input_bytes));
		if (raw_bytes.starts_with("\xef\xbb\xbf"))
			return sdk::unexpected(json_error("utf8-bom", 0U));
		if (const auto invalid = invalid_utf8_offset(raw_bytes))
			return sdk::unexpected(json_error("invalid-utf8", *invalid));

		strict_json_parser parser{raw_bytes, limits};
		auto root = parser.parse();
		if (!root)
			return sdk::unexpected(std::move(root.error()));
		return json_document{std::move(raw_bytes), std::move(*root)};
	}

	std::string canonical_json(const json_value& value)
	{
		std::string output;
		append_canonical_json(output, value);
		return output;
	}

	std::string canonical_json_line(const json_value& value)
	{
		auto output = canonical_json(value);
		output.push_back('\n');
		return output;
	}
} // namespace cxxlens::detail::clang22::materialization
