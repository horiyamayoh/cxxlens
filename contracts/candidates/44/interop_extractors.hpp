#pragma once

/**
 * @file interop_extractors.hpp
 * @brief Issue #44 Contract Candidate declarations for custom Clang fact extraction.
 *
 * This non-installed fragment completes the exact public contract without changing production
 * behavior. Issue #53 owns integration into the stable interop header and #54 owns freezing.
 */

#include <memory>
#include <optional>
#include <string>

#include <cxxlens/facts.hpp>
#include <cxxlens/interop/clang.hpp>

namespace cxxlens::interop
{
	/** @brief Detached, versioned custom fact input accepted by `fact_sink`.
	 *
	 * Identity is the tuple `(provider_namespace, schema_id, schema_version.major,
	 * semantic_key, source)`. `payload_json` is canonical JSON validated against the exact
	 * registered schema before it can enter a fact snapshot. It must not contain a Clang/LLVM
	 * pointer, address, source-manager token, pretty-type-only identity, or operational metadata.
	 */
	struct custom_fact
	{
		/** @brief Reverse-domain namespace owned by exactly one registered extractor. */
		std::string provider_namespace;
		/** @brief Exact custom schema ID within the provider namespace. */
		std::string schema_id;
		/** @brief Independent schema version; no adjacent-version fallback is permitted. */
		semantic_version schema_version;
		/** @brief Stable semantic identity, never a display name or pretty type alone. */
		std::string semantic_key;
		/** @brief Canonical schema-valid detached JSON object. */
		std::string payload_json;
		/** @brief Optional normalized source identity copied out of the live frontend job. */
		std::optional<source_span> source;
	};

	/** @brief Callback-scoped transactional sink for detached built-in and custom facts.
	 *
	 * The sink is valid only during one extractor call on its owning thread. Calls stage values in
	 * the current observation batch; any validation failure rejects that extractor's staged rows
	 * without discarding rows from independent extractors.
	 */
	class fact_sink
	{
	  public:
		/** @brief Stage a detached built-in fact after identity and snapshot validation. */
		[[nodiscard]] result<void> emit(fact value);
		/** @brief Validate exact namespace/schema ownership and stage one detached custom fact. */
		[[nodiscard]] result<void> emit_custom(custom_fact value);
		/** @brief Attach typed evidence to the current extractor batch; prose is never authoritative. */
		[[nodiscard]] result<void> emit_evidence(evidence_item value);
		/** @brief Preserve explicit partiality for the current compile unit and variant. */
		[[nodiscard]] result<void> mark_partial(unresolved value);

	  private:
		struct state;
		explicit fact_sink(state* value) noexcept : state_{value} {}
		state* state_{};
	};

	/** @brief Registered synchronous extractor using only callback-scoped Clang borrows. */
	class clang_fact_extractor
	{
	  public:
		virtual ~clang_fact_extractor() = default;
		/** @brief Return the stable reverse-domain extractor ID used for ownership and provenance. */
		[[nodiscard]] virtual std::string id() const = 0;
		/** @brief Return the extractor semantics version independently of custom schema versions. */
		[[nodiscard]] virtual semantic_version version() const = 0;
		/** @brief Synchronously copy detached facts; neither argument may escape or cross threads. */
		[[nodiscard]] virtual result<void> extract(borrowed_clang_tu& unit, fact_sink& sink) = 0;
	};

	/** @brief Register one extractor and its declared namespace/schema ownership atomically.
	 * @retval value Opaque workspace-scoped token on success.
	 * @note Duplicate IDs, namespace/schema conflicts, invalid versions, or registration after a
	 * workspace operation starts return structured errors; no first-wins behavior is allowed.
	 */
	[[nodiscard]] result<std::string> register_extractor(
		workspace& workspace, std::shared_ptr<clang_fact_extractor> extractor);

	/** @brief Remove an idle registered extractor by exact workspace-scoped token.
	 * @note Unknown, stale, cross-workspace, or currently executing tokens fail explicitly. A
	 * successful return guarantees that no future operation can acquire the extractor.
	 */
	[[nodiscard]] result<void> unregister_extractor(workspace& workspace, std::string token);
} // namespace cxxlens::interop
