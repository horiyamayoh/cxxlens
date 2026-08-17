#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/company_lock_acquire.hpp>
#include <cxxlens/sdk.hpp>
#if defined(__linux__) && defined(__GLIBC__)
#include <sys/syscall.h>
#endif
#if defined(__linux__) && defined(__GLIBC__) && defined(SYS_pidfd_open) && \
	defined(SYS_pidfd_send_signal)
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#endif
#include <sys/socket.h>
#include <unistd.h>

#include "sdk/provider_ng1_process_internal.hpp"
#include "sdk/provider_ng1_transport_internal.hpp"
#include "sdk/provider_runtime_internal.hpp"
#include "sdk/provider_validation_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::provider;
	static_assert(!std::is_aggregate_v<provider_selection>);
	static_assert(std::is_same_v<decltype(std::declval<provider_selection&>().selected_candidate()),
								 const provider_candidate&>);

	constexpr std::string_view binary_digest =
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	constexpr std::string_view fixture_contract_digest =
		"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
#if defined(CXXLENS_SANITIZER_INSTRUMENTED)
	constexpr std::uint64_t provider_address_space_budget =
		std::numeric_limits<std::uint64_t>::max();
	constexpr std::uint64_t provider_subprocess_budget = 1024U;
#else
	constexpr std::uint64_t provider_address_space_budget = 256U * 1024U * 1024U;
	constexpr std::uint64_t provider_subprocess_budget = 1U;
