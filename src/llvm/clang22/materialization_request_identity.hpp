#pragma once

#include <memory>
#include <string>

#include <cxxlens/sdk/common.hpp>

#include "materialization_request_stream.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/** Exact v2.1 identities derived from the sealed raw request occurrence. */
	struct streamed_materialization_request_identity
	{
		std::string materialization_request_id;
		std::string request_digest;
		std::string semantic_request_digest;

		[[nodiscard]] bool
		operator==(const streamed_materialization_request_identity&) const = default;
	};

	/** Source-private digest construction seam for successful/error taxonomy fault tests. */
	class materialization_request_identity_digest_factory
	{
	  public:
		virtual ~materialization_request_identity_digest_factory() = default;
		[[nodiscard]] virtual std::unique_ptr<materialization_digest_accumulator> make_digest() = 0;
	};

	/**
	 * Derive both request projections without retaining the all-request, all-task, or source DOM.
	 *
	 * The caller supplies the immutable pass-one envelope and its sealed, spool-backed task index.
	 * Global authority is replayed once into its bounded window; tasks are replayed one at a time
	 * and each indexed `source.content_base64` token is decoded directly into the canonical hash
	 * stream. This projection helper does not perform selected-version full-schema validation; its
	 * result is not request-admission authority until the enclosing source-dependent validator has
	 * completed that earlier phase.
	 */
	[[nodiscard]] sdk::result<streamed_materialization_request_identity>
	derive_streamed_materialization_request_identity(
		materialization_replayable_spool& spool,
		const materialization_request_envelope& envelope,
		materialization_request_task_index& task_index);

	/** Dependency-injected identity derivation used only by private failure-taxonomy tests. */
	[[nodiscard]] sdk::result<streamed_materialization_request_identity>
	derive_streamed_materialization_request_identity(
		materialization_replayable_spool& spool,
		const materialization_request_envelope& envelope,
		materialization_request_task_index& task_index,
		materialization_request_identity_digest_factory& digest_factory);

	/**
	 * Derive the v2.1 identity and reject the first non-exact supplied root binding.
	 *
	 * Exact root bindings do not substitute for the caller's prior selected-version full-schema
	 * verdict.
	 */
	[[nodiscard]] sdk::result<streamed_materialization_request_identity>
	validate_streamed_materialization_request_identity(
		materialization_replayable_spool& spool,
		const materialization_request_envelope& envelope,
		materialization_request_task_index& task_index);

	/** Dependency-injected exact-binding validation used only by private taxonomy tests. */
	[[nodiscard]] sdk::result<streamed_materialization_request_identity>
	validate_streamed_materialization_request_identity(
		materialization_replayable_spool& spool,
		const materialization_request_envelope& envelope,
		materialization_request_task_index& task_index,
		materialization_request_identity_digest_factory& digest_factory);
} // namespace cxxlens::detail::clang22::materialization
