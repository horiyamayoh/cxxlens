#pragma once

/** @file recipe.hpp @brief High-level recipe lowering without hiding query partiality. */

#include <cstdint>
#include <string>
#include <string_view>
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

namespace cxxlens::recipes
{
	/** @brief Semantic classification that preserves execution and input partiality. */
	enum class call_search_state : std::uint8_t
	{
		/** @brief Complete execution found calls to exactly one target entity. */
		matched,
		/** @brief Complete execution and complete inputs proved there were no matches. */
		empty_complete,
		/** @brief Complete execution found no matches but its inputs were incomplete. */
		empty_incomplete,
		/** @brief Complete execution found calls to multiple distinct target entities. */
		ambiguous,
		/** @brief Truncation or cancellation returned only a sealed result prefix. */
		partial,
		/** @brief Execution failed before any sealed result could be classified. */
		failed,
	};
	/** @brief Test exact membership in the closed call search state enum. */
	[[nodiscard]] constexpr bool is_valid(const call_search_state value) noexcept
	{
		return value >= call_search_state::matched && value <= call_search_state::failed;
	}

	/** @brief Flagship call-search report owning both its exact plan and query result. */
	class call_search_report
	{
	  public:
		[[nodiscard]] const sdk::recipe_plan& plan() const noexcept;
		[[nodiscard]] const sdk::query::query_result& result() const noexcept;
		[[nodiscard]] sdk::query::result_row_cursor matches() const;
		[[nodiscard]] call_search_state state() const;
		[[nodiscard]] std::string canonical_form() const;

	  private:
		call_search_report(sdk::recipe_plan plan,
						   sdk::query::query_result result,
						   call_search_state state);
		sdk::recipe_plan plan_;
		sdk::query::query_result result_;
		call_search_state state_{call_search_state::empty_incomplete};
		friend class call_search_recipe;
	};

	/** @brief Versioned calls-to-function recipe lowered to the common Logical Query IR. */
	class call_search_recipe
	{
	  public:
		[[nodiscard]] const sdk::recipe_descriptor& descriptor() const noexcept;
		[[nodiscard]] std::string_view qualified_name() const noexcept;
		[[nodiscard]] sdk::result<sdk::recipe_plan> lower() const;
		[[nodiscard]] sdk::result<call_search_report>
		run(sdk::snapshot_handle snapshot, sdk::query::execution_request request = {}) const;

	  private:
		explicit call_search_recipe(std::string qualified_name);
		std::string qualified_name_;
		sdk::recipe_descriptor descriptor_;
		friend sdk::result<call_search_recipe> calls_to_function(std::string);
	};

	/** @brief Create the NG0 flagship recipe for one exact qualified function name. */
	[[nodiscard]] sdk::result<call_search_recipe> calls_to_function(std::string qualified_name);
} // namespace cxxlens::recipes
