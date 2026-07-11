#include <cxxlens/core.hpp>

auto main() -> int
{
	return cxxlens::versions().library.major == 0U ? 0 : 1;
}
