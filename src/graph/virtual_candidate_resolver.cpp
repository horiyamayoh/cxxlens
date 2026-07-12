#include "virtual_candidate_resolver.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

#include "../core/canonical_json.hpp"
#include "../core/json_projections.hpp"

namespace cxxlens::detail::graph
{
	namespace
	{
		using json_value = cxxlens::detail::json::json_value;

		[[nodiscard]] error resolver_error(std::string reason)
		{
			error failure;
			failure.code.value = "core.internal-invariant-violation";
			failure.message = "virtual candidate resolution invariant failed";
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		void canonicalize(std::vector<symbol_id>& values)
		{
			std::ranges::sort(values,
							  {},
							  [](const symbol_id& value)
							  {
								  return value.value();
							  });
			values.erase(std::unique(values.begin(), values.end()), values.end());
		}

		[[nodiscard]] unresolved
		uncertainty(const unresolved_kind kind, std::string code, std::string input = {})
		{
			unresolved item;
			item.kind = kind;
			item.stable_code = std::move(code);
			item.summary = "candidate resolution is partial";
			if (!input.empty())
				item.missing_inputs.push_back(std::move(input));
			return item;
		}

		[[nodiscard]] std::string_view status_name(const resolution_status value)
		{
			constexpr std::array names{"exact", "partial", "unresolved"};
			return names.at(static_cast<std::size_t>(value));
		}

		[[nodiscard]] std::string_view confidence_name(const confidence value)
		{
			constexpr std::array names{"speculative", "possible", "probable", "high", "certain"};
			return names.at(static_cast<std::size_t>(value));
		}

		[[nodiscard]] std::string_view guarantee_name(const result_guarantee value)
		{
			constexpr std::array names{"exact_within_coverage",
									   "sound_over_approximation",
									   "sound_under_approximation",
									   "best_effort",
									   "heuristic"};
			return names.at(static_cast<std::size_t>(value));
		}

		[[nodiscard]] json_value symbol_array(const std::vector<symbol_id>& values)
		{
			json_value::array output;
			for (const auto& value : values)
				output.emplace_back(std::string{value.value()});
			return output;
		}

		[[nodiscard]] json_value unresolved_array(const std::vector<unresolved>& values)
		{
			json_value::array output;
			for (const auto& value : values)
				output.emplace_back(
					json_value::object{{"kind", static_cast<std::uint64_t>(value.kind)},
									   {"stable_code", value.stable_code},
									   {"missing_inputs",
										[&]
										{
											json_value::array inputs;
											for (const auto& input : value.missing_inputs)
												inputs.emplace_back(input);
											return json_value{std::move(inputs)};
										}()}});
			return output;
		}

		struct one_resolution
		{
			std::optional<symbol_id> static_target;
			std::vector<symbol_id> candidates;
			dispatch_form form{dispatch_form::dependent};
			std::vector<unresolved> unresolved_items;
		};

		[[nodiscard]] one_resolution resolve_one(const std::vector<override_edge>& overrides,
												 std::optional<symbol_id> static_target,
												 std::vector<symbol_id> observed,
												 const dispatch_form form,
												 const bool receiver_is_final,
												 const bool closed_world,
												 const bool inheritance_complete,
												 const bool override_complete,
												 const std::size_t budget,
												 const std::stop_token& cancellation)
		{
			one_resolution output{std::move(static_target), {}, form, {}};
			if (form == dispatch_form::virtual_open || form == dispatch_form::indirect ||
				form == dispatch_form::dependent)
				output.candidates = std::move(observed);
			if (output.static_target)
				output.candidates.push_back(*output.static_target);
			canonicalize(output.candidates);
			if (output.candidates.size() > budget)
			{
				output.candidates.resize(budget);
				output.unresolved_items.push_back(
					uncertainty(unresolved_kind::dataflow_budget_exceeded,
								"search.candidate-budget-exhausted"));
			}
			if (cancellation.stop_requested())
			{
				output.unresolved_items.push_back(
					uncertainty(unresolved_kind::custom, "core.cancelled"));
				canonicalize(output.candidates);
				return output;
			}

			const bool expands = form == dispatch_form::virtual_open && !receiver_is_final;
			if (expands && output.static_target)
			{
				std::vector<symbol_id> work{*output.static_target};
				std::set<std::string> visited;
				while (!work.empty())
				{
					auto current = std::move(work.back());
					work.pop_back();
					if (!visited.insert(std::string{current.value()}).second)
						continue;
					for (const auto& edge : overrides)
						if (edge.overridden_method == current)
						{
							output.candidates.push_back(edge.overriding_method);
							work.push_back(edge.overriding_method);
							if (output.candidates.size() > budget)
							{
								output.candidates.resize(budget);
								output.unresolved_items.push_back(
									uncertainty(unresolved_kind::dataflow_budget_exceeded,
												"search.candidate-budget-exhausted"));
								work.clear();
								break;
							}
						}
				}
			}
			if (form == dispatch_form::dependent)
				output.unresolved_items.push_back(
					uncertainty(unresolved_kind::dependent_type, "search.dependent-call-target"));
			if (form == dispatch_form::indirect)
				output.unresolved_items.push_back(
					uncertainty(unresolved_kind::function_pointer_target_unknown,
								"search.indirect-call-target"));
			if (expands && !override_complete)
				output.unresolved_items.push_back(
					uncertainty(unresolved_kind::capability_unavailable,
								"search.override-coverage-incomplete",
								"override_relation"));
			if (expands && !inheritance_complete)
				output.unresolved_items.push_back(
					uncertainty(unresolved_kind::capability_unavailable,
								"search.inheritance-coverage-incomplete",
								"inheritance"));
			if (expands && !closed_world)
				output.unresolved_items.push_back(
					uncertainty(unresolved_kind::open_world_virtual_target,
								"search.open-world-virtual-target"));
			if (!output.static_target && form != dispatch_form::indirect)
				output.unresolved_items.push_back(uncertainty(unresolved_kind::unresolved_overload,
															  "search.static-target-unresolved"));
			canonicalize(output.candidates);
			return output;
		}

		[[nodiscard]] std::vector<symbol_id>
		intersect_sets(const std::vector<per_variant_candidates>& variants)
		{
			if (variants.empty())
				return {};
			auto output = variants.front().candidates;
			for (std::size_t index = 1U; index < variants.size(); ++index)
			{
				std::vector<symbol_id> intersection;
				std::ranges::set_intersection(
					output,
					variants.at(index).candidates,
					std::back_inserter(intersection),
					{},
					[](const symbol_id& value)
					{
						return value.value();
					},
					[](const symbol_id& value)
					{
						return value.value();
					});
				output = std::move(intersection);
			}
			return output;
		}
	} // namespace

