#include "json_parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace cxxlens::testing::detail
{
	namespace
	{
		using cxxlens::detail::json::json_value;
		constexpr std::size_t maximum_json_bytes = std::size_t{4U} * 1024U * 1024U;
		constexpr std::size_t maximum_json_depth = 64U;

		[[nodiscard]] error parse_error(const std::size_t offset, std::string reason)
		{
			error failure;
			failure.code.value = "testing.json-invalid";
			failure.message = "JSON input is invalid";
			failure.attributes.emplace("offset", std::to_string(offset));
			failure.attributes.emplace("reason", std::move(reason));
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
			else if (code_point <= 0xFFFFU)
			{
				output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
				output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
				output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
			}
			else
			{
				output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
				output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
				output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
				output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
			}
		}

		class parser
		{
		  public:
			explicit parser(const std::string_view input) : input_{input} {}

			[[nodiscard]] result<json_value> parse()
			{
				if (input_.size() > maximum_json_bytes)
					return parse_error(0U, "size-limit");
				auto value = parse_value(0U);
				if (!value)
					return std::move(value.error());
				skip_space();
				if (position_ != input_.size())
					return parse_error(position_, "trailing-data");
				return std::move(value.value());
			}

		  private:
			[[nodiscard]] result<json_value> parse_value(const std::size_t depth)
			{
				if (depth > maximum_json_depth)
					return parse_error(position_, "depth-limit");
				skip_space();
				if (position_ >= input_.size())
					return parse_error(position_, "value-missing");
				switch (input_.at(position_))
				{
					case '{':
						return parse_object(depth);
					case '[':
						return parse_array(depth);
					case '"':
					{
						auto string = parse_string();
						if (!string)
							return std::move(string.error());
						return json_value{std::move(string.value())};
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
						return parse_error(position_, "colon-missing");
					auto value = parse_value(depth + 1U);
					if (!value)
						return std::move(value.error());
					if (std::ranges::any_of(object,
											[&](const auto& field)
											{
												return field.first == key.value();
											}))
						return parse_error(position_, "duplicate-key");
					object.emplace_back(std::move(key.value()), std::move(value.value()));
					skip_space();
					if (consume('}'))
						break;
					if (!consume(','))
						return parse_error(position_, "comma-missing");
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
						return parse_error(position_, "comma-missing");
				}
				return json_value{std::move(array)};
			}

			[[nodiscard]] result<std::string> parse_string()
			{
				if (!consume('"'))
					return parse_error(position_, "string-expected");
				std::string output;
				while (position_ < input_.size())
				{
					const auto character = static_cast<unsigned char>(input_.at(position_++));
					if (character == '"')
					{
						if (!cxxlens::detail::json::valid_utf8(output))
							return parse_error(position_, "invalid-utf8");
						return output;
					}
					if (character < 0x20U)
						return parse_error(position_, "control-character");
					if (character != '\\')
					{
						output.push_back(static_cast<char>(character));
						continue;
					}
					if (position_ >= input_.size())
						return parse_error(position_, "escape-missing");
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
								return parse_error(position_, "invalid-unicode-escape");
							append_utf8(output, code_point.value());
							break;
						}
						default:
							return parse_error(position_, "unsupported-escape");
					}
				}
				return parse_error(position_, "unterminated-string");
			}

			[[nodiscard]] result<std::uint32_t> hexadecimal_quad()
			{
				if (position_ + 4U > input_.size())
					return parse_error(position_, "short-unicode-escape");
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
						return parse_error(position_, "invalid-unicode-hex");
				}
				return value;
			}

			[[nodiscard]] result<json_value> parse_number()
			{
				const auto begin = position_;
				while (position_ < input_.size() &&
					   std::string_view{"-+0123456789.eE"}.contains(input_.at(position_)))
					++position_;
				if (begin == position_)
					return parse_error(position_, "number-expected");
				const auto token = input_.substr(begin, position_ - begin);
				if (token.find_first_of(".eE") != std::string_view::npos)
				{
					double value{};
					const auto converted =
						std::from_chars(token.data(), token.data() + token.size(), value);
					if (converted.ec != std::errc{} || converted.ptr != token.data() + token.size())
						return parse_error(begin, "invalid-number");
					return json_value{value};
				}
				if (token.starts_with('-'))
				{
					std::int64_t value{};
					const auto converted =
						std::from_chars(token.data(), token.data() + token.size(), value);
					if (converted.ec != std::errc{} || converted.ptr != token.data() + token.size())
						return parse_error(begin, "invalid-integer");
					return json_value{value};
				}
				std::uint64_t value{};
				const auto converted =
					std::from_chars(token.data(), token.data() + token.size(), value);
				if (converted.ec != std::errc{} || converted.ptr != token.data() + token.size())
					return parse_error(begin, "invalid-integer");
				return json_value{value};
			}

			[[nodiscard]] result<json_value> literal(const std::string_view expected,
													 json_value value)
			{
				if (input_.substr(position_, expected.size()) != expected)
					return parse_error(position_, "invalid-literal");
				position_ += expected.size();
				return value;
			}

			void skip_space()
			{
				while (position_ < input_.size() &&
					   std::isspace(static_cast<unsigned char>(input_.at(position_))) != 0)
					++position_;
			}

			[[nodiscard]] bool consume(const char character)
			{
				if (position_ < input_.size() && input_.at(position_) == character)
				{
					++position_;
					return true;
				}
				return false;
			}

			std::string_view input_;
			std::size_t position_{};
		};
	} // namespace

	result<cxxlens::detail::json::json_value> parse_json(const std::string_view input)
	{
		return parser{input}.parse();
	}
} // namespace cxxlens::testing::detail
