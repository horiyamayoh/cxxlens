#include <cxxlens/testing.hpp>

int main()
{
	auto bundle =
		cxxlens::testing::workspace_fixture::cpp("int main() { return 0; }").materialize();
	return bundle && bundle.value().validate() ? 0 : 1;
}
