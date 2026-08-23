#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk
{
	class snapshot_store;

	// These declarations intentionally live in test support rather than the installed SDK.  The
	// definitions exercise persisted corruption and recovery paths; they are not product actions.
	[[nodiscard]] result<void>
	mark_publication_corrupt_for_testing(snapshot_store& store, std::string_view publication_id);
	[[nodiscard]] result<void>
	rewrite_publication_payload_for_testing(snapshot_store& store,
											std::string_view publication_id,
											std::string_view before,
											std::string_view after,
											std::size_t occurrence);
	[[nodiscard]] result<void>
	rewrite_publication_payload_schema_for_testing(snapshot_store& store,
											   std::string_view publication_id,
											   std::uint8_t payload_version);
	[[nodiscard]] result<void>
	rewrite_publication_identity_field_for_testing(snapshot_store& store,
													std::string_view publication_id,
													std::string_view field);
	[[nodiscard]] result<std::string>
	rewrite_snapshot_version_for_testing(snapshot_store& store,
											 std::string_view publication_id,
											 std::string_view component,
											 std::uint64_t wire_value,
											 std::uint32_t semantic_value);
	[[nodiscard]] result<std::string>
	rewrite_publication_counters_for_testing(snapshot_store& store,
												 std::string_view publication_id,
												 std::uint64_t sequence,
												 std::uint64_t generation);
[[nodiscard]] result<std::string>
	insert_noncommitted_publication_for_testing(snapshot_store& store,
													std::string_view source_publication_id,
													std::uint8_t payload_version);
[[nodiscard]] result<void>
	poison_rejected_generation_for_testing(snapshot_store& store,
											 std::string_view publication_id,
											 std::uint64_t generation);
} // namespace cxxlens::sdk
