#include "materialization_report.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <span>
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
				case detailed_report_error_kind::spool_io:
					code = "spool-io";
					break;
				case detailed_report_error_kind::spool_corrupt:
					code = "spool-corrupt";
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

		[[nodiscard]] std::string digest_text(const std::string_view value)
		{
			return sdk::content_digest(std::as_bytes(std::span{value.data(), value.size()}));
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
					   std::size_t& projection_bytes,
					   std::vector<detailed_provider_batch_projection::row_projection>& projections)
		{
			std::string projection;
			if (rows.size() > limits.max_side_channel_records)
				return sdk::unexpected(fail(detailed_report_error_kind::limit_exceeded,
											"provider_sealed_transcript.rows",
											"count"));
			projections.clear();
			projections.reserve(rows.size());
			for (std::size_t index{}; index < rows.size(); ++index)
			{
				std::string row = rows[index].canonical_form();
				if (!bounded_text(row, limits) || !sdk::validate_utf8_text(row))
					return sdk::unexpected(fail(detailed_report_error_kind::invalid_capture,
												"provider_sealed_transcript.rows",
												"canonical-form"));
				const auto row_digest = digest_text(row);
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
				projections.push_back({index, std::move(row), std::move(row_digest)});
			}
			return sdk::semantic_digest("cxxlens.clang22.materialization-report.row-set.v1",
										projection);
		}

		[[nodiscard]] sdk::result<void>
		validate_primary_span(const observation_v2_primary_span& span,
							  const detailed_report_limits& limits)
		{
			for (const auto& [field, value] : {
					 std::pair{std::string_view{"span_id"}, std::string_view{span.span_id}},
					 std::pair{std::string_view{"snapshot"}, std::string_view{span.snapshot}},
					 std::pair{std::string_view{"file"}, std::string_view{span.file}},
					 std::pair{std::string_view{"role"}, std::string_view{span.role}},
				 })
				if (!bounded_text(value, limits) || !sdk::validate_strong_id(value))
					return sdk::unexpected(
						fail(detailed_report_error_kind::invalid_capture,
							 "observation_rows.primary_span." + std::string{field},
							 "strong-id"));
			if (span.begin > span.end)
				return sdk::unexpected(fail(detailed_report_error_kind::invalid_capture,
											"observation_rows.primary_span.range",
											"order"));
			auto expected = sdk::source_span_identity(
				span.snapshot, span.file, span.begin, span.end, span.role);
			if (!expected || *expected != span.span_id)
				return sdk::unexpected(fail(detailed_report_error_kind::transcript_mismatch,
											"observation_rows.primary_span.span_id",
											"identity"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_observation_row(const sealed_observation_v2_row& value,
								 const sdk::provider::detail::sealed_provider_batch& batch,
								 const std::string_view compile_unit,
								 const detailed_report_limits& limits)
		{
			auto descriptor = observation_v2_descriptor(value.observation.kind);
			if (!descriptor || (*descriptor)->id != batch.descriptor_id())
				return sdk::unexpected(fail(detailed_report_error_kind::transcript_mismatch,
											"observation_rows.descriptor",
											"batch-binding"));
			if (!bounded_text(value.observation.final_relation_compile_unit_id, limits) ||
				value.observation.final_relation_compile_unit_id != compile_unit ||
				!sdk::validate_strong_id(value.observation.final_relation_compile_unit_id))
				return sdk::unexpected(fail(detailed_report_error_kind::transcript_mismatch,
											"observation_rows.compile_unit",
											"task-binding"));
			if (!bounded_text(value.observation.semantic_key, limits) ||
				!sdk::validate_utf8_text(value.observation.semantic_key) ||
				!bounded_text(value.observation.payload_digest, limits))
				return sdk::unexpected(fail(detailed_report_error_kind::invalid_capture,
											"observation_rows.observation",
											"bounded-value"));
			if (value.observation.limitation &&
				(!bounded_text(*value.observation.limitation, limits) ||
				 !sdk::validate_utf8_text(*value.observation.limitation)))
				return sdk::unexpected(fail(detailed_report_error_kind::invalid_capture,
											"observation_rows.limitation",
											"utf8"));
			if (value.observation.exact_equivalence != !value.observation.limitation.has_value())
				return sdk::unexpected(fail(detailed_report_error_kind::transcript_mismatch,
											"observation_rows.exact_equivalence",
											"limitation-coupling"));
			if (value.observation.kind == observation_v2_kind::type &&
				value.observation.primary_span)
				return sdk::unexpected(fail(detailed_report_error_kind::transcript_mismatch,
											"observation_rows.primary_span",
											"type-forbidden"));
			if (value.observation.primary_span)
				if (auto valid = validate_primary_span(*value.observation.primary_span, limits);
					!valid)
					return valid;
			return {};
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

		constexpr std::size_t maximum_publication_text_bytes = 512U;
		constexpr std::size_t maximum_diagnostic_code_bytes = 64U;
		constexpr std::size_t maximum_diagnostic_phase_bytes = 64U;
		constexpr std::size_t maximum_diagnostic_bytes = 4096U;
		constexpr std::size_t maximum_json_string_expansion = 6U;
		constexpr std::size_t maximum_json_u64_bytes = 20U;

		[[nodiscard]] bool checked_add(std::size_t& total, const std::size_t value) noexcept
		{
			if (value > std::numeric_limits<std::size_t>::max() - total)
				return false;
			total += value;
			return true;
		}

		[[nodiscard]] std::optional<std::size_t>
		json_string_capacity(const std::size_t payload_bytes) noexcept
		{
			if (payload_bytes >
				std::numeric_limits<std::size_t>::max() / maximum_json_string_expansion)
				return std::nullopt;
			std::size_t value = payload_bytes * maximum_json_string_expansion;
			if (!checked_add(value, 2U))
				return std::nullopt;
			return value;
		}

		struct json_capacity_object
		{
			std::size_t bytes{1U};
			std::size_t member_count{};

			[[nodiscard]] bool add_member(const std::string_view name,
										  const std::size_t value_bytes) noexcept
			{
				if (member_count != 0U && !checked_add(bytes, 1U))
					return false;
				auto encoded_name = json_string_capacity(name.size());
				if (!encoded_name || !checked_add(bytes, *encoded_name) ||
					!checked_add(bytes, 1U) || !checked_add(bytes, value_bytes))
					return false;
				++member_count;
				return true;
			}

			[[nodiscard]] std::optional<std::size_t> finish() noexcept
			{
				if (!checked_add(bytes, 1U))
					return std::nullopt;
				return bytes;
			}
		};

		struct json_capacity_array
		{
			std::size_t bytes{1U};
			std::size_t element_count{};

			[[nodiscard]] bool add_element(const std::size_t value_bytes) noexcept
			{
				if (element_count != 0U && !checked_add(bytes, 1U))
					return false;
				if (!checked_add(bytes, value_bytes))
					return false;
				++element_count;
				return true;
			}

			[[nodiscard]] std::optional<std::size_t> finish() noexcept
			{
				if (!checked_add(bytes, 1U))
					return std::nullopt;
				return bytes;
			}
		};

		template <class Consumer>
		[[nodiscard]] std::optional<std::size_t> json_array_capacity(const std::size_t count,
																	 Consumer&& consume) noexcept
		{
			json_capacity_array output;
			for (std::size_t index{}; index < count; ++index)
			{
				auto value = consume(index);
				if (!value || !output.add_element(*value))
					return std::nullopt;
			}
			return output.finish();
		}

		[[nodiscard]] std::optional<std::size_t>
		bounded_json_string_capacity(const std::string_view value,
									 const detailed_report_limits& limits) noexcept
		{
			if (value.size() > limits.max_string_bytes || !sdk::validate_utf8_text(value))
				return std::nullopt;
			return json_string_capacity(value.size());
		}

		[[nodiscard]] std::optional<std::size_t>
		publication_json_string_capacity(const std::string_view value,
										 const detailed_report_limits& limits) noexcept
		{
			const auto maximum = std::min(limits.max_string_bytes, maximum_publication_text_bytes);
			if (value.empty() || value.size() > maximum || !sdk::validate_utf8_text(value))
				return std::nullopt;
			return json_string_capacity(maximum);
		}

		[[nodiscard]] std::optional<std::size_t>
		publication_identity_capacity(const detailed_publication_projection& value,
									  const detailed_report_limits& limits) noexcept
		{
			json_capacity_object output;
			auto publication_id = publication_json_string_capacity(value.publication_id, limits);
			auto series_id = publication_json_string_capacity(value.series_id, limits);
			auto snapshot_id = publication_json_string_capacity(value.snapshot_id, limits);
			auto parent = value.parent_publication
				? publication_json_string_capacity(*value.parent_publication, limits)
				: std::optional<std::size_t>{4U};
			if (!publication_id || !series_id || !snapshot_id || !parent ||
				!output.add_member("publication_id", *publication_id) ||
				!output.add_member("series_id", *series_id) ||
				!output.add_member("snapshot_id", *snapshot_id) ||
				!output.add_member("sequence", maximum_json_u64_bytes) ||
				!output.add_member("physical_generation", maximum_json_u64_bytes) ||
				!output.add_member("parent_publication", *parent))
				return std::nullopt;
			return output.finish();
		}

		[[nodiscard]] std::optional<std::size_t>
		detailed_batch_capacity(const detailed_provider_batch_projection& batch,
								const detailed_report_limits& limits) noexcept
		{
			json_capacity_object output;
			const auto string = [&](const std::string_view value)
			{
				return bounded_json_string_capacity(value, limits);
			};
			const auto chunks =
				json_array_capacity(batch.ordered_chunk_digests.size(),
									[&](const std::size_t index)
									{
										return string(batch.ordered_chunk_digests[index]);
									});
			auto task_id = string(batch.task_id);
			auto descriptor_id = string(batch.descriptor_id);
			auto descriptor_digest = string(batch.descriptor_digest);
			auto dependency = string(batch.dependency_group_id);
			auto atomic = string(batch.atomic_output_group_id);
			auto batch_id = string(batch.batch_id);
			auto batch_digest = string(batch.batch_digest);
			auto row_digest = string(batch.row_set_digest);
			if (!chunks || !task_id || !descriptor_id || !descriptor_digest || !dependency ||
				!atomic || !batch_id || !batch_digest || !row_digest ||
				!output.add_member("task_id", *task_id) ||
				!output.add_member("descriptor_id", *descriptor_id) ||
				!output.add_member("descriptor_digest", *descriptor_digest) ||
				!output.add_member("dependency_group_id", *dependency) ||
				!output.add_member("atomic_output_group_id", *atomic) ||
				!output.add_member("batch_id", *batch_id) ||
				!output.add_member("batch_digest", *batch_digest) ||
				!output.add_member("ordered_chunk_digests", *chunks) ||
				!output.add_member("row_count", maximum_json_u64_bytes) ||
				!output.add_member("row_set_digest", *row_digest))
				return std::nullopt;
			return output.finish();
		}

		[[nodiscard]] std::optional<std::size_t>
		detailed_task_capacity(const detailed_task_report_capture& task,
							   const detailed_report_limits& limits) noexcept
		{
			json_capacity_object output;
			const auto string = [&](const std::string_view value)
			{
				return bounded_json_string_capacity(value, limits);
			};
			const auto chunks =
				json_array_capacity(task.ordered_chunk_digests.size(),
									[&](const std::size_t index)
									{
										return string(task.ordered_chunk_digests[index]);
									});
			json_capacity_object input_transfer;
			auto logical_digest = string(task.task_input_digest);
			auto chunk_digest_set = string(task.ordered_chunk_payload_digest_set_digest);
			auto input_transfer_value = [&]() -> std::optional<std::size_t>
			{
				if (!logical_digest || !chunk_digest_set || !chunks ||
					!input_transfer.add_member(
						"protocol_version",
						json_string_capacity(std::string_view{"1.1.0"}.size()).value()) ||
					!input_transfer.add_member(
						"required_feature",
						json_string_capacity(std::string_view{"task-input-chunks-v1"}.size())
							.value()) ||
					!input_transfer.add_member(
						"task_input_codec",
						json_string_capacity(std::string_view{"cxxlens.clang22.task.v3"}.size())
							.value()) ||
					!input_transfer.add_member("logical_input_bytes", maximum_json_u64_bytes) ||
					!input_transfer.add_member("logical_input_digest", *logical_digest) ||
					!input_transfer.add_member("canonical_chunk_bytes", maximum_json_u64_bytes) ||
					!input_transfer.add_member("chunk_count", maximum_json_u64_bytes) ||
					!input_transfer.add_member("ordered_chunk_digests", *chunks) ||
					!input_transfer.add_member("ordered_chunk_payload_digest_set_digest",
											   *chunk_digest_set))
					return std::nullopt;
				return input_transfer.finish();
			}();

			json_capacity_object runtime_receipt;
			auto raw_digest = string(task.raw_frame_stream_digest);
			auto frame_digest = string(task.frame_transcript_digest);
			auto sealed_digest = string(task.sealed_transcript_digest);
			std::optional<std::size_t> runtime_receipt_value;
			if (raw_digest && frame_digest && sealed_digest &&
				runtime_receipt.add_member("raw_frame_stream_bytes", maximum_json_u64_bytes) &&
				runtime_receipt.add_member("raw_frame_stream_digest", *raw_digest) &&
				runtime_receipt.add_member("frame_count", maximum_json_u64_bytes) &&
				runtime_receipt.add_member("frame_transcript_digest", *frame_digest) &&
				runtime_receipt.add_member("sealed_transcript_digest", *sealed_digest))
				runtime_receipt_value = runtime_receipt.finish();

			const auto coverage_capacity = json_array_capacity(
				task.coverage.size(),
				[&](const std::size_t index) -> std::optional<std::size_t>
				{
					const auto& record = task.coverage[index];
					json_capacity_object value;
					auto kind = string(record.kind);
					auto id = string(record.id);
					auto state = string(record.state);
					auto reason = string(record.reason);
					if (!kind || !id || !state || !reason || !value.add_member("kind", *kind) ||
						!value.add_member("id", *id) || !value.add_member("state", *state) ||
						!value.add_member("reason", *reason))
						return std::nullopt;
					return value.finish();
				});
			const auto unresolved_capacity = json_array_capacity(
				task.unresolved.size(),
				[&](const std::size_t index) -> std::optional<std::size_t>
				{
					const auto& record = task.unresolved[index];
					json_capacity_object value;
					auto code = string(record.code);
					auto subject = string(record.subject);
					auto detail = string(record.detail);
					if (!code || !subject || !detail || !value.add_member("code", *code) ||
						!value.add_member("subject", *subject) ||
						!value.add_member("detail", *detail))
						return std::nullopt;
					return value.finish();
				});
			const auto evidence_capacity =
				json_array_capacity(task.evidence.size(),
									[&](const std::size_t index) -> std::optional<std::size_t>
									{
										const auto& record = task.evidence[index];
										json_capacity_object value;
										auto kind = string(record.kind);
										auto subject = string(record.subject);
										auto producer = string(record.producer);
										auto summary = string(record.summary);
										if (!kind || !subject || !producer || !summary ||
											!value.add_member("kind", *kind) ||
											!value.add_member("subject", *subject) ||
											!value.add_member("producer", *producer) ||
											!value.add_member("summary", *summary))
											return std::nullopt;
										return value.finish();
									});
			const auto batches_capacity =
				json_array_capacity(task.batches.size(),
									[&](const std::size_t index)
									{
										return detailed_batch_capacity(task.batches[index], limits);
									});
			json_capacity_object transcript;
			std::optional<std::size_t> transcript_value;
			if (batches_capacity && coverage_capacity && unresolved_capacity && evidence_capacity &&
				transcript.add_member("batch_count", maximum_json_u64_bytes) &&
				transcript.add_member("batches", *batches_capacity) &&
				transcript.add_member("coverage_count", maximum_json_u64_bytes) &&
				transcript.add_member("coverage", *coverage_capacity) &&
				transcript.add_member("unresolved_count", maximum_json_u64_bytes) &&
				transcript.add_member("unresolved", *unresolved_capacity) &&
				transcript.add_member("evidence_count", maximum_json_u64_bytes) &&
				transcript.add_member("evidence", *evidence_capacity))
				transcript_value = transcript.finish();

			auto provider_task_id = string(task.provider_task_id);
			auto provider_execution_id = string(task.provider_execution_id);
			auto selected = string(task.selected_catalog_compile_unit_id);
			auto compile_unit = string(task.compile_unit_id);
			auto task_digest = string(task.task_input_digest);
			if (!input_transfer_value || !runtime_receipt_value || !transcript_value ||
				!provider_task_id || !provider_execution_id || !selected || !compile_unit ||
				!task_digest || !output.add_member("provider_task_id", *provider_task_id) ||
				!output.add_member("provider_execution_id", *provider_execution_id) ||
				!output.add_member("selected_catalog_compile_unit_id", *selected) ||
				!output.add_member("compile_unit_id", *compile_unit) ||
				!output.add_member("task_input_digest", *task_digest) ||
				!output.add_member(
					"terminal",
					json_string_capacity(std::string_view{"provider.success"}.size()).value()) ||
				!output.add_member("input_transfer", *input_transfer_value) ||
				!output.add_member("runtime_receipt", *runtime_receipt_value) ||
				!output.add_member("provider_sealed_transcript", *transcript_value))
				return std::nullopt;
			return output.finish();
		}

		[[nodiscard]] std::optional<std::size_t>
		verification_capacity(const std::span<const detailed_store_access_projection> receipts,
							  const detailed_report_limits& limits) noexcept
		{
			return json_array_capacity(
				receipts.size(),
				[&](const std::size_t index) -> std::optional<std::size_t>
				{
					const auto& receipt = receipts[index];
					json_capacity_object value;
					auto path = bounded_json_string_capacity(receipt.path, limits);
					auto status = bounded_json_string_capacity(receipt.status, limits);
					auto code = receipt.error_code
						? bounded_json_string_capacity(*receipt.error_code, limits)
						: std::optional<std::size_t>{4U};
					auto field = receipt.error_field
						? bounded_json_string_capacity(*receipt.error_field, limits)
						: std::optional<std::size_t>{4U};
					if (!path || !status || !code || !field || !value.add_member("path", *path) ||
						!value.add_member("status", *status) ||
						!value.add_member("error_code", *code) ||
						!value.add_member("error_field", *field))
						return std::nullopt;
					return value.finish();
				});
		}

		[[nodiscard]] std::optional<std::size_t>
		publication_capacity(const detailed_success_report_model& model) noexcept
		{
			const auto& limits = model.limits;
			json_capacity_object output;
			auto backend = bounded_json_string_capacity(model.store.backend, limits);
			auto series_id = publication_json_string_capacity(model.store.series_id, limits);
			auto selector_id = publication_json_string_capacity(model.store.selector_id, limits);
			auto candidate = publication_identity_capacity(*model.store.candidate_identity, limits);
			auto published = publication_identity_capacity(*model.store.published_record, limits);
			if (!backend || !series_id || !selector_id || !candidate || !published ||
				!output.add_member("backend", *backend) ||
				!output.add_member("series_id", *series_id) ||
				!output.add_member("selector_id", *selector_id) ||
				!output.add_member("publication_attempted", 5U) ||
				!output.add_member(
					"outcome",
					json_string_capacity(std::string_view{"committed_verified"}.size()).value()) ||
				!output.add_member(
					"candidate_identity_state",
					json_string_capacity(std::string_view{"constructed"}.size()).value()) ||
				!output.add_member("candidate_identity", *candidate) ||
				!output.add_member("published_record", *published) ||
				!output.add_member(
					"invocation_commit_state",
					json_string_capacity(std::string_view{"committed"}.size()).value()) ||
				!output.add_member("committed_transaction_count", maximum_json_u64_bytes) ||
				!output.add_member("prior_history_retained", 5U) ||
				!output.add_member("verification", 4U))
				return std::nullopt;
			return output.finish();
		}

		[[nodiscard]] std::optional<std::size_t>
		final_framing_capacity(const std::size_t task_count) noexcept
		{
			(void)task_count;
			json_capacity_object output;
			const auto string = [](const std::string_view value)
			{
				return json_string_capacity(value.size());
			};
			if (!output.add_member(
					"schema",
					*string("cxxlens.clang22-materialization-report.source-private-bounded.v1")) ||
				!output.add_member("report_version", *string("1.0.0")) ||
				!output.add_member("response_kind", *string("detailed_projection")) ||
				!output.add_member("result", *string("projection_ready")) ||
				!output.add_member("generated_at", 4U) ||
				!output.add_member("process_exit_status", 1U) ||
				!output.add_member("task_results", 4U) || !output.add_member("publication", 4U) ||
				!output.add_member("projection", *string("source-private-bounded")))
				return std::nullopt;
			auto result = output.finish();
			if (!result || !checked_add(*result, 1U))
				return std::nullopt;
			return result;
		}

		[[nodiscard]] std::optional<std::size_t>
		maximum_diagnostic_capacity(const detailed_report_limits& limits) noexcept
		{
			const auto diagnostic = std::min(limits.max_string_bytes, maximum_diagnostic_bytes);
			json_capacity_object output;
			auto code = json_string_capacity(maximum_diagnostic_code_bytes);
			auto phase = json_string_capacity(maximum_diagnostic_phase_bytes);
			auto subject = json_string_capacity(
				std::min(limits.max_string_bytes, maximum_publication_text_bytes));
			auto detail = json_string_capacity(diagnostic);
			if (!code || !phase || !subject || !detail || !output.add_member("code", *code) ||
				!output.add_member("phase", *phase) || !output.add_member("subject", *subject) ||
				!output.add_member("diagnostic", *detail))
				return std::nullopt;
			auto value = output.finish();
			if (!value)
				return std::nullopt;
			auto field = json_string_capacity(std::string_view{"error"}.size());
			if (!field || !checked_add(*field, 1U) || !checked_add(*field, *value))
				return std::nullopt;
			return field;
		}

	} // namespace

	sdk::result<detailed_report_capacity_bound>
	checked_detailed_report_capacity_upper_bound(const detailed_success_report_model& model)
	{
		if (model.limits.max_tasks == 0U || model.limits.max_batches_per_task == 0U ||
			model.limits.max_chunks_per_batch == 0U ||
			model.limits.max_side_channel_records == 0U || model.limits.max_string_bytes == 0U ||
			model.limits.max_projection_bytes == 0U)
			return sdk::unexpected(
				fail(detailed_report_error_kind::invalid_capture, "limits", "zero"));
		if (model.tasks.empty() || model.tasks.size() > model.limits.max_tasks)
			return sdk::unexpected(
				fail(detailed_report_error_kind::limit_exceeded, "task_results", "count"));
		if (!generated_at(model.generated_at))
			return sdk::unexpected(
				fail(detailed_report_error_kind::invalid_time, "generated_at", "utc-second"));
		if (!model.store.published_record || !model.store.candidate_identity ||
			!exact_success_verification(model.store.verification))
			return sdk::unexpected(fail(detailed_report_error_kind::publication_unverified,
										"publication",
										"committed-verified-required"));

		try
		{
			const auto task_values = json_array_capacity(model.tasks.size(),
														 [&](const std::size_t index)
														 {
															 return detailed_task_capacity(
																 model.tasks[index], model.limits);
														 });
			auto generated = bounded_json_string_capacity(model.generated_at, model.limits);
			auto independent =
				task_values && generated ? std::optional<std::size_t>{*task_values} : std::nullopt;
			if (independent && !checked_add(*independent, *generated))
				independent = std::nullopt;
			auto framing = final_framing_capacity(model.tasks.size());
			auto publication = publication_capacity(model);
			auto receipts = verification_capacity(model.store.verification, model.limits);
			auto diagnostics = maximum_diagnostic_capacity(model.limits);
			if (!independent || !framing || !publication || !receipts || !diagnostics)
				return sdk::unexpected(fail(detailed_report_error_kind::limit_exceeded,
											"report",
											"capacity-overflow-or-unbounded-value"));

			detailed_report_capacity_bound result{
				*independent, *framing, *publication, *receipts, *diagnostics, 0U};
			if (!checked_add(result.total, result.publication_independent_projection) ||
				!checked_add(result.total, result.final_json_framing) ||
				!checked_add(result.total, result.exact_publication_outcome) ||
				!checked_add(result.total, result.exact_sdk_records_and_receipts) ||
				!checked_add(result.total, result.maximum_bounded_diagnostics))
				return sdk::unexpected(fail(
					detailed_report_error_kind::limit_exceeded, "report", "capacity-overflow"));
			return result;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				fail(detailed_report_error_kind::spool_io, "report", "capacity-allocation"));
		}
	}

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

		constexpr std::array<std::string_view, 5U> base_descriptor_ids{"build.project.v1",
																	   "build.toolchain_context.v1",
																	   "build.variant.v1",
																	   "source.file.v1",
																	   "build.compile_unit.v1"};
		const auto copy_and_validate_rows =
			[&](const std::span<const sdk::detached_row> rows,
				std::string_view field,
				std::span<const std::string_view> expected,
				std::vector<sdk::detached_row>& destination) -> sdk::result<void>
		{
			if (rows.size() != expected.size() || rows.size() > limits.max_side_channel_records)
				return sdk::unexpected(fail(detailed_report_error_kind::transcript_mismatch,
											std::string{field},
											"row-count"));
			destination.clear();
			destination.reserve(rows.size());
			for (std::size_t index{}; index < rows.size(); ++index)
			{
				if (rows[index].descriptor_id != expected[index])
					return sdk::unexpected(fail(detailed_report_error_kind::transcript_mismatch,
												std::string{field},
												"descriptor-order"));
				const auto canonical = rows[index].canonical_form();
				if (!bounded_text(canonical, limits) || !sdk::validate_utf8_text(canonical))
					return sdk::unexpected(fail(detailed_report_error_kind::invalid_capture,
												std::string{field},
												"canonical-form"));
				destination.push_back(rows[index]);
			}
			return {};
		};
		if (auto valid =
				copy_and_validate_rows(materialized.base_claim_rows(),
									   "base_claim_rows",
									   std::span<const std::string_view>{base_descriptor_ids},
									   capture.base_claim_rows);
			!valid)
			return sdk::unexpected(std::move(valid.error()));
		if (materialized.source_span_claim_rows().size() > limits.max_side_channel_records)
			return sdk::unexpected(fail(
				detailed_report_error_kind::limit_exceeded, "source_span_claim_rows", "count"));
		capture.source_span_claim_rows.reserve(materialized.source_span_claim_rows().size());
		std::set<std::string, std::less<>> source_span_identities;
		for (const auto& row : materialized.source_span_claim_rows())
		{
			if (row.descriptor_id != "source.span.v1")
				return sdk::unexpected(fail(detailed_report_error_kind::transcript_mismatch,
											"source_span_claim_rows",
											"descriptor"));
			const auto canonical = row.canonical_form();
			if (!bounded_text(canonical, limits) || !sdk::validate_utf8_text(canonical) ||
				!source_span_identities.insert(canonical).second)
				return sdk::unexpected(fail(detailed_report_error_kind::invalid_capture,
											"source_span_claim_rows",
											"canonical-or-duplicate"));
			capture.source_span_claim_rows.push_back(row);
		}

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
			auto rows = row_set_digest(batch.rows(), limits, projection_bytes, output.rows);
			if (!rows)
				return sdk::unexpected(std::move(rows.error()));
			output.row_set_digest = std::move(*rows);
			capture.batches.push_back(std::move(output));
		}
		if (materialized.observation_rows().size() > limits.max_side_channel_records)
			return sdk::unexpected(
				fail(detailed_report_error_kind::limit_exceeded, "observation_rows", "count"));
		capture.observation_rows.reserve(materialized.observation_rows().size());
		std::size_t observation_index{};
		const auto provider_batches = materialized.provider_seal().batches();
		for (std::size_t batch_index = 3U; batch_index < provider_batches.size(); ++batch_index)
		{
			const auto& batch = provider_batches[batch_index];
			for (std::size_t row_index{}; row_index < batch.rows().size(); ++row_index)
			{
				if (observation_index >= materialized.observation_rows().size())
					return sdk::unexpected(fail(detailed_report_error_kind::transcript_mismatch,
												"observation_rows",
												"missing-binding"));
				const auto& observation = materialized.observation_rows()[observation_index];
				if (observation.batch_index != batch_index || observation.row_index != row_index ||
					observation.batch_index >= capture.batches.size() ||
					observation.row_index >= capture.batches[batch_index].rows.size())
					return sdk::unexpected(fail(detailed_report_error_kind::transcript_mismatch,
												"observation_rows",
												"sealed-index"));
				if (auto valid = validate_observation_row(
						observation, batch, capture.compile_unit_id, limits);
					!valid)
					return sdk::unexpected(std::move(valid.error()));
				detailed_observation_row_projection projected;
				projected.batch_index = batch_index;
				projected.row_index = row_index;
				projected.observation_row_digest =
					capture.batches[batch_index].rows[row_index].row_digest;
				projected.exact_equivalence = observation.observation.exact_equivalence;
				projected.limitation = observation.observation.limitation;
				projected.primary_span = observation.observation.primary_span;
				capture.observation_rows.push_back(std::move(projected));
				++observation_index;
			}
		}
		if (observation_index != materialized.observation_rows().size())
			return sdk::unexpected(fail(detailed_report_error_kind::transcript_mismatch,
										"observation_rows",
										"unexpected-binding"));
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

	sdk::result<detailed_task_report_capture> capture_detailed_task_report(
		const sdk::provider::detail::provider_process_validation_outcome& outcome,
		const sealed_materialization_result& materialized,
		const materialization_v2_1_task_metadata_receipt& metadata,
		const detailed_report_limits& limits)
	{
		auto captured = capture_detailed_task_report(outcome, materialized, limits);
		if (!captured)
			return captured;
		if (metadata.provider_task_id != captured->provider_task_id ||
			metadata.provider_execution_id != captured->provider_execution_id ||
			metadata.task_input_digest != captured->task_input_digest ||
			metadata.selected_catalog_compile_unit_id !=
				captured->selected_catalog_compile_unit_id ||
			metadata.final_relation_compile_unit_id != captured->compile_unit_id)
			return sdk::unexpected(fail(detailed_report_error_kind::transcript_mismatch,
										"task_context",
										"metadata-binding"));
		if (!bounded_text(metadata.condition_universe_id, limits) ||
			!bounded_text(metadata.condition_id, limits) ||
			!bounded_text(metadata.interpretation_domain, limits) ||
			!bounded_text(metadata.catalog_id, limits) ||
			!bounded_text(metadata.catalog_digest, limits) ||
			!bounded_text(metadata.variant_id, limits) ||
			!bounded_text(metadata.toolchain_context_id, limits) ||
			!bounded_text(metadata.toolchain_digest, limits) ||
			!bounded_text(metadata.source_snapshot_id, limits) ||
			!bounded_text(metadata.file_id, limits) ||
			!bounded_text(metadata.logical_path, limits) ||
			!bounded_text(metadata.source_content_digest, limits) ||
			!bounded_text(metadata.source_encoding, limits) ||
			!bounded_text(metadata.line_index_id, limits) ||
			!sdk::validate_strong_id(metadata.condition_universe_id) ||
			!sdk::validate_strong_id(metadata.condition_id) ||
			!sdk::validate_registered_symbol(metadata.interpretation_domain) ||
			!sdk::validate_strong_id(metadata.catalog_id) ||
			!sdk::validate_strong_id(metadata.variant_id) ||
			!sdk::validate_strong_id(metadata.toolchain_context_id) ||
			!sdk::validate_strong_id(metadata.source_snapshot_id) ||
			!sdk::validate_strong_id(metadata.file_id) ||
			!sdk::validate_strong_id(metadata.logical_path) ||
			!sdk::validate_strong_id(metadata.line_index_id))
			return sdk::unexpected(fail(
				detailed_report_error_kind::invalid_capture, "task_context", "semantic-fields"));
		captured->condition_universe_id = metadata.condition_universe_id;
		captured->condition_id = metadata.condition_id;
		captured->interpretation_domain = metadata.interpretation_domain;
		captured->project_id = metadata.project_id;
		captured->catalog_id = metadata.catalog_id;
		captured->catalog_digest = metadata.catalog_digest;
		captured->variant_id = metadata.variant_id;
		captured->toolchain_context_id = metadata.toolchain_context_id;
		captured->toolchain_digest = metadata.toolchain_digest;
		captured->source_snapshot_id = metadata.source_snapshot_id;
		captured->source_file_id = metadata.file_id;
		captured->source_logical_path = metadata.logical_path;
		captured->source_content_digest = metadata.source_content_digest;
		captured->source_size_bytes = metadata.source_size_bytes;
		captured->source_encoding = metadata.source_encoding;
		captured->source_line_index_id = metadata.line_index_id;
		captured->source_read_only = metadata.source_read_only;
		return captured;
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
												const detailed_report_limits& limits,
												std::size_t& accounted) noexcept
		{
			const auto limit = limits.max_projection_bytes;
			accounted = sizeof(capture);
			const auto account = [&](const std::string_view value)
			{
				return account_string(accounted, value, limit);
			};
			if (!account(capture.provider_task_id) || !account(capture.provider_execution_id) ||
				!account(capture.project_id) || !account(capture.catalog_id) ||
				!account(capture.catalog_digest) ||
				!account(capture.selected_catalog_compile_unit_id) ||
				!account(capture.compile_unit_id) || !account(capture.variant_id) ||
				!account(capture.toolchain_context_id) || !account(capture.toolchain_digest) ||
				!account(capture.source_snapshot_id) || !account(capture.source_file_id) ||
				!account(capture.source_logical_path) || !account(capture.source_content_digest) ||
				!account(capture.source_encoding) || !account(capture.source_line_index_id) ||
				!account(capture.task_input_digest) || !account(capture.condition_universe_id) ||
				!account(capture.condition_id) || !account(capture.interpretation_domain) ||
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
			if (capture.batches.size() > limits.max_batches_per_task ||
				capture.observation_rows.size() > limits.max_side_channel_records ||
				capture.coverage.size() > limits.max_side_channel_records ||
				capture.unresolved.size() > limits.max_side_channel_records ||
				capture.evidence.size() > limits.max_side_channel_records)
				return false;
			if (!add_accounted_count(accounted,
									 capture.batches.size(),
									 sizeof(detailed_provider_batch_projection),
									 limit))
				return false;
			for (const auto& batch : capture.batches)
			{
				if (batch.rows.size() != batch.row_count ||
					batch.rows.size() > limits.max_side_channel_records ||
					batch.ordered_chunk_digests.size() > limits.max_chunks_per_batch)
					return false;
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
				if (!add_accounted_count(accounted,
										 batch.rows.size(),
										 sizeof(detailed_provider_batch_projection::row_projection),
										 limit))
					return false;
				for (const auto& row : batch.rows)
					if (!account(row.row_canonical_form) || !account(row.row_digest))
						return false;
			}
			if (!add_accounted_count(accounted,
									 capture.observation_rows.size(),
									 sizeof(detailed_observation_row_projection),
									 limit))
				return false;
			for (const auto& row : capture.observation_rows)
			{
				if (!account(row.observation_row_digest))
					return false;
				if (row.limitation && !account(*row.limitation))
					return false;
				if (row.primary_span)
				{
					const auto& span = *row.primary_span;
					if (!account(span.span_id) || !account(span.snapshot) || !account(span.file) ||
						!account(span.role))
						return false;
				}
			}
			const auto account_rows = [&](const std::vector<sdk::detached_row>& rows)
			{
				if (rows.size() > limits.max_side_channel_records ||
					!add_accounted_count(accounted, rows.size(), sizeof(sdk::detached_row), limit))
					return false;
				for (const auto& row : rows)
				{
					if (!account(row.descriptor_id) || !account(row.canonical_form()) ||
						!add_accounted_count(
							accounted,
							row.cells.size(),
							sizeof(std::pair<const std::string, sdk::detached_cell>),
							limit))
						return false;
					for (const auto& [column, cell] : row.cells)
						if (!account(column) || !account(cell.canonical_form()) ||
							(cell.unknown_reason && !account(*cell.unknown_reason)))
							return false;
				}
				return true;
			};
			if (!account_rows(capture.base_claim_rows) ||
				!account_rows(capture.source_span_claim_rows))
				return false;
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

		constexpr std::array<char, 8U> report_spool_magic{'C', 'X', 'L', 'D', 'R', 'S', 'P', '1'};
		constexpr std::uint8_t report_spool_version = 1U;
		constexpr std::size_t report_spool_max_tasks = 4096U;
		constexpr std::size_t report_spool_max_batches = 6U;
		constexpr std::size_t report_spool_max_chunks = 65536U;
		constexpr std::size_t report_spool_max_side_records = 65536U;
		constexpr std::size_t report_spool_max_string_bytes = 16U * 1024U * 1024U;

		[[nodiscard]] bool valid_report_spool_limits(const detailed_report_limits& limits) noexcept
		{
			return limits.max_tasks != 0U && limits.max_tasks <= report_spool_max_tasks &&
				limits.max_batches_per_task != 0U &&
				limits.max_batches_per_task <= report_spool_max_batches &&
				limits.max_chunks_per_batch != 0U &&
				limits.max_chunks_per_batch <= report_spool_max_chunks &&
				limits.max_side_channel_records != 0U &&
				limits.max_side_channel_records <= report_spool_max_side_records &&
				limits.max_string_bytes != 0U &&
				limits.max_string_bytes <= report_spool_max_string_bytes &&
				limits.max_projection_bytes != 0U &&
				limits.max_projection_bytes <= detailed_report_limits::maximum_report_bytes;
		}

		[[nodiscard]] sdk::error report_spool_failure(const detailed_report_error_kind kind,
													  const std::string_view field,
													  const std::string_view detail)
		{
			return fail(kind, std::string{field}, std::string{detail});
		}

		class report_spool_writer
		{
		  public:
			report_spool_writer(materialization_replayable_spool& storage,
								const std::uint64_t remaining) noexcept
				: storage_{storage}, remaining_{remaining}
			{
			}

			[[nodiscard]] sdk::result<void> bytes(const std::span<const std::byte> value)
			{
				if (static_cast<std::uint64_t>(value.size()) > remaining_)
					return sdk::unexpected(report_spool_failure(
						detailed_report_error_kind::limit_exceeded, "task_spool", "bytes"));
				if (value.empty())
					return {};
				auto written = storage_.append(value);
				if (!written)
					return sdk::unexpected(report_spool_failure(
						detailed_report_error_kind::spool_io, "task_spool", "append"));
				remaining_ -= static_cast<std::uint64_t>(value.size());
				written_bytes_ += static_cast<std::uint64_t>(value.size());
				return {};
			}

			[[nodiscard]] sdk::result<void> byte(const std::uint8_t value)
			{
				return bytes(std::array{static_cast<std::byte>(value)});
			}

			[[nodiscard]] sdk::result<void> boolean(const bool value)
			{
				return byte(value ? 1U : 0U);
			}

			[[nodiscard]] sdk::result<void> u64(const std::uint64_t value)
			{
				std::array<std::byte, 8U> encoded{};
				for (std::size_t index{}; index < encoded.size(); ++index)
					encoded[index] = static_cast<std::byte>(
						(value >> static_cast<unsigned>(56U - index * 8U)) & 0xffU);
				return bytes(encoded);
			}

			[[nodiscard]] sdk::result<void> string(const std::string_view value)
			{
				if (auto length = u64(static_cast<std::uint64_t>(value.size())); !length)
					return length;
				return bytes(std::as_bytes(std::span<const char>{value.data(), value.size()}));
			}

			[[nodiscard]] sdk::result<void> optional_string(const std::optional<std::string>& value)
			{
				if (auto present = boolean(value.has_value()); !present)
					return present;
				if (!value)
					return {};
				return string(*value);
			}

			[[nodiscard]] sdk::result<void> strings(const std::vector<std::string>& values)
			{
				if (auto count = u64(static_cast<std::uint64_t>(values.size())); !count)
					return count;
				for (const auto& value : values)
					if (auto written = string(value); !written)
						return written;
				return {};
			}

			[[nodiscard]] sdk::result<void>
			optional_span(const std::optional<observation_v2_primary_span>& value)
			{
				if (auto present = boolean(value.has_value()); !present)
					return present;
				if (!value)
					return {};
				const auto& span = *value;
				for (const auto field : {std::string_view{span.span_id},
										 std::string_view{span.snapshot},
										 std::string_view{span.file},
										 std::string_view{span.role}})
					if (auto written = string(field); !written)
						return written;
				if (auto begin = u64(span.begin); !begin)
					return begin;
				if (auto end = u64(span.end); !end)
					return end;
				return boolean(span.read_only);
			}

			[[nodiscard]] sdk::result<void> cell(const sdk::detached_cell& value)
			{
				if (!sdk::is_valid(value.type.scalar) || !sdk::is_valid(value.state) ||
					!value.validate())
					return sdk::unexpected(report_spool_failure(
						detailed_report_error_kind::invalid_capture, "task_spool.cell", "invalid"));
				if (auto scalar = byte(static_cast<std::uint8_t>(value.type.scalar)); !scalar)
					return scalar;
				if (auto parameter = string(value.type.parameter); !parameter)
					return parameter;
				if (auto optional = boolean(value.type.optional); !optional)
					return optional;
				if (auto state = byte(static_cast<std::uint8_t>(value.state)); !state)
					return state;
				if (auto present = boolean(value.value.has_value()); !present)
					return present;
				if (value.value)
				{
					if (auto kind = byte(static_cast<std::uint8_t>(value.value->index())); !kind)
						return kind;
					const auto& scalar = *value.value;
					if (const auto* boolean_value = std::get_if<bool>(&scalar))
					{
						if (auto written = boolean(*boolean_value); !written)
							return written;
					}
					else if (const auto* signed_value = std::get_if<std::int64_t>(&scalar))
					{
						if (auto written = u64(std::bit_cast<std::uint64_t>(*signed_value));
							!written)
							return written;
					}
					else if (const auto* unsigned_value = std::get_if<std::uint64_t>(&scalar))
					{
						if (auto written = u64(*unsigned_value); !written)
							return written;
					}
					else if (const auto* string_value = std::get_if<std::string>(&scalar))
					{
						if (auto written = string(*string_value); !written)
							return written;
					}
					else if (const auto* bytes_value = std::get_if<std::vector<std::byte>>(&scalar))
					{
						if (auto length = u64(static_cast<std::uint64_t>(bytes_value->size()));
							!length)
							return length;
						if (auto written = bytes(*bytes_value); !written)
							return written;
					}
				}
				if (auto reason = optional_string(value.unknown_reason); !reason)
					return reason;
				return {};
			}

			[[nodiscard]] sdk::result<void> row(const sdk::detached_row& value)
			{
				if (auto descriptor = string(value.descriptor_id); !descriptor)
					return descriptor;
				if (auto count = u64(static_cast<std::uint64_t>(value.cells.size())); !count)
					return count;
				for (const auto& [column, cell_value] : value.cells)
				{
					if (auto name = string(column); !name)
						return name;
					if (auto cell_result = cell(cell_value); !cell_result)
						return cell_result;
				}
				return {};
			}

			[[nodiscard]] std::uint64_t written_bytes() const noexcept
			{
				return written_bytes_;
			}

		  private:
			materialization_replayable_spool& storage_;
			std::uint64_t remaining_{};
			std::uint64_t written_bytes_{};
		};

		[[nodiscard]] sdk::result<void>
		write_report_capture(report_spool_writer& writer,
							 const detailed_task_report_capture& capture)
		{
			std::array<std::byte, report_spool_magic.size()> magic{};
			for (std::size_t index{}; index < magic.size(); ++index)
				magic[index] = static_cast<std::byte>(report_spool_magic[index]);
			if (auto header = writer.bytes(magic); !header)
				return header;
			if (auto version = writer.byte(report_spool_version); !version)
				return version;
			const auto write_fixed_strings =
				[&](const std::initializer_list<std::string_view> values) -> sdk::result<void>
			{
				for (const auto value : values)
					if (auto written = writer.string(value); !written)
						return written;
				return {};
			};
			if (auto fields = write_fixed_strings({capture.provider_task_id,
												   capture.provider_execution_id,
												   capture.project_id,
												   capture.catalog_id,
												   capture.catalog_digest,
												   capture.selected_catalog_compile_unit_id,
												   capture.compile_unit_id,
												   capture.variant_id,
												   capture.toolchain_context_id,
												   capture.toolchain_digest,
												   capture.source_snapshot_id,
												   capture.source_file_id,
												   capture.source_logical_path,
												   capture.source_content_digest});
				!fields)
				return fields;
			if (auto value = writer.u64(capture.source_size_bytes); !value)
				return value;
			if (auto fields =
					write_fixed_strings({capture.source_encoding, capture.source_line_index_id});
				!fields)
				return fields;
			if (auto value = writer.boolean(capture.source_read_only); !value)
				return value;
			if (auto fields = write_fixed_strings({capture.task_input_digest,
												   capture.condition_universe_id,
												   capture.condition_id,
												   capture.interpretation_domain});
				!fields)
				return fields;
			if (auto value = writer.u64(capture.input_protocol_major); !value)
				return value;
			if (auto value = writer.u64(capture.input_protocol_minor); !value)
				return value;
			if (auto value = writer.u64(capture.logical_input_bytes); !value)
				return value;
			if (auto value = writer.u64(capture.canonical_chunk_bytes); !value)
				return value;
			if (auto value = writer.u64(capture.input_chunk_count); !value)
				return value;
			if (auto values = writer.strings(capture.ordered_chunk_digests); !values)
				return values;
			if (auto fields = write_fixed_strings({capture.ordered_chunk_payload_digest_set_digest,
												   capture.raw_frame_stream_digest,
												   capture.frame_transcript_digest,
												   capture.sealed_transcript_digest});
				!fields)
				return fields;
			if (auto value = writer.u64(capture.raw_frame_stream_bytes); !value)
				return value;
			if (auto value = writer.u64(capture.frame_count); !value)
				return value;

			if (auto count = writer.u64(static_cast<std::uint64_t>(capture.coverage.size()));
				!count)
				return count;
			for (const auto& value : capture.coverage)
				if (auto fields =
						write_fixed_strings({value.kind, value.id, value.state, value.reason});
					!fields)
					return fields;
			if (auto count = writer.u64(static_cast<std::uint64_t>(capture.unresolved.size()));
				!count)
				return count;
			for (const auto& value : capture.unresolved)
				if (auto fields = write_fixed_strings({value.code, value.subject, value.detail});
					!fields)
					return fields;
			if (auto count = writer.u64(static_cast<std::uint64_t>(capture.evidence.size()));
				!count)
				return count;
			for (const auto& value : capture.evidence)
				if (auto fields = write_fixed_strings(
						{value.kind, value.subject, value.producer, value.summary});
					!fields)
					return fields;

			if (auto count = writer.u64(static_cast<std::uint64_t>(capture.batches.size())); !count)
				return count;
			for (const auto& batch : capture.batches)
			{
				if (auto fields = write_fixed_strings({batch.task_id,
													   batch.descriptor_id,
													   batch.descriptor_digest,
													   batch.dependency_group_id,
													   batch.atomic_output_group_id,
													   batch.batch_id,
													   batch.batch_digest});
					!fields)
					return fields;
				if (auto values = writer.strings(batch.ordered_chunk_digests); !values)
					return values;
				if (auto value = writer.u64(batch.row_count); !value)
					return value;
				if (auto value = writer.string(batch.row_set_digest); !value)
					return value;
				if (auto count = writer.u64(static_cast<std::uint64_t>(batch.rows.size())); !count)
					return count;
				for (const auto& row : batch.rows)
				{
					if (auto value = writer.u64(static_cast<std::uint64_t>(row.row_index)); !value)
						return value;
					if (auto value = writer.string(row.row_canonical_form); !value)
						return value;
					if (auto value = writer.string(row.row_digest); !value)
						return value;
				}
			}

			if (auto count =
					writer.u64(static_cast<std::uint64_t>(capture.observation_rows.size()));
				!count)
				return count;
			for (const auto& row : capture.observation_rows)
			{
				if (auto value = writer.u64(static_cast<std::uint64_t>(row.batch_index)); !value)
					return value;
				if (auto value = writer.u64(static_cast<std::uint64_t>(row.row_index)); !value)
					return value;
				if (auto value = writer.string(row.observation_row_digest); !value)
					return value;
				if (auto value = writer.boolean(row.exact_equivalence); !value)
					return value;
				if (auto value = writer.optional_string(row.limitation); !value)
					return value;
				if (auto value = writer.optional_span(row.primary_span); !value)
					return value;
			}

			const auto write_rows =
				[&](const std::vector<sdk::detached_row>& rows) -> sdk::result<void>
			{
				if (auto count = writer.u64(static_cast<std::uint64_t>(rows.size())); !count)
					return count;
				for (const auto& row : rows)
					if (auto written = writer.row(row); !written)
						return written;
				return {};
			};
			if (auto rows = write_rows(capture.base_claim_rows); !rows)
				return rows;
			return write_rows(capture.source_span_claim_rows);
		}

		class report_spool_reader
		{
		  public:
			report_spool_reader(materialization_replayable_spool& storage,
								const std::uint64_t begin,
								const std::uint64_t end,
								const detailed_report_limits& limits) noexcept
				: storage_{storage}, offset_{begin}, end_{end}, limits_{limits}
			{
			}

			[[nodiscard]] sdk::result<void> exact(const std::span<std::byte> destination)
			{
				if (static_cast<std::uint64_t>(destination.size()) > end_ - offset_)
					return sdk::unexpected(report_spool_failure(
						detailed_report_error_kind::spool_corrupt, "task_spool", "truncated"));
				std::size_t consumed{};
				while (consumed < destination.size())
				{
					auto read = storage_.read_at(offset_, destination.subspan(consumed));
					if (!read)
						return sdk::unexpected(report_spool_failure(
							detailed_report_error_kind::spool_io, "task_spool", "read"));
					if (*read == 0U || *read > destination.size() - consumed ||
						static_cast<std::uint64_t>(*read) > end_ - offset_)
						return sdk::unexpected(report_spool_failure(
							detailed_report_error_kind::spool_corrupt, "task_spool", "short-read"));
					consumed += *read;
					offset_ += static_cast<std::uint64_t>(*read);
				}
				return {};
			}

			[[nodiscard]] sdk::result<std::uint8_t> byte()
			{
				std::array<std::byte, 1U> value{};
				if (auto read = exact(value); !read)
					return sdk::unexpected(std::move(read.error()));
				return std::to_integer<std::uint8_t>(value[0]);
			}

			[[nodiscard]] sdk::result<bool> boolean()
			{
				auto value = byte();
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				if (*value > 1U)
					return sdk::unexpected(
						report_spool_failure(detailed_report_error_kind::spool_corrupt,
											 "task_spool.bool",
											 "closed-enum"));
				return *value == 1U;
			}

			[[nodiscard]] sdk::result<std::uint64_t> u64()
			{
				std::array<std::byte, 8U> bytes{};
				if (auto read = exact(bytes); !read)
					return sdk::unexpected(std::move(read.error()));
				std::uint64_t value{};
				for (const auto byte : bytes)
					value = (value << 8U) |
						static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(byte));
				return value;
			}

			[[nodiscard]] sdk::result<std::size_t> count(const std::size_t maximum,
														 const std::string_view field)
			{
				auto value = u64();
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				if (*value > static_cast<std::uint64_t>(maximum) ||
					*value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
					return sdk::unexpected(report_spool_failure(
						detailed_report_error_kind::spool_corrupt, field, "count"));
				return static_cast<std::size_t>(*value);
			}

			[[nodiscard]] sdk::result<std::string> string(const std::string_view field)
			{
				auto length = u64();
				if (!length)
					return sdk::unexpected(std::move(length.error()));
				if (*length > static_cast<std::uint64_t>(limits_.max_string_bytes) ||
					*length > end_ - offset_ ||
					*length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
					return sdk::unexpected(report_spool_failure(
						detailed_report_error_kind::spool_corrupt, field, "string-length"));
				std::string value(static_cast<std::size_t>(*length), '\0');
				if (auto read =
						exact(std::as_writable_bytes(std::span<char>{value.data(), value.size()}));
					!read)
					return sdk::unexpected(std::move(read.error()));
				return value;
			}

			[[nodiscard]] sdk::result<std::vector<std::byte>> bytes(const std::string_view field)
			{
				auto length = u64();
				if (!length)
					return sdk::unexpected(std::move(length.error()));
				if (*length > static_cast<std::uint64_t>(limits_.max_string_bytes) ||
					*length > end_ - offset_ ||
					*length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
					return sdk::unexpected(report_spool_failure(
						detailed_report_error_kind::spool_corrupt, field, "bytes-length"));
				std::vector<std::byte> value(static_cast<std::size_t>(*length));
				if (auto read = exact(value); !read)
					return sdk::unexpected(std::move(read.error()));
				return value;
			}

			[[nodiscard]] sdk::result<std::optional<std::string>>
			optional_string(const std::string_view field)
			{
				auto present = boolean();
				if (!present)
					return sdk::unexpected(std::move(present.error()));
				if (!*present)
					return std::optional<std::string>{};
				auto value = string(field);
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				return std::optional<std::string>{std::move(*value)};
			}

			[[nodiscard]] sdk::result<std::optional<observation_v2_primary_span>> optional_span()
			{
				auto present = boolean();
				if (!present)
					return sdk::unexpected(std::move(present.error()));
				if (!*present)
					return std::optional<observation_v2_primary_span>{};
				observation_v2_primary_span value;
				auto span_id = string("task_spool.span_id");
				auto snapshot = string("task_spool.span.snapshot");
				auto file = string("task_spool.span.file");
				auto role = string("task_spool.span.role");
				if (!span_id || !snapshot || !file || !role)
					return sdk::unexpected(report_spool_failure(
						detailed_report_error_kind::spool_corrupt, "task_spool.span", "string"));
				value.span_id = std::move(*span_id);
				value.snapshot = std::move(*snapshot);
				value.file = std::move(*file);
				value.role = std::move(*role);
				auto begin = u64();
				auto end = u64();
				auto read_only = boolean();
				if (!begin || !end || !read_only)
					return sdk::unexpected(report_spool_failure(
						detailed_report_error_kind::spool_corrupt, "task_spool.span", "value"));
				value.begin = *begin;
				value.end = *end;
				value.read_only = *read_only;
				return std::optional<observation_v2_primary_span>{std::move(value)};
			}

			[[nodiscard]] sdk::result<sdk::detached_cell> cell()
			{
				auto scalar = byte();
				auto parameter = string("task_spool.cell.parameter");
				auto optional = boolean();
				auto state = byte();
				auto has_value = boolean();
				if (!scalar || !parameter || !optional || !state || !has_value ||
					!sdk::is_valid(static_cast<sdk::scalar_kind>(*scalar)) ||
					*state > static_cast<std::uint8_t>(sdk::cell_state::unknown))
					return sdk::unexpected(report_spool_failure(
						detailed_report_error_kind::spool_corrupt, "task_spool.cell", "header"));
				sdk::detached_cell value;
				value.type = {
					static_cast<sdk::scalar_kind>(*scalar), std::move(*parameter), *optional};
				value.state = static_cast<sdk::cell_state>(*state);
				if (*has_value)
				{
					auto kind = byte();
					if (!kind || *kind > 4U)
						return sdk::unexpected(
							report_spool_failure(detailed_report_error_kind::spool_corrupt,
												 "task_spool.cell.value",
												 "kind"));
					switch (*kind)
					{
						case 0U:
						{
							auto item = boolean();
							if (!item)
								return sdk::unexpected(std::move(item.error()));
							value.value = sdk::scalar_value{*item};
							break;
						}
						case 1U:
						{
							auto item = u64();
							if (!item)
								return sdk::unexpected(std::move(item.error()));
							value.value = sdk::scalar_value{std::bit_cast<std::int64_t>(*item)};
							break;
						}
						case 2U:
						{
							auto item = u64();
							if (!item)
								return sdk::unexpected(std::move(item.error()));
							value.value = sdk::scalar_value{*item};
							break;
						}
						case 3U:
						{
							auto item = string("task_spool.cell.string");
							if (!item)
								return sdk::unexpected(std::move(item.error()));
							value.value = sdk::scalar_value{std::move(*item)};
							break;
						}
						case 4U:
						{
							auto item = bytes("task_spool.cell.bytes");
							if (!item)
								return sdk::unexpected(std::move(item.error()));
							value.value = sdk::scalar_value{std::move(*item)};
							break;
						}
						default:
							return sdk::unexpected(
								report_spool_failure(detailed_report_error_kind::spool_corrupt,
													 "task_spool.cell.value",
													 "kind"));
					}
				}
				auto reason = optional_string("task_spool.cell.unknown_reason");
				if (!reason)
					return sdk::unexpected(std::move(reason.error()));
				value.unknown_reason = std::move(*reason);
				if (!value.validate())
					return sdk::unexpected(report_spool_failure(
						detailed_report_error_kind::spool_corrupt, "task_spool.cell", "invalid"));
				return value;
			}

			[[nodiscard]] sdk::result<sdk::detached_row> row()
			{
				auto descriptor = string("task_spool.row.descriptor_id");
				auto count = this->count(limits_.max_side_channel_records, "task_spool.row.cells");
				if (!descriptor || !count)
					return sdk::unexpected(report_spool_failure(
						detailed_report_error_kind::spool_corrupt, "task_spool.row", "header"));
				sdk::detached_row value;
				value.descriptor_id = std::move(*descriptor);
				std::string previous;
				for (std::size_t index{}; index < *count; ++index)
				{
					auto column = string("task_spool.row.column");
					if (!column || (!previous.empty() && *column <= previous))
						return sdk::unexpected(
							report_spool_failure(detailed_report_error_kind::spool_corrupt,
												 "task_spool.row.column",
												 "order"));
					auto cell_value = cell();
					if (!cell_value)
						return sdk::unexpected(std::move(cell_value.error()));
					previous = *column;
					if (!value.cells.emplace(std::move(*column), std::move(*cell_value)).second)
						return sdk::unexpected(
							report_spool_failure(detailed_report_error_kind::spool_corrupt,
												 "task_spool.row.column",
												 "duplicate"));
				}
				return value;
			}

			[[nodiscard]] std::uint64_t offset() const noexcept
			{
				return offset_;
			}

		  private:
			materialization_replayable_spool& storage_;
			std::uint64_t offset_{};
			std::uint64_t end_{};
			const detailed_report_limits& limits_;
		};

		[[nodiscard]] sdk::result<detailed_task_report_capture>
		read_report_capture(report_spool_reader& reader, const detailed_report_limits& limits)
		{
			std::array<std::byte, report_spool_magic.size()> magic{};
			if (auto read = reader.exact(magic); !read)
				return sdk::unexpected(std::move(read.error()));
			for (std::size_t index{}; index < magic.size(); ++index)
				if (std::to_integer<char>(magic[index]) != report_spool_magic[index])
					return sdk::unexpected(report_spool_failure(
						detailed_report_error_kind::spool_corrupt, "task_spool.magic", "mismatch"));
			auto version = reader.byte();
			if (!version || *version != report_spool_version)
				return sdk::unexpected(report_spool_failure(
					detailed_report_error_kind::spool_corrupt, "task_spool.version", "mismatch"));

			detailed_task_report_capture capture;
			const auto read_fixed_strings =
				[&](const std::initializer_list<std::string*> fields) -> sdk::result<void>
			{
				for (auto* field : fields)
				{
					auto value = reader.string("task_spool.string");
					if (!value)
						return sdk::unexpected(std::move(value.error()));
					*field = std::move(*value);
				}
				return {};
			};
			if (auto fields = read_fixed_strings({&capture.provider_task_id,
												  &capture.provider_execution_id,
												  &capture.project_id,
												  &capture.catalog_id,
												  &capture.catalog_digest,
												  &capture.selected_catalog_compile_unit_id,
												  &capture.compile_unit_id,
												  &capture.variant_id,
												  &capture.toolchain_context_id,
												  &capture.toolchain_digest,
												  &capture.source_snapshot_id,
												  &capture.source_file_id,
												  &capture.source_logical_path,
												  &capture.source_content_digest});
				!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto source_size = reader.u64();
			if (!source_size)
				return sdk::unexpected(std::move(source_size.error()));
			capture.source_size_bytes = *source_size;
			if (auto fields =
					read_fixed_strings({&capture.source_encoding, &capture.source_line_index_id});
				!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto source_read_only = reader.boolean();
			if (!source_read_only)
				return sdk::unexpected(std::move(source_read_only.error()));
			capture.source_read_only = *source_read_only;
			if (auto fields = read_fixed_strings({&capture.task_input_digest,
												  &capture.condition_universe_id,
												  &capture.condition_id,
												  &capture.interpretation_domain});
				!fields)
				return sdk::unexpected(std::move(fields.error()));
			const auto read_u16 = [&](std::uint16_t& output) -> sdk::result<void>
			{
				auto value = reader.u64();
				if (!value || *value > std::numeric_limits<std::uint16_t>::max())
					return sdk::unexpected(report_spool_failure(
						detailed_report_error_kind::spool_corrupt, "task_spool.protocol", "u16"));
				output = static_cast<std::uint16_t>(*value);
				return {};
			};
			if (auto value = read_u16(capture.input_protocol_major); !value)
				return sdk::unexpected(std::move(value.error()));
			if (auto value = read_u16(capture.input_protocol_minor); !value)
				return sdk::unexpected(std::move(value.error()));
			if (auto value = reader.u64(); !value)
				return sdk::unexpected(std::move(value.error()));
			else
				capture.logical_input_bytes = *value;
			if (auto value = reader.u64(); !value)
				return sdk::unexpected(std::move(value.error()));
			else
				capture.canonical_chunk_bytes = *value;
			if (auto value = reader.u64(); !value)
				return sdk::unexpected(std::move(value.error()));
			else
				capture.input_chunk_count = *value;
			auto chunk_count = reader.count(limits.max_chunks_per_batch, "task_spool.chunks");
			if (!chunk_count)
				return sdk::unexpected(std::move(chunk_count.error()));
			capture.ordered_chunk_digests.reserve(*chunk_count);
			for (std::size_t index{}; index < *chunk_count; ++index)
			{
				auto value = reader.string("task_spool.chunk");
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				capture.ordered_chunk_digests.push_back(std::move(*value));
			}
			if (auto fields = read_fixed_strings({&capture.ordered_chunk_payload_digest_set_digest,
												  &capture.raw_frame_stream_digest,
												  &capture.frame_transcript_digest,
												  &capture.sealed_transcript_digest});
				!fields)
				return sdk::unexpected(std::move(fields.error()));
			if (auto value = reader.u64(); !value)
				return sdk::unexpected(std::move(value.error()));
			else
				capture.raw_frame_stream_bytes = *value;
			if (auto value = reader.u64(); !value)
				return sdk::unexpected(std::move(value.error()));
			else
				capture.frame_count = *value;

			auto coverage_count =
				reader.count(limits.max_side_channel_records, "task_spool.coverage");
			if (!coverage_count)
				return sdk::unexpected(std::move(coverage_count.error()));
			capture.coverage.reserve(*coverage_count);
			for (std::size_t index{}; index < *coverage_count; ++index)
			{
				detailed_coverage_projection value;
				if (auto fields =
						read_fixed_strings({&value.kind, &value.id, &value.state, &value.reason});
					!fields)
					return sdk::unexpected(std::move(fields.error()));
				capture.coverage.push_back(std::move(value));
			}
			auto unresolved_count =
				reader.count(limits.max_side_channel_records, "task_spool.unresolved");
			if (!unresolved_count)
				return sdk::unexpected(std::move(unresolved_count.error()));
			capture.unresolved.reserve(*unresolved_count);
			for (std::size_t index{}; index < *unresolved_count; ++index)
			{
				detailed_unresolved_projection value;
				if (auto fields = read_fixed_strings({&value.code, &value.subject, &value.detail});
					!fields)
					return sdk::unexpected(std::move(fields.error()));
				capture.unresolved.push_back(std::move(value));
			}
			auto evidence_count =
				reader.count(limits.max_side_channel_records, "task_spool.evidence");
			if (!evidence_count)
				return sdk::unexpected(std::move(evidence_count.error()));
			capture.evidence.reserve(*evidence_count);
			for (std::size_t index{}; index < *evidence_count; ++index)
			{
				detailed_evidence_projection value;
				if (auto fields = read_fixed_strings(
						{&value.kind, &value.subject, &value.producer, &value.summary});
					!fields)
					return sdk::unexpected(std::move(fields.error()));
				capture.evidence.push_back(std::move(value));
			}

			auto batch_count = reader.count(limits.max_batches_per_task, "task_spool.batches");
			if (!batch_count)
				return sdk::unexpected(std::move(batch_count.error()));
			capture.batches.reserve(*batch_count);
			for (std::size_t index{}; index < *batch_count; ++index)
			{
				detailed_provider_batch_projection batch;
				if (auto fields = read_fixed_strings({&batch.task_id,
													  &batch.descriptor_id,
													  &batch.descriptor_digest,
													  &batch.dependency_group_id,
													  &batch.atomic_output_group_id,
													  &batch.batch_id,
													  &batch.batch_digest});
					!fields)
					return sdk::unexpected(std::move(fields.error()));
				auto chunks = reader.count(limits.max_chunks_per_batch, "task_spool.batch.chunks");
				if (!chunks)
					return sdk::unexpected(std::move(chunks.error()));
				batch.ordered_chunk_digests.reserve(*chunks);
				for (std::size_t chunk{}; chunk < *chunks; ++chunk)
				{
					auto value = reader.string("task_spool.batch.chunk");
					if (!value)
						return sdk::unexpected(std::move(value.error()));
					batch.ordered_chunk_digests.push_back(std::move(*value));
				}
				auto row_count = reader.u64();
				auto row_set_digest = reader.string("task_spool.batch.row_set_digest");
				if (!row_count || !row_set_digest ||
					*row_count > static_cast<std::uint64_t>(limits.max_side_channel_records))
					return sdk::unexpected(
						report_spool_failure(detailed_report_error_kind::spool_corrupt,
											 "task_spool.batch.rows",
											 "count"));
				batch.row_count = *row_count;
				batch.row_set_digest = std::move(*row_set_digest);
				auto rows = reader.count(limits.max_side_channel_records, "task_spool.batch.rows");
				if (!rows || *rows != batch.row_count)
					return sdk::unexpected(
						report_spool_failure(detailed_report_error_kind::spool_corrupt,
											 "task_spool.batch.rows",
											 "count"));
				batch.rows.reserve(*rows);
				for (std::size_t row_index{}; row_index < *rows; ++row_index)
				{
					detailed_provider_batch_projection::row_projection row;
					auto index_value = reader.u64();
					auto canonical = reader.string("task_spool.batch.row.canonical");
					auto digest = reader.string("task_spool.batch.row.digest");
					if (!index_value || !canonical || !digest ||
						*index_value >
							static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
						return sdk::unexpected(
							report_spool_failure(detailed_report_error_kind::spool_corrupt,
												 "task_spool.batch.row",
												 "value"));
					row.row_index = static_cast<std::size_t>(*index_value);
					row.row_canonical_form = std::move(*canonical);
					row.row_digest = std::move(*digest);
					batch.rows.push_back(std::move(row));
				}
				capture.batches.push_back(std::move(batch));
			}

			auto observation_count =
				reader.count(limits.max_side_channel_records, "task_spool.observation_rows");
			if (!observation_count)
				return sdk::unexpected(std::move(observation_count.error()));
			capture.observation_rows.reserve(*observation_count);
			for (std::size_t index{}; index < *observation_count; ++index)
			{
				detailed_observation_row_projection row;
				auto batch_index = reader.u64();
				auto row_index = reader.u64();
				auto digest = reader.string("task_spool.observation.digest");
				auto exact = reader.boolean();
				auto limitation = reader.optional_string("task_spool.observation.limitation");
				auto span = reader.optional_span();
				if (!batch_index || !row_index || !digest || !exact || !limitation || !span ||
					*batch_index >
						static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
					*row_index >
						static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
					return sdk::unexpected(
						report_spool_failure(detailed_report_error_kind::spool_corrupt,
											 "task_spool.observation",
											 "value"));
				row.batch_index = static_cast<std::size_t>(*batch_index);
				row.row_index = static_cast<std::size_t>(*row_index);
				row.observation_row_digest = std::move(*digest);
				row.exact_equivalence = *exact;
				row.limitation = std::move(*limitation);
				row.primary_span = std::move(*span);
				capture.observation_rows.push_back(std::move(row));
			}

			const auto read_rows = [&](std::vector<sdk::detached_row>& rows) -> sdk::result<void>
			{
				auto count = reader.count(limits.max_side_channel_records, "task_spool.rows");
				if (!count)
					return sdk::unexpected(std::move(count.error()));
				rows.reserve(*count);
				for (std::size_t index{}; index < *count; ++index)
				{
					auto row = reader.row();
					if (!row)
						return sdk::unexpected(std::move(row.error()));
					rows.push_back(std::move(*row));
				}
				return {};
			};
			if (auto rows = read_rows(capture.base_claim_rows); !rows)
				return sdk::unexpected(std::move(rows.error()));
			if (auto rows = read_rows(capture.source_span_claim_rows); !rows)
				return sdk::unexpected(std::move(rows.error()));
			std::size_t accounted{};
			if (!account_task_capture(capture, limits, accounted))
				return sdk::unexpected(report_spool_failure(
					detailed_report_error_kind::spool_corrupt, "task_spool.capture", "bounds"));
			return capture;
		}
	} // namespace

	detailed_task_report_replayable_spool::detailed_task_report_replayable_spool(
		detailed_report_limits limits, std::unique_ptr<materialization_replayable_spool> storage)
		: limits_{limits}, storage_{std::move(storage)}
	{
	}

	detailed_task_report_replayable_spool::detailed_task_report_replayable_spool(
		detailed_task_report_replayable_spool&&) noexcept = default;

	detailed_task_report_replayable_spool& detailed_task_report_replayable_spool::operator=(
		detailed_task_report_replayable_spool&&) noexcept = default;

	detailed_task_report_replayable_spool::~detailed_task_report_replayable_spool() = default;

	sdk::result<detailed_task_report_replayable_spool>
	detailed_task_report_replayable_spool::create(detailed_report_limits limits)
	{
		if (!valid_report_spool_limits(limits))
			return sdk::unexpected(
				fail(detailed_report_error_kind::invalid_capture, "task_spool.limits", "bounded"));
		try
		{
			auto storage = make_materialization_private_spool();
			if (!storage)
				return sdk::unexpected(
					fail(detailed_report_error_kind::spool_io, "task_spool", "create"));
			detailed_task_report_replayable_spool output{limits, std::move(*storage)};
			output.record_offsets_.reserve(limits.max_tasks);
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				fail(detailed_report_error_kind::spool_io, "task_spool", "allocation"));
		}
	}

	sdk::result<void>
	detailed_task_report_replayable_spool::append(detailed_task_report_capture capture)
	{
		if (!storage_ || poisoned_ || sealed_)
			return sdk::unexpected(
				fail(detailed_report_error_kind::spool_io, "task_spool", "lifecycle"));
		if (record_offsets_.size() >= limits_.max_tasks)
			return sdk::unexpected(
				fail(detailed_report_error_kind::limit_exceeded, "task_spool", "count"));
		try
		{
			std::size_t accounted{};
			if (!account_task_capture(capture, limits_, accounted))
				return sdk::unexpected(fail(
					detailed_report_error_kind::limit_exceeded, "task_spool", "capture-bytes"));
			const auto validate_rows = [](const std::vector<sdk::detached_row>& rows)
			{
				for (const auto& row : rows)
					for (const auto& [column, cell] : row.cells)
						if (!sdk::is_valid(cell.state) || !sdk::is_valid(cell.type.scalar) ||
							!cell.validate())
							return false;
				return true;
			};
			if (!validate_rows(capture.base_claim_rows) ||
				!validate_rows(capture.source_span_claim_rows))
				return sdk::unexpected(
					fail(detailed_report_error_kind::invalid_capture, "task_spool.rows", "cell"));

			const auto remaining = limits_.max_projection_bytes - spooled_bytes_;
			report_spool_writer writer{*storage_, static_cast<std::uint64_t>(remaining)};
			auto encoded = write_report_capture(writer, capture);
			if (!encoded)
			{
				poisoned_ = true;
				return encoded;
			}
			if (writer.written_bytes() == 0U ||
				writer.written_bytes() > static_cast<std::uint64_t>(remaining))
			{
				poisoned_ = true;
				return sdk::unexpected(
					fail(detailed_report_error_kind::spool_corrupt, "task_spool", "size"));
			}
			record_offsets_.push_back(spooled_bytes_);
			spooled_bytes_ += writer.written_bytes();
			return {};
		}
		catch (const std::bad_alloc&)
		{
			poisoned_ = true;
			return sdk::unexpected(
				fail(detailed_report_error_kind::spool_io, "task_spool", "allocation"));
		}
		catch (...)
		{
			poisoned_ = true;
			return sdk::unexpected(
				fail(detailed_report_error_kind::spool_io, "task_spool", "exception"));
		}
	}

	sdk::result<void> detailed_task_report_replayable_spool::seal()
	{
		if (!storage_ || poisoned_ || sealed_)
			return sdk::unexpected(
				fail(detailed_report_error_kind::spool_io, "task_spool", "lifecycle"));
		auto result = storage_->seal();
		if (!result)
		{
			poisoned_ = true;
			return sdk::unexpected(
				fail(detailed_report_error_kind::spool_io, "task_spool", "seal"));
		}
		sealed_ = true;
		return {};
	}

	sdk::result<void> detailed_task_report_replayable_spool::replay(const consumer& consume) const
	{
		if (!storage_ || poisoned_ || !sealed_ || !consume)
			return sdk::unexpected(
				fail(detailed_report_error_kind::spool_io, "task_spool", "replay-lifecycle"));
		try
		{
			for (std::size_t index{}; index < record_offsets_.size(); ++index)
			{
				const auto begin = record_offsets_[index];
				const auto end = index + 1U < record_offsets_.size() ? record_offsets_[index + 1U]
																	 : spooled_bytes_;
				if (begin >= end || end > spooled_bytes_)
					return sdk::unexpected(fail(
						detailed_report_error_kind::spool_corrupt, "task_spool.offset", "order"));
				report_spool_reader reader{*storage_, begin, end, limits_};
				auto capture = read_report_capture(reader, limits_);
				if (!capture)
					return sdk::unexpected(std::move(capture.error()));
				if (reader.offset() != end)
					return sdk::unexpected(fail(detailed_report_error_kind::spool_corrupt,
												"task_spool.record",
												"trailing-bytes"));
				if (auto accepted = consume(std::move(*capture)); !accepted)
					return accepted;
			}
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				fail(detailed_report_error_kind::spool_io, "task_spool", "allocation"));
		}
		catch (...)
		{
			return sdk::unexpected(
				fail(detailed_report_error_kind::spool_io, "task_spool", "exception"));
		}
	}

	std::size_t detailed_task_report_replayable_spool::task_count() const noexcept
	{
		return record_offsets_.size();
	}

	std::uint64_t detailed_task_report_replayable_spool::spooled_bytes() const noexcept
	{
		return spooled_bytes_;
	}

	bool detailed_task_report_replayable_spool::sealed() const noexcept
	{
		return sealed_;
	}

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
		if (!account_task_capture(capture, limits_, capture_bytes) ||
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

		auto capacity = checked_detailed_report_capacity_upper_bound(model);
		if (!capacity)
			return sdk::unexpected(std::move(capacity.error()));
		if (capacity->total > model.limits.max_projection_bytes)
			return sdk::unexpected(
				fail(detailed_report_error_kind::limit_exceeded, "report", "capacity-reservation"));

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
