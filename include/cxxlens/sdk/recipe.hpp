#pragma once

/** @file recipe.hpp @brief High-level recipe lowering without hiding query partiality. */

#include <string>
#include <vector>

#include <cxxlens/sdk/query.hpp>

namespace cxxlens::sdk
{
	/** @brief Versioned high-level recipe descriptor. */
	struct recipe_descriptor
	{
		std::string id;
		semantic_version version;
		std::string summary;
	};

	/** @brief Recipe lowered to an exact Logical Query IR and explicit requirements. */
	class recipe_plan
	{
	  public:
		[[nodiscard]] static result<recipe_plan> lower(recipe_descriptor descriptor,
													   query::logical_query_ir query);
		[[nodiscard]] const recipe_descriptor& descriptor() const noexcept;
		[[nodiscard]] const query::logical_query_ir& query() const noexcept;
		[[nodiscard]] std::vector<relation_descriptor> requirements() const;
		[[nodiscard]] std::string explain() const;

	  private:
		recipe_plan(recipe_descriptor descriptor, query::logical_query_ir query);
		recipe_descriptor descriptor_;
		query::logical_query_ir query_;
	};
} // namespace cxxlens::sdk
