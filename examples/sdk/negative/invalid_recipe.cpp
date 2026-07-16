#include <utility>

#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/sdk/recipe.hpp>

int main()
{
	auto query = cxxlens::sdk::query::from<cxxlens::cc::relations::call_site>();
	if (!query)
		return 1;
	auto rejected = cxxlens::sdk::recipe_plan::lower({"not-namespaced", {1U, 0U, 0U}, {}},
													 std::move(*query).finish());
	return !rejected && rejected.error().code == "sdk.recipe-invalid" ? 0 : 1;
}
