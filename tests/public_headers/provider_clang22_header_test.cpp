#include <type_traits>

#include <cxxlens/provider/clang22.hpp>

int main()
{
	static_assert(!cxxlens::provider::clang22::detachable_scalar<void*>);
	return 0;
}
