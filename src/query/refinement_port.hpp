#pragma once

#include <vector>

#include <cxxlens/facts.hpp>

#include "../graph/virtual_candidate_resolver.hpp"

namespace cxxlens::detail::query
{
	struct dispatch_refinement
	{
		graph::dispatch_form form{graph::dispatch_form::dependent};
		std::vector<symbol_id> possible_targets;
		bool receiver_is_final{};
		bool complete{};
	};

	class targeted_refinement_port
	{
	  public:
		virtual ~targeted_refinement_port() = default;
		[[nodiscard]] virtual result<dispatch_refinement>
		refine(const call_site& call, const execution_context& context) = 0;
	};
} // namespace cxxlens::detail::query
