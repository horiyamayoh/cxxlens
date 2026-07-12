#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cxxlens/facts.hpp>

#include "scheduler.hpp"

namespace cxxlens::detail::provisioning
{
	struct service_options
	{
		path root;
		std::vector<compile_unit> units;
		std::optional<path> configuration_file;
		std::optional<path> cache_directory;
		std::string workspace_key;
	};

	struct trace_row
	{
		std::string requirement_id;
		std::string state;
		std::string reason_code;
		auto operator<=>(const trace_row&) const = default;
	};

	struct provisioning_trace
	{
		std::string schema{"cxxlens.provisioning-trace.v1"};
		std::uint64_t requested{};
		std::uint64_t warm{};
		std::uint64_t scheduled{};
		std::uint64_t covered{};
		std::uint64_t failed{};
		std::uint64_t unresolved{};
		std::vector<trace_row> rows;

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string to_json() const;
	};

	class provisioning_service
	{
	  public:
		[[nodiscard]] static result<std::shared_ptr<provisioning_service>>
		create(service_options options, std::shared_ptr<scheduling::worker_port> worker = {});

		[[nodiscard]] result<void>
		ensure(const fact_profile& profile, const analysis_scope& scope, execution_context context);
		[[nodiscard]] fact_store facts() const;
		[[nodiscard]] capability_set capabilities() const;
		[[nodiscard]] result<std::vector<diagnostic>> diagnose(execution_context context) const;
		[[nodiscard]] provisioning_trace last_trace() const;

		void set_worker(std::shared_ptr<scheduling::worker_port> worker,
						bool requires_clang_capability);

	  private:
		struct implementation;
		explicit provisioning_service(std::unique_ptr<implementation> value);
		std::unique_ptr<implementation> implementation_;
	};
} // namespace cxxlens::detail::provisioning

namespace cxxlens::detail
{
	struct workspace_provisioning_access
	{
		static void set_worker(workspace& value,
							   std::shared_ptr<scheduling::worker_port> worker,
							   bool requires_clang_capability = false);
		[[nodiscard]] static provisioning::provisioning_trace last_trace(const workspace& value);
	};
} // namespace cxxlens::detail
