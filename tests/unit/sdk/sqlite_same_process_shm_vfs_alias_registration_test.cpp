#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <barrier>

#if defined(__linux__)
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "sdk/sqlite_same_process_shm_vfs_alias_registration_internal.hpp"

namespace cxxlens::sdk
{
	class sqlite_same_process_shm_vfs_alias_registration_test_peer
	{
	  public:
		[[nodiscard]] static sqlite_shm_vfs_alias_lifecycle_binding
		binding(sqlite_shm_process_registry_handle process,
				sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
				sqlite_backend_opaque_identity alias_lifetime,
				sqlite_backend_opaque_identity runtime_lifetime_identity,
				sqlite_backend_opaque_identity runtime_lifetime_pin_identity,
				std::shared_ptr<void> runtime_lifetime_owner,
				std::string registered_vfs_name,
				void* vfs_implementation,
				sqlite_shm_vfs_alias_lifecycle_binding::find_function find,
				sqlite_shm_vfs_alias_lifecycle_binding::register_function register_vfs,
				sqlite_shm_vfs_alias_lifecycle_binding::unregister_function unregister_vfs)
		{
			return {std::move(process),
					std::move(shared_runtime_vfs_cohort),
					std::move(alias_lifetime),
					std::move(runtime_lifetime_identity),
					std::move(runtime_lifetime_pin_identity),
					std::move(runtime_lifetime_owner),
					std::move(registered_vfs_name),
					vfs_implementation,
					find,
					register_vfs,
					unregister_vfs};
		}

		[[nodiscard]] static sqlite_shm_registry_alias_pin&
		alias_pin(sqlite_shm_registered_vfs_alias& alias)
		{
			return *alias.alias_;
		}

		static void exhaust_lifecycle_sequence() noexcept
		{
			sqlite_same_process_shm_vfs_alias_registration_port::
				exhaust_lifecycle_sequence_for_testing();
		}
	};
} // namespace cxxlens::sdk

namespace
{
	using cxxlens::sdk::sqlite_backend_opaque_identity;
	using cxxlens::sdk::sqlite_same_process_shm_process_port;
	using cxxlens::sdk::sqlite_same_process_shm_vfs_alias_registration_port;
	using cxxlens::sdk::sqlite_same_process_shm_vfs_alias_registration_test_peer;
	using cxxlens::sdk::sqlite_shm_lease_recovery_action;
	using cxxlens::sdk::sqlite_shm_lease_rejection_reason;
	using cxxlens::sdk::sqlite_shm_process_registry_handle;
	using cxxlens::sdk::sqlite_shm_registered_vfs_alias;

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

	struct runtime_owner_probe
	{
		explicit runtime_owner_probe(std::shared_ptr<std::atomic_int> destruction_count)
			: destruction_count_{std::move(destruction_count)}
		{
		}

		~runtime_owner_probe()
		{
			destruction_count_->fetch_add(1, std::memory_order_relaxed);
		}

		std::shared_ptr<std::atomic_int> destruction_count_;
	};

	struct fake_vfs
	{
		std::uint64_t marker{};
	};

	struct native_registry_fixture;
	thread_local native_registry_fixture* active_native_registry{};

	struct native_registry_fixture
	{
		std::string expected_name;
		void* expected_vfs{};
		void* registered{};
		void* registration_value{};
		int register_status{};
		int unregister_status{};
		int find_calls{};
		int register_calls{};
		int unregister_calls{};
		int throw_find_call{};
		bool throw_register{};
		bool throw_unregister{};
		bool install_on_register{true};
		bool clear_on_unregister{true};
		std::function<void()> on_register;
		std::function<void()> on_unregister;

		[[nodiscard]] static void* find(const char* name)
		{
			auto& fixture = current();
			++fixture.find_calls;
			if (fixture.throw_find_call == fixture.find_calls)
				throw std::runtime_error("find failure");
			if (name == nullptr || fixture.expected_name != name)
				return nullptr;
			return fixture.registered;
		}

		[[nodiscard]] static int register_vfs(void* value, const int make_default)
		{
			auto& fixture = current();
			++fixture.register_calls;
			if (fixture.on_register)
				fixture.on_register();
			if (fixture.throw_register)
				throw std::runtime_error("register failure");
			if (make_default != 0 || value != fixture.expected_vfs)
				return 99;
			if (fixture.install_on_register)
				fixture.registered =
					fixture.registration_value != nullptr ? fixture.registration_value : value;
			return fixture.register_status;
		}

		[[nodiscard]] static int unregister_vfs(void* value)
		{
			auto& fixture = current();
			++fixture.unregister_calls;
			if (fixture.on_unregister)
				fixture.on_unregister();
			if (fixture.throw_unregister)
				throw std::runtime_error("unregister failure");
			if (value != fixture.expected_vfs)
				return 99;
			if (fixture.clear_on_unregister)
				fixture.registered = nullptr;
			return fixture.unregister_status;
		}

