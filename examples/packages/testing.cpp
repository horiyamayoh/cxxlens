#include <type_traits>

#include <cxxlens/testing.hpp>

// Contract scenarios: positive use; negative error; ambiguous resolution; partial/unresolved.
// Planned mutation surfaces are observed as dry-run plans; this example performs no apply.
static_assert(std::is_class_v<cxxlens::testing::workspace_fixture>);

int main()
{
	return 0;
}
