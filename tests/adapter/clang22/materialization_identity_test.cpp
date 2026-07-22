#include "llvm/clang22/materialization_identity.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}
} // namespace

int main()
{
	using namespace cxxlens::detail::clang22::materialization;
	const auto parsed = parse_json_object(R"({"z":[null,true,-2],"a":{"text":"雪","value":1e2}})");
	require(parsed.has_value(), "materialization identity fixture did not parse");
	const auto digest =
		projection_digest("cxxlens.materialization-identity-test.v1", parsed->root());
	require(digest.has_value() &&
				*digest ==
					"semantic-v2:sha256:"
					"2761a287a173a5fdb411214b92ede5a2ed6d1dc6f01a8c9a13ca187ded47ba1a",
			"JSON canonical tuple projection drifted from the Python oracle");

	const auto stripped = object_without(parsed->root(), std::array<std::string_view, 1>{"z"});
	require(stripped.has_value() && stripped->member("z") == nullptr &&
				stripped->member("a") != nullptr,
			"object projection member removal failed");
}