	  private:
		[[nodiscard]] static native_registry_fixture& current()
		{
			if (active_native_registry == nullptr)
				throw std::runtime_error("no active native registry fixture");
			return *active_native_registry;
		}
	};

	class native_registry_scope
	{
	  public:
		explicit native_registry_scope(native_registry_fixture& fixture)
		{
			require(active_native_registry == nullptr, "native registry fixture is not nested");
			active_native_registry = &fixture;
		}

		~native_registry_scope()
		{
			active_native_registry = nullptr;
		}

		native_registry_scope(const native_registry_scope&) = delete;
		native_registry_scope& operator=(const native_registry_scope&) = delete;
	};

	[[nodiscard]] cxxlens::sdk::sqlite_shm_vfs_alias_lifecycle_binding
	make_binding(const sqlite_shm_process_registry_handle& process,
				 native_registry_fixture& fixture,
				 const std::uint64_t marker,
				 const std::shared_ptr<runtime_owner_probe>& owner)
	{
		return sqlite_same_process_shm_vfs_alias_registration_test_peer::binding(
			process,
			identity("test.vfs-alias.shared-cohort", 1U),
			identity("test.vfs-alias.alias-lifetime", marker),
			identity("test.vfs-alias.runtime-lifetime", marker),
			identity("test.vfs-alias.runtime-lifetime-pin", marker),
			owner,
			fixture.expected_name,
			fixture.expected_vfs,
			native_registry_fixture::find,
			native_registry_fixture::register_vfs,
			native_registry_fixture::unregister_vfs);
	}

	[[nodiscard]] sqlite_shm_process_registry_handle acquire_process()
	{
		auto acquired = sqlite_same_process_shm_process_port::acquire();
#if defined(__linux__) && defined(SYS_pidfd_open)
		require(acquired.has_value(), "qualified process port available");
		return std::move(acquired.value());
#else
		require(!acquired, "unsupported process port fails closed");
		return {};
#endif
	}

	[[nodiscard]] sqlite_shm_registered_vfs_alias
	register_clean_alias(const sqlite_shm_process_registry_handle& process,
						 native_registry_fixture& fixture,
						 const std::uint64_t marker,
						 std::weak_ptr<runtime_owner_probe>& weak_owner,
						 std::shared_ptr<std::atomic_int>& destruction_count)
	{
		destruction_count = std::make_shared<std::atomic_int>(0);
		auto owner = std::make_shared<runtime_owner_probe>(destruction_count);
		weak_owner = owner;
		auto registered = sqlite_same_process_shm_vfs_alias_registration_port::register_alias(
			make_binding(process, fixture, marker, owner));
		owner.reset();
		require(registered.has_value(), "register exact VFS alias");
		auto output = std::move(registered.value());
		require(output.valid(), "registered alias owner valid");
		require(!weak_owner.expired(), "registry pin retains runtime owner");
		return output;
	}

	void verify_clean_lifecycle_and_unique_epochs()
	{
#if defined(__linux__) && defined(SYS_pidfd_open)
		auto process = acquire_process();
		auto* const registry = process.registry();
		require(registry != nullptr, "process registry present");
		const auto baseline = registry->snapshot();
		std::optional<sqlite_backend_opaque_identity> previous_epoch;
		for (std::uint64_t marker = 1U; marker <= 2U; ++marker)
		{
			fake_vfs vfs{marker};
			native_registry_fixture fixture;
			fixture.expected_name = "cxxlens-test-alias-" + std::to_string(marker);
			fixture.expected_vfs = &vfs;
			native_registry_scope scope{fixture};
			std::weak_ptr<runtime_owner_probe> weak_owner;
			std::shared_ptr<std::atomic_int> destruction_count;
			auto alias =
				register_clean_alias(process, fixture, marker, weak_owner, destruction_count);
			const auto snapshot = registry->snapshot();
			require(snapshot.process_live && !snapshot.registry_quarantined,
					"clean registry remains live");
			require(snapshot.alias_record_count == baseline.alias_record_count + marker &&
						snapshot.registered_alias_count == 1U &&
						snapshot.detached_alias_tombstone_count ==
							baseline.detached_alias_tombstone_count + marker - 1U &&
						snapshot.quarantined_alias_count == 0U,
					"one live alias coexists with immutable detached history");
			require(alias.process_instance() == process.process_instance(),
					"registered alias retains process receipt");
			require(alias.vfs_implementation_identity() == &vfs &&
						alias.registered_vfs_name() == fixture.expected_name,
					"registered alias retains exact native tuple");
			require(alias.registration_epoch().profile ==
							"cxxlens.sqlite.shm.vfs-alias-registration-epoch.v1" &&
						!alias.registration_epoch().bytes.empty(),
					"registration epoch is closed and nonempty");
			if (previous_epoch)
				require(*previous_epoch != alias.registration_epoch(),
						"sequential alias registrations never reuse an epoch");
			previous_epoch = alias.registration_epoch();
			require(alias.unregister_alias().has_value(), "unregister exact VFS alias");
			require(!alias.valid(), "detached alias owner is no longer valid");
			require(fixture.registered == nullptr && fixture.register_calls == 1 &&
						fixture.unregister_calls == 1,
					"native register and unregister each execute exactly once");
			require(weak_owner.expired() && destruction_count->load(std::memory_order_relaxed) == 1,
					"clean detach releases exact runtime owner once");
			const auto detached = registry->snapshot();
			require(detached.alias_record_count == baseline.alias_record_count + marker &&
						detached.registered_alias_count == 0U &&
						detached.quarantined_alias_count == 0U &&
						detached.detached_alias_tombstone_count ==
							baseline.detached_alias_tombstone_count + marker,
					"clean detach preserves one immutable tombstone per lifecycle");
		}
#endif
	}

