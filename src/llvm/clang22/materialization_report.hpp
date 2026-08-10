#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_execution_journal.hpp"
#include "materialization_seal.hpp"
#include "sdk/provider_runtime_internal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/**
	 * Encode the exact compact-failure branch of materialization report v2.1.
	 *
	 * The only semantic input is a non-forgeable token issued by the consumed execution journal.
	 * No caller-constructed phase, effect ledger, or publication-dependent value is accepted.
	 */
	[[nodiscard]] sdk::result<std::string>
	encode_compact_failure_report(const compact_failure_authority& authority,
								  std::string generated_at);

	/** Bounded limits for the source-private detailed-report projection. */
	struct detailed_report_limits
	{
		std::size_t max_tasks{4096U};
		std::size_t max_batches_per_task{6U};
		std::size_t max_chunks_per_batch{65536U};
		std::size_t max_side_channel_records{65536U};
		std::size_t max_string_bytes{16U * 1024U * 1024U};
		std::size_t max_projection_bytes{64U * 1024U * 1024U};
	};

	/** Closed error taxonomy for detailed capture/encoding. */
	enum class detailed_report_error_kind : std::uint8_t
	{
		invalid_capture,
		limit_exceeded,
		missing_input_seal,
		missing_runtime_receipt,
		transcript_mismatch,
		publication_unverified,
		invalid_time,
	};

	/** Typed source-private error converted to sdk::error at the public boundary. */
	struct detailed_report_error
	{
		detailed_report_error_kind kind{detailed_report_error_kind::invalid_capture};
		std::string field;
		std::string detail;

		[[nodiscard]] sdk::error as_sdk_error() const;
	};

	struct detailed_provider_batch_projection
	{
		std::string task_id;
		std::string descriptor_id;
		std::string descriptor_digest;
		std::string dependency_group_id;
		std::string atomic_output_group_id;
		std::string batch_id;
		std::string batch_digest;
		std::vector<std::string> ordered_chunk_digests;
		std::uint64_t row_count{};
		std::string row_set_digest;
	};

	struct detailed_coverage_projection
	{
		std::string kind;
		std::string id;
		std::string state;
		std::string reason;
	};

	struct detailed_unresolved_projection
	{
		std::string code;
		std::string subject;
		std::string detail;
	};

	struct detailed_evidence_projection
	{
		std::string kind;
		std::string subject;
		std::string producer;
		std::string summary;
	};

	/**
	 * Owned, bounded evidence copied from one successful provider validation pass. Raw frames and
	 * row payloads are deliberately not retained; row identity is represented by row_set_digest.
	 */
	struct detailed_task_report_capture
	{
		std::string provider_task_id;
		std::string provider_execution_id;
		std::string selected_catalog_compile_unit_id;
		std::string compile_unit_id;
		std::string task_input_digest;

		std::uint16_t input_protocol_major{};
		std::uint16_t input_protocol_minor{};
		std::uint64_t logical_input_bytes{};
		std::uint64_t canonical_chunk_bytes{};
		std::uint64_t input_chunk_count{};
		std::vector<std::string> ordered_chunk_digests;
		std::string ordered_chunk_payload_digest_set_digest;

		std::uint64_t raw_frame_stream_bytes{};
		std::string raw_frame_stream_digest;
		std::uint64_t frame_count{};
		std::string frame_transcript_digest;
		std::string sealed_transcript_digest;

		std::vector<detailed_coverage_projection> coverage;
		std::vector<detailed_unresolved_projection> unresolved;
		std::vector<detailed_evidence_projection> evidence;
		std::vector<detailed_provider_batch_projection> batches;
	};

	/**
	 * Source-private bounded owner for task evidence collected across a request.  The owner
	 * accounts for value payloads before adoption so a request cannot turn per-task limits into an
	 * unbounded report-residency allocation.
	 */
	class detailed_task_report_accumulator
	{
	  public:
		explicit detailed_task_report_accumulator(detailed_report_limits limits = {}) noexcept;

		[[nodiscard]] sdk::result<void> append(detailed_task_report_capture capture);
		[[nodiscard]] std::span<const detailed_task_report_capture> tasks() const noexcept;

	  private:
		detailed_report_limits limits_;
		std::size_t accounted_bytes_{};
		std::vector<detailed_task_report_capture> tasks_;
	};

	struct detailed_publication_projection
	{
		std::string publication_id;
		std::string series_id;
		std::string snapshot_id;
		std::uint64_t sequence{};
		std::uint64_t physical_generation{};
		std::optional<std::string> parent_publication;
	};

	struct detailed_store_access_projection
	{
		std::string path;
		std::string status;
		std::optional<std::string> error_code;
		std::optional<std::string> error_field;
	};

	/** Owned post-publication observation. A verified publication is required for success output.
	 */
	struct detailed_store_report_capture
	{
		std::string backend;
		std::string series_id;
		std::string selector_id;
		bool publication_attempted{};
		std::uint32_t publish_call_count{};
		std::optional<detailed_publication_projection> published_record;
		std::optional<detailed_publication_projection> candidate_identity;
		std::vector<detailed_store_access_projection> verification;
		bool prior_history_retained{};
		bool verified{};
	};

	/** Capture one task while its runtime outcome and immutable materialization seal are live. */
	[[nodiscard]] sdk::result<detailed_task_report_capture> capture_detailed_task_report(
		const sdk::provider::detail::provider_process_validation_outcome& outcome,
		const sealed_materialization_result& materialized,
		const detailed_report_limits& limits = {});

	/** Capture only value-owned facts from the post-publication observation. */
	[[nodiscard]] sdk::result<detailed_store_report_capture>
	capture_detailed_store_report(const materialization_store_observation& observation,
								  const detailed_report_limits& limits = {});

	/** Source-private input to the source-private bounded projection encoder. */
	struct detailed_success_report_model
	{
		std::string generated_at;
		std::vector<detailed_task_report_capture> tasks;
		detailed_store_report_capture store;
		detailed_report_limits limits{};
	};

	/**
	 * Encode a canonical JSON source-private bounded projection. This is intentionally not the
	 * full public schema report: it emits only the typed evidence owned by this two-file model. It
	 * returns an error instead of emitting a success-like public result unless all task seals and
	 * the Store observation are valid.
	 */
	[[nodiscard]] sdk::result<std::string>
	encode_detailed_success_report(const detailed_success_report_model& model);
} // namespace cxxlens::detail::clang22::materialization
