#pragma once

/**
 * @file closure.hpp
 * @brief Typed source-closure controls for protocol-2 message IDs 24-29.
 *
 * The wire control shapes intentionally follow the proposed source-closure
 * transport contract, but are independent value types.  No filesystem or
 * provider runtime is reached by this slice.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>

#include "codec.hpp"

namespace cxxlens::protocol_v2
{
	inline constexpr std::size_t max_closure_members = 4'096U;
	inline constexpr std::size_t max_closure_blobs = 4'096U;
	inline constexpr std::size_t max_closure_manifest_bytes = 41'943'040U;
	inline constexpr std::size_t max_closure_blob_bytes = 16'777'216U;
	inline constexpr std::size_t max_closure_unique_blob_bytes = 50'331'648U;
	inline constexpr std::size_t max_closure_chunk_payload_bytes = 1'048'576U;
	inline constexpr std::size_t max_closure_manifest_chunks = 40U;
	inline constexpr std::size_t max_closure_chunks_per_blob = 16U;
	inline constexpr std::size_t max_closure_blob_chunk_frames = 4'144U;
	inline constexpr std::size_t max_closure_task_spool_bytes = 92'274'688U;
	inline constexpr std::size_t max_closure_resident_transport_bytes = 1'310'720U;

	struct closure_limits
	{
		std::size_t maximum_members{max_closure_members};
		std::size_t maximum_blobs{max_closure_blobs};
		std::size_t maximum_manifest_bytes{max_closure_manifest_bytes};
		std::size_t maximum_blob_bytes{max_closure_blob_bytes};
		std::size_t maximum_unique_blob_bytes{max_closure_unique_blob_bytes};
		std::size_t maximum_chunk_payload_bytes{max_closure_chunk_payload_bytes};
		std::size_t maximum_manifest_chunks{max_closure_manifest_chunks};
		std::size_t maximum_chunks_per_blob{max_closure_chunks_per_blob};
		std::size_t maximum_blob_chunk_frames{max_closure_blob_chunk_frames};
		std::size_t maximum_task_spool_bytes{max_closure_task_spool_bytes};
		std::size_t maximum_resident_transport_bytes{max_closure_resident_transport_bytes};
	};

	enum class manifest_kind : std::uint8_t
	{
		descriptor,
		chunk,
	};

	struct source_closure_manifest_descriptor
	{
		manifest_kind kind{manifest_kind::descriptor};
		std::string session_id;
		std::string task_id;
		std::string task_v4_digest;
		std::string closure_id;
		std::string closure_digest;
		std::string manifest_digest;
		std::uint64_t total_bytes{};
		std::uint64_t chunk_bytes{};
		std::uint64_t chunk_count{};

		[[nodiscard]] bool operator==(const source_closure_manifest_descriptor&) const = default;
	};

	struct source_closure_manifest_chunk
	{
		manifest_kind kind{manifest_kind::chunk};
		std::string session_id;
		std::string task_id;
		std::string manifest_digest;
		std::uint64_t chunk_index{};
		std::uint64_t offset{};
		std::uint64_t byte_count{};

		[[nodiscard]] bool operator==(const source_closure_manifest_chunk&) const = default;
	};

	using source_closure_manifest =
		std::variant<source_closure_manifest_descriptor, source_closure_manifest_chunk>;

	struct source_closure_blob_descriptor
	{
		std::string session_id;
		std::string task_id;
		std::string closure_digest;
		std::uint64_t blob_ordinal{};
		std::string blob_digest;
		std::uint64_t total_bytes{};
		std::uint64_t chunk_bytes{};
		std::uint64_t chunk_count{};

		[[nodiscard]] bool operator==(const source_closure_blob_descriptor&) const = default;
	};

	struct source_closure_chunk
	{
		std::string session_id;
		std::string task_id;
		std::uint64_t blob_ordinal{};
		std::string blob_digest;
		std::uint64_t chunk_index{};
		std::uint64_t offset{};
		std::uint64_t byte_count{};

		[[nodiscard]] bool operator==(const source_closure_chunk&) const = default;
	};

	struct source_closure_seal
	{
		std::string session_id;
		std::string task_id;
		std::string task_v4_digest;
		std::string manifest_digest;
		std::string blob_receipts_digest;
		std::uint64_t blob_count{};
		std::uint64_t total_bytes{};
		std::string closure_digest;
		std::string transfer_digest;

		[[nodiscard]] bool operator==(const source_closure_seal&) const = default;
	};

	struct source_closure_ack
	{
		std::string session_id;
		std::string task_id;
		std::string closure_digest;
		std::string transfer_digest;
		std::string spool_receipt;
		std::string cleanup_owner;

		[[nodiscard]] bool operator==(const source_closure_ack&) const = default;
	};

	struct source_closure_reject
	{
		std::string session_id;
		std::string task_id;
		std::string failure_phase;
		std::string reason_code;
		cbor::map observed_counters;
		std::string cleanup_receipt;

		[[nodiscard]] bool operator==(const source_closure_reject&) const = default;
	};

	using closure_control = std::variant<source_closure_manifest,
										 source_closure_blob_descriptor,
										 source_closure_chunk,
										 source_closure_seal,
										 source_closure_ack,
										 source_closure_reject>;

	[[nodiscard]] sdk::result<bytes> encode_closure_control(message_type type,
															const closure_control& control,
															closure_limits bound = {});

	[[nodiscard]] sdk::result<closure_control> decode_closure_control(message_type type,
																	  std::span<const byte> control,
																	  closure_limits bound = {});

	/** @brief Enforce empty descriptor/seal/ack/reject or exact chunk payload bytes. */
	[[nodiscard]] sdk::result<void> validate_closure_payload(message_type type,
															 const closure_control& control,
															 std::span<const byte> payload,
															 closure_limits bound = {});

	enum class closure_phase : std::uint8_t
	{
		task_v4_sealed,
		manifest_open,
		manifest_streaming,
		manifest_validated,
		blob_streaming,
		closure_sealed,
		acknowledged,
		rejected,
	};

	/** @brief Session binding and optional explicit credit for a closure transfer. */
	struct closure_session
	{
		std::string session_id;
		std::string task_id;
		std::string task_v4_digest;
		std::string closure_digest;
		std::string manifest_digest;
		std::uint64_t stream_id{1U};
		credit initial_credit{max_closure_task_spool_bytes, max_closure_blob_chunk_frames};
		closure_limits limits{};
	};

	/**
	 * @brief Fail-closed source-closure state machine over already decoded frames.
	 *
	 * The state owns no file bytes and never performs ambient filesystem access.
	 * It validates frame sequence, typed bindings, chunk shape, credit, and the
	 * bounded transfer counters before making a state transition.
	 */
	class closure_transfer
	{
	  public:
		static sdk::result<closure_transfer> create(closure_session session);

		closure_transfer() = delete;
		closure_transfer(const closure_transfer&) = default;
		closure_transfer& operator=(const closure_transfer&) = default;

		[[nodiscard]] sdk::result<void> accept(const frame& value);
		[[nodiscard]] closure_phase phase() const noexcept
		{
			return phase_;
		}
		[[nodiscard]] std::uint64_t manifest_bytes() const noexcept
		{
			return manifest_observed_;
		}
		[[nodiscard]] std::uint64_t blob_bytes() const noexcept
		{
			return blob_observed_total_;
		}
		[[nodiscard]] std::uint64_t blob_count() const noexcept
		{
			return next_blob_ordinal_;
		}

		[[nodiscard]] const closure_session& session() const noexcept
		{
			return session_;
		}

	  private:
		closure_transfer(closure_session session, sequence_guard sequence, credit_window credit)
			: session_{std::move(session)}, sequence_{std::move(sequence)},
			  credit_{std::move(credit)}
		{
		}

		[[nodiscard]] sdk::result<void> accept_impl(const frame& value);
		[[nodiscard]] sdk::result<void> bind_common(std::string_view session_id,
													std::string_view task_id,
													std::string_view closure_digest = {}) const;

		closure_session session_;
		sequence_guard sequence_;
		credit_window credit_;
		closure_phase phase_{closure_phase::task_v4_sealed};
		std::uint64_t manifest_total_{};
		std::uint64_t manifest_chunk_bytes_{};
		std::uint64_t manifest_chunk_count_{};
		std::uint64_t manifest_next_chunk_{};
		std::uint64_t manifest_observed_{};
		std::uint64_t next_blob_ordinal_{};
		std::uint64_t current_blob_ordinal_{};
		std::string current_blob_digest_;
		std::uint64_t current_blob_total_{};
		std::uint64_t current_blob_chunk_bytes_{};
		std::uint64_t current_blob_chunk_count_{};
		std::uint64_t current_blob_next_chunk_{};
		std::uint64_t current_blob_observed_{};
		std::uint64_t blob_observed_total_{};
	};
} // namespace cxxlens::protocol_v2
