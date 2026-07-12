#include <cxxlens/interop/clang.hpp>

int main()
{
	return cxxlens::interop::linked_clang_version().llvm_major == 22U ? 0 : 1;
}