	void verify_preexisting_name_rejects_before_native_effect()
	{
#if defined(__linux__) && defined(SYS_pidfd_open)
		auto process = acquire_process();
		fake_vfs vfs{3U};
		native_registry_fixture fixture;
		fixture.expected_name = "cxxlens-test-preexisting";
		fixture.expected_vfs = &vfs;
		fixture.registered = &vfs;
		native_registry_scope scope{fixture};
		auto destruction_count = std::make_shared<std::atomic_int>(0);
		auto owner = std::make_shared<runtime_owner_probe>(destruction_count);
		std::weak_ptr<runtime_owner_probe> weak_owner = owner;
		const auto baseline = process.registry()->snapshot();
		auto rejected = sqlite_same_process_shm_vfs_alias_registration_port::register_alias(
			make_binding(process, fixture, 3U, owner));
		owner.reset();
		require(!rejected.has_value() &&
					rejected.error().reason == sqlite_shm_lease_rejection_reason::invalid_request,
				"preexisting name is rejected deterministically");
		require(fixture.find_calls == 1 && fixture.register_calls == 0,
				"preexisting name never reaches native registration");
		require(weak_owner.expired() && destruction_count->load(std::memory_order_relaxed) == 1,
				"pre-native rejection releases runtime owner");
		const auto snapshot = process.registry()->snapshot();
		require(snapshot.process_live && !snapshot.registry_quarantined &&
					snapshot.alias_record_count == baseline.alias_record_count &&
					snapshot.detached_alias_tombstone_count ==
						baseline.detached_alias_tombstone_count &&
					snapshot.reserved_alias_count == 0U && snapshot.registering_alias_count == 0U &&
					snapshot.registered_alias_count == 0U &&
					snapshot.unregistering_alias_count == 0U &&
					snapshot.quarantined_alias_count == 0U,
				"pre-native collision leaves registry state untouched");
#endif
	}

	void verify_unregister_waits_without_native_retry()
	{
#if defined(__linux__) && defined(SYS_pidfd_open)
		auto process = acquire_process();
		fake_vfs vfs{4U};
		native_registry_fixture fixture;
		fixture.expected_name = "cxxlens-test-retiring";
		fixture.expected_vfs = &vfs;
		native_registry_scope scope{fixture};
		std::weak_ptr<runtime_owner_probe> weak_owner;
		std::shared_ptr<std::atomic_int> destruction_count;
		auto alias = register_clean_alias(process, fixture, 4U, weak_owner, destruction_count);
		const cxxlens::sdk::sqlite_shm_lease_family_binding family{
			alias.process_instance(),
			alias.shared_runtime_vfs_cohort(),
			identity("test.vfs-alias.file-family", 4U)};
		auto family_pin = process.registry()->install_or_join_family(
			sqlite_same_process_shm_vfs_alias_registration_test_peer::alias_pin(alias), family);
		require(family_pin.has_value(), "install family under registered alias");
		auto waiting = alias.unregister_alias();
		require(!waiting.has_value() &&
					waiting.error().reason == sqlite_shm_lease_rejection_reason::retiring &&
					waiting.error().action ==
						sqlite_shm_lease_recovery_action::await_complete_attachment_gate_boundary,
				"unregister waits at exact registry quiescence boundary");
		require(fixture.unregister_calls == 0 && alias.valid(),
				"quiescence wait performs no native unregister");
		require(process.registry()->release_family(family_pin.value()).has_value(),
				"release exact family pin");
		require(alias.unregister_alias().has_value(),
				"same owner resumes before the one native unregister attempt");
		require(fixture.unregister_calls == 1 && weak_owner.expired() &&
					destruction_count->load(std::memory_order_relaxed) == 1,
				"resumed detach executes and releases exactly once");
#endif
	}

