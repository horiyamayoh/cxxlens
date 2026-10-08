#pragma once

#include <type_traits>
#include <utility>

#include <cxxlens/sdk/query.hpp>

namespace cxxlens::sdk::query::detail
{
	template <class Step>
	annotated_row copy_projected_row(const annotated_row& row, Step&& step)
	{
		// The caller has reserved the complete owned-row geometry before any
		// allocation here. Charge each actual copy rather than wire framing.
		const auto clone_text = [&](std::string_view value)
		{
			step(value.size() + 1U);
			return std::string{value};
		};
		const auto clone_list = [&]<class T>(const std::vector<T>& values, auto clone)
		{
			step(1U);
			std::vector<T> copied;
			copied.reserve(values.size());
			for (const auto& value : values)
			{
				step(1U);
				copied.push_back(clone(value));
			}
			return copied;
		};
		const auto clone_condition = [&](const claim_condition& condition)
		{
			step(1U);
			return claim_condition{clone_text(condition.universe),
								   clone_list(condition.fragments, clone_text)};
		};
		const auto clone_producer = [&](const claim_producer& producer)
		{
			step(1U);
			return claim_producer{clone_text(producer.id), clone_text(producer.semantic_contract)};
		};
		const auto clone_guarantee = [&](const claim_guarantee& guarantee)
		{
			step(1U);
			return claim_guarantee{clone_text(guarantee.approximation),
								   clone_text(guarantee.scope),
								   clone_text(guarantee.assumptions),
								   clone_list(guarantee.verification_modalities, clone_text)};
		};
		annotated_row copied;
		step(1U);
		copied.multiplicity = row.multiplicity;
		for (const auto& [name, original] : row.values)
		{
			step(4U); // Map node and the fixed kind, optionality and state.
			detached_cell value;
			value.type = {
				original.type.scalar, clone_text(original.type.parameter), original.type.optional};
			value.state = original.state;
			step(1U);
			if (original.value)
				value.value = std::visit(
					[&](const auto& payload) -> scalar_value
					{
						using type = std::remove_cvref_t<decltype(payload)>;
						if constexpr (std::is_same_v<type, std::string>)
							return clone_text(payload);
						else if constexpr (std::is_same_v<type, std::vector<std::byte>>)
						{
							step(payload.size() + 1U);
							return payload;
						}
						else
						{
							step(1U);
							return payload;
						}
					},
					*original.value);
			step(1U);
			if (original.unknown_reason)
				value.unknown_reason = clone_text(*original.unknown_reason);
			copied.values.emplace_hint(copied.values.end(), clone_text(name), std::move(value));
		}
		copied.presence = clone_condition(row.presence);
		copied.interpretation = clone_text(row.interpretation);
		copied.claim_contributors = clone_list(row.claim_contributors, clone_text);
		copied.producer_contracts = clone_list(row.producer_contracts, clone_producer);
		copied.provenance = clone_list(row.provenance, clone_text);
		copied.contributor_guarantees = clone_list(row.contributor_guarantees, clone_guarantee);
		copied.contributor_edges =
			clone_list(row.contributor_edges,
					   [&](const query_contributor_edge& edge)
					   {
						   step(1U);
						   return query_contributor_edge{clone_text(edge.claim_contributor),
														 clone_producer(edge.producer),
														 clone_text(edge.provenance),
														 clone_guarantee(edge.guarantee),
														 clone_condition(edge.condition),
														 clone_text(edge.interpretation)};
					   });
		return copied;
	}
} // namespace cxxlens::sdk::query::detail
