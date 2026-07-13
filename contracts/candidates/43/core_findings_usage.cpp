#include "core_findings.hpp"

int main()
{
	cxxlens::finding_set findings;
	const auto filtered = findings.minimum_confidence(cxxlens::confidence::high)
						  .minimum_severity(cxxlens::severity::warning);
	return filtered.empty() && !filtered.to_json().empty() ? 0 : 1;
}