	void verify_same_thread_reentry_is_zero_effect()
	{
#if defined(__linux__) && defined(SYS_pidfd_open)
		auto process = acquire_process();
		fake_vfs vfs{5U};
		native_registry_fixture fixture;
		fixture.expected_name = "cxxlens-test-reentry";
		fixture.expected_vfs = &vfs;
		native_registry_scope scope{fixture};
		std::optional<sqlite_shm_lease_rejection_reason> register_reentry_reason;
		fixture.on_register = [&]
		{
			auto owner =
				std::make_shared<runtime_owner_probe>(std::make_shared<std::atomic_int>(0));
			auto nested = sqlite_same_process_shm_vfs_alias_registration_port::register_alias(
				make_binding(process, fixture, 50U, owner));
			require(!nested.has_value(), "same-thread registration reentry rejected");
			register_reentry_reason = nested.error().reason;
		};
		std::weak_ptr<runtime_owner_probe> weak_owner;
		std::shared_ptr<std::atomic_int> destruction_count;
		auto alias = register_clean_alias(process, fixture, 5U, weak_owner, destruction_count);
		require(register_reentry_reason == sqlite_shm_lease_rejection_reason::invalid_request &&
					fixture.register_calls == 1,
				"registration reentry has zero native effect");
		std::optional<sqlite_shm_lease_rejection_reason> unregister_reentry_reason;
		fixture.on_unregister = [&]
		{
			auto nested = alias.unregister_alias();
			require(!nested.has_value(), "same-thread unregister reentry rejected");
			unregister_reentry_reason = nested.error().reason;
		};
		require(alias.unregister_alias().has_value(), "outer unregister remains exact");
		require(unregister_reentry_reason == sqlite_shm_lease_rejection_reason::invalid_request &&
					fixture.unregister_calls == 1,
				"unregister reentry has zero native effect");
#endif
	}

	void verify_cross_thread_callback_reentry_is_bounded_zero_effect()
	{
#if defined(__linux__) && defined(SYS_pidfd_open)
		auto process = acquire_process();
		fake_vfs outer_vfs{6U};
		fake_vfs nested_vfs{60U};
		native_registry_fixture outer_fixture;
		outer_fixture.expected_name = "cxxlens-test-cross-thread-reentry";
		outer_fixture.expected_vfs = &outer_vfs;
		native_registry_fixture nested_fixture;
		nested_fixture.expected_name = "cxxlens-test-cross-thread-competitor";
		nested_fixture.expected_vfs = &nested_vfs;
		native_registry_scope outer_scope{outer_fixture};
		std::optional<sqlite_shm_lease_rejection_reason> nested_reason;
		std::optional<sqlite_shm_lease_recovery_action> nested_action;
		auto nested_destruction_count = std::make_shared<std::atomic_int>(0);
		auto nested_owner = std::make_shared<runtime_owner_probe>(nested_destruction_count);
		std::weak_ptr<runtime_owner_probe> weak_nested_owner = nested_owner;
		outer_fixture.on_register = [&]
		{
			std::thread competitor{
				[&, owner = nested_owner]() mutable
				{
					native_registry_scope nested_scope{nested_fixture};
					auto result =
						sqlite_same_process_shm_vfs_alias_registration_port::register_alias(
							make_binding(process, nested_fixture, 60U, owner));
					owner.reset();
					if (!result)
					{
						nested_reason = result.error().reason;
						nested_action = result.error().action;
					}
				}};
			competitor.join();
		};

		std::weak_ptr<runtime_owner_probe> weak_outer_owner;
		std::shared_ptr<std::atomic_int> outer_destruction_count;
		auto alias = register_clean_alias(
			process, outer_fixture, 6U, weak_outer_owner, outer_destruction_count);
		nested_owner.reset();
		require(nested_reason == sqlite_shm_lease_rejection_reason::retiring &&
					nested_action ==
						sqlite_shm_lease_recovery_action::await_complete_attachment_gate_boundary,
				"cross-thread callback competitor returns at the bounded gate");
		require(nested_fixture.find_calls == 0 && nested_fixture.register_calls == 0 &&
					nested_fixture.unregister_calls == 0,
				"bounded competitor performs no native observation or effect");
		require(weak_nested_owner.expired() &&
					nested_destruction_count->load(std::memory_order_relaxed) == 1,
				"bounded competitor releases its unregistered owner");
		require(alias.unregister_alias().has_value(),
				"outer lifecycle remains exact after bounded competitor");
		require(weak_outer_owner.expired() &&
					outer_destruction_count->load(std::memory_order_relaxed) == 1,
				"outer lifecycle releases its runtime owner once");
#endif
	}

