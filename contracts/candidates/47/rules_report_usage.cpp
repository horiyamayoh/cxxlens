#include "rules_report.hpp"

int main()
{
	using namespace cxxlens;
	const auto suppressions = rules::suppression_policy::none()
							  .rule("modernize.example", "tracked exception")
							  .minimum_severity(severity::warning)
							  .reject_expired();
	const auto pack = rules::rule_pack{}.suppressions(suppressions).failure_policy(
		rules::rule_failure_policy::continue_partial);
	const auto options = report::options{
		.output = report::format::json,
		.paths = report::path_presentation::redacted_token,
		.output_budget_bytes = 1024,
	};
	(void)pack;
	(void)options;
	return 0;
}
