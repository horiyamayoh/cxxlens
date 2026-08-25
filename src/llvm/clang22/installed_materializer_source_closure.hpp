#pragma once

/**
 * @file installed_materializer_source_closure.hpp
 * @brief Typed Protocol 2.0 source-closure ingress for the installed materializer.
 *
 * Request metadata arrives on stdin while source bytes arrive on the provider-owned inherited
 * channel. This boundary validates the complete source-free request identity before admitting any
 * closure frame. The returned value is the only input accepted by the worker/Store bridge; this
 * function itself stops before worker output and publication.
 */

#include "materialization_request_v2_2.hpp"
#include "source_closure_receiver.hpp"

namespace cxxlens::detail::clang22
{
	struct installed_materializer_source_closure_result
	{
		materialization::validated_materialization_request_v2_2 request;
		source_closure_transfer_binding binding;
		source_closure_receiver_result receiver;
	};

	/**
	 * Validate a complete request-v2.2 envelope and receive its task-bound source closure.
	 *
	 * The physical descriptor and identity values come from the provider process boundary only;
	 * they are checked against the typed request and are never treated as standalone authority.
	 * Missing ingress mode returns `source-closure.channel-required` so the installed CLI can
	 * retain its disconnected metadata-only failure response.
	 */
	[[nodiscard]] sdk::result<installed_materializer_source_closure_result>
	receive_installed_materializer_source_closure(const materialization::json_value& request_root);
} // namespace cxxlens::detail::clang22