	void verify_fork_during_native_callback_recovers_child_gate()
	{
#if defined(__linux__) && defined(SYS_pidfd_open)
		auto process = acquire_process();
		fake_vfs parent_vfs{7U};
		native_registry_fixture parent_fixture;
		parent_fixture.expected_name = "cxxlens-test-fork-held-gate-parent";
		parent_fixture.expected_vfs = &parent_vfs;
		native_registry_scope parent_scope{parent_fixture};
		bool child_completed{};
		parent_fixture.on_register = [&]
		{
			const auto child = ::fork();
			if (child == 0)
			{
				::alarm(3U);
				active_native_registry = nullptr;
				try
				{
					auto child_process = acquire_process();
					fake_vfs child_vfs{70U};
					native_registry_fixture child_fixture;
					child_fixture.expected_name = "cxxlens-test-fork-held-gate-child";
					child_fixture.expected_vfs = &child_vfs;
					native_registry_scope child_scope{child_fixture};
					std::weak_ptr<runtime_owner_probe> weak_owner;
					std::shared_ptr<std::atomic_int> destruction_count;
					auto child_alias = register_clean_alias(
						child_process, child_fixture, 70U, weak_owner, destruction_count);
					const bool exact = child_alias.unregister_alias().has_value() &&
						child_fixture.register_calls == 1 && child_fixture.unregister_calls == 1 &&
						weak_owner.expired() &&
						destruction_count->load(std::memory_order_relaxed) == 1;
					::_exit(exact ? EXIT_SUCCESS : EXIT_FAILURE);
				}
				catch (...)
				{
					::_exit(EXIT_FAILURE);
				}
			}
			if (child < 0)
				return;
			int status{};
			child_completed = ::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
				WEXITSTATUS(status) == EXIT_SUCCESS;
		};

		std::weak_ptr<runtime_owner_probe> weak_parent_owner;
		std::shared_ptr<std::atomic_int> parent_destruction_count;
		auto parent_alias = register_clean_alias(
			process, parent_fixture, 7U, weak_parent_owner, parent_destruction_count);
		require(parent_alias.unregister_alias().has_value(),
				"parent lifecycle completes after held-gate fork");
		require(child_completed,
				"fork child resets inherited active marker, gate, and epoch source");
		require(weak_parent_owner.expired() &&
					parent_destruction_count->load(std::memory_order_relaxed) == 1,
				"held-gate fork leaves parent owner lifecycle exact");
#endif
	}

	void observe_native_effect_concurrency(std::atomic_int& active,
										   std::atomic_int& maximum) noexcept
	{
		const auto current = active.fetch_add(1, std::memory_order_acq_rel) + 1;
		auto prior = maximum.load(std::memory_order_relaxed);
		while (prior < current &&
			   !maximum.compare_exchange_weak(
				   prior, current, std::memory_order_acq_rel, std::memory_order_relaxed))
		{
		}
		std::this_thread::sleep_for(std::chrono::milliseconds{2});
		active.fetch_sub(1, std::memory_order_acq_rel);
	}

	[[nodiscard]] std::uint64_t epoch_sequence(const sqlite_backend_opaque_identity& epoch)
	{
		require(epoch.bytes.size() >= sizeof(std::uint64_t), "epoch sequence present");
		std::uint64_t output{};
		for (std::size_t index{}; index < sizeof(output); ++index)
			output = (output << 8U) | std::to_integer<std::uint8_t>(epoch.bytes[index]);
		return output;
	}

