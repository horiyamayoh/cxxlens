#include "flow.hpp"

int main()
{
	using namespace cxxlens;
	const auto options = flow::flow_options{};
	const auto pack_options = models::model_pack_options{};
	const auto path = flow::value_path{.kind = flow::value_path_kind::pointee};
	(void)pack_options;
	(void)path;
	return options.unknown_calls == flow::unknown_call_policy::preserve_unresolved ? 0 : 1;
}
