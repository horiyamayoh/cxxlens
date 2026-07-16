#include "ng_legacy_fact_store_adapter.hpp"

#include <algorithm>
#include <tuple>

namespace cxxlens::detail::store
{
	namespace
	{
		[[nodiscard]] std::string coverage_state_name(const cxxlens::coverage_state state)
		{
			switch (state)
			{
				case cxxlens::coverage_state::covered:
					return "covered";
				case cxxlens::coverage_state::excluded:
				case cxxlens::coverage_state::not_applicable:
					return "not_covered";
				case cxxlens::coverage_state::failed:
				case cxxlens::coverage_state::unresolved:
					return "unresolved";
			}
			return "unknown";
		}
	} // namespace

	sdk::result<sdk::partition_draft>
	adapt_legacy_fact_snapshot(const snapshot_data& source,
							   const legacy_migration_request& request,
							   const legacy_fact_mapper& mapper)
	{
		if (auto valid = source.validate(); !valid)
			return sdk::unexpected(
				{"store.legacy-source-invalid", "snapshot", valid.error().code.value});
		if (request.relation_descriptor_id.empty() || request.scope.empty() ||
			request.interpretation.empty() || request.producer_semantics.empty() ||
			request.producer_input_basis_digest.empty() || request.precision_profile.empty() ||
			request.assumption_set_id.empty())
			return sdk::unexpected({"store.legacy-request-invalid", "request", {}});

		sdk::partition_draft output;
		output.relation_descriptor_id = request.relation_descriptor_id;
		output.scope = request.scope;
		output.condition = request.condition;
		output.interpretation = request.interpretation;
		output.producer_semantics = request.producer_semantics;
		output.producer_input_basis_digest = request.producer_input_basis_digest;
		output.precision_profile = request.precision_profile;
		output.assumption_set_id = request.assumption_set_id;
		for (const auto& fact : source.facts)
		{
			auto mapped = mapper.map(fact);
			if (!mapped)
				return sdk::unexpected(std::move(mapped.error()));
			if (mapped->descriptor != request.relation_descriptor_id ||
				mapped->presence != request.condition ||
				mapped->interpretation != request.interpretation ||
				mapped->producer.semantic_contract != request.producer_semantics)
				return sdk::unexpected(
					{"store.legacy-mapping-mismatch", fact.stable_key, mapped->descriptor});
			output.claims.push_back(std::move(*mapped));
		}
		for (const auto& unit : source.coverage.units())
		{
			const auto state = coverage_state_name(unit.state);
			output.coverage.push_back(
				{unit.kind,
				 unit.id,
				 state,
				 state == "covered" ? std::string{} : unit.reason.value_or("legacy-unclassified")});
		}
		std::ranges::sort(output.claims, {}, &sdk::claim::content);
		std::ranges::sort(
			output.coverage,
			[](const sdk::snapshot_coverage_unit& left, const sdk::snapshot_coverage_unit& right)
			{
				return std::tie(left.domain, left.key, left.state, left.reason) <
					std::tie(right.domain, right.key, right.state, right.reason);
			});
		return output;
	}
} // namespace cxxlens::detail::store
