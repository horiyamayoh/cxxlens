#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#if defined(__linux__)
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "sdk/sqlite_same_process_shm_process_port_internal.hpp"

namespace cxxlens::sdk
{
	class sqlite_same_process_shm_registry_test_peer
	{
	  public:
		static void invalidate_process(sqlite_same_process_shm_mapping_registry& registry) noexcept
		{
			registry.invalidate_process_instance_for_testing();
		}
	};
} // namespace cxxlens::sdk

namespace
{
	using cxxlens::sdk::sqlite_backend_opaque_identity;
	using cxxlens::sdk::sqlite_same_process_shm_process_port;
	using cxxlens::sdk::sqlite_same_process_shm_registry_test_peer;
	using cxxlens::sdk::sqlite_shm_process_identity_observation;
	using cxxlens::sdk::sqlite_shm_process_identity_rejection_reason;
	using cxxlens::sdk::validate_sqlite_shm_process_identity;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error(std::string{message});
	}

	[[nodiscard]] bool
	same_registry_snapshot(const cxxlens::sdk::sqlite_shm_mapping_registry_snapshot& left,
						   const cxxlens::sdk::sqlite_shm_mapping_registry_snapshot& right) noexcept
	{
		return left.process_epoch == right.process_epoch &&
			left.cohort_count == right.cohort_count &&
			left.alias_record_count == right.alias_record_count &&
			left.reserved_alias_count == right.reserved_alias_count &&
			left.registering_alias_count == right.registering_alias_count &&
			left.registered_alias_count == right.registered_alias_count &&
			left.unregistering_alias_count == right.unregistering_alias_count &&
			left.detached_alias_tombstone_count == right.detached_alias_tombstone_count &&
			left.quarantined_alias_count == right.quarantined_alias_count &&
			left.family_record_count == right.family_record_count &&
			left.active_family_count == right.active_family_count &&
			left.retired_family_tombstone_count == right.retired_family_tombstone_count &&
			left.quarantined_family_count == right.quarantined_family_count &&
			left.active_family_pin_count == right.active_family_pin_count &&
			left.active_activity_pin_count == right.active_activity_pin_count &&
			left.active_reader_open_count == right.active_reader_open_count &&
			left.duplicate_rejection_count == right.duplicate_rejection_count &&
			left.cross_binding_rejection_count == right.cross_binding_rejection_count &&
			left.ambiguous_lookup_count == right.ambiguous_lookup_count &&
			left.generation_source_count == right.generation_source_count &&
			left.reader_lifecycle_sequence_source_count ==
			right.reader_lifecycle_sequence_source_count &&
			left.retired_reader_lifecycle_tombstone_count ==
			right.retired_reader_lifecycle_tombstone_count &&
			left.retired_reader_open_epoch_close_tombstone_count ==
			right.retired_reader_open_epoch_close_tombstone_count &&
			left.reader_lifecycle_last_issued_sequence ==
			right.reader_lifecycle_last_issued_sequence &&
			left.process_live == right.process_live &&
			left.registry_quarantined == right.registry_quarantined;
	}

	[[nodiscard]] sqlite_backend_opaque_identity identity(const std::string_view profile,
														  const std::uint64_t value)
	{
		sqlite_backend_opaque_identity output;
		output.profile = std::string{profile};
		for (auto shift = 56U;; shift -= 8U)
		{
			output.bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
			if (shift == 0U)
				break;
		}
		return output;
	}

	void verify_exact_one_process_registry()
	{
		auto first = sqlite_same_process_shm_process_port::acquire();
#if defined(__linux__) && defined(SYS_pidfd_open)
		require(first.has_value(), "qualified process port available");
#else
		require(!first, "unsupported process port fails closed");
		return;
#endif
		auto first_handle = std::move(first.value());
		require(first_handle.valid(), "first process handle valid");
		require(first_handle.registry() != nullptr, "first process registry present");
		require(!first_handle.process_instance().profile.empty(), "process profile present");
		require(!first_handle.process_instance().bytes.empty(), "process receipt present");

		auto second = sqlite_same_process_shm_process_port::acquire();
		require(second.has_value(), "second process port acquire");
		require(second.value().valid(), "second process handle valid");
		require(second.value().registry() == first_handle.registry(),
				"one process-global registry instance");
		require(second.value().process_instance() == first_handle.process_instance(),
				"one process-global identity receipt");

		constexpr std::size_t thread_count = 32U;
		std::array<const void*, thread_count> registries{};
		std::array<bool, thread_count> identities{};
		std::array<std::thread, thread_count> threads;
		std::atomic_bool start{false};
		for (std::size_t index = 0; index < thread_count; ++index)
		{
			threads[index] = std::thread{
				[&, index]
				{
					while (!start.load(std::memory_order_acquire))
						std::this_thread::yield();
					auto acquired = sqlite_same_process_shm_process_port::acquire();
					if (!acquired || !acquired.value().valid())
						return;
					registries[index] = acquired.value().registry();
					identities[index] =
						acquired.value().process_instance() == first_handle.process_instance();
				},
			};
		}
		start.store(true, std::memory_order_release);
		for (auto& thread : threads)
			thread.join();
		for (std::size_t index = 0; index < thread_count; ++index)
		{
			require(registries[index] == first_handle.registry(),
					"concurrent acquire shares exact registry");
			require(identities[index], "concurrent acquire shares exact process receipt");
		}

		auto runtime_owner = std::make_shared<std::uint64_t>(7U);
		auto adopted =
			first_handle.adopt_runtime_lifetime(identity("test.process-port.runtime", 1U),
												identity("test.process-port.runtime-pin", 1U),
												runtime_owner);
		require(adopted.has_value(), "qualified process handle adopts runtime lifetime");
		require(adopted.value().valid(), "adopted runtime lifetime pin valid");
		runtime_owner.reset();
		require(adopted.value().valid(), "runtime lifetime retained by process registry pin");
	}

	void verify_fork_child_invalidates_inherited_registry()
	{
#if defined(__linux__) && defined(SYS_pidfd_open)
		auto parent = sqlite_same_process_shm_process_port::acquire();
		require(parent.has_value(), "parent process port acquire");
		auto parent_handle = std::move(parent.value());
		const auto parent_identity = parent_handle.process_instance();
		auto* const parent_registry = parent_handle.registry();

		int pipe_descriptors[2]{-1, -1};
		require(::pipe(pipe_descriptors) == 0, "fork result pipe");
		const auto child = ::fork();
		require(child >= 0, "fork process-port child");
		if (child == 0)
		{
			(void)::close(pipe_descriptors[0]);
			std::uint8_t verdict{};
			const bool inherited_stale = !parent_handle.valid() &&
				parent_handle.registry() == nullptr && !parent_registry->snapshot().process_live;
			auto acquired = sqlite_same_process_shm_process_port::acquire();
			const bool fresh = acquired.has_value() && acquired.value().valid() &&
				acquired.value().registry() != nullptr &&
				acquired.value().process_instance() != parent_identity;
			verdict = inherited_stale && fresh ? 1U : 0U;
			const auto ignored = ::write(pipe_descriptors[1], &verdict, sizeof(verdict));
			(void)ignored;
			(void)::close(pipe_descriptors[1]);
			::_exit(verdict == 1U ? EXIT_SUCCESS : EXIT_FAILURE);
		}

		(void)::close(pipe_descriptors[1]);
		std::uint8_t verdict{};
		const auto count = ::read(pipe_descriptors[0], &verdict, sizeof(verdict));
		(void)::close(pipe_descriptors[0]);
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait process-port child");
		require(count == static_cast<ssize_t>(sizeof(verdict)) && verdict == 1U,
				"child receives fresh non-reusable process registry");
		require(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
				"process-port child exits successfully");
		require(parent_handle.valid(), "parent handle remains valid after child fork");
		auto parent_again = sqlite_same_process_shm_process_port::acquire();
		require(parent_again.has_value() && parent_again.value().registry() == parent_registry,
				"parent retains original registry after child fork");
#endif
	}

	void verify_same_process_epoch_loss_never_recreates_registry()
	{
#if defined(__linux__) && defined(SYS_pidfd_open)
		auto acquired = sqlite_same_process_shm_process_port::acquire();
		require(acquired.has_value(), "process port acquire before forced epoch loss");
		auto handle = std::move(acquired.value());
		const auto parent_identity = handle.process_instance();
		auto* const registry = handle.registry();
		require(registry != nullptr, "registry present before forced epoch loss");

		sqlite_same_process_shm_registry_test_peer::invalidate_process(*registry);
		require(!handle.valid() && handle.registry() == nullptr,
				"process handle rejects a stale registry epoch");
		auto first_retry = sqlite_same_process_shm_process_port::acquire();
		auto second_retry = sqlite_same_process_shm_process_port::acquire();
		require(!first_retry && !second_retry,
				"same process never replaces a lost process-global registry");
		require(first_retry.error().action ==
						cxxlens::sdk::sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					second_retry.error().action ==
						cxxlens::sdk::sqlite_shm_lease_recovery_action::quarantine_no_retry,
				"same-process epoch loss remains sticky quarantine");

		int pipe_descriptors[2]{-1, -1};
		require(::pipe(pipe_descriptors) == 0, "quarantine fork result pipe");
		const auto child = ::fork();
		require(child >= 0, "fork child after same-process quarantine");
		if (child == 0)
		{
			(void)::close(pipe_descriptors[0]);
			std::uint8_t verdict{};
			auto child_acquired = sqlite_same_process_shm_process_port::acquire();
			const bool fresh = child_acquired.has_value() && child_acquired.value().valid() &&
				child_acquired.value().registry() != nullptr &&
				child_acquired.value().process_instance() != parent_identity;
			verdict = fresh ? 1U : 0U;
			const auto ignored = ::write(pipe_descriptors[1], &verdict, sizeof(verdict));
			(void)ignored;
			(void)::close(pipe_descriptors[1]);
			::_exit(verdict == 1U ? EXIT_SUCCESS : EXIT_FAILURE);
		}

		(void)::close(pipe_descriptors[1]);
		std::uint8_t verdict{};
		const auto count = ::read(pipe_descriptors[0], &verdict, sizeof(verdict));
		(void)::close(pipe_descriptors[0]);
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait quarantined-process child");
		require(count == static_cast<ssize_t>(sizeof(verdict)) && verdict == 1U,
				"fork child starts a distinct process-instance registry after parent quarantine");
		require(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
				"quarantined-process child exits successfully");

		auto parent_retry = sqlite_same_process_shm_process_port::acquire();
		require(!parent_retry &&
					parent_retry.error().action ==
						cxxlens::sdk::sqlite_shm_lease_recovery_action::quarantine_no_retry,
				"fork child recovery does not clear parent quarantine");
#endif
	}

	void verify_process_identity_validator_is_pure_and_fail_closed()
	{
		sqlite_shm_process_identity_observation expected{
			41U,
			100U,
			7U,
			11U,
			13U,
			17U,
			19U,
			true,
		};
		const auto original = expected;
		require(validate_sqlite_shm_process_identity(expected, expected).has_value(),
				"exact live process identity is accepted");

		auto require_rejection = [&](const sqlite_shm_process_identity_observation& observed,
									 const sqlite_shm_process_identity_rejection_reason reason,
									 const std::string_view message)
		{
			auto result = validate_sqlite_shm_process_identity(expected, observed);
			require(!result && result.error().reason == reason, message);
			require(expected == original, "identity validator does not mutate expected input");
		};

		auto pid_mismatch = expected;
		++pid_mismatch.pid;
		require_rejection(pid_mismatch,
						  sqlite_shm_process_identity_rejection_reason::pid_mismatch,
						  "PID reuse is rejected before native effect");

		auto start_mismatch = expected;
		++start_mismatch.process_start_ticks;
		require_rejection(start_mismatch,
						  sqlite_shm_process_identity_rejection_reason::process_start_mismatch,
						  "reused PID start identity is rejected before native effect");

		auto dead_pidfd = expected;
		dead_pidfd.pidfd_live = false;
		require_rejection(dead_pidfd,
						  sqlite_shm_process_identity_rejection_reason::pidfd_not_live,
						  "dead pidfd fails closed");

		auto unknown_liveness = expected;
		unknown_liveness.pidfd_live = false;
		const auto unknown_result =
			validate_sqlite_shm_process_identity(unknown_liveness, expected);
		require(!unknown_result &&
					unknown_result.error().reason ==
						sqlite_shm_process_identity_rejection_reason::pidfd_not_live,
				"unknown pidfd liveness fails closed");
		require(expected == original, "unknown liveness does not mutate expected input");

		auto namespace_mismatch = expected;
		++namespace_mismatch.pid_namespace_inode;
		require_rejection(namespace_mismatch,
						  sqlite_shm_process_identity_rejection_reason::pid_namespace_mismatch,
						  "PID namespace replacement is rejected before native effect");

		auto pidfd_mismatch = expected;
		++pidfd_mismatch.pidfd_inode;
		require_rejection(pidfd_mismatch,
						  sqlite_shm_process_identity_rejection_reason::pidfd_mismatch,
						  "PIDFD replacement is rejected before native effect");

		auto fork_mismatch = expected;
		++fork_mismatch.fork_epoch;
		require_rejection(fork_mismatch,
						  sqlite_shm_process_identity_rejection_reason::fork_epoch_mismatch,
						  "inherited fork epoch is rejected before native effect");

		auto invalid = expected;
		invalid.pid = 0U;
		require_rejection(invalid,
						  sqlite_shm_process_identity_rejection_reason::invalid_identity,
						  "incomplete process identity fails closed");

#if defined(__linux__) && defined(SYS_pidfd_open)
		auto acquired = sqlite_same_process_shm_process_port::acquire();
		require(acquired.has_value(), "validator production process acquire");
		auto handle = std::move(acquired.value());
		auto* const registry = handle.registry();
		require(handle.valid() && registry != nullptr,
				"production process identity is still live after validation faults");
		const auto before = registry->snapshot();
		require(handle.valid() && handle.registry() == registry,
				"production validator accepts the exact live process identity");
		const auto after = registry->snapshot();
		require(same_registry_snapshot(before, after),
				"identity validation has zero registry effect");
#endif
	}
} // namespace

int main()
{
	try
	{
		verify_process_identity_validator_is_pure_and_fail_closed();
		verify_exact_one_process_registry();
		verify_fork_child_invalidates_inherited_registry();
		verify_same_process_epoch_loss_never_recreates_registry();
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
