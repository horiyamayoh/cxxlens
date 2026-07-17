#include <type_traits>

#include <cxxlens/provider/clang22.hpp>

int main()
{
	static_assert(!cxxlens::provider::clang22::detachable_scalar<void*>);
	static_assert(
		std::is_same_v<decltype(cxxlens::provider::clang22::detached_source_span{}.origin_chain),
					   std::vector<cxxlens::provider::clang22::detached_source_origin>>);
	return 0;
}