	result<void> candidate_resolution::validate() const
	{
		if (schema != "cxxlens.virtual-candidate-resolution.v1" || call_key.empty() ||
			!std::ranges::is_sorted(possible_targets,
									{},
									[](const symbol_id& value)
									{
										return value.value();
									}) ||
			std::ranges::adjacent_find(possible_targets) != possible_targets.end())
			return resolver_error("candidate-order-invalid");
		if ((static_target && !static_target->valid()) ||
			std::ranges::any_of(possible_targets,
								[](const symbol_id& value)
								{
									return !value.valid();
								}))
			return resolver_error("candidate-identity-invalid");
		if (!std::ranges::is_sorted(per_variant,
									{},
									[](const per_variant_candidates& value)
									{
										return value.variant.value();
									}) ||
			std::ranges::adjacent_find(
				per_variant,
				[](const per_variant_candidates& left, const per_variant_candidates& right)
				{
					return left.variant == right.variant;
				}) != per_variant.end())
			return resolver_error("variant-order-invalid");
		for (const auto& variant : per_variant)
			if (!variant.variant.valid() ||
				!std::ranges::is_sorted(variant.candidates,
										{},
										[](const symbol_id& value)
										{
											return value.value();
										}) ||
				std::ranges::adjacent_find(variant.candidates) != variant.candidates.end())
				return resolver_error("variant-candidate-order-invalid");
		if (!std::ranges::is_sorted(unresolved_items, {}, &unresolved::stable_code) ||
			std::ranges::adjacent_find(unresolved_items,
									   [](const unresolved& left, const unresolved& right)
									   {
										   return left.stable_code == right.stable_code;
									   }) != unresolved_items.end())
			return resolver_error("unresolved-order-invalid");
		if (status == resolution_status::exact && !unresolved_items.empty())
			return resolver_error("exact-resolution-has-unresolved");
		if (status == resolution_status::unresolved && !possible_targets.empty())
			return resolver_error("unresolved-resolution-has-candidates");
		if (why.items().empty())
			return resolver_error("evidence-missing");
		return {};
	}

	std::string candidate_resolution::to_json() const
	{
		json_value::array variants;
		for (const auto& value : per_variant)
			variants.emplace_back(
				json_value::object{{"candidates", symbol_array(value.candidates)},
								   {"variant", std::string{value.variant.value()}}});
		json_value document{cxxlens::detail::json::envelope(
			{schema},
			{{"call_key", call_key},
			 {"certainty", std::string{confidence_name(certainty)}},
			 {"evidence", cxxlens::detail::json::evidence_value(why)},
			 {"guarantee", std::string{guarantee_name(guarantee)}},
			 {"per_variant", std::move(variants)},
			 {"possible_targets", symbol_array(possible_targets)},
			 {"static_target",
			  static_target ? json_value{std::string{static_target->value()}} : json_value{}},
			 {"status", std::string{status_name(status)}},
			 {"unresolved", unresolved_array(unresolved_items)}})};
		return cxxlens::detail::json::write(document).value();
	}

