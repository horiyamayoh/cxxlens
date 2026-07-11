#include <cxxlens/cxxlens.hpp>

auto main() -> int
{
	const cxxlens::path current{"."};
	return current.empty() || cxxlens::versions().llvm.major != 22U ? 1 : 0;
}
