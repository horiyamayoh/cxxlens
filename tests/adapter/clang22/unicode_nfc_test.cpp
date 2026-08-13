#include "llvm/clang22/unicode_nfc.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace
{
	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	void expect_normalized(const std::string_view value, const bool expected)
	{
		auto normalized = cxxlens::detail::clang22::is_nfc_utf8(value);
		require(normalized.has_value(), "valid UTF-8 did not produce an NFC result");
		require(*normalized == expected, "NFC classification differed from the oracle");
	}
} // namespace

int main()
{
	expect_normalized("project://main.cpp", true);
	expect_normalized("project://caf\xc3\xa9.cpp", true);				// U+00E9
	expect_normalized("project://cafe\xcc\x81.cpp", false);				// U+0065 U+0301
	expect_normalized("project://q\xcc\x81.cpp", true);					// no canonical composition
	expect_normalized("project://\xea\xb0\x80.cpp", true);				// U+AC00
	expect_normalized("project://\xe1\x84\x80\xe1\x85\xa1.cpp", false); // U+1100 U+1161

	const std::string embedded_nul{"project://a\0b.cpp", 17U};
	expect_normalized(embedded_nul, true);
	const std::string invalid_utf8{"\xc3\x28", 2U};
	require(!cxxlens::detail::clang22::is_nfc_utf8(invalid_utf8),
			"invalid UTF-8 was accepted by the NFC boundary");
}
