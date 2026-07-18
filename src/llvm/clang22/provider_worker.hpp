#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

namespace cxxlens::detail::clang22
{
	enum class observation_kind : std::uint8_t
	{
		entity = 1,
		type = 2,
		call = 3,
	};

	struct detached_observation
	{
		observation_kind kind{observation_kind::entity};
		std::string compile_unit;
		std::string semantic_key;
		std::map<std::string, std::string, std::less<>> payload;
		std::optional<std::string> source_span_id;
		std::vector<std::string> source_origin_chain;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] std::string canonical_form() const;
	};

	struct observation_batch
	{
		std::string unit;
		std::string variant;
		std::vector<detached_observation> observations;
		std::uint64_t failed_count{};
		std::vector<std::string> diagnostics;

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/** @brief Canonical pre-normalization dedup key retaining macro spelling occurrences. */
	[[nodiscard]] std::string observation_dedup_key(const detached_observation& observation);

	struct clang22_task_input
	{
		std::string compile_unit;
		std::string variant;
		std::string source_snapshot;
		std::string file;
		std::string logical_path;
		std::string source;
		std::vector<std::string> arguments;

		[[nodiscard]] sdk::result<void> validate() const;
	};

	struct declaration_identity_input
	{
		std::optional<std::string> usr;
		std::string toolchain_digest;
		std::string declaration_kind;
		std::string qualified_name;
		std::string canonical_signature;
		std::string template_identity;
		std::string constraint_identity;
		std::string declaration_context;
		std::string canonical_source_anchor;
	};

	struct declaration_identity
	{
		std::string semantic_key;
		std::string confidence;

		[[nodiscard]] bool operator==(const declaration_identity&) const = default;
	};

	struct canonicalized_provider_batch
	{
		std::vector<sdk::detached_row> entity_observations;
		std::vector<sdk::detached_row> type_observations;
		std::vector<sdk::detached_row> call_observations;
		std::vector<sdk::detached_row> entities;
		std::vector<sdk::detached_row> call_sites;
		std::vector<sdk::detached_row> direct_targets;
		std::vector<sdk::provider::unresolved_item> unresolved;
		bool exact_equivalence{};
		std::vector<std::string> equivalence_limitations;
	};

	[[nodiscard]] sdk::result<std::vector<std::byte>>
	encode_task_input(const clang22_task_input& input);
	[[nodiscard]] sdk::result<clang22_task_input>
	decode_task_input(std::span<const std::byte> input);

	[[nodiscard]] sdk::relation_descriptor entity_observation_descriptor();
	[[nodiscard]] sdk::relation_descriptor type_observation_descriptor();
	[[nodiscard]] sdk::relation_descriptor call_observation_descriptor();

	[[nodiscard]] bool invocation_has_exact_equivalence(std::span<const std::string> arguments,
														std::vector<std::string>& limitations);

	[[nodiscard]] sdk::result<declaration_identity>
	make_declaration_identity(const declaration_identity_input& input);

	[[nodiscard]] sdk::result<canonicalized_provider_batch>
	canonicalize_provider_batch(const observation_batch& batch,
								const std::string& toolchain_digest,
								bool invocation_exact,
								std::vector<std::string> invocation_limitations = {});

	[[nodiscard]] int run_provider_worker(std::span<const std::byte> input, std::ostream& output);
} // namespace cxxlens::detail::clang22