	void verify_parallel_alias_lifecycles_are_serialized()
	{
#if defined(__linux__) && defined(SYS_pidfd_open)
		constexpr std::size_t alias_count = 8U;
		auto process = acquire_process();
		auto* const registry = process.registry();
		require(registry != nullptr, "parallel process registry present");
		const auto baseline = registry->snapshot();
		std::array<fake_vfs, alias_count> vfs{};
		std::array<native_registry_fixture, alias_count> fixtures{};
		std::array<std::shared_ptr<std::atomic_int>, alias_count> destruction_counts{};
		std::array<std::shared_ptr<runtime_owner_probe>, alias_count> owners{};
		std::array<std::weak_ptr<runtime_owner_probe>, alias_count> weak_owners{};
		std::array<std::uint64_t, alias_count> registration_sequences{};
		std::array<bool, alias_count> registered{};
		std::array<bool, alias_count> unregistered{};
		std::array<std::thread, alias_count> threads;
		std::atomic_int active_native_effects{};
		std::atomic_int maximum_native_effects{};
		std::barrier start{static_cast<std::ptrdiff_t>(alias_count + 1U)};
		std::barrier live{static_cast<std::ptrdiff_t>(alias_count + 1U)};
		std::barrier release{static_cast<std::ptrdiff_t>(alias_count + 1U)};

		for (std::size_t index{}; index < alias_count; ++index)
		{
			const auto marker = 1000U + static_cast<std::uint64_t>(index);
			vfs[index].marker = marker;
			fixtures[index].expected_name = "cxxlens-test-parallel-" + std::to_string(marker);
			fixtures[index].expected_vfs = &vfs[index];
			fixtures[index].on_register = [&]
			{
				observe_native_effect_concurrency(active_native_effects, maximum_native_effects);
			};
			fixtures[index].on_unregister = fixtures[index].on_register;
			destruction_counts[index] = std::make_shared<std::atomic_int>(0);
			owners[index] = std::make_shared<runtime_owner_probe>(destruction_counts[index]);
			weak_owners[index] = owners[index];
			threads[index] = std::thread{
				[&, index, marker, owner = owners[index]]() mutable
				{
					native_registry_scope scope{fixtures[index]};
					start.arrive_and_wait();
					auto result =
						sqlite_same_process_shm_vfs_alias_registration_port::register_alias(
							make_binding(process, fixtures[index], marker, owner));
					owner.reset();
					registered[index] = result.has_value();
					if (result)
						registration_sequences[index] =
							epoch_sequence(result.value().registration_epoch());
					live.arrive_and_wait();
					release.arrive_and_wait();
					if (result)
						unregistered[index] = result.value().unregister_alias().has_value();
				}};
		}
		for (auto& owner : owners)
			owner.reset();

		start.arrive_and_wait();
		live.arrive_and_wait();
		const auto live_snapshot = registry->snapshot();
		require(live_snapshot.alias_record_count == baseline.alias_record_count + alias_count &&
					live_snapshot.registered_alias_count == alias_count &&
					live_snapshot.quarantined_alias_count == 0U,
				"parallel registration publishes every exact alias once");
		require(maximum_native_effects.load(std::memory_order_acquire) == 1,
				"parallel native registration effects are serialized");
		release.arrive_and_wait();
		for (auto& thread : threads)
			thread.join();

		for (std::size_t index{}; index < alias_count; ++index)
		{
			require(registered[index] && unregistered[index],
					"parallel alias lifecycle completes exactly once");
			require(fixtures[index].register_calls == 1 && fixtures[index].unregister_calls == 1,
					"parallel native callbacks execute once per alias");
			require(weak_owners[index].expired() &&
						destruction_counts[index]->load(std::memory_order_relaxed) == 1,
					"parallel detach releases each runtime owner once");
			for (std::size_t prior{}; prior < index; ++prior)
				require(registration_sequences[prior] != registration_sequences[index],
						"parallel registration epochs are globally unique");
		}
		require(active_native_effects.load(std::memory_order_acquire) == 0 &&
					maximum_native_effects.load(std::memory_order_acquire) == 1,
				"parallel register and unregister effects never overlap");
		const auto detached = registry->snapshot();
		require(detached.alias_record_count == baseline.alias_record_count + alias_count &&
					detached.detached_alias_tombstone_count ==
						baseline.detached_alias_tombstone_count + alias_count &&
					detached.registered_alias_count == 0U && detached.quarantined_alias_count == 0U,
				"parallel clean detach preserves exact immutable history");
#endif
	}

	template <class Function>
	void require_child_success(const std::string_view profile, Function&& function)
	{
#if defined(__linux__) && defined(SYS_pidfd_open)
		const auto child = ::fork();
		require(child >= 0, profile);
		if (child == 0)
		{
			try
			{
				std::forward<Function>(function)();
				::_exit(EXIT_SUCCESS);
			}
			catch (...)
			{
				::_exit(EXIT_FAILURE);
			}
		}
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait negative lifecycle child");
		require(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS, profile);
#else
		(void)profile;
		(void)function;
#endif
	}

