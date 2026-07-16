#include <utility>

#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/sdk/recipe.hpp>

int main()
{
	using relation = cxxlens::cc::relations::call_site;
	auto query = cxxlens::sdk::query::from<relation>();
	if (!query)
		return 1;
	auto plan = cxxlens::sdk::recipe_plan::lower(
		{"company.recipe.calls", {1U, 0U, 0U}, "Call analysis"}, std::move(*query).finish());
	return plan && !plan->explain().empty() && plan->requirements().size() == 1U ? 0 : 1;
}
