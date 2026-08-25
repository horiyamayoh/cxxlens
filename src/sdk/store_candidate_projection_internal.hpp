#pragma once

/**
 * @file store_candidate_projection_internal.hpp
 * @brief Value-owned source-private projection of a validated Store candidate.
 *
 * The projection is deliberately below the public SDK ABI. It contains only canonical bytes and
 * stable keys; no writer, backend pointer, or test mutation hook crosses the boundary.
 */

#include <string>
#include <vector>

#include <cxxlens/sdk/common.hpp>
#include <cxxlens/sdk/store.hpp>

namespace cxxlens::sdk::detail
{
	struct snapshot_candidate_projection_record
	{
		std::string kind;
		std::string key;
		std::vector<std::byte> payload;

		[[nodiscard]] bool operator==(const snapshot_candidate_projection_record&) const = default;
	};

	struct snapshot_candidate_projection
	{
		std::vector<snapshot_candidate_projection_record> records;

		[[nodiscard]] bool operator==(const snapshot_candidate_projection&) const = default;
	};

	/** Canonical payload encoders shared by expected and actual projection builders. */
	[[nodiscard]] result<std::vector<std::byte>>
	encode_snapshot_candidate_partition(const partition_draft& partition);
	[[nodiscard]] result<std::vector<std::byte>>
	encode_snapshot_candidate_binding(const snapshot_partition_binding& binding);
	[[nodiscard]] result<std::vector<std::byte>>
	encode_snapshot_candidate_closure(const closure_candidate& closure);
	[[nodiscard]] result<std::vector<std::byte>>
	encode_snapshot_candidate_manifest(const snapshot_manifest& manifest);
	[[nodiscard]] result<std::vector<std::byte>>
	encode_snapshot_candidate_unresolved(const unresolved_reference& unresolved);
} // namespace cxxlens::sdk::detail
