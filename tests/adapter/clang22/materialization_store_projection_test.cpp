#include "llvm/clang22/materialization_store_projection.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/clang22/materialization_bounded_report.hpp"

namespace
{
	namespace materialization = cxxlens::detail::clang22::materialization;
	namespace sdk = cxxlens::sdk;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	using record = materialization::materialization_store_projection_record;
	using kind = materialization::materialization_store_projection_record_kind;

	[[nodiscard]] std::vector<record> canonical_records()
	{
		return {
			{kind::partition_begin, "partition:0", {std::byte{'b'}}},
			{kind::semantic_key, "partition:0/key:0", {std::byte{'k'}}},
			{kind::claim_full_projection, "partition:0/claim:0", {std::byte{'c'}}},
			{kind::detached_row, "partition:0/row:0", {std::byte{'r'}}},
			{kind::claim_annotation, "partition:0/annotation:0", {std::byte{'a'}}},
			{kind::coverage, "partition:0/coverage:0", {std::byte{'v'}}},
			{kind::unresolved, "partition:0/unresolved:0", {std::byte{'u'}}},
			{kind::closure_binding, "partition:0/closure:0", {std::byte{'l'}}},
			{kind::provenance, "partition:0/provenance:0", {std::byte{'p'}}},
			{kind::guarantee, "partition:0/guarantee:0", {std::byte{'g'}}},
			{kind::partition_census, "partition:0/census", {std::byte{'n'}}},
			{kind::partition_end, "partition:0", {std::byte{'e'}}},
			{kind::global_identity, "snapshot:0", {std::byte{'i'}}},
		};
	}

	[[nodiscard]] materialization::materialization_store_projection_stream
	make_stream(const std::vector<record>& records,
				const materialization::materialization_store_projection_limits limits = {})
	{
		auto stream = materialization::materialization_store_projection_stream::create(limits);
		require(stream.has_value(), "projection stream creation failed");
		for (const auto& value : records)
			require(stream->append(value).has_value(), "projection record append failed");
		require(stream->seal().has_value(), "projection stream seal failed");
		return std::move(*stream);
	}

	void positive_canonical_projection_preserves_digests()
	{
		const auto records = canonical_records();
		auto expected = make_stream(records);
		auto actual = make_stream(records);
		auto comparison =
			materialization::compare_materialization_store_projections(expected, actual);
		require(comparison && comparison->equal(), "canonical expected/actual projection differed");
		require(expected.record_count() == records.size() && expected.byte_count() != 0U,
				"projection stream lost bounded record census");
		require(expected.content_digest() && expected.content_digest()->starts_with("sha256:"),
				"content digest was not retained");
		require(expected.semantic_digest() &&
					expected.semantic_digest()->starts_with("semantic-v2:sha256:"),
				"semantic digest was not retained");
		auto expected_content = expected.content_digest();
		auto actual_content = actual.content_digest();
		auto expected_semantic = expected.semantic_digest();
		auto actual_semantic = actual.semantic_digest();
		require(expected_content && actual_content && expected_semantic && actual_semantic &&
					*expected_content == *actual_content && *expected_semantic == *actual_semantic,
				"equivalent streams changed their semantic/content digests");

		auto cursor = expected.open_cursor();
		require(cursor.has_value(), "sealed projection cursor did not open");
		std::size_t count{};
		for (;; ++count)
		{
			auto next = (*cursor)->next();
			require(next.has_value(), "projection cursor read failed");
			if (!*next)
				break;
			require(**next == records[count], "projection cursor changed a record");
		}
		require(count == records.size(), "projection cursor census changed");
	}

