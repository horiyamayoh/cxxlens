#include "sdk/materialization_store_candidate_bridge_internal.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using namespace cxxlens::sdk::detail;
	using cxxlens::sdk::result;

	void require(const bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] std::string input_digest(const std::vector<std::vector<std::byte>>& tasks,
										   std::uint64_t& input_bytes)
	{
		auto digest = make_materialization_sha256_accumulator();
		require(digest != nullptr, "digest adapter unavailable");
		input_bytes = 0U;
		for (std::size_t index{}; index < tasks.size(); ++index)
		{
			bounded_store_record record;
			record.kind = bounded_store_record_kind::task_result;
			record.key = std::to_string(index);
			record.payload = tasks[index];
			auto encoded = encode_bounded_store_record(record);
			require(encoded.has_value(), "task frame failed");
			require(digest->update(*encoded).has_value(), "digest update failed");
			input_bytes += tasks[index].size();
		}
		auto result = digest->finish();
		require(result.has_value(), "digest finish failed");
		return std::move(*result);
	}

	[[nodiscard]] materialization_store_candidate_bridge_request
	make_request(const bool mismatch,
				 const bool unknown,
				 const bool replay_failure = false,
				 const bool resource_limit = false)
	{
		std::vector<std::vector<std::byte>> tasks{
			{std::byte{0x01U}, std::byte{0x02U}},
			{std::byte{0x03U}},
		};
		std::uint64_t input_bytes{};
		const auto digest = input_digest(tasks, input_bytes);
		materialization_store_candidate_bridge_request request;
		request.staging_session_id = "staging-test";
		request.expected_head = "head-0";
		request.external_census = {tasks.size(), input_bytes, digest};
		if (resource_limit)
			request.limits.max_tasks = 1U;
		request.replay_tasks = [tasks = std::move(tasks),
								replay_failure](const auto& consumer) -> result<void>
		{
			for (std::size_t index{}; index < tasks.size(); ++index)
			{
				if (replay_failure && index == 1U)
					return cxxlens::sdk::unexpected({"test.replay-failure", "task", "injected"});
				const auto& task = tasks[index];
				if (auto consumed = consumer(task); !consumed)
					return consumed;
			}
			return {};
		};
		request.build_expected_projection = [](bounded_store_record_spool& spool) -> result<void>
		{
			bounded_store_record record;
			record.kind = bounded_store_record_kind::global_identity;
			record.key = "snapshot";
			record.payload = {std::byte{0xa1U}};
			return spool.append(record);
		};
		request.build_actual_projection =
			[mismatch](bounded_store_record_spool& spool) -> result<void>
		{
			bounded_store_record record;
			record.kind = bounded_store_record_kind::global_identity;
			record.key = "snapshot";
			record.payload = {mismatch ? std::byte{0xa2U} : std::byte{0xa1U}};
			return spool.append(record);
		};
		request.write_publication_independent_report =
			[](bounded_store_report_writer& report) -> result<void>
		{
			const std::array<std::byte, 2U> prefix{std::byte{'{'}, std::byte{'"'}};
			return report.append(prefix);
		};
		request.write_exact_outcome_report = [](bounded_store_report_writer& report,
												const auto) -> result<void>
		{
			const std::array<std::byte, 2U> suffix{std::byte{'}'}, std::byte{'\n'}};
			return report.append(suffix);
		};
		if (!unknown)
			request.publish_once = [](const auto, const auto)
			{
				return bounded_store_publication_terminal::committed_verified;
			};
		else
			request.publish_once = [](const auto, const auto)
			{
				return bounded_store_publication_terminal::publication_outcome_unknown;
			};
		return request;
	}
} // namespace

int main()
{
	using namespace cxxlens::sdk::detail;
	{
		auto result = run_materialization_store_candidate_bridge(make_request(false, false));
		require(result.has_value(), "positive bridge failed");
		require(result->terminal &&
					*result->terminal == bounded_store_publication_terminal::committed_verified,
				"positive terminal mismatch");
		require(result->report_finalized, "positive report not finalized");
	}
	{
		auto result = run_materialization_store_candidate_bridge(make_request(true, false));
		require(!result.has_value(), "projection mismatch accepted");
		require(result.error().code == "store.corrupt",
				"projection mismatch classification changed");
	}
	{
		auto result = run_materialization_store_candidate_bridge(make_request(false, true));
		require(result.has_value(), "opaque terminal was not retained");
		require(result->terminal &&
					*result->terminal ==
						bounded_store_publication_terminal::publication_outcome_unknown,
				"opaque publication lost terminal classification");
	}
	{
		auto result = run_materialization_store_candidate_bridge(make_request(false, false, true));
		require(!result.has_value(), "task replay fault unexpectedly succeeded");
		require(result.error().code == "test.replay-failure", "task replay fault was remapped");
	}
	{
		auto result =
			run_materialization_store_candidate_bridge(make_request(false, false, false, true));
		require(!result.has_value(), "task resource limit unexpectedly succeeded");
		require(result.error().code == "store.resource-limit", "task resource limit was not typed");
	}
	{
		auto first = run_materialization_store_candidate_bridge(make_request(false, false));
		auto second = run_materialization_store_candidate_bridge(make_request(false, false));
		require(first.has_value() && second.has_value(), "repeat bridge run failed");
		require(first->terminal == second->terminal, "repeat terminal is nondeterministic");
	}
	return 0;
}
