#include "qa.hpp"
#include "review.hpp"

int main()
{
	using namespace cxxlens;
	const auto diff_options = review::diff_options{};
	const auto baseline_options = review::baseline_options{};
	const auto process = qa::process_policy{};
	const auto coverage = qa::coverage_import_options{};
	(void)baseline_options;
	(void)process;
	(void)coverage;
	return diff_options.include_worktree ? 0 : 1;
}
