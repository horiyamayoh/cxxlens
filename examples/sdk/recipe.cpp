#include <cxxlens/sdk/recipe.hpp>

int main()
{
	auto recipe = cxxlens::recipes::calls_to_function("app::dangerous");
	if (!recipe)
		return 1;
	auto plan = recipe->lower();
	return plan && !plan->explain().empty() && plan->requirements().size() == 3U ? 0 : 1;
}
