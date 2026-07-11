#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "fault_plan.hpp"

namespace cxxlens::detail::runtime
{

	struct process_request
	{
		std::vector<std::string> argv;
		std::filesystem::path working_directory;
		std::vector<std::pair<std::string, std::string>> environment;
		std::chrono::milliseconds timeout{std::chrono::seconds{300}};
		bool shell_allowed{false};
	};

	struct process_result
	{
		int exit_code{};
		std::string standard_output;
		std::string standard_error;
	};

	class process_port
	{
	  public:
		virtual ~process_port() = default;
		[[nodiscard]] virtual runtime_result<process_result>
		run(const process_request& request, const request_context& context) const = 0;
	};

	class argv_process_adapter final : public process_port
	{
	  public:
		[[nodiscard]] runtime_result<process_result>
		run(const process_request& request, const request_context& context) const override;
	};

	class fake_process_adapter final : public process_port
	{
	  public:
		explicit fake_process_adapter(process_result result = {}, fault_plan faults = {});
		[[nodiscard]] runtime_result<process_result>
		run(const process_request& request, const request_context& context) const override;

	  private:
		process_result result_;
		fault_plan faults_;
	};

} // namespace cxxlens::detail::runtime
