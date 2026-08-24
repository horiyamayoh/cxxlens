#pragma once

/**
 * @file closure.hpp
 * @brief Typed source-closure controls for protocol-2 message IDs 24-29.
 *
 * The wire control shapes intentionally follow the proposed source-closure
 * transport contract, but are independent value types.  No filesystem or
 * provider runtime is reached by this slice.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

	struct source_closure_manifest_descriptor_view
	{
		manifest_kind kind{manifest_kind::descriptor};
		std::string_view session_id;
		std::string_view task_id;
		std::string_view task_v4_digest;
		std::string_view closure_id;
		std::string_view closure_digest;
		std::string_view manifest_digest;
		std::uint64_t total_bytes{};
		std::uint64_t chunk_bytes{};
		std::uint64_t chunk_count{};
	};

	struct source_closure_manifest_chunk_view
	{
		manifest_kind kind{manifest_kind::chunk};
		std::string_view session_id;
		std::string_view task_id;
		std::string_view manifest_digest;
		std::uint64_t chunk_index{};
		std::uint64_t offset{};
		std::uint64_t byte_count{};
	};

	using source_closure_manifest_view =
		std::variant<source_closure_manifest_descriptor_view, source_closure_manifest_chunk_view>;

	struct source_closure_blob_descriptor_view
	{
		std::string_view session_id;
		std::string_view task_id;
		std::string_view closure_digest;
		std::uint64_t blob_ordinal{};
		std::string_view blob_digest;
		std::uint64_t total_bytes{};
		std::uint64_t chunk_bytes{};
		std::uint64_t chunk_count{};
	};

	struct source_closure_chunk_view
	{
		std::string_view session_id;
		std::string_view task_id;
		std::uint64_t blob_ordinal{};
		std::string_view blob_digest;
		std::uint64_t chunk_index{};
		std::uint64_t offset{};
		std::uint64_t byte_count{};
	};

	struct source_closure_seal_view
	{
		std::string_view session_id;
		std::string_view task_id;
		std::string_view task_v4_digest;
		std::string_view manifest_digest;
		std::string_view blob_receipts_digest;
		std::uint64_t blob_count{};
		std::uint64_t total_bytes{};
		std::string_view closure_digest;
		std::string_view transfer_digest;
	};

	struct source_closure_ack_view
	{
		std::string_view session_id;
		std::string_view task_id;
		std::string_view closure_digest;
		std::string_view transfer_digest;
		std::string_view spool_receipt;
		std::string_view cleanup_owner;
	};

	struct source_closure_reject_view
	{
		std::string_view session_id;
		std::string_view task_id;
		std::string_view failure_phase;
		std::string_view reason_code;
		std::span<const byte> observed_counters;
		std::string_view cleanup_receipt;
	};

	using closure_control_view = std::variant<source_closure_manifest_view,
											  source_closure_blob_descriptor_view,
											  source_closure_chunk_view,
											  source_closure_seal_view,
											  source_closure_ack_view,
											  source_closure_reject_view>;

	struct closure_text_range
	{
		std::uint32_t offset{};
		std::uint32_t size{};
	};

	struct closure_parsed_control
	{
		manifest_kind kind{manifest_kind::descriptor};
		std::array<closure_text_range, 15U> text{};
		std::array<std::uint64_t, 8U> number{};
		closure_text_range observed_counters{};
	};

	/** Move-only owner of one canonical, schema-validated closure control. */
	class closure_control_token final
	{
	  public:
		closure_control_token(const closure_control_token&) = delete;
		closure_control_token& operator=(const closure_control_token&) = delete;
		closure_control_token(closure_control_token&& other) noexcept;
		closure_control_token& operator=(closure_control_token&& other) noexcept;
		~closure_control_token() noexcept = default;

		[[nodiscard]] message_type type() const noexcept
		{
			return type_;
		}
		/** Borrowed view; invalid after moving/destroying this token and hidden after consume. */
		[[nodiscard]] std::optional<closure_control_view> value() const noexcept;
		[[nodiscard]] std::span<const byte> control_bytes() const noexcept
		{
			return control_;
		}
		[[nodiscard]] std::size_t control_capacity() const noexcept
		{
			return control_.capacity();
		}
		/** Token-owned bytes plus fixed parser/validator workspace at peak. */
		[[nodiscard]] std::size_t resident_bytes() const noexcept
		{
			return resident_bytes_;
		}
		[[nodiscard]] const digest32& control_digest() const noexcept
		{
			return control_digest_;
		}
		/**
		 * Consume once. The returned borrowed view remains valid only while this
		 * (now-consumed) token stays alive and unmoved.
		 */
		[[nodiscard]] sdk::result<closure_control_view> consume() &&;

	  private:
		friend sdk::result<closure_control_token> decode_closure_control_token(
			message_type, bytes&&, const bytes&, closure_limits, std::size_t);
		closure_control_token(message_type type,
							  bytes control,
							  closure_parsed_control parsed,
							  std::size_t resident_bytes,
							  digest32 control_digest) noexcept
			: type_{type}, control_{std::move(control)}, parsed_{parsed},
			  resident_bytes_{resident_bytes}, control_digest_{control_digest}
		{
		}

		message_type type_{};
		bytes control_;
		closure_parsed_control parsed_{};
		std::size_t resident_bytes_{};
		digest32 control_digest_{};
		bool consumed_{};
	};

	/**
	 * Concrete fixed storage that can overlap an adopted token while scanning,
	 * hashing, validating, returning a view, and wrapping the result. Dynamic
	 * control/payload capacities and the 104-byte header are accounted separately.
	 */
	inline constexpr std::size_t closure_control_decode_fixed_workspace_bytes =
		cbor::canonical_scan_workspace_bytes + sizeof(cbor::scan_result) + sha256_workspace_bytes +
		sizeof(digest32) + sizeof(closure_parsed_control) + sizeof(closure_control_view) +
		sizeof(std::optional<closure_control_view>) + sizeof(sdk::result<void>) +
		sizeof(sdk::result<closure_parsed_control>) + sizeof(sdk::result<closure_control_view>) +
		sizeof(sdk::result<closure_control_token>);

	[[nodiscard]] sdk::result<bytes> encode_closure_control(message_type type,
															const closure_control& control,
															closure_limits bound = {});

	[[nodiscard]] sdk::result<closure_control> decode_closure_control(message_type type,
																	  std::span<const byte> control,
																	  closure_limits bound = {});

	/**
	 * Adopt the caller's final control vector and validate it without a DOM or
	 * canonical-output copy. The decoder reads the payload vector's actual
	 * capacity, always includes the 104-byte header, and adds caller-owned fixed
	 * state supplied by `caller_fixed_resident_bytes`.
	 */
	[[nodiscard]] sdk::result<closure_control_token>
	decode_closure_control_token(message_type type,
								 bytes&& control,
								 const bytes& payload,
								 closure_limits bound = {},
								 std::size_t caller_fixed_resident_bytes = sizeof(frame));

	/** @brief Enforce empty descriptor/seal/ack/reject or exact chunk payload bytes. */
	[[nodiscard]] sdk::result<void> validate_closure_payload(message_type type,
															 const closure_control& control,
															 std::span<const byte> payload,
															 closure_limits bound = {});
	[[nodiscard]] sdk::result<void> validate_closure_payload(message_type type,
															 const closure_control_view& control,
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
		/** First sequence is part of the transfer identity and is never inferred. */
		std::uint64_t first_sequence{};
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
		class prepared_ack_transition;

		static sdk::result<closure_transfer> create(closure_session session);

		closure_transfer() = delete;
		closure_transfer(const closure_transfer&) = delete;
		closure_transfer& operator=(const closure_transfer&) = delete;
		closure_transfer(closure_transfer&& other) noexcept;
		closure_transfer& operator=(closure_transfer&& other) noexcept;

		/** Compatibility entry; production receivers adopt control through a token. */
		[[nodiscard]] sdk::result<void> accept(const frame& value);
		/** Consume exactly one adopted, schema-validated control without decoding again. */
		[[nodiscard]] sdk::result<void> accept_decoded(const frame& metadata_and_payload,
													   closure_control_token&& control);

		/**
		 * Build the complete outbound ACK wire and its fixed transition before I/O.
		 * Dropping or aborting the result leaves the live transfer unchanged.
		 */
		[[nodiscard]] sdk::result<prepared_ack_transition>
		prepare_acknowledgement(const source_closure_ack& value, std::uint64_t sequence) const;
		/** Commit after successful wire emission; stale/replayed/foreign values are no-ops. */
		void commit_acknowledgement(prepared_ack_transition&& transition) noexcept;
		[[nodiscard]] closure_phase phase() const noexcept
		{
			return state_.phase;
		}
		[[nodiscard]] std::uint64_t manifest_bytes() const noexcept
		{
			return state_.manifest_observed;
		}
		[[nodiscard]] std::uint64_t blob_bytes() const noexcept
		{
			return state_.blob_observed_total;
		}
		[[nodiscard]] std::uint64_t blob_count() const noexcept
		{
			return state_.next_blob_ordinal;
		}
		[[nodiscard]] std::uint64_t next_sequence() const noexcept
		{
			return state_.sequence.next_sequence();
		}

		[[nodiscard]] const closure_session& session() const noexcept
		{
			return session_;
		}

	  private:
		struct fixed_text
		{
			std::array<char, 96U> storage{};
			std::uint8_t size{};

			[[nodiscard]] bool assign(std::string_view value) noexcept;
			[[nodiscard]] std::string_view view() const noexcept
			{
				return {storage.data(), size};
			}
		};

		struct mutable_state
		{
			sequence_guard sequence;
			credit_window credit;
			closure_phase phase{closure_phase::task_v4_sealed};
			std::uint64_t manifest_total{};
			std::uint64_t manifest_chunk_bytes{};
			std::uint64_t manifest_chunk_count{};
			std::uint64_t manifest_next_chunk{};
			std::uint64_t manifest_observed{};
			std::uint64_t next_blob_ordinal{};
			std::uint64_t current_blob_ordinal{};
			fixed_text current_blob_digest;
			std::uint64_t current_blob_total{};
			std::uint64_t current_blob_chunk_bytes{};
			std::uint64_t current_blob_chunk_count{};
			std::uint64_t current_blob_next_chunk{};
			std::uint64_t current_blob_observed{};
			std::uint64_t blob_observed_total{};
			fixed_text sealed_transfer_digest;
		};

		closure_transfer(closure_session session, mutable_state state) noexcept
			: session_{std::move(session)}, state_{state}
		{
		}

		[[nodiscard]] sdk::result<mutable_state>
		prepare_transition(const frame& value,
						   const closure_control_view& control,
						   std::size_t control_bytes,
						   const digest32& control_digest,
						   bool consume_credit) const;
		[[nodiscard]] sdk::result<void> bind_common(std::string_view session_id,
													std::string_view task_id,
													std::string_view closure_digest = {}) const;

		closure_session session_;
		mutable_state state_;
		std::uint64_t generation_{1U};
		bool active_{true};
	};

	/** Move-only authority for one already-encoded ACK transition. */
	class closure_transfer::prepared_ack_transition final
	{
	  public:
		prepared_ack_transition(const prepared_ack_transition&) = delete;
		prepared_ack_transition& operator=(const prepared_ack_transition&) = delete;
		prepared_ack_transition(prepared_ack_transition&& other) noexcept;
		prepared_ack_transition& operator=(prepared_ack_transition&& other) noexcept;
		~prepared_ack_transition() noexcept = default;

		[[nodiscard]] std::span<const byte> wire_bytes() const noexcept
		{
			return wire_;
		}
		[[nodiscard]] std::size_t resident_bytes() const noexcept
		{
			return sizeof(prepared_ack_transition) + wire_.capacity();
		}
		void abort() noexcept;

	  private:
		friend class closure_transfer;
		prepared_ack_transition(const closure_transfer* owner,
								std::uint64_t generation,
								mutable_state next,
								bytes wire) noexcept
			: owner_{owner}, generation_{generation}, next_{next}, wire_{std::move(wire)}
		{
		}

		const closure_transfer* owner_{};
		std::uint64_t generation_{};
		mutable_state next_;
		bytes wire_;
		bool consumed_{};
	};
} // namespace cxxlens::protocol_v2
