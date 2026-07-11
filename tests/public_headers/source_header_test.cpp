#include <cxxlens/source.hpp>

auto main() -> int
{
	const cxxlens::path current{"."};
	return current.empty() ? 1 : 0;
}
