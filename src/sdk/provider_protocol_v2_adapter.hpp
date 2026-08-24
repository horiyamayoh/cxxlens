#pragma once

/**
 * @file provider_protocol_v2_adapter.hpp
 * @brief The single SDK-provider entry point for Protocol 2.0 closure controls.
 *
 * `cxxlens::protocol_v2` owns the wire header, checksum, canonical-CBOR,
 * version, flag, sequence, and credit mechanics. This header is the small
 * conversion facade used by the public SDK value types and source-closure
 * state. No public call site parses or hashes a second wire representation.
 *
 * This is source-private and is not a second wire protocol or downgrade path.
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
	/** Encode through the sole Protocol 2 wire codec. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<std::vector<std::byte>>
	encode_provider_protocol_v2_frame(const frame& value, protocol_limits limits);

	/** Decode through the sole Protocol 2 wire codec and map stable SDK failures. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<frame>
	decode_provider_protocol_v2_frame(std::span<const std::byte> input, protocol_limits limits);

	/** Decode a bounded transcript through the same codec and failure mapping. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<std::vector<frame>>
	decode_provider_protocol_v2_frame_stream(std::span<const std::byte> input,
											 protocol_limits limits,
											 std::uint64_t maximum_frames);

	/** Header-validated, move-only frame awaiting direct reads into final body vectors. */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN prepared_provider_protocol_v2_frame
	{
	  public:
		prepared_provider_protocol_v2_frame(const prepared_provider_protocol_v2_frame&) = delete;
		prepared_provider_protocol_v2_frame&
		operator=(const prepared_provider_protocol_v2_frame&) = delete;
		prepared_provider_protocol_v2_frame(prepared_provider_protocol_v2_frame&&) noexcept =
			default;
		prepared_provider_protocol_v2_frame&
		operator=(prepared_provider_protocol_v2_frame&&) noexcept = default;

		[[nodiscard]] std::size_t control_bytes() const noexcept;
		[[nodiscard]] std::size_t payload_bytes() const noexcept;
		[[nodiscard]] std::size_t body_resident_bytes() const noexcept;
		[[nodiscard]] message_type type() const noexcept;
		[[nodiscard]] std::uint16_t flags() const noexcept;
		[[nodiscard]] std::uint64_t stream_id() const noexcept;
		[[nodiscard]] std::uint64_t sequence() const noexcept;
		[[nodiscard]] result<frame> finalize(std::vector<std::byte>&& control,
											 std::vector<std::byte>&& payload) &&;

	  private:
		friend result<prepared_provider_protocol_v2_frame> prepare_provider_protocol_v2_frame(
			std::span<const std::byte, protocol_v2::fixed_header_bytes>, protocol_limits);

		explicit prepared_provider_protocol_v2_frame(
			protocol_v2::prepared_frame_decode prepared) noexcept
			: prepared_{std::move(prepared)}
		{
		}

		protocol_v2::prepared_frame_decode prepared_;
	};

	/** Validate the exact 104-byte header before the receiver allocates its final body vectors. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<prepared_provider_protocol_v2_frame>
	prepare_provider_protocol_v2_frame(
		std::span<const std::byte, protocol_v2::fixed_header_bytes> header, protocol_limits limits);

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
