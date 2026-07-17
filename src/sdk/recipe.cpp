#include <array>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
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

namespace cxxlens::recipes
{
	namespace
	{
		[[nodiscard]] std::string state_name(const call_search_state state)
		{
			switch (state)
			{
				case call_search_state::matched:
					return "matched";
				case call_search_state::empty_complete:
					return "empty_complete";
				case call_search_state::empty_incomplete:
					return "empty_incomplete";
				case call_search_state::ambiguous:
					return "ambiguous";
				case call_search_state::partial:
					return "partial";
				case call_search_state::failed:
					return "failed";
			}
			return "failed";
		}

		[[nodiscard]] sdk::result<call_search_state>
		classify(const sdk::query::query_result& result)
		{
			switch (result.execution())
			{
				case sdk::query::execution_status::truncated:
				case sdk::query::execution_status::cancelled_with_partial:
					return call_search_state::partial;
				case sdk::query::execution_status::failed_before_result:
					return call_search_state::failed;
				case sdk::query::execution_status::complete:
					break;
			}
			std::set<std::string, std::less<>> targets;
			auto cursor = result.rows();
			std::size_t matches{};
			while (true)
			{
				auto next = cursor.next();
				if (!next)
					return sdk::unexpected(std::move(next.error()));
				if (!*next)
					break;
				auto row = (*next)->copy();
				if (!row)
					return sdk::unexpected(std::move(row.error()));
				++matches;
				const auto target = row->values.find("output.target");
				if (target != row->values.end() && target->second.value)
					if (const auto* value = std::get_if<std::string>(&*target->second.value))
						targets.insert(*value);
			}
			if (targets.size() > 1U)
				return call_search_state::ambiguous;
			if (matches != 0U)
				return call_search_state::matched;
			const bool absence_proved = result.inputs_complete() && result.closed() &&
				!result.closure_ids().empty() &&
				result.summary_guarantee().approximation == "exact";
			return absence_proved ? call_search_state::empty_complete
								  : call_search_state::empty_incomplete;
		}
	} // namespace

	call_search_report::call_search_report(sdk::recipe_plan plan,
										   sdk::query::query_result result,
										   const call_search_state state)
		: plan_{std::move(plan)}, result_{std::move(result)}, state_{state}
	{
	}

	const sdk::recipe_plan& call_search_report::plan() const noexcept
	{
		return plan_;
	}

	const sdk::query::query_result& call_search_report::result() const noexcept
	{
		return result_;
	}

	sdk::query::result_row_cursor call_search_report::matches() const
	{
		return result_.rows();
	}

	call_search_state call_search_report::state() const
	{
		return state_;
	}

	std::string call_search_report::canonical_form() const
	{
		std::ostringstream output;
		output << R"({"query_result":)" << result_.canonical_form() << R"(,"recipe_id":")"
			   << plan_.descriptor().id << R"(","recipe_version":")"
			   << plan_.descriptor().version.string() << R"(","relation_versions":[)";
		const auto requirements = plan_.requirements();
		for (std::size_t index = 0U; index < requirements.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			output << R"({"descriptor_id":")" << requirements[index].id << R"(","version":")"
				   << requirements[index].version.string() << "\"}";
		}
		output << R"(],"schema":"cxxlens.calls-to-function-report.v1","state":")"
			   << state_name(state_) << "\"}";
		return output.str();
	}

	call_search_recipe::call_search_recipe(std::string qualified_name)
		: qualified_name_{std::move(qualified_name)},
		  descriptor_{"cxxlens.recipes.calls_to_function",
					  {1U, 2U, 0U},
					  "Find direct C/C++ call sites by exact qualified function name"}
	{
	}

	const sdk::recipe_descriptor& call_search_recipe::descriptor() const noexcept
	{
		return descriptor_;
	}

	std::string_view call_search_recipe::qualified_name() const noexcept
	{
		return qualified_name_;
	}

	sdk::result<sdk::recipe_plan> call_search_recipe::lower() const
	{
		using entity = cc::relations::entity;
		using direct_target = cc::relations::call_direct_target;
		using call_site = cc::relations::call_site;

		auto entities = sdk::query::from<entity>();
		auto name_predicate = sdk::query::equals_present(
			sdk::query::col<entity::qualified_name>(), sdk::query::literal::utf8(qualified_name_));
		if (!entities)
			return sdk::unexpected(std::move(entities.error()));
		if (!name_predicate)
			return sdk::unexpected(std::move(name_predicate.error()));
		auto filtered_entities = std::move(*entities).where(std::move(*name_predicate));
		if (!filtered_entities)
			return sdk::unexpected(std::move(filtered_entities.error()));

		auto targets = sdk::query::from<direct_target>();
		auto target_join = sdk::query::equals_present(sdk::query::col<direct_target::target>(),
													  sdk::query::col<entity::entity_column>());
		if (!targets)
			return sdk::unexpected(std::move(targets.error()));
		if (!target_join)
			return sdk::unexpected(std::move(target_join.error()));
		auto with_targets =
			std::move(*filtered_entities).inner_join(std::move(*targets), std::move(*target_join));
		if (!with_targets)
			return sdk::unexpected(std::move(with_targets.error()));

		auto calls = sdk::query::from<call_site>();
		auto call_join = sdk::query::equals_present(sdk::query::col<direct_target::call>(),
													sdk::query::col<call_site::call>());
		if (!calls)
			return sdk::unexpected(std::move(calls.error()));
		if (!call_join)
			return sdk::unexpected(std::move(call_join.error()));
		auto joined = std::move(*with_targets).inner_join(std::move(*calls), std::move(*call_join));
		if (!joined)
			return sdk::unexpected(std::move(joined.error()));

		const std::array order{sdk::query::col<call_site::call>(),
							   sdk::query::col<entity::entity_column>()};
		auto ordered = std::move(*joined).order_by(order);
		if (!ordered)
			return sdk::unexpected(std::move(ordered.error()));
		const std::array projection{
			sdk::query::col<call_site::call>(),
			sdk::query::col<call_site::source>(),
			sdk::query::col<call_site::caller>(),
			sdk::query::col<direct_target::target>(),
			sdk::query::col<entity::qualified_name>(),
		};
		auto projected = std::move(*ordered).project(projection);
		if (!projected)
			return sdk::unexpected(std::move(projected.error()));
		return sdk::recipe_plan::lower(descriptor_, std::move(*projected).finish());
	}

	sdk::result<call_search_report>
	call_search_recipe::run(sdk::snapshot_handle snapshot,
							const sdk::query::execution_request request) const
	{
		auto plan = lower();
		if (!plan)
			return sdk::unexpected(std::move(plan.error()));
		auto engine = sdk::query::reference_engine::bind(std::move(snapshot));
		if (!engine)
			return sdk::unexpected(std::move(engine.error()));
		auto result = engine->execute(plan->query(), request);
		if (!result)
			return sdk::unexpected(std::move(result.error()));
		auto state = classify(*result);
		if (!state)
			return sdk::unexpected(std::move(state.error()));
		return call_search_report{std::move(*plan), std::move(*result), *state};
	}

	sdk::result<call_search_recipe> calls_to_function(std::string qualified_name)
	{
		if (qualified_name.empty())
			return sdk::unexpected(sdk::error{"sdk.recipe-invalid", "qualified_name", "empty"});
		return call_search_recipe{std::move(qualified_name)};
	}
} // namespace cxxlens::recipes
