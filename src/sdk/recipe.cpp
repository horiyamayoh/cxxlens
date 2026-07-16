#include <ranges>
#include <sstream>

#include <cxxlens/sdk/recipe.hpp>

namespace cxxlens::sdk
{
	recipe_plan::recipe_plan(recipe_descriptor descriptor, query::logical_query_ir query)
		: descriptor_{std::move(descriptor)}, query_{std::move(query)}
	{
	}

	result<recipe_plan> recipe_plan::lower(recipe_descriptor descriptor,
										   query::logical_query_ir query)
	{
		if (descriptor.id.empty() || !descriptor.id.contains('.') || descriptor.version.major == 0U)
			return cxxlens::sdk::unexpected(error{"sdk.recipe-invalid", "descriptor", {}});
		if (auto valid = query.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		return recipe_plan{std::move(descriptor), std::move(query)};
	}

	const recipe_descriptor& recipe_plan::descriptor() const noexcept
	{
		return descriptor_;
	}

	const query::logical_query_ir& recipe_plan::query() const noexcept
	{
		return query_;
	}

	std::vector<relation_descriptor> recipe_plan::requirements() const
	{
		return query_.relation_requirements;
	}

	std::string recipe_plan::explain() const
	{
		std::ostringstream output;
		output << descriptor_.id << '@' << descriptor_.version.string() << " -> " << query_.digest()
			   << "\nrequirements:";
		for (const auto& descriptor : query_.relation_requirements)
			output << "\n- " << descriptor.id << '@' << descriptor.version.string();
		output << "\npartiality: query-result-owned\n";
		return output.str();
	}
} // namespace cxxlens::sdk
