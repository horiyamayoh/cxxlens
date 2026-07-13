#include "generate.hpp"

int main()
{
	using namespace cxxlens::generate;
	const auto mock_options = mock::generation_options{};
	const auto harness_options = method_harness::harness_options{};
	const auto copy_options = copy::copy_options{};
	const auto fuzz_input = fuzz::input_model{.kind = fuzz::input_kind::bytes, .max_size = 4096};
	(void)mock_options;
	(void)harness_options;
	(void)copy_options;
	(void)fuzz_input;
	return cxxlens::transform::apply_options{}.mode == cxxlens::transform::apply_mode::dry_run ? 0 : 1;
}
