#pragma once

#include <memory>

#include <cxxlens/facts.hpp>

#include "store_port.hpp"

namespace cxxlens::detail
{
	struct fact_store_access
	{
		[[nodiscard]] static fact_store
		make_store(std::shared_ptr<const store::snapshot_data> snapshot, path workspace_root = {});
		[[nodiscard]] static fact make_fact(facts::detached_fact_record record);
		[[nodiscard]] static result<type_ref> make_type(const facts::detached_fact_record& record);
		[[nodiscard]] static call_site enrich_call(const call_site& value,
												   std::optional<symbol_id> static_target,
												   std::vector<symbol_id> possible_targets,
												   confidence certainty,
												   result_guarantee guarantee,
												   evidence why);
	};
} // namespace cxxlens::detail
