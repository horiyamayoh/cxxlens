#include <cxxlens/cxxlens.hpp>

auto main() -> int
{
	const auto product_versions = cxxlens::versions();
	return product_versions.library.major == 0U && product_versions.llvm.major == 22U ? 0 : 1;
}
