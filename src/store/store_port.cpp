#include "store_port.hpp"

#include <algorithm>
#include <ranges>
#include <set>
#include <string>
#include <tuple>

namespace cxxlens::detail::store
{
	namespace
	{
		[[nodiscard]] error store_error(std::string code, std::string reason)
		{
			error failure;
			failure.code.value = std::move(code);
			failure.message = "Fact store operation failed";
			failure.scope = failure_scope::workspace;
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		[[nodiscard]] bool fact_less(const facts::detached_fact_record& left,
									 const facts::detached_fact_record& right)
		{
			return std::tuple{left.kind, left.stable_key, left.id.value()} <
				std::tuple{right.kind, right.stable_key, right.id.value()};
		}
	} // namespace

	result<void> snapshot_data::validate() const
	{
		if (metadata.workspace_key.empty() ||
			metadata.schema_version != "cxxlens.fact-snapshot.v1" ||
			metadata.semantics_version.empty() || metadata.adapter_id.empty() ||
			metadata.adapter_version.empty())
			return store_error("facts.cache-incompatible", "invalid-snapshot-metadata");
		if (auto checked = coverage.validate(); !checked)
			return store_error("facts.transaction-failed", "invalid-snapshot-coverage");
		if (!std::ranges::is_sorted(facts, fact_less))
			return store_error("facts.transaction-failed", "noncanonical-fact-order");
		std::set<std::string> ids;
		for (const auto& fact : facts)
		{
			if (auto checked = facts::validate(fact); !checked)
				return store_error("facts.transaction-failed", "invalid-detached-fact");
			if (!ids.insert(std::string{fact.id.value()}).second)
				return store_error("facts.transaction-failed", "duplicate-fact-id");
		}
		return {};
	}
} // namespace cxxlens::detail::store
