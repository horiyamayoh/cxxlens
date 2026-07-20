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

#include "llvm/clang22/observation_v2.hpp"
#include "llvm/clang22/provider_task_v3.hpp"

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
		std::optional<materialization::observation_v2_primary_span> primary_span;
		std::vector<materialization::observation_v2_origin> origins;

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
		std::optional<materialization::observation_v2_task_authority> materialization_authority;

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/** @brief Canonical pre-normalization dedup key retaining macro spelling occurrences. */
	[[nodiscard]] std::string observation_dedup_key(const detached_observation& observation);

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

	/** Closed worker output slots in the contract's exact sealed-batch order. */
	enum class provider_output_slot : std::uint8_t
	{
		call_direct_target = 1,
		call_site = 2,
		entity = 3,
		call_observation = 4,
		entity_observation = 5,
		type_observation = 6,
	};

	/** One exact descriptor/dependency-group binding in the worker output plan. */
	struct provider_output_binding
	{
		provider_output_slot slot{provider_output_slot::call_direct_target};
		std::string descriptor_id;
		std::string dependency_group;

		[[nodiscard]] bool operator==(const provider_output_binding&) const = default;
	};

	/** Return canonical descriptors first, then observations, with no optional slots. */
	[[nodiscard]] std::vector<provider_output_binding> provider_output_plan();
	/** Fail closed on missing, duplicate, extra, reordered, or regrouped output slots. */
	[[nodiscard]] sdk::result<void>
	validate_provider_output_plan(std::span<const provider_output_binding> plan);

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
