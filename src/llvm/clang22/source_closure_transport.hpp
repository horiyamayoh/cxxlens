#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::detail::clang22
{
	/** Source-closure frames use the sole accepted Protocol 2.0 minor. */
	inline constexpr std::uint16_t source_closure_protocol_minor = 0U;
	inline constexpr std::string_view source_closure_capability = "task-source-closure-v2";

	/** Reserved source-closure message IDs. Heartbeat 23 is owned by NG1. */
	enum class source_closure_message_id : std::uint16_t
	{
		manifest = 24U,
		blob = 25U,
		chunk = 26U,
		seal = 27U,
		ack = 28U,
		reject = 29U,
	};

	[[nodiscard]] constexpr bool is_source_closure_message_id(const std::uint16_t value) noexcept
	{
		return value >= 24U && value <= 29U;
	}

	/** Exact bounded values from ADR 0102. */
	struct source_closure_transport_limits
	{
		std::uint64_t maximum_members{4096U};
		std::uint64_t maximum_unique_blobs{4096U};
		std::uint64_t maximum_logical_path_bytes{4096U};
		std::uint64_t maximum_blob_bytes{16U * 1024U * 1024U};
		std::uint64_t maximum_unique_blob_bytes{48U * 1024U * 1024U};
		std::uint64_t maximum_manifest_bytes{40U * 1024U * 1024U};
		std::uint64_t maximum_manifest_chunks{40U};
		std::uint64_t maximum_chunk_payload_bytes{1024U * 1024U};
		std::uint64_t maximum_chunks_per_blob{16U};
		std::uint64_t maximum_blob_chunk_frames{4144U};
		std::uint64_t maximum_task_spool_bytes{88U * 1024U * 1024U};
		std::uint64_t maximum_resident_transport_bytes{1310720U};
	};

	struct source_closure_transfer_binding
	{
		std::string session_id;
		std::string task_id;
		std::string task_v4_digest;
		std::string closure_id;
		std::string closure_digest;
		std::string manifest_digest;
		std::uint64_t first_sequence{};
	};

	struct source_closure_manifest_descriptor
	{
		std::string session_id;
		std::string task_id;
		std::string task_v4_digest;
		std::string closure_id;
		std::string closure_digest;
		std::string manifest_digest;
		std::uint64_t total_bytes{};
		std::uint64_t chunk_bytes{};
		std::uint64_t chunk_count{};
	};

	struct source_closure_manifest_chunk
	{
		std::string session_id;
		std::string task_id;
		std::string manifest_digest;
		std::uint64_t chunk_index{};
		std::uint64_t offset{};
		std::uint64_t byte_count{};
	};

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
	};

	struct source_closure_blob_chunk
	{
		std::string session_id;
		std::string task_id;
		std::uint64_t blob_ordinal{};
		std::string blob_digest;
		std::uint64_t chunk_index{};
		std::uint64_t offset{};
		std::uint64_t byte_count{};
	};

	struct source_closure_manifest_summary
	{
		std::string closure_id;
		std::string closure_digest;
		std::string manifest_digest;
		std::uint64_t member_count{};
		std::uint64_t blob_count{};
		std::uint64_t total_blob_bytes{};
	};

	struct source_closure_blob_receipt
	{
		std::uint64_t blob_ordinal{};
		std::string blob_digest;
		std::uint64_t size_bytes{};
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
	};

	struct source_closure_ack
	{
		std::string session_id;
		std::string task_id;
		std::string closure_digest;
		std::string transfer_digest;
		std::string spool_receipt;
		std::string cleanup_owner;
	};

	/** Provider-issued credentials returned only by the authenticated spool backend. */
	struct source_closure_ack_credentials
	{
		std::string spool_receipt;
		std::string cleanup_owner;
		std::string transfer_digest;
	};

	struct source_closure_reject
	{
		std::string session_id;
		std::string task_id;
		std::string failure_phase;
		std::string reason_code;
		std::vector<std::pair<std::string, std::uint64_t>> observed_counters;
		std::string cleanup_receipt;
	};

	/** The transfer lifecycle; task-accepted is owned by the outer provider validator. */
	enum class source_closure_transfer_state : std::uint8_t
	{
		task_v4_sealed,
		manifest_open,
		manifest_streaming,
		manifest_validated,
		blob_open,
		blob_streaming,
		blob_sealed,
		closure_sealed,
		closure_acknowledged,
		rejected,
		local_terminal,
	};

	enum class source_closure_local_terminal : std::uint8_t
	{
		none,
		connection_lost,
		worker_crashed,
	};

	/** Outer task-v4 adapter; revalidation must cover inherited v2.1 authority before frame 24. */
	class source_closure_task_v4_authority
	{
	  public:
		virtual ~source_closure_task_v4_authority() = default;
		[[nodiscard]] virtual std::string_view task_id() const noexcept = 0;
		[[nodiscard]] virtual std::string_view task_v4_digest() const noexcept = 0;
		[[nodiscard]] virtual sdk::result<void> revalidate() const = 0;
	};

	/**
	 * Bounded sink owned by the task-local spool. It must not retain a complete closure in memory.
	 * `finish_manifest` is the semantic manifest-validation seam: the sink replays its own sealed
	 * manifest spool and returns the validated census, so the transport validator never guesses
	 * member/blob authority from a digest or a partial payload.
	 */
	class source_closure_transfer_sink
	{
	  public:
		virtual ~source_closure_transfer_sink() = default;
		[[nodiscard]] virtual sdk::result<void>
		begin_manifest(const source_closure_manifest_descriptor& descriptor) = 0;
		[[nodiscard]] virtual sdk::result<void>
		append_manifest(std::span<const std::byte> bytes) = 0;
		[[nodiscard]] virtual sdk::result<source_closure_manifest_summary>
		finish_manifest(std::string_view manifest_digest) = 0;
		[[nodiscard]] virtual sdk::result<void>
		begin_blob(const source_closure_blob_descriptor& descriptor) = 0;
		[[nodiscard]] virtual sdk::result<void> append_blob(std::span<const std::byte> bytes) = 0;
		[[nodiscard]] virtual sdk::result<void>
		finish_blob(const source_closure_blob_receipt& receipt) = 0;
		/** Finish the sealed spool and issue credentials bound to the supplied transfer digest. */
		[[nodiscard]] virtual sdk::result<source_closure_ack_credentials>
		finish_closure(std::string_view transfer_digest) = 0;
		/** Return the provider-issued cleanup receipt; no caller-supplied receipt is trusted. */
		[[nodiscard]] virtual sdk::result<std::string> cleanup() = 0;
	};

	/** Validate Protocol 2.0 capability before accepting any closure payload bytes. */
	[[nodiscard]] sdk::result<void>
	validate_source_closure_capability(std::uint16_t protocol_minor,
									   std::span<const std::string_view> capabilities);

	/** Validate source-closure frame ID and the closed zero-flags invariant. */
	[[nodiscard]] sdk::result<void> validate_source_closure_frame_header(std::uint16_t message_id,
																		 std::uint16_t flags);

	/** Digest the canonical ordered blob-receipt array used by message 27. */
	[[nodiscard]] sdk::result<std::string>
	source_closure_blob_receipts_digest(std::span<const source_closure_blob_receipt> receipts);

	/** Digest the exact message-27 transfer projection. */
	[[nodiscard]] sdk::result<std::string>
	source_closure_transfer_digest(const source_closure_transfer_binding& binding,
								   std::string_view blob_receipts_digest,
								   std::uint64_t blob_count,
								   std::uint64_t total_bytes);

	/** Validate one task-bound manifest/blob transfer under the active Protocol 2.0 profile. */
	class source_closure_transfer_validator
	{
	  public:
		source_closure_transfer_validator(source_closure_transfer_binding binding,
										  source_closure_task_v4_authority& authority,
										  source_closure_transfer_sink& sink,
										  source_closure_transport_limits limits = {});

		[[nodiscard]] sdk::result<void>
		begin_manifest(const source_closure_manifest_descriptor& descriptor,
					   std::uint64_t sequence);
		[[nodiscard]] sdk::result<void> manifest_chunk(const source_closure_manifest_chunk& control,
													   std::span<const std::byte> payload,
													   std::uint64_t sequence);
		[[nodiscard]] sdk::result<void> begin_blob(const source_closure_blob_descriptor& descriptor,
												   std::uint64_t sequence);
		[[nodiscard]] sdk::result<void> blob_chunk(const source_closure_blob_chunk& control,
												   std::span<const std::byte> payload,
												   std::uint64_t sequence);
		[[nodiscard]] sdk::result<void> seal(const source_closure_seal& value,
											 std::uint64_t sequence);
		[[nodiscard]] sdk::result<void> acknowledge(const source_closure_ack& value,
													std::uint64_t sequence);
		[[nodiscard]] sdk::result<void> reject(const source_closure_reject& value,
											   std::uint64_t sequence);
		[[nodiscard]] sdk::result<source_closure_reject> cancel();
		[[nodiscard]] sdk::result<source_closure_reject> timeout();
		[[nodiscard]] sdk::result<void> connection_lost(bool cancel_observed);
		[[nodiscard]] sdk::result<void> worker_crashed(bool cancel_observed);

		[[nodiscard]] source_closure_transfer_state state() const noexcept
		{
			return state_;
		}
		[[nodiscard]] std::uint64_t next_sequence() const noexcept
		{
			return next_sequence_;
		}
		[[nodiscard]] std::uint64_t completed_blobs() const noexcept
		{
			return completed_blobs_;
		}
		[[nodiscard]] std::uint64_t total_blob_bytes() const noexcept
		{
			return total_blob_bytes_;
		}
		[[nodiscard]] std::string_view transfer_digest() const noexcept
		{
			return transfer_digest_;
		}
		[[nodiscard]] source_closure_local_terminal local_terminal() const noexcept
		{
			return local_terminal_;
		}
		[[nodiscard]] bool cancel_observed() const noexcept
		{
			return cancel_observed_;
		}
		[[nodiscard]] std::span<const source_closure_blob_receipt> blob_receipts() const noexcept
		{
			return blob_receipts_;
		}

	  private:
		[[nodiscard]] sdk::result<void> sequence(std::uint64_t value);
		[[nodiscard]] sdk::result<void> ensure_identity(std::string_view session_id,
														std::string_view task_id) const;
		[[nodiscard]] sdk::result<void>
		fail(std::string code, std::string field, std::string detail = {});
		[[nodiscard]] sdk::result<void> validate_reject(const source_closure_reject& value) const;
		[[nodiscard]] sdk::result<std::string> cleanup_once();
		[[nodiscard]] sdk::result<source_closure_reject> make_terminal_reject(std::string reason);
		[[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> phase_counters() const;

		source_closure_transfer_binding binding_;
		source_closure_task_v4_authority* authority_{};
		source_closure_transfer_sink* sink_{};
		source_closure_transport_limits limits_;
		source_closure_transfer_state state_{source_closure_transfer_state::task_v4_sealed};
		std::uint64_t next_sequence_{};
		std::uint64_t declared_bytes_{};
		std::uint64_t declared_chunk_bytes_{};
		std::uint64_t declared_chunk_count_{};
		std::uint64_t next_chunk_index_{};
		std::uint64_t next_offset_{};
		std::uint64_t current_blob_ordinal_{};
		std::string current_blob_digest_;
		std::uint64_t completed_blobs_{};
		std::uint64_t total_blob_bytes_{};
		std::uint64_t manifest_bytes_{};
		std::uint64_t blob_chunk_frames_{};
		std::uint64_t manifest_blob_count_{};
		std::uint64_t manifest_member_count_{};
		std::uint64_t manifest_total_blob_bytes_{};
		std::vector<source_closure_blob_receipt> blob_receipts_;
		std::string transfer_digest_;
		source_closure_ack_credentials ack_credentials_;
		std::string cleanup_receipt_;
		bool cleanup_done_{};
		source_closure_local_terminal local_terminal_{source_closure_local_terminal::none};
		bool cancel_observed_{};
		std::array<std::uint32_t, 8U> blob_hash_state_{};
		std::array<std::byte, 64U> blob_hash_buffer_{};
		std::size_t blob_hash_buffer_size_{};
		std::uint64_t blob_hash_total_bytes_{};
	};
} // namespace cxxlens::detail::clang22
