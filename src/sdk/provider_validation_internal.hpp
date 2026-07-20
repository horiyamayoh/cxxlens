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
	};

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
