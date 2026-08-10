#include "materialization_report.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <string_view>
#include <utility>

#include "materialization_json.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error invalid(std::string field, std::string detail = {})
		{
			return {"materialization.report-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error
		detailed_error(const detailed_report_error_kind kind, std::string field, std::string detail)
		{
			std::string code;
			switch (kind)
			{
				case detailed_report_error_kind::invalid_capture:
					code = "invalid-capture";
					break;
				case detailed_report_error_kind::limit_exceeded:
					code = "limit-exceeded";
					break;
				case detailed_report_error_kind::missing_input_seal:
					code = "missing-input-seal";
					break;
				case detailed_report_error_kind::missing_runtime_receipt:
					code = "missing-runtime-receipt";
					break;
				case detailed_report_error_kind::transcript_mismatch:
					code = "transcript-mismatch";
					break;
				case detailed_report_error_kind::publication_unverified:
					code = "publication-unverified";
					break;
				case detailed_report_error_kind::invalid_time:
					code = "invalid-time";
					break;
				default:
					// The enum is source-private, but a corrupted value must never produce an
					// empty diagnostic that looks like a valid report failure.
					code = "unknown-error-kind";
					break;
			}
			return {"materialization.report-invalid",
					std::move(field),
					code + (detail.empty() ? std::string{} : ":" + detail)};
		}

		[[nodiscard]] sdk::error
		fail(const detailed_report_error_kind kind, std::string field, std::string detail)
		{
			return detailed_error(kind, std::move(field), std::move(detail));
		}

		[[nodiscard]] bool bounded_text(const std::string_view value,
										const detailed_report_limits& limits) noexcept
		{
			return !value.empty() && value.size() <= limits.max_string_bytes;
		}

		[[nodiscard]] bool bounded_text_or_empty(const std::string_view value,
												 const detailed_report_limits& limits) noexcept
		{
			return value.size() <= limits.max_string_bytes;
		}

		[[nodiscard]] bool add_bounded_text(std::size_t& total,
											const std::string_view value,
											const detailed_report_limits& limits,
											const bool require_value) noexcept
		{
			if ((require_value && value.empty()) || value.size() > limits.max_string_bytes ||
				total > limits.max_projection_bytes ||
				value.size() > limits.max_projection_bytes - total)
				return false;
			total += value.size();
			return true;
		}

		[[nodiscard]] std::string_view
		text(const materialization_store_receipt_status value) noexcept
		{
			switch (value)
			{
				case materialization_store_receipt_status::not_attempted:
					return "not_attempted";
				case materialization_store_receipt_status::present:
					return "present";
				case materialization_store_receipt_status::sdk_error:
					return "sdk_error";
			}
			return {};
		}

		[[nodiscard]] std::string_view text(const materialization_store_path value) noexcept
		{
			switch (value)
			{
				case materialization_store_path::current_selector:
					return "current-selector";
				case materialization_store_path::open_publication:
					return "open-publication";
				case materialization_store_path::open_snapshot:
					return "open-snapshot";
			}
			return {};
		}

		[[nodiscard]] sdk::result<std::string>
		row_set_digest(const std::span<const sdk::detached_row> rows,
					   const detailed_report_limits& limits,
					   std::size_t& projection_bytes)
		{
			std::string projection;
			for (std::size_t index{}; index < rows.size(); ++index)
			{
				auto row = rows[index].canonical_form();
				const auto index_text = std::to_string(index);
				constexpr std::size_t framing_bytes = 2U; // ':' and '\n'
				if (projection_bytes > limits.max_projection_bytes ||
					index_text.size() > limits.max_projection_bytes - projection_bytes)
					return sdk::unexpected(fail(detailed_report_error_kind::limit_exceeded,
												"provider_sealed_transcript.rows",
												"projection-bytes"));
				const auto remaining_after_index =
					limits.max_projection_bytes - projection_bytes - index_text.size();
				if (framing_bytes > remaining_after_index ||
					row.size() > remaining_after_index - framing_bytes)
					return sdk::unexpected(fail(detailed_report_error_kind::limit_exceeded,
												"provider_sealed_transcript.rows",
												"projection-bytes"));
				projection_bytes += index_text.size() + row.size() + framing_bytes;
				projection.append(index_text);
				projection.push_back(':');
				projection.append(row);
				projection.push_back('\n');
			}
			return sdk::semantic_digest("cxxlens.clang22.materialization-report.row-set.v1",
										projection);
		}

		[[nodiscard]] bool
		same_sealed_batch(const sdk::provider::detail::sealed_provider_batch& left,
						  const sdk::provider::detail::sealed_provider_batch& right) noexcept
		{
			if (left.task_id() != right.task_id() ||
				left.descriptor_id() != right.descriptor_id() ||
				left.descriptor_digest() != right.descriptor_digest() ||
				left.dependency_group_id() != right.dependency_group_id() ||
				left.atomic_output_group_id() != right.atomic_output_group_id() ||
				left.batch_id() != right.batch_id() ||
				left.batch_digest() != right.batch_digest() ||
				left.ordered_chunk_digests().size() != right.ordered_chunk_digests().size() ||
				left.rows().size() != right.rows().size())
				return false;
			for (std::size_t index{}; index < left.ordered_chunk_digests().size(); ++index)
				if (left.ordered_chunk_digests()[index] != right.ordered_chunk_digests()[index])
					return false;
			for (std::size_t index{}; index < left.rows().size(); ++index)
				if (left.rows()[index].canonical_form() != right.rows()[index].canonical_form())
					return false;
			return true;
		}

		[[nodiscard]] bool same_sealed_transcript(
			const sdk::provider::detail::sealed_provider_transcript& left,
			const sdk::provider::detail::sealed_provider_transcript& right) noexcept
		{
			if (left.batches().size() != right.batches().size() ||
				left.coverage().size() != right.coverage().size() ||
				left.unresolved().size() != right.unresolved().size() ||
				left.evidence().size() != right.evidence().size())
				return false;
			for (std::size_t index{}; index < left.batches().size(); ++index)
				if (!same_sealed_batch(left.batches()[index], right.batches()[index]))
					return false;
			for (std::size_t index{}; index < left.coverage().size(); ++index)
			{
				const auto& a = left.coverage()[index];
				const auto& b = right.coverage()[index];
				if (a.kind != b.kind || a.id != b.id || a.state != b.state || a.reason != b.reason)
					return false;
			}
			for (std::size_t index{}; index < left.unresolved().size(); ++index)
				if (left.unresolved()[index] != right.unresolved()[index])
					return false;
			for (std::size_t index{}; index < left.evidence().size(); ++index)
				if (left.evidence()[index] != right.evidence()[index])
					return false;
			return true;
		}

		[[nodiscard]] bool
		same_publication_identity(const detailed_publication_projection& left,
								  const detailed_publication_projection& right) noexcept
		{
			return left.publication_id == right.publication_id &&
				left.series_id == right.series_id && left.snapshot_id == right.snapshot_id &&
				left.sequence == right.sequence &&
				left.parent_publication == right.parent_publication;
		}

		[[nodiscard]] bool same_publication_identity(const detailed_publication_projection& left,
													 const sdk::publication_record& right) noexcept
		{
			return left.publication_id == right.publication_id &&
				left.series_id == right.series_id && left.snapshot_id == right.snapshot_id &&
				left.sequence == right.sequence &&
				left.parent_publication == right.parent_publication;
		}

		[[nodiscard]] bool
		same_publication_observation(const detailed_publication_projection& left,
									 const sdk::publication_record& right) noexcept
		{
			return same_publication_identity(left, right) &&
				left.physical_generation == right.physical_generation;
		}

		[[nodiscard]] bool exact_success_verification(
			const std::span<const detailed_store_access_projection> verification) noexcept
		{
			constexpr std::array<std::string_view, 3U> expected_paths{
				"current-selector", "open-publication", "open-snapshot"};
			if (verification.size() != expected_paths.size())
				return false;
			for (std::size_t index{}; index < expected_paths.size(); ++index)
				if (verification[index].path != expected_paths[index] ||
					verification[index].status != "present" || verification[index].error_code ||
					verification[index].error_field)
					return false;
			return true;
		}

		[[nodiscard]] bool
		valid_publication_projection(const detailed_publication_projection& value,
									 const detailed_report_limits& limits) noexcept
		{
			return bounded_text(value.publication_id, limits) &&
				bounded_text(value.series_id, limits) && bounded_text(value.snapshot_id, limits) &&
				(!value.parent_publication || bounded_text(*value.parent_publication, limits));
		}

		template <class T>
		[[nodiscard]] sdk::result<json_value> json_string(T value)
		{
			auto result = json_value::string(std::string{std::move(value)});
			if (!result)
				return sdk::unexpected(invalid("report", "invalid-string"));
			return result;
		}

		/** Only used for ASCII protocol constants whose UTF-8 validity is closed by construction.
		 */
		[[nodiscard]] json_value json_constant(const std::string_view value)
		{
			auto result = json_value::string(std::string{value});
			return result ? std::move(*result) : json_value::null();
		}

		[[nodiscard]] sdk::result<json_value>
		json_object(std::vector<std::pair<std::string, json_value>> fields)
		{
			json_value::object_type value;
			for (auto& [name, member] : fields)
				if (!value.emplace(std::move(name), std::move(member)).second)
					return sdk::unexpected(invalid("report", "duplicate-member"));
			auto result = json_value::object(std::move(value));
			if (!result)
				return sdk::unexpected(invalid("report", "invalid-object"));
			return result;
		}

		[[nodiscard]] sdk::result<json_value>
		json_strings(const std::span<const std::string> values)
		{
			json_value::array_type output;
			output.reserve(values.size());
			for (const auto& value : values)
			{
				auto member = json_string(value);
				if (!member)
					return sdk::unexpected(std::move(member.error()));
				output.push_back(std::move(*member));
			}
			return json_value::array(std::move(output));
		}

		[[nodiscard]] sdk::result<json_value>
		json_optional_string(const std::optional<std::string>& value)
		{
			if (!value)
				return json_value::null();
			return json_string(*value);
		}

		[[nodiscard]] sdk::result<json_value>
		publication_json(const detailed_publication_projection& value)
		{
			auto publication_id = json_string(value.publication_id);
			auto series_id = json_string(value.series_id);
			auto snapshot_id = json_string(value.snapshot_id);
			auto parent = json_optional_string(value.parent_publication);
			if (!publication_id || !series_id || !snapshot_id || !parent)
				return sdk::unexpected(invalid("publication", "string"));
			return json_object({
				{"publication_id", std::move(*publication_id)},
				{"series_id", std::move(*series_id)},
				{"snapshot_id", std::move(*snapshot_id)},
				{"sequence", json_value::unsigned_integer(value.sequence)},
				{"physical_generation", json_value::unsigned_integer(value.physical_generation)},
				{"parent_publication", std::move(*parent)},
			});
		}

		[[nodiscard]] bool generated_at(const std::string_view value) noexcept
		{
			// The production clock emits this closed UTC spelling. Restricting the accepted form is
			// stronger than the schema's date-time format and keeps report bytes deterministic.
			if (value.size() != 20U || value[4U] != '-' || value[7U] != '-' || value[10U] != 'T' ||
				value[13U] != ':' || value[16U] != ':' || value[19U] != 'Z')
				return false;
			for (const auto index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U, 18U})
				if (value[index] < '0' || value[index] > '9')
					return false;
			const auto number = [&](const std::size_t offset, const std::size_t count)
			{
				unsigned result{};
				for (std::size_t index{}; index < count; ++index)
					result = result * 10U + static_cast<unsigned>(value[offset + index] - '0');
				return result;
			};
			const std::chrono::year_month_day date{
				std::chrono::year{static_cast<int>(number(0U, 4U))},
				std::chrono::month{number(5U, 2U)},
				std::chrono::day{number(8U, 2U)}};
			return date.ok() && number(11U, 2U) <= 23U && number(14U, 2U) <= 59U &&
				number(17U, 2U) <= 59U;
		}

		[[nodiscard]] sdk::result<json_value> string(std::string value)
		{
			auto result = json_value::string(std::move(value));
			if (!result)
				return sdk::unexpected(invalid("report", "invalid-string"));
			return result;
		}

		[[nodiscard]] sdk::result<json_value>
		object(std::initializer_list<std::pair<std::string, json_value>> fields)
		{
			json_value::object_type value;
			for (auto& [name, member] : fields)
				if (!value.emplace(name, std::move(member)).second)
					return sdk::unexpected(invalid("report", "duplicate-member"));
			auto result = json_value::object(std::move(value));
			if (!result)
				return sdk::unexpected(invalid("report", "invalid-object"));
			return result;
		}

		[[nodiscard]] std::string_view text(const compact_store_draft_state value) noexcept
		{
			return value == compact_store_draft_state::not_created ? "not-created" : "discarded";
		}

		[[nodiscard]] std::string_view text(const compact_head_observation value) noexcept
		{
			switch (value)
			{
				case compact_head_observation::not_observed:
					return "not-observed";
				case compact_head_observation::absent:
					return "absent";
				case compact_head_observation::present:
					return "present";
				case compact_head_observation::sdk_error:
					return "sdk-error";
			}
			return {};
		}

		[[nodiscard]] std::string_view text(const materialization_store_operation value) noexcept
		{
			switch (value)
			{
				case materialization_store_operation::configuration:
					return "configuration";
				case materialization_store_operation::store_open:
					return "store_open";
				case materialization_store_operation::head_current:
					return "head_current";
				case materialization_store_operation::writer_begin:
					return "writer_begin";
				case materialization_store_operation::partition_stage:
					return "partition_stage";
				case materialization_store_operation::closure_stage:
					return "closure_stage";
				case materialization_store_operation::writer_validate:
					return "writer_validate";
				case materialization_store_operation::writer_publish:
					return "writer_publish";
				case materialization_store_operation::store_reopen:
					return "store_reopen";
				case materialization_store_operation::verify_current:
					return "verify_current";
				case materialization_store_operation::verify_open_publication:
					return "verify_open_publication";
				case materialization_store_operation::verify_open_snapshot:
					return "verify_open_snapshot";
				case materialization_store_operation::verify_projection:
					return "verify_projection";
			}
			return {};
		}

		[[nodiscard]] std::string_view text(const compact_failure_phase value) noexcept
		{
			switch (value)
			{
				case compact_failure_phase::input_limit:
					return "input-limit";
				case compact_failure_phase::json_decode:
					return "json-decode";
				case compact_failure_phase::request_envelope:
					return "request-envelope";
				case compact_failure_phase::request_version:
					return "request-version";
				case compact_failure_phase::request_schema:
					return "request-schema";
				case compact_failure_phase::request_binding:
					return "request-binding";
				case compact_failure_phase::installation_binding:
					return "installation-binding";
				case compact_failure_phase::worker_launch:
					return "worker-launch";
				case compact_failure_phase::transcript:
					return "transcript";
				case compact_failure_phase::materialization_validation:
					return "materialization-validation";
				case compact_failure_phase::store_open:
					return "store-open";
				case compact_failure_phase::store_stage:
					return "store-stage";
				case compact_failure_phase::report_construction:
					return "report-construction";
			}
			return {};
		}

	} // namespace

	sdk::error detailed_report_error::as_sdk_error() const
	{
		return detailed_error(kind, field, detail);
	}

	sdk::result<detailed_task_report_capture> capture_detailed_task_report(
		const sdk::provider::detail::provider_process_validation_outcome& outcome,
		const sealed_materialization_result& materialized,
		const detailed_report_limits& limits)
	{
		if (limits.max_tasks == 0U || limits.max_batches_per_task == 0U ||
			limits.max_chunks_per_batch == 0U || limits.max_side_channel_records == 0U ||
			limits.max_string_bytes == 0U || limits.max_projection_bytes == 0U)
			return sdk::unexpected(
				fail(detailed_report_error_kind::invalid_capture, "limits", "zero"));
		if (!outcome.input_seal)
			return sdk::unexpected(
				fail(detailed_report_error_kind::missing_input_seal, "input_transfer", "absent"));
		if (!outcome.runtime_receipt)
			return sdk::unexpected(fail(
				detailed_report_error_kind::missing_runtime_receipt, "runtime_receipt", "absent"));
		// The provider seal is consumed by validate_and_seal_materialization before this
		// projection is captured.  In that normal production path the immutable seal held by
		// `materialized` is the authority.  When a caller still retains outcome.sealed, verify
		// the two authorities below as an additional binding check.
		if (!outcome.validated_transcript_success || outcome.terminal != "provider.success")
			return sdk::unexpected(
				fail(detailed_report_error_kind::transcript_mismatch, "terminal", "not-success"));

		const auto& input = *outcome.input_seal;
		const auto& runtime = *outcome.runtime_receipt;
		if (outcome.task_input_digest != materialized.task_input_digest() ||
			input.task().task_input_digest != materialized.task_input_digest() ||
			input.task().task_id != materialized.provider_task_id() ||
			(outcome.sealed &&
			 !same_sealed_transcript(*outcome.sealed, materialized.provider_seal())))
			return sdk::unexpected(
				fail(detailed_report_error_kind::transcript_mismatch, "task", "authority-binding"));
		auto expected_sealed_digest =
			sdk::provider::detail::provider_sealed_transcript_receipt_digest(
				materialized.provider_task_id(), "provider.success", materialized.provider_seal());
		if (!expected_sealed_digest ||
			*expected_sealed_digest != runtime.sealed_transcript_digest())
			return sdk::unexpected(fail(detailed_report_error_kind::transcript_mismatch,
										"runtime_receipt",
										"sealed-digest"));
		if (runtime.raw_stdout_byte_count() == 0U || runtime.decoded_frame_count() == 0U ||
			!bounded_text(runtime.raw_stdout_sha256(), limits) ||
			!bounded_text(runtime.frame_transcript_digest(), limits) ||
			!bounded_text(runtime.sealed_transcript_digest(), limits))
			return sdk::unexpected(fail(detailed_report_error_kind::missing_runtime_receipt,
										"runtime_receipt",
										"incomplete"));
		if (input.chunk_count() > limits.max_chunks_per_batch ||
			input.ordered_chunk_digests().size() != input.chunk_count() ||
			input.total_bytes() > limits.max_projection_bytes)
			return sdk::unexpected(fail(
				detailed_report_error_kind::limit_exceeded, "input_transfer", "bounded-input"));
		for (const auto& digest : input.ordered_chunk_digests())
			if (!bounded_text(digest, limits))
				return sdk::unexpected(fail(detailed_report_error_kind::invalid_capture,
											"input_transfer.ordered_chunk_digests",
											"string"));

		detailed_task_report_capture capture;
		capture.provider_task_id = std::string{materialized.provider_task_id()};
		capture.provider_execution_id = std::string{materialized.provider_execution_id()};
		capture.task_input_digest = std::string{materialized.task_input_digest()};
		capture.input_protocol_major = input.protocol_major();
		capture.input_protocol_minor = input.protocol_minor();
		capture.logical_input_bytes = input.total_bytes();
		capture.canonical_chunk_bytes = input.chunk_bytes();
		capture.input_chunk_count = input.chunk_count();
		capture.ordered_chunk_digests.assign(input.ordered_chunk_digests().begin(),
											 input.ordered_chunk_digests().end());
		capture.ordered_chunk_payload_digest_set_digest =
			std::string{input.ordered_chunk_digest_set_digest()};
		capture.raw_frame_stream_bytes = runtime.raw_stdout_byte_count();
		capture.raw_frame_stream_digest = std::string{runtime.raw_stdout_sha256()};
		capture.frame_count = runtime.decoded_frame_count();
		capture.frame_transcript_digest = std::string{runtime.frame_transcript_digest()};
		capture.sealed_transcript_digest = std::string{runtime.sealed_transcript_digest()};
		capture.selected_catalog_compile_unit_id =
			std::string{materialized.selected_catalog_compile_unit_id()};
		capture.compile_unit_id = std::string{materialized.final_relation_compile_unit_id()};
		if (!bounded_text(capture.provider_task_id, limits) ||
			!bounded_text(capture.provider_execution_id, limits) ||
			!bounded_text(capture.task_input_digest, limits) ||
			!bounded_text(capture.selected_catalog_compile_unit_id, limits) ||
			!bounded_text(capture.compile_unit_id, limits) ||
			!bounded_text(capture.ordered_chunk_payload_digest_set_digest, limits))
			return sdk::unexpected(
				fail(detailed_report_error_kind::invalid_capture, "task", "empty-identity"));

		std::size_t projection_bytes{};
		for (const auto& batch : materialized.provider_seal().batches())
		{
			if (capture.batches.size() >= limits.max_batches_per_task ||
				batch.ordered_chunk_digests().size() > limits.max_chunks_per_batch)
			{
				return sdk::unexpected(fail(detailed_report_error_kind::limit_exceeded,
											"provider_sealed_transcript.batches",
											"count"));
			}
			if (batch.task_id() != capture.provider_task_id)
				return sdk::unexpected(fail(detailed_report_error_kind::transcript_mismatch,
											"provider_sealed_transcript.batches",
											"task-id"));
			if (!bounded_text(batch.descriptor_id(), limits) ||
				!bounded_text(batch.descriptor_digest(), limits) ||
				!bounded_text(batch.dependency_group_id(), limits) ||
				!bounded_text(batch.atomic_output_group_id(), limits) ||
				!bounded_text(batch.batch_id(), limits) ||
				!bounded_text(batch.batch_digest(), limits))
				return sdk::unexpected(fail(detailed_report_error_kind::invalid_capture,
											"provider_sealed_transcript.batches",
											"identity-string"));
			for (const auto& digest : batch.ordered_chunk_digests())
				if (!bounded_text(digest, limits))
					return sdk::unexpected(fail(detailed_report_error_kind::invalid_capture,
												"provider_sealed_transcript.batches",
												"chunk-digest-string"));
			detailed_provider_batch_projection output;
			output.task_id = std::string{batch.task_id()};
			output.descriptor_id = std::string{batch.descriptor_id()};
			output.descriptor_digest = std::string{batch.descriptor_digest()};
			output.dependency_group_id = std::string{batch.dependency_group_id()};
			output.atomic_output_group_id = std::string{batch.atomic_output_group_id()};
			output.batch_id = std::string{batch.batch_id()};
			output.batch_digest = std::string{batch.batch_digest()};
			output.ordered_chunk_digests.assign(batch.ordered_chunk_digests().begin(),
												batch.ordered_chunk_digests().end());
			output.row_count = batch.rows().size();
			auto rows = row_set_digest(batch.rows(), limits, projection_bytes);
			if (!rows)
				return sdk::unexpected(std::move(rows.error()));
			output.row_set_digest = std::move(*rows);
			capture.batches.push_back(std::move(output));
		}
		for (const auto& value : materialized.provider_seal().coverage())
		{
			if (capture.coverage.size() >= limits.max_side_channel_records)
				return sdk::unexpected(
					fail(detailed_report_error_kind::limit_exceeded, "coverage", "count"));
			if (!bounded_text(value.kind, limits) || !bounded_text(value.id, limits) ||
				!bounded_text(value.state, limits) || !bounded_text_or_empty(value.reason, limits))
				return sdk::unexpected(
					fail(detailed_report_error_kind::invalid_capture, "coverage", "string"));
			capture.coverage.push_back({value.kind, value.id, value.state, value.reason});
		}
		for (const auto& value : materialized.provider_seal().unresolved())
		{
			if (capture.unresolved.size() >= limits.max_side_channel_records)
				return sdk::unexpected(
					fail(detailed_report_error_kind::limit_exceeded, "unresolved", "count"));
			if (!bounded_text(value.code, limits) || !bounded_text(value.subject, limits) ||
				!bounded_text_or_empty(value.detail, limits))
				return sdk::unexpected(
					fail(detailed_report_error_kind::invalid_capture, "unresolved", "string"));
			capture.unresolved.push_back({value.code, value.subject, value.detail});
		}
		for (const auto& value : materialized.provider_seal().evidence())
		{
			if (capture.evidence.size() >= limits.max_side_channel_records)
				return sdk::unexpected(
					fail(detailed_report_error_kind::limit_exceeded, "evidence", "count"));
			if (!bounded_text(value.kind, limits) || !bounded_text(value.subject, limits) ||
				!bounded_text(value.producer, limits) ||
				!bounded_text_or_empty(value.summary, limits))
				return sdk::unexpected(
					fail(detailed_report_error_kind::invalid_capture, "evidence", "string"));
			capture.evidence.push_back({value.kind, value.subject, value.producer, value.summary});
		}
		return capture;
	}

	namespace
	{
		[[nodiscard]] bool add_accounted_bytes(std::size_t& total,
											   const std::size_t value,
											   const std::size_t limit) noexcept
		{
			if (value > limit || total > limit - value)
				return false;
			total += value;
			return true;
		}

		[[nodiscard]] bool add_accounted_count(std::size_t& total,
											   const std::size_t count,
											   const std::size_t element_bytes,
											   const std::size_t limit) noexcept
		{
			if (element_bytes != 0U &&
				(count > limit / element_bytes || count * element_bytes > limit - total))
				return false;
			return add_accounted_bytes(total, count * element_bytes, limit);
		}

		template <class StringLike>
		[[nodiscard]] bool account_string(std::size_t& total,
										  const StringLike& value,
										  const std::size_t limit) noexcept
		{
			return add_accounted_bytes(total, sizeof(std::string), limit) &&
				add_accounted_bytes(total, value.size(), limit);
		}

		[[nodiscard]] bool account_task_capture(const detailed_task_report_capture& capture,
												const std::size_t limit,
												std::size_t& accounted) noexcept
		{
			accounted = sizeof(capture);
			const auto account = [&](const std::string_view value)
			{
				return account_string(accounted, value, limit);
			};
			if (!account(capture.provider_task_id) || !account(capture.provider_execution_id) ||
				!account(capture.selected_catalog_compile_unit_id) ||
				!account(capture.compile_unit_id) || !account(capture.task_input_digest) ||
				!account(capture.ordered_chunk_payload_digest_set_digest) ||
				!account(capture.raw_frame_stream_digest) ||
				!account(capture.frame_transcript_digest) ||
				!account(capture.sealed_transcript_digest) ||
				!add_accounted_count(
					accounted, capture.ordered_chunk_digests.size(), sizeof(std::string), limit))
				return false;
			for (const auto& value : capture.ordered_chunk_digests)
				if (!account(value))
					return false;
			if (!add_accounted_count(accounted,
									 capture.batches.size(),
									 sizeof(detailed_provider_batch_projection),
									 limit))
				return false;
			for (const auto& batch : capture.batches)
			{
				if (!account(batch.task_id) || !account(batch.descriptor_id) ||
					!account(batch.descriptor_digest) || !account(batch.dependency_group_id) ||
					!account(batch.atomic_output_group_id) || !account(batch.batch_id) ||
					!account(batch.batch_digest) || !account(batch.row_set_digest) ||
					!add_accounted_count(
						accounted, batch.ordered_chunk_digests.size(), sizeof(std::string), limit))
					return false;
				for (const auto& value : batch.ordered_chunk_digests)
					if (!account(value))
						return false;
			}
			if (!add_accounted_count(accounted,
									 capture.coverage.size(),
									 sizeof(detailed_coverage_projection),
									 limit) ||
				!add_accounted_count(accounted,
									 capture.unresolved.size(),
									 sizeof(detailed_unresolved_projection),
									 limit) ||
				!add_accounted_count(accounted,
									 capture.evidence.size(),
									 sizeof(detailed_evidence_projection),
									 limit))
				return false;
			for (const auto& value : capture.coverage)
				if (!account(value.kind) || !account(value.id) || !account(value.state) ||
					!account(value.reason))
					return false;
			for (const auto& value : capture.unresolved)
				if (!account(value.code) || !account(value.subject) || !account(value.detail))
					return false;
			for (const auto& value : capture.evidence)
				if (!account(value.kind) || !account(value.subject) || !account(value.producer) ||
					!account(value.summary))
					return false;
			return true;
		}
	} // namespace

	detailed_task_report_accumulator::detailed_task_report_accumulator(
		detailed_report_limits limits) noexcept
		: limits_{limits}
	{
		tasks_.reserve(std::min(limits_.max_tasks, std::size_t{4096U}));
	}

	sdk::result<void> detailed_task_report_accumulator::append(detailed_task_report_capture capture)
	{
		if (limits_.max_tasks == 0U || limits_.max_projection_bytes == 0U ||
			tasks_.size() >= limits_.max_tasks)
			return sdk::unexpected(
				fail(detailed_report_error_kind::limit_exceeded, "task_results", "count"));
		std::size_t capture_bytes{};
		if (!account_task_capture(capture, limits_.max_projection_bytes, capture_bytes) ||
			!add_accounted_bytes(accounted_bytes_, capture_bytes, limits_.max_projection_bytes))
			return sdk::unexpected(fail(
				detailed_report_error_kind::limit_exceeded, "task_results", "projection-bytes"));
		tasks_.push_back(std::move(capture));
		return {};
	}

	std::span<const detailed_task_report_capture>
	detailed_task_report_accumulator::tasks() const noexcept
	{
		return tasks_;
	}

	sdk::result<detailed_store_report_capture>
	capture_detailed_store_report(const materialization_store_observation& observation,
								  const detailed_report_limits& limits)
	{
		if (limits.max_tasks == 0U || limits.max_batches_per_task == 0U ||
			limits.max_chunks_per_batch == 0U || limits.max_side_channel_records == 0U ||
			limits.max_string_bytes == 0U || limits.max_projection_bytes == 0U)
			return sdk::unexpected(
				fail(detailed_report_error_kind::invalid_capture, "limits", "zero"));
		if (observation.backend != "memory" && observation.backend != "sqlite")
			return sdk::unexpected(fail(
				detailed_report_error_kind::invalid_capture, "publication.backend", "unsupported"));
		detailed_store_report_capture capture;
		capture.backend = observation.backend;
		capture.series_id = observation.series_id;
		capture.selector_id = observation.selector.id();
		capture.publication_attempted = observation.publication_attempted;
		capture.publish_call_count = observation.publish_call_count;
		if (!bounded_text(capture.series_id, limits) || !bounded_text(capture.selector_id, limits))
			return sdk::unexpected(
				fail(detailed_report_error_kind::invalid_capture, "publication.selector", "empty"));
		const auto copy_record = [](const sdk::publication_record& value)
		{
			return detailed_publication_projection{value.publication_id,
												   value.series_id,
												   value.snapshot_id,
												   value.sequence,
												   value.physical_generation,
												   value.parent_publication};
		};
		if (observation.publish_returned_record)
		{
			if (!bounded_text(observation.publish_returned_record->publication_id, limits) ||
				!bounded_text(observation.publish_returned_record->series_id, limits) ||
				!bounded_text(observation.publish_returned_record->snapshot_id, limits) ||
				(observation.publish_returned_record->parent_publication &&
				 !bounded_text(*observation.publish_returned_record->parent_publication, limits)))
				return sdk::unexpected(fail(detailed_report_error_kind::invalid_capture,
											"publication.published_record",
											"string"));
			capture.published_record = copy_record(*observation.publish_returned_record);
		}
		if (observation.candidate_identity)
		{
			capture.candidate_identity =
				detailed_publication_projection{observation.candidate_identity->publication_id,
												observation.candidate_identity->series_id,
												observation.candidate_identity->snapshot_id,
												observation.candidate_identity->sequence,
												0U,
												observation.candidate_identity->parent_publication};
			if (!valid_publication_projection(*capture.candidate_identity, limits))
				return sdk::unexpected(fail(detailed_report_error_kind::invalid_capture,
											"publication.candidate_identity",
											"string"));
		}
		for (const auto& receipt : observation.verification_receipts)
		{
			detailed_store_access_projection value;
			value.path = std::string{text(receipt.path)};
			value.status = std::string{text(receipt.status)};
			if (!bounded_text(value.path, limits) || !bounded_text(value.status, limits))
				return sdk::unexpected(fail(detailed_report_error_kind::invalid_capture,
											"publication.verification",
											"closed-enum"));
			if (receipt.error)
			{
				if (!bounded_text(receipt.error->code, limits) ||
					!bounded_text_or_empty(receipt.error->field, limits))
					return sdk::unexpected(fail(detailed_report_error_kind::invalid_capture,
												"publication.verification.error",
												"string"));
				value.error_code = receipt.error->code;
				value.error_field = receipt.error->field;
			}
			capture.verification.push_back(std::move(value));
		}
		capture.prior_history_retained = !observation.expected_parent_publication ||
			(observation.head_observation.status == materialization_store_receipt_status::present &&
			 observation.head_observation.projection &&
			 observation.head_observation.projection->publication.publication_id ==
				 *observation.expected_parent_publication);
		bool verification_identity_matches = observation.verification_store.has_value();
		if (capture.published_record)
		{
			for (const auto& receipt : observation.verification_receipts)
			{
				if (receipt.status != materialization_store_receipt_status::present ||
					receipt.error || !receipt.projection ||
					receipt.projection->physical_backend != observation.backend ||
					!same_publication_observation(*capture.published_record,
												  receipt.projection->publication))
					verification_identity_matches = false;
			}
		}
		capture.verified = capture.publication_attempted && capture.publish_call_count == 1U &&
			capture.published_record && capture.candidate_identity &&
			observation.publish_returned_record->state == sdk::publication_state::committed &&
			!observation.publish_returned_record->corrupt && !observation.first_issue &&
			same_publication_identity(*capture.candidate_identity, *capture.published_record) &&
			capture.candidate_identity->parent_publication ==
				observation.expected_parent_publication &&
			exact_success_verification(capture.verification) && verification_identity_matches &&
			capture.prior_history_retained;
		return capture;
	}

	sdk::result<std::string>
	encode_detailed_success_report(const detailed_success_report_model& model)
	{
		if (model.limits.max_tasks == 0U || model.limits.max_batches_per_task == 0U ||
			model.limits.max_chunks_per_batch == 0U ||
			model.limits.max_side_channel_records == 0U || model.limits.max_string_bytes == 0U ||
			model.limits.max_projection_bytes == 0U)
			return sdk::unexpected(
				fail(detailed_report_error_kind::invalid_capture, "limits", "zero"));
		if (!generated_at(model.generated_at))
			return sdk::unexpected(
				fail(detailed_report_error_kind::invalid_time, "generated_at", "utc-second"));
		if (model.tasks.empty() || model.tasks.size() > model.limits.max_tasks)
			return sdk::unexpected(
				fail(detailed_report_error_kind::limit_exceeded, "task_results", "count"));
		if (!model.store.verified || !model.store.publication_attempted ||
			model.store.publish_call_count != 1U || !model.store.published_record ||
			!model.store.candidate_identity || !model.store.prior_history_retained ||
			!valid_publication_projection(*model.store.published_record, model.limits) ||
			!valid_publication_projection(*model.store.candidate_identity, model.limits) ||
			!same_publication_identity(*model.store.published_record,
									   *model.store.candidate_identity) ||
			!exact_success_verification(model.store.verification) ||
			!bounded_text(model.store.backend, model.limits) ||
			!bounded_text(model.store.series_id, model.limits) ||
			!bounded_text(model.store.selector_id, model.limits))
			return sdk::unexpected(fail(detailed_report_error_kind::publication_unverified,
										"publication",
										"committed-verified-required"));

		std::size_t projection_bytes{};
		auto accept_text = [&](const std::string_view value, const bool required) noexcept
		{
			return add_bounded_text(projection_bytes, value, model.limits, required);
		};
		json_value::array_type tasks;
		tasks.reserve(model.tasks.size());
		for (const auto& task : model.tasks)
		{
			if (task.batches.empty() || task.batches.size() > model.limits.max_batches_per_task ||
				task.batches.front().ordered_chunk_digests.size() >
					model.limits.max_chunks_per_batch ||
				task.coverage.size() > model.limits.max_side_channel_records ||
				task.unresolved.size() > model.limits.max_side_channel_records ||
				task.evidence.size() > model.limits.max_side_channel_records ||
				task.input_protocol_major != 1U || task.input_protocol_minor != 1U ||
				task.input_chunk_count != task.ordered_chunk_digests.size() ||
				task.raw_frame_stream_bytes == 0U || task.frame_count == 0U ||
				task.provider_task_id.empty() || task.provider_execution_id.empty() ||
				task.task_input_digest.empty() || task.selected_catalog_compile_unit_id.empty() ||
				task.compile_unit_id.empty())
				return sdk::unexpected(fail(detailed_report_error_kind::invalid_capture,
											"task_results",
											"incomplete-authority"));
			if (!accept_text(task.provider_task_id, true) ||
				!accept_text(task.provider_execution_id, true) ||
				!accept_text(task.selected_catalog_compile_unit_id, true) ||
				!accept_text(task.compile_unit_id, true) ||
				!accept_text(task.task_input_digest, true) ||
				!accept_text(task.ordered_chunk_payload_digest_set_digest, true) ||
				!accept_text(task.raw_frame_stream_digest, true) ||
				!accept_text(task.frame_transcript_digest, true) ||
				!accept_text(task.sealed_transcript_digest, true))
				return sdk::unexpected(fail(
					detailed_report_error_kind::limit_exceeded, "task_results", "bounded-string"));
			for (const auto& digest : task.ordered_chunk_digests)
				if (!accept_text(digest, true))
					return sdk::unexpected(fail(detailed_report_error_kind::limit_exceeded,
												"input_transfer.ordered_chunk_digests",
												"bounded-string"));

			auto chunk_set = json_string(task.ordered_chunk_payload_digest_set_digest);
			auto task_digest = json_string(task.task_input_digest);
			auto task_id = json_string(task.provider_task_id);
			auto execution_id = json_string(task.provider_execution_id);
			auto selected = json_string(task.selected_catalog_compile_unit_id);
			auto compile_unit = json_string(task.compile_unit_id);
			auto input_digest = json_string(task.task_input_digest);
			auto ordered_chunks = json_strings(task.ordered_chunk_digests);
			if (!chunk_set || !task_digest || !task_id || !execution_id || !selected ||
				!compile_unit || !input_digest || !ordered_chunks)
				return sdk::unexpected(invalid("task_results", "identity-string"));
			auto input_transfer = json_object({
				{"protocol_version", json_constant("1.1.0")},
				{"required_feature", json_constant("task-input-chunks-v1")},
				{"task_input_codec", json_constant("cxxlens.clang22.task.v3")},
				{"logical_input_bytes", json_value::unsigned_integer(task.logical_input_bytes)},
				{"logical_input_digest", std::move(*input_digest)},
				{"canonical_chunk_bytes", json_value::unsigned_integer(task.canonical_chunk_bytes)},
				{"chunk_count", json_value::unsigned_integer(task.input_chunk_count)},
				{"ordered_chunk_digests", std::move(*ordered_chunks)},
				{"ordered_chunk_payload_digest_set_digest", std::move(*chunk_set)},
			});
			if (!input_transfer)
				return sdk::unexpected(std::move(input_transfer.error()));
			auto raw_digest = json_string(task.raw_frame_stream_digest);
			auto frame_digest = json_string(task.frame_transcript_digest);
			auto sealed_digest = json_string(task.sealed_transcript_digest);
			if (!raw_digest || !frame_digest || !sealed_digest)
				return sdk::unexpected(invalid("runtime_receipt", "digest-string"));
			auto runtime_receipt = json_object({
				{"raw_frame_stream_bytes",
				 json_value::unsigned_integer(task.raw_frame_stream_bytes)},
				{"raw_frame_stream_digest", std::move(*raw_digest)},
				{"frame_count", json_value::unsigned_integer(task.frame_count)},
				{"frame_transcript_digest", std::move(*frame_digest)},
				{"sealed_transcript_digest", std::move(*sealed_digest)},
			});
			if (!runtime_receipt)
				return sdk::unexpected(std::move(runtime_receipt.error()));

			json_value::array_type batches;
			for (const auto& batch : task.batches)
			{
				if (batch.ordered_chunk_digests.size() > model.limits.max_chunks_per_batch ||
					!accept_text(batch.task_id, true) || !accept_text(batch.descriptor_id, true) ||
					!accept_text(batch.descriptor_digest, true) ||
					!accept_text(batch.dependency_group_id, true) ||
					!accept_text(batch.atomic_output_group_id, true) ||
					!accept_text(batch.batch_id, true) || !accept_text(batch.batch_digest, true) ||
					!accept_text(batch.row_set_digest, true))
					return sdk::unexpected(fail(detailed_report_error_kind::limit_exceeded,
												"provider_sealed_transcript.batches",
												"bounded-string"));
				for (const auto& digest : batch.ordered_chunk_digests)
					if (!accept_text(digest, true))
						return sdk::unexpected(fail(detailed_report_error_kind::limit_exceeded,
													"provider_sealed_transcript.batches",
													"chunk-digest-string"));
				auto batch_chunks = json_strings(batch.ordered_chunk_digests);
				auto batch_task = json_string(batch.task_id);
				auto descriptor = json_string(batch.descriptor_id);
				auto descriptor_digest = json_string(batch.descriptor_digest);
				auto dependency = json_string(batch.dependency_group_id);
				auto atomic = json_string(batch.atomic_output_group_id);
				auto batch_id = json_string(batch.batch_id);
				auto batch_digest = json_string(batch.batch_digest);
				auto row_digest = json_string(batch.row_set_digest);
				if (!batch_chunks || !batch_task || !descriptor || !descriptor_digest ||
					!dependency || !atomic || !batch_id || !batch_digest || !row_digest)
					return sdk::unexpected(invalid("provider_sealed_transcript.batches", "string"));
				auto value = json_object({
					{"task_id", std::move(*batch_task)},
					{"descriptor_id", std::move(*descriptor)},
					{"descriptor_digest", std::move(*descriptor_digest)},
					{"dependency_group_id", std::move(*dependency)},
					{"atomic_output_group_id", std::move(*atomic)},
					{"batch_id", std::move(*batch_id)},
					{"batch_digest", std::move(*batch_digest)},
					{"ordered_chunk_digests", std::move(*batch_chunks)},
					{"row_count", json_value::unsigned_integer(batch.row_count)},
					{"row_set_digest", std::move(*row_digest)},
				});
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				batches.push_back(std::move(*value));
			}
			json_value::array_type coverage;
			for (const auto& record : task.coverage)
			{
				if (!accept_text(record.kind, true) || !accept_text(record.id, true) ||
					!accept_text(record.state, true) || !accept_text(record.reason, false))
					return sdk::unexpected(fail(
						detailed_report_error_kind::limit_exceeded, "coverage", "bounded-string"));
				auto kind = json_string(record.kind);
				auto id = json_string(record.id);
				auto state = json_string(record.state);
				auto reason = json_string(record.reason);
				if (!kind || !id || !state || !reason)
					return sdk::unexpected(invalid("coverage", "string"));
				auto value = json_object({{"kind", std::move(*kind)},
										  {"id", std::move(*id)},
										  {"state", std::move(*state)},
										  {"reason", std::move(*reason)}});
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				coverage.push_back(std::move(*value));
			}
			json_value::array_type unresolved;
			for (const auto& record : task.unresolved)
			{
				if (!accept_text(record.code, true) || !accept_text(record.subject, true) ||
					!accept_text(record.detail, false))
					return sdk::unexpected(fail(detailed_report_error_kind::limit_exceeded,
												"unresolved",
												"bounded-string"));
				auto code = json_string(record.code);
				auto subject = json_string(record.subject);
				auto detail = json_string(record.detail);
				if (!code || !subject || !detail)
					return sdk::unexpected(invalid("unresolved", "string"));
				auto value = json_object({{"code", std::move(*code)},
										  {"subject", std::move(*subject)},
										  {"detail", std::move(*detail)}});
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				unresolved.push_back(std::move(*value));
			}
			json_value::array_type evidence;
			for (const auto& record : task.evidence)
			{
				if (!accept_text(record.kind, true) || !accept_text(record.subject, true) ||
					!accept_text(record.producer, true) || !accept_text(record.summary, false))
					return sdk::unexpected(fail(
						detailed_report_error_kind::limit_exceeded, "evidence", "bounded-string"));
				auto kind = json_string(record.kind);
				auto subject = json_string(record.subject);
				auto producer = json_string(record.producer);
				auto summary = json_string(record.summary);
				if (!kind || !subject || !producer || !summary)
					return sdk::unexpected(invalid("evidence", "string"));
				auto value = json_object({{"kind", std::move(*kind)},
										  {"subject", std::move(*subject)},
										  {"producer", std::move(*producer)},
										  {"summary", std::move(*summary)}});
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				evidence.push_back(std::move(*value));
			}
			auto transcript = json_object({
				{"batch_count", json_value::unsigned_integer(batches.size())},
				{"batches", json_value::array(std::move(batches))},
				{"coverage_count", json_value::unsigned_integer(coverage.size())},
				{"coverage", json_value::array(std::move(coverage))},
				{"unresolved_count", json_value::unsigned_integer(unresolved.size())},
				{"unresolved", json_value::array(std::move(unresolved))},
				{"evidence_count", json_value::unsigned_integer(evidence.size())},
				{"evidence", json_value::array(std::move(evidence))},
			});
			if (!transcript)
				return sdk::unexpected(std::move(transcript.error()));
			auto task_value = json_object({
				{"provider_task_id", std::move(*task_id)},
				{"provider_execution_id", std::move(*execution_id)},
				{"selected_catalog_compile_unit_id", std::move(*selected)},
				{"compile_unit_id", std::move(*compile_unit)},
				{"task_input_digest", std::move(*task_digest)},
				{"terminal", json_constant("provider.success")},
				{"input_transfer", std::move(*input_transfer)},
				{"runtime_receipt", std::move(*runtime_receipt)},
				{"provider_sealed_transcript", std::move(*transcript)},
			});
			if (!task_value)
				return sdk::unexpected(std::move(task_value.error()));
			tasks.push_back(std::move(*task_value));
		}

		auto accept_publication = [&](const detailed_publication_projection& value) noexcept
		{
			return accept_text(value.publication_id, true) && accept_text(value.series_id, true) &&
				accept_text(value.snapshot_id, true) &&
				(!value.parent_publication || accept_text(*value.parent_publication, true));
		};
		if (!accept_text(model.store.backend, true) || !accept_text(model.store.series_id, true) ||
			!accept_text(model.store.selector_id, true) ||
			!accept_publication(*model.store.published_record) ||
			!accept_publication(*model.store.candidate_identity))
			return sdk::unexpected(
				fail(detailed_report_error_kind::limit_exceeded, "publication", "bounded-string"));
		for (const auto& receipt : model.store.verification)
			if (!accept_text(receipt.path, true) || !accept_text(receipt.status, true) ||
				(receipt.error_code && !accept_text(*receipt.error_code, true)) ||
				(receipt.error_field && !accept_text(*receipt.error_field, false)))
				return sdk::unexpected(fail(detailed_report_error_kind::limit_exceeded,
											"publication.verification",
											"bounded-string"));
		auto published = publication_json(*model.store.published_record);
		auto candidate = publication_json(*model.store.candidate_identity);
		auto backend = json_string(model.store.backend);
		if (!published || !candidate || !backend)
			return sdk::unexpected(invalid("publication", "string"));
		json_value::array_type verification;
		for (const auto& receipt : model.store.verification)
		{
			auto path = json_string(receipt.path);
			auto status = json_string(receipt.status);
			auto code = receipt.error_code ? json_string(*receipt.error_code)
										   : sdk::result<json_value>{json_value::null()};
			auto field = receipt.error_field ? json_string(*receipt.error_field)
											 : sdk::result<json_value>{json_value::null()};
			if (!path || !status || !code || !field)
				return sdk::unexpected(invalid("publication.verification", "string"));
			auto value = json_object({{"path", std::move(*path)},
									  {"status", std::move(*status)},
									  {"error_code", std::move(*code)},
									  {"error_field", std::move(*field)}});
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			verification.push_back(std::move(*value));
		}
		auto series_id = json_string(model.store.series_id);
		auto selector_id = json_string(model.store.selector_id);
		if (!series_id || !selector_id)
			return sdk::unexpected(invalid("publication", "selector-string"));
		auto publication = json_object({
			{"backend", std::move(*backend)},
			{"series_id", std::move(*series_id)},
			{"selector_id", std::move(*selector_id)},
			{"publication_attempted", json_value::boolean(model.store.publication_attempted)},
			{"outcome", json_constant("committed_verified")},
			{"candidate_identity_state", json_constant("constructed")},
			{"candidate_identity", std::move(*candidate)},
			{"published_record", std::move(*published)},
			{"invocation_commit_state", json_constant("committed")},
			{"committed_transaction_count",
			 json_value::unsigned_integer(model.store.publish_call_count)},
			{"prior_history_retained", json_value::boolean(model.store.prior_history_retained)},
			{"verification", json_value::array(std::move(verification))},
		});
		if (!publication)
			return sdk::unexpected(std::move(publication.error()));
		// This projection is deliberately a different schema and result vocabulary.  It is not
		// admissible as the public success report until the authority/schema issues are resolved.
		auto schema =
			json_string("cxxlens.clang22-materialization-report.source-private-bounded.v1");
		auto version = json_string("1.0.0");
		auto response = json_string("detailed_projection");
		auto result = json_string("projection_ready");
		auto generated = json_string(model.generated_at);
		if (!schema || !version || !response || !result || !generated)
			return sdk::unexpected(invalid("report", "root-string"));
		auto root = json_object({
			{"schema", std::move(*schema)},
			{"report_version", std::move(*version)},
			{"response_kind", std::move(*response)},
			{"result", std::move(*result)},
			{"generated_at", std::move(*generated)},
			{"process_exit_status", json_value::unsigned_integer(0U)},
			{"task_results", json_value::array(std::move(tasks))},
			{"publication", std::move(*publication)},
			{"projection", json_constant("source-private-bounded")},
		});
		if (!root)
			return sdk::unexpected(std::move(root.error()));
		auto encoded = canonical_json_line(*root);
		if (encoded.size() > model.limits.max_projection_bytes)
			return sdk::unexpected(
				fail(detailed_report_error_kind::limit_exceeded, "report", "projection-bytes"));
		return encoded;
	}

	sdk::result<std::string>
	encode_compact_failure_report(const compact_failure_authority& authority, std::string timestamp)
	{
		if (!authority.valid() || !generated_at(timestamp))
			return sdk::unexpected(invalid("report", "compact-authority-or-time"));

		const auto& input = authority.raw_input();
		const auto& binding = authority.request_binding();
		const auto& error = authority.error();

		auto schema = string("cxxlens.clang22-materialization-report.v2");
		auto report_version = string("2.1.0");
		auto response_kind = string("compact_failure");
		auto result = string("failed");
		auto generated = string(std::move(timestamp));
		auto prefix_digest = string(input.observed_prefix_digest);
		auto binding_state = string(binding ? "request-bound" : "raw-input-only");
		auto store_state = string(std::string{text(authority.store_draft_state())});
		auto head_state = string(std::string{text(authority.head_observation())});
		auto code = string(error.code);
		auto phase = string(std::string{text(authority.phase())});
		auto subject = string(error.subject);
		auto diagnostic = string(error.diagnostic);
		if (!schema || !report_version || !response_kind || !result || !generated ||
			!prefix_digest || !binding_state || !store_state || !head_state || !code || !phase ||
			!subject || !diagnostic)
			return sdk::unexpected(invalid("report", "string"));

		auto raw = object({
			{"byte_limit", json_value::unsigned_integer(input.byte_limit)},
			{"complete", json_value::boolean(input.complete)},
			{"observed_prefix_digest", std::move(*prefix_digest)},
			{"observed_size_bytes", json_value::unsigned_integer(input.observed_size_bytes)},
		});
		json_value request = json_value::null();
		if (binding)
		{
			auto id = string(binding->materialization_request_id);
			auto request_digest = string(binding->request_digest);
			auto semantic_digest = string(binding->semantic_request_digest);
			if (!id || !request_digest || !semantic_digest)
				return sdk::unexpected(invalid("report.binding", "string"));
			auto value = object({
				{"materialization_request_id", std::move(*id)},
				{"request_digest", std::move(*request_digest)},
				{"semantic_request_digest", std::move(*semantic_digest)},
			});
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			request = std::move(*value);
		}
		auto binding_value = object({
			{"request", std::move(request)},
			{"state", std::move(*binding_state)},
		});
		json_value observed_head = json_value::null();
		if (authority.observed_head_publication())
		{
			auto value = string(*authority.observed_head_publication());
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			observed_head = std::move(*value);
		}
		json_value store_cause = json_value::null();
		if (authority.store_failure_cause())
		{
			const auto& cause = *authority.store_failure_cause();
			auto kind = string("sdk_error");
			auto operation = string(std::string{text(cause.operation)});
			auto cause_code = string(cause.code);
			auto field = string(cause.field);
			if (!kind || !operation || !cause_code || !field)
				return sdk::unexpected(invalid("report.effects", "store-cause-string"));
			auto detail_kind = string("opaque");
			auto detail_digest = string(cause.detail_digest);
			auto detail_diagnostic = string(cause.detail);
			if (!detail_kind || !detail_digest || !detail_diagnostic)
				return sdk::unexpected(invalid("report.effects", "store-detail-string"));
			auto detail = object({
				{"byte_count", json_value::unsigned_integer(cause.detail_byte_count)},
				{"diagnostic", std::move(*detail_diagnostic)},
				{"digest", std::move(*detail_digest)},
				{"kind", std::move(*detail_kind)},
			});
			if (!detail)
				return sdk::unexpected(std::move(detail.error()));
			json_value access_path = json_value::null();
			if (cause.path)
			{
				auto path = string(std::string{text(*cause.path)});
				if (!path)
					return sdk::unexpected(invalid("report.effects", "store-path-string"));
				access_path = std::move(*path);
			}
			auto value = object({
				{"access_path", std::move(access_path)},
				{"code", std::move(*cause_code)},
				{"detail", std::move(*detail)},
				{"field", std::move(*field)},
				{"kind", std::move(*kind)},
				{"operation", std::move(*operation)},
			});
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			store_cause = std::move(*value);
		}
		auto effect_value = object({
			{"committed_transaction_count", json_value::unsigned_integer(0U)},
			{"head_observation", std::move(*head_state)},
			{"observed_head_publication", std::move(observed_head)},
			{"prior_history_retained", json_value::boolean(true)},
			{"publication_attempted", json_value::boolean(false)},
			{"store_draft_state", std::move(*store_state)},
			{"store_failure_cause", std::move(store_cause)},
			{"worker_launch_attempt_count",
			 json_value::unsigned_integer(authority.worker_launch_attempt_count())},
			{"worker_launch_success_count",
			 json_value::unsigned_integer(authority.worker_launch_success_count())},
		});
		auto error_value = object({
			{"code", std::move(*code)},
			{"diagnostic", std::move(*diagnostic)},
			{"phase", std::move(*phase)},
			{"subject", std::move(*subject)},
		});
		if (!raw || !binding_value || !effect_value || !error_value)
			return sdk::unexpected(invalid("report", "object"));
		auto root = object({
			{"binding", std::move(*binding_value)},
			{"effects", std::move(*effect_value)},
			{"error", std::move(*error_value)},
			{"generated_at", std::move(*generated)},
			{"process_exit_status", json_value::unsigned_integer(1U)},
			{"raw_input_observation", std::move(*raw)},
			{"report_version", std::move(*report_version)},
			{"response_kind", std::move(*response_kind)},
			{"result", std::move(*result)},
			{"schema", std::move(*schema)},
		});
		if (!root)
			return sdk::unexpected(std::move(root.error()));
		return canonical_json_line(*root);
	}
} // namespace cxxlens::detail::clang22::materialization
