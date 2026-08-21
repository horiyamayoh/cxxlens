#include "llvm/clang22/materialization_store_candidate.hpp"

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

namespace
{
	namespace sdk = cxxlens::sdk;
	using namespace cxxlens::detail::clang22::materialization;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] std::unique_ptr<bounded_store_record_spool>
	make_record_spool(const bounded_store_limits limits = {})
	{
		auto raw = make_materialization_private_spool();
		require(raw.has_value(), "reference record spool did not open");
		auto wrapped = make_bounded_store_record_spool(std::move(*raw), limits);
		require(wrapped.has_value(), "reference record spool did not wrap");
		return std::move(*wrapped);
	}

	[[nodiscard]] std::unique_ptr<materialization_private_spool> make_report_spool()
	{
		auto raw = make_materialization_private_spool();
		require(raw.has_value(), "reference report spool did not open");
		return std::move(*raw);
	}

	struct fake_publication final : bounded_store_publication_port
	{
		bounded_store_publication_terminal result{
			bounded_store_publication_terminal::committed_verified};
		std::string candidate;
		std::string expected_head;
		std::uint32_t calls{};

		bounded_store_publication_terminal publish_once(const std::string_view candidate_id,
														const std::string_view head) override
		{
			++calls;
			candidate = candidate_id;
			expected_head = head;
			return result;
		}
	};

	struct candidate_fixture
	{
		bounded_store_candidate candidate;
		bounded_store_external_census census;
	};

	[[nodiscard]] candidate_fixture make_candidate(const bounded_store_limits limits = {},
												   const bool seal = true)
	{
		auto input = make_record_spool(limits);
		auto expected = make_record_spool(limits);
		auto actual = make_record_spool(limits);
		auto candidate = begin_bounded_store_candidate("staging-private-1",
													   "head-0",
													   limits,
													   std::move(input),
													   std::move(expected),
													   std::move(actual));
		require(candidate.has_value(), "candidate did not begin");
		const std::array task_bytes{std::byte{'t'}, std::byte{'a'}, std::byte{'s'}, std::byte{'k'}};
		require(candidate->append_task(task_bytes).has_value(), "candidate task did not append");
		const bounded_store_record task_record{
			bounded_store_record_kind::task_result, "0", {task_bytes.begin(), task_bytes.end()}};
		auto encoded = encode_bounded_store_record(task_record, limits);
		require(encoded.has_value(), "candidate task oracle did not encode");
		candidate_fixture output{
			std::move(*candidate),
			{1U, static_cast<std::uint64_t>(task_bytes.size()), sdk::content_digest(*encoded)}};
		if (seal)
			require(output.candidate.seal_input(output.census).has_value(),
					"candidate input did not seal");
		return output;
	}

	[[nodiscard]] std::vector<bounded_store_record> projection(const bool tamper = false)
	{
		return {
			{bounded_store_record_kind::partition_begin, "p0", {std::byte{'b'}}},
			{bounded_store_record_kind::semantic_key, "p0/key", {std::byte{'k'}}},
			{bounded_store_record_kind::claim_full_projection,
			 "p0/claim",
			 {std::byte{static_cast<unsigned char>(tamper ? 'x' : 'c')}}},
			{bounded_store_record_kind::detached_row, "p0/row", {std::byte{'r'}}},
			{bounded_store_record_kind::claim_annotation, "p0/annotation", {std::byte{'a'}}},
			{bounded_store_record_kind::coverage, "p0/coverage", {std::byte{'v'}}},
			{bounded_store_record_kind::unresolved, "p0/unresolved", {std::byte{'u'}}},
			{bounded_store_record_kind::closure_binding, "p0/closure", {std::byte{'l'}}},
			{bounded_store_record_kind::provenance, "p0/provenance", {std::byte{'p'}}},
			{bounded_store_record_kind::guarantee, "p0/guarantee", {std::byte{'g'}}},
			{bounded_store_record_kind::partition_census, "p0/census", {std::byte{'n'}}},
			{bounded_store_record_kind::partition_end, "p0", {std::byte{'e'}}},
			{bounded_store_record_kind::global_identity, "snapshot", {std::byte{'i'}}},
		};
	}

	void build_projection_pair(bounded_store_candidate& candidate, const bool tamper_actual = false)
	{
		const auto expected = projection();
		require(candidate
					.build_expected_projection(
						[&](bounded_store_record_spool& sink) -> sdk::result<void>
						{
							for (const auto& record : expected)
								if (auto appended = sink.append(record); !appended)
									return sdk::unexpected(std::move(appended.error()));
							return {};
						})
					.has_value(),
				"expected projection did not seal");
		const auto actual = projection(tamper_actual);
		require(candidate
					.build_actual_projection(
						[&](bounded_store_record_spool& sink) -> sdk::result<void>
						{
							for (const auto& record : actual)
								if (auto appended = sink.append(record); !appended)
									return sdk::unexpected(std::move(appended.error()));
							return {};
						})
					.has_value(),
				"actual projection did not seal");
	}

	void reserve_report(bounded_store_candidate& candidate, bounded_store_report_writer& report)
	{
		require(candidate.compare_projections().has_value(), "dual projection did not compare");
		require(candidate.reserve_report_tail(report).has_value(), "report tail did not reserve");
	}

	void positive_memory_and_sqlite_compatible_port_contract()
	{
		auto fixture = make_candidate();
		auto report_result = make_bounded_store_report_writer(make_report_spool());
		require(report_result.has_value(), "report writer did not begin");
		auto report = std::move(*report_result);
		build_projection_pair(fixture.candidate);
		reserve_report(fixture.candidate, report);
		const std::array detail{std::byte{'o'}, std::byte{'k'}};
		require(report.append(detail).has_value(), "report detail did not append");
		fake_publication backend;
		require(fixture.candidate.publish_once(backend).has_value(), "publication did not run");
		require(backend.calls == 1U && backend.candidate == fixture.candidate.candidate_id() &&
					backend.expected_head == "head-0",
				"publication identity/head binding was not retained");
		require(fixture.candidate.publication_terminal() ==
					std::optional{bounded_store_publication_terminal::committed_verified},
				"committed terminal was not retained");
		require(fixture.candidate.finalize_report(report).has_value(),
				"report did not finalize after Store terminal");
		require(report.finalized() &&
					report.terminal() ==
						std::optional{bounded_store_publication_terminal::committed_verified} &&
					report.bytes_written() == 2U,
				"report terminal/bytes were not exact");
	}

	void full_byte_projection_tamper_and_order_are_rejected()
	{
		auto tampered = make_candidate();
		build_projection_pair(tampered.candidate, true);
		auto mismatch = tampered.candidate.compare_projections();
		require(!mismatch && mismatch.error().code == "store.corrupt" &&
					mismatch.error().detail == "full-byte-mismatch",
				"full-byte tamper was accepted");

		auto reordered = make_candidate();
		const auto expected = projection();
		require(reordered.candidate
					.build_expected_projection(
						[&](bounded_store_record_spool& sink) -> sdk::result<void>
						{
							for (const auto& record : expected)
								if (auto appended = sink.append(record); !appended)
									return sdk::unexpected(std::move(appended.error()));
							return {};
						})
					.has_value(),
				"reordered expected projection did not seal");
		auto actual = expected;
		std::ranges::swap(actual[0], actual[1]);
		auto actual_result = reordered.candidate.build_actual_projection(
			[&](bounded_store_record_spool& sink) -> sdk::result<void>
			{
				for (const auto& record : actual)
					if (auto appended = sink.append(record); !appended)
						return sdk::unexpected(std::move(appended.error()));
				return {};
			});
		require(!actual_result && actual_result.error().code == "store.corrupt" &&
					actual_result.error().field == "actual-projection",
				"backend physical-key reorder was accepted");
	}

	void unknown_terminal_is_fail_closed_and_not_retried()
	{
		auto fixture = make_candidate();
		build_projection_pair(fixture.candidate);
		auto report_result = make_bounded_store_report_writer(make_report_spool());
		require(report_result.has_value(), "unknown report writer did not begin");
		auto report = std::move(*report_result);
		reserve_report(fixture.candidate, report);
		fake_publication backend;
		backend.result = bounded_store_publication_terminal::publication_outcome_unknown;
		require(fixture.candidate.publish_once(backend).has_value(),
				"unknown publication was not retained as a terminal");
		require(fixture.candidate.publication_terminal() ==
					std::optional{bounded_store_publication_terminal::publication_outcome_unknown},
				"unknown publication terminal was lost");
		auto replay = fixture.candidate.publish_once(backend);
		require(!replay && replay.error().code == "store.candidate-state" && backend.calls == 1U,
				"ambiguous publication was retried");
		require(fixture.candidate.finalize_report(report).has_value(),
				"unknown report did not finalize safely");
	}

	void report_reservation_and_resource_bounds_are_enforced()
	{
		bounded_store_limits limits;
		limits.max_tasks = 1U;
		limits.max_aggregate_bytes = 4U;
		limits.max_record_bytes = 128U;
		limits.max_spool_bytes = 4096U;
		limits.report_tail_bytes = 4U;
		limits.max_report_bytes = 4U;
		auto fixture = make_candidate(limits, false);
		const std::array another{std::byte{'x'}};
		auto extra = fixture.candidate.append_task(another);
		require(!extra && extra.error().code == "store.resource-limit",
				"task bound was not enforced");
		require(fixture.candidate.seal_input(fixture.census).has_value(),
				"bounded candidate input did not seal");
		build_projection_pair(fixture.candidate);
		auto report_result = make_bounded_store_report_writer(make_report_spool(), limits);
		require(report_result.has_value(), "bounded report writer did not begin");
		auto report = std::move(*report_result);
		fake_publication backend;
		auto before_reservation = fixture.candidate.publish_once(backend);
		require(!before_reservation && before_reservation.error().code == "store.candidate-state",
				"publication crossed before report reservation");
		reserve_report(fixture.candidate, report);
		const std::array too_large{
			std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'}};
		auto report_overflow = report.append(too_large);
		require(!report_overflow && report_overflow.error().code == "store.resource-limit",
				"report tail bound was not enforced");
	}

	void invalid_terminal_is_fail_closed()
	{
		auto fixture = make_candidate();
		build_projection_pair(fixture.candidate);
		auto report_result = make_bounded_store_report_writer(make_report_spool());
		require(report_result.has_value(), "invalid-terminal report writer did not begin");
		auto report = std::move(*report_result);
		reserve_report(fixture.candidate, report);
		fake_publication backend;
		backend.result = static_cast<bounded_store_publication_terminal>(255U);
		auto publication = fixture.candidate.publish_once(backend);
		require(!publication && publication.error().code == "store.publication-outcome-invalid" &&
					fixture.candidate.publication_terminal() ==
						std::optional{
							bounded_store_publication_terminal::publication_outcome_unknown} &&
					backend.calls == 1U,
				"invalid publication terminal was not converted to one fail-closed unknown");
		require(fixture.candidate.finalize_report(report).has_value(),
				"invalid-terminal unknown report did not finalize");
	}
} // namespace

int main()
{
	positive_memory_and_sqlite_compatible_port_contract();
	full_byte_projection_tamper_and_order_are_rejected();
	unknown_terminal_is_fail_closed_and_not_retried();
	report_reservation_and_resource_bounds_are_enforced();
	invalid_terminal_is_fail_closed();
	return 0;
}
