#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <cxxlens/sdk/relation.hpp>

namespace cxxlens::detail::clang22::materialization
{
	/** Closed provider-native observation kinds accepted by the ADR-0096 v2 codec. */
	enum class observation_v2_kind : std::uint8_t
	{
		entity = 1,
		type = 2,
		call = 3,
	};

	/** One already decoded payload entry; callers must retain strict UTF-8 byte order. */
	struct observation_v2_payload_entry
	{
		std::string key;
		std::string value;
		[[nodiscard]] bool operator==(const observation_v2_payload_entry&) const = default;
	};

	/** Exact seven-field source.span authority bundle carried by entity/call observations. */
	struct observation_v2_primary_span
	{
		std::string span_id;
		std::string snapshot;
		std::string file;
		std::uint64_t begin{};
		std::uint64_t end{};
		std::string role;
		bool read_only{};
		[[nodiscard]] bool operator==(const observation_v2_primary_span&) const = default;
	};

	/** One immediate-to-outermost source-origin occurrence; duplicates are significant. */
	struct observation_v2_origin
	{
		std::string kind;
		std::string logical_path;
		std::int64_t begin{};
		std::int64_t end{};
		bool read_only{true};
		[[nodiscard]] bool operator==(const observation_v2_origin&) const = default;
	};

	/** Exact provider-native record under cxxlens.clang22.observation-native.v2. */
	struct native_observation_v2
	{
		observation_v2_kind kind{observation_v2_kind::entity};
		std::string final_relation_compile_unit_id;
		std::string semantic_key;
		std::vector<observation_v2_payload_entry> payload;
		std::optional<observation_v2_primary_span> primary_span;
		std::vector<observation_v2_origin> origin_chain;
		bool exact_equivalence{true};
		std::optional<std::string> limitation;
	};

	/** Task authority against which record-local compile-unit and span fields are rebound. */
	struct observation_v2_task_authority
	{
		std::string final_relation_compile_unit_id;
		std::string source_snapshot_id;
		std::string source_file_id;
		std::uint64_t source_size_bytes{};
	};

	/**
	 * Strict typed view decoded from one immutable sealed observation row.
	 *
	 * `payload_digest` remains an opaque descriptor-bound digest. The source payload map is
	 * intentionally not reconstructed from its one-way digest.
	 */
	struct decoded_observation_v2_row
	{
		observation_v2_kind kind{observation_v2_kind::entity};
		std::string final_relation_compile_unit_id;
		std::string semantic_key;
		std::string payload_digest;
		std::optional<observation_v2_primary_span> primary_span;
		std::vector<observation_v2_origin> origin_chain;
		bool exact_equivalence{true};
		std::optional<std::string> limitation;

		[[nodiscard]] bool operator==(const decoded_observation_v2_row&) const = default;
	};

	[[nodiscard]] const sdk::relation_descriptor& entity_observation_v2_descriptor();
	[[nodiscard]] const sdk::relation_descriptor& type_observation_v2_descriptor();
	[[nodiscard]] const sdk::relation_descriptor& call_observation_v2_descriptor();
	[[nodiscard]] sdk::result<const sdk::relation_descriptor*>
	observation_v2_descriptor(observation_v2_kind kind);

	/** Validate, project, identity-bind, and independently revalidate one native v2 row. */
	[[nodiscard]] sdk::result<sdk::detached_row>
	make_observation_v2_row(const native_observation_v2& record,
							const observation_v2_task_authority& task);

	/**
	 * Strictly decode one generic immutable-seal row without consulting raw provider frames.
	 *
	 * The exact descriptor, row/domain identity, task compile unit, optional source-span bundle,
	 * canonical origin bytes, semantic payload-digest spelling, and equivalence/limitation
	 * coupling are all revalidated before a value is returned.
	 */
	[[nodiscard]] sdk::result<decoded_observation_v2_row>
	decode_observation_v2_row(const sdk::detached_row& row,
							  const observation_v2_task_authority& task);
} // namespace cxxlens::detail::clang22::materialization
