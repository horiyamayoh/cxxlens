#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <cxxlens/sdk.hpp>

namespace cxxlens::test::sqlite_v3_scenario
{
	inline constexpr std::size_t large_value_count = 3U;
	inline constexpr std::size_t large_value_byte_count = 6U * 1024U * 1024U;
	inline constexpr std::size_t generator_scratch_byte_count = 64U * 1024U;

	struct store_projection
	{
		std::string snapshot_id;
		std::string publication_id;
		std::uint64_t sequence{};
		std::uint64_t physical_generation{};
		std::string canonical_export_digest;

		[[nodiscard]] bool operator==(const store_projection&) const = default;
	};

	struct exact_v2_scenario
	{
		store_projection prior;
		store_projection current;
		std::string current_canonical_export;
	};

	/** Construct the deterministic Store relation authority shared by unit and qualification runs.
	 */
	[[nodiscard]] cxxlens::sdk::relation_engine make_engine();
	[[nodiscard]] cxxlens::sdk::snapshot_series_selector
	selector(const cxxlens::sdk::relation_engine& engine);
	[[nodiscard]] cxxlens::sdk::snapshot_draft
	snapshot_draft(const cxxlens::sdk::relation_engine& engine,
				   std::optional<std::string> parent = std::nullopt);

	/** A small valid canonical-v5 partition suitable for format and precedence fixtures. */
	[[nodiscard]] cxxlens::sdk::partition_draft
	small_partition(const cxxlens::sdk::relation_engine& engine, bool reverse = false);

	/**
	 * Deterministic semantic input whose canonical-v5 encoding is independently required to exceed
	 * the actual 16 MiB SQLite limit. Every generated value and scratch block remains below 8 MiB;
	 * the Store's production streaming encoder remains the only canonical-v5 byte producer.
	 */
	[[nodiscard]] cxxlens::sdk::partition_draft
	bounded_large_v5_partition(const cxxlens::sdk::relation_engine& engine);

	[[nodiscard]] cxxlens::sdk::snapshot_handle
	publish_partition(cxxlens::sdk::snapshot_store& store,
					  const cxxlens::sdk::relation_engine& engine,
					  cxxlens::sdk::partition_draft partition,
					  std::optional<std::string> parent = std::nullopt);
	[[nodiscard]] cxxlens::sdk::snapshot_handle
	publish_small(cxxlens::sdk::snapshot_store& store,
				  const cxxlens::sdk::relation_engine& engine,
				  bool reverse = false,
				  std::optional<std::string> parent = std::nullopt);
	[[nodiscard]] cxxlens::sdk::snapshot_handle
	publish_bounded_large_v5(cxxlens::sdk::snapshot_store& store,
							 const cxxlens::sdk::relation_engine& engine,
							 std::optional<std::string> parent = std::nullopt);

	[[nodiscard]] store_projection
	capture_projection(cxxlens::sdk::snapshot_store& store,
					   const cxxlens::sdk::snapshot_handle& snapshot);
	[[nodiscard]] store_projection
	create_current_v3_scenario(const std::filesystem::path& path,
							   const cxxlens::sdk::relation_engine& engine);
	[[nodiscard]] exact_v2_scenario
	create_exact_v2_scenario(const std::filesystem::path& path,
							 const cxxlens::sdk::relation_engine& engine);
} // namespace cxxlens::test::sqlite_v3_scenario
