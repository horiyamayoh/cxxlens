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
	};
} // namespace cxxlens::detail
