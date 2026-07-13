#include <type_traits>

#include <cxxlens/models.hpp>

// Contract scenarios: positive use; negative error; ambiguous resolution; partial/unresolved.
// Planned mutation surfaces are observed as dry-run plans; this example performs no apply.
static_assert(std::is_class_v<cxxlens::models::api_model_pack>);

int main()
{
	return 0;
}
