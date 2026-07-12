#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <cxxlens/facts.hpp>
#include <cxxlens/workspace.hpp>

#include "../graph/virtual_candidate_resolver.hpp"
#include "predicate_evaluator.hpp"
#include "query_plan.hpp"
#include "refinement_port.hpp"

namespace cxxlens::detail::query
{
	struct reason_count
	{
		std::string reason_code;
		predicate_outcome outcome{predicate_outcome::unresolved};
		std::uint64_t count{};
		auto operator<=>(const reason_count&) const = default;
	};

	struct candidate_accounting
	{
		std::uint64_t considered{};
		std::uint64_t matched{};
		std::uint64_t rejected{};
		std::uint64_t unresolved{};
		[[nodiscard]] bool balanced() const noexcept
		{
			return considered == matched + rejected + unresolved;
		}
	};

	struct raw_call_match
	{
		fact_id call;
		call_site site;
		std::optional<symbol_id> static_target;
		std::vector<symbol_id> possible_targets;
		std::vector<graph::per_variant_candidates> per_variant;
		confidence certainty{confidence::possible};
		result_guarantee guarantee{result_guarantee::best_effort};
		evidence why;
		std::vector<unresolved> unresolved_items;
	};

	struct query_trace
	{
		std::string schema{"cxxlens.query-trace.v1"};
		std::string snapshot_key;
		candidate_accounting accounting;
		std::vector<reason_count> reasons;
		std::uint64_t refinements_requested{};
		std::uint64_t refinements_succeeded{};
		std::uint64_t refinements_failed{};
		bool cancelled{};
		bool budget_exhausted{};
		bool output_limited{};

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string to_json() const;
	};

	struct execution_result
	{
		std::vector<raw_call_match> matches;
		std::vector<fact_id> rejected;
		std::vector<unresolved> unresolved_items;
		coverage_report coverage;
		result_guarantee guarantee{result_guarantee::best_effort};
		query_trace trace;

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string to_json() const;
	};

	struct execution_options
	{
		analysis_scope scope{analysis_scope::all()};
		std::size_t candidate_budget{100000U};
		std::size_t refinement_budget{1000U};
		std::optional<std::size_t> result_limit;
		bool closed_world{};
		execution_context execution;
	};

	[[nodiscard]] result<execution_result> execute(const query_plan& plan,
												   const fact_store& store,
												   execution_options options,
												   targeted_refinement_port* refinement = nullptr);

	[[nodiscard]] result<execution_result>
	provision_compile_execute(const workspace& workspace,
							  const select::semantic_selector& selector,
							  execution_options options,
							  targeted_refinement_port* refinement = nullptr);
} // namespace cxxlens::detail::query
