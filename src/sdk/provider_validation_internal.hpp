#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

#include "provider_visibility_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	struct expected_provider_identity;

	enum class transcript_terminal_kind : std::uint8_t
	{
		failed,
		complete,
	};

	struct transcript_validation_request
	{
		std::string task_id;
		std::string provider_id;
		semantic_version provider_version;
		const manifest* provider_manifest{};
		std::span<const relation_descriptor> output_descriptors;
		protocol_credit output_credit;
		const execution_budget* budget{};
		bool require_handshake{};
		const expected_provider_identity* expected_provider{};
	};

	/** Closed provider/session/launcher authority supplied independently of provider stdout. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN expected_provider_identity
	{
		std::string provider_id;
		semantic_version provider_version;
		std::string provider_binary_digest;
		std::string provider_semantic_contract_digest;
		std::uint16_t protocol_major{1U};
		std::uint16_t protocol_minor{};
		std::vector<std::string> required_features;
		std::string sandbox_policy_digest;
		std::vector<std::string> offered_relations;

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] bool operator==(const expected_provider_identity&) const = default;
	};

	/** Replayable source used by the source-private host encoder without a whole-input vector. */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN replayable_host_input
	{
	  public:
		virtual ~replayable_host_input() = default;
		[[nodiscard]] virtual result<std::uint64_t> size() const = 0;
		/** Read at most output.size() bytes at offset; short reads are explicitly supported. */
		[[nodiscard]] virtual result<std::size_t> read_at(std::uint64_t offset,
														  std::span<std::byte> output) const = 0;
	};

	/** Sequential destination for validated logical input chunks. */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN host_input_chunk_sink
	{
	  public:
		virtual ~host_input_chunk_sink() = default;
		[[nodiscard]] virtual result<void> append(std::span<const std::byte> bytes) = 0;
	};

	/** Short-read byte source used to decode and validate at most one host frame at a time. */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN host_input_byte_source
	{
	  public:
		virtual ~host_input_byte_source() = default;
		/** Return zero only at EOF; implementations may otherwise return any positive short read.
		 */
		[[nodiscard]] virtual result<std::size_t> read(std::span<std::byte> output) = 0;
	};

	/** Exact negotiated host-input profile; feature activation is launcher authority. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN host_input_profile
	{
		host_transcript_expectation expectation;
		bool task_input_chunks_v1{};
	};

	/** Immutable input seal produced only after exact transition/length/digest validation. */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN sealed_host_input
	{
	  public:
		[[nodiscard]] const open_task_metadata& task() const noexcept;
		[[nodiscard]] protocol_credit credit() const noexcept;
		[[nodiscard]] std::uint16_t protocol_major() const noexcept;
		[[nodiscard]] std::uint16_t protocol_minor() const noexcept;
		[[nodiscard]] std::uint64_t total_bytes() const noexcept;
		[[nodiscard]] std::uint64_t chunk_bytes() const noexcept;
		[[nodiscard]] std::uint64_t chunk_count() const noexcept;
		[[nodiscard]] std::span<const std::string> ordered_chunk_digests() const noexcept;
		/** Digest of the exact task/input/chunk-digest projection used by transfer receipts. */
		[[nodiscard]] std::string_view ordered_chunk_digest_set_digest() const noexcept;

	  private:
		sealed_host_input(open_task_metadata task,
						  protocol_credit credit,
						  std::uint16_t protocol_major,
						  std::uint16_t protocol_minor,
						  std::uint64_t total_bytes,
						  std::uint64_t chunk_bytes,
						  std::vector<std::string> ordered_chunk_digests,
						  std::string ordered_chunk_digest_set_digest);

		open_task_metadata task_;
		protocol_credit credit_;
		std::uint16_t protocol_major_{};
		std::uint16_t protocol_minor_{};
		std::uint64_t total_bytes_{};
		std::uint64_t chunk_bytes_{};
		std::vector<std::string> ordered_chunk_digests_;
		std::string ordered_chunk_digest_set_digest_;

		friend result<sealed_host_input> encode_host_transcript_incremental(
			const host_input_profile&, protocol_credit, const replayable_host_input&, frame_sink&);
		friend result<sealed_host_input> validate_host_transcript_incremental(
			std::span<const frame>, const host_input_profile&, host_input_chunk_sink&);
		friend result<sealed_host_input> validate_host_transcript_stream(host_input_byte_source&,
																		 const host_input_profile&,
																		 host_input_chunk_sink&);
	};

	/** Stream exact minor-0/minor-1 frames to a sink while retaining at most one input chunk. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<sealed_host_input>
	encode_host_transcript_incremental(const host_input_profile& profile,
									   protocol_credit credit,
									   const replayable_host_input& input,
									   frame_sink& output);

	/** Validate exact host frames incrementally and append only authenticated ordered chunks. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<sealed_host_input>
	validate_host_transcript_incremental(std::span<const frame> frames,
										 const host_input_profile& profile,
										 host_input_chunk_sink& input);

	/** Decode one bounded frame at a time from arbitrary short reads and return one immutable seal.
	 */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<sealed_host_input>
	validate_host_transcript_stream(host_input_byte_source& source,
									const host_input_profile& profile,
									host_input_chunk_sink& input);

	struct transcript_validation_result;

	/**
	 * One descriptor batch reconstructed from a single validated columnar transcript pass.
	 *
	 * The constructor is validator-private so downstream code can consume, but cannot forge, an
	 * adoption value from diagnostic frames.  Every retained row passes strict descriptor row
	 * validation; domain identity is additionally required exactly when the descriptor declares a
	 * result column.  All accessors are const and all payload is value-owned.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN sealed_provider_batch
	{
	  public:
		[[nodiscard]] std::string_view task_id() const noexcept;
		[[nodiscard]] std::string_view descriptor_id() const noexcept;
		[[nodiscard]] std::string_view descriptor_digest() const noexcept;
		[[nodiscard]] std::string_view dependency_group_id() const noexcept;
		[[nodiscard]] std::string_view atomic_output_group_id() const noexcept;
		[[nodiscard]] std::string_view batch_id() const noexcept;
		[[nodiscard]] std::string_view batch_digest() const noexcept;
		[[nodiscard]] std::span<const std::string> ordered_chunk_digests() const noexcept;
		[[nodiscard]] std::span<const detached_row> rows() const noexcept;

	  private:
		sealed_provider_batch(std::string task_id,
							  std::string descriptor_id,
							  std::string descriptor_digest,
							  std::string dependency_group_id,
							  std::string atomic_output_group_id,
							  std::string batch_id,
							  std::string batch_digest,
							  std::vector<std::string> ordered_chunk_digests,
							  std::vector<detached_row> rows);

		std::string task_id_;
		std::string descriptor_id_;
		std::string descriptor_digest_;
		std::string dependency_group_id_;
		std::string atomic_output_group_id_;
		std::string batch_id_;
		std::string batch_digest_;
		std::vector<std::string> ordered_chunk_digests_;
		std::vector<detached_row> rows_;

		friend result<transcript_validation_result>
		validate_provider_transcript(const transcript_validation_request& request,
									 std::span<const frame> frames,
									 protocol_limits session_limits);
	};

	/** Complete immutable adoption value produced only for a successfully sealed transcript. */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN sealed_provider_transcript
	{
	  public:
		[[nodiscard]] std::span<const sealed_provider_batch> batches() const noexcept;
		[[nodiscard]] std::span<const coverage_unit> coverage() const noexcept;
		[[nodiscard]] std::span<const unresolved_item> unresolved() const noexcept;
		[[nodiscard]] std::span<const evidence_item> evidence() const noexcept;

	  private:
		sealed_provider_transcript(std::vector<sealed_provider_batch> batches,
								   std::vector<coverage_unit> coverage,
								   std::vector<unresolved_item> unresolved,
								   std::vector<evidence_item> evidence);

		std::vector<sealed_provider_batch> batches_;
		std::vector<coverage_unit> coverage_;
		std::vector<unresolved_item> unresolved_;
		std::vector<evidence_item> evidence_;

		friend result<transcript_validation_result>
		validate_provider_transcript(const transcript_validation_request& request,
									 std::span<const frame> frames,
									 protocol_limits session_limits);
	};

	/** Typed terminal plus optional adoption seal from the same validation pass. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN transcript_validation_result
	{
		transcript_terminal_kind kind{transcript_terminal_kind::failed};
		std::string reason;
		[[nodiscard]] const std::optional<sealed_provider_transcript>& sealed() const noexcept;
		[[nodiscard]] std::optional<sealed_provider_transcript> take_sealed() && noexcept;
		/** A row/domain validation error is retained without changing public transcript semantics.
		 */
		[[nodiscard]] const std::optional<error>& sealing_error() const noexcept;
		[[nodiscard]] std::optional<error> take_sealing_error() && noexcept;

	  private:
		std::optional<sealed_provider_transcript> sealed_;
		std::optional<error> sealing_error_;
		friend result<transcript_validation_result>
		validate_provider_transcript(const transcript_validation_request& request,
									 std::span<const frame> frames,
									 protocol_limits session_limits);
	};

	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<transcript_validation_result>
	validate_provider_transcript(const transcript_validation_request& request,
								 std::span<const frame> frames,
								 protocol_limits session_limits);
} // namespace cxxlens::sdk::provider::detail