	void verify_lifecycle_sequence_exhaustion_is_sticky_and_zero_effect()
	{
		require_child_success(
			"registration epoch exhaustion child",
			[]
			{
				auto process = acquire_process();
				auto* const registry = process.registry();
				require(registry != nullptr, "exhaustion process registry present");
				const auto baseline = registry->snapshot();
				fake_vfs vfs{600U};
				native_registry_fixture fixture;
				fixture.expected_name = "cxxlens-test-registration-exhaustion";
				fixture.expected_vfs = &vfs;
				native_registry_scope scope{fixture};
				auto destruction_count = std::make_shared<std::atomic_int>(0);
				auto owner = std::make_shared<runtime_owner_probe>(destruction_count);
				std::weak_ptr<runtime_owner_probe> weak_owner = owner;
				sqlite_same_process_shm_vfs_alias_registration_test_peer::
					exhaust_lifecycle_sequence();
				auto rejected = sqlite_same_process_shm_vfs_alias_registration_port::register_alias(
					make_binding(process, fixture, 600U, owner));
				owner.reset();
				require(!rejected.has_value() &&
							rejected.error().reason ==
								sqlite_shm_lease_rejection_reason::generation_exhausted &&
							rejected.error().action ==
								sqlite_shm_lease_recovery_action::quarantine_no_retry,
						"registration epoch exhaustion is terminal");
				require(fixture.find_calls == 1 && fixture.register_calls == 0 &&
							fixture.unregister_calls == 0,
						"registration epoch exhaustion has zero native effect");
				require(weak_owner.expired() &&
							destruction_count->load(std::memory_order_relaxed) == 1,
						"pre-registration exhaustion releases unregistered owner");
				const auto snapshot = registry->snapshot();
				require(snapshot.alias_record_count == baseline.alias_record_count &&
							snapshot.detached_alias_tombstone_count ==
								baseline.detached_alias_tombstone_count &&
							snapshot.quarantined_alias_count == 0U,
						"pre-registration exhaustion leaves registry state untouched");
			});

		require_child_success(
			"unregistration epoch exhaustion child",
			[]
			{
				auto process = acquire_process();
				fake_vfs vfs{601U};
				native_registry_fixture fixture;
				fixture.expected_name = "cxxlens-test-unregistration-exhaustion";
				fixture.expected_vfs = &vfs;
				native_registry_scope scope{fixture};
				std::weak_ptr<runtime_owner_probe> weak_owner;
				std::shared_ptr<std::atomic_int> destruction_count;
				auto alias =
					register_clean_alias(process, fixture, 601U, weak_owner, destruction_count);
				sqlite_same_process_shm_vfs_alias_registration_test_peer::
					exhaust_lifecycle_sequence();
				auto rejected = alias.unregister_alias();
				require(!rejected.has_value() &&
							rejected.error().reason ==
								sqlite_shm_lease_rejection_reason::generation_exhausted &&
							rejected.error().action ==
								sqlite_shm_lease_recovery_action::quarantine_no_retry,
						"unregistration epoch exhaustion is terminal");
				require(!alias.valid() && fixture.unregister_calls == 0,
						"unregistration exhaustion forbids native unregister and retry");
				const auto replay = alias.unregister_alias();
				require(!replay.has_value() && fixture.unregister_calls == 0,
						"unregistration exhaustion remains sticky");
				const auto snapshot = process.registry()->snapshot();
				require(snapshot.alias_record_count == 1U &&
							snapshot.quarantined_alias_count == 1U && !weak_owner.expired() &&
							destruction_count->load(std::memory_order_relaxed) == 0,
						"registered owner is retained in quarantine after exhaustion");
			});
	}

	void require_quarantined_registration_failure(const int register_status,
												  const bool install_on_register,
												  void* registration_value,
												  const int throw_find_call,
												  const bool throw_register)
	{
		auto process = acquire_process();
		fake_vfs vfs{100U};
		fake_vfs wrong{101U};
		native_registry_fixture fixture;
		fixture.expected_name = "cxxlens-test-register-failure";
		fixture.expected_vfs = &vfs;
		fixture.register_status = register_status;
		fixture.install_on_register = install_on_register;
		fixture.registration_value = registration_value == reinterpret_cast<void*>(1U)
			? static_cast<void*>(&wrong)
			: registration_value;
		fixture.throw_find_call = throw_find_call;
		fixture.throw_register = throw_register;
		native_registry_scope scope{fixture};
		auto destruction_count = std::make_shared<std::atomic_int>(0);
		auto owner = std::make_shared<runtime_owner_probe>(destruction_count);
		std::weak_ptr<runtime_owner_probe> weak_owner = owner;
		auto rejected = sqlite_same_process_shm_vfs_alias_registration_port::register_alias(
			make_binding(process, fixture, 100U, owner));
		owner.reset();
		require(
			!rejected.has_value() &&
				rejected.error().reason == sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				rejected.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry,
			"native registration uncertainty is terminal");
		const auto snapshot = process.registry()->snapshot();
		require(snapshot.alias_record_count == 1U && snapshot.quarantined_alias_count == 1U,
				"failed native registration quarantines exact alias");
		require(!weak_owner.expired() && destruction_count->load(std::memory_order_relaxed) == 0,
				"quarantine retains runtime owner");
		require(fixture.register_calls == 1, "native register is never retried");
	}

	void verify_registration_failures_quarantine()
	{
		require_child_success("non-OK registration child",
							  []
							  {
								  require_quarantined_registration_failure(
									  1, false, nullptr, 0, false);
							  });
		require_child_success("post-registration mismatch child",
							  []
							  {
								  require_quarantined_registration_failure(
									  0, true, reinterpret_cast<void*>(1U), 0, false);
							  });
		require_child_success("post-registration find exception child",
							  []
							  {
								  require_quarantined_registration_failure(
									  0, true, nullptr, 2, false);
							  });
		require_child_success("registration exception child",
							  []
							  {
								  require_quarantined_registration_failure(
									  0, true, nullptr, 0, true);
							  });
	}

	void verify_drop_without_unregister_quarantines()
	{
		require_child_success(
			"abandoned registered alias child",
			[]
			{
				auto process = acquire_process();
				fake_vfs vfs{200U};
				native_registry_fixture fixture;
				fixture.expected_name = "cxxlens-test-abandon";
				fixture.expected_vfs = &vfs;
				native_registry_scope scope{fixture};
				std::weak_ptr<runtime_owner_probe> weak_owner;
				std::shared_ptr<std::atomic_int> destruction_count;
				{
					auto alias =
						register_clean_alias(process, fixture, 200U, weak_owner, destruction_count);
					require(alias.valid(), "alias valid before abandonment");
				}
				const auto snapshot = process.registry()->snapshot();
				require(snapshot.alias_record_count == 1U &&
							snapshot.quarantined_alias_count == 1U && fixture.unregister_calls == 0,
						"destructor quarantines without native unregister");
				require(!weak_owner.expired() &&
							destruction_count->load(std::memory_order_relaxed) == 0,
						"abandonment retains native VFS owner");
			});
	}

