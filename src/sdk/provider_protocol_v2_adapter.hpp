#pragma once

/**
 * @file provider_protocol_v2_adapter.hpp
 * @brief The single SDK-provider entry point for Protocol 2.0 closure controls.
 *
 * The provider wire frame and message registry are owned by
 * `cxxlens::sdk::provider`.  The source-closure implementation predates that
 * public provider path, so this header exposes a deliberately small adapter
 * at the provider boundary.  Callers never pass a foreign frame to the
 * closure state machine; conversion is performed here after the provider
 * codec has checked the 104-byte header, checksums, version, and limits.
 *
 * This is source-private.  It is not a second wire protocol and does not
 * provide a compatibility path for Protocol 1.x or a downgrade.
 */

#include <span>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

#include "../protocol_v2/closure.hpp"
#include "provider_ng1_transport_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	/** Source-closure values owned by the Protocol 2.0 implementation. */
	using provider_protocol_v2_closure_limits = protocol_v2::closure_limits;
	using provider_protocol_v2_manifest_kind = protocol_v2::manifest_kind;
	using provider_protocol_v2_manifest_descriptor =
		protocol_v2::source_closure_manifest_descriptor;
	using provider_protocol_v2_manifest_chunk = protocol_v2::source_closure_manifest_chunk;
	using provider_protocol_v2_manifest = protocol_v2::source_closure_manifest;
	using provider_protocol_v2_blob = protocol_v2::source_closure_blob_descriptor;
	using provider_protocol_v2_chunk = protocol_v2::source_closure_chunk;
	using provider_protocol_v2_seal = protocol_v2::source_closure_seal;
	using provider_protocol_v2_ack = protocol_v2::source_closure_ack;
	using provider_protocol_v2_reject = protocol_v2::source_closure_reject;
	using provider_protocol_v2_control = protocol_v2::closure_control;
	using provider_protocol_v2_phase = protocol_v2::closure_phase;

	/** Session binding supplied by the task-v4 ingress before closure admission. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN provider_protocol_v2_session
	{
		std::string session_id;
		std::string task_id;
		std::string task_v4_digest;
		std::string closure_digest;
		std::string manifest_digest;
		std::uint64_t stream_id{1U};
		protocol_credit initial_credit{protocol_v2::max_closure_task_spool_bytes,
									   protocol_v2::max_closure_blob_chunk_frames};
		provider_protocol_v2_closure_limits limits{};

		[[nodiscard]] result<void> validate() const;
	};

	/** Encode a typed source-closure control through the SDK provider boundary. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<std::vector<std::byte>>
	encode_provider_protocol_v2_closure_control(message_type type,
												const provider_protocol_v2_control& control,
												provider_protocol_v2_closure_limits limits = {});

	/** Decode and validate a typed source-closure control through the SDK provider boundary. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<provider_protocol_v2_control>
	decode_provider_protocol_v2_closure_control(message_type type,
												std::span<const std::byte> control,
												provider_protocol_v2_closure_limits limits = {});

	/** Enforce empty control-only messages or exact chunk payload length. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<void>
	validate_provider_protocol_v2_closure_payload(message_type type,
												  const provider_protocol_v2_control& control,
												  std::span<const std::byte> payload,
												  provider_protocol_v2_closure_limits limits = {});

	/**
	 * Stateful Protocol 2.0 source-closure validator at the sdk::provider edge.
	 * The state is transactional: a failed frame leaves the previous state
	 * unchanged, including sequence and credit counters.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN provider_protocol_v2_closure_state
	{
	  public:
		[[nodiscard]] static result<provider_protocol_v2_closure_state>
		create(provider_protocol_v2_session session);

		[[nodiscard]] result<void> accept(const frame& value);
		[[nodiscard]] provider_protocol_v2_phase phase() const noexcept
		{
			return transfer_.phase();
		}
		[[nodiscard]] std::uint64_t manifest_bytes() const noexcept
		{
			return transfer_.manifest_bytes();
		}
		[[nodiscard]] std::uint64_t blob_bytes() const noexcept
		{
			return transfer_.blob_bytes();
		}
		[[nodiscard]] std::uint64_t blob_count() const noexcept
		{
			return transfer_.blob_count();
		}

	  private:
		explicit provider_protocol_v2_closure_state(protocol_v2::closure_transfer transfer)
			: transfer_{std::move(transfer)}
		{
		}

		protocol_v2::closure_transfer transfer_;
	};

	/** Protocol 2.0 heartbeat is the existing typed NG1 control, with no payload or flags. */
	using provider_protocol_v2_heartbeat_kind = ng1_heartbeat_kind;
	using provider_protocol_v2_heartbeat_control = ng1_heartbeat_control;
	using provider_protocol_v2_session_binding = ng1_session_binding;

	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<std::vector<std::byte>>
	encode_provider_protocol_v2_heartbeat_control(
		const provider_protocol_v2_heartbeat_control& value);
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<provider_protocol_v2_heartbeat_control>
	decode_provider_protocol_v2_heartbeat_control(std::span<const std::byte> control);
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<void>
	validate_provider_protocol_v2_heartbeat(
		const frame& value, const provider_protocol_v2_session_binding* expected_binding = nullptr);
} // namespace cxxlens::sdk::provider::detail
