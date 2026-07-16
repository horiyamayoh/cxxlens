#pragma once

// Temporary one-way migration boundary. Remove with Issue #72.

#include <string>

#include <cxxlens/sdk/store.hpp>

#include "store_port.hpp"

namespace cxxlens::detail::store
{
	struct legacy_migration_request
	{
		std::string relation_descriptor_id;
		std::string scope;
		sdk::claim_condition condition;
		std::string interpretation;
		std::string producer_semantics;
		std::string producer_input_basis_digest;
		std::string precision_profile;
		std::string assumption_set_id;
	};

	class legacy_fact_mapper
	{
	  public:
		virtual ~legacy_fact_mapper() = default;
		[[nodiscard]] virtual sdk::result<sdk::claim>
		map(const facts::detached_fact_record& fact) const = 0;
	};

	[[nodiscard]] sdk::result<sdk::partition_draft>
	adapt_legacy_fact_snapshot(const snapshot_data& source,
							   const legacy_migration_request& request,
							   const legacy_fact_mapper& mapper);
} // namespace cxxlens::detail::store
