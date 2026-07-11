#include <cxxlens/core/evidence.hpp>

auto main() -> int
{
	cxxlens::coverage_report coverage;
	cxxlens::evidence why;
	return coverage.complete() && why.items().empty() ? 0 : 1;
}
