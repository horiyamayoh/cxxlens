#include <type_traits>

#include <cxxlens/provider/clang22.hpp>

int main()
{
	static_assert(!cxxlens::provider::clang22::detachable_scalar<void*>);
	static_assert(
		!std::is_copy_constructible_v<cxxlens::provider::clang22::borrowed_translation_unit>);
	cxxlens::sdk::detached_row safe;
	safe.descriptor_id = "frontend.clang22.call_observation.v1";
	if (!cxxlens::provider::clang22::detect_native_escape(safe))
		return 1;
	bool visited = false;
	auto parsed = cxxlens::provider::clang22::with_translation_unit(
		{"source-snapshot:example",
		 "file:input.cpp",
		 "input.cpp",
		 "int target(); int main(){ return target(); }",
		 {"-std=c++23"}},
		[&](cxxlens::provider::clang22::borrowed_translation_unit&) -> cxxlens::sdk::result<void>
		{
			visited = true;
			return {};
		});
	if (parsed)
		return visited ? 0 : 1;
	return parsed.error().code == "native.unsupported-clang-major" ? 0 : 1;
}
