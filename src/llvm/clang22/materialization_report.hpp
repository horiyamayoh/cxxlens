#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_execution_journal.hpp"
#include "materialization_io.hpp"
#include "materialization_request_v2_1.hpp"
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
		static constexpr std::size_t maximum_report_bytes = 1024U * 1024U * 1024U;
		std::size_t max_tasks{4096U};
		std::size_t max_batches_per_task{6U};
		std::size_t max_chunks_per_batch{65536U};
		std::size_t max_side_channel_records{65536U};
		std::size_t max_string_bytes{16U * 1024U * 1024U};
		std::size_t max_projection_bytes{maximum_report_bytes};
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
		spool_io,
		spool_corrupt,
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
		/** One bounded row binding precursor; claim refs are intentionally not representable here.
		 */
		struct row_projection
		{
			std::size_t row_index{};
			std::string row_canonical_form;
			std::string row_digest;
		};

		std::string task_id;
		std::string descriptor_id;
		std::string descriptor_digest;
		std::string dependency_group_id;
		std::string atomic_output_group_id;
		std::string batch_id;
		std::string batch_digest;
		std::vector<sdk::provider::batch_column_summary> columns;
		std::vector<std::string> ordered_chunk_digests;
		std::uint64_t row_count{};
		std::string row_set_digest;
		std::vector<row_projection> rows;
	};

	/** Bounded observation metadata rebound to one sealed provider row. */
	struct detailed_observation_row_projection
	{
		std::size_t batch_index{};
		std::size_t row_index{};
		std::string observation_row_digest;
		bool exact_equivalence{};
		std::optional<std::string> limitation;
		/** Exact seven-field source.span bundle, when the sealed observation carried one. */
		std::optional<observation_v2_primary_span> primary_span;
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
		std::string project_id;
		std::string catalog_id;
		std::string catalog_digest;
		std::string selected_catalog_compile_unit_id;
		std::string compile_unit_id;
		std::string variant_id;
		std::string toolchain_context_id;
		std::string toolchain_digest;
		std::string source_snapshot_id;
		std::string source_file_id;
		std::string source_logical_path;
		std::string source_content_digest;
		std::uint64_t source_size_bytes{};
		std::string source_encoding;
		std::string source_line_index_id;
		bool source_read_only{};
		std::string task_input_digest;
		std::string condition_universe_id;
		std::string condition_id;
		std::string interpretation_domain;

		std::uint16_t input_protocol_major{};
		std::uint16_t input_protocol_minor{};
		std::uint64_t logical_input_bytes{};
		std::uint64_t canonical_chunk_bytes{};
		std::uint64_t input_chunk_count{};
		std::vector<std::string> ordered_chunk_digests;
		std::string ordered_chunk_payload_digest_set_digest;

		std::uint64_t raw_frame_stream_bytes{};
		/** Exact provider stdout bytes retained only for source-private reuse proof. */
		std::vector<std::byte> raw_frame_stream;
		std::string raw_frame_stream_digest;
		std::uint64_t frame_count{};
		std::string frame_transcript_digest;
		std::string sealed_transcript_digest;
		/** Canonical cross-binding of raw-frame and retained semantic transcript evidence. */
		std::string capture_binding_digest;

		std::vector<detailed_coverage_projection> coverage;
		std::vector<detailed_unresolved_projection> unresolved;
		std::vector<detailed_evidence_projection> evidence;
		std::vector<detailed_provider_batch_projection> batches;
		std::vector<detailed_observation_row_projection> observation_rows;
		/** Exact dependency-ordered base rows retained from the sealed task authority. */
		std::vector<sdk::detached_row> base_claim_rows;
		/** Exact deduplicated source.span rows retained from the sealed task authority. */
		std::vector<sdk::detached_row> source_span_claim_rows;
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

	/**
	 * Source-private replayable owner for production task captures.
	 *
	 * Captures are encoded into the existing anonymous sealed spool one at a time.  The owner keeps
	 * only bounded record offsets and the spool handle; replay decodes one complete capture for the
	 * callback and releases it before decoding the next one.  This is deliberately a separate
	 * ingress from `detailed_task_report_accumulator`: the latter's span API is retained for the
	 * existing bounded projection tests and cannot be made streaming without changing its callers.
	 */
	class detailed_task_report_replayable_spool
	{
	  public:
		using consumer = std::function<sdk::result<void>(detailed_task_report_capture&&)>;

		static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));

		detailed_task_report_replayable_spool(const detailed_task_report_replayable_spool&) =
			delete;
		detailed_task_report_replayable_spool&
		operator=(const detailed_task_report_replayable_spool&) = delete;
		detailed_task_report_replayable_spool(detailed_task_report_replayable_spool&&) noexcept;
		detailed_task_report_replayable_spool&
		operator=(detailed_task_report_replayable_spool&&) noexcept;
		~detailed_task_report_replayable_spool();

		[[nodiscard]] static sdk::result<detailed_task_report_replayable_spool>
		create(detailed_report_limits limits = {});

		/** Consume one capture; after return the caller's capture may be released. */
		[[nodiscard]] sdk::result<void> append(detailed_task_report_capture capture);

		/** Seal the complete record stream before any replay is allowed. */
		[[nodiscard]] sdk::result<void> seal();

		/** Replay all records, one decoded capture at a time. The spool remains replayable. */
		[[nodiscard]] sdk::result<void> replay(const consumer& consume) const;

		/** Replay one indexed record without decoding the other captures. */
		[[nodiscard]] sdk::result<void> replay_one(std::size_t task_index,
												   const consumer& consume) const;

		[[nodiscard]] std::size_t task_count() const noexcept;
		[[nodiscard]] std::uint64_t spooled_bytes() const noexcept;
		[[nodiscard]] bool sealed() const noexcept;

	  private:
		detailed_task_report_replayable_spool(
			detailed_report_limits limits,
			std::unique_ptr<materialization_replayable_spool> storage);

		detailed_report_limits limits_;
		std::unique_ptr<materialization_replayable_spool> storage_;
		std::vector<std::uint64_t> record_offsets_;
		std::uint64_t spooled_bytes_{};
		bool sealed_{};
		bool poisoned_{};
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

	/** Capture with the authenticated v2.1 semantic task context used by claim origins. */
	[[nodiscard]] sdk::result<detailed_task_report_capture> capture_detailed_task_report(
		const sdk::provider::detail::provider_process_validation_outcome& outcome,
		const sealed_materialization_result& materialized,
		const materialization_v2_1_task_metadata_receipt& metadata,
		const detailed_report_limits& limits = {});

	/** Encode one bounded task capture for a source-private durable artifact. */
	[[nodiscard]] sdk::result<std::vector<std::byte>>
	encode_detailed_task_report_capture(const detailed_task_report_capture& capture,
										const detailed_report_limits& limits = {});

	/** Strictly decode one durable task capture, rejecting trailing or non-canonical bytes. */
	[[nodiscard]] sdk::result<detailed_task_report_capture>
	decode_detailed_task_report_capture(std::span<const std::byte> bytes,
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
	 * Checked compositional upper bound for the source-private detailed report.
	 *
	 * The components deliberately remain separate: a response ceiling is only the final admission
	 * check, not the proof.  The bound includes the publication-independent task projection, fixed
	 * response framing, the exact publication outcome shape, the SDK verification receipts, and a
	 * bounded diagnostic envelope which may be needed for an outcome-specific response.
	 */
	struct detailed_report_capacity_bound
	{
		std::size_t publication_independent_projection{};
		std::size_t final_json_framing{};
		std::size_t exact_publication_outcome{};
		std::size_t exact_sdk_records_and_receipts{};
		std::size_t maximum_bounded_diagnostics{};
		std::size_t total{};
	};

	/** Calculate the checked bound without admitting it against `max_projection_bytes`. */
	[[nodiscard]] sdk::result<detailed_report_capacity_bound>
	checked_detailed_report_capacity_upper_bound(const detailed_success_report_model& model);

	/**
	 * Encode a canonical JSON source-private bounded projection. This is intentionally not the
	 * full public schema report: it emits only the typed evidence owned by this two-file model. It
	 * returns an error instead of emitting a success-like public result unless all task seals and
	 * the Store observation are valid.
	 */
	[[nodiscard]] sdk::result<std::string>
	encode_detailed_success_report(const detailed_success_report_model& model);
} // namespace cxxlens::detail::clang22::materialization
