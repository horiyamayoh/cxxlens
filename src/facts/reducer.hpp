#pragma once

#include <string>
#include <vector>

#include <cxxlens/core/failure.hpp>

#include "../llvm/common/frontend_port.hpp"
#include "fact_contract.hpp"

namespace cxxlens::detail::facts
{
	enum class reduction_decision : std::uint8_t
	{
		merged,
		variant_split,
		conflict,
	};

	struct reduction_trace_row
	{
		std::string group_key;
		reduction_decision decision{reduction_decision::merged};
		std::vector<fact_id> facts;
		std::vector<compile_unit_id> contributors;
		std::vector<build_variant_id> variants;
		bool operator==(const reduction_trace_row&) const = default;
	};

	struct reduction_conflict
	{
		std::string id;
		std::string code;
		std::string group_key;
		std::vector<compile_unit_id> contributors;
		std::vector<build_variant_id> variants;
		bool operator==(const reduction_conflict&) const = default;
	};

	struct reduction_result
	{
		std::string schema{"cxxlens.reduction-trace.v1"};
		std::vector<detached_fact_record> facts;
		std::vector<reduction_trace_row> trace;
		std::vector<reduction_conflict> conflicts;
		std::vector<frontend::normalized_diagnostic> diagnostics;
		coverage_report coverage;

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string to_json() const;
	};

	[[nodiscard]] result<reduction_result>
	reduce_observations(std::vector<frontend::observation_batch> batches);
} // namespace cxxlens::detail::facts
