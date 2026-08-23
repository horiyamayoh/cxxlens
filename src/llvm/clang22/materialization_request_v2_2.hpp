#pragma once

/** @file materialization_request_v2_2.hpp @brief Request-v2.2 metadata admission. */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "materialization_json.hpp"
#include "provider_task_v4.hpp"

namespace cxxlens::detail::clang22::materialization
{
	inline constexpr std::string_view materialization_request_v2_2_schema =
		"cxxlens.clang22-materialization-request.v2_2";
	inline constexpr std::string_view materialization_request_v2_2_version = "2.2.0";
	inline constexpr std::uint16_t materialization_protocol_v2_major = 2U;
	inline constexpr std::uint16_t materialization_protocol_v2_minor = 0U;

	/** Exact required capabilities for Protocol 2 closure-bearing requests. */
	[[nodiscard]] std::vector<std::string> materialization_request_v2_2_required_features();

	/** Request-level bounds; all are checked before a validated result is returned. */
	struct materialization_request_v2_2_limits
	{
		std::size_t maximum_closures{4096U};
		std::size_t maximum_tasks{4096U};
		std::uint64_t maximum_unique_blob_bytes{48U * 1024U * 1024U};
		provider_task_v4_limits task_limits{};
	};

	/**
	 * Request v2.2 metadata model.
	 *
	 * `inherited_authority` is the complete source-free authority projection. It is retained
	 * as typed JSON only so the request identity can cover fields owned by the existing admission
	 * admission path. The validator rejects `content_base64` and source-byte members recursively;
	 * no byte payload is represented by the C++ task/closure types.
	 */
	struct materialization_request_v2_2
	{
		std::string schema{materialization_request_v2_2_schema};
		std::string request_version{materialization_request_v2_2_version};
		std::uint16_t protocol_major{materialization_protocol_v2_major};
		std::uint16_t protocol_minor{materialization_protocol_v2_minor};
		std::string request_id;
		std::string request_digest;
		std::vector<std::string> required_features{
			materialization_request_v2_2_required_features()};
		std::string materialization_request_id;
		std::string semantic_request_digest;
		json_value inherited_authority{json_value::null()};
		std::vector<provider_task_v4_base_task> base_tasks;
		std::vector<source_closure_summary> source_closures;
		std::vector<provider_task_v4> task_extensions;
	};

	/** Move-owned result proving request identity, task/closure binding, and capability
	 * negotiation. */
	struct validated_materialization_request_v2_2
	{
		materialization_request_v2_2 request;
		std::vector<std::string> negotiated_features;
		std::uint64_t unique_blob_bytes{};

		[[nodiscard]] const materialization_request_v2_2& value() const noexcept
		{
			return request;
		}
	};

	/** Negotiate Protocol 2.0 and the two required request capabilities. */
	[[nodiscard]] sdk::result<std::vector<std::string>>
	negotiate_materialization_request_v2_2(std::uint16_t peer_protocol_major,
										   std::uint16_t peer_protocol_minor,
										   std::span<const std::string> advertised_features);

	/**
	 * Validate the JSON metadata envelope before a closure transport is opened.
	 *
	 * The installed materializer receives the request document before Protocol 2.0 source-closure
	 * frames.  This structural gate deliberately does not manufacture a typed request from JSON
	 * (the inherited authority remains owned by its producer); it checks the closed v2.2 shape,
	 * required metadata arrays, and the absence of source bytes so malformed input cannot reach a
	 * worker or Store effect while the transport handoff is pending.
	 */
	[[nodiscard]] sdk::result<void>
	validate_materialization_request_v2_2_document(const json_value& root);

	/** Derive request identity over source-free inherited authority and v4 extensions. */
	[[nodiscard]] sdk::result<std::string>
	derive_materialization_request_v2_2_digest(const materialization_request_v2_2& request);

	/** Validate request identity and cross-bind all base tasks, extensions, and summaries. */
	[[nodiscard]] sdk::result<validated_materialization_request_v2_2>
	validate_materialization_request_v2_2(materialization_request_v2_2 request,
										  std::span<const std::string> advertised_features,
										  materialization_request_v2_2_limits limits = {});

	/**
	 * Validate a request against manifests transferred after request admission.
	 * The manifest vector may be empty during the pre-transfer phase; when present it must be a
	 * complete one-to-one closure census and is checked before main-member authority is accepted.
	 */
	[[nodiscard]] sdk::result<validated_materialization_request_v2_2>
	validate_materialization_request_v2_2(materialization_request_v2_2 request,
										  std::span<const std::string> advertised_features,
										  std::span<const source_closure_manifest> manifests,
										  materialization_request_v2_2_limits limits = {});
} // namespace cxxlens::detail::clang22::materialization
