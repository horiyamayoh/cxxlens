#include "llvm/clang22/materialization_json.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace
{
	using namespace cxxlens::detail::clang22::materialization;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] json_limits small_limits()
	{
		return {
			.max_input_bytes = 4096U,
			.max_depth = 64U,
			.max_array_elements = 32U,
			.max_object_members = 32U,
			.max_string_bytes = 256U,
			.max_total_string_bytes = 1024U,
			.max_total_values = 128U,
		};
	}

	[[nodiscard]] json_document parse(std::string raw, const json_limits& limits = small_limits())
	{
		auto parsed = parse_json_object(std::move(raw), limits);
		require(parsed.has_value(), parsed ? "" : parsed.error().detail);
		return std::move(*parsed);
	}

	void reject(std::string raw,
				const std::string_view reason,
				const json_limits& limits = small_limits())
	{
		auto parsed = parse_json_object(std::move(raw), limits);
		require(!parsed.has_value(), "invalid JSON was accepted");
		require(parsed.error().code == "materialization.json-invalid",
				"JSON failure used the wrong error code");
		require(parsed.error().detail.contains(reason), "JSON failure used the wrong reason");
	}

	void canonical_and_accessors()
	{
		const std::string emoji{"\xf0\x9f\x98\x80"};
		const std::string e_acute{"\xc3\xa9"};
		const std::string raw = " \r\n{\"z\":\"\\u0061\",\"emoji\":\"\\uD83D\\uDE00\","
								"\"a\":\"\\u00E9\",\"slash\":\"\\/\","
								"\"control\":\"\\b\\f\\n\\r\\t\\u0000\\u001F\","
								"\"numbers\":{\"exact_fraction\":1.0,\"exact_exponent\":1e2,"
								"\"max\":18446744073709551615,\"min\":-9223372036854775808,"
								"\"negative_zero\":-0,\"scaled\":100e-2}}\t";
		auto document = parse(raw);
		require(document.raw_bytes() == raw, "raw JSON occurrence bytes were not preserved");

		const auto expected = std::string{"{\"a\":\""} + e_acute +
			"\",\"control\":\"\\b\\f\\n\\r\\t\\u0000\\u001f\",\"emoji\":\"" + emoji +
			"\",\"numbers\":{\"exact_exponent\":100,\"exact_fraction\":1,"
			"\"max\":18446744073709551615,\"min\":-9223372036854775808,"
			"\"negative_zero\":0,\"scaled\":1},"
			"\"slash\":\"/\",\"z\":\"a\"}";
		require(canonical_json(document.root()) == expected,
				"canonical JSON did not match the Python checker form");
		require(canonical_json_line(document.root()) == expected + '\n',
				"response JSON line did not have exactly one final LF");

		constexpr std::array<std::string_view, 6U> keys = {
			"a", "control", "emoji", "numbers", "slash", "z"};
		require(document.root().has_exact_members(keys), "exact member-set access failed");
		constexpr std::array<std::string_view, 6U> duplicate_keys = {
			"a", "a", "emoji", "numbers", "slash", "z"};
		require(!document.root().has_exact_members(duplicate_keys),
				"duplicate requested member names were accepted");
		require(document.root().member("missing") == nullptr,
				"missing member lookup did not fail safely");
		const auto* numbers = document.root().member("numbers");
		require(numbers != nullptr && numbers->as_object() != nullptr,
				"object type accessor failed");
		require(numbers->as_array() == nullptr, "wrong-type array accessor succeeded");
		require(numbers->member("max") != nullptr &&
					*numbers->member("max")->as_unsigned_integer() ==
						std::numeric_limits<std::uint64_t>::max(),
				"uint64 JSON integer was not retained exactly");
		require(numbers->member("min") != nullptr &&
					*numbers->member("min")->as_signed_integer() ==
						std::numeric_limits<std::int64_t>::min(),
				"int64 JSON integer was not retained exactly");
		require(numbers->member("negative_zero") != nullptr &&
					*numbers->member("negative_zero")->as_unsigned_integer() == 0U,
				"negative zero did not normalize to the checker integer value");

		const auto unicode_keys =
			parse(std::string{"{\""} + emoji + "\":1,\"" + e_acute + "\":2,\"a\":3}");
		require(canonical_json(unicode_keys.root()) ==
					std::string{"{\"a\":3,\""} + e_acute + "\":2,\"" + emoji + "\":1}",
				"object keys were not sorted in Unicode scalar order");
	}

	void lexical_rejections()
	{
		std::string invalid_utf8{"{\"x\":\""};
		invalid_utf8.push_back(static_cast<char>(0xc0U));
		invalid_utf8 += "\"}";
		reject(std::move(invalid_utf8), "invalid-utf8");
		reject(std::string{"\xef\xbb\xbf"} + "{}", "utf8-bom");
		reject("{\"outer\":{\"x\":1,\"x\":2}}", "duplicate-member");
		reject("{\"outer\":{\"a\":1,\"\\u0061\":2}}", "duplicate-member");
		reject("{\"\\u00e9\":1,\"\xc3\xa9\":2}", "duplicate-member");

		for (const auto raw : {
				 "{\"x\":\"\\ud800\"}",
				 "{\"x\":\"\\udc00\"}",
				 "{\"x\":\"\\ud800\\u0041\"}",
			 })
			reject(raw, "surrogate-pair");

		reject("{\"x\":01}", "number-syntax");
		reject("{\"x\":1.5}", "non-integer-number");
		reject("{\"x\":1e-2}", "non-integer-number");
		reject("{\"x\":1e400}", "unsigned-integer-overflow");
		reject("{\"x\":-9223372036854775809}", "signed-integer-overflow");
		reject("{\"x\":18446744073709551616}", "unsigned-integer-overflow");
		reject("{\"x\":NaN}", "number-syntax");
		reject("{\"x\":Infinity}", "number-syntax");

		reject("{} {}", "trailing-data");
		reject("{}x", "trailing-data");
		reject("[]", "top-level-object-required");
		reject("null", "top-level-object-required");
		reject("{\"x\":1}\v", "trailing-data");
		reject("{\"x\":\"raw\nnewline\"}", "raw-control-character");
	}

	void exact_decimal_integer_domain()
	{
		for (const auto& [token, canonical] :
			 std::array<std::pair<std::string_view, std::string_view>, 9U>{
				 std::pair{"1.00", "1"},
				 std::pair{"1.20e1", "12"},
				 std::pair{"1000e-3", "1"},
				 std::pair{"184467440737095516150e-1", "18446744073709551615"},
				 std::pair{"-92233720368547758080e-1", "-9223372036854775808"},
				 std::pair{"-0.0001e4", "-1"},
				 std::pair{"0e999999999999999999999999999999", "0"},
				 std::pair{"0.0e-999999999999999999999999999999", "0"},
				 std::pair{"1e+2", "100"},
			 })
		{
			const auto document = parse(std::string{"{\"x\":"} + std::string{token} + '}');
			require(canonical_json(document.root()) ==
						std::string{"{\"x\":"} + std::string{canonical} + '}',
					"exact decimal integer normalization drifted");
		}

		reject("{\"x\":1000e-4}", "non-integer-number");
		reject("{\"x\":184467440737095516150e0}", "unsigned-integer-overflow");
		reject("{\"x\":-92233720368547758080e0}", "signed-integer-overflow");
	}

	void configured_bounds()
	{
		auto limits = small_limits();
		limits.max_input_bytes = 2U;
		reject("{\"x\":1}", "input-byte-limit", limits);

		limits = small_limits();
		limits.max_depth = 2U;
		(void)parse("{\"x\":[1]}", limits);
		reject("{\"x\":[[1]]}", "depth-limit", limits);

		limits = small_limits();
		limits.max_array_elements = 1U;
		reject("{\"x\":[1,2]}", "array-element-limit", limits);

		limits = small_limits();
		limits.max_object_members = 1U;
		reject("{\"a\":1,\"b\":2}", "object-member-limit", limits);

		limits = small_limits();
		limits.max_string_bytes = 3U;
		reject("{\"x\":\"four\"}", "string-byte-limit", limits);

		limits = small_limits();
		limits.max_total_string_bytes = 3U;
		reject("{\"a\":\"b\",\"c\":\"d\"}", "string-byte-limit", limits);

		limits = small_limits();
		limits.max_total_values = 2U;
		reject("{\"x\":[1]}", "value-count-limit", limits);

		const auto nested = [](const std::size_t container_depth)
		{
			std::string raw{"{\"x\":"};
			raw.append(container_depth - 1U, '[');
			raw.push_back('0');
			raw.append(container_depth - 1U, ']');
			raw.push_back('}');
			return raw;
		};
		limits = small_limits();
		limits.max_depth = 100U;
		(void)parse(nested(64U), limits);
		reject(nested(65U), "depth-limit", limits);
	}
} // namespace

int main()
{
	canonical_and_accessors();
	lexical_rejections();
	exact_decimal_integer_domain();
	configured_bounds();
	return 0;
}
