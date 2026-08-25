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
#include "llvm/clang22/source_closure_task_v4.hpp"
#include "llvm/clang22/source_closure_task_v4_worker.hpp"

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
								std::vector<std::string> invocation_limitations = {},
								std::string_view toolchain_context_id = {});

	/**
	 * Authenticated source-closure state supplied by the Protocol 2.0 dispatcher.
	 *
	 * The runtime owns the concrete implementation.  In particular, `revalidate()` must verify
	 * the completed message-24..29 transfer and its cleanup/custody receipt; a caller must not
	 * implement this interface from a boolean or an untrusted task field.  Keeping this as a
	 * narrow port lets the worker consume the v2.2 authority without importing an obsolete task
	 * decoder into the new path.
	 */
	class provider_worker_v2_2_closure_authority
	{
	  public:
		virtual ~provider_worker_v2_2_closure_authority() = default;
		[[nodiscard]] virtual sdk::result<void> revalidate() const = 0;
		[[nodiscard]] virtual bool acknowledged() const noexcept = 0;
		[[nodiscard]] virtual std::string_view session_id() const noexcept = 0;
		[[nodiscard]] virtual std::string_view task_id() const noexcept = 0;
		[[nodiscard]] virtual std::string_view task_v4_digest() const noexcept = 0;
		[[nodiscard]] virtual std::string_view closure_id() const noexcept = 0;
		[[nodiscard]] virtual std::string_view closure_digest() const noexcept = 0;
		[[nodiscard]] virtual std::string_view transfer_digest() const noexcept = 0;
	};

	using provider_worker_v2_2_task_accepted_callback =
		std::move_only_function<sdk::result<void>(const source_closure_task_v4_identity&)>;

	/** Inputs which have already crossed the Protocol 2.0 task-v4/closure transport boundary. */
	struct provider_worker_v2_2_dispatch_input
	{
		source_closure_task_v4_worker_input task;
		const provider_worker_v2_2_closure_authority* closure{};
		/** Called only after closure acknowledgement and identity revalidation. */
		provider_worker_v2_2_task_accepted_callback task_accepted;
	};

	/**
	 * Execute one authenticated task-v4 closure through the exact Clang callback.
	 *
	 * This is the v2.2 worker entrypoint.  It rejects before `task_accepted` when the source
	 * closure has not reached the authenticated ACK state, and it decodes only task-v4 input.  The
	 * callback is reached only after the acceptance callback succeeds; no result is manufactured
	 * for a missing/foreign/failed closure transfer.
	 */
	[[nodiscard]] sdk::result<source_closure_task_v4_worker_receipt>
	run_provider_worker_v2_2(provider_worker_v2_2_dispatch_input input);

	/**
	 * Run the explicit Protocol 2.0 task-v4/source-closure ingress.
	 *
	 * The task envelope is read from `input`; source bytes are accepted only from the separately
	 * inherited closure channel.  A successful Clang translation still ends in `task_failed` until
	 * a recipe/output/publication authority is supplied, so this entrypoint cannot manufacture a
	 * Store result from execution alone.
	 */
	[[nodiscard]] int run_provider_worker_v4_source_closure(std::istream& input,
															std::ostream& output,
															int read_descriptor,
															int write_descriptor);
} // namespace cxxlens::detail::clang22
