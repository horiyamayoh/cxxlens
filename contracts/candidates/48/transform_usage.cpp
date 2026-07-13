#include "transform.hpp"

int main()
{
	using namespace cxxlens;
	const auto plan = transform::plan_options{
		.scope = analysis_scope::all(),
		.macro_policy = transform::macro_edit_policy::reject,
		.generated = transform::generated_code_policy::read_only,
		.formatting = transform::format_policy::changed_ranges,
		.reparse = transform::reparse_policy::affected_variants,
	};
	const auto apply = transform::apply_options{};
	(void)plan;
	return apply.mode == transform::apply_mode::dry_run ? 0 : 1;
}