#endif
	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] sandbox_policy baseline_policy()
	{
		auto policies = builtin_sandbox_policies();
		require(policies.size() == 2U && policies.front().validate().has_value(),
				"built-in sandbox policy registry is invalid");
		return std::move(policies.front());
	}

	[[nodiscard]] detached_row protocol_test_row()
	{
		using relation = cxxlens::company::relations::lock_acquire;
		relation::builder builder;
		require(builder
					.set<relation::acquire>(
						detached_cell::typed("company_lock_acquire_id", "lock-acquire:1"))
					.has_value(),
				"protocol row acquire setup failed");
		require(builder.set<relation::lock>(detached_cell::typed("company_lock_id", "lock:1"))
					.has_value(),
				"protocol row lock setup failed");
		require(builder.set<relation::source>(detached_cell::typed("source_span_id", "span:1"))
					.has_value(),
				"protocol row source setup failed");
		require(builder
					.set<relation::mode>(
						detached_cell{{scalar_kind::open_symbol, "company.lock-mode/1", false},
									  cell_state::present,
									  scalar_value{std::string{"exclusive"}},
									  std::nullopt})
					.has_value(),
				"protocol row mode setup failed");
		require(builder.set<relation::ordinal>(detached_cell::unsigned_integer(0U)).has_value(),
				"protocol row ordinal setup failed");
		auto row = std::move(builder).finish();
		require(row.has_value(), "protocol row validation failed");
		return std::move(*row);
	}

	[[nodiscard]] detached_row identity_valid_protocol_test_row()
	{
		auto row = protocol_test_row();
		const auto& descriptor = cxxlens::company::relations::lock_acquire::descriptor();
		auto identity = derive_domain_identity(descriptor, row);
		require(identity.has_value(), "protocol row identity derivation failed");
		row.cells.at("company.lock.acquire.v1.acquire") =
			detached_cell::typed("company_lock_acquire_id", std::move(*identity));
		require(validate_domain_identity(descriptor, row).has_value(),
				"protocol row identity setup failed");
		return row;
	}

	[[nodiscard]] detached_row resultless_protocol_test_row()
	{
		using relation = cxxlens::cc::relations::call_direct_target;
		relation::builder builder;
		require(builder.set<relation::call>(detached_cell::typed("cc_call_id", "call:direct-1"))
					.has_value(),
				"resultless call setup failed");
		require(
			builder.set<relation::target>(detached_cell::typed("cc_entity_id", "entity:target-1"))
				.has_value(),
			"resultless target setup failed");
		require(builder
					.set<relation::resolution>(detached_cell{
						{scalar_kind::open_symbol, "cc.direct-target-resolution/1", false},
						cell_state::present,
						scalar_value{std::string{"exact"}},
						std::nullopt})
					.has_value(),
				"resultless resolution setup failed");
		auto row = std::move(builder).finish();
		require(row.has_value() &&
					!cxxlens::cc::relations::call_direct_target::descriptor()
						 .domain_identity.result_column,
				"resultless protocol row validation failed");
		return std::move(*row);
	}

	class transcript_sink final : public frame_sink
	{
	  public:
		result<void> write(const std::span<const std::byte> bytes) override
		{
			transcript.insert(transcript.end(), bytes.begin(), bytes.end());
			return {};
		}

		std::vector<std::byte> transcript;
	};

	[[nodiscard]] std::uint64_t logical_output_size(const std::span<const frame> frames)
	{
		std::uint64_t output{};
		for (const auto& value : frames)
			if (cxxlens::sdk::provider::detail::counts_toward_output_budget(value.type))
				output += value.control.size() + value.payload.size();
		return output;
	}

	class parity_provider final : public portable_provider
	{
	  public:
		[[nodiscard]] std::string_view id() const noexcept override
		{
			return "company.test.process-provider";
		}
		[[nodiscard]] semantic_version version() const noexcept override
		{
			return {1U, 0U, 0U};
		}
		[[nodiscard]] std::string_view semantic_contract_digest() const noexcept override
		{
			return fixture_contract_digest;
		}
		result<void> run(const cxxlens::sdk::provider::task& task_value,
						 cxxlens::sdk::provider::context& context) override
		{
			auto output = context.relation(cxxlens::company::relations::lock_acquire::descriptor());
			if (auto begun = output.begin("dependency-1", "atomic-1", "batch-1"); !begun)
				return begun;
			if (auto pushed = output.push(protocol_test_row()); !pushed)
				return pushed;
			if (auto pushed = output.push(protocol_test_row()); !pushed)
				return pushed;
			if (auto ended = output.end(); !ended)
				return ended;
			context.coverage().request("task", task_value.task_id);
			return context.coverage().classify({"task", task_value.task_id, "covered", {}});
		}
	};

	class sealed_parity_provider final : public portable_provider
	{
	  public:
		[[nodiscard]] std::string_view id() const noexcept override
		{
			return "company.test.process-provider";
		}
		[[nodiscard]] semantic_version version() const noexcept override
		{
			return {1U, 0U, 0U};
		}
		[[nodiscard]] std::string_view semantic_contract_digest() const noexcept override
		{
			return fixture_contract_digest;
		}
		result<void> run(const cxxlens::sdk::provider::task& task_value,
						 cxxlens::sdk::provider::context& context) override
		{
			auto output = context.relation(cxxlens::company::relations::lock_acquire::descriptor());
			if (auto begun = output.begin("dependency-1", "atomic-1", "batch-1"); !begun)
				return begun;
			if (auto pushed = output.push(identity_valid_protocol_test_row()); !pushed)
				return pushed;
			if (auto pushed = output.push(identity_valid_protocol_test_row()); !pushed)
				return pushed;
			if (auto ended = output.end(); !ended)
				return ended;
			if (auto begun = output.begin("dependency-2", "atomic-2", "batch-2"); !begun)
				return begun;
			if (auto ended = output.end(); !ended)
				return ended;
			auto resultless =
				context.relation(cxxlens::cc::relations::call_direct_target::descriptor());
			if (auto begun = resultless.begin("dependency-3", "atomic-3", "batch-3"); !begun)
				return begun;
			if (auto pushed = resultless.push(resultless_protocol_test_row()); !pushed)
				return pushed;
			if (auto ended = resultless.end(); !ended)
				return ended;
			context.coverage().request("task", task_value.task_id);
			return context.coverage().classify({"task", task_value.task_id, "covered", {}});
		}
	};

	class invalid_sandbox_port final : public provider_process_port
	{
	  public:
		explicit invalid_sandbox_port(const sandbox_assurance achieved) : achieved_{achieved} {}

		[[nodiscard]] result<process_output> run(const process_invocation& invocation,
												 std::stop_token) const override
		{
			return process_output{
				process_status::exited,
				0,
				0,
				{},
				{},
				{"test-port",
				 {"invalid-assurance"},
				 achieved_,
				 invocation.sandbox.policy_digest,
				 "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},
				{},
				{}};
		}

	  private:
		sandbox_assurance achieved_;
	};

	[[nodiscard]] std::string executable_digest(const std::string& executable)
	{
		std::ifstream input{executable, std::ios::binary};
		require(input.good(), "provider fixture could not be opened");
		const std::string bytes{std::istreambuf_iterator<char>{input},
								std::istreambuf_iterator<char>{}};
		require(!input.bad(), "provider fixture could not be read");
		return content_digest(std::as_bytes(std::span{bytes}));
	}

	[[nodiscard]] std::string selection_fixture_digest(const std::string& executable)
	{
		static std::optional<std::pair<std::string, std::string>> cached;
		if (!cached || cached->first != executable)
			cached.emplace(executable, executable_digest(executable));
		return cached->second;
	}

	[[nodiscard]] relation_descriptor snapshot_test_descriptor()
	{
		relation_descriptor value;
		value.id = "company.test.runtime_snapshot.v1";
		value.name = "company.test.runtime_snapshot";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.test.runtime_snapshot/1";
		value.owner_namespace = "company.test";
		value.columns = {
			{"company.test.runtime_snapshot.v1.key",
			 "key",
			 {scalar_kind::typed_id, "runtime_snapshot_id", false},
			 true,
			 column_role::claim_key},
		};
		value.key_columns = {"company.test.runtime_snapshot.v1.key"};
		value.merge = merge_mode::set;
		value.descriptor_digest =
			*semantic_digest("cxxlens.relation-descriptor-binding.v2",
							 value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] detached_row snapshot_test_row()
	{
		auto descriptor = snapshot_test_descriptor();
		row_builder builder{descriptor};
		require(
			builder
				.set(
					{descriptor.id, descriptor.columns.front().id, descriptor.columns.front().type},
					detached_cell::typed("runtime_snapshot_id", "runtime:snapshot:1"))
				.has_value(),
			"snapshot test row setup failed");
		auto row = std::move(builder).finish();
		require(row.has_value(), "snapshot test row validation failed");
		return std::move(*row);
	}

	[[nodiscard]] partition_draft snapshot_test_partition(const relation_engine& engine)
	{
		observation observed{
			snapshot_test_row(),
			{"runtime-universe", {"all"}},
			"company.test.runtime-canonical-1",
			{"company.test.process-provider", std::string{binary_digest}},
			{"sha256:7777777777777777777777777777777777777777777777777777777777777777"},
			"evidence:runtime-snapshot",
			{"exact", "partition", "assumptions:none", {"schema_validated"}},
		};
		auto assertion = make_assertion(engine, std::move(observed));
		require(assertion.has_value(), "snapshot test assertion failed");
		partition_draft draft;
		draft.relation_descriptor_id = snapshot_test_descriptor().id;
		draft.scope = "runtime-scope";
		draft.condition = {"runtime-universe", {"all"}};
		draft.interpretation = "company.test.runtime-canonical-1";
		draft.producer_semantics = binary_digest;
		draft.precision_profile = "exact";
		draft.assumption_set_id = "assumptions:none";
		draft.claims = {std::move(*assertion)};
		auto basis = claim_input_basis_digest(draft.claims.front().input_basis);
		require(basis.has_value(), "snapshot test input basis failed");
		draft.producer_input_basis_digest = std::move(*basis);
		draft.coverage = {{"runtime", "runtime-scope", "covered", ""}};
		return draft;
	}

	[[nodiscard]] manifest make_manifest(const semantic_version version = {1U, 0U, 0U},
										 std::string binary = std::string{binary_digest})
	{
		manifest value;
		value.provider_id = "company.test.process-provider";
		value.provider_version = version;
		value.package_identity = "company.test.process-provider.package";
		value.publisher = "company.test";
		value.license = "Apache-2.0";
		value.protocol = {1U, 0U, 0U, {"credit-backpressure"}, {}};
		value.platform_tuples = {"linux-glibc"};
		value.provider_binary_digest = std::move(binary);
		value.provider_semantic_contract_digest = fixture_contract_digest;
		value.offered_relations = {"company.lock.acquire@1"};
		value.interpretation_domains = {"provider.company.test.process-provider"};
		value.invalidation_contract =
			"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
		value.determinism_contract =
			"sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
		value.resource_class = "provider.test";
		value.sandbox_minimum = "enforced";
		value.requested_qualifications = {"canonical-semantic-qualified"};
		return value;
	}

	[[nodiscard]] sandbox_report make_sandbox(const sandbox_assurance achieved)
	{
		auto policy = baseline_policy();
		return {"linux-glibc",
				policy.mechanisms,
				achieved,
				policy.policy_digest(),
				"sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
	}

	[[nodiscard]] sandbox_report make_sandbox(const sandbox_policy& policy,
											  const sandbox_assurance achieved)
	{
		return {"linux-glibc",
				policy.mechanisms,
				achieved,
				policy.policy_digest(),
				"sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
	}

	[[nodiscard]] provider_candidate
	candidate(const std::string& executable,
			  const std::string& mode,
			  const discovery_source source = discovery_source::explicit_path,
			  const sandbox_assurance achieved = sandbox_assurance::enforced)
	{
		return {make_manifest({1U, 0U, 0U}, selection_fixture_digest(executable)),
				source,
				{executable, mode},
				true,
				true,
				true,
				{"canonical-semantic-qualified"},
				make_sandbox(achieved),
				{}};
	}

	[[nodiscard]] provider_selection_request selection_request(const std::string& executable)
	{
		return {"company.test.process-provider",
				{1U, 0U, 0U},
				selection_fixture_digest(executable),
				std::string{fixture_contract_digest},
				{sandbox_assurance::enforced, baseline_policy().policy_digest()},
				true,
				std::nullopt};
	}

#if defined(__linux__) && defined(__GLIBC__) && defined(SYS_pidfd_open) && \
	defined(SYS_pidfd_send_signal)
	[[nodiscard]] std::optional<std::uint64_t> process_start_time(const pid_t process)
	{
		std::ifstream stat{std::string{"/proc/"} + std::to_string(process) + "/stat"};
		std::string line;
		if (!std::getline(stat, line))
			return std::nullopt;
		const auto closing_name = line.rfind(')');
		if (closing_name == std::string::npos || closing_name + 2U >= line.size())
			return std::nullopt;
		std::istringstream fields{line.substr(closing_name + 2U)};
		char state{};
		if (!(fields >> state))
			return std::nullopt;
		for (std::uint32_t field = 4U; field <= 22U; ++field)
		{
			std::uint64_t value{};
			if (!(fields >> value))
				return std::nullopt;
			if (field == 22U)
				return value;
		}
		return std::nullopt;
	}

	[[nodiscard]] int open_pidfd(const pid_t process)
	{
		return static_cast<int>(::syscall(SYS_pidfd_open, process, 0U));
	}

	[[nodiscard]] bool pidfd_exited(const int pidfd)
	{
		siginfo_t status{};
		if (::waitid(P_PIDFD, static_cast<id_t>(pidfd), &status, WEXITED | WNOHANG | WNOWAIT) ==
				0 &&
			status.si_pid != 0)
			return true;
		pollfd descriptor{pidfd, POLLIN | POLLHUP | POLLERR, 0};
		const auto polled = ::poll(&descriptor, 1U, 0);
		return polled > 0 && (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
	}

	[[nodiscard]] bool wait_pidfd_exit(const int pidfd, const std::chrono::milliseconds timeout)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (!pidfd_exited(pidfd) && std::chrono::steady_clock::now() < deadline)
		{
			pollfd descriptor{pidfd, POLLIN | POLLHUP | POLLERR, 0};
			(void)::poll(&descriptor, 1U, 10);
		}
		return pidfd_exited(pidfd);
	}

	[[nodiscard]] bool process_is_zombie(const pid_t process)
	{
		std::ifstream stat{std::string{"/proc/"} + std::to_string(process) + "/stat"};
		std::string line;
		if (!std::getline(stat, line))
			return false;
		const auto closing_name = line.rfind(')');
		return closing_name != std::string::npos && closing_name + 2U < line.size() &&
			line[closing_name + 2U] == 'Z';
	}

	[[nodiscard]] bool kill_pidfd(const int pidfd)
	{
		return ::syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0U) == 0;
	}

	[[nodiscard]] bool pidfd_runtime_available()
	{
		const auto pidfd = open_pidfd(::getpid());
		if (pidfd < 0)
			return false;
		const bool can_signal = ::syscall(SYS_pidfd_send_signal, pidfd, 0, nullptr, 0U) == 0;
		(void)::close(pidfd);
		return can_signal;
	}

	struct holder_observation
	{
		bool valid{};
		int pidfd{-1};
		pid_t pid{};
		std::uint64_t start_time{};
	};

	struct descendant_observation
	{
		holder_observation holder;
		holder_observation sentinel;
		std::chrono::steady_clock::time_point ready_at{};
	};

	[[nodiscard]] std::optional<holder_observation>
	observe_descendant(const std::filesystem::path& marker)
	{
		std::ifstream marker_input{marker};
		long long raw_pid{};
		std::uint64_t raw_start_time{};
		if (!(marker_input >> raw_pid >> raw_start_time) || raw_pid <= 0 || raw_start_time == 0 ||
			raw_pid > static_cast<long long>(std::numeric_limits<pid_t>::max()))
			return std::nullopt;
		const auto pid = static_cast<pid_t>(raw_pid);
		const auto pidfd = open_pidfd(pid);
		if (pidfd < 0)
			return std::nullopt;
		const auto actual_start_time = process_start_time(pid);
		if (!actual_start_time || *actual_start_time != raw_start_time)
		{
			(void)::close(pidfd);
			return std::nullopt;
		}
		return holder_observation{true, pidfd, pid, raw_start_time};
	}

	[[nodiscard]] bool process_exit_observed(const holder_observation& observation)
	{
		if (pidfd_exited(observation.pidfd))
			return true;
		const auto start_time = process_start_time(observation.pid);
		return start_time && *start_time == observation.start_time &&
			process_is_zombie(observation.pid);
	}

	[[nodiscard]] std::optional<std::uint64_t> descendant_fixture_subprocess_budget()
	{
		namespace fs = std::filesystem;
		rlimit limit{};
		require(::getrlimit(RLIMIT_NPROC, &limit) == 0,
				"could not inspect the inherited process-count ceiling");
		std::error_code iteration_error;
		std::uint64_t same_uid_threads{};
		for (fs::directory_iterator entry{fs::path{"/proc"}, iteration_error};
			 entry != fs::directory_iterator{} && !iteration_error;
			 entry.increment(iteration_error))
		{
			const auto name = entry->path().filename().string();
			std::uint64_t pid{};
			const auto [end, parse_error] =
				std::from_chars(name.data(), name.data() + name.size(), pid);
			if (parse_error != std::errc{} || end != name.data() + name.size())
				continue;
			std::ifstream status{entry->path() / "status"};
			std::string field;
			std::uint64_t real_uid{};
			std::uint64_t threads{1U};
			bool found_uid{};
			while (status >> field)
			{
				if (field == "Uid:")
				{
					found_uid = static_cast<bool>(status >> real_uid);
				}
				else if (field == "Threads:")
					status >> threads;
				std::string ignored;
				std::getline(status, ignored);
			}
			if (found_uid && real_uid == static_cast<std::uint64_t>(::getuid()))
				same_uid_threads += threads;
		}
		require(!iteration_error, "could not count same-uid threads for timeout fixture");
		const auto required = same_uid_threads + 3U;
		constexpr std::uint64_t scheduling_margin = 4U;
		const auto maximum = limit.rlim_max > std::numeric_limits<std::uint64_t>::max()
			? std::numeric_limits<std::uint64_t>::max()
			: static_cast<std::uint64_t>(limit.rlim_max);
		const auto requested = std::max(provider_subprocess_budget, required + scheduling_margin);
		if (maximum < requested)
			return std::nullopt;
		return requested;
	}

	void check_ng1_post_fork_guard_kills_group_without_ack()
	{
		if (!pidfd_runtime_available())
			return;
		int ready[2]{};
		require(::pipe(ready) == 0, "NG1 post-fork guard readiness pipe failed");
		const auto child = ::fork();
		require(child >= 0, "NG1 post-fork guard fork failed");
		if (child == 0)
		{
			(void)::close(ready[0]);
			if (::setpgid(0, 0) != 0)
				::_exit(120);
			const auto descendant = ::fork();
			if (descendant < 0)
				::_exit(121);
			if (descendant == 0)
			{
				(void)::close(ready[1]);
				for (;;)
					(void)::pause();
			}
			const auto written = ::write(ready[1], &descendant, sizeof(descendant));
			(void)::close(ready[1]);
			if (written != static_cast<ssize_t>(sizeof(descendant)))
				::_exit(122);
			for (;;)
				(void)::pause();
		}

		(void)::close(ready[1]);
		bool ready_ok{};
		pid_t descendant{};
		int descendant_pidfd{-1};
		bool descendant_alive_before_cleanup{};
		{
			// Deliberately do not provide an ACK/established marker.  This models the
			// parent being descheduled until the bounded process-group handshake expires.
			cxxlens::sdk::provider::detail::ng1_post_fork_process_guard guard{
				static_cast<int>(child)};
			pollfd descriptor{ready[0], POLLIN | POLLHUP | POLLERR, 0};
			if (::poll(&descriptor, 1U, 1000) > 0)
			{
				const auto received = ::read(ready[0], &descendant, sizeof(descendant));
				ready_ok = received == static_cast<ssize_t>(sizeof(descendant));
			}
			(void)::close(ready[0]);
			if (ready_ok)
			{
				descendant_pidfd = open_pidfd(descendant);
				descendant_alive_before_cleanup =
					descendant_pidfd >= 0 && !pidfd_exited(descendant_pidfd);
			}
		} // The guard must kill the unacknowledged group's descendant before returning.

		bool descendant_exited_after_cleanup{};
		if (descendant_pidfd >= 0)
		{
			descendant_exited_after_cleanup =
				wait_pidfd_exit(descendant_pidfd, std::chrono::milliseconds{500});
			(void)::close(descendant_pidfd);
		}
		require(ready_ok, "NG1 post-fork guard did not observe the descendant");
		require(descendant_alive_before_cleanup,
				"NG1 post-fork guard descendant was not alive before cleanup");
		require(descendant_exited_after_cleanup,
				"NG1 post-fork guard leaked a descendant when the ACK was unobserved");
	}
#endif

	[[nodiscard]] provider_fallback_tuple fallback_tuple(const provider_candidate& value,
														 const std::uint32_t priority,
														 const semantic_version requested = {
															 1U, 0U, 0U})
	{
		const auto version = value.description.provider_version;
		return {priority,
				value.description.provider_id,
				version,
				value.description.provider_binary_digest,
				value.description.provider_semantic_contract_digest,
				version > requested
					? fallback_direction::upgrade
					: (version < requested ? fallback_direction::downgrade
										   : fallback_direction::same_version_rebuild),
				true,
				{}};
	}

	[[nodiscard]] process_task_request task(provider_selection selection)
	{
		process_task_request request;
		request.selection = std::move(selection);
		request.output_descriptors = {cxxlens::company::relations::lock_acquire::descriptor()};
		request.task_id = "task-1";
		request.payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
		request.task_input_digest = content_digest(request.payload);
		request.normalized_invocation_digest =
			"sha256:1111111111111111111111111111111111111111111111111111111111111111";
		request.toolchain_digest =
			"sha256:2222222222222222222222222222222222222222222222222222222222222222";
		request.environment_digest =
			"sha256:3333333333333333333333333333333333333333333333333333333333333333";
		request.sandbox = {sandbox_assurance::enforced, baseline_policy().policy_digest()};
		request.budget.wall_ms = 2000U;
		request.budget.cpu_ms = 2000U;
		request.budget.address_space_bytes = provider_address_space_budget;
		request.budget.output_bytes = 4U * 1024U * 1024U;
		request.budget.open_files = 64U;
		request.budget.subprocesses = provider_subprocess_budget;
		return request;
	}

	[[nodiscard]] provider_selection select(const std::string& executable, const std::string& mode)
	{
		const auto value = candidate(executable, mode);
		auto selected = select_provider(selection_request(executable), std::span{&value, 1U});
		require(selected.has_value(), "exact provider selection failed");
		return std::move(*selected);
	}

	void check_selection(const std::string& executable)
	{
		auto policies = builtin_sandbox_policies();
		require(policies.size() == 2U && policies[0U].id < policies[1U].id &&
					policies[0U].policy_digest() ==
						"semantic-v2:sha256:"
						"b4e95d8c88cf660fff40c4d9e7e4ae07bcb078013b5370c6b1abb80b0d75d375" &&
					policies[1U].policy_digest() ==
						"semantic-v2:sha256:"
						"6fb3327ee0028e358de90a7ca9f6c1f4d42ac156c06282579579bd0a6d1bbb44" &&
					policies[0U].policy_digest() != policies[1U].policy_digest() &&
					policies[0U].mechanisms != policies[1U].mechanisms,
				"built-in sandbox policies are not distinct canonical plans");
		auto changed_policy = policies.front();
		changed_policy.id.back() = 'f';
		require(changed_policy.validate().has_value() &&
					changed_policy.policy_digest() != policies.front().policy_digest() &&
					!resolve_sandbox_policy(changed_policy.policy_digest()) &&
					resolve_sandbox_policy(changed_policy.policy_digest()).error().code ==
						"security.sandbox-policy-mismatch",
				"one-byte sandbox policy mutation retained built-in authority");

		auto exact = candidate(executable, "success");
		auto selected = select_provider(selection_request(executable), std::span{&exact, 1U});
		require(selected && !selected->fallback_used() &&
					selected->decisions().front().reason == "selected-exact" &&
					selected->validate().has_value(),
				"exact selection was not explained");
		auto unknown_policy_request = selection_request(executable);
		unknown_policy_request.sandbox.policy_digest =
			"sha256:9999999999999999999999999999999999999999999999999999999999999999";
		auto unknown_policy = select_provider(unknown_policy_request, std::span{&exact, 1U});
		require(!unknown_policy &&
					unknown_policy.error().code == "security.sandbox-policy-mismatch",
				"unknown well-formed sandbox policy reached selection authority");

		auto adjacent = exact;
		adjacent.description.provider_version = {1U, 1U, 0U};
		auto rejected = select_provider(selection_request(executable), std::span{&adjacent, 1U});
		require(!rejected && rejected.error().code == "provider.not-found",
				"adjacent provider version silently fell back");

		auto shadow = exact;
		shadow.description.package_identity = "company.test.shadow.package";
		shadow.description.provider_binary_digest =
			"sha256:9999999999999999999999999999999999999999999999999999999999999999";
		std::array shadowed{exact, shadow};
		auto ambiguous = select_provider(selection_request(executable), shadowed);
		require(!ambiguous && ambiguous.error().code == "security.provider-shadowing",
				"provider shadowing was first-wins selected");

		std::vector<provider_candidate> metadata_variants;
		auto manifest_variant = exact;
		manifest_variant.description.publisher = "company.other";
		metadata_variants.push_back(manifest_variant);
		auto argv_variant = exact;
		argv_variant.executable_argv.back() = "alternate-mode";
		metadata_variants.push_back(argv_variant);
		auto sandbox_variant = exact;
		sandbox_variant.sandbox.evidence_digest =
			"sha256:7777777777777777777777777777777777777777777777777777777777777777";
		metadata_variants.push_back(sandbox_variant);
		auto certification_variant = exact;
		certification_variant.certification_valid = false;
		metadata_variants.push_back(certification_variant);
		for (const auto& variant : metadata_variants)
		{
			std::array forward{exact, variant};
			std::array reverse{variant, exact};
			auto forward_result = select_provider(selection_request(executable), forward);
			auto reverse_result = select_provider(selection_request(executable), reverse);
			require(!forward_result && !reverse_result &&
						forward_result.error() == reverse_result.error() &&
						forward_result.error().code == "security.provider-shadowing",
					"full candidate identity ambiguity depended on discovery input order");
		}

		auto lower_source = exact;
		lower_source.source = discovery_source::installation_manifest;
		std::array duplicate_forward{lower_source, exact};
		std::array duplicate_reverse{exact, lower_source};
		auto duplicate_selected = select_provider(selection_request(executable), duplicate_forward);
		auto duplicate_reversed = select_provider(selection_request(executable), duplicate_reverse);
		require(duplicate_selected && duplicate_reversed &&
					duplicate_selected->canonical_form() == duplicate_reversed->canonical_form() &&
					duplicate_selected->decisions().size() == 2U &&
					duplicate_selected->decisions()[0U].candidate_digest ==
						duplicate_selected->decisions()[1U].candidate_digest &&
					duplicate_selected->decisions()[0U].candidate_digest.starts_with(
						"semantic-v2:sha256:"),
				"cross-source exact duplicate selection or evidence was not canonical");
		auto same_source_duplicate = std::array{exact, exact};
		auto duplicate_rejected =
			select_provider(selection_request(executable), same_source_duplicate);
		require(!duplicate_rejected &&
					duplicate_rejected.error().code == "security.provider-shadowing",
				"same-source duplicate candidate was not rejected deterministically");

		auto weak = candidate(
			executable, "success", discovery_source::explicit_path, sandbox_assurance::best_effort);
		auto unavailable = select_provider(selection_request(executable), std::span{&weak, 1U});
		require(!unavailable && unavailable.error().code == "security.downgrade-forbidden",
				"insufficient enforced sandbox was selected");

		auto path_only = exact;
		path_only.authoritative_path = false;
		auto path_rejected =
			select_provider(selection_request(executable), std::span{&path_only, 1U});
		require(!path_rejected && path_rejected.error().code == "security.downgrade-forbidden",
				"PATH-only provider discovery became authority");

		for (auto invalid :
			 {
				 [&]
				 {
					 auto value = exact;
					 value.trust_valid = false;
					 return value;
				 }(),
				 [&]
				 {
					 auto value = exact;
					 value.certification_valid = false;
					 return value;
				 }(),
				 [&]
				 {
					 auto value = exact;
					 value.validation_error = "security.signature-mismatch";
					 return value;
				 }(),
			 })
		{
			auto verdict_rejected =
				select_provider(selection_request(executable), std::span{&invalid, 1U});
			require(!verdict_rejected,
					"invalid trust/certification verdict produced a selection token");
		}

		auto fallback = exact;
		fallback.description.provider_version = {1U, 1U, 0U};
		auto fallback_request = selection_request(executable);
		fallback_request.fallback_policy = provider_fallback_policy{
			"company.test.provider-fallback-policy", {fallback_tuple(fallback, 1U)}};
		auto allowed = select_provider(fallback_request, std::span{&fallback, 1U});
		require(allowed && allowed->fallback_used() && allowed->fallback_policy_digest() &&
					allowed->selected_candidate().description.provider_version ==
						semantic_version{1U, 1U, 0U} &&
					allowed->canonical_form().contains(*allowed->fallback_policy_digest()),
				"exact fallback policy tuple was not selected or recorded");
		auto fallback_variant = fallback;
		fallback_variant.executable_argv.back() = "alternate-fallback-mode";
		std::array fallback_forward{fallback, fallback_variant};
		std::array fallback_reverse{fallback_variant, fallback};
		auto fallback_ambiguous = select_provider(fallback_request, fallback_forward);
		auto fallback_ambiguous_reversed = select_provider(fallback_request, fallback_reverse);
		require(!fallback_ambiguous && !fallback_ambiguous_reversed &&
					fallback_ambiguous.error() == fallback_ambiguous_reversed.error() &&
					fallback_ambiguous.error().code == "security.provider-shadowing",
				"fallback candidate ambiguity depended on discovery input order");

		auto unrelated_major = fallback;
		unrelated_major.description.provider_version = {9U, 0U, 0U};
		auto major_rejected = select_provider(fallback_request, std::span{&unrelated_major, 1U});
		require(!major_rejected && major_rejected.error().code == "provider.not-found",
				"unlisted provider major was accepted by fallback policy");
		auto major_request = selection_request(executable);
		major_request.fallback_policy = provider_fallback_policy{
			"company.test.major-policy", {fallback_tuple(unrelated_major, 1U)}};
		auto major_allowed = select_provider(major_request, std::span{&unrelated_major, 1U});
		require(major_allowed && major_allowed->fallback_used(),
				"explicitly listed provider major fallback was rejected");

		auto rebuild = exact;
		rebuild.description.provider_binary_digest =
			"sha256:9999999999999999999999999999999999999999999999999999999999999999";
		auto rebuild_rejected = select_provider(fallback_request, std::span{&rebuild, 1U});
		require(!rebuild_rejected && rebuild_rejected.error().code == "provider.not-found",
				"unlisted same-version binary was accepted by fallback policy");
		auto rebuild_request = selection_request(executable);
		rebuild_request.fallback_policy =
			provider_fallback_policy{"company.test.rebuild-policy", {fallback_tuple(rebuild, 1U)}};
		auto rebuild_allowed = select_provider(rebuild_request, std::span{&rebuild, 1U});
		require(rebuild_allowed && rebuild_allowed->fallback_used(),
				"listed same-version rebuild tuple was rejected");

		auto semantic_change = fallback;
		semantic_change.description.provider_semantic_contract_digest =
			"sha256:8888888888888888888888888888888888888888888888888888888888888888";
		auto semantic_rejected = select_provider(fallback_request, std::span{&semantic_change, 1U});
		require(!semantic_rejected && semantic_rejected.error().code == "provider.not-found",
				"unlisted semantic contract was accepted by fallback policy");
		semantic_change.certified_qualifications.push_back("cross-version-qualified");
		auto semantic_entry = fallback_tuple(semantic_change, 1U);
		semantic_entry.required_qualifications = {"cross-version-qualified"};
		auto semantic_request = selection_request(executable);
		semantic_request.fallback_policy =
			provider_fallback_policy{"company.test.semantic-policy", {std::move(semantic_entry)}};
		auto semantic_allowed = select_provider(semantic_request, std::span{&semantic_change, 1U});
		require(semantic_allowed && semantic_allowed->fallback_used(),
				"qualified listed semantic contract fallback was rejected");
		auto self_claimed = semantic_change;
		self_claimed.certified_qualifications = {"canonical-semantic-qualified"};
		self_claimed.description.requested_qualifications.push_back("cross-version-qualified");
		auto self_claim_rejected = select_provider(semantic_request, std::span{&self_claimed, 1U});
		require(!self_claim_rejected && self_claim_rejected.error().code == "provider.not-found",
				"manifest self-claim substituted for certified fallback qualification");

		auto preferred = exact;
		preferred.description.provider_version = {1U, 2U, 0U};
		auto secondary = fallback;
		std::array fallback_candidates{secondary, preferred};
		auto precedence_request = selection_request(executable);
		precedence_request.fallback_policy = provider_fallback_policy{
			"company.test.precedence-policy",
			{fallback_tuple(secondary, 2U), fallback_tuple(preferred, 1U)}};
		auto precedence = select_provider(precedence_request, fallback_candidates);
		require(precedence &&
					precedence->selected_candidate().description.provider_version ==
						semantic_version{1U, 2U, 0U},
				"fallback policy priority did not define canonical selection");
		auto reversed_policy = *precedence_request.fallback_policy;
		std::ranges::reverse(reversed_policy.allowed);
		require(reversed_policy.canonical_form() ==
						precedence_request.fallback_policy->canonical_form() &&
					reversed_policy.semantic_digest() ==
						precedence_request.fallback_policy->semantic_digest(),
				"fallback policy identity depended on tuple input order");
	}

	void check_verified_executable_binding()
	{
#if defined(__linux__) && defined(__GLIBC__)
		namespace fs = std::filesystem;
		const auto root = fs::temp_directory_path() /
			("cxxlens-verified-executable-" + std::to_string(::getpid()));
		const auto parent_target = root / "parent" / "provider";
		const auto working_target = root / "working" / "provider";
		fs::create_directories(parent_target.parent_path());
		fs::create_directories(working_target.parent_path());
		fs::copy_file("/bin/true", parent_target, fs::copy_options::overwrite_existing);
		fs::copy_file("/bin/false", working_target, fs::copy_options::overwrite_existing);

		process_invocation invocation;
		invocation.argv = {"./provider"};
		invocation.working_directory = working_target.parent_path().string();
		invocation.budget.wall_ms = 2000U;
		invocation.budget.cpu_ms = 2000U;
		invocation.budget.address_space_bytes = provider_address_space_budget;
		invocation.budget.output_bytes = 1024U * 1024U;
		invocation.budget.open_files = 64U;
		invocation.budget.subprocesses = provider_subprocess_budget;
		const auto policy = baseline_policy();
		invocation.sandbox = {sandbox_assurance::enforced, policy.policy_digest()};
		auto processes = make_system_provider_process_port();
		const auto original_directory = fs::current_path();
		fs::current_path(parent_target.parent_path());
		invocation.expected_binary_digest = executable_digest(parent_target.string());
		auto parent_digest = processes->run(invocation, {});
		invocation.expected_binary_digest = executable_digest(working_target.string());
		auto working_digest = processes->run(invocation, {});
		fs::current_path(original_directory);
		const auto measured_digest = invocation.expected_binary_digest;
		const std::vector<std::string> no_mechanisms;
		auto rejected_evidence = sandbox_evidence_digest(
			policy, invocation.budget, sandbox_assurance::none, no_mechanisms, measured_digest);
		auto executed_evidence = sandbox_evidence_digest(policy,
														 invocation.budget,
														 sandbox_assurance::enforced,
														 policy.mechanisms,
														 measured_digest);

		const auto race_target = root / "race" / "provider";
		const auto verified_old = root / "race" / "verified-old";
		fs::create_directories(race_target.parent_path());
		fs::copy_file("/bin/true", race_target, fs::copy_options::overwrite_existing);
		{
			std::ofstream padding{race_target, std::ios::binary | std::ios::app};
			const std::array<char, 1024U * 1024U> zeros{};
			for (std::size_t block = 0U; block < 64U; ++block)
				padding.write(zeros.data(), zeros.size());
		}
		invocation.argv = {race_target.string()};
		invocation.working_directory.clear();
		invocation.budget.wall_ms = 5000U;
		invocation.expected_binary_digest = executable_digest(race_target.string());
		std::optional<result<process_output>> race_result;
		std::atomic_bool race_finished{};
		std::thread runner{[&]
						   {
							   race_result.emplace(processes->run(invocation, {}));
							   race_finished.store(true);
						   }};
		bool source_opened{};
		const auto open_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
		while (!race_finished.load() && std::chrono::steady_clock::now() < open_deadline)
		{
			std::error_code error;
			for (const auto& entry : fs::directory_iterator{"/proc/self/fd", error})
			{
				const auto target = fs::read_symlink(entry.path(), error);
				if (!error && target == race_target)
				{
					source_opened = true;
					break;
				}
				error.clear();
			}
			if (source_opened)
				break;
			std::this_thread::yield();
		}
		if (source_opened)
		{
			fs::rename(race_target, verified_old);
			fs::copy_file("/bin/false", race_target, fs::copy_options::overwrite_existing);
		}
		runner.join();
		const bool rename_bound = source_opened && race_result && race_result->has_value() &&
			race_result->value().status == process_status::exited &&
			race_result->value().exit_code == 0 &&
			race_result->value().measured_executable_digest == invocation.expected_binary_digest;
		fs::remove_all(root);

		require(
			parent_digest && parent_digest->status == process_status::launch_failed &&
				parent_digest->failure_code == "provider.binary-identity-mismatch" &&
				parent_digest->measured_executable_digest == measured_digest && rejected_evidence &&
				parent_digest->sandbox.evidence_digest == *rejected_evidence,
			"relative executable was measured against the parent cwd instead of working directory");
		require(working_digest && working_digest->status == process_status::exited &&
					working_digest->exit_code == 1 &&
					working_digest->measured_executable_digest == measured_digest &&
					working_digest->sandbox.achieved == sandbox_assurance::enforced &&
					executed_evidence &&
					working_digest->sandbox.evidence_digest == *executed_evidence,
				"working-directory executable bytes were not measured and executed as one image");
		require(rename_bound,
				"path replacement changed the executable after its verified descriptor was opened");
#endif
	}

	void check_sandbox_closed_enum(const std::string& executable)
	{
		execution_budget minimum_budget;
		minimum_budget.wall_ms = minimum_budget.cpu_ms = minimum_budget.address_space_bytes =
			minimum_budget.transport_bytes = minimum_budget.output_bytes = minimum_budget.rows =
				minimum_budget.diagnostics = minimum_budget.open_files =
					minimum_budget.subprocesses = 1U;
		require(minimum_budget.validate().has_value(),
				"equal positive minimum execution budget was rejected");
		constexpr std::array budget_fields{
			&execution_budget::wall_ms,
			&execution_budget::cpu_ms,
			&execution_budget::address_space_bytes,
			&execution_budget::transport_bytes,
			&execution_budget::output_bytes,
			&execution_budget::rows,
			&execution_budget::diagnostics,
			&execution_budget::open_files,
			&execution_budget::subprocesses,
		};
		for (const auto field : budget_fields)
		{
			auto invalid = minimum_budget;
			invalid.*field = 0U;
			auto validation = invalid.validate();
			require(!validation && validation.error().code == "provider.task-invalid",
					"one execution budget dimension accepted zero");
		}
		constexpr std::array levels{sandbox_assurance::none,
									sandbox_assurance::best_effort,
									sandbox_assurance::enforced,
									sandbox_assurance::certified};
		for (const auto required : levels)
			for (const auto achieved : levels)
			{
				auto request = selection_request(executable);
				request.sandbox.minimum = required;
				auto value =
					candidate(executable, "success", discovery_source::explicit_path, achieved);
				value.description.sandbox_minimum = "none";
				auto selected = select_provider(request, std::span{&value, 1U});
				const bool expected =
					static_cast<std::uint8_t>(achieved) >= static_cast<std::uint8_t>(required);
				require(selected.has_value() == expected,
						"valid sandbox assurance comparison matrix diverged");
			}

		const auto policy = baseline_policy();
		execution_budget budget;
		budget.wall_ms = budget.cpu_ms = budget.address_space_bytes = budget.output_bytes =
			budget.open_files = budget.subprocesses = 1U;
		for (const auto raw : {4U, 255U})
		{
			const auto invalid = static_cast<sandbox_assurance>(raw);
			auto request = selection_request(executable);
			request.sandbox.minimum = invalid;
			auto value = candidate(executable, "success");
			auto invalid_required = select_provider(request, std::span{&value, 1U});
			require(!invalid_required &&
						invalid_required.error().code == "provider.sandbox-requirement-invalid" &&
						invalid_required.error().field == "minimum",
					"invalid required assurance weakened the selection boundary");

			request = selection_request(executable);
			value = candidate(executable, "success");
			value.sandbox.achieved = invalid;
			auto invalid_achieved = select_provider(request, std::span{&value, 1U});
			require(!invalid_achieved &&
						invalid_achieved.error().code == "provider.sandbox-report-invalid" &&
						invalid_achieved.error().field == "achieved",
					"invalid achieved assurance passed certified ordinal comparison");

			value = candidate(executable, "success");
			auto token = select_provider(request, std::span{&value, 1U});
			require(token.has_value(), "valid sandbox selection token fixture failed");
			auto& replay_candidate = const_cast<provider_candidate&>(token->selected_candidate());
			replay_candidate.sandbox.achieved = invalid;
			auto replay = token->validate();
			require(!replay && replay.error().code == "provider.sandbox-report-invalid" &&
						replay.error().field == "achieved",
					"selection token replay accepted invalid sandbox assurance");

			auto evidence = sandbox_evidence_digest(
				policy, budget, invalid, policy.mechanisms, selection_fixture_digest(executable));
			require(!evidence && evidence.error().code == "provider.sandbox-report-invalid" &&
						evidence.error().field == "achieved",
					"evidence digest canonicalized an invalid sandbox assurance");

			invalid_sandbox_port port{invalid};
			process_provider_runtime runtime{port};
			auto report = runtime.execute(task(select(executable, "success")));
			require(!report && report.error().code == "provider.sandbox-report-invalid" &&
						report.error().field == "achieved",
					"custom process port bypassed runtime sandbox enum validation");
		}
		for (const auto achieved : levels)
			require(sandbox_evidence_digest(policy,
											budget,
											achieved,
											policy.mechanisms,
											selection_fixture_digest(executable))
						.has_value(),
					"valid sandbox assurance failed evidence binding");
		auto unmeasured = sandbox_evidence_digest(
			policy, budget, sandbox_assurance::enforced, policy.mechanisms, {});
		require(!unmeasured && unmeasured.error().code == "provider.sandbox-report-invalid" &&
					unmeasured.error().field == "measured_executable_digest",
				"enforced sandbox evidence omitted the measured executable binding");
	}

	void check_host_transcript_validator(const std::string& executable)
	{
		auto process_request = task(select(executable, "success"));
		const auto& description = process_request.selection.selected_candidate().description;
		host_transcript_request host{{description.canonical_json(),
									  {process_request.task_id,
									   process_request.task_input_digest,
									   process_request.normalized_invocation_digest,
									   process_request.toolchain_digest,
									   process_request.environment_digest},
									  process_request.limits},
									 process_request.output_credit,
									 process_request.payload};
		auto encoded = encode_host_transcript(host);
		auto frames = encoded ? decode_frame_stream(*encoded, process_request.limits)
							  : result<std::vector<frame>>{unexpected(encoded.error())};
		auto validated = frames ? validate_host_transcript(*frames, host.expectation)
								: result<validated_host_transcript>{unexpected(frames.error())};
		require(validated && validated->task == host.expectation.task &&
					validated->credit.bytes == host.credit.bytes &&
					validated->credit.frames == host.credit.frames &&
					validated->payload == host.payload,
				"runtime-generated host transcript failed the shared worker validator");
		const auto rejects = [&](std::vector<frame> values)
		{
			return !validate_host_transcript(values, host.expectation);
		};
		for (std::size_t index = 0U; index < frames->size(); ++index)
		{
			auto wrong_type = *frames;
			wrong_type[index].type = index == 4U ? message_type::credit : message_type::close;
			auto wrong_sequence = *frames;
			wrong_sequence[index].sequence ^= 1U;
			auto wrong_stream = *frames;
			wrong_stream[index].stream_id = 2U;
			auto wrong_flags = *frames;
			wrong_flags[index].flags = static_cast<std::uint16_t>(frame_flag::required_extension);
			auto empty_control = *frames;
			empty_control[index].control.clear();
			require(rejects(std::move(wrong_type)) && rejects(std::move(wrong_sequence)) &&
						rejects(std::move(wrong_stream)) && rejects(std::move(wrong_flags)) &&
						rejects(std::move(empty_control)),
					"host transcript accepted a type/sequence/stream/flags/control mutation");
		}
		auto wrong_order = *frames;
		std::swap(wrong_order[1U], wrong_order[3U]);
		auto missing_close = *frames;
		missing_close.pop_back();
		auto duplicate_close = *frames;
		duplicate_close.push_back(duplicate_close.back());
		auto forbidden_payload = *frames;
		forbidden_payload[1U].payload.push_back(std::byte{1U});
		require(rejects(std::move(wrong_order)) && rejects(std::move(missing_close)) &&
					rejects(std::move(duplicate_close)) && rejects(std::move(forbidden_payload)),
				"host transcript accepted ordering/count/payload state mutation");

		for (std::size_t field = 0U; field < 5U; ++field)
		{
			auto wrong_open = *frames;
			auto metadata = host.expectation.task;
			std::array<std::string*, 5U> values{&metadata.task_id,
												&metadata.task_input_digest,
												&metadata.normalized_invocation_digest,
												&metadata.toolchain_digest,
												&metadata.environment_digest};
			*values[field] += "-mismatch";
			wrong_open[2U].control = *encode_open_task_metadata(metadata);
			require(rejects(std::move(wrong_open)), "host transcript accepted open_task mismatch");
		}
		auto wrong_payload = *frames;
		wrong_payload[2U].payload.push_back(std::byte{2U});
		auto wrong_close = *frames;
		wrong_close[4U].control = *encode_close_metadata({"other-task"});
		auto wrong_schema = *frames;
		wrong_schema[1U].control =
			*encode_schema_negotiate_metadata({"cxxlens.provider-protocol.v1", 1U});
		auto wrong_manifest = host.expectation;
		wrong_manifest.provider_manifest += " ";
		require(rejects(std::move(wrong_payload)) && rejects(std::move(wrong_close)) &&
					rejects(std::move(wrong_schema)) &&
					!validate_host_transcript(*frames, wrong_manifest),
				"host transcript accepted payload/close/schema/manifest binding mismatch");

		auto maximum_credit = *frames;
		maximum_credit[3U].control = *encode_credit_metadata(
			{std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max()});
		auto maximum = validate_host_transcript(maximum_credit, host.expectation);
		auto decimal_overflow = *frames;
		decimal_overflow[3U].control = *encode_control_text("18446744073709551616|10");
		auto extreme_decimal = *frames;
		extreme_decimal[3U].control = *encode_control_text(std::string(4096U, '9') + "|10");
		require(maximum && maximum->credit.bytes == std::numeric_limits<std::uint64_t>::max() &&
					maximum->credit.frames == std::numeric_limits<std::uint64_t>::max() &&
					rejects(std::move(decimal_overflow)) && rejects(std::move(extreme_decimal)),
				"host credit boundary did not reject decimal overflow or preserve uint64 max");

		auto chunk_manifest = description;
		chunk_manifest.protocol.maximum_minor = 1U;
		chunk_manifest.protocol.required_features = {"credit-backpressure", "task-input-chunks-v1"};
		constexpr std::size_t chunk_bytes = 1024U * 1024U;
		std::vector<std::byte> streaming_bytes;
		host_transcript_expectation streaming_expectation;
		for (const auto size :
			 std::array<std::size_t, 5U>{0U, 1U, chunk_bytes - 1U, chunk_bytes, chunk_bytes + 1U})
		{
			std::vector<std::byte> payload(size);
			for (std::size_t index{}; index < payload.size(); ++index)
				payload[index] = static_cast<std::byte>(index % 251U);
			auto expectation = host.expectation;
			expectation.provider_manifest = chunk_manifest.canonical_json();
			expectation.limits.minimum_minor = 1U;
			expectation.limits.maximum_minor = 1U;
			expectation.task.task_input_digest = content_digest(payload);
			auto wire = encode_host_transcript({expectation, host.credit, payload});
			auto decoded = wire ? decode_frame_stream(*wire, expectation.limits)
								: result<std::vector<frame>>{unexpected(wire.error())};
			auto checked = decoded ? validate_host_transcript(*decoded, expectation)
								   : result<validated_host_transcript>{unexpected(decoded.error())};
			const auto expected_chunks = size == 0U ? 0U : 1U + ((size - 1U) / chunk_bytes);
			require(checked && checked->payload == payload &&
						decoded->size() == 6U + expected_chunks &&
						decoded->at(2U).type == message_type::open_task &&
						decoded->at(2U).payload.empty() &&
						decoded->at(3U).type == message_type::input_descriptor &&
						decoded->at(4U + expected_chunks).type == message_type::credit &&
						decoded->at(5U + expected_chunks).type == message_type::close,
					"minor-1 host input boundary failed exact descriptor/chunk shape");
			if (size == chunk_bytes + 1U)
			{
				streaming_bytes = std::move(*wire);
				streaming_expectation = std::move(expectation);
				auto missing = *decoded;
				missing.erase(missing.begin() + 4);
				auto duplicate = *decoded;
				duplicate.insert(duplicate.begin() + 5, duplicate.at(4U));
				auto reordered = *decoded;
				std::swap(reordered.at(4U), reordered.at(5U));
				auto digest_mismatch = *decoded;
				digest_mismatch.at(4U).payload.front() ^= std::byte{1U};
				auto descriptor_mismatch = *decoded;
				descriptor_mismatch.at(3U).control.clear();
				auto extra = *decoded;
				auto extra_frame = extra.back();
				extra_frame.sequence += 1U;
				extra.push_back(std::move(extra_frame));
				require(!validate_host_transcript(missing, streaming_expectation) &&
							!validate_host_transcript(duplicate, streaming_expectation) &&
							!validate_host_transcript(reordered, streaming_expectation) &&
							!validate_host_transcript(digest_mismatch, streaming_expectation) &&
							!validate_host_transcript(descriptor_mismatch, streaming_expectation) &&
							!validate_host_transcript(extra, streaming_expectation),
						"minor-1 host input accepted "
						"missing/duplicate/reordered/digest/descriptor/extra");
			}
		}

		class short_read_source final : public detail::host_input_byte_source
		{
		  public:
			explicit short_read_source(const std::span<const std::byte> bytes) : bytes_{bytes} {}
			result<std::size_t> read(const std::span<std::byte> output) override
			{
				const auto count = std::min<std::size_t>(
					{output.size(), bytes_.size() - offset_, std::size_t{4093U}});
				std::ranges::copy(bytes_.subspan(offset_, count), output.begin());
				offset_ += count;
				return count;
			}

		  private:
			std::span<const std::byte> bytes_;
			std::size_t offset_{};
		} short_reads{streaming_bytes};
		class collecting_input_sink final : public detail::host_input_chunk_sink
		{
		  public:
			result<void> append(const std::span<const std::byte> bytes) override
			{
				bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
				return {};
			}
			std::vector<std::byte> bytes_;
		} streamed_input;
		auto streamed_seal = detail::validate_host_transcript_stream(
			short_reads, {streaming_expectation, true}, streamed_input);
		require(
			streamed_seal && streamed_seal->protocol_minor() == 1U &&
				streamed_seal->total_bytes() == chunk_bytes + 1U &&
				streamed_seal->chunk_bytes() == chunk_bytes && streamed_seal->chunk_count() == 2U &&
				streamed_seal->ordered_chunk_digests().size() == 2U &&
				streamed_input.bytes_.size() == chunk_bytes + 1U &&
				streamed_seal->ordered_chunk_digest_set_digest().starts_with("semantic-v2:sha256:"),
			"arbitrary-short-read host stream did not produce the exact immutable input seal");

		class oversized_input final : public detail::replayable_host_input
		{
		  public:
			result<std::uint64_t> size() const override
			{
				return 64U * 1024U * 1024U + 1U;
			}
			result<std::size_t> read_at(std::uint64_t, std::span<std::byte>) const override
			{
				return cxxlens::sdk::unexpected(error{"sdk.test-unexpected-read", "input", {}});
			}
		} oversized;
		class discard_frames final : public frame_sink
		{
		  public:
			result<void> write(std::span<const std::byte>) override
			{
				return {};
			}
		} discarded;
		auto oversized_result = detail::encode_host_transcript_incremental(
			{streaming_expectation, true}, host.credit, oversized, discarded);
		require(!oversized_result &&
					oversized_result.error().code == "provider.host-transcript-invalid" &&
					oversized_result.error().detail == "input-limit",
				"minor-1 logical input limit did not fail before reading or allocation");
	}

	void check_sealed_provider_validation()
	{
		sealed_parity_provider provider;
		const auto& descriptor = cxxlens::company::relations::lock_acquire::descriptor();
		const auto& resultless_descriptor =
			cxxlens::cc::relations::call_direct_target::descriptor();
		auto catalog = project_catalog::make(
			".",
			"sha256:3333333333333333333333333333333333333333333333333333333333333333",
			{{"unit.cpp",
			  "sha256:1111111111111111111111111111111111111111111111111111111111111111",
			  std::string{binary_digest},
			  "sha256:3333333333333333333333333333333333333333333333333333333333333333"}});
		require(catalog.has_value(), "sealed validator catalog setup failed");
		auto task_value =
			cxxlens::sdk::provider::task::make({std::string{provider.id()},
												provider.version(),
												std::string{provider.semantic_contract_digest()},
												{descriptor, resultless_descriptor},
												{},
												{"company.test.canonical-1"},
												"observation",
												"assertion"},
											   std::move(*catalog),
											   {descriptor, resultless_descriptor},
											   "condition:all",
											   "company.test.canonical-1",
											   {"dependency-1", "dependency-2", "dependency-3"});
		require(task_value.has_value(), "sealed validator task setup failed");
		const auto task = std::move(*task_value);

		transcript_sink sink;
		protocol_writer writer{sink};
		const protocol_credit credit{64U * 1024U * 1024U, 65536U};
		writer.grant_credit(credit);
		execution_context execution;
		execution.budget.output_bytes = 64U * 1024U * 1024U;
		execution.budget.rows = 4U;
		execution.budget.diagnostics = 16U;
		require(run_worker(provider, task, writer, execution).has_value(),
				"sealed validator provider run failed");
		auto frames = decode_frame_stream(sink.transcript);
		require(frames.has_value(), "sealed validator transcript decode failed");
		const cxxlens::sdk::provider::detail::transcript_validation_request request{
			task.task_id,
			std::string{provider.id()},
			provider.version(),
			nullptr,
			task.outputs,
			credit,
			&execution.budget,
			false,
		};
		const auto validate = [&](const std::vector<frame>& candidate)
		{
			return cxxlens::sdk::provider::detail::validate_provider_transcript(
				request, candidate, protocol_limits{});
		};

		auto positive = validate(*frames);
		require(positive &&
					positive->kind ==
						cxxlens::sdk::provider::detail::transcript_terminal_kind::complete &&
					positive->reason == "provider.success" && positive->sealed() &&
					!positive->sealing_error(),
				"valid transcript did not produce an adoption seal");
		detail::provider_runtime_provenance forged_multi_batch_provenance;
		forged_multi_batch_provenance.task_id = task.task_id;
		forged_multi_batch_provenance.dependency_group_id = "dependency:forged";
		forged_multi_batch_provenance.atomic_output_group_id = "atomic:forged";
		forged_multi_batch_provenance.batch_id = "batch:forged";
		auto forged_multi_batch_receipt =
			detail::make_provider_runtime_receipt(1U,
												  "sha256:" + std::string(64U, 'a'),
												  *frames,
												  std::move(forged_multi_batch_provenance),
												  "provider.success",
												  *positive->sealed());
		require(!forged_multi_batch_receipt &&
					forged_multi_batch_receipt.error().detail == "multi-batch-provenance",
				"runtime receipt admitted caller provenance for multiple sealed batches");
		const auto batches = positive->sealed()->batches();
		require(batches.size() == 3U && batches[0U].task_id() == task.task_id &&
					batches[0U].descriptor_id() == descriptor.id &&
					batches[0U].descriptor_digest() == descriptor.descriptor_digest &&
					batches[0U].dependency_group_id() == "dependency-1" &&
					batches[0U].atomic_output_group_id() == "atomic-1" &&
					batches[0U].batch_id() == "batch-1" && !batches[0U].batch_digest().empty() &&
					batches[0U].rows().size() == 2U &&
					batches[0U].ordered_chunk_digests().size() == descriptor.columns.size(),
				"sealed positive batch lost exact binding or reconstructed rows");
		for (const auto& row : batches[0U].rows())
			require(validate_row(descriptor, row).has_value() &&
						validate_domain_identity(descriptor, row).has_value(),
					"sealed row was not SDK-valid");
		require(batches[1U].dependency_group_id() == "dependency-2" &&
					batches[1U].atomic_output_group_id() == "atomic-2" &&
					batches[1U].batch_id() == "batch-2" && batches[1U].rows().empty() &&
					batches[1U].ordered_chunk_digests().empty(),
				"zero-row batch was omitted or retained nonempty leaves");
		require(batches[2U].descriptor_id() == resultless_descriptor.id &&
					batches[2U].descriptor_digest() == resultless_descriptor.descriptor_digest &&
					batches[2U].dependency_group_id() == "dependency-3" &&
					batches[2U].atomic_output_group_id() == "atomic-3" &&
					batches[2U].batch_id() == "batch-3" && batches[2U].rows().size() == 1U &&
					batches[2U].ordered_chunk_digests().size() ==
						resultless_descriptor.columns.size() &&
					validate_row(resultless_descriptor, batches[2U].rows().front()).has_value() &&
					!resultless_descriptor.domain_identity.result_column,
				"nonzero resultless descriptor batch did not produce a strict adoption seal");
		require(positive->sealed()->coverage().size() == 1U &&
					positive->sealed()->coverage().front().id == task.task_id &&
					positive->sealed()->unresolved().empty() &&
					positive->sealed()->evidence().empty(),
				"decoded side channels were not retained exactly");

		auto failed_terminal = *frames;
		failed_terminal.back().type = message_type::task_failed;
		failed_terminal.back().control =
			*encode_task_failed_metadata({"provider.cancelled", task.task_id, "test"});
		failed_terminal.back().payload.clear();
		auto failed_verdict = validate(failed_terminal);
		require(failed_verdict &&
					failed_verdict->kind ==
						cxxlens::sdk::provider::detail::transcript_terminal_kind::failed &&
					failed_verdict->reason == "provider.cancelled" && !failed_verdict->sealed(),
				"failed task terminal retained an adoption seal");

		const auto rebind_first_identity_cell = [&](detached_cell replacement)
		{
			auto mutated = *frames;
			const auto first_chunk =
				std::ranges::find(mutated, message_type::column_chunk, &frame::type);
			require(first_chunk != mutated.end(), "sealed transcript has no column chunk");
			const auto first_chunk_index = static_cast<std::size_t>(first_chunk - mutated.begin());
			auto identity_chunk = decode_column_chunk(
				first_chunk->control, first_chunk->payload, descriptor.columns.front());
			require(identity_chunk && identity_chunk->cells.size() == 2U,
					"sealed identity chunk decode failed");
			identity_chunk->cells.front() = std::move(replacement);
			identity_chunk->chunk_digest.clear();
			auto encoded_identity_chunk =
				encode_column_chunk(*identity_chunk, descriptor.columns.front());
			require(encoded_identity_chunk.has_value(), "sealed identity chunk mutation failed");
			const auto old_identity_payload_bytes = first_chunk->payload.size();
			first_chunk->control = encoded_identity_chunk->control;
			first_chunk->payload = encoded_identity_chunk->payload;
			const auto first_end =
				std::ranges::find(mutated | std::views::drop(first_chunk_index + 1U),
								  message_type::batch_end,
								  &frame::type);
			require(first_end != mutated.end(), "sealed transcript has no batch terminal");
			auto identity_terminal =
				decode_columnar_batch_end(first_end->control, first_end->payload);
			require(identity_terminal.has_value(), "sealed batch terminal decode failed");
			auto& identity_summary = identity_terminal->columns.front();
			require(identity_summary.payload_bytes >= old_identity_payload_bytes,
					"sealed batch summary underflow");
			identity_summary.payload_bytes -= old_identity_payload_bytes;
			identity_summary.payload_bytes += encoded_identity_chunk->payload.size();
			identity_terminal->ordered_chunk_digests.front() = encoded_identity_chunk->chunk_digest;
			identity_terminal->batch_digest = columnar_batch_digest(*identity_terminal);
			auto encoded_identity_terminal = encode_columnar_batch_end(*identity_terminal);
			require(encoded_identity_terminal.has_value(), "sealed batch terminal mutation failed");
			first_end->control = std::move(encoded_identity_terminal->control);
			first_end->payload = std::move(encoded_identity_terminal->payload);
			return mutated;
		};

		auto identity_omission = rebind_first_identity_cell(
			detached_cell::unknown(descriptor.columns.front().type, "provider omitted result"));
		auto identity_missing = validate(identity_omission);
		require(
			identity_missing &&
				identity_missing->kind ==
					cxxlens::sdk::provider::detail::transcript_terminal_kind::complete &&
				identity_missing->reason == "provider.success" && !identity_missing->sealed() &&
				identity_missing->sealing_error() &&
				identity_missing->sealing_error()->code == "sdk.domain-identity-missing",
			"missing result-bearing identity produced an adoption seal or changed public verdict");

		auto identity_mutation = rebind_first_identity_cell(
			detached_cell::typed("company_lock_acquire_id", "company_lock_acquire_id:invalid"));
		auto identity_rejected = validate(identity_mutation);
		require(identity_rejected &&
					identity_rejected->kind ==
						cxxlens::sdk::provider::detail::transcript_terminal_kind::complete &&
					identity_rejected->reason == "provider.success" &&
					!identity_rejected->sealed() && identity_rejected->sealing_error() &&
					identity_rejected->sealing_error()->code == "sdk.domain-identity-mismatch",
				"domain identity mismatch produced an adoption seal or changed public verdict");

		auto count_mutation = *frames;
		const auto first_count_chunk =
			std::ranges::find(count_mutation, message_type::column_chunk, &frame::type);
		require(first_count_chunk != count_mutation.end(),
				"sealed transcript has no first column chunk");
		const auto first_chunk_index =
			static_cast<std::size_t>(first_count_chunk - count_mutation.begin());
		const auto second_chunk =
			std::ranges::find(count_mutation | std::views::drop(first_chunk_index + 1U),
							  message_type::column_chunk,
							  &frame::type);
		require(second_chunk != count_mutation.end(),
				"sealed transcript has no second column chunk");
		auto shortened = decode_column_chunk(
			second_chunk->control, second_chunk->payload, descriptor.columns.at(1U));
		require(shortened && shortened->cells.size() == 2U,
				"sealed second column chunk decode failed");
		shortened->cells.pop_back();
		shortened->row_count = 1U;
		shortened->chunk_digest.clear();
		auto encoded_shortened = encode_column_chunk(*shortened, descriptor.columns.at(1U));
		require(encoded_shortened.has_value(), "sealed short column mutation failed");
		second_chunk->control = std::move(encoded_shortened->control);
		second_chunk->payload = std::move(encoded_shortened->payload);
		auto count_rejected = validate(count_mutation);
		require(!count_rejected && count_rejected.error().code == "provider.batch-invalid" &&
					count_rejected.error().detail == "chunk-binding",
				"cross-column row count mismatch reached an adoption seal");
	}

	void check_process_faults(const std::string& executable, const bool receipt_only = false)
	{
		auto processes = make_system_provider_process_port();
		require(processes != nullptr, "system provider process port unavailable");
		process_provider_runtime runtime{*processes};
		auto forged = runtime.execute(task(provider_selection{}));
		require(!forged && forged.error().code == "provider.selection-invalid",
				"default/forged selection token reached process launch");

		std::array<int, 2U> inherited_descriptors{-1, -1};
		if (!receipt_only)
		{
			require(::socketpair(AF_UNIX, SOCK_STREAM, 0, inherited_descriptors.data()) == 0,
					"parent non-CLOEXEC socket fixture failed");
			for (const auto mode : {"success", "network-check", "fd-clean"})
			{
				auto request = task(select(executable, mode));
				if (std::string_view{mode} == "success")
					request.task_id = "task|delimiter-雪";
				auto report = runtime.execute(request);
				cxxlens::sdk::provider::task reference_task;
				reference_task.task_id = request.task_id;
				reference_task.outputs = request.output_descriptors;
				auto reference = report
					? cxxlens::sdk::testing::validate_process_transcript(reference_task,
																		 report->provider,
																		 report->frames,
																		 request.output_credit,
																		 request.limits)
					: cxxlens::sdk::result<cxxlens::sdk::testing::conformance_report>{
						  cxxlens::sdk::unexpected(
							  cxxlens::sdk::error{"sdk.test-setup", "process-report", {}})};
				auto applied_policy = report
					? resolve_sandbox_policy(report->sandbox.policy_digest)
					: result<sandbox_policy>{unexpected(error{"sdk.test-setup", "sandbox", {}})};
				auto evidence = report && applied_policy
					? sandbox_evidence_digest(*applied_policy,
											  request.budget,
											  report->sandbox.achieved,
											  report->sandbox.mechanisms,
											  report->provider.provider_binary_digest)
					: result<std::string>{unexpected(error{"sdk.test-setup", "evidence", {}})};
				std::string failure_detail{"successful process provider failed: "};
				failure_detail += mode;
				failure_detail += " terminal=";
				failure_detail += report ? report->terminal : report.error().code;
				if (report)
				{
					failure_detail += " exit=" + std::to_string(report->exit_code);
					failure_detail += " signal=" + std::to_string(report->termination_signal);
					for (const auto& diagnostic : report->diagnostics)
						failure_detail += " [" + diagnostic.code + ":" + diagnostic.detail + "]";
				}
				require(
					report && report->succeeded() &&
						report->measured_executable_digest ==
							report->provider.provider_binary_digest &&
						report->frames.front().type == message_type::hello &&
						report->frames.size() == 15U &&
						report->frames.at(1U).type == message_type::schema_negotiate &&
						report->frames.at(2U).type == message_type::task_accepted &&
						report->frames.at(3U).type == message_type::batch_begin &&
						report->frames.at(10U).type == message_type::batch_end &&
						report->frames.at(11U).type == message_type::coverage_chunk &&
						report->frames.back().type == message_type::task_complete &&
						report->sandbox.achieved == sandbox_assurance::enforced &&
						report->sandbox.policy_digest ==
							request.selection.authority_request().sandbox.policy_digest &&
						applied_policy &&
						report->sandbox.mechanisms == applied_policy->mechanisms && evidence &&
						report->sandbox.evidence_digest == *evidence &&
						report->canonical_form().contains("cxxlens.provider-execution-report.v1") &&
						reference && reference->accepted,
					failure_detail);
			}
		}

		auto receipt_candidate = candidate(executable, "success");
		receipt_candidate.description.protocol.maximum_minor = 1U;
		receipt_candidate.description.protocol.required_features = {"credit-backpressure",
																	"task-input-chunks-v1"};
		auto receipt_selection =
			select_provider(selection_request(executable), std::span{&receipt_candidate, 1U});
		require(receipt_selection.has_value(), "runtime receipt provider selection failed");
		auto receipt_request = task(std::move(*receipt_selection));
		receipt_request.limits.minimum_minor = 1U;
		receipt_request.limits.maximum_minor = 1U;
		auto sealed_execution = detail::execute_provider_process(*processes, receipt_request);
		std::string receipt_failure{
			"successful process did not retain input/output seals, identity, and runtime receipt"};
		if (sealed_execution)
		{
			receipt_failure += " terminal=" + sealed_execution->terminal;
			receipt_failure += " exit=" + std::to_string(sealed_execution->exit_code);
			receipt_failure += " signal=" + std::to_string(sealed_execution->termination_signal);
			for (const auto& diagnostic : sealed_execution->diagnostics)
				receipt_failure += " [" + diagnostic.code + ":" + diagnostic.detail + "]";
		}
		else
			receipt_failure += " error=" + sealed_execution.error().code;
		require(sealed_execution && sealed_execution->succeeded() && sealed_execution->input_seal &&
					sealed_execution->sealed && sealed_execution->provider_identity &&
					sealed_execution->runtime_receipt,
				receipt_failure);
		auto runtime_binding =
			detail::validate_provider_process_runtime_binding(*sealed_execution, receipt_request);
		require(runtime_binding.has_value(),
				"accepted process task was not bound to its sealed runtime receipt");
		auto stale_input_request = receipt_request;
		stale_input_request.task_input_digest =
			"sha256:9999999999999999999999999999999999999999999999999999999999999999";
		auto stale_input = detail::validate_provider_process_runtime_binding(*sealed_execution,
																			 stale_input_request);
		require(!stale_input && stale_input.error().code == "provider.task-binding-mismatch" &&
					stale_input.error().field == "task_input_digest",
				"stale task input digest reached the runtime handoff");
		auto mutated_raw_observation = *sealed_execution;
		mutated_raw_observation.raw_frame_stream.back() ^= std::byte{1U};
		auto mutated_raw = detail::validate_provider_process_runtime_binding(
			mutated_raw_observation, receipt_request);
		require(!mutated_raw && mutated_raw.error().code == "provider.task-binding-mismatch" &&
					mutated_raw.error().field == "runtime_receipt.raw_observation",
				"mutated raw provider output reached the runtime handoff");
		auto missing_runtime_receipt = *sealed_execution;
		missing_runtime_receipt.runtime_receipt.reset();
		require(!missing_runtime_receipt.succeeded(),
				"successful transcript without runtime receipt escaped the adoption boundary");
		const auto& input_seal = *sealed_execution->input_seal;
		const auto& sealed_identity = *sealed_execution->provider_identity;
		const auto& receipt = *sealed_execution->runtime_receipt;
		require(input_seal.protocol_major() == 1U && input_seal.protocol_minor() == 1U &&
					input_seal.total_bytes() == receipt_request.payload.size() &&
					input_seal.chunk_bytes() == 1024U * 1024U && input_seal.chunk_count() == 1U &&
					input_seal.task().task_input_digest == receipt_request.task_input_digest &&
					sealed_identity.provider_id == receipt_candidate.description.provider_id &&
					sealed_identity.provider_binary_digest ==
						receipt_candidate.description.provider_binary_digest &&
					sealed_identity.protocol_minor == 1U &&
					std::ranges::binary_search(sealed_identity.required_features,
											   "task-input-chunks-v1") &&
					sealed_identity.offered_relations ==
						receipt_candidate.description.offered_relations,
				"sealed input or independent provider identity lost an exact authority binding");
		std::vector<std::byte> raw_stdout;
		for (const auto& value : sealed_execution->frames)
		{
			auto encoded_frame = encode_frame(value, receipt_request.limits);
			require(encoded_frame.has_value(), "receipt frame did not canonically re-encode");
			raw_stdout.insert(raw_stdout.end(), encoded_frame->begin(), encoded_frame->end());
		}
		require(receipt.raw_stdout_byte_count() == raw_stdout.size() &&
					receipt.raw_stdout_sha256() == content_digest(raw_stdout) &&
					receipt.decoded_frame_count() == sealed_execution->frames.size() &&
					receipt.frame_transcript_digest().starts_with("semantic-v2:sha256:") &&
					receipt.sealed_transcript_digest().starts_with("semantic-v2:sha256:") &&
					receipt.frame_transcript_digest() != receipt.sealed_transcript_digest(),
				"runtime receipt did not bind exact raw bytes, decoded count, and distinct typed "
				"seals");
		auto corrupt_stdout = raw_stdout;
		corrupt_stdout.back() ^= std::byte{1U};
		auto corrupt_decode = decode_frame_stream(corrupt_stdout, receipt_request.limits);
		require(!corrupt_decode && corrupt_decode.error().code == "provider.checksum-mismatch",
				"one-byte raw stdout mutation survived frame checksum validation");
		auto wrong_identity = sealed_identity;
		wrong_identity.provider_binary_digest =
			"sha256:9999999999999999999999999999999999999999999999999999999999999999";
		const detail::transcript_validation_request wrong_identity_validation{
			receipt_request.task_id,
			receipt_candidate.description.provider_id,
			receipt_candidate.description.provider_version,
			&receipt_candidate.description,
			receipt_request.output_descriptors,
			receipt_request.output_credit,
			&receipt_request.budget,
			true,
			&wrong_identity,
		};
		auto identity_rejected = detail::validate_provider_transcript(
			wrong_identity_validation, sealed_execution->frames, receipt_request.limits);
		require(!identity_rejected &&
					identity_rejected.error().code == "provider.binary-identity-mismatch",
				"provider stdout self-consistency overrode the independent selected identity");
		auto public_receipt_report = runtime.execute(receipt_request);
		require(public_receipt_report && public_receipt_report->succeeded() &&
					public_receipt_report->semantic_digest() != receipt.frame_transcript_digest() &&
					public_receipt_report->semantic_digest() != receipt.sealed_transcript_digest(),
				"public process report semantic digest aliased runtime-private receipt authority");

		class replay_source final : public detail::replayable_host_input
		{
		  public:
			explicit replay_source(const std::span<const std::byte> bytes) : bytes_{bytes} {}
			result<std::uint64_t> size() const override
			{
				return bytes_.size();
			}
			result<std::size_t> read_at(const std::uint64_t offset,
										const std::span<std::byte> output) const override
			{
				if (offset > bytes_.size())
					return cxxlens::sdk::unexpected(error{"sdk.test-read", "offset", {}});
				const auto count =
					std::min<std::size_t>({output.size(), bytes_.size() - offset, std::size_t{2U}});
				std::ranges::copy(bytes_.subspan(static_cast<std::size_t>(offset), count),
								  output.begin());
				return count;
			}

		  private:
			std::span<const std::byte> bytes_;
		};
		const auto replay_payload = receipt_request.payload;
		replay_source replay_input{replay_payload};
		auto replay_request = receipt_request;
		replay_request.payload.clear();
		auto replay_port = detail::make_system_replayable_provider_process_port();
		require(replay_port != nullptr, "system replayable provider process port unavailable");
		auto replayed =
			detail::execute_provider_process_replayable(*replay_port, replay_request, replay_input);
		require(
			replayed && replayed->succeeded() && replayed->input_seal &&
				replayed->input_seal->task().task_input_digest ==
					receipt_request.task_input_digest &&
				replayed->input_seal->total_bytes() == replay_payload.size() &&
				replayed->runtime_receipt && replayed->provider_identity,
			"replayable process execution fell back to request.payload or lost sealed receipts");
		(void)::close(inherited_descriptors[0]);
		(void)::close(inherited_descriptors[1]);
		if (receipt_only)
			return;

		auto policies = builtin_sandbox_policies();
		const auto& strict_policy = policies.back();
		auto strict_candidate = candidate(executable, "success");
		strict_candidate.sandbox = make_sandbox(strict_policy, sandbox_assurance::enforced);
		auto strict_authority = selection_request(executable);
		strict_authority.sandbox.policy_digest = strict_policy.policy_digest();
		auto strict_selection = select_provider(strict_authority, std::span{&strict_candidate, 1U});
		require(strict_selection.has_value(), "strict sandbox policy selection failed");
		auto strict_request = task(std::move(*strict_selection));
		strict_request.sandbox.policy_digest = strict_policy.policy_digest();
		auto strict_report = runtime.execute(strict_request);
		require(strict_report && strict_report->succeeded() &&
					strict_report->sandbox.mechanisms == strict_policy.mechanisms &&
					strict_report->sandbox.mechanisms != baseline_policy().mechanisms,
				"distinct built-in sandbox policies applied the same mechanism plan");

		auto install_failure_request = task(select(executable, "success"));
		install_failure_request.budget.open_files = std::numeric_limits<std::uint64_t>::max();
		auto install_failure = runtime.execute(install_failure_request);
		require(install_failure && install_failure->terminal == "security.sandbox-insufficient" &&
					install_failure->sandbox.achieved == sandbox_assurance::none &&
					!install_failure->succeeded(),
				"failed sandbox mechanism installation reported enforced assurance");

		auto manifest_minimum_candidate = candidate(executable, "success");
		auto weaker_authority = selection_request(executable);
		weaker_authority.sandbox.minimum = sandbox_assurance::best_effort;
		auto manifest_minimum_selection =
			select_provider(weaker_authority, std::span{&manifest_minimum_candidate, 1U});
		require(manifest_minimum_selection.has_value(),
				"manifest-minimum provider selection failed");
		auto weakened_request = task(std::move(*manifest_minimum_selection));
		weakened_request.sandbox.minimum = sandbox_assurance::none;
		auto enforced = runtime.execute(weakened_request);
		require(enforced && enforced->succeeded() &&
					enforced->sandbox.achieved == sandbox_assurance::enforced,
				"runtime did not enforce max(selection, request, manifest) sandbox minimum");
		auto mismatched_policy = task(select(executable, "success"));
		mismatched_policy.sandbox.policy_digest =
			"sha256:9999999999999999999999999999999999999999999999999999999999999999";
		auto policy_rejected = runtime.execute(mismatched_policy);
		require(!policy_rejected &&
					policy_rejected.error().code == "security.sandbox-policy-mismatch",
				"runtime accepted a sandbox policy not bound by selection authority");

		auto optional = runtime.execute(task(select(executable, "optional-extension")));
		require(optional && optional->succeeded() && optional->frames.size() == 16U &&
					optional->frames.at(3U).flags ==
						static_cast<std::uint16_t>(frame_flag::optional_extension) &&
					static_cast<std::uint16_t>(optional->frames.at(3U).type) == 65000U,
				"unknown optional extension was not skipped with accounting evidence");
		auto optional_credit_request = task(select(executable, "optional-extension"));
		optional_credit_request.output_credit.frames = 15U;
		auto optional_credit = runtime.execute(optional_credit_request);
		require(optional_credit && optional_credit->terminal == "provider.credit-exceeded",
				"skipped optional extension was omitted from frame credit accounting");

		auto minor_candidate = candidate(executable, "success");
		minor_candidate.description.protocol.maximum_minor = 1U;
		minor_candidate.description.protocol.required_features = {"credit-backpressure",
																  "task-input-chunks-v1"};
		auto minor_selection =
			select_provider(selection_request(executable), std::span{&minor_candidate, 1U});
		require(minor_selection.has_value(), "minor-capable provider selection failed");
		auto minor_request = task(std::move(*minor_selection));
		minor_request.limits.maximum_minor = 1U;
		auto negotiated_minor = runtime.execute(minor_request);
		require(
			negotiated_minor && negotiated_minor->succeeded() &&
				std::ranges::all_of(negotiated_minor->frames,
									[](const frame& value)
									{
										return value.protocol_minor == 1U;
									}),
			std::string{"session did not bind frames to the negotiated protocol minor: "} +
				(negotiated_minor ? negotiated_minor->terminal : negotiated_minor.error().code) +
				(negotiated_minor && !negotiated_minor->diagnostics.empty()
					 ? ":" + negotiated_minor->diagnostics.back().detail
					 : std::string{}));

		auto missing_chunk_feature = candidate(executable, "success");
		missing_chunk_feature.description.protocol.minimum_minor = 1U;
		missing_chunk_feature.description.protocol.maximum_minor = 1U;
		auto missing_chunk_selection =
			select_provider(selection_request(executable), std::span{&missing_chunk_feature, 1U});
		require(missing_chunk_selection.has_value(), "minor-1 missing-feature selection failed");
		auto missing_chunk_request = task(std::move(*missing_chunk_selection));
		missing_chunk_request.limits.minimum_minor = 1U;
		missing_chunk_request.limits.maximum_minor = 1U;
		auto missing_chunk = runtime.execute(missing_chunk_request);
		require(!missing_chunk && missing_chunk.error().code == "provider.required-feature-missing",
				"protocol minor 1 was activated without task-input-chunks-v1");

		auto plain_transcript = runtime.execute(task(select(executable, "success")));
		auto eos_transcript = runtime.execute(task(select(executable, "success-eos")));
		require(plain_transcript && eos_transcript && plain_transcript->succeeded() &&
					eos_transcript->succeeded() &&
					eos_transcript->frames.back().flags ==
						static_cast<std::uint16_t>(frame_flag::end_of_stream) &&
					plain_transcript->semantic_digest() != eos_transcript->semantic_digest(),
				"frame flags were omitted from semantic transcript identity");

		auto failed = runtime.execute(task(select(executable, "failed")));
		require(failed && failed->terminal == "provider.schema-invalid" &&
					failed->frames.back().type == message_type::task_failed && !failed->succeeded(),
				"provider task failure lost its structured terminal");
		auto forged_success = runtime.execute(task(select(executable, "failure-success")));
		require(forged_success && forged_success->terminal == "provider.schema-invalid" &&
					forged_success->frames.back().type == message_type::task_failed &&
					!forged_success->succeeded(),
				"task_failed forged a successful process report");
		auto unknown_failure = runtime.execute(task(select(executable, "failure-unknown")));
		require(unknown_failure && unknown_failure->terminal == "provider.schema-invalid" &&
					unknown_failure->frames.back().type == message_type::task_failed &&
					!unknown_failure->succeeded(),
				"unregistered task failure escaped the terminal registry");
		auto forged_report = *failed;
		forged_report.terminal = "provider.success";
		require(!forged_report.succeeded(),
				"raw terminal text overrode the validated report terminal state");

		auto crash = runtime.execute(task(select(executable, "crash")));
		require(crash && crash->terminal == "provider.crash" &&
					crash->termination_signal == SIGSEGV,
				"worker crash was not distinguished");

		auto timeout_request = task(select(executable, "timeout"));
		timeout_request.budget.wall_ms = 25U;
		auto timeout = runtime.execute(timeout_request);
		require(timeout && timeout->terminal == "provider.timeout",
				"worker timeout was not distinguished");

		auto cancelled_request = task(select(executable, "timeout"));
		std::stop_source cancelled;
		cancelled.request_stop();
		cancelled_request.cancellation = cancelled.get_token();
		auto cancellation = runtime.execute(cancelled_request);
		require(cancellation && cancellation->terminal == "provider.cancelled",
				"worker cancellation was not distinguished");

		auto malformed = runtime.execute(task(select(executable, "malformed")));
		require(malformed && malformed->terminal == "provider.truncated-stream",
				"malformed worker output was not distinguished");

		auto exact_transport_request = task(select(executable, "output-limit"));
		exact_transport_request.budget.transport_bytes = 1024U * 1024U;
		auto exact_transport = runtime.execute(exact_transport_request);
		require(exact_transport && exact_transport->terminal != "provider.output-limit",
				"exact process transport byte budget was rejected");
		auto limited_request = task(select(executable, "output-limit"));
		limited_request.budget.transport_bytes = 1024U * 1024U - 1U;
		auto limited = runtime.execute(limited_request);
		require(limited && limited->terminal == "provider.output-limit",
				"worker output limit was not distinguished");

		auto identity = runtime.execute(task(select(executable, "wrong-identity")));
		require(identity && identity->terminal == "provider.binary-identity-mismatch",
				"worker identity mismatch was accepted");

		for (const auto& [mode, terminal] : std::array{
				 std::pair{"minimal", "provider.protocol-state-invalid"},
				 std::pair{"provider-credit", "provider.protocol-state-invalid"},
				 std::pair{"provider-open-task", "provider.protocol-state-invalid"},
				 std::pair{"provider-batch-ack", "provider.protocol-state-invalid"},
				 std::pair{"missing-accepted", "provider.protocol-state-invalid"},
				 std::pair{"wrong-task", "provider.task-binding-mismatch"},
				 std::pair{"wrong-complete-task", "provider.protocol-state-invalid"},
				 std::pair{"missing-complete-control", "provider.protocol-state-invalid"},
				 std::pair{"unsealed-batch", "provider.protocol-state-invalid"},
				 std::pair{"inconsistent-batch", "provider.batch-invalid"},
				 std::pair{"bad-column", "provider.batch-invalid"},
				 std::pair{"column-length-mismatch", "provider.batch-invalid"},
				 std::pair{"reordered-column", "provider.batch-invalid"},
				 std::pair{"unknown-descriptor", "provider.relation-incompatible"},
				 std::pair{"incomplete-coverage", "provider.coverage-incomplete"},
				 std::pair{"bad-eos", "provider.protocol-state-invalid"},
				 std::pair{"invalid-utf8", "provider.protocol-state-invalid"},
				 std::pair{"nul-control", "provider.protocol-state-invalid"},
			 })
		{
			auto invalid_request = task(select(executable, mode));
			auto rejected = runtime.execute(invalid_request);
			require(rejected && rejected->terminal == terminal && !rejected->succeeded(),
					std::string{"invalid provider transcript was accepted: "} + mode);
			if (std::string_view{mode} == "bad-column")
			{
				cxxlens::sdk::provider::task reference_task;
				reference_task.task_id = invalid_request.task_id;
				reference_task.outputs = invalid_request.output_descriptors;
				auto reference = cxxlens::sdk::testing::validate_process_transcript(
					reference_task,
					rejected->provider,
					rejected->frames,
					invalid_request.output_credit,
					invalid_request.limits);
				require(reference && !reference->accepted &&
							reference->reason_code == rejected->terminal,
						"process runtime and public reference validator reason diverged");
			}
		}

		auto credit_request = task(select(executable, "success"));
		credit_request.output_credit.frames = 1U;
		auto credit = runtime.execute(credit_request);
		require(credit && credit->terminal == "provider.credit-exceeded" && !credit->succeeded(),
				"provider output exceeding granted frame credit was accepted");

		auto feature_candidate = candidate(executable, "success");
		feature_candidate.description.protocol.required_features = {"company.unsupported-feature"};
		auto feature_selection =
			select_provider(selection_request(executable), std::span{&feature_candidate, 1U});
		require(feature_selection.has_value(), "feature provider selection failed");
		auto feature_request = task(std::move(*feature_selection));
		auto feature = runtime.execute(feature_request);
		require(!feature && feature.error().code == "provider.required-feature-missing",
				"unsupported required provider feature was negotiated");

		transcript_sink sink;
		protocol_writer writer{sink};
		writer.grant_credit({64U * 1024U * 1024U, 65536U});
		parity_provider provider;
		const auto& descriptor = cxxlens::company::relations::lock_acquire::descriptor();
		auto logical_catalog = project_catalog::make(
			".",
			"sha256:3333333333333333333333333333333333333333333333333333333333333333",
			{{"unit.cpp",
			  "sha256:1111111111111111111111111111111111111111111111111111111111111111",
			  std::string{binary_digest},
			  "sha256:3333333333333333333333333333333333333333333333333333333333333333"}});
		require(logical_catalog.has_value(), "logical parity catalog failed");
		auto logical_task_value =
			cxxlens::sdk::provider::task::make({std::string{provider.id()},
												provider.version(),
												std::string{provider.semantic_contract_digest()},
												{descriptor},
												{},
												{"company.test.canonical-1"},
												"observation",
												"assertion"},
											   std::move(*logical_catalog),
											   {descriptor},
											   "condition:all",
											   "company.test.canonical-1",
											   {"dependency-1"});
		require(logical_task_value.has_value(), "logical parity task failed");
		const auto logical_task = std::move(*logical_task_value);
		auto process_request = task(select(executable, "success"));
		process_request.task_id = logical_task.task_id;
		auto process = runtime.execute(process_request);
		require(process && process->succeeded(), "process parity transcript failed");
		auto logical = run_worker(provider, logical_task, writer);
		require(logical.has_value(), "in-process logical provider stream failed");
		auto logical_frames = decode_frame_stream(sink.transcript);
		require(logical_frames && logical_frames->size() + 2U == process->frames.size(),
				"logical/wire provider transcript size diverged");
		auto aggregated = decode_column_chunk(
			logical_frames->at(2U).control, logical_frames->at(2U).payload, descriptor);
		require(aggregated && aggregated->row_count == 2U,
				"relation_sink did not aggregate bounded rows into a column chunk");
		for (std::size_t index = 0U; index < logical_frames->size(); ++index)
			require(logical_frames->at(index).type == process->frames.at(index + 2U).type,
					"logical/wire provider state transition diverged");
		auto logical_verdict =
			cxxlens::sdk::testing::validate_logical_transcript(logical_task,
															   provider.id(),
															   provider.version(),
															   *logical_frames,
															   {64U * 1024U * 1024U, 65536U});
		auto process_verdict =
			cxxlens::sdk::testing::validate_process_transcript(logical_task,
															   process->provider,
															   process->frames,
															   process_request.output_credit,
															   process_request.limits);
		require(logical_verdict && process_verdict && logical_verdict->accepted &&
					process_verdict->accepted &&
					logical_verdict->reason_code == process_verdict->reason_code,
				"logical/wire provider semantic verdict diverged");

		std::vector<frame> process_budget_frames{process->frames.begin(),
												 process->frames.begin() + 2};
		for (auto value : *logical_frames)
		{
			value.sequence += 2U;
			process_budget_frames.push_back(std::move(value));
		}
		const auto logical_bytes = logical_output_size(*logical_frames);
		const auto process_bytes = logical_output_size(process_budget_frames);
		require(logical_bytes == process_bytes && logical_bytes > 1U,
				"logical output-byte accounting included process handshake or framing bytes");
		transcript_sink exact_sink;
		protocol_writer exact_writer{exact_sink};
		exact_writer.grant_credit({64U * 1024U * 1024U, 65536U});
		execution_context exact_execution;
		exact_execution.budget.output_bytes = logical_bytes;
		exact_execution.budget.rows = 2U;
		require(run_worker(provider, logical_task, exact_writer, exact_execution).has_value() &&
					exact_writer.output_bytes() == logical_bytes,
				"run_worker rejected the exact logical output budget");
		transcript_sink limited_sink;
		protocol_writer limited_writer{limited_sink};
		limited_writer.grant_credit({64U * 1024U * 1024U, 65536U});
		auto limited_execution = exact_execution;
		--limited_execution.budget.output_bytes;
		auto limited_logical =
			run_worker(provider, logical_task, limited_writer, limited_execution);
		require(!limited_logical && limited_logical.error().code == "provider.output-limit" &&
					limited_logical.error().field == "output_bytes",
				"run_worker did not enforce logical output bytes before success");
		transcript_sink row_limited_sink;
		protocol_writer row_limited_writer{row_limited_sink};
		row_limited_writer.grant_credit({64U * 1024U * 1024U, 65536U});
		auto row_limited_execution = exact_execution;
		row_limited_execution.budget.rows = 1U;
		auto limited_rows =
			run_worker(provider, logical_task, row_limited_writer, row_limited_execution);
		require(!limited_rows && limited_rows.error().code == "provider.output-limit" &&
					limited_rows.error().field == "rows",
				"run_worker did not enforce the task-global row budget");
		execution_budget exact_budget;
		exact_budget.output_bytes = logical_bytes;
		exact_budget.rows = 2U;
		auto exact_logical =
			cxxlens::sdk::testing::validate_logical_transcript(logical_task,
															   provider.id(),
															   provider.version(),
															   *logical_frames,
															   {64U * 1024U * 1024U, 65536U},
															   exact_budget);
		auto exact_process =
			cxxlens::sdk::testing::validate_process_transcript(logical_task,
															   process->provider,
															   process_budget_frames,
															   process_request.output_credit,
															   process_request.limits,
															   exact_budget);
		require(exact_logical && exact_process && exact_logical->accepted &&
					exact_process->accepted,
				"equal logical output and row budget was rejected");

		auto under_output = exact_budget;
		--under_output.output_bytes;
		auto output_logical =
			cxxlens::sdk::testing::validate_logical_transcript(logical_task,
															   provider.id(),
															   provider.version(),
															   *logical_frames,
															   {64U * 1024U * 1024U, 65536U},
															   under_output);
		auto output_process =
			cxxlens::sdk::testing::validate_process_transcript(logical_task,
															   process->provider,
															   process_budget_frames,
															   process_request.output_credit,
															   process_request.limits,
															   under_output);
		require(output_logical && output_process && !output_logical->accepted &&
					!output_process->accepted &&
					output_logical->reason_code == "provider.output-limit" &&
					output_logical->reason_code == output_process->reason_code,
				"logical output-byte limit diverged by execution surface");

		auto under_rows = exact_budget;
		under_rows.rows = 1U;
		auto rows_logical =
			cxxlens::sdk::testing::validate_logical_transcript(logical_task,
															   provider.id(),
															   provider.version(),
															   *logical_frames,
															   {64U * 1024U * 1024U, 65536U},
															   under_rows);
		auto rows_process =
			cxxlens::sdk::testing::validate_process_transcript(logical_task,
															   process->provider,
															   process_budget_frames,
															   process_request.output_credit,
															   process_request.limits,
															   under_rows);
		require(rows_logical && rows_process && !rows_logical->accepted &&
					!rows_process->accepted &&
					rows_logical->reason_code == "provider.output-limit" &&
					rows_logical->reason_code == rows_process->reason_code,
				"row budget could be bypassed by column chunks or execution surface");

		const std::array diagnostic_records{
			unresolved_item{"company.test.first", "subject:1", "first"},
			unresolved_item{"company.test.second", "subject:2", "second"}};
		auto diagnostic_logical_frames = *logical_frames;
		auto diagnostic_process_frames = process_budget_frames;
		const auto diagnostic_control = encode_unresolved_metadata(diagnostic_records);
		require(diagnostic_control.has_value(), "diagnostic budget fixture encoding failed");
		const auto replace_diagnostics = [&](std::vector<frame>& frames)
		{
			auto found = std::ranges::find(frames, message_type::unresolved_chunk, &frame::type);
			require(found != frames.end(), "diagnostic side-channel frame missing");
			found->control = *diagnostic_control;
		};
		replace_diagnostics(diagnostic_logical_frames);
		replace_diagnostics(diagnostic_process_frames);
		auto diagnostic_budget = exact_budget;
		diagnostic_budget.output_bytes = std::numeric_limits<std::uint64_t>::max();
		diagnostic_budget.diagnostics = 2U;
		auto exact_diagnostics_logical =
			cxxlens::sdk::testing::validate_logical_transcript(logical_task,
															   provider.id(),
															   provider.version(),
															   diagnostic_logical_frames,
															   {64U * 1024U * 1024U, 65536U},
															   diagnostic_budget);
		auto exact_diagnostics_process =
			cxxlens::sdk::testing::validate_process_transcript(logical_task,
															   process->provider,
															   diagnostic_process_frames,
															   process_request.output_credit,
															   process_request.limits,
															   diagnostic_budget);
		require(exact_diagnostics_logical && exact_diagnostics_process &&
					exact_diagnostics_logical->accepted && exact_diagnostics_process->accepted,
				"exact diagnostic record budget was rejected");
		diagnostic_budget.diagnostics = 1U;
		auto diagnostics_logical =
			cxxlens::sdk::testing::validate_logical_transcript(logical_task,
															   provider.id(),
															   provider.version(),
															   diagnostic_logical_frames,
															   {64U * 1024U * 1024U, 65536U},
															   diagnostic_budget);
		auto diagnostics_process =
			cxxlens::sdk::testing::validate_process_transcript(logical_task,
															   process->provider,
															   diagnostic_process_frames,
															   process_request.output_credit,
															   process_request.limits,
															   diagnostic_budget);
		require(diagnostics_logical && diagnostics_process && !diagnostics_logical->accepted &&
					!diagnostics_process->accepted &&
					diagnostics_logical->reason_code == "provider.output-limit" &&
					diagnostics_logical->reason_code == diagnostics_process->reason_code,
				"diagnostic record budget diverged by framing or execution surface");
	}

	bool check_timeout_regression(const std::string& executable)
	{
		auto processes = make_system_provider_process_port();
		require(processes != nullptr, "system provider process port unavailable");
		process_provider_runtime runtime{*processes};
		auto timeout_request = task(select(executable, "timeout"));
		timeout_request.budget.wall_ms = 25U;
		const auto timeout_started = std::chrono::steady_clock::now();
		auto timeout_report = runtime.execute(timeout_request);
		require(timeout_report && timeout_report->terminal == "provider.timeout",
				"provider timeout regression lost the typed timeout terminal");
		require(std::chrono::steady_clock::now() - timeout_started < std::chrono::seconds{10},
				"provider timeout regression exceeded the hard anti-hang bound");

#if defined(__linux__) && defined(__GLIBC__) && defined(SYS_pidfd_open) && \
	defined(SYS_pidfd_send_signal)
		namespace fs = std::filesystem;
		const auto marker = fs::temp_directory_path() /
			("cxxlens-provider-timeout-grandchild-" + std::to_string(::getpid()) + ".pid");
		std::error_code marker_error;
		fs::remove(marker, marker_error);
		require(!marker_error, "could not remove stale pipe-holding descendant marker");
		const auto holder_marker = fs::path{marker.string() + ".holder"};
		const auto sentinel_marker = fs::path{marker.string() + ".sentinel"};
		fs::remove(holder_marker, marker_error);
		require(!marker_error, "could not remove stale holder descendant marker");
		fs::remove(sentinel_marker, marker_error);
		require(!marker_error, "could not remove stale sentinel descendant marker");
		const auto negative_marker = fs::path{marker.string() + ".negative"};
		fs::remove(negative_marker, marker_error);
		require(!marker_error, "could not remove stale negative descendant marker");
		{
			std::ofstream negative_marker_output{negative_marker};
			require(negative_marker_output.good(),
					"could not create the negative descendant marker");
			negative_marker_output << "partial-marker";
			require(negative_marker_output.good(),
					"could not write the negative descendant marker");
		}
		require(!observe_descendant(negative_marker),
				"descendant observation accepted an incomplete marker");
		fs::remove(negative_marker, marker_error);
		require(!marker_error, "could not remove the negative descendant marker");
		auto grandchild_request = task(select(executable, "timeout-grandchild:" + marker.string()));
		// The fixture forks after launch; derive the budget from the inherited ceiling and
		// current same-UID thread count instead of assuming a fixed host process count.
		const auto subprocess_budget = descendant_fixture_subprocess_budget();
		if (!subprocess_budget)
			return false;
		grandchild_request.budget.subprocesses = *subprocess_budget;
		grandchild_request.budget.wall_ms = 5000U;
		std::promise<descendant_observation> holder_promise;
		auto holder_future = holder_promise.get_future();
		std::thread holder_watcher{
			[marker, promise = std::move(holder_promise)]() mutable
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{7};
				while (std::chrono::steady_clock::now() < deadline)
				{
					auto holder = observe_descendant(marker.string() + ".holder");
					auto sentinel = observe_descendant(marker.string() + ".sentinel");
					if (holder && sentinel)
					{
						promise.set_value({std::move(*holder),
										   std::move(*sentinel),
										   std::chrono::steady_clock::now()});
						return;
					}
					if (holder)
						(void)::close(holder->pidfd);
					if (sentinel)
						(void)::close(sentinel->pidfd);
					std::this_thread::sleep_for(std::chrono::milliseconds{1});
				}
				promise.set_value({});
			},
		};
		const auto grandchild_started = std::chrono::steady_clock::now();
		auto grandchild_report = runtime.execute(grandchild_request);
		const auto grandchild_finished = std::chrono::steady_clock::now();
		const auto grandchild_terminal =
			grandchild_report ? grandchild_report->terminal : grandchild_report.error().code;
		// Capture the typed terminal before any observation fallback or descendant cleanup.  The
		// cleanup assertions below must not turn a valid timeout into an observation-only result.
		const bool typed_timeout_terminal =
			grandchild_report && grandchild_report->terminal == "provider.timeout";
		const auto cleanup_descendant = [&](const descendant_observation observation)
		{
			const bool holder_valid = observation.holder.valid;
			const bool sentinel_valid = observation.sentinel.valid;
			const bool holder_exited_before_cleanup =
				holder_valid && process_exit_observed(observation.holder);
			const bool sentinel_exited_before_cleanup =
				sentinel_valid && process_exit_observed(observation.sentinel);
			bool holder_exited_after_cleanup = holder_exited_before_cleanup;
			bool sentinel_exited_after_cleanup = sentinel_exited_before_cleanup;
			if (holder_valid && !holder_exited_after_cleanup)
			{
				(void)kill_pidfd(observation.holder.pidfd);
				holder_exited_after_cleanup =
					wait_pidfd_exit(observation.holder.pidfd, std::chrono::seconds{2});
			}
			if (sentinel_valid && !sentinel_exited_after_cleanup)
			{
				(void)kill_pidfd(observation.sentinel.pidfd);
				sentinel_exited_after_cleanup =
					wait_pidfd_exit(observation.sentinel.pidfd, std::chrono::seconds{2});
			}
			if (holder_valid)
				(void)::close(observation.holder.pidfd);
			if (sentinel_valid)
				(void)::close(observation.sentinel.pidfd);
			std::error_code marker_cleanup_error;
			fs::remove(marker, marker_cleanup_error);
			std::error_code holder_marker_cleanup_error;
			fs::remove(holder_marker, holder_marker_cleanup_error);
			std::error_code sentinel_marker_cleanup_error;
			fs::remove(sentinel_marker, sentinel_marker_cleanup_error);
			return std::array{holder_valid,
							  sentinel_valid,
							  holder_exited_before_cleanup,
							  sentinel_exited_before_cleanup,
							  holder_exited_after_cleanup,
							  sentinel_exited_after_cleanup,
							  !marker_cleanup_error && !holder_marker_cleanup_error &&
								  !sentinel_marker_cleanup_error};
		};
		const auto descendants = holder_future.get();
		holder_watcher.join();
		// Executable verification is part of runtime setup and is materially slower under
		// sanitizer instrumentation.  The cleanup bound starts at the positive fixture readiness
		// observation, so it measures timeout/RAII behavior rather than hashing cold-start time.
		const auto observation_origin =
			descendants.ready_at != std::chrono::steady_clock::time_point{} &&
				descendants.ready_at <= grandchild_finished
			? descendants.ready_at
			: grandchild_started;
		const auto grandchild_elapsed = grandchild_finished - observation_origin;
		auto observed_descendants = descendants;
		if (!observed_descendants.holder.valid)
			if (auto holder = observe_descendant(holder_marker))
				observed_descendants.holder = std::move(*holder);
		if (!observed_descendants.sentinel.valid)
			if (auto sentinel = observe_descendant(sentinel_marker))
				observed_descendants.sentinel = std::move(*sentinel);
		const auto cleanup = cleanup_descendant(observed_descendants);
		require(typed_timeout_terminal,
				"pipe-holding descendant lost the typed timeout terminal: " + grandchild_terminal);
		require(cleanup[0] && cleanup[1],
				"pipe-holding process-group descendant identities could not be observed");
		require(
			cleanup[2] && cleanup[3],
			std::string{
				"provider timeout did not terminate the pipe-holding process-group descendant: "} +
				"after-cleanup-holder=" + std::to_string(cleanup[4]) +
				" after-cleanup-sentinel=" + std::to_string(cleanup[5]) +
				" budget=" + std::to_string(*subprocess_budget) + " elapsed=" +
				std::to_string(
					std::chrono::duration_cast<std::chrono::milliseconds>(grandchild_elapsed)
						.count()) +
				"ms");
		require(cleanup[4] && cleanup[5], "pipe-holding descendants cleanup failed");
		require(cleanup[6], "pipe-holding descendant markers could not be removed");
		fs::remove(negative_marker, marker_error);
		require(!marker_error, "could not clean up the negative descendant marker");
		require(grandchild_elapsed < std::chrono::seconds{8},
				"provider timeout waited for the pipe-holding descendant instead of closing it: " +
					std::to_string(
						std::chrono::duration_cast<std::chrono::milliseconds>(grandchild_elapsed)
							.count()) +
					"ms");
#endif
		return true;
	}

	void check_semantic_input_digests(const std::string& executable)
	{
		auto request = task(select(executable, "success"));
		const auto invocation =
			semantic_digest("cxxlens.test.provider-invocation.v1", "semantic-invocation");
		const auto toolchain =
			semantic_digest("cxxlens.test.provider-toolchain.v1", "semantic-toolchain");
		const auto environment =
			semantic_digest("cxxlens.test.provider-environment.v1", "semantic-environment");
		require(invocation && toolchain && environment,
				"semantic provider input digest setup failed");
		request.normalized_invocation_digest = *invocation;
		request.toolchain_digest = *toolchain;
		request.environment_digest = *environment;
		auto processes = make_system_provider_process_port();
		require(processes != nullptr, "system provider process port unavailable");
		process_provider_runtime runtime{*processes};
		auto report = runtime.execute(request);
		std::string failure_detail{
			"semantic provider input digests were rejected by the process runtime"};
		if (report)
		{
			failure_detail += " terminal=" + report->terminal;
			failure_detail += " exit=" + std::to_string(report->exit_code);
			for (const auto& diagnostic : report->diagnostics)
				failure_detail += " [" + diagnostic.code + ":" + diagnostic.detail + "]";
		}
		else
			failure_detail += " error=" + report.error().code;
		require(report && report->succeeded(), failure_detail);
	}

	void check_prior_snapshot_preserved(const std::string& executable)
	{
		relation_registry registry;
		require(registry.add(snapshot_test_descriptor()).has_value(),
				"snapshot registry setup failed");
		auto engine = registry.build("provider-runtime-snapshot");
		require(engine.has_value(), "snapshot relation engine failed");
		auto store = make_in_memory_snapshot_store(*engine);
		require(store.has_value(), "snapshot store failed");
		snapshot_series_selector selector{
			"catalog",
			"scope",
			std::string{engine->generation()},
			"runtime-universe",
			std::string{engine->registry_digest()},
			"sha256:4444444444444444444444444444444444444444444444444444444444444444",
			"sha256:5555555555555555555555555555555555555555555555555555555555555555"};
		auto writer = store->begin(snapshot_draft{
			selector,
			{1U, 0U, 0U},
			"sha256:6666666666666666666666666666666666666666666666666666666666666666",
			std::nullopt});
		require(writer.has_value(), "snapshot writer failed");
		auto staged = writer->stage(snapshot_test_partition(*engine));
		require(staged.has_value(),
				"baseline snapshot staging failed: " +
					(staged ? std::string{} : staged.error().code + " " + staged.error().detail));
		require(writer->validate().has_value(), "baseline snapshot validation failed");
		auto published = writer->publish();
		require(published.has_value(), "baseline snapshot failed");
		const auto prior = published->manifest().id;

		auto processes = make_system_provider_process_port();
		process_provider_runtime runtime{*processes};
		auto failed = runtime.execute(task(select(executable, "crash")));
		require(failed && !failed->succeeded(), "crashing provider unexpectedly succeeded");
		auto current = store->current(selector);
		require(current && current->manifest().id == prior,
				"failed worker destroyed or replaced the prior snapshot");
	}

	void check_ng1_live_duplex_process()
	{
#if defined(__linux__) && defined(__GLIBC__)
		const auto policy = baseline_policy();
		process_invocation invocation;
		invocation.argv = {"/bin/cat"};
		invocation.budget.wall_ms = 3000U;
		invocation.budget.cpu_ms = 3000U;
		invocation.budget.address_space_bytes = provider_address_space_budget;
		invocation.budget.transport_bytes = 1024U * 1024U;
		invocation.budget.output_bytes = 1024U * 1024U;
		invocation.budget.open_files = 64U;
		invocation.budget.subprocesses = provider_subprocess_budget;
		invocation.sandbox = {sandbox_assurance::enforced, policy.policy_digest()};
		invocation.expected_binary_digest = executable_digest("/bin/cat");

		protocol_limits limits;
		limits.minimum_minor = 1U;
		limits.maximum_minor = 1U;
		auto port = detail::make_system_ng1_duplex_process_port();
		require(port != nullptr, "NG1 live process port was not created");
		auto make_invocation = [&](std::vector<std::string> arguments)
		{
			auto value = invocation;
			value.argv = std::move(arguments);
			value.expected_binary_digest = executable_digest(value.argv.front());
			return value;
		};
		auto start_process = [&](const process_invocation& request)
		{
			auto started = port->start(request, limits, {});
			if (!started)
				require(false,
						"NG1 live regression process launch failed: " + started.error().code);
			return std::move(*started);
		};
		auto process = port->start(invocation, limits, {});
		if (!process)
			require(false, "NG1 live process launch failed: " + process.error().code);
		auto staged_digest = semantic_digest("test.ng1.live", "staged");
		require(staged_digest.has_value(), "NG1 live staged digest construction failed");

		detail::ng1_heartbeat_control control{"cxxlens.provider-control.heartbeat.v1",
											  detail::ng1_heartbeat_kind::ack,
											  "provider:test",
											  {1U, 2U, 3U},
											  "session:test",
											  "task:test",
											  7U,
											  0U,
											  123U,
											  0U,
											  *staged_digest};
		auto encoded_control = detail::encode_ng1_heartbeat_control(control);
		require(encoded_control.has_value(), "NG1 live heartbeat encoding failed");
		frame wire;
		wire.type = detail::ng1_heartbeat_message_type;
		wire.stream_id = 7U;
		wire.sequence = 0U;
		wire.control = *encoded_control;
		wire.protocol_major = 1U;
		wire.protocol_minor = 1U;
		auto sent = (*process)->send_frame(wire);
		require(sent.has_value(), "NG1 live frame send failed");
		auto received = (*process)->receive_frame({});
		require(received.has_value() && received->has_value(),
				"NG1 live frame receive reached EOF");
		require(received->value().type == wire.type &&
					received->value().stream_id == wire.stream_id &&
					received->value().sequence == wire.sequence &&
					received->value().control == wire.control &&
					received->value().payload == wire.payload &&
					received->value().protocol_major == wire.protocol_major &&
					received->value().protocol_minor == wire.protocol_minor &&
					received->value().flags == wire.flags,
				"NG1 live frame round trip changed wire data");
		auto finished = (*process)->finish({});
		std::string finish_detail{
			"NG1 live process did not finish with exact identity and exit evidence"};
		if (finished)
		{
			finish_detail += " status=" + std::to_string(static_cast<int>(finished->status));
			finish_detail += " exit=" + std::to_string(finished->exit_code);
			finish_detail += " signal=" + std::to_string(finished->termination_signal);
			finish_detail += " digest=" + finished->measured_executable_digest;
			finish_detail += " stderr=" + finished->standard_error;
		}
		else
			finish_detail += " error=" + finished.error().code + ":" + finished.error().detail;
		require(finished.has_value() && finished->status == process_status::exited &&
					finished->exit_code == 0 &&
					finished->measured_executable_digest == invocation.expected_binary_digest,
				finish_detail);
		require(finished->sandbox.achieved == sandbox_assurance::enforced,
				"NG1 live process lost enforced sandbox evidence");

		auto rejected_process = port->start(invocation, limits, {});
		if (!rejected_process)
			require(false,
					"NG1 live negative process launch failed: " + rejected_process.error().code);
		wire.flags = static_cast<std::uint16_t>(frame_flag::optional_extension);
		auto rejected = (*rejected_process)->send_frame(wire);
		require(!rejected && rejected.error().code == "provider.protocol-state-invalid",
				"NG1 live channel accepted optional flags on the reserved heartbeat");
		auto terminated = (*rejected_process)->terminate(process_status::cancelled);
		require(terminated.has_value() && terminated->status == process_status::cancelled,
				"NG1 live channel did not preserve explicit cancellation evidence");

		auto make_wire = [&](std::vector<std::byte> payload = {})
		{
			auto value = wire;
			value.type = message_type::input_chunk;
			value.flags = {};
			value.payload = std::move(payload);
			return value;
		};

		// Host input is not provider stdout/stderr transport.  A one-byte output budget must
		// not reject a larger frame that is consumed by the provider without producing output.
		auto input_only = make_invocation({"/bin/sh", "-c", "while IFS= read -r line; do :; done"});
		input_only.budget.transport_bytes = 1U;
		input_only.budget.wall_ms = 5000U;
		auto input_only_process = start_process(input_only);
		auto input_only_wire = make_wire(std::vector<std::byte>(4096U, std::byte{0x41}));
		auto input_only_sent = input_only_process->send_frame(input_only_wire);
		if (!input_only_sent)
			require(false,
					"NG1 live host input was incorrectly charged to the output transport budget: " +
						input_only_sent.error().code + ":" + input_only_sent.error().field + ":" +
						input_only_sent.error().detail);
		auto input_only_finished = input_only_process->finish({});
		require(input_only_finished.has_value() &&
					input_only_finished->status == process_status::exited &&
					input_only_finished->exit_code == 0,
				"NG1 live input-only process did not finish cleanly");

		// A provider that closes stdin must produce a typed write failure, not terminate the
		// host through the default SIGPIPE disposition.
		auto closed_input = make_invocation({"/bin/sh", "-c", "exec 0<&-; exit 0"});
		closed_input.budget.wall_ms = 5000U;
		auto closed_input_process = start_process(closed_input);
		auto closed_input_eof = closed_input_process->receive_frame({});
		require(closed_input_eof.has_value() && !closed_input_eof->has_value(),
				"NG1 live closed-input provider did not reach stdout EOF");
		auto closed_input_send = closed_input_process->send_frame(make_wire());
		require(!closed_input_send,
				"NG1 live write to a closed provider stdin unexpectedly succeeded");
		auto closed_input_finished = closed_input_process->finish({});
		require(closed_input_finished.has_value(),
				"NG1 live closed-input provider could not finish after typed write failure");

		// Receive-side cancellation terminates the worker while returning an error to the driver.
		// The subsequent finish must still expose the exact retained process outcome.
		auto cancelled_process = start_process(invocation);
		std::stop_source cancellation;
		cancellation.request_stop();
		auto cancelled_receive = cancelled_process->receive_frame(cancellation.get_token());
		require(!cancelled_receive && cancelled_receive.error().code == "provider.cancelled",
				"NG1 receive cancellation did not return its typed error");
		auto cancelled_finish = cancelled_process->finish({});
		require(cancelled_finish.has_value() &&
					cancelled_finish->status == process_status::cancelled,
				"NG1 receive cancellation discarded the exact process outcome");

		// A provider that never reads stdin must be killed when a backpressured send reaches its
		// wall deadline, and the later finish must retain the timeout evidence.
		auto blocked_send = make_invocation({"/usr/bin/sleep", "5"});
		blocked_send.budget.wall_ms = 50U;
		auto blocked_send_process = start_process(blocked_send);
		auto blocked_send_payload = std::vector<std::byte>(4U * 1024U * 1024U, std::byte{0x43});
		auto blocked_send_result =
			blocked_send_process->send_frame(make_wire(std::move(blocked_send_payload)));
		if (blocked_send_result || blocked_send_result.error().code != "provider.timeout")
			require(false,
					"NG1 backpressured send did not terminate at its wall deadline: " +
						(blocked_send_result ? std::string{"success"}
											 : blocked_send_result.error().code + ":" +
								 blocked_send_result.error().field + ":" +
								 blocked_send_result.error().detail));
		auto blocked_send_finish = blocked_send_process->finish({});
		require(blocked_send_finish.has_value() &&
					blocked_send_finish->status == process_status::timed_out,
				"NG1 send deadline discarded timeout process evidence");

		// The leader can exit while a descendant retains the output pipe.  The bounded finish
		// path must preserve its typed timeout while killing the original process group.
		namespace fs = std::filesystem;
		const auto descendant_marker = fs::temp_directory_path() /
			("cxxlens-ng1-live-descendant-" + std::to_string(::getpid()) + ".pid");
		std::error_code marker_error;
		fs::remove(descendant_marker, marker_error);
		require(!marker_error, "could not remove stale NG1 live descendant marker");
		const auto descendant_budget = descendant_fixture_subprocess_budget();
		require(descendant_budget.has_value(),
				"could not derive a process budget for the NG1 live descendant test");
		const auto descendant_command =
			"/usr/bin/sleep 30 & child=$!; read -r pid comm state ppid pgrp session tty_nr tpgid "
			"flags minflt cminflt majflt cmajflt utime stime cutime cstime priority nice "
			"num_threads itrealvalue start_time rest < /proc/$child/stat; printf '%s %s\\n' "
			"\"$child\" \"$start_time\" > " +
			descendant_marker.string() + "; exit 0";
		auto descendant = make_invocation({"/bin/sh", "-c", descendant_command});
		descendant.budget.subprocesses = *descendant_budget;
		descendant.budget.wall_ms = 2000U;
		const auto descendant_deadline_origin = std::chrono::steady_clock::now();
		auto descendant_process = start_process(descendant);
		std::optional<holder_observation> observed_descendant;
		// Use shell builtins for the /proc read so marker readiness stays inside the live
		// operation deadline without spawning a helper process after the descendant fork.  The
		// origin is captured before start_process(), whose internal deadline starts during the
		// call, so descheduling after startup cannot extend this readiness window.
		const auto marker_deadline =
			descendant_deadline_origin + std::chrono::milliseconds{descendant.budget.wall_ms};
		while (std::chrono::steady_clock::now() < marker_deadline && !observed_descendant)
		{
			observed_descendant = observe_descendant(descendant_marker);
			if (!observed_descendant)
				std::this_thread::sleep_for(std::chrono::milliseconds{1});
		}
		require(observed_descendant.has_value(),
				"NG1 live descendant identity could not be observed before cleanup");
		std::this_thread::sleep_for(std::chrono::milliseconds{50});
		const bool descendant_alive_before_cleanup = !pidfd_exited(observed_descendant->pidfd);
		const auto descendant_finished = descendant_process->finish({});
		const bool descendant_exited_after_cleanup =
			wait_pidfd_exit(observed_descendant->pidfd, std::chrono::milliseconds{500});
		if (!descendant_exited_after_cleanup)
		{
			(void)kill_pidfd(observed_descendant->pidfd);
			(void)wait_pidfd_exit(observed_descendant->pidfd, std::chrono::seconds{2});
		}
		const bool descendant_timed_out = descendant_finished.has_value() &&
			descendant_finished->status == process_status::timed_out;
		const bool descendant_exited = pidfd_exited(observed_descendant->pidfd);
		(void)::close(observed_descendant->pidfd);
		fs::remove(descendant_marker, marker_error);
		require(descendant_timed_out && descendant_alive_before_cleanup &&
					descendant_exited_after_cleanup && descendant_exited && !marker_error,
				"NG1 live process-group cleanup leaked a post-reap descendant");

		// While the provider is writing a large stderr burst, the host must continue draining it
		// while its nonblocking stdin is backpressured.  Polling stdin alone deadlocks this case.
		auto backpressured = make_invocation(
			{"/bin/sh",
			 "-c",
			 "/usr/bin/dd if=/dev/zero bs=4096 count=256 >&2; /bin/cat >/dev/null"});
		backpressured.budget.wall_ms = 5000U;
		backpressured.budget.subprocesses = *descendant_budget;
		backpressured.budget.transport_bytes = 2U * 1024U * 1024U;
		auto backpressured_process = start_process(backpressured);
		auto backpressure_payload = std::vector<std::byte>(1024U * 1024U, std::byte{0x42});
		auto backpressure_sent =
			backpressured_process->send_frame(make_wire(std::move(backpressure_payload)));
		if (!backpressure_sent)
			require(false,
					"NG1 live duplex write deadlocked instead of draining provider stderr: " +
						backpressure_sent.error().code + ":" + backpressure_sent.error().field +
						":" + backpressure_sent.error().detail);
		auto backpressured_finished = backpressured_process->finish({});
		require(backpressured_finished.has_value() &&
					backpressured_finished->status == process_status::exited &&
					backpressured_finished->exit_code == 0,
				"NG1 live backpressure process did not finish cleanly");
#else
		// The unavailable implementation is intentionally fail-closed on non-Linux platforms.
#endif
	}

} // namespace

int main(const int argument_count, const char* const* arguments)
{
	if (argument_count == 3 && std::string_view{arguments[1]} == "--host-only")
	{
		check_host_transcript_validator(arguments[2]);
		return 0;
	}
	if (argument_count == 3 && std::string_view{arguments[1]} == "--receipt-only")
	{
		check_process_faults(arguments[2], true);
		return 0;
	}
	if (argument_count == 2 && std::string_view{arguments[1]} == "--sealed-only")
	{
		check_sealed_provider_validation();
		return 0;
	}
	if (argument_count == 3 && std::string_view{arguments[1]} == "--timeout-regression")
	{
#if defined(__linux__) && defined(__GLIBC__) && defined(SYS_pidfd_open) && \
	defined(SYS_pidfd_send_signal)
		if (!pidfd_runtime_available())
			return 77;
		return check_timeout_regression(arguments[2]) ? 0 : 77;
#else
		return 77;
#endif
	}
	require(argument_count == 2, "provider process fixture path missing");
	const std::string executable{arguments[1]};
	check_selection(executable);
	check_verified_executable_binding();
	check_sandbox_closed_enum(executable);
	check_host_transcript_validator(executable);
	check_sealed_provider_validation();
	check_semantic_input_digests(executable);
#if defined(__linux__) && defined(__GLIBC__) && defined(SYS_pidfd_open) && \
	defined(SYS_pidfd_send_signal)
	check_ng1_post_fork_guard_kills_group_without_ack();
#endif
	check_ng1_live_duplex_process();
	check_process_faults(executable);
	check_prior_snapshot_preserved(executable);
}
