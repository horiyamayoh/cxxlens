#include "llvm/clang22/materialization_json.hpp"

#include <cstdlib>
#include <string>

int main()
{
	using namespace cxxlens::detail::clang22::materialization;

	auto parsed = parse_json_object(R"({"value":1e2})");
	if (!parsed || canonical_json(parsed->root()) != R"({"value":100})")
		std::abort();

	auto wrong_root = parse_json_object("[]");
	if (wrong_root || wrong_root.error().code != "materialization.json-invalid" ||
		!wrong_root.error().detail.contains("top-level-object-required:byte=0"))
		std::abort();

	json_limits limits;
	limits.max_input_bytes = 2U;
	auto oversized = parse_json_object(R"({"value":1})", limits);
	if (oversized || oversized.error().code != "materialization.json-invalid" ||
		!oversized.error().detail.contains("input-byte-limit:byte=2"))
		std::abort();
}
