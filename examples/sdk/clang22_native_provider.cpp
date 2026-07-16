#include <type_traits>

#include <cxxlens/provider/clang22.hpp>

int main()
{
	static_assert(!cxxlens::provider::clang22::detachable_scalar<void*>);
	static_assert(
		!std::is_copy_constructible_v<cxxlens::provider::clang22::borrowed_translation_unit>);
	cxxlens::sdk::detached_row safe;
	safe.descriptor_id = "frontend.clang22.call_observation.v1";
	return cxxlens::provider::clang22::detect_native_escape(safe) ? 0 : 1;
}
