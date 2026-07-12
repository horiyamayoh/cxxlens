#include <type_traits>

#include <cxxlens/interop/clang.hpp>

int main()
{
	static_assert(!std::is_copy_constructible_v<cxxlens::interop::borrowed_clang_tu>);
	static_assert(!std::is_copy_assignable_v<cxxlens::interop::borrowed_clang_tu>);
	const auto version = cxxlens::interop::linked_clang_version();
	return version.llvm_major == 0U || version.llvm_major == 22U ? 0 : 1;
}
