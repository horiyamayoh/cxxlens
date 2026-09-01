#include "sdk/materialization_store_candidate_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using namespace cxxlens::sdk::detail;
	using cxxlens::sdk::error;
	using cxxlens::sdk::result;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	struct spool_observation
	{
		std::uint64_t append_calls{};
		std::uint64_t seal_calls{};
		std::uint64_t read_calls{};
		bool fail_append{};
		bool fail_seal{};
		bool fail_read{};
	};

	class memory_spool final : public materialization_replayable_spool
	{
	  public:
		explicit memory_spool(std::shared_ptr<spool_observation> observation)
			: observation_{std::move(observation)}
		{
		}

		materialization_io_result<void> append(const std::span<const std::byte> bytes) override
		{
			++observation_->append_calls;
			if (sealed_)
				return materialization_io_failure{
					materialization_io_failure_kind::invalid_configuration,
					materialization_io_operation::spool_write};
			if (observation_->fail_append)
				return materialization_io_failure{materialization_io_failure_kind::write,
												  materialization_io_operation::spool_write};
			bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
			return {};
		}

		materialization_io_result<void> seal() override
		{
			++observation_->seal_calls;
			if (observation_->fail_seal)
				return materialization_io_failure{materialization_io_failure_kind::spool,
												  materialization_io_operation::spool_seal};
			if (sealed_)
				return {};
			sealed_ = true;
			return {};
		}

		materialization_io_result<std::size_t> read(std::span<std::byte> destination) override
		{
			return read_at(cursor_, destination);
		}

		materialization_io_result<std::size_t> read_at(const std::uint64_t offset,
													   std::span<std::byte> destination) override
		{
			++observation_->read_calls;
			if (observation_->fail_read)
				return materialization_io_failure{materialization_io_failure_kind::read,
												  materialization_io_operation::spool_read};
			if (offset > bytes_.size())
				return materialization_io_failure{
					materialization_io_failure_kind::invalid_configuration,
					materialization_io_operation::spool_read};
			if (offset == bytes_.size() || destination.empty())
				return std::size_t{};
			const auto count = std::min<std::size_t>(
				bytes_.size() - static_cast<std::size_t>(offset), destination.size());
			std::ranges::copy(std::span{bytes_}.subspan(static_cast<std::size_t>(offset), count),
							  destination.begin());
			cursor_ = offset + count;
			return count;
		}

		materialization_io_result<void> rewind() override
		{
			cursor_ = 0U;
			return {};
		}

		[[nodiscard]] std::uint64_t size_bytes() const noexcept override
		{
			return bytes_.size();
		}
		[[nodiscard]] bool sealed() const noexcept override
		{
			return sealed_;
		}

	  private:
		std::shared_ptr<spool_observation> observation_;
		std::vector<std::byte> bytes_;
		std::uint64_t cursor_{};
		bool sealed_{};
	};

	[[nodiscard]] std::unique_ptr<bounded_store_record_spool>
	make_record_spool(const std::shared_ptr<spool_observation>& observation,
					  const bounded_store_limits limits = {})
	{
		auto wrapped =
			make_bounded_store_record_spool(std::make_unique<memory_spool>(observation), limits);
		require(wrapped.has_value(), "record spool did not begin");
		return std::move(*wrapped);
	}

	[[nodiscard]] std::unique_ptr<materialization_private_spool>
	make_report_spool(const std::shared_ptr<spool_observation>& observation)
	{
		return std::make_unique<memory_spool>(observation);
	}

	struct backend final : bounded_store_publication_port
	{
		bounded_store_publication_terminal terminal{
			bounded_store_publication_terminal::committed_verified};
		std::uint64_t calls{};
		bool throw_on_call{};
		std::function<void()> on_call;

		bounded_store_publication_terminal publish_once(const std::string_view,
														const std::string_view) override
		{
			++calls;
			if (on_call)
				on_call();
			if (throw_on_call)
				throw std::runtime_error{"opaque publication failure"};
			return terminal;
		}
	};

	struct candidate_fixture
	{
		bounded_store_candidate candidate;
		std::uint64_t input_bytes{};
		std::string input_digest;
	};

	[[nodiscard]] candidate_fixture
	make_candidate(const bounded_store_limits limits = {},
				   const std::function<result<void>()>& cleanup = {})
	{
		const auto input_observation = std::make_shared<spool_observation>();
		const auto expected_observation = std::make_shared<spool_observation>();
		const auto actual_observation = std::make_shared<spool_observation>();
		auto candidate =
			begin_bounded_store_candidate("staging-200",
										  "head-0",
										  limits,
										  make_record_spool(input_observation, limits),
										  make_record_spool(expected_observation, limits),
										  make_record_spool(actual_observation, limits),
										  cleanup);
		require(candidate.has_value(), "candidate did not begin");
		const std::array task_bytes{std::byte{static_cast<unsigned char>('t')},
									std::byte{static_cast<unsigned char>('a')},
									std::byte{static_cast<unsigned char>('s')},
									std::byte{static_cast<unsigned char>('k')}};
		require(candidate->append_task(task_bytes).has_value(), "initial task did not append");
		const bounded_store_record task_record{
			bounded_store_record_kind::task_result, "0", {task_bytes.begin(), task_bytes.end()}};
		auto encoded = encode_bounded_store_record(task_record, limits);
		require(encoded.has_value(), "initial task did not encode");
		require(candidate
					->seal_input({1U,
								  static_cast<std::uint64_t>(task_bytes.size()),
								  cxxlens::sdk::content_digest(*encoded)})
					.has_value(),
				"initial task input did not seal");
		return {std::move(*candidate),
				static_cast<std::uint64_t>(task_bytes.size()),
				cxxlens::sdk::content_digest(*encoded)};
	}

	[[nodiscard]] std::vector<bounded_store_record> projection(const bool tamper = false)
	{
		const auto byte = [](const unsigned char value)
		{
			return std::vector<std::byte>{std::byte{value}};
		};
		return {
			{bounded_store_record_kind::partition_begin, "p", byte('b')},
			{bounded_store_record_kind::semantic_key, "p/key", byte('k')},
			{bounded_store_record_kind::claim_full_projection, "p/claim", byte(tamper ? 'x' : 'c')},
			{bounded_store_record_kind::detached_row, "p/row", byte('r')},
			{bounded_store_record_kind::coverage, "p/coverage", byte('v')},
			{bounded_store_record_kind::partition_end, "p", byte('e')},
			{bounded_store_record_kind::global_identity, "snapshot", byte('i')},
		};
	}

	void build_projection_pair(bounded_store_candidate& candidate, const bool tamper_actual = false)
	{
		const auto expected = projection();
		auto expected_result = candidate.build_expected_projection(
			[&](bounded_store_record_spool& sink) -> result<void>
			{
				for (const auto& record : expected)
					if (auto appended = sink.append(record); !appended)
						return cxxlens::sdk::unexpected(std::move(appended.error()));
				return {};
			});
		require(expected_result.has_value(), "expected projection did not seal");
		const auto actual = projection(tamper_actual);
		auto actual_result = candidate.build_actual_projection(
			[&](bounded_store_record_spool& sink) -> result<void>
			{
				for (const auto& record : actual)
					if (auto appended = sink.append(record); !appended)
						return cxxlens::sdk::unexpected(std::move(appended.error()));
				return {};
			});
		require(actual_result.has_value(), "actual projection did not seal");
	}

	void reserve_report(bounded_store_candidate& candidate, bounded_store_report_writer& report)
	{
		require(candidate.compare_projections().has_value(), "projections did not compare");
		require(candidate.reserve_report_tail(report).has_value(), "report tail not reserved");
	}

	void overflow_is_rejected_before_record_spool_io()
	{
		bounded_store_limits limits;
		limits.max_record_bytes = 90U;
		const auto observation = std::make_shared<spool_observation>();
		auto spool = make_record_spool(observation, limits);
		const bounded_store_record oversized{
			bounded_store_record_kind::task_result,
			"0",
			std::vector<std::byte>(limits.max_record_bytes, std::byte{0})};
		auto appended = spool->append(oversized);
		require(!appended && appended.error().code == "store.resource-limit",
				"record overflow was accepted");
		require(observation->append_calls == 0U && spool->byte_count() == 0U,
				"record overflow touched private spool I/O");

		const auto report_observation = std::make_shared<spool_observation>();
		limits.report_tail_bytes = 4U;
		limits.max_report_bytes = 4U;
		auto report_result =
			make_bounded_store_report_writer(make_report_spool(report_observation), limits);
		require(report_result.has_value(), "small report writer did not begin");
		auto report = std::move(*report_result);
		require(report.reserve().has_value(), "small report did not reserve");
		const std::array five{
			std::byte{1U}, std::byte{2U}, std::byte{3U}, std::byte{4U}, std::byte{5U}};
		auto report_overflow = report.append(five);
		require(!report_overflow && report_overflow.error().code == "store.resource-limit" &&
					report_observation->append_calls == 0U,
				"report overflow touched private spool I/O");
	}

	void exact_task_and_aggregate_limits_are_terminal()
	{
		bounded_store_limits task_limits;
		task_limits.max_tasks = 1U;
		const auto task_observation = std::make_shared<spool_observation>();
		auto task_spool = make_record_spool(task_observation, task_limits);
		auto expected_spool = make_record_spool(std::make_shared<spool_observation>(), task_limits);
		auto actual_spool = make_record_spool(std::make_shared<spool_observation>(), task_limits);
		auto candidate = begin_bounded_store_candidate("staging-task-limit",
													   "head",
													   task_limits,
													   std::move(task_spool),
													   std::move(expected_spool),
													   std::move(actual_spool));
		require(candidate.has_value(), "task-limit candidate did not begin");
		const std::array one{std::byte{1U}};
		require(candidate->append_task(one).has_value(), "task-limit first task failed");
		const auto writes_before = task_observation->append_calls;
		auto extra = candidate->append_task(one);
		require(!extra && extra.error().code == "store.resource-limit" &&
					candidate->phase() == bounded_store_candidate_phase::aborted &&
					task_observation->append_calls == writes_before,
				"4096/task bound was not terminal before spool I/O");

		bounded_store_limits aggregate_limits;
		aggregate_limits.max_tasks = 2U;
		aggregate_limits.max_aggregate_bytes = 1U;
		const auto aggregate_observation = std::make_shared<spool_observation>();
		auto aggregate_candidate = begin_bounded_store_candidate(
			"staging-aggregate-limit",
			"head",
			aggregate_limits,
			make_record_spool(aggregate_observation, aggregate_limits),
			make_record_spool(std::make_shared<spool_observation>(), aggregate_limits),
			make_record_spool(std::make_shared<spool_observation>(), aggregate_limits));
		require(aggregate_candidate.has_value(), "aggregate-limit candidate did not begin");
		auto two = aggregate_candidate->append_task(one);
		require(two.has_value(), "aggregate-limit first task failed");
		const auto aggregate_writes_before = aggregate_observation->append_calls;
		const std::array another{std::byte{2U}};
		auto aggregate_overflow = aggregate_candidate->append_task(another);
		require(!aggregate_overflow && aggregate_overflow.error().code == "store.resource-limit" &&
					aggregate_observation->append_calls == aggregate_writes_before,
				"aggregate limit touched private spool I/O");
	}

	void four_thousand_ninety_six_tasks_are_admitted_once()
	{
		const auto observation = std::make_shared<spool_observation>();
		const auto limits = bounded_store_limits{};
		auto candidate = begin_bounded_store_candidate(
			"staging-4096",
			"head",
			limits,
			make_record_spool(observation, limits),
			make_record_spool(std::make_shared<spool_observation>(), limits),
			make_record_spool(std::make_shared<spool_observation>(), limits));
		require(candidate.has_value() && bounded_store_max_tasks == 4096U,
				"4096-task candidate did not begin");
		auto digest = make_materialization_sha256_accumulator();
		require(digest != nullptr, "4096-task digest did not begin");
		const std::array one{std::byte{7U}};
		for (std::uint64_t index{}; index < bounded_store_max_tasks; ++index)
		{
			const bounded_store_record record{bounded_store_record_kind::task_result,
											  std::to_string(index),
											  {one.begin(), one.end()}};
			auto encoded = encode_bounded_store_record(record, limits);
			require(encoded.has_value(), "4096-task digest record failed to encode");
			require(digest->update(*encoded).has_value(), "4096-task digest update failed");
			require(candidate->append_task(one).has_value(), "4096-task append failed");
		}
		auto input_digest = digest->finish();
		require(input_digest.has_value(), "4096-task digest did not finish");
		require(
			candidate->seal_input({bounded_store_max_tasks, bounded_store_max_tasks, *input_digest})
				.has_value(),
			"4096-task input did not seal");
	}

	void spool_and_cleanup_faults_preserve_original_terminal()
	{
		const auto failing_observation = std::make_shared<spool_observation>();
		failing_observation->fail_append = true;
		std::uint64_t cleanup_calls{};
		auto candidate = begin_bounded_store_candidate(
			"staging-spool-fault",
			"head",
			{},
			make_record_spool(failing_observation),
			make_record_spool(std::make_shared<spool_observation>()),
			make_record_spool(std::make_shared<spool_observation>()),
			[&]() -> result<void>
			{
				++cleanup_calls;
				return cxxlens::sdk::unexpected(error{"test.cleanup", "staging", "fault"});
			});
		require(candidate.has_value(), "spool-fault candidate did not begin");
		const std::array one{std::byte{1U}};
		auto failed = candidate->append_task(one);
		require(!failed && failed.error().code == "store.spool-failure" &&
					candidate->phase() == bounded_store_candidate_phase::aborted &&
					candidate->cleanup_failed() && cleanup_calls == 1U,
				"spool/cleanup fault did not fail closed");
		backend publication;
		auto retry = candidate->publish_once(publication);
		require(!retry && publication.calls == 0U,
				"spool-fault candidate crossed publication boundary");

		const auto seal_observation = std::make_shared<spool_observation>();
		seal_observation->fail_seal = true;
		auto seal_candidate =
			begin_bounded_store_candidate("staging-seal-fault",
										  "head",
										  {},
										  make_record_spool(seal_observation),
										  make_record_spool(std::make_shared<spool_observation>()),
										  make_record_spool(std::make_shared<spool_observation>()));
		require(seal_candidate.has_value() && seal_candidate->append_task(one).has_value(),
				"seal-fault setup failed");
		const bounded_store_record record{
			bounded_store_record_kind::task_result, "0", {one.begin(), one.end()}};
		auto encoded = encode_bounded_store_record(record);
		require(encoded.has_value(), "seal-fault census record failed");
		auto seal_failed =
			seal_candidate->seal_input({1U, 1U, cxxlens::sdk::content_digest(*encoded)});
		require(!seal_failed && seal_failed.error().code == "store.spool-failure" &&
					seal_candidate->phase() == bounded_store_candidate_phase::aborted,
				"spool seal fault did not fail closed");

		const auto report_observation = std::make_shared<spool_observation>();
		report_observation->fail_append = true;
		auto report_result =
			make_bounded_store_report_writer(make_report_spool(report_observation));
		require(report_result.has_value(), "report-fault writer did not begin");
		auto report = std::move(*report_result);
		require(report.reserve().has_value(), "report-fault writer did not reserve");
		auto report_failed = report.append(one);
		require(!report_failed && report_failed.error().code == "store.spool-failure" &&
					report_observation->append_calls == 1U,
				"report spool fault was not classified");
		auto report_retry = report.append(one);
		require(!report_retry && report_retry.error().code == "store.candidate-state" &&
					report_observation->append_calls == 1U,
				"failed report spool was retried");
	}

	void publication_is_one_shot_and_partial_visibility_is_forbidden()
	{
		std::uint64_t cleanup_calls{};
		auto fixture = make_candidate({},
									  [&]() -> result<void>
									  {
										  ++cleanup_calls;
										  return {};
									  });
		build_projection_pair(fixture.candidate);
		const auto report_observation = std::make_shared<spool_observation>();
		auto report_result =
			make_bounded_store_report_writer(make_report_spool(report_observation));
		require(report_result.has_value(), "publication report did not begin");
		auto report = std::move(*report_result);
		reserve_report(fixture.candidate, report);
		const std::array detail{std::byte{9U}};
		require(report.append(detail).has_value(), "publication detail did not append");
		backend publication;
		publication.terminal = bounded_store_publication_terminal::publication_outcome_unknown;
		require(fixture.candidate.publish_once(publication).has_value(),
				"unknown publication was not captured as terminal");
		require(
			publication.calls == 1U && cleanup_calls == 1U &&
				fixture.candidate.publication_terminal() ==
					std::optional{bounded_store_publication_terminal::publication_outcome_unknown},
			"unknown publication or cleanup terminal drifted");
		auto retry = fixture.candidate.publish_once(publication);
		require(!retry && publication.calls == 1U, "ambiguous publication was retried");
		require(fixture.candidate.finalize_report(report).has_value() && report.finalized(),
				"terminal report did not finalize");

		// A projection mismatch aborts before reserve/publication and leaves zero backend calls.
		auto mismatch = make_candidate();
		build_projection_pair(mismatch.candidate, true);
		auto compared = mismatch.candidate.compare_projections();
		require(!compared && compared.error().code == "store.corrupt" &&
					mismatch.candidate.phase() == bounded_store_candidate_phase::aborted,
				"projection mismatch was published");
		backend no_publication;
		auto publish_after_mismatch = mismatch.candidate.publish_once(no_publication);
		require(!publish_after_mismatch && no_publication.calls == 0U,
				"mismatched candidate crossed publication boundary");
	}

	void publication_exception_is_unknown_and_not_retried()
	{
		auto fixture = make_candidate();
		build_projection_pair(fixture.candidate);
		auto report_result = make_bounded_store_report_writer(
			make_report_spool(std::make_shared<spool_observation>()));
		require(report_result.has_value(), "exception report did not begin");
		auto report = std::move(*report_result);
		reserve_report(fixture.candidate, report);
		backend publication;
		publication.throw_on_call = true;
		auto thrown = fixture.candidate.publish_once(publication);
		require(
			!thrown && thrown.error().code == "store.publication-outcome-unknown" &&
				fixture.candidate.publication_terminal() ==
					std::optional{bounded_store_publication_terminal::publication_outcome_unknown},
			"publication exception was not an unknown terminal");
		auto retry = fixture.candidate.publish_once(publication);
		require(!retry && publication.calls == 1U, "publication exception was retried");
	}
} // namespace

int main()
{
	overflow_is_rejected_before_record_spool_io();
	exact_task_and_aggregate_limits_are_terminal();
	four_thousand_ninety_six_tasks_are_admitted_once();
	spool_and_cleanup_faults_preserve_original_terminal();
	publication_is_one_shot_and_partial_visibility_is_forbidden();
	publication_exception_is_unknown_and_not_retried();
	return 0;
}
