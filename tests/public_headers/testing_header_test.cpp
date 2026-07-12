#include <cxxlens/testing.hpp>

int main()
{
	const auto fixture = cxxlens::testing::workspace_fixture::cpp("int main() { return 0; }");
	auto bundle = fixture.materialize();
	auto workspace = fixture.open();
	return bundle && bundle.value().validate() && workspace &&
			workspace.value().compile_units().size() == 1U
		? 0
		: 1;
}