	void permutation_and_reordered_input_are_rejected()
	{
		const auto records = canonical_records();
		auto expected = make_stream(records);
		auto permuted = records;
		std::swap(permuted[1U], permuted[2U]);
		auto actual = make_stream(permuted);
		auto order = actual.validate_canonical_order();
		require(!order && order.error().code == "store.corrupt" &&
					order.error().field == "projection-order" &&
					order.error().detail == "noncanonical",
				"permuted/reordered backend records were accepted");
		auto comparison =
			materialization::compare_materialization_store_projections(expected, actual);
		require(!comparison && comparison.error().code == "store.corrupt",
				"comparison bypassed backend physical-key order");
	}

	void missing_extra_and_payload_mismatch_are_values()
	{
		const auto records = canonical_records();
		{
			auto expected = make_stream(records);
			auto missing_records = records;
			missing_records.erase(missing_records.begin() + 3U);
			auto actual = make_stream(missing_records);
			auto comparison =
				materialization::compare_materialization_store_projections(expected, actual);
			require(comparison &&
						comparison->kind ==
							materialization::materialization_store_projection_mismatch_kind::
								expected_missing &&
						comparison->record_index == 3U && comparison->expected.has_value() &&
						!comparison->actual.has_value(),
					"missing actual record was not an expected/actual value mismatch");
		}
		{
			auto expected_records = records;
			expected_records.pop_back();
			auto expected = make_stream(expected_records);
			auto actual = make_stream(records);
			auto comparison =
				materialization::compare_materialization_store_projections(expected, actual);
			require(comparison &&
						comparison->kind ==
							materialization::materialization_store_projection_mismatch_kind::
								actual_extra &&
						comparison->record_index == expected_records.size() &&
						!comparison->expected.has_value() && comparison->actual.has_value(),
					"extra actual record was not an expected/actual value mismatch");
		}
		{
			auto changed = records;
			changed[2U].payload = {std::byte{'x'}};
			auto expected = make_stream(records);
			auto actual = make_stream(changed);
			auto comparison =
				materialization::compare_materialization_store_projections(expected, actual);
			require(comparison &&
						comparison->kind ==
							materialization::materialization_store_projection_mismatch_kind::
								full_byte_mismatch &&
						comparison->record_index == 2U && comparison->expected &&
						comparison->actual,
					"payload tamper was not a full-byte expected/actual mismatch");
		}
	}

	void checksum_tamper_and_truncation_fail_closed()
	{
		const record value{
			kind::claim_full_projection, "p/claim", {std::byte{'c'}, std::byte{'1'}}};
		auto encoded = materialization::encode_materialization_store_projection_record(value);
		require(encoded.has_value(), "record encoding failed");
		auto decoded = materialization::decode_materialization_store_projection_record(*encoded);
		require(decoded && *decoded == value, "encoded record did not decode exactly");

		auto tampered = *encoded;
		// Keep the framing lengths intact so the checksum, rather than a length parser, owns
		// this corruption verdict.
		tampered[17U + value.key.size()] ^= std::byte{1};
		auto tampered_result =
			materialization::decode_materialization_store_projection_record(tampered);
		require(!tampered_result && tampered_result.error().code == "store.corrupt" &&
					tampered_result.error().detail == "checksum",
				"checksum-recomputed tamper was accepted");

		for (std::size_t length : {std::size_t{0U}, encoded->size() - 1U, encoded->size() / 2U})
		{
			auto truncated = materialization::decode_materialization_store_projection_record(
				std::span<const std::byte>{encoded->data(), length});
			require(!truncated && truncated.error().code == "store.corrupt",
					"truncated projection frame was accepted");
		}
	}

