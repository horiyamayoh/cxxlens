#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <cxxlens/select.hpp>
#include <cxxlens/workspace.hpp>

namespace cxxlens::detail::query
{
	enum class stage_kind : std::uint8_t
	{
		resolve_symbol,
		fact_index_scan,
		variant_join,
		hierarchy_closure,
		override_closure,
		candidate_filter,
		ast_refinement,
		confidence_assign,
		evidence_assemble,
		deduplicate_sort_limit
	};

	struct query_stage
	{
		std::string id;
		stage_kind kind{stage_kind::fact_index_scan};
		std::vector<std::string> inputs;
		std::vector<fact_kind> required_facts;
		std::uint64_t estimated_cost{};
		bool optional{};
		auto operator<=>(const query_stage&) const = default;
	};

	struct compile_options
	{
		analysis_scope scope{analysis_scope::all()};
		std::size_t candidate_budget{100000U};
		std::size_t refinement_budget{1000U};
		std::optional<std::size_t> result_limit;
		bool allow_targeted_refinement{true};
	};

	struct query_plan
	{
		std::string schema{"cxxlens.query-plan.v1"};
		std::string selector_json;
		std::string reusable_subexpression_key;
		std::string snapshot_key;
		select::selector_requirements requirements;
		compile_options options;
		std::vector<query_stage> stages;

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string to_json() const;
	};

	[[nodiscard]] result<query_plan> compile(const select::semantic_selector& selector,
											 compile_options options,
											 std::string snapshot_key);
} // namespace cxxlens::detail::query
