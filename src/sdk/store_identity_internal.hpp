#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/sdk/store.hpp>

namespace cxxlens::sdk::detail
{
	[[nodiscard]] inline canonical_value store_identity_text(std::string value)
	{
		return canonical_value::from_string(std::move(value));
	}

	[[nodiscard]] inline canonical_value
	store_identity_texts(const std::vector<std::string>& values)
	{
		std::vector<canonical_value> output;
		output.reserve(values.size());
		for (const auto& value : values)
			output.push_back(store_identity_text(value));
		return canonical_value::from_tuple(std::move(output));
	}

	[[nodiscard]] constexpr std::int64_t
	store_identity_counter_integer(const std::uint64_t value) noexcept
	{
		constexpr auto signed_max =
			static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
		if (value <= signed_max)
			return static_cast<std::int64_t>(value);
		return -1 - static_cast<std::int64_t>(std::numeric_limits<std::uint64_t>::max() - value);
	}

	/** Exact source-private implementation shared by Store and candidate preview validation. */
	[[nodiscard]] inline result<std::string>
	snapshot_manifest_identity(const snapshot_manifest& value)
	{
		std::vector<canonical_value> partitions;
		partitions.reserve(value.partitions.size());
		for (const auto& partition : value.partitions)
			partitions.push_back(
				canonical_value::from_tuple({store_identity_text(partition.partition_id),
											 store_identity_text(partition.content_digest),
											 store_identity_text(partition.coverage_digest)}));
		std::ranges::sort(partitions,
						  [](const canonical_value& left, const canonical_value& right)
						  {
							  return left.tuple.front().text < right.tuple.front().text;
						  });
		auto closures = value.closure_ids;
		std::ranges::sort(closures);
		const std::array fields{
			store_identity_text(value.snapshot_semantics_version.string()),
			store_identity_text(value.catalog_semantic_digest),
			store_identity_text(value.condition_universe_id),
			store_identity_text(value.relation_registry_digest),
			store_identity_text(value.interpretation_policy_digest),
			canonical_value::from_tuple(std::move(partitions)),
			store_identity_texts(closures),
		};
		return canonical_identity_digest("snapshot", fields);
	}

	/** Exact source-private publication identity shared before and after publication. */
	[[nodiscard]] inline result<std::string>
	publication_record_identity(const std::string& series_id,
								const std::string& snapshot_id,
								const std::uint64_t sequence,
								const std::optional<std::string>& parent_publication)
	{
		const std::array fields{
			store_identity_text(series_id),
			store_identity_text(snapshot_id),
			canonical_value::from_integer(store_identity_counter_integer(sequence)),
			store_identity_text(parent_publication.value_or("")),
		};
		return canonical_identity_digest("publication", fields);
	}
} // namespace cxxlens::sdk::detail
