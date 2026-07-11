#include "core/canonical_json.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <locale>
#include <string>

namespace
{
	using cxxlens::detail::json::json_value;

	auto check(const bool condition, const std::string& message) -> bool
	{
		if (!condition)
			std::cerr << message << '\n';
		return condition;
	}
} // namespace

auto main() -> int
{
	using namespace cxxlens::detail::json;
	bool passed = true;
	const json_value forward{
		json_value::object{{"z", std::uint64_t{7U}},
						   {"a", "alpha"},
						   {"escaped", "\"\\\n\t\b\f\r"},
						   {"array", json_value::array{true, null_value{}, -2.5}}}};
	const json_value reverse{
		json_value::object{{"array", json_value::array{true, null_value{}, -2.5}},
						   {"escaped", "\"\\\n\t\b\f\r"},
						   {"a", "alpha"},
						   {"z", std::uint64_t{7U}}}};
	const auto first = write(forward);
	const auto second = write(reverse);
	passed &= check(first && second && first.value() == second.value(),
					"object insertion order changed canonical JSON");
	passed &=
		check(first.value() ==
				  R"({"a":"alpha","array":[true,null,-2.5],"escaped":"\"\\\n\t\b\f\r","z":7})",
			  "canonical JSON edge vector changed");

	passed &= check(!write(json_value{json_value::object{{"same", true}, {"same", false}}}),
					"duplicate key accepted");
	passed &= check(!write(json_value{std::numeric_limits<double>::infinity()}),
					"infinite number accepted");
	passed &= check(!write(json_value{std::numeric_limits<double>::quiet_NaN()}), "NaN accepted");
	passed &= check(write(json_value{-0.0}).value() == "0", "negative zero was not normalized");
	passed &= check(!write(json_value{std::string{"\xC0\xAF", 2U}}), "invalid UTF-8 accepted");
	passed &= check(valid_utf8("日本語") && valid_utf8("\xF0\x9F\x98\x80"), "valid UTF-8 rejected");

	json_value deep;
	for (std::size_t depth = 0U; depth < 66U; ++depth)
		deep = json_value::array{std::move(deep)};
	passed &= check(!write(deep, 64U), "deep nesting limit not enforced");

	const json_value document{envelope({"cxxlens.fixture.v1"}, {{"payload", "semantic"}})};
	const auto document_json = write(document).value();
	passed &= check(document_json.find(R"("schema":"cxxlens.fixture.v1")") != std::string::npos &&
						document_json.find(R"("semantics_version":"1.0.0")") != std::string::npos &&
						document_json.find(R"("library_version":"0.1.0")") != std::string::npos,
					"version envelope incomplete");
	passed &= check(document_json.find("timestamp") == std::string::npos &&
						document_json.find("cache_hit") == std::string::npos,
					"operational metadata entered semantic envelope");

	const auto before_locale = write(json_value{1234.5}).value();
	try
	{
		std::locale::global(std::locale{"C.UTF-8"});
	}
	catch (const std::runtime_error&)
	{
		std::locale::global(std::locale::classic());
	}
	passed &=
		check(before_locale == write(json_value{1234.5}).value(), "locale changed number encoding");
	return passed ? 0 : 1;
}
