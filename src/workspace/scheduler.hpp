#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include <cxxlens/core/failure.hpp>
#include <cxxlens/facts.hpp>

#include "../llvm/common/frontend_port.hpp"
#include "../runtime/hash_port.hpp"
#include "../runtime/time_port.hpp"

namespace cxxlens::detail::scheduling
{
	enum class task_kind : std::uint8_t
	{
		parse,
		refinement,
	};

	enum class task_priority : std::uint8_t
	{
		explicit_target,
		changed,
		refinement,
		background,
	};

	struct subscriber_request
	{
		std::string id;
		std::stop_token cancellation;
	};

	struct task_request
	{
		task_kind kind{task_kind::parse};
		task_priority priority{task_priority::background};
		frontend::parse_task parse;
		fact_profile profile{fact_profile::minimal()};
		std::string snapshot_key;
		std::string refinement_id;
		std::vector<std::string> dependencies;
		std::vector<subscriber_request> subscribers;
		std::uint64_t cost{1U};
	};

	enum class task_state : std::uint8_t
	{
		succeeded,
		failed,
		cancelled,
		deadline_exceeded,
		budget_exhausted,
		output_limited,
		dependency_failed,
	};

	struct subscriber_delivery
	{
		std::string id;
		task_state state{task_state::failed};
		std::string reason_code;
		auto operator<=>(const subscriber_delivery&) const = default;
	};

	struct task_result
	{
		std::string task_key;
		std::string input_fingerprint;
		task_kind kind{task_kind::parse};
		task_state state{task_state::failed};
		std::string reason_code;
		std::vector<std::string> dependencies;
		std::vector<subscriber_delivery> deliveries;
		std::vector<frontend::normalized_diagnostic> diagnostics;
		frontend::parse_coverage frontend_coverage;
		std::string semantic_batch;
		std::shared_ptr<const frontend::observation_batch> observation_batch;
	};

	struct scheduler_coverage
	{
		std::uint64_t requested{};
		std::uint64_t succeeded{};
		std::uint64_t failed{};
		std::uint64_t cancelled{};
		std::uint64_t deadline_exceeded{};
		std::uint64_t budget_exhausted{};
		std::uint64_t output_limited{};
		std::uint64_t dependency_failed{};
	};

	struct trace_row
	{
		std::string task_key;
		std::string input_fingerprint;
		std::string event;
		std::string detail;
		auto operator<=>(const trace_row&) const = default;
	};

	struct scheduler_batch
	{
		std::string schema{"cxxlens.scheduler-trace.v1"};
		std::vector<task_result> tasks;
		std::vector<trace_row> trace;
		scheduler_coverage coverage;

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string to_json() const;
	};

	struct scheduler_options
	{
		std::size_t jobs{1U};
		std::uint64_t seed{};
		std::size_t maximum_queued_tasks{1024U};
		std::size_t maximum_output_bytes{std::size_t{16U} * 1024U * 1024U};
		std::size_t memory_per_job_mb{256U};
		std::uint64_t cost_budget{};
		execution_context execution;
	};

	class worker_port
	{
	  public:
		virtual ~worker_port() = default;
		[[nodiscard]] virtual result<frontend::observation_batch>
		execute(const task_request& task, execution_context context) = 0;
	};

	class scheduler
	{
	  public:
		scheduler(const runtime::hash_port& hashes, const runtime::time_port& time) noexcept;
		[[nodiscard]] result<std::string> task_key(const task_request& task) const;
		[[nodiscard]] result<scheduler_batch> run(std::vector<task_request> requests,
												  worker_port& worker,
												  scheduler_options options = {}) const;

	  private:
		const runtime::hash_port& hashes_;
		const runtime::time_port& time_;
	};
} // namespace cxxlens::detail::scheduling