	virtual_candidate_resolver::virtual_candidate_resolver(std::vector<override_edge> overrides)
		: overrides_{std::move(overrides)}
	{
		std::ranges::sort(overrides_,
						  {},
						  [](const override_edge& edge)
						  {
							  return std::pair{std::string{edge.overridden_method.value()},
											   std::string{edge.overriding_method.value()}};
						  });
	}

	candidate_resolution virtual_candidate_resolver::resolve(candidate_request request) const
	{
		candidate_resolution output;
		output.call_key = std::move(request.call_key);
		output.static_target = request.static_target;
		std::vector<unresolved> unresolved_items;
		if (request.variants.empty())
		{
			auto resolved =
				resolve_one(overrides_,
							request.static_target,
							std::move(request.observed_candidates),
							request.receiver_is_final ? dispatch_form::final_virtual : request.form,
							request.receiver_is_final,
							request.closed_world,
							request.inheritance_coverage_complete,
							request.override_coverage_complete,
							request.candidate_budget,
							request.cancellation);
			output.possible_targets = std::move(resolved.candidates);
			unresolved_items = std::move(resolved.unresolved_items);
		}
		else
		{
			std::ranges::sort(
				request.variants,
				[](const variant_call_observation& left, const variant_call_observation& right)
				{
					return left.variant.value() < right.variant.value();
				});
			for (auto& variant : request.variants)
			{
				auto resolved = resolve_one(overrides_,
											std::move(variant.static_target),
											std::move(variant.observed_candidates),
											variant.form,
											request.receiver_is_final,
											request.closed_world,
											request.inheritance_coverage_complete,
											request.override_coverage_complete,
											request.candidate_budget,
											request.cancellation);
				output.per_variant.push_back(
					{std::move(variant.variant), std::move(resolved.candidates)});
				unresolved_items.insert(unresolved_items.end(),
										resolved.unresolved_items.begin(),
										resolved.unresolved_items.end());
			}
			if (request.variant_policy == select::variant_match_policy::all_variants)
			{
				output.possible_targets = intersect_sets(output.per_variant);
				if (output.possible_targets.empty())
					unresolved_items.push_back(
						uncertainty(unresolved_kind::build_variant_disagreement,
									"search.no-common-variant-candidate"));
			}
			else
				for (const auto& variant : output.per_variant)
					output.possible_targets.insert(output.possible_targets.end(),
												   variant.candidates.begin(),
												   variant.candidates.end());
			canonicalize(output.possible_targets);
			const bool disagreement =
				std::ranges::adjacent_find(
					output.per_variant,
					[](const per_variant_candidates& left, const per_variant_candidates& right)
					{
						return left.candidates != right.candidates;
					}) != output.per_variant.end();
			if (disagreement &&
				request.variant_policy == select::variant_match_policy::reject_disagreement)
				unresolved_items.push_back(uncertainty(unresolved_kind::build_variant_disagreement,
													   "search.variant-candidate-disagreement"));
		}

		std::ranges::sort(unresolved_items,
						  {},
						  [](const unresolved& value)
						  {
							  return value.stable_code;
						  });
		unresolved_items.erase(std::unique(unresolved_items.begin(),
										   unresolved_items.end(),
										   [](const unresolved& left, const unresolved& right)
										   {
											   return left.stable_code == right.stable_code;
										   }),
							   unresolved_items.end());
		output.unresolved_items = std::move(unresolved_items);
		if (output.possible_targets.empty())
		{
			output.status = resolution_status::unresolved;
			output.certainty = confidence::possible;
			output.guarantee = result_guarantee::best_effort;
		}
		else if (!output.unresolved_items.empty())
		{
			output.status = resolution_status::partial;
			output.certainty = confidence::probable;
			output.guarantee = result_guarantee::sound_under_approximation;
		}
		else
		{
			output.status = resolution_status::exact;
			output.certainty = confidence::certain;
			output.guarantee = result_guarantee::exact_within_coverage;
		}
		evidence_item evidence_row;
		evidence_row.kind = evidence_kind::call_resolution;
		evidence_row.summary = "candidate rule table applied";
		evidence_row.attributes.emplace("rule_table", "cxxlens.virtual-dispatch.v1");
		evidence_row.attributes.emplace("status", std::string{status_name(output.status)});
		evidence_row.attributes.emplace("candidate_count",
										std::to_string(output.possible_targets.size()));
		output.why.add(std::move(evidence_row));
		return output;
	}
} // namespace cxxlens::detail::graph
