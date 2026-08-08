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
	using cxxlens::sdk::sqlite_same_process_shm_registry_test_peer;
	using cxxlens::sdk::sqlite_same_process_shm_process_port;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error(std::string{message});
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
		auto adopted = first_handle.adopt_runtime_lifetime(
			identity("test.process-port.runtime", 1U),
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
			const bool inherited_stale = !parent_handle.valid() && parent_handle.registry() == nullptr &&
				!parent_registry->snapshot().process_live;
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
#endif
	}
} // namespace

int main()
{
	try
	{
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
