#include <cxxlens/core/finding.hpp>

auto main() -> int
{
	cxxlens::diagnostic observation;
	observation.id = "clang.warning";
	cxxlens::finding_set findings;
	return observation.validate() && findings.empty() ? 0 : 1;
}
