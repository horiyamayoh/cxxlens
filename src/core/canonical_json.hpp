#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <cxxlens/core/failure.hpp>

namespace cxxlens::detail::json
{
	struct null_value
	{
		[[nodiscard]] bool operator==(const null_value&) const noexcept = default;
	};

	struct json_value
	{
		using array = std::vector<json_value>;
		using object = std::vector<std::pair<std::string, json_value>>;
		using storage = std::variant<null_value,
									 bool,
									 std::int64_t,
									 std::uint64_t,
									 double,
									 std::string,
									 array,
									 object>;
		storage value;

		json_value() : value{null_value{}} {}
		json_value(null_value input) : value{input} {}
		json_value(bool input) : value{input} {}
		json_value(std::int64_t input) : value{input} {}
		json_value(std::uint64_t input) : value{input} {}
		json_value(double input) : value{input} {}
		json_value(std::string input) : value{std::move(input)} {}
		json_value(const char* input) : value{std::string{input}} {}
		json_value(array input) : value{std::move(input)} {}
		json_value(object input) : value{std::move(input)} {}
	};

	struct document_versions
	{
		std::string schema;
		std::string semantics{"1.0.0"};
		std::string library{"0.1.0"};
	};

	[[nodiscard]] json_value::object envelope(document_versions versions,
											  json_value::object semantic_fields);
	[[nodiscard]] result<std::string> write(const json_value& value,
											std::size_t maximum_depth = 64U);
	[[nodiscard]] bool valid_utf8(std::string_view value) noexcept;
} // namespace cxxlens::detail::json