	void require_quarantined_unregistration_failure(const int unregister_status,
													const bool clear_on_unregister,
													const bool throw_unregister,
													const int throw_find_call)
	{
		auto process = acquire_process();
		fake_vfs vfs{300U};
		native_registry_fixture fixture;
		fixture.expected_name = "cxxlens-test-unregister-failure";
		fixture.expected_vfs = &vfs;
		fixture.unregister_status = unregister_status;
		fixture.clear_on_unregister = clear_on_unregister;
		fixture.throw_unregister = throw_unregister;
		fixture.throw_find_call = throw_find_call;
		native_registry_scope scope{fixture};
		std::weak_ptr<runtime_owner_probe> weak_owner;
		std::shared_ptr<std::atomic_int> destruction_count;
		auto alias = register_clean_alias(process, fixture, 300U, weak_owner, destruction_count);
		auto rejected = alias.unregister_alias();
		require(
			!rejected.has_value() &&
				rejected.error().reason == sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				rejected.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry,
			"native unregister uncertainty is terminal");
		require(!alias.valid(), "failed unregister consumes owner into quarantine");
		auto replay = alias.unregister_alias();
		require(!replay.has_value() && fixture.unregister_calls == 1,
				"failed native unregister is never retried");
		const auto snapshot = process.registry()->snapshot();
		require(snapshot.alias_record_count == 1U && snapshot.quarantined_alias_count == 1U,
				"failed native unregister quarantines exact alias");
		require(!weak_owner.expired() && destruction_count->load(std::memory_order_relaxed) == 0,
				"unregister quarantine retains runtime owner");
	}

	void verify_unregistration_failures_quarantine()
	{
		require_child_success("non-OK unregister child",
							  []
							  {
								  require_quarantined_unregistration_failure(1, false, false, 0);
							  });
		require_child_success("post-unregister discovery child",
							  []
							  {
								  require_quarantined_unregistration_failure(0, false, false, 0);
							  });
		require_child_success("unregister exception child",
							  []
							  {
								  require_quarantined_unregistration_failure(0, true, true, 0);
							  });
		require_child_success("post-unregister find exception child",
							  []
							  {
								  require_quarantined_unregistration_failure(0, true, false, 3);
							  });
	}

	void verify_fork_child_cannot_unregister_inherited_alias()
	{
#if defined(__linux__) && defined(SYS_pidfd_open)
		auto process = acquire_process();
		fake_vfs vfs{400U};
		native_registry_fixture fixture;
		fixture.expected_name = "cxxlens-test-fork";
		fixture.expected_vfs = &vfs;
		native_registry_scope scope{fixture};
		std::weak_ptr<runtime_owner_probe> weak_owner;
		std::shared_ptr<std::atomic_int> destruction_count;
		auto alias = register_clean_alias(process, fixture, 400U, weak_owner, destruction_count);
		const auto child = ::fork();
		require(child >= 0, "fork registered alias child");
		if (child == 0)
		{
			auto rejected = alias.unregister_alias();
			const bool exact = !rejected.has_value() &&
				rejected.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
				fixture.unregister_calls == 0 && !alias.valid();
			::_exit(exact ? EXIT_SUCCESS : EXIT_FAILURE);
		}
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait inherited alias child");
		require(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
				"fork child rejects inherited alias before native unregister");
		require(alias.valid() && fixture.unregister_calls == 0,
				"parent retains exact registered alias");
		require(alias.unregister_alias().has_value(), "parent unregisters exact alias once");
		require(fixture.unregister_calls == 1 && weak_owner.expired() &&
					destruction_count->load(std::memory_order_relaxed) == 1,
				"child invalidation does not affect parent lifecycle");
#endif
	}
} // namespace

int main()
{
	try
	{
		verify_clean_lifecycle_and_unique_epochs();
		verify_preexisting_name_rejects_before_native_effect();
		verify_unregister_waits_without_native_retry();
		verify_same_thread_reentry_is_zero_effect();
		verify_cross_thread_callback_reentry_is_bounded_zero_effect();
		verify_fork_during_native_callback_recovers_child_gate();
		verify_parallel_alias_lifecycles_are_serialized();
		verify_lifecycle_sequence_exhaustion_is_sticky_and_zero_effect();
		verify_registration_failures_quarantine();
		verify_drop_without_unregister_quarantines();
		verify_unregistration_failures_quarantine();
		verify_fork_child_cannot_unregister_inherited_alias();
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