	void limits_bound_records_spool_and_cursor_storage()
	{
		materialization::materialization_store_projection_limits limits;
		limits.max_record_bytes = 96U;
		limits.max_spool_bytes = 192U;
		limits.max_records = 2U;
		auto stream_result =
			materialization::materialization_store_projection_stream::create(limits);
		require(stream_result.has_value(), "small bounded projection stream did not create");
		auto stream = std::move(*stream_result);
		require(stream.append({kind::semantic_key, "k0", {std::byte{'0'}}}).has_value(),
				"bounded first record did not append");
		const std::array large_payload{
			std::byte{'a'}, std::byte{'b'}, std::byte{'c'}, std::byte{'d'}, std::byte{'e'},
			std::byte{'f'}, std::byte{'g'}, std::byte{'h'}, std::byte{'i'}, std::byte{'j'},
			std::byte{'k'}, std::byte{'l'}, std::byte{'m'}, std::byte{'n'}, std::byte{'o'},
			std::byte{'p'}, std::byte{'q'}, std::byte{'r'}, std::byte{'s'}, std::byte{'t'},
			std::byte{'u'}, std::byte{'v'}, std::byte{'w'}, std::byte{'x'}, std::byte{'y'},
			std::byte{'z'}};
		const record large_record{
			kind::semantic_key, "k1", {large_payload.begin(), large_payload.end()}};
		auto oversized = stream.append(large_record);
		require(!oversized && oversized.error().code == "store.resource-limit",
				"record budget did not reject oversized frame");
		require(stream.append({kind::semantic_key, "k1", {std::byte{'1'}}}).has_value(),
				"bounded second record did not append");
		auto third = stream.append({kind::semantic_key, "k2", {std::byte{'2'}}});
		require(!third && third.error().code == "store.resource-limit",
				"record-count budget did not reject third record");
		require(stream.seal().has_value(), "bounded stream did not seal");
		require(stream.record_count() == 2U, "bounded stream retained rejected records");
	}

	[[nodiscard]] std::unique_ptr<materialization::materialization_private_spool>
	make_report_spool()
	{
		auto spool = materialization::make_materialization_private_spool();
		require(spool.has_value(), "report private spool did not create");
		return std::move(*spool);
	}

	void report_tail_is_reserved_bounded_and_digest_bound()
	{
		materialization::materialization_bounded_report_limits limits;
		limits.max_report_bytes = 8U;
		limits.max_tail_bytes = 4U;
		auto writer_result = materialization::materialization_bounded_report_writer::create(
			make_report_spool(), limits);
		require(writer_result.has_value(), "bounded report writer did not create");
		auto writer = std::move(*writer_result);
		const std::array bytes{std::byte{'o'}, std::byte{'k'}, std::byte{'!'}, std::byte{'\n'}};
		auto before_reserve = writer.append(bytes);
		require(!before_reserve && before_reserve.error().code == "materialization.report-invalid",
				"report bytes were accepted before reservation");
		require(writer.reserve().has_value(), "report tail did not reserve");
		require(writer.append(bytes).has_value(), "report tail append failed");
		const std::array extra{std::byte{'x'}};
		auto overflow = writer.append(extra);
		require(!overflow && overflow.error().code == "materialization.report-limit",
				"report tail exceeded its checked maximum");
		require(
			writer
				.finalize(
					materialization::materialization_bounded_report_terminal::committed_verified)
				.has_value(),
			"report tail did not finalize");
		require(writer.finalized() && writer.bytes_written() == bytes.size() &&
					writer.terminal() ==
						std::optional{materialization::materialization_bounded_report_terminal::
										  committed_verified},
				"report terminal or byte census drifted");
		require(writer.content_digest() && writer.content_digest()->starts_with("sha256:") &&
					writer.semantic_digest() &&
					writer.semantic_digest()->starts_with("semantic-v2:sha256:"),
				"report semantic/content digests were not retained");
		auto after_finalize = writer.append(extra);
		require(!after_finalize && after_finalize.error().code == "materialization.report-invalid",
				"finalized report accepted another append");
	}
} // namespace

int main()
{
	positive_canonical_projection_preserves_digests();
	permutation_and_reordered_input_are_rejected();
	missing_extra_and_payload_mismatch_are_values();
	checksum_tamper_and_truncation_fail_closed();
	limits_bound_records_spool_and_cursor_storage();
	report_tail_is_reserved_bounded_and_digest_bound();
	return 0;
}
