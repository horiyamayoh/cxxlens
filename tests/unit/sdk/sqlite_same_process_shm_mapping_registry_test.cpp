#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include <barrier>
#include <sys/wait.h>
#include <unistd.h>

#include "sdk/sqlite_same_process_shm_mapping_registry_internal.hpp"
#include "sdk/sqlite_writer_shm_mapping_epoch_internal.hpp"

namespace cxxlens::sdk
{
	class sqlite_same_process_shm_registry_test_peer
	{
	  public:
		[[nodiscard]] static sqlite_shm_registry_process_owner
		process_owner(sqlite_backend_opaque_identity process_instance)
		{
			return sqlite_shm_registry_process_owner{std::move(process_instance)};
		}

		[[nodiscard]] static sqlite_shm_registry_process_owner
		replay_owner(const sqlite_shm_registry_process_owner& source)
		{
			return {
				source.process_instance_,
				source.seal_,
				source.process_epoch_,
			};
		}

		[[nodiscard]] static sqlite_shm_lease_result<
			std::unique_ptr<sqlite_same_process_shm_mapping_registry>>
		create_with_generation(sqlite_shm_registry_process_owner owner,
							   const std::uint64_t first_generation)
		{
			return sqlite_same_process_shm_mapping_registry::create_with_generation_for_testing(
				std::move(owner), first_generation);
		}

		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_registry_runtime_lifetime_pin>
		adopt_runtime_lifetime(sqlite_same_process_shm_mapping_registry& registry,
							   sqlite_backend_opaque_identity identity,
							   sqlite_backend_opaque_identity pin_identity,
							   std::shared_ptr<void> owner)
		{
			return registry.adopt_runtime_lifetime_for_testing(
				std::move(identity), std::move(pin_identity), std::move(owner));
		}

		[[nodiscard]] static std::pair<sqlite_writer_shm_native_lifetime_revoker,
									   sqlite_writer_shm_native_lifetime_source>
		native_lifetime_source(const sqlite_writer_shm_native_lifetime_role role,
							   sqlite_backend_opaque_identity native_lifetime_identity,
							   sqlite_backend_opaque_identity semantic_receipt,
							   std::optional<sqlite_backend_opaque_identity> native_xopen_receipt,
							   const std::shared_ptr<void>& retained_owner)
		{
			return sqlite_writer_shm_native_lifetime_test_factory::create_source(
				role,
				std::move(native_lifetime_identity),
				std::move(semantic_receipt),
				std::move(native_xopen_receipt),
				retained_owner);
		}

		[[nodiscard]] static sqlite_shm_registry_alias_binding
		alias_binding(sqlite_backend_opaque_identity process_instance,
					  sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
					  sqlite_backend_opaque_identity alias_lifetime,
					  sqlite_shm_registry_runtime_lifetime_pin runtime_lifetime)
		{
			return {
				std::move(process_instance),
				std::move(shared_runtime_vfs_cohort),
				std::move(alias_lifetime),
				std::move(runtime_lifetime),
			};
		}

		[[nodiscard]] static sqlite_shm_verified_alias_registration_receipt
		registration_receipt(sqlite_backend_opaque_identity process_instance,
							 sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
							 sqlite_backend_opaque_identity alias_lifetime,
							 sqlite_backend_opaque_identity runtime_lifetime_identity,
							 sqlite_backend_opaque_identity runtime_lifetime_pin_identity,
							 sqlite_backend_opaque_identity registration_epoch)
		{
			return {
				std::move(process_instance),
				std::move(shared_runtime_vfs_cohort),
				std::move(alias_lifetime),
				std::move(runtime_lifetime_identity),
				std::move(runtime_lifetime_pin_identity),
				std::move(registration_epoch),
			};
		}

		[[nodiscard]] static sqlite_shm_verified_alias_unregistration_receipt
		unregistration_receipt(sqlite_backend_opaque_identity process_instance,
							   sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
							   sqlite_backend_opaque_identity alias_lifetime,
							   sqlite_backend_opaque_identity runtime_lifetime_identity,
							   sqlite_backend_opaque_identity runtime_lifetime_pin_identity,
							   sqlite_backend_opaque_identity registration_epoch,
							   sqlite_backend_opaque_identity unregistration_epoch)
		{
			return {
				std::move(process_instance),
				std::move(shared_runtime_vfs_cohort),
				std::move(alias_lifetime),
				std::move(runtime_lifetime_identity),
				std::move(runtime_lifetime_pin_identity),
				std::move(registration_epoch),
				std::move(unregistration_epoch),
			};
		}

		static void
		invalidate_process_instance(sqlite_same_process_shm_mapping_registry& registry) noexcept
		{
			registry.invalidate_process_instance_for_testing();
		}

		static void lock_registry_mutex(sqlite_same_process_shm_mapping_registry& registry)
		{
			registry.lock_registry_mutex_for_fork_testing();
		}

		static void
		unlock_registry_mutex(sqlite_same_process_shm_mapping_registry& registry) noexcept
		{
			registry.unlock_registry_mutex_for_fork_testing();
		}

		[[nodiscard]] static bool
		inject_duplicate_family(sqlite_same_process_shm_mapping_registry& registry,
								const sqlite_shm_lease_family_binding& family) noexcept
		{
			return registry.inject_duplicate_family_for_testing(family);
		}

		static void exhaust_counters(sqlite_same_process_shm_mapping_registry& registry) noexcept
		{
			registry.exhaust_registry_counters_for_testing();
		}

		static void
		exhaust_counter(sqlite_same_process_shm_mapping_registry& registry,
						const detail::sqlite_shm_registry_counter_for_testing counter) noexcept
		{
			registry.exhaust_registry_counter_for_testing(counter);
		}

		[[nodiscard]] static sqlite_same_process_shm_mapping_lease_coordinator*
		coordinator(sqlite_same_process_shm_mapping_registry& registry,
					const sqlite_shm_registry_activity_pin& activity) noexcept
		{
			return registry.coordinator_for_activity_for_testing(activity);
		}

		[[nodiscard]] static sqlite_same_process_shm_mapping_lease_coordinator*
		coordinator(sqlite_same_process_shm_mapping_registry& registry,
					const sqlite_shm_lease_family_binding& family) noexcept
		{
			return registry.coordinator_for_family_for_testing(family);
		}

		[[nodiscard]] static bool
		activity_seal_matches(sqlite_same_process_shm_mapping_registry& registry,
							  const sqlite_shm_registry_activity_seal& seal,
							  const sqlite_backend_opaque_identity& process_instance,
							  const sqlite_shm_lease_family_binding& family,
							  const sqlite_backend_opaque_identity& alias_lifetime) noexcept
		{
			return registry.activity_seal_matches_for_testing(
				seal, process_instance, family, alias_lifetime);
		}

		[[nodiscard]] static std::uint64_t state_destruction_count() noexcept
		{
			return sqlite_same_process_shm_mapping_registry::state_destruction_count_for_testing();
		}

		[[nodiscard]] static const void* generation_source_identity(
			const sqlite_same_process_shm_mapping_registry& registry) noexcept
		{
			return registry.generation_source_identity_for_testing();
		}
	};

	class sqlite_same_process_shm_lease_test_peer
	{
	  public:
		[[nodiscard]] static sqlite_shm_verified_writer_post_map_receipt
		writer_map(sqlite_shm_writer_map_request request,
				   sqlite_backend_opaque_identity open_epoch,
				   const sqlite_shm_mapping_tuple mapping,
				   const sqlite_shm_writer_extend_pair pair,
				   sqlite_backend_opaque_identity effect)
		{
			return {
				std::move(request),
				std::move(open_epoch),
				mapping,
				pair,
				std::move(effect),
			};
		}

		[[nodiscard]] static sqlite_shm_verified_writer_eligibility_receipt
		eligibility(sqlite_shm_lease_family_binding family,
					sqlite_backend_opaque_identity connection,
					sqlite_backend_opaque_identity open_epoch,
					sqlite_backend_effect_arm_receipt effect,
					sqlite_backend_opaque_identity complete_gate)
		{
			return {
				std::move(family),
				std::move(connection),
				std::move(open_epoch),
				std::move(effect),
				std::move(complete_gate),
			};
		}

		static void fail_next_writer_attachment_seal_transition(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_writer_attachment_seal_failure_for_testing();
		}

		static void fail_next_registry_incoming_liveness(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_registry_writer_incoming_liveness_loss_for_testing();
		}

		static void fail_next_registry_existing_liveness(
			sqlite_same_process_shm_mapping_lease_coordinator& coordinator) noexcept
		{
			coordinator.inject_registry_writer_existing_liveness_loss_for_testing();
		}
	};
} // namespace cxxlens::sdk

namespace
{
	using namespace cxxlens::sdk;

	static_assert(!std::is_default_constructible_v<sqlite_shm_registry_process_owner>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_registry_process_owner>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_registry_process_owner>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_registry_runtime_lifetime_pin>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_registry_runtime_lifetime_pin>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_registry_alias_pin>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_registry_family_pin>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_registry_activity_pin>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_registry_activity_pin>);
	static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_member_authority>);
	static_assert(!std::is_move_assignable_v<sqlite_shm_writer_member_authority>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_writer_member_authority>);
	static_assert(!std::is_default_constructible_v<sqlite_shm_registry_activity_seal>);
	static_assert(std::is_nothrow_copy_constructible_v<sqlite_shm_registry_activity_seal>);
	static_assert(std::is_nothrow_destructible_v<sqlite_shm_registry_activity_seal>);
	static_assert(!std::is_default_constructible_v<sqlite_shm_verified_alias_registration_receipt>);
	static_assert(
		!std::is_default_constructible_v<sqlite_shm_verified_alias_unregistration_receipt>);

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error{std::string{message}};
	}

	[[nodiscard]] sqlite_backend_opaque_identity identity(const std::string_view profile,
														  const std::uint8_t marker)
	{
		return {std::string{profile}, {static_cast<std::byte>(marker)}};
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

	struct registry_fixture
	{
		sqlite_backend_opaque_identity process_instance;
		sqlite_backend_opaque_identity cohort;
		sqlite_backend_opaque_identity alias_lifetime;
		sqlite_backend_opaque_identity runtime_identity;
		sqlite_backend_opaque_identity runtime_pin_identity;
		sqlite_backend_opaque_identity registration_epoch;
		sqlite_backend_opaque_identity unregistration_epoch;
		sqlite_shm_lease_family_binding family;
		std::shared_ptr<std::atomic_int> owner_destruction_count;
		std::weak_ptr<runtime_owner_probe> owner;
		std::unique_ptr<sqlite_same_process_shm_mapping_registry> registry;
		std::optional<sqlite_shm_registry_alias_pin> alias;
		std::optional<sqlite_shm_registry_family_pin> family_pin;
		std::optional<sqlite_shm_registry_activity_pin> activity;
	};

	struct alias_member
	{
		sqlite_backend_opaque_identity alias_lifetime;
		sqlite_backend_opaque_identity runtime_identity;
		sqlite_backend_opaque_identity runtime_pin_identity;
		sqlite_backend_opaque_identity registration_epoch;
		sqlite_backend_opaque_identity unregistration_epoch;
		std::shared_ptr<std::atomic_int> owner_destruction_count;
		std::weak_ptr<runtime_owner_probe> owner;
		std::optional<sqlite_shm_registry_alias_pin> alias;
		std::optional<sqlite_shm_registry_family_pin> family_pin;
	};

	[[nodiscard]] alias_member
	register_alias(sqlite_same_process_shm_mapping_registry& registry,
				   const sqlite_backend_opaque_identity& process_instance,
				   const sqlite_backend_opaque_identity& cohort,
				   const std::uint8_t marker,
				   const std::shared_ptr<runtime_owner_probe>& runtime_owner,
				   const sqlite_shm_lease_family_binding* family = nullptr)
	{
		alias_member member;
		member.alias_lifetime = identity("test.registry.alias", marker);
		member.runtime_identity = identity("test.registry.runtime", marker);
		member.runtime_pin_identity = identity("test.registry.runtime-pin", marker);
		member.registration_epoch = identity("test.registry.registration", marker);
		member.unregistration_epoch = identity("test.registry.unregistration", marker);
		member.owner_destruction_count = runtime_owner->destruction_count_;
		member.owner = runtime_owner;

		auto adopted = sqlite_same_process_shm_registry_test_peer::adopt_runtime_lifetime(
			registry, member.runtime_identity, member.runtime_pin_identity, runtime_owner);
		require(adopted.has_value(), "adopt member runtime owner");
		auto binding = sqlite_same_process_shm_registry_test_peer::alias_binding(
			process_instance, cohort, member.alias_lifetime, std::move(adopted.value()));
		auto reserved = registry.reserve_alias(std::move(binding));
		require(reserved.has_value(), "reserve member alias");
		member.alias.emplace(std::move(reserved.value()));
		require(registry.begin_alias_register(*member.alias).has_value(),
				"arm member alias registration");
		const auto registration = sqlite_same_process_shm_registry_test_peer::registration_receipt(
			process_instance,
			cohort,
			member.alias_lifetime,
			member.runtime_identity,
			member.runtime_pin_identity,
			member.registration_epoch);
		require(registry.confirm_alias_registered(*member.alias, registration).has_value(),
				"confirm member alias registration");
		if (family != nullptr)
		{
			auto pinned = registry.install_or_join_family(*member.alias, *family);
			require(pinned.has_value(), "pin member family");
			member.family_pin.emplace(std::move(pinned.value()));
		}
		return member;
	}

	void unregister_alias(sqlite_same_process_shm_mapping_registry& registry,
						  const sqlite_backend_opaque_identity& process_instance,
						  const sqlite_backend_opaque_identity& cohort,
						  alias_member& member)
	{
		if (member.family_pin)
		{
			require(registry.release_family(*member.family_pin).has_value(),
					"release member family");
			member.family_pin.reset();
		}
		require(registry.begin_alias_unregister(*member.alias).has_value(),
				"begin member alias unregister");
		require(registry.poll_alias_unregister(*member.alias).has_value(),
				"poll member alias unregister");
		const auto receipt = sqlite_same_process_shm_registry_test_peer::unregistration_receipt(
			process_instance,
			cohort,
			member.alias_lifetime,
			member.runtime_identity,
			member.runtime_pin_identity,
			member.registration_epoch,
			member.unregistration_epoch);
		require(registry.confirm_alias_unregistered(*member.alias, receipt).has_value(),
				"confirm member alias unregister");
		member.alias.reset();
	}

	[[nodiscard]] sqlite_backend_effect_arm_receipt
	effect_gate(const sqlite_backend_opaque_identity& connection, const std::uint8_t marker)
	{
		return {
			"test.registry.effect-gate",
			identity("test.registry.capability", marker),
			connection,
			"/test/registry.db",
			identity("test.registry.prerequisite", marker),
			identity("test.registry.validation", marker),
			sqlite_backend_effect_stage::fully_armed,
			marker,
			false,
		};
	}

	[[nodiscard]] sqlite_shm_callback_execution_receipt callback(const std::uint8_t marker,
																 const std::uint64_t depth = 0U)
	{
		return {
			identity("test.registry.thread", marker),
			depth,
			identity("test.registry.callback", marker),
		};
	}

	[[nodiscard]] sqlite_shm_native_attachment_identity
	writer_attachment(const sqlite_shm_lease_family_binding& family,
					  const sqlite_backend_opaque_identity& alias_lifetime,
					  const sqlite_backend_opaque_identity& connection,
					  const sqlite_backend_opaque_identity& open_epoch,
					  const std::uint8_t marker)
	{
		auto attachment = sqlite_shm_native_attachment_identity::bind(
			family,
			alias_lifetime,
			connection,
			identity("test.registry.main-native-file", marker),
			identity("test.registry.main-xopen", marker),
			open_epoch,
			identity("test.registry.native-callback-cohort", marker),
			identity("test.registry.attachment-epoch", marker));
		require(attachment.has_value(), "bind registry writer attachment");
		return std::move(attachment.value());
	}

	[[nodiscard]] sqlite_shm_writer_map_request
	writer_request(const sqlite_shm_lease_family_binding& family,
				   const sqlite_backend_opaque_identity& alias_lifetime,
				   const sqlite_backend_opaque_identity& connection,
				   const sqlite_backend_opaque_identity& open_epoch,
				   const std::uint8_t marker)
	{
		return {
			family,
			alias_lifetime,
			connection,
			writer_attachment(family, alias_lifetime, connection, open_epoch, marker),
			callback(marker),
			0,
			4096,
			1,
		};
	}

	[[nodiscard]] sqlite_shm_mapping_tuple mapping(const volatile void* page)
	{
		return {0, 4096, 0U, 4096U, page, 4096U};
	}

	class inert_bridge_observation_port final
		: public sqlite_writer_shm_mapping_epoch_observation_port
	{
	  public:
		[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_post_observation>
		observe_after_native_map(const sqlite_writer_shm_mapping_epoch_binding&,
								 const sqlite_writer_shm_stat_census&,
								 const volatile void*) override
		{
			return sqlite_shm_lease_rejection{
				sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
				sqlite_shm_lease_recovery_action::quarantine_no_retry};
		}
	};

	class bridge_epoch_port final : public sqlite_writer_shm_mapping_epoch_port
	{
	  public:
		explicit bridge_epoch_port(const std::uint8_t marker)
			: marker_{marker}, observation_{std::make_shared<inert_bridge_observation_port>()}
		{
		}

	  protected:
		[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_preparation>
		arm_watch_before_pre_stat(const sqlite_writer_shm_mapping_epoch_request&) override
		{
			return sqlite_writer_shm_mapping_epoch_preparation{
				identity("test.registry.bridge-epoch", marker_),
				identity("test.registry.bridge-watch", marker_),
				sqlite_writer_shm_stat_census{sqlite_writer_shm_entry_state::absent,
											  identity("test.registry.bridge-parent", marker_),
											  identity("test.registry.bridge-filesystem", marker_),
											  identity("test.registry.bridge-mount", marker_),
											  std::nullopt,
											  std::nullopt,
											  0U},
				observation_};
		}

	  private:
		std::uint8_t marker_{};
		std::shared_ptr<inert_bridge_observation_port> observation_;
	};

	struct bridge_native_source
	{
		sqlite_writer_shm_native_lifetime_revoker revoker;
		sqlite_writer_shm_native_lifetime_source source;
		std::shared_ptr<int> owner;
	};

	struct bridge_epoch_sources
	{
		sqlite_backend_opaque_identity retained_parent_receipt;
		sqlite_backend_opaque_identity wal_file_receipt;
		sqlite_backend_opaque_identity wal_xopen_receipt;
		sqlite_backend_opaque_identity shm_attachment_receipt;
		sqlite_backend_opaque_identity expected_shm_leaf;
		bridge_native_source parent;
		bridge_native_source main;
		bridge_native_source wal;
		bridge_native_source shm;

		[[nodiscard]] sqlite_writer_shm_mapping_epoch_activation
		arm(const sqlite_shm_writer_map_request& request, const std::uint8_t marker)
		{
			auto parent_pin = parent.source.mint_pin();
			auto main_pin = main.source.mint_pin();
			auto wal_pin = wal.source.mint_pin();
			auto shm_pin = shm.source.mint_pin();
			require(parent_pin && main_pin && wal_pin && shm_pin,
					"mint exact bridge native lifetime pins");
			bridge_epoch_port port{marker};
			auto activation = port.arm(sqlite_writer_shm_mapping_epoch_request{
				sqlite_writer_shm_mapping_epoch_binding{request,
														request.caller_extend,
														expected_shm_leaf,
														retained_parent_receipt,
														wal_file_receipt,
														wal_xopen_receipt,
														shm_attachment_receipt},
				std::move(*parent_pin),
				std::move(*main_pin),
				std::move(*wal_pin),
				std::move(*shm_pin)});
			require(activation.has_value(), "arm exact registry bridge epoch");
			return std::move(*activation);
		}
	};

	[[nodiscard]] bridge_native_source
	make_bridge_native_source(const sqlite_writer_shm_native_lifetime_role role,
							  const sqlite_backend_opaque_identity& lifetime_identity,
							  const sqlite_backend_opaque_identity& semantic_receipt,
							  std::optional<sqlite_backend_opaque_identity> xopen,
							  const int owner_marker)
	{
		auto owner = std::make_shared<int>(owner_marker);
		auto source = sqlite_same_process_shm_registry_test_peer::native_lifetime_source(
			role, lifetime_identity, semantic_receipt, std::move(xopen), owner);
		return {std::move(source.first), std::move(source.second), std::move(owner)};
	}

	[[nodiscard]] std::shared_ptr<bridge_epoch_sources>
	make_bridge_epoch_sources(const sqlite_shm_writer_map_request& request,
							  const std::uint8_t marker)
	{
		const auto retained_parent = identity("test.registry.bridge-retained-parent", marker);
		const auto wal_file = identity("test.registry.bridge-wal-file", marker);
		const auto wal_xopen = identity("test.registry.bridge-wal-xopen", marker);
		const auto shm_attachment = identity("test.registry.bridge-shm-attachment", marker);
		return std::make_shared<bridge_epoch_sources>(bridge_epoch_sources{
			retained_parent,
			wal_file,
			wal_xopen,
			shm_attachment,
			identity("test.registry.bridge-shm-leaf", marker),
			make_bridge_native_source(sqlite_writer_shm_native_lifetime_role::retained_parent,
									  identity("test.registry.bridge-parent-lifetime", marker),
									  retained_parent,
									  std::nullopt,
									  1),
			make_bridge_native_source(sqlite_writer_shm_native_lifetime_role::main_database,
									  identity("test.registry.bridge-main-lifetime", marker),
									  request.attachment.main_native_file_receipt(),
									  request.attachment.main_xopen_receipt(),
									  2),
			make_bridge_native_source(sqlite_writer_shm_native_lifetime_role::write_ahead_log,
									  identity("test.registry.bridge-wal-lifetime", marker),
									  wal_file,
									  wal_xopen,
									  3),
			make_bridge_native_source(
				sqlite_writer_shm_native_lifetime_role::shared_memory_attachment,
				identity("test.registry.bridge-shm-lifetime", marker),
				shm_attachment,
				std::nullopt,
				4)});
	}

	struct bridge_epoch
	{
		sqlite_writer_shm_mapping_epoch_activation activation;
		std::shared_ptr<bridge_epoch_sources> sources;
	};

	[[nodiscard]] bridge_epoch make_bridge_epoch(const sqlite_shm_writer_map_request& request,
												 const std::uint8_t marker)
	{
		auto sources = make_bridge_epoch_sources(request, marker);
		auto activation = sources->arm(request, marker);
		return {std::move(activation), std::move(sources)};
	}

	[[nodiscard]] sqlite_shm_writer_eligibility
	install_eligibility(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
						const sqlite_shm_lease_family_binding& family,
						const sqlite_backend_opaque_identity& connection,
						const sqlite_backend_opaque_identity& open_epoch,
						const std::uint8_t marker)
	{
		const auto receipt = sqlite_same_process_shm_lease_test_peer::eligibility(
			family,
			connection,
			open_epoch,
			effect_gate(connection, marker),
			identity("test.registry.complete-gate", marker));
		auto installed = coordinator.install_writer_eligibility(receipt);
		require(installed.has_value(), "install registry writer eligibility");
		return std::move(installed.value());
	}

	[[nodiscard]] sqlite_shm_pending_mapping
	install_pending(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
					const sqlite_shm_writer_map_request& request,
					const sqlite_backend_opaque_identity& open_epoch,
					const sqlite_shm_mapping_tuple& mapped,
					const std::uint8_t marker)
	{
		auto begun = coordinator.begin_writer_map(request);
		require(begun.has_value(), "begin registry writer map");
		auto inflight = std::move(begun.value());
		auto native_receipt = sqlite_writer_shm_native_map_receipt_validator::validate(
			inflight, 0, mapped.native_mapping);
		require(native_receipt.has_value(), "validate registry native writer map");
		auto post_native =
			coordinator.record_writer_native_mapping(inflight, native_receipt.value());
		require(post_native.has_value() && !inflight.valid(), "record registry native writer map");
		const auto post_receipt = sqlite_same_process_shm_lease_test_peer::writer_map(
			request,
			open_epoch,
			mapped,
			sqlite_shm_writer_extend_pair::one_one,
			identity("test.registry.writer-effect", marker));
		auto pending = coordinator.install_pending(post_native.value(), post_receipt);
		require(pending.has_value() && !post_native.value().valid(),
				"install registry pending writer");
		return std::move(pending.value());
	}

	[[nodiscard]] sqlite_shm_writer_holder
	promote_writer(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
				   sqlite_shm_pending_mapping& pending,
				   const sqlite_shm_writer_eligibility& eligibility)
	{
		auto holder = coordinator.promote_writer(pending, eligibility);
		require(holder.has_value() && !pending.valid(), "promote registry writer holder");
		return std::move(holder.value());
	}

	void retire_writer(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
					   sqlite_shm_writer_holder& holder,
					   const std::uint8_t marker)
	{
		const auto release_callback = callback(marker);
		auto retirement = coordinator.release_writer_holder(holder, release_callback);
		require(retirement.has_value() &&
					retirement->decision() == sqlite_shm_writer_retirement_decision::ready &&
					!holder.valid(),
				"retire registry writer holder");
		require(coordinator
					.complete_writer_cleanup(retirement->cleanup(),
											 release_callback,
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"complete registry writer retirement cleanup");
	}

	void cleanup_pending(sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
						 sqlite_shm_pending_mapping& pending,
						 const std::uint8_t marker)
	{
		const auto cleanup_callback = callback(marker);
		auto cleanup = coordinator.begin_writer_cleanup(pending, cleanup_callback);
		require(cleanup.has_value() && !pending.valid(), "begin exhausted pending cleanup");
		require(coordinator
					.complete_writer_cleanup(cleanup.value(),
											 cleanup_callback,
											 sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"complete exhausted pending cleanup");
	}

	[[nodiscard]] std::unique_ptr<sqlite_same_process_shm_mapping_registry>
	make_registry(const sqlite_backend_opaque_identity& process_instance)
	{
		auto owner = sqlite_same_process_shm_registry_test_peer::process_owner(process_instance);
		auto created = sqlite_same_process_shm_mapping_registry::create(std::move(owner));
		require(created.has_value(), "create standalone registry");
		return std::move(created.value());
	}

	[[nodiscard]] registry_fixture make_fixture(const std::uint8_t marker,
												const bool acquire_activity)
	{
		registry_fixture fixture;
		fixture.process_instance = identity("test.registry.process", marker);
		fixture.cohort = identity("test.registry.cohort", marker);
		fixture.alias_lifetime = identity("test.registry.alias", marker);
		fixture.runtime_identity = identity("test.registry.runtime", marker);
		fixture.runtime_pin_identity = identity("test.registry.runtime-pin", marker);
		fixture.registration_epoch = identity("test.registry.registration", marker);
		fixture.unregistration_epoch = identity("test.registry.unregistration", marker);
		fixture.family = {
			fixture.process_instance,
			fixture.cohort,
			identity("test.registry.file-family", marker),
		};
		fixture.owner_destruction_count = std::make_shared<std::atomic_int>(0);

		auto process_owner =
			sqlite_same_process_shm_registry_test_peer::process_owner(fixture.process_instance);
		auto created = sqlite_same_process_shm_mapping_registry::create(std::move(process_owner));
		require(created.has_value(), "create registry fixture");
		fixture.registry = std::move(created.value());

		auto runtime_owner = std::make_shared<runtime_owner_probe>(fixture.owner_destruction_count);
		fixture.owner = runtime_owner;
		auto adopted = sqlite_same_process_shm_registry_test_peer::adopt_runtime_lifetime(
			*fixture.registry,
			fixture.runtime_identity,
			fixture.runtime_pin_identity,
			runtime_owner);
		require(adopted.has_value(), "adopt runtime owner");
		runtime_owner.reset();

		auto alias_binding =
			sqlite_same_process_shm_registry_test_peer::alias_binding(fixture.process_instance,
																	  fixture.cohort,
																	  fixture.alias_lifetime,
																	  std::move(adopted.value()));
		auto reserved = fixture.registry->reserve_alias(std::move(alias_binding));
		require(reserved.has_value(), "reserve registry alias");
		fixture.alias.emplace(std::move(reserved.value()));
		require(fixture.registry->begin_alias_register(*fixture.alias).has_value(),
				"arm alias registration");
		const auto registration = sqlite_same_process_shm_registry_test_peer::registration_receipt(
			fixture.process_instance,
			fixture.cohort,
			fixture.alias_lifetime,
			fixture.runtime_identity,
			fixture.runtime_pin_identity,
			fixture.registration_epoch);
		require(
			fixture.registry->confirm_alias_registered(*fixture.alias, registration).has_value(),
			"confirm alias registration");

		auto pinned = fixture.registry->install_or_join_family(*fixture.alias, fixture.family);
		require(pinned.has_value(), "install registry family");
		fixture.family_pin.emplace(std::move(pinned.value()));
		if (acquire_activity)
		{
			auto activity = fixture.registry->acquire_activity(*fixture.family_pin);
			require(activity.has_value(), "acquire registry activity");
			fixture.activity.emplace(std::move(activity.value()));
		}
		return fixture;
	}

	void clean_fixture(registry_fixture& fixture)
	{
		if (fixture.activity)
		{
			require(fixture.registry->release_activity(*fixture.activity).has_value(),
					"release registry activity");
			fixture.activity.reset();
		}
		if (fixture.family_pin)
		{
			require(fixture.registry->release_family(*fixture.family_pin).has_value(),
					"release registry family");
			fixture.family_pin.reset();
		}
		require(fixture.registry->begin_alias_unregister(*fixture.alias).has_value(),
				"begin alias unregister");
		require(fixture.registry->poll_alias_unregister(*fixture.alias).has_value(),
				"poll alias unregister");
		const auto unregistration =
			sqlite_same_process_shm_registry_test_peer::unregistration_receipt(
				fixture.process_instance,
				fixture.cohort,
				fixture.alias_lifetime,
				fixture.runtime_identity,
				fixture.runtime_pin_identity,
				fixture.registration_epoch,
				fixture.unregistration_epoch);
		require(fixture.registry->confirm_alias_unregistered(*fixture.alias, unregistration)
					.has_value(),
				"confirm alias unregister");
		fixture.alias.reset();
		require(fixture.owner.expired(), "exact detach releases runtime owner");
		require(fixture.owner_destruction_count->load(std::memory_order_relaxed) == 1,
				"runtime owner released exactly once");
	}

	void verify_registry_writer_member_is_exact_and_cleanup_only()
	{
		auto fixture = make_fixture(20U, false);
		auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
			*fixture.registry, fixture.family);
		require(coordinator != nullptr, "resolve exact family coordinator for bridge test");
		const auto request = writer_request(fixture.family,
											fixture.alias_lifetime,
											identity("test.registry.bridge-connection", 20U),
											identity("test.registry.bridge-open", 20U),
											20U);
		auto epoch = make_bridge_epoch(request, 20U);
		auto observer = epoch.activation.take_observer();
		auto arm = epoch.activation.take_arm();
		auto begun =
			fixture.registry->begin_writer_map(*fixture.family_pin, std::move(arm), request);
		require(begun.has_value(), "predelegate exact registry writer member");
		auto inflight = std::move(*begun);
		auto registry_snapshot = fixture.registry->snapshot();
		auto lease_snapshot = coordinator->snapshot();
		require(registry_snapshot.active_activity_pin_count == 1U &&
					lease_snapshot.writer_member_authority_count == 1U &&
					lease_snapshot.writer_inflight_count == 1U && !lease_snapshot.quarantined,
				"exact member bundle was not atomically visible in both registries");

		int native_page{};
		auto native_receipt =
			sqlite_writer_shm_native_map_receipt_validator::validate(inflight, 0, &native_page);
		require(native_receipt.has_value(), "validate bridge native mapping");
		auto post_native = coordinator->record_writer_native_mapping(inflight, *native_receipt);
		require(post_native.has_value() && !inflight.valid(), "record exact bound native mapping");
		const auto synthetic = sqlite_same_process_shm_lease_test_peer::writer_map(
			request,
			request.attachment.open_epoch(),
			mapping(&native_page),
			sqlite_shm_writer_extend_pair::one_one,
			identity("test.registry.synthetic-post-map", 20U));
		auto pending = coordinator->install_pending(*post_native, synthetic);
		require(
			!pending &&
				pending.error().reason ==
					sqlite_shm_lease_rejection_reason::pending_or_eligibility_only &&
				pending.error().action ==
					sqlite_shm_lease_recovery_action::attempt_nonremoving_unmap_then_outer_ioerr &&
				post_native->valid(),
			"synthetic post-map receipt advanced registry-bound member");

		auto cleanup = coordinator->begin_writer_cleanup(*post_native, request.callback);
		require(cleanup.has_value() && !post_native->valid(),
				"bound post-native member did not retain cleanup-only admission");
		require(coordinator
					->complete_writer_cleanup(*cleanup,
											  request.callback,
											  sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"confirmed bound cleanup did not complete");
		registry_snapshot = fixture.registry->snapshot();
		lease_snapshot = coordinator->snapshot();
		require(registry_snapshot.active_activity_pin_count == 0U &&
					lease_snapshot.writer_member_authority_count == 0U &&
					lease_snapshot.writer_inflight_count == 0U &&
					lease_snapshot.writer_attachment_unresolved_count == 0U &&
					!lease_snapshot.quarantined,
				"confirmed cleanup did not release exact member outside coordinator lock");
		clean_fixture(fixture);
	}

	void verify_exact_attachment_cleanup_drains_all_bound_members()
	{
		auto fixture = make_fixture(19U, false);
		auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
			*fixture.registry, fixture.family);
		require(coordinator != nullptr, "resolve grouped bridge cleanup coordinator");
		const auto first_request = writer_request(fixture.family,
												  fixture.alias_lifetime,
												  identity("test.registry.bridge-connection", 19U),
												  identity("test.registry.bridge-open", 19U),
												  19U);
		auto sources = make_bridge_epoch_sources(first_request, 19U);
		auto first_activation = sources->arm(first_request, 19U);
		auto first_arm = first_activation.take_arm();
		auto first = fixture.registry->begin_writer_map(
			*fixture.family_pin, std::move(first_arm), first_request);
		require(first.has_value(), "begin first grouped bound member");
		auto first_inflight = std::move(*first);

		auto second_request = first_request;
		second_request.callback = {first_request.callback.thread_identity,
								   1U,
								   identity("test.registry.grouped-nested-callback", 18U)};
		second_request.caller_extend = 0;
		auto second_activation = sources->arm(second_request, 18U);
		auto second_arm = second_activation.take_arm();
		auto second = fixture.registry->begin_writer_map(
			*fixture.family_pin, std::move(second_arm), second_request);
		require(second.has_value(), "begin second grouped bound member");
		auto second_inflight = std::move(*second);
		auto before_native = coordinator->snapshot();
		require(fixture.registry->snapshot().active_activity_pin_count == 2U &&
					before_native.writer_member_authority_count == 2U &&
					before_native.writer_attachment_unresolved_member_count == 2U,
				"grouped bound members did not retain two exact owners and arms");

		int native_page{};
		auto first_native = sqlite_writer_shm_native_map_receipt_validator::validate(
			first_inflight, 0, &native_page);
		auto second_native = sqlite_writer_shm_native_map_receipt_validator::validate(
			second_inflight, 0, &native_page);
		require(first_native && second_native, "validate both grouped bound native mappings");
		auto first_post = coordinator->record_writer_native_mapping(first_inflight, *first_native);
		auto second_post =
			coordinator->record_writer_native_mapping(second_inflight, *second_native);
		require(first_post && second_post, "record both grouped bound native mappings");

		auto cleanup = coordinator->begin_writer_cleanup(*second_post, second_request.callback);
		require(cleanup.has_value(),
				"one exact attachment cleanup owner did not seal grouped members");
		require(!second_post->valid(), "grouped cleanup did not consume nested anchor member");
		const auto sealed = coordinator->snapshot();
		require(sealed.writer_member_authority_count == 2U &&
					sealed.writer_attachment_audit_member_count == 2U &&
					sealed.writer_attachment_audit_native_mapping_count == 2U &&
					sealed.writer_cleanup_count == 1U,
				"grouped cleanup lost exact member audit or authority ownership");
		require(coordinator
					->complete_writer_cleanup(*cleanup,
											  second_request.callback,
											  sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"one confirmed attachment cleanup did not drain both bound members");
		const auto complete = coordinator->snapshot();
		require(fixture.registry->snapshot().active_activity_pin_count == 0U &&
					complete.writer_member_authority_count == 0U &&
					complete.writer_attachment_unresolved_member_count == 0U &&
					complete.writer_attachment_audit_member_count == 2U && !complete.quarantined,
				"grouped confirmed cleanup did not release both activities and arms exactly once");
		clean_fixture(fixture);
	}

	void verify_shallower_anchor_cannot_bypass_nested_writer_callback()
	{
		auto fixture = make_fixture(18U, false);
		auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
			*fixture.registry, fixture.family);
		require(coordinator != nullptr, "resolve grouped callback-order coordinator");
		const auto first_request = writer_request(fixture.family,
												  fixture.alias_lifetime,
												  identity("test.registry.bridge-connection", 18U),
												  identity("test.registry.bridge-open", 18U),
												  18U);
		auto sources = make_bridge_epoch_sources(first_request, 18U);
		auto first_activation = sources->arm(first_request, 18U);
		auto first_arm = first_activation.take_arm();
		auto first = fixture.registry->begin_writer_map(
			*fixture.family_pin, std::move(first_arm), first_request);
		require(first.has_value(), "begin shallow grouped bound member");
		auto first_inflight = std::move(*first);

		auto nested_request = first_request;
		nested_request.callback = {first_request.callback.thread_identity,
								   1U,
								   identity("test.registry.grouped-nested-callback", 17U)};
		auto nested_activation = sources->arm(nested_request, 17U);
		auto nested_arm = nested_activation.take_arm();
		auto nested = fixture.registry->begin_writer_map(
			*fixture.family_pin, std::move(nested_arm), nested_request);
		require(nested.has_value(), "begin nested grouped bound member");
		auto nested_inflight = std::move(*nested);

		int native_page{};
		auto first_native = sqlite_writer_shm_native_map_receipt_validator::validate(
			first_inflight, 0, &native_page);
		auto nested_native = sqlite_writer_shm_native_map_receipt_validator::validate(
			nested_inflight, 0, &native_page);
		require(first_native && nested_native,
				"validate shallow and nested grouped native mappings");
		auto first_post = coordinator->record_writer_native_mapping(first_inflight, *first_native);
		auto nested_post =
			coordinator->record_writer_native_mapping(nested_inflight, *nested_native);
		require(first_post && nested_post, "record shallow and nested grouped native mappings");

		auto rejected = coordinator->begin_writer_cleanup(*first_post, first_request.callback);
		const auto terminal = coordinator->snapshot();
		require(!rejected && !first_post->valid() && terminal.quarantined &&
					terminal.writer_member_authority_count == 2U &&
					terminal.writer_attachment_audit_member_count == 0U &&
					fixture.registry->snapshot().active_activity_pin_count == 2U,
				"shallow anchor equality bypassed nested sibling callback ordering");
		auto retry = coordinator->begin_writer_cleanup(*nested_post, nested_request.callback);
		require(!retry && !nested_post->valid() &&
					coordinator->snapshot().writer_attachment_audit_member_count == 0U,
				"terminal callback-order failure exposed a retry cleanup owner");
	}

	void verify_registry_writer_begin_rejects_used_or_mismatched_epoch()
	{
		auto fixture = make_fixture(21U, false);
		auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
			*fixture.registry, fixture.family);
		require(coordinator != nullptr, "resolve coordinator for bridge negatives");
		const auto request = writer_request(fixture.family,
											fixture.alias_lifetime,
											identity("test.registry.bridge-connection", 21U),
											identity("test.registry.bridge-open", 21U),
											21U);

		auto used_epoch = make_bridge_epoch(request, 21U);
		auto used_observer = used_epoch.activation.take_observer();
		auto used_arm = used_epoch.activation.take_arm();
		auto consumed = seal_sqlite_writer_shm_mapping_epoch(used_observer, nullptr);
		require(!consumed, "null post-map unexpectedly produced an epoch receipt");
		auto used =
			fixture.registry->begin_writer_map(*fixture.family_pin, std::move(used_arm), request);
		require(!used && fixture.registry->snapshot().active_activity_pin_count == 0U &&
					coordinator->snapshot().writer_member_authority_count == 0U,
				"already-sealed epoch arm entered registry predelegation");

		auto mismatched_epoch = make_bridge_epoch(request, 22U);
		auto mismatched_arm = mismatched_epoch.activation.take_arm();
		auto mismatched_request = request;
		mismatched_request.page_number = 1;
		auto mismatched = fixture.registry->begin_writer_map(
			*fixture.family_pin, std::move(mismatched_arm), mismatched_request);
		const auto after_mismatch = coordinator->snapshot();
		require(!mismatched && fixture.registry->snapshot().active_activity_pin_count == 0U &&
					after_mismatch.writer_member_authority_count == 0U &&
					after_mismatch.writer_inflight_count == 0U &&
					after_mismatch.writer_attachment_member_count == 0U,
				"wrong exact map request consumed or published epoch authority");
		clean_fixture(fixture);
	}

	void verify_registry_writer_attachment_origin_never_mixes()
	{
		{
			auto fixture = make_fixture(22U, false);
			auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
				*fixture.registry, fixture.family);
			require(coordinator != nullptr, "resolve bound-first origin coordinator");
			const auto request = writer_request(fixture.family,
												fixture.alias_lifetime,
												identity("test.registry.bridge-connection", 22U),
												identity("test.registry.bridge-open", 22U),
												22U);
			auto epoch = make_bridge_epoch(request, 23U);
			auto arm = epoch.activation.take_arm();
			auto bound =
				fixture.registry->begin_writer_map(*fixture.family_pin, std::move(arm), request);
			require(bound.has_value(), "install bound-first attachment member");
			auto inflight = std::move(*bound);
			auto shared_request = request;
			shared_request.callback = callback(23U);
			shared_request.caller_extend = 0;
			auto shared_activation = epoch.sources->arm(shared_request, 25U);
			auto shared_arm = shared_activation.take_arm();
			auto shared_bound = fixture.registry->begin_writer_map(
				*fixture.family_pin, std::move(shared_arm), shared_request);
			require(shared_bound.has_value(),
					"same exact lifetime sources with distinct pins and {0,0} extend did not "
					"share {1,1} attachment");
			auto shared_inflight = std::move(*shared_bound);

			auto duplicate_source_request = request;
			duplicate_source_request.callback = callback(25U);
			auto duplicate_sources = make_bridge_epoch(duplicate_source_request, 23U);
			auto duplicate_arm = duplicate_sources.activation.take_arm();
			auto duplicate_source_bound = fixture.registry->begin_writer_map(
				*fixture.family_pin, std::move(duplicate_arm), duplicate_source_request);
			require(!duplicate_source_bound &&
						duplicate_source_bound.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch,
					"duplicate opaque lifetime identities from distinct controls joined one "
					"attachment");
			auto legacy_request = request;
			legacy_request.callback = callback(26U);
			auto legacy = coordinator->begin_writer_map(legacy_request);
			require(!legacy &&
						legacy.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch,
					"legacy member joined registry-bound attachment");
			require(coordinator->resolve_writer_map_failure(inflight).has_value(),
					"resolve bound-first no-map member");
			require(coordinator->resolve_writer_map_failure(shared_inflight).has_value(),
					"resolve same-source bound sibling");
			clean_fixture(fixture);
		}

		{
			auto fixture = make_fixture(23U, false);
			auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
				*fixture.registry, fixture.family);
			require(coordinator != nullptr, "resolve legacy-first origin coordinator");
			const auto request = writer_request(fixture.family,
												fixture.alias_lifetime,
												identity("test.registry.bridge-connection", 23U),
												identity("test.registry.bridge-open", 23U),
												23U);
			auto legacy = coordinator->begin_writer_map(request);
			require(legacy.has_value(), "install legacy-first attachment member");
			auto legacy_inflight = std::move(*legacy);
			auto bound_request = request;
			bound_request.callback = callback(24U);
			auto epoch = make_bridge_epoch(bound_request, 24U);
			auto arm = epoch.activation.take_arm();
			auto bound = fixture.registry->begin_writer_map(
				*fixture.family_pin, std::move(arm), bound_request);
			require(!bound &&
						bound.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch &&
						fixture.registry->snapshot().active_activity_pin_count == 0U,
					"registry-bound member joined legacy attachment");
			require(coordinator->resolve_writer_map_failure(legacy_inflight).has_value(),
					"resolve legacy-first no-map member");
			clean_fixture(fixture);
		}
	}

	void verify_registry_writer_liveness_loss_blocks_admission_but_keeps_cleanup()
	{
		auto fixture = make_fixture(24U, false);
		auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
			*fixture.registry, fixture.family);
		require(coordinator != nullptr, "resolve liveness-loss coordinator");
		const auto request = writer_request(fixture.family,
											fixture.alias_lifetime,
											identity("test.registry.bridge-connection", 24U),
											identity("test.registry.bridge-open", 24U),
											24U);
		auto epoch = make_bridge_epoch(request, 25U);
		auto arm = epoch.activation.take_arm();
		auto begun =
			fixture.registry->begin_writer_map(*fixture.family_pin, std::move(arm), request);
		require(begun.has_value(), "begin liveness-loss bound member");
		auto inflight = std::move(*begun);
		int native_page{};
		auto native_receipt =
			sqlite_writer_shm_native_map_receipt_validator::validate(inflight, 0, &native_page);
		require(native_receipt.has_value() && epoch.sources->shm.revoker.revoke(),
				"revoke bound SHM lifetime after native success");
		auto post_native = coordinator->record_writer_native_mapping(inflight, *native_receipt);
		require(post_native.has_value(),
				"liveness loss made observed native mapping cleanup unreachable");
		const auto lost = coordinator->snapshot();
		require(lost.quarantined && lost.writer_member_authority_count == 1U &&
					lost.writer_member_liveness_lost_count == 1U,
				"bound liveness loss remained admission-visible");

		auto sibling_request = writer_request(fixture.family,
											  fixture.alias_lifetime,
											  identity("test.registry.bridge-connection", 25U),
											  identity("test.registry.bridge-open", 25U),
											  25U);
		auto sibling_epoch = make_bridge_epoch(sibling_request, 26U);
		auto sibling_arm = sibling_epoch.activation.take_arm();
		auto sibling = fixture.registry->begin_writer_map(
			*fixture.family_pin, std::move(sibling_arm), sibling_request);
		require(!sibling &&
					sibling.error().reason == sqlite_shm_lease_rejection_reason::quarantined,
				"revoked bound member admitted a sibling");

		auto cleanup = coordinator->begin_writer_cleanup(*post_native, request.callback);
		require(cleanup.has_value(), "revoked bound member lost cleanup admission");
		require(coordinator
					->complete_writer_cleanup(*cleanup,
											  request.callback,
											  sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"confirmed cleanup could not drain revoked bound member");
		require(coordinator->snapshot().writer_member_authority_count == 0U &&
					fixture.registry->snapshot().active_activity_pin_count == 0U,
				"revoked bound member retained authority after confirmed cleanup");
	}

	void verify_registry_writer_no_map_liveness_boundary_is_sticky()
	{
		auto fixture = make_fixture(25U, false);
		auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
			*fixture.registry, fixture.family);
		require(coordinator != nullptr, "resolve no-map liveness coordinator");
		const auto request = writer_request(fixture.family,
											fixture.alias_lifetime,
											identity("test.registry.bridge-connection", 27U),
											identity("test.registry.bridge-open", 27U),
											27U);
		auto epoch = make_bridge_epoch(request, 27U);
		auto arm = epoch.activation.take_arm();
		auto begun =
			fixture.registry->begin_writer_map(*fixture.family_pin, std::move(arm), request);
		require(begun.has_value(), "begin revoked no-map member");
		auto inflight = std::move(*begun);
		require(epoch.sources->main.revoker.revoke(), "revoke live predelegated main lifetime");
		require(coordinator->resolve_writer_map_failure(inflight).has_value(),
				"revoked no-map member did not drain exact activity");
		const auto drained = coordinator->snapshot();
		require(drained.quarantined && drained.writer_member_authority_count == 0U &&
					fixture.registry->snapshot().active_activity_pin_count == 0U,
				"revoked no-map resolution erased sticky quarantine evidence");

		auto fresh_request = writer_request(fixture.family,
											fixture.alias_lifetime,
											identity("test.registry.bridge-connection", 28U),
											identity("test.registry.bridge-open", 28U),
											28U);
		auto fresh = coordinator->begin_writer_map(fresh_request);
		require(!fresh && fresh.error().reason == sqlite_shm_lease_rejection_reason::quarantined,
				"revoked no-map drain revived same-family admission");
	}

	void verify_confirmed_cleanup_terminalizes_liveness_loss()
	{
		auto fixture = make_fixture(26U, false);
		auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
			*fixture.registry, fixture.family);
		require(coordinator != nullptr, "resolve clean terminal coordinator");
		const auto request = writer_request(fixture.family,
											fixture.alias_lifetime,
											identity("test.registry.bridge-connection", 29U),
											identity("test.registry.bridge-open", 29U),
											29U);
		auto epoch = make_bridge_epoch(request, 29U);
		auto arm = epoch.activation.take_arm();
		auto begun =
			fixture.registry->begin_writer_map(*fixture.family_pin, std::move(arm), request);
		require(begun.has_value(), "begin clean terminal member");
		auto inflight = std::move(*begun);
		int native_page{};
		auto native_receipt =
			sqlite_writer_shm_native_map_receipt_validator::validate(inflight, 0, &native_page);
		require(native_receipt.has_value() && epoch.sources->shm.revoker.revoke(),
				"revoke cleanup-terminal SHM lifetime");
		auto post_native = coordinator->record_writer_native_mapping(inflight, *native_receipt);
		require(post_native.has_value(), "record cleanup-terminal native mapping");
		auto cleanup = coordinator->begin_writer_cleanup(*post_native, request.callback);
		require(cleanup.has_value(), "admit cleanup-terminal unmap");
		require(coordinator
					->complete_writer_cleanup(*cleanup,
											  request.callback,
											  sqlite_shm_native_cleanup_outcome::confirmed_success)
					.has_value(),
				"confirmed native cleanup did not terminalize lifetime loss");
		const auto terminal = coordinator->snapshot();
		require(!terminal.quarantined && terminal.writer_member_authority_count == 0U &&
					fixture.registry->snapshot().active_activity_pin_count == 0U,
				"confirmed cleanup left transient liveness quarantine active");

		const auto fresh_request = writer_request(fixture.family,
												  fixture.alias_lifetime,
												  identity("test.registry.bridge-connection", 30U),
												  identity("test.registry.bridge-open", 30U),
												  30U);
		auto fresh_epoch = make_bridge_epoch(fresh_request, 30U);
		auto fresh_arm = fresh_epoch.activation.take_arm();
		auto fresh = fixture.registry->begin_writer_map(
			*fixture.family_pin, std::move(fresh_arm), fresh_request);
		require(fresh.has_value(), "confirmed cleanup did not restore fresh admission");
		auto fresh_inflight = std::move(*fresh);
		require(coordinator->resolve_writer_map_failure(fresh_inflight).has_value(),
				"resolve fresh no-map member after confirmed cleanup");
		clean_fixture(fixture);
	}

	void verify_registry_writer_final_publish_liveness_losers_roll_back()
	{
		{
			auto fixture = make_fixture(27U, false);
			auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
				*fixture.registry, fixture.family);
			require(coordinator != nullptr, "resolve incoming-loser coordinator");
			const auto request = writer_request(fixture.family,
												fixture.alias_lifetime,
												identity("test.registry.bridge-connection", 31U),
												identity("test.registry.bridge-open", 31U),
												31U);
			auto epoch = make_bridge_epoch(request, 31U);
			auto arm = epoch.activation.take_arm();
			sqlite_same_process_shm_lease_test_peer::fail_next_registry_incoming_liveness(
				*coordinator);
			::alarm(5U);
			auto rejected =
				fixture.registry->begin_writer_map(*fixture.family_pin, std::move(arm), request);
			::alarm(0U);
			const auto lease = coordinator->snapshot();
			require(!rejected && fixture.registry->snapshot().active_activity_pin_count == 0U &&
						lease.writer_member_authority_count == 0U &&
						lease.writer_inflight_count == 0U &&
						lease.writer_attachment_member_count == 0U && !lease.quarantined,
					"incoming final-liveness loser consumed authority or retained placeholder");
			clean_fixture(fixture);
		}

		{
			auto fixture = make_fixture(28U, false);
			auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
				*fixture.registry, fixture.family);
			require(coordinator != nullptr, "resolve existing-loser coordinator");
			const auto first_request =
				writer_request(fixture.family,
							   fixture.alias_lifetime,
							   identity("test.registry.bridge-connection", 32U),
							   identity("test.registry.bridge-open", 32U),
							   32U);
			auto first_epoch = make_bridge_epoch(first_request, 32U);
			auto first_arm = first_epoch.activation.take_arm();
			auto first = fixture.registry->begin_writer_map(
				*fixture.family_pin, std::move(first_arm), first_request);
			require(first.has_value(), "begin existing-loser anchor");
			auto first_inflight = std::move(*first);

			const auto second_request =
				writer_request(fixture.family,
							   fixture.alias_lifetime,
							   identity("test.registry.bridge-connection", 33U),
							   identity("test.registry.bridge-open", 33U),
							   33U);
			auto second_epoch = make_bridge_epoch(second_request, 33U);
			auto second_arm = second_epoch.activation.take_arm();
			sqlite_same_process_shm_lease_test_peer::fail_next_registry_existing_liveness(
				*coordinator);
			::alarm(5U);
			auto second = fixture.registry->begin_writer_map(
				*fixture.family_pin, std::move(second_arm), second_request);
			::alarm(0U);
			const auto rejected = coordinator->snapshot();
			require(!second && rejected.quarantined &&
						rejected.writer_member_authority_count == 1U &&
						rejected.writer_inflight_count == 1U &&
						rejected.writer_attachment_unresolved_member_count == 1U &&
						fixture.registry->snapshot().active_activity_pin_count == 1U,
					"existing final-liveness loser published sibling or consumed anchor");
			require(coordinator->resolve_writer_map_failure(first_inflight).has_value(),
					"drain existing-loser anchor");
			require(coordinator->snapshot().quarantined &&
						fixture.registry->snapshot().active_activity_pin_count == 0U,
					"existing-loser drain revived quarantined coordinator");
		}
	}

	void verify_activity_seal_is_one_shot_weak_and_exactly_bound()
	{
		auto fixture = make_fixture(30U, true);
		auto sealed = fixture.activity->seal_for_audit();
		require(sealed.has_value(), "mint exact registry activity audit seal");
		auto seal = std::move(sealed.value());
		auto copied = seal; // NOLINT(performance-unnecessary-copy-initialization)
		const sqlite_shm_lease_family_binding wrong_family{
			fixture.process_instance,
			fixture.cohort,
			identity("test.registry.wrong-file-family", 30U),
		};
		require(
			seal.valid() && copied.valid() &&
				sqlite_same_process_shm_registry_test_peer::activity_seal_matches(
					*fixture.registry,
					seal,
					fixture.process_instance,
					fixture.family,
					fixture.alias_lifetime) &&
				!sqlite_same_process_shm_registry_test_peer::activity_seal_matches(
					*fixture.registry,
					seal,
					identity("test.registry.wrong-process", 30U),
					fixture.family,
					fixture.alias_lifetime) &&
				!sqlite_same_process_shm_registry_test_peer::activity_seal_matches(
					*fixture.registry,
					seal,
					fixture.process_instance,
					wrong_family,
					fixture.alias_lifetime) &&
				!sqlite_same_process_shm_registry_test_peer::activity_seal_matches(
					*fixture.registry,
					seal,
					fixture.process_instance,
					fixture.family,
					identity("test.registry.wrong-alias", 30U)),
			"weak seal binds exact process, alias, family, family-pin, and activity coordinates");

		const auto duplicate_seal = fixture.activity->seal_for_audit();
		require(!duplicate_seal.has_value() &&
					duplicate_seal.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
				"activity audit seal is one-shot");
		require(fixture.registry->release_activity(*fixture.activity).has_value(),
				"clean release consumes exact activity owner");
		const auto after_release = fixture.registry->snapshot();
		const auto family_after_release = fixture.registry->family_snapshot(fixture.family);
		const bool matcher_rejects_released =
			!sqlite_same_process_shm_registry_test_peer::activity_seal_matches(
				*fixture.registry,
				seal,
				fixture.process_instance,
				fixture.family,
				fixture.alias_lifetime);
		require(!fixture.activity->valid() && !seal.valid() && !copied.valid() &&
					after_release.active_activity_pin_count == 0U &&
					family_after_release.activity_pin_count == 0U && matcher_rejects_released,
				"clean release invalidates every weak seal and drains exact counts once");
		const auto duplicate_release = fixture.registry->release_activity(*fixture.activity);
		require(!duplicate_release.has_value() &&
					duplicate_release.error().reason ==
						sqlite_shm_lease_rejection_reason::stale_token &&
					fixture.registry->snapshot().active_activity_pin_count == 0U,
				"duplicate release cannot consume registry counts twice");
		fixture.activity.reset();
		clean_fixture(fixture);
	}

	void verify_activity_owner_destruction_never_takes_registry_mutex()
	{
		auto fixture = make_fixture(31U, true);
		auto acquired_sibling = fixture.registry->acquire_activity(*fixture.family_pin);
		require(acquired_sibling.has_value(), "acquire lock-held sibling activity");
		std::optional<sqlite_shm_registry_activity_pin> sibling;
		sibling.emplace(std::move(acquired_sibling.value()));
		auto acquired_unsealed_sibling = fixture.registry->acquire_activity(*fixture.family_pin);
		require(acquired_unsealed_sibling.has_value(), "acquire unsealed lock-held sibling");
		std::optional<sqlite_shm_registry_activity_pin> unsealed_sibling;
		unsealed_sibling.emplace(std::move(acquired_unsealed_sibling.value()));
		auto sealed = fixture.activity->seal_for_audit();
		require(sealed.has_value(), "mint lock-held destruction audit seal");
		auto sibling_sealed = sibling->seal_for_audit();
		require(sibling_sealed.has_value(), "mint lock-held sibling audit seal");

		const sqlite_shm_lease_family_binding same_alias_family{
			fixture.process_instance,
			fixture.cohort,
			identity("test.registry.same-alias-file-family", 31U),
		};
		auto installed_same_alias_family =
			fixture.registry->install_or_join_family(*fixture.alias, same_alias_family);
		require(installed_same_alias_family.has_value(), "install same-alias distinct family");
		std::optional<sqlite_shm_registry_family_pin> same_alias_family_pin;
		same_alias_family_pin.emplace(std::move(installed_same_alias_family.value()));
		auto acquired_same_alias = fixture.registry->acquire_activity(*same_alias_family_pin);
		require(acquired_same_alias.has_value(), "acquire same-alias distinct-family activity");
		std::optional<sqlite_shm_registry_activity_pin> same_alias_activity;
		same_alias_activity.emplace(std::move(acquired_same_alias.value()));
		auto same_alias_sealed = same_alias_activity->seal_for_audit();
		require(same_alias_sealed.has_value(), "mint same-alias distinct-family audit seal");

		auto same_family_count = std::make_shared<std::atomic_int>(0);
		auto same_family_runtime_owner = std::make_shared<runtime_owner_probe>(same_family_count);
		auto same_family_member = register_alias(*fixture.registry,
												 fixture.process_instance,
												 fixture.cohort,
												 82U,
												 same_family_runtime_owner,
												 &fixture.family);
		same_family_runtime_owner.reset();
		auto acquired_same_family =
			fixture.registry->acquire_activity(*same_family_member.family_pin);
		require(acquired_same_family.has_value(), "acquire distinct-alias same-family activity");
		std::optional<sqlite_shm_registry_activity_pin> same_family_activity;
		same_family_activity.emplace(std::move(acquired_same_family.value()));
		auto same_family_sealed = same_family_activity->seal_for_audit();
		require(same_family_sealed.has_value(), "mint distinct-alias same-family audit seal");

		auto unrelated_count = std::make_shared<std::atomic_int>(0);
		auto unrelated_runtime_owner = std::make_shared<runtime_owner_probe>(unrelated_count);
		auto unrelated = register_alias(*fixture.registry,
										fixture.process_instance,
										fixture.cohort,
										81U,
										unrelated_runtime_owner);
		unrelated_runtime_owner.reset();
		const sqlite_shm_lease_family_binding unrelated_family{
			fixture.process_instance,
			fixture.cohort,
			identity("test.registry.unrelated-file-family", 31U),
		};
		auto unrelated_family_pin =
			fixture.registry->install_or_join_family(*unrelated.alias, unrelated_family);
		require(unrelated_family_pin.has_value(), "install unrelated lock-held family");
		unrelated.family_pin.emplace(std::move(unrelated_family_pin.value()));
		auto acquired_unrelated = fixture.registry->acquire_activity(*unrelated.family_pin);
		require(acquired_unrelated.has_value(), "acquire unrelated lock-held activity");
		std::optional<sqlite_shm_registry_activity_pin> unrelated_activity;
		unrelated_activity.emplace(std::move(acquired_unrelated.value()));
		auto unrelated_sealed = unrelated_activity->seal_for_audit();
		require(unrelated_sealed.has_value(), "mint unrelated lock-held audit seal");

		const auto child = ::fork();
		require(child >= 0, "fork lock-held activity destruction");
		if (child == 0)
		{
			::alarm(5U);
			const auto& seal = sealed.value();
			sqlite_same_process_shm_registry_test_peer::lock_registry_mutex(*fixture.registry);
			fixture.activity.reset();
			const auto rejected_late_seal = unsealed_sibling->seal_for_audit();
			const bool invalid_without_lock = !seal.valid() && !sibling->valid() &&
				!sibling_sealed->valid() && !unsealed_sibling->valid() &&
				!rejected_late_seal.has_value() &&
				rejected_late_seal.error().reason ==
					sqlite_shm_lease_rejection_reason::stale_token &&
				!same_alias_activity->valid() && !same_alias_sealed->valid() &&
				!same_family_activity->valid() && !same_family_sealed->valid() &&
				unrelated_activity->valid() && unrelated_sealed->valid();
			sqlite_same_process_shm_registry_test_peer::unlock_registry_mutex(*fixture.registry);
			const auto synchronized = fixture.registry->snapshot();
			const bool closed_matchers =
				!sqlite_same_process_shm_registry_test_peer::activity_seal_matches(
					*fixture.registry,
					seal,
					fixture.process_instance,
					fixture.family,
					fixture.alias_lifetime) &&
				!sqlite_same_process_shm_registry_test_peer::activity_seal_matches(
					*fixture.registry,
					sibling_sealed.value(),
					fixture.process_instance,
					fixture.family,
					fixture.alias_lifetime) &&
				!sqlite_same_process_shm_registry_test_peer::activity_seal_matches(
					*fixture.registry,
					same_alias_sealed.value(),
					fixture.process_instance,
					same_alias_family,
					fixture.alias_lifetime) &&
				!sqlite_same_process_shm_registry_test_peer::activity_seal_matches(
					*fixture.registry,
					same_family_sealed.value(),
					fixture.process_instance,
					fixture.family,
					same_family_member.alias_lifetime) &&
				sqlite_same_process_shm_registry_test_peer::activity_seal_matches(
					*fixture.registry,
					unrelated_sealed.value(),
					fixture.process_instance,
					unrelated_family,
					unrelated.alias_lifetime) &&
				unrelated_activity->valid() && unrelated_sealed->valid();
			const auto sibling_release = fixture.registry->release_activity(*sibling);
			const auto unsealed_sibling_release =
				fixture.registry->release_activity(*unsealed_sibling);
			const auto same_alias_release =
				fixture.registry->release_activity(*same_alias_activity);
			const auto same_family_release =
				fixture.registry->release_activity(*same_family_activity);
			const auto unrelated_release = fixture.registry->release_activity(*unrelated_activity);
			const auto drained = fixture.registry->snapshot();
			const bool exact_local_quarantine = invalid_without_lock && closed_matchers &&
				synchronized.active_activity_pin_count == 5U && sibling_release.has_value() &&
				unsealed_sibling_release.has_value() && same_alias_release.has_value() &&
				same_family_release.has_value() && unrelated_release.has_value() &&
				drained.active_activity_pin_count == 0U &&
				synchronized.quarantined_alias_count == 2U &&
				synchronized.quarantined_family_count == 2U && !synchronized.registry_quarantined &&
				!drained.registry_quarantined;
			::_exit(exact_local_quarantine ? 0 : 1);
		}
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait lock-held activity destruction");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"activity owner destructor is atomic-only and next entry drains exact counts");
		require(fixture.registry->release_activity(*sibling).has_value(),
				"release parent lock-held sibling activity");
		sibling.reset();
		require(fixture.registry->release_activity(*unsealed_sibling).has_value(),
				"release parent unsealed lock-held sibling activity");
		unsealed_sibling.reset();
		require(fixture.registry->release_activity(*same_alias_activity).has_value(),
				"release parent same-alias distinct-family activity");
		same_alias_activity.reset();
		require(fixture.registry->release_activity(*same_family_activity).has_value(),
				"release parent distinct-alias same-family activity");
		same_family_activity.reset();
		require(fixture.registry->release_activity(*unrelated_activity).has_value(),
				"release parent unrelated lock-held activity");
		unrelated_activity.reset();
		require(fixture.registry->release_family(*same_alias_family_pin).has_value(),
				"release parent same-alias distinct family");
		same_alias_family_pin.reset();
		unregister_alias(
			*fixture.registry, fixture.process_instance, fixture.cohort, same_family_member);
		unregister_alias(*fixture.registry, fixture.process_instance, fixture.cohort, unrelated);
		clean_fixture(fixture);
	}

	void verify_activity_abandonment_races_registry_visibility()
	{
		auto fixture = make_fixture(34U, true);
		auto acquired_sibling = fixture.registry->acquire_activity(*fixture.family_pin);
		require(acquired_sibling.has_value(), "acquire race sibling activity");
		std::optional<sqlite_shm_registry_activity_pin> sibling;
		sibling.emplace(std::move(acquired_sibling.value()));
		auto sibling_sealed = sibling->seal_for_audit();
		require(sibling_sealed.has_value(), "mint race sibling audit seal");

		const auto child = ::fork();
		require(child >= 0, "fork activity visibility race");
		if (child == 0)
		{
			::alarm(5U);
			std::barrier start{3};
			std::optional<sqlite_shm_registry_activity_pin> raced_activity;
			std::thread abandoner{
				[&]()
				{
					start.arrive_and_wait();
					fixture.activity.reset();
				},
			};
			std::thread observer{
				[&]()
				{
					start.arrive_and_wait();
					auto acquired = fixture.registry->acquire_activity(*fixture.family_pin);
					if (acquired)
						raced_activity.emplace(std::move(acquired.value()));
					(void)fixture.registry->snapshot();
				},
			};
			start.arrive_and_wait();
			abandoner.join();
			observer.join();

			const auto synchronized = fixture.registry->snapshot();
			const auto fresh = fixture.registry->acquire_activity(*fixture.family_pin);
			const bool authority_closed = !sibling->valid() && !sibling_sealed->valid() &&
				(!raced_activity || !raced_activity->valid()) && !fresh.has_value() &&
				synchronized.quarantined_alias_count == 1U &&
				synchronized.quarantined_family_count == 1U && !synchronized.registry_quarantined;
			const auto sibling_release = fixture.registry->release_activity(*sibling);
			bool raced_release_ok = true;
			if (raced_activity)
				raced_release_ok = fixture.registry->release_activity(*raced_activity).has_value();
			const auto drained = fixture.registry->snapshot();
			::_exit(authority_closed && sibling_release.has_value() && raced_release_ok &&
							drained.active_activity_pin_count == 0U
						? 0
						: 1);
		}
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait activity visibility race");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"thread race closes sibling authority and drains every published count once");

		require(fixture.registry->release_activity(*sibling).has_value(),
				"release parent race sibling activity");
		sibling.reset();
		clean_fixture(fixture);
	}

	void verify_quarantined_activity_owner_remains_exactly_releasable()
	{
		auto fixture = make_fixture(32U, true);
		auto sealed = fixture.activity->seal_for_audit();
		require(sealed.has_value(), "mint quarantine-release audit seal");
		const auto child = ::fork();
		require(child >= 0, "fork quarantined activity release");
		if (child == 0)
		{
			fixture.family_pin.reset();
			const bool authority_invalid = !fixture.activity->valid() && !sealed->valid();
			const auto release = fixture.registry->release_activity(*fixture.activity);
			const auto snapshot = fixture.registry->snapshot();
			const bool released = authority_invalid && release.has_value() &&
				snapshot.active_activity_pin_count == 0U &&
				snapshot.active_family_pin_count == 0U && snapshot.quarantined_alias_count == 1U &&
				snapshot.quarantined_family_count == 1U && !snapshot.registry_quarantined;
			::_exit(released ? 0 : 1);
		}
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait quarantined activity release");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"local quarantine invalidates authority without blocking exact count release");
		clean_fixture(fixture);
	}

	void verify_activity_owner_and_seal_do_not_retain_registry_state()
	{
		auto fixture = make_fixture(33U, true);
		auto sealed = fixture.activity->seal_for_audit();
		require(sealed.has_value(), "mint registry-lifetime audit seal");
		const auto state_destruction_before =
			sqlite_same_process_shm_registry_test_peer::state_destruction_count();
		const auto child = ::fork();
		require(child >= 0, "fork registry activity cycle probe");
		if (child == 0)
		{
			std::optional<sqlite_shm_registry_activity_pin> retained_owner;
			retained_owner.emplace(std::move(*fixture.activity));
			fixture.activity.reset();
			const auto& retained_seal = sealed.value();
			fixture.registry.reset();
			fixture.family_pin.reset();
			fixture.alias.reset();
			const bool state_released =
				sqlite_same_process_shm_registry_test_peer::state_destruction_count() ==
				state_destruction_before + 1U;
			const bool weak_seal_invalid = !retained_owner->valid() && !retained_seal.valid();
			retained_owner.reset();
			const bool seal_does_not_retain_control = !retained_seal.valid();
			::_exit(state_released && weak_seal_invalid && seal_does_not_retain_control ? 0 : 1);
		}
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait registry activity cycle probe");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"activity owner and weak seal create no registry/coordinator lifetime cycle");
		clean_fixture(fixture);
	}

	void verify_process_owner_is_one_shot()
	{
		const auto process = identity("test.registry.process-owner", 1U);
		auto owner = sqlite_same_process_shm_registry_test_peer::process_owner(process);
		auto replay = sqlite_same_process_shm_registry_test_peer::replay_owner(owner);
		auto witness = sqlite_same_process_shm_registry_test_peer::replay_owner(owner);
		require(owner.valid() && replay.valid() && witness.valid(),
				"unclaimed owner receipts initially valid");

		const auto before = sqlite_same_process_shm_registry_test_peer::state_destruction_count();
		std::barrier start{3};
		std::array<bool, 2> succeeded{};
		std::array<sqlite_shm_lease_rejection_reason, 2> failures{};
		std::array<std::unique_ptr<sqlite_same_process_shm_mapping_registry>, 2> registries;
		std::thread first{
			[&, claim = std::move(owner)]() mutable
			{
				start.arrive_and_wait();
				auto result = sqlite_same_process_shm_mapping_registry::create(std::move(claim));
				succeeded[0] = result.has_value();
				if (result)
					registries[0] = std::move(result.value());
				else
					failures[0] = result.error().reason;
			},
		};
		std::thread second{
			[&, claim = std::move(replay)]() mutable
			{
				start.arrive_and_wait();
				auto result = sqlite_same_process_shm_mapping_registry::create(std::move(claim));
				succeeded[1] = result.has_value();
				if (result)
					registries[1] = std::move(result.value());
				else
					failures[1] = result.error().reason;
			},
		};
		start.arrive_and_wait();
		first.join();
		second.join();
		const auto success_count =
			static_cast<unsigned>(succeeded[0]) + static_cast<unsigned>(succeeded[1]);
		require(success_count == 1U && !witness.valid(),
				"process owner seal has one atomic winner and remains claimed");
		const auto loser_failure = succeeded[0] ? failures[1] : failures[0];
		require(loser_failure == sqlite_shm_lease_rejection_reason::stale_token,
				"concurrent replayed process owner claim rejected as stale");
		registries[0].reset();
		registries[1].reset();
		require(sqlite_same_process_shm_registry_test_peer::state_destruction_count() ==
					before + 1U,
				"live registry state destroyed exactly once");
	}

	void verify_registration_arm_and_exact_receipt_binding()
	{
		const auto process = identity("test.registry.process", 6U);
		const auto cohort = identity("test.registry.cohort", 6U);
		const auto alias_lifetime = identity("test.registry.alias", 6U);
		const auto runtime_identity = identity("test.registry.runtime", 6U);
		const auto runtime_pin_identity = identity("test.registry.runtime-pin", 6U);
		const auto registration_epoch = identity("test.registry.registration", 6U);
		const auto unregistration_epoch = identity("test.registry.unregistration", 6U);
		auto process_owner = sqlite_same_process_shm_registry_test_peer::process_owner(process);
		auto created = sqlite_same_process_shm_mapping_registry::create(std::move(process_owner));
		require(created.has_value(), "create registration-arm registry");
		auto registry = std::move(created.value());

		auto destruction_count = std::make_shared<std::atomic_int>(0);
		auto runtime_owner = std::make_shared<runtime_owner_probe>(destruction_count);
		const auto runtime_owner_weak = std::weak_ptr<runtime_owner_probe>{runtime_owner};
		auto adopted = sqlite_same_process_shm_registry_test_peer::adopt_runtime_lifetime(
			*registry, runtime_identity, runtime_pin_identity, runtime_owner);
		require(adopted.has_value(), "adopt registration-arm runtime owner");
		runtime_owner.reset();
		auto binding = sqlite_same_process_shm_registry_test_peer::alias_binding(
			process, cohort, alias_lifetime, std::move(adopted.value()));
		auto reserved = registry->reserve_alias(std::move(binding));
		require(reserved.has_value(), "reserve registration-arm alias");
		std::optional<sqlite_shm_registry_alias_pin> alias;
		alias.emplace(std::move(reserved.value()));

		const auto registration =
			sqlite_same_process_shm_registry_test_peer::registration_receipt(process,
																			 cohort,
																			 alias_lifetime,
																			 runtime_identity,
																			 runtime_pin_identity,
																			 registration_epoch);
		const auto premature = registry->confirm_alias_registered(*alias, registration);
		require(!premature.has_value() &&
					premature.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
				"registration cannot be confirmed before the native-effect arm");
		require(registry->begin_alias_register(*alias).has_value(), "registration arm is one-shot");
		const auto cancel_after_arm = registry->cancel_unregistered_alias(*alias);
		require(!cancel_after_arm.has_value() &&
					cancel_after_arm.error().reason ==
						sqlite_shm_lease_rejection_reason::invalid_request,
				"armed native registration cannot be cancelled as pre-effect state");

		const auto child = ::fork();
		require(child >= 0, "fork registration receipt mismatch");
		if (child == 0)
		{
			const auto mismatched =
				sqlite_same_process_shm_registry_test_peer::registration_receipt(
					process,
					identity("test.registry.wrong-cohort", 6U),
					alias_lifetime,
					runtime_identity,
					runtime_pin_identity,
					registration_epoch);
			const auto rejected = registry->confirm_alias_registered(*alias, mismatched);
			const auto snapshot = registry->snapshot();
			const bool retained = !runtime_owner_weak.expired() &&
				destruction_count->load(std::memory_order_relaxed) == 0;
			::_exit(!rejected.has_value() &&
							rejected.error().reason ==
								sqlite_shm_lease_rejection_reason::receipt_mismatch &&
							rejected.error().action ==
								sqlite_shm_lease_recovery_action::quarantine_no_retry &&
							!alias->valid() && snapshot.quarantined_alias_count == 1U && retained
						? 0
						: 1);
		}
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait registration receipt mismatch");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"receipt mismatch quarantines and retains the armed runtime owner");

		require(registry->confirm_alias_registered(*alias, registration).has_value(),
				"exact registration receipt confirms alias");
		const auto replay = registry->confirm_alias_registered(*alias, registration);
		require(!replay.has_value() &&
					replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
				"registration receipt cannot be replayed");
		require(registry->begin_alias_unregister(*alias).has_value(),
				"begin registration-arm alias unregister");
		require(registry->poll_alias_unregister(*alias).has_value(),
				"empty registration-arm alias is quiescent");
		const auto unregistration =
			sqlite_same_process_shm_registry_test_peer::unregistration_receipt(
				process,
				cohort,
				alias_lifetime,
				runtime_identity,
				runtime_pin_identity,
				registration_epoch,
				unregistration_epoch);
		require(registry->confirm_alias_unregistered(*alias, unregistration).has_value(),
				"exact unregistration receipt detaches armed alias");
		alias.reset();
		require(runtime_owner_weak.expired() &&
					destruction_count->load(std::memory_order_relaxed) == 1,
				"exact armed lifecycle releases runtime owner once");
	}

	void verify_reserved_alias_teardown_releases_runtime_owner_once()
	{
		const auto exercise = [](const std::uint8_t marker, const bool cancel_explicitly)
		{
			const auto process = identity("test.registry.reserved-process", marker);
			const auto cohort = identity("test.registry.reserved-cohort", marker);
			auto registry = make_registry(process);
			auto destruction_count = std::make_shared<std::atomic_int>(0);
			auto runtime_owner = std::make_shared<runtime_owner_probe>(destruction_count);
			const auto runtime_owner_weak = std::weak_ptr<runtime_owner_probe>{runtime_owner};
			auto adopted = sqlite_same_process_shm_registry_test_peer::adopt_runtime_lifetime(
				*registry,
				identity("test.registry.reserved-runtime", marker),
				identity("test.registry.reserved-runtime-pin", marker),
				runtime_owner);
			require(adopted.has_value(), "adopt reserved-alias runtime owner");
			runtime_owner.reset();
			auto binding = sqlite_same_process_shm_registry_test_peer::alias_binding(
				process,
				cohort,
				identity("test.registry.reserved-alias", marker),
				std::move(adopted.value()));
			auto reserved = registry->reserve_alias(std::move(binding));
			require(reserved.has_value(), "reserve pre-effect alias");
			std::optional<sqlite_shm_registry_alias_pin> alias;
			alias.emplace(std::move(reserved.value()));

			if (cancel_explicitly)
			{
				require(registry->cancel_unregistered_alias(*alias).has_value(),
						"cancel proven pre-effect alias");
				require(!alias->valid(), "cancel disarms reserved alias pin");
			}
			alias.reset();
			const auto snapshot = registry->snapshot();
			require(snapshot.reserved_alias_count == 0U &&
						snapshot.detached_alias_tombstone_count == 1U &&
						runtime_owner_weak.expired() &&
						destruction_count->load(std::memory_order_relaxed) == 1,
					"reserved alias teardown releases its runtime owner exactly once");
			registry.reset();
			require(destruction_count->load(std::memory_order_relaxed) == 1,
					"registry destruction cannot release a detached owner twice");
		};

		exercise(17U, true);
		exercise(18U, false);
	}

	void verify_registered_alias_abandonment_retains_owner_through_state_teardown()
	{
		const auto process = identity("test.registry.abandon-process", 22U);
		const auto cohort = identity("test.registry.abandon-cohort", 22U);
		auto registry = make_registry(process);
		auto destruction_count = std::make_shared<std::atomic_int>(0);
		auto runtime_owner = std::make_shared<runtime_owner_probe>(destruction_count);
		auto member = register_alias(*registry, process, cohort, 72U, runtime_owner);
		const auto runtime_owner_weak = std::weak_ptr<runtime_owner_probe>{runtime_owner};
		runtime_owner.reset();
		const auto state_destruction_before =
			sqlite_same_process_shm_registry_test_peer::state_destruction_count();

		const auto child = ::fork();
		require(child >= 0, "fork registered-alias teardown");
		if (child == 0)
		{
			member.alias.reset();
			const auto quarantined = registry->snapshot();
			registry.reset();
			const bool state_released =
				sqlite_same_process_shm_registry_test_peer::state_destruction_count() ==
				state_destruction_before + 1U;
			const bool owner_retained = !runtime_owner_weak.expired() &&
				destruction_count->load(std::memory_order_relaxed) == 0;
			::_exit(quarantined.quarantined_alias_count == 1U &&
							!quarantined.registry_quarantined && state_released && owner_retained
						? 0
						: 1);
		}
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait registered-alias teardown");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"registered alias abandonment releases state but retains native owner");

		unregister_alias(*registry, process, cohort, member);
		require(runtime_owner_weak.expired() &&
					destruction_count->load(std::memory_order_relaxed) == 1,
				"registered-alias teardown probe leaves parent lifecycle clean");
	}

	void verify_shared_runtime_owner_and_identity_tombstones()
	{
		const auto process = identity("test.registry.process", 12U);
		const auto cohort = identity("test.registry.cohort", 12U);
		auto registry = make_registry(process);
		auto shared_count = std::make_shared<std::atomic_int>(0);
		auto shared_owner = std::make_shared<runtime_owner_probe>(shared_count);
		auto first = register_alias(*registry, process, cohort, 40U, shared_owner);
		auto second = register_alias(*registry, process, cohort, 41U, shared_owner);
		const auto shared_owner_weak = std::weak_ptr<runtime_owner_probe>{shared_owner};
		shared_owner.reset();

		unregister_alias(*registry, process, cohort, first);
		require(!shared_owner_weak.expired() && shared_count->load(std::memory_order_relaxed) == 0,
				"detaching one alias preserves a shared runtime owner");
		unregister_alias(*registry, process, cohort, second);
		require(shared_owner_weak.expired() && shared_count->load(std::memory_order_relaxed) == 1,
				"detaching the last alias releases shared runtime owner exactly once");

		auto reused_runtime_count = std::make_shared<std::atomic_int>(0);
		auto reused_runtime_owner = std::make_shared<runtime_owner_probe>(reused_runtime_count);
		const auto reused_runtime_weak = std::weak_ptr<runtime_owner_probe>{reused_runtime_owner};
		auto reused_runtime = sqlite_same_process_shm_registry_test_peer::adopt_runtime_lifetime(
			*registry,
			first.runtime_identity,
			identity("test.registry.runtime-pin", 42U),
			reused_runtime_owner);
		require(reused_runtime.has_value(), "adopt runtime identity replay probe");
		reused_runtime_owner.reset();
		auto reused_runtime_binding = sqlite_same_process_shm_registry_test_peer::alias_binding(
			process,
			cohort,
			identity("test.registry.alias", 42U),
			std::move(reused_runtime.value()));
		const auto runtime_rejected = registry->reserve_alias(std::move(reused_runtime_binding));
		require(!runtime_rejected.has_value() &&
					runtime_rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::stale_token &&
					reused_runtime_weak.expired() &&
					reused_runtime_count->load(std::memory_order_relaxed) == 1,
				"detached runtime identity tombstone cannot be reused");

		auto reused_pin_count = std::make_shared<std::atomic_int>(0);
		auto reused_pin_owner = std::make_shared<runtime_owner_probe>(reused_pin_count);
		const auto reused_pin_weak = std::weak_ptr<runtime_owner_probe>{reused_pin_owner};
		auto reused_pin = sqlite_same_process_shm_registry_test_peer::adopt_runtime_lifetime(
			*registry,
			identity("test.registry.runtime", 43U),
			second.runtime_pin_identity,
			reused_pin_owner);
		require(reused_pin.has_value(), "adopt runtime pin replay probe");
		reused_pin_owner.reset();
		auto reused_pin_binding = sqlite_same_process_shm_registry_test_peer::alias_binding(
			process, cohort, identity("test.registry.alias", 43U), std::move(reused_pin.value()));
		const auto pin_rejected = registry->reserve_alias(std::move(reused_pin_binding));
		require(!pin_rejected.has_value() &&
					pin_rejected.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
					reused_pin_weak.expired() &&
					reused_pin_count->load(std::memory_order_relaxed) == 1,
				"detached runtime pin identity tombstone cannot be reused");

		auto unowned_storage = std::make_unique<int>(7);
		std::shared_ptr<void> nonowning{std::shared_ptr<void>{}, unowned_storage.get()};
		const auto nonowning_rejected =
			sqlite_same_process_shm_registry_test_peer::adopt_runtime_lifetime(
				*registry,
				identity("test.registry.runtime", 44U),
				identity("test.registry.runtime-pin", 44U),
				std::move(nonowning));
		require(!nonowning_rejected.has_value() &&
					nonowning_rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::invalid_identity,
				"nonnull aliasing shared_ptr without an owner control block is rejected");
		const auto snapshot = registry->snapshot();
		require(snapshot.duplicate_rejection_count == 2U && !snapshot.registry_quarantined,
				"identity replay is counted without weakening unrelated registry state");

		auto reused_alias_count = std::make_shared<std::atomic_int>(0);
		auto reused_alias_owner = std::make_shared<runtime_owner_probe>(reused_alias_count);
		const auto reused_alias_weak = std::weak_ptr<runtime_owner_probe>{reused_alias_owner};
		auto reused_alias = sqlite_same_process_shm_registry_test_peer::adopt_runtime_lifetime(
			*registry,
			identity("test.registry.runtime", 45U),
			identity("test.registry.runtime-pin", 45U),
			reused_alias_owner);
		require(reused_alias.has_value(), "adopt detached alias-lifetime replay probe");
		reused_alias_owner.reset();
		auto reused_alias_binding = sqlite_same_process_shm_registry_test_peer::alias_binding(
			process, cohort, first.alias_lifetime, std::move(reused_alias.value()));
		const auto alias_rejected = registry->reserve_alias(std::move(reused_alias_binding));
		const auto quarantined = registry->snapshot();
		require(!alias_rejected.has_value() &&
					alias_rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					alias_rejected.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					quarantined.registry_quarantined &&
					quarantined.cross_binding_rejection_count == 1U &&
					quarantined.alias_record_count == snapshot.alias_record_count &&
					quarantined.family_record_count == 0U &&
					quarantined.active_family_pin_count == 0U &&
					quarantined.active_activity_pin_count == 0U && reused_alias_weak.expired() &&
					reused_alias_count->load(std::memory_order_relaxed) == 1,
				"detached alias lifetime cannot be rebound to fresh runtime authority");
	}

	void verify_registration_epoch_replay_and_armed_abandonment()
	{
		const auto process = identity("test.registry.process", 13U);
		const auto cohort = identity("test.registry.cohort", 13U);
		auto registry = make_registry(process);
		auto first_count = std::make_shared<std::atomic_int>(0);
		auto first_owner = std::make_shared<runtime_owner_probe>(first_count);
		auto first = register_alias(*registry, process, cohort, 50U, first_owner);
		first_owner.reset();

		alias_member second;
		second.alias_lifetime = identity("test.registry.alias", 51U);
		second.runtime_identity = identity("test.registry.runtime", 51U);
		second.runtime_pin_identity = identity("test.registry.runtime-pin", 51U);
		second.registration_epoch = identity("test.registry.registration", 51U);
		second.unregistration_epoch = identity("test.registry.unregistration", 51U);
		second.owner_destruction_count = std::make_shared<std::atomic_int>(0);
		auto second_owner = std::make_shared<runtime_owner_probe>(second.owner_destruction_count);
		second.owner = second_owner;
		auto adopted = sqlite_same_process_shm_registry_test_peer::adopt_runtime_lifetime(
			*registry, second.runtime_identity, second.runtime_pin_identity, second_owner);
		require(adopted.has_value(), "adopt epoch-replay runtime owner");
		second_owner.reset();
		auto binding = sqlite_same_process_shm_registry_test_peer::alias_binding(
			process, cohort, second.alias_lifetime, std::move(adopted.value()));
		auto reserved = registry->reserve_alias(std::move(binding));
		require(reserved.has_value(), "reserve epoch-replay alias");
		second.alias.emplace(std::move(reserved.value()));
		require(registry->begin_alias_register(*second.alias).has_value(),
				"arm epoch-replay alias registration");

		const auto replay_child = ::fork();
		require(replay_child >= 0, "fork registration epoch replay");
		if (replay_child == 0)
		{
			const auto replayed = sqlite_same_process_shm_registry_test_peer::registration_receipt(
				process,
				cohort,
				second.alias_lifetime,
				second.runtime_identity,
				second.runtime_pin_identity,
				first.registration_epoch);
			const auto rejected = registry->confirm_alias_registered(*second.alias, replayed);
			const auto snapshot = registry->snapshot();
			::_exit(!rejected.has_value() &&
							rejected.error().reason ==
								sqlite_shm_lease_rejection_reason::receipt_mismatch &&
							rejected.error().action ==
								sqlite_shm_lease_recovery_action::quarantine_no_retry &&
							snapshot.registered_alias_count == 1U &&
							snapshot.quarantined_alias_count == 1U && !second.owner.expired()
						? 0
						: 1);
		}
		int status{};
		require(::waitpid(replay_child, &status, 0) == replay_child,
				"wait registration epoch replay");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"registration epoch cannot be reused by another alias");

		const auto abandonment_child = ::fork();
		require(abandonment_child >= 0, "fork armed alias abandonment");
		if (abandonment_child == 0)
		{
			second.alias.reset();
			const auto snapshot = registry->snapshot();
			::_exit(snapshot.registered_alias_count == 1U &&
							snapshot.quarantined_alias_count == 1U && !second.owner.expired() &&
							second.owner_destruction_count->load(std::memory_order_relaxed) == 0
						? 0
						: 1);
		}
		status = 0;
		require(::waitpid(abandonment_child, &status, 0) == abandonment_child,
				"wait armed alias abandonment");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"armed alias abandonment retains runtime owner in quarantine");

		const auto exact = sqlite_same_process_shm_registry_test_peer::registration_receipt(
			process,
			cohort,
			second.alias_lifetime,
			second.runtime_identity,
			second.runtime_pin_identity,
			second.registration_epoch);
		require(registry->confirm_alias_registered(*second.alias, exact).has_value(),
				"parent confirms unreplayed registration epoch");
		unregister_alias(*registry, process, cohort, second);
		unregister_alias(*registry, process, cohort, first);
		require(first.owner.expired() && second.owner.expired() &&
					first_count->load(std::memory_order_relaxed) == 1 &&
					second.owner_destruction_count->load(std::memory_order_relaxed) == 1,
				"epoch replay probes leave parent lifecycle clean");
	}

	void verify_receipt_epochs_are_direction_and_lifecycle_unique()
	{
		const auto process = identity("test.registry.receipt-process", 19U);
		const auto cohort = identity("test.registry.receipt-cohort", 19U);
		auto registry = make_registry(process);

		auto completed_count = std::make_shared<std::atomic_int>(0);
		auto completed_owner = std::make_shared<runtime_owner_probe>(completed_count);
		auto completed = register_alias(*registry, process, cohort, 70U, completed_owner);
		completed_owner.reset();
		unregister_alias(*registry, process, cohort, completed);
		require(completed.owner.expired() && completed_count->load(std::memory_order_relaxed) == 1,
				"establish completed registration and unregistration epochs");

		alias_member current;
		current.alias_lifetime = identity("test.registry.alias", 71U);
		current.runtime_identity = identity("test.registry.runtime", 71U);
		current.runtime_pin_identity = identity("test.registry.runtime-pin", 71U);
		current.registration_epoch = identity("test.registry.registration", 71U);
		current.unregistration_epoch = identity("test.registry.unregistration", 71U);
		current.owner_destruction_count = std::make_shared<std::atomic_int>(0);
		auto current_owner = std::make_shared<runtime_owner_probe>(current.owner_destruction_count);
		current.owner = current_owner;
		auto adopted = sqlite_same_process_shm_registry_test_peer::adopt_runtime_lifetime(
			*registry, current.runtime_identity, current.runtime_pin_identity, current_owner);
		require(adopted.has_value(), "adopt cross-direction replay runtime owner");
		current_owner.reset();
		auto binding = sqlite_same_process_shm_registry_test_peer::alias_binding(
			process, cohort, current.alias_lifetime, std::move(adopted.value()));
		auto reserved = registry->reserve_alias(std::move(binding));
		require(reserved.has_value(), "reserve cross-direction replay alias");
		current.alias.emplace(std::move(reserved.value()));
		require(registry->begin_alias_register(*current.alias).has_value(),
				"arm cross-direction replay registration");

		const auto registration_replay_child = ::fork();
		require(registration_replay_child >= 0, "fork unregistration-to-registration replay");
		if (registration_replay_child == 0)
		{
			const auto replayed = sqlite_same_process_shm_registry_test_peer::registration_receipt(
				process,
				cohort,
				current.alias_lifetime,
				current.runtime_identity,
				current.runtime_pin_identity,
				completed.unregistration_epoch);
			const auto rejected = registry->confirm_alias_registered(*current.alias, replayed);
			const auto snapshot = registry->snapshot();
			::_exit(!rejected.has_value() &&
							rejected.error().reason ==
								sqlite_shm_lease_rejection_reason::receipt_mismatch &&
							rejected.error().action ==
								sqlite_shm_lease_recovery_action::quarantine_no_retry &&
							!current.alias->valid() &&
							snapshot.detached_alias_tombstone_count == 1U &&
							snapshot.quarantined_alias_count == 1U && !current.owner.expired()
						? 0
						: 1);
		}
		int status{};
		require(::waitpid(registration_replay_child, &status, 0) == registration_replay_child,
				"wait unregistration-to-registration replay");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"an unregistration epoch cannot be replayed as registration authority");

		const auto exact_registration =
			sqlite_same_process_shm_registry_test_peer::registration_receipt(
				process,
				cohort,
				current.alias_lifetime,
				current.runtime_identity,
				current.runtime_pin_identity,
				current.registration_epoch);
		require(registry->confirm_alias_registered(*current.alias, exact_registration).has_value(),
				"parent confirms unique registration epoch");
		require(registry->begin_alias_unregister(*current.alias).has_value(),
				"begin receipt replay alias unregister");
		require(registry->poll_alias_unregister(*current.alias).has_value(),
				"empty receipt replay alias is quiescent");

		const auto expect_unregistration_epoch_rejected =
			[&](const sqlite_backend_opaque_identity& replayed_epoch)
		{
			const auto child = ::fork();
			require(child >= 0, "fork registration-to-unregistration epoch replay");
			if (child == 0)
			{
				const auto replayed =
					sqlite_same_process_shm_registry_test_peer::unregistration_receipt(
						process,
						cohort,
						current.alias_lifetime,
						current.runtime_identity,
						current.runtime_pin_identity,
						current.registration_epoch,
						replayed_epoch);
				const auto rejected =
					registry->confirm_alias_unregistered(*current.alias, replayed);
				const auto snapshot = registry->snapshot();
				::_exit(!rejected.has_value() &&
								rejected.error().reason ==
									sqlite_shm_lease_rejection_reason::receipt_mismatch &&
								rejected.error().action ==
									sqlite_shm_lease_recovery_action::quarantine_no_retry &&
								!current.alias->valid() &&
								snapshot.detached_alias_tombstone_count == 1U &&
								snapshot.quarantined_alias_count == 1U && !current.owner.expired()
							? 0
							: 1);
			}
			int child_status{};
			require(::waitpid(child, &child_status, 0) == child,
					"wait registration-to-unregistration epoch replay");
			require(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
					"seen epoch is rejected across receipt direction and alias lifecycle");
		};
		expect_unregistration_epoch_rejected(current.registration_epoch);
		expect_unregistration_epoch_rejected(completed.unregistration_epoch);

		const auto exact_unregistration =
			sqlite_same_process_shm_registry_test_peer::unregistration_receipt(
				process,
				cohort,
				current.alias_lifetime,
				current.runtime_identity,
				current.runtime_pin_identity,
				current.registration_epoch,
				current.unregistration_epoch);
		require(
			registry->confirm_alias_unregistered(*current.alias, exact_unregistration).has_value(),
			"parent confirms unique unregistration epoch");
		const auto replay =
			registry->confirm_alias_unregistered(*current.alias, exact_unregistration);
		require(!replay.has_value() &&
					replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
				"completed unregistration receipt replay is exactly stale");
		current.alias.reset();
		require(current.owner.expired() &&
					current.owner_destruction_count->load(std::memory_order_relaxed) == 1,
				"receipt replay probes leave current runtime owner clean");
	}

	void verify_registered_family_happy_path()
	{
		auto fixture = make_fixture(2U, true);
		const auto snapshot = fixture.registry->snapshot();
		require(snapshot.process_live && snapshot.registered_alias_count == 1U &&
					snapshot.active_family_count == 1U && snapshot.active_family_pin_count == 1U &&
					snapshot.active_activity_pin_count == 1U &&
					snapshot.generation_source_count == 1U,
				"registered family census is exact");
		const auto family = fixture.registry->family_snapshot(fixture.family);
		require(family.exact_active_match_count == 1U && family.lookup_visible &&
					family.coordinator_present && family.alias_pin_count == 1U &&
					family.activity_pin_count == 1U,
				"singleton family coordinator is visible");
		const auto first_entry_epoch = family.entry_epoch;
		const auto* generation_source =
			sqlite_same_process_shm_registry_test_peer::generation_source_identity(
				*fixture.registry);
		clean_fixture(fixture);
		const auto retired = fixture.registry->family_snapshot(fixture.family);
		require(retired.exact_active_match_count == 0U && retired.exact_retired_match_count == 1U &&
					!retired.lookup_visible,
				"last clean family pin leaves an invisible retired tombstone");

		auto successor_count = std::make_shared<std::atomic_int>(0);
		auto successor_owner = std::make_shared<runtime_owner_probe>(successor_count);
		auto successor = register_alias(*fixture.registry,
										fixture.process_instance,
										fixture.cohort,
										22U,
										successor_owner,
										&fixture.family);
		const auto successor_owner_weak = std::weak_ptr<runtime_owner_probe>{successor_owner};
		successor_owner.reset();
		const auto fresh = fixture.registry->family_snapshot(fixture.family);
		require(fresh.exact_active_match_count == 1U && fresh.exact_retired_match_count == 1U &&
					fresh.entry_epoch > first_entry_epoch &&
					sqlite_same_process_shm_registry_test_peer::generation_source_identity(
						*fixture.registry) == generation_source,
				"retired family gets a fresh epoch on the one registry generation source");
		unregister_alias(*fixture.registry, fixture.process_instance, fixture.cohort, successor);
		require(successor_owner_weak.expired() &&
					successor_count->load(std::memory_order_relaxed) == 1,
				"fresh family successor releases its exact runtime owner");
	}

	void verify_duplicate_family_blocks_existing_pin_admission()
	{
		auto fixture = make_fixture(3U, false);
		const auto child = ::fork();
		require(child >= 0, "fork duplicate-family counterexample");
		if (child == 0)
		{
			const bool injected =
				sqlite_same_process_shm_registry_test_peer::inject_duplicate_family(
					*fixture.registry, fixture.family);
			auto activity = fixture.registry->acquire_activity(*fixture.family_pin);
			const auto snapshot = fixture.registry->snapshot();
			const bool closed = injected && !activity.has_value() &&
				activity.error().reason == sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				snapshot.registry_quarantined && snapshot.ambiguous_lookup_count == 1U;
			::_exit(closed ? 0 : 1);
		}
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait duplicate-family counterexample");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"duplicate family never selects a first coordinator");

		const auto retirement_child = ::fork();
		require(retirement_child >= 0, "fork duplicate-family retirement counterexample");
		if (retirement_child == 0)
		{
			const bool injected =
				sqlite_same_process_shm_registry_test_peer::inject_duplicate_family(
					*fixture.registry, fixture.family);
			const auto released = fixture.registry->release_family(*fixture.family_pin);
			const auto family = fixture.registry->family_snapshot(fixture.family);
			const auto snapshot = fixture.registry->snapshot();
			const bool closed = injected && !released.has_value() &&
				released.error().reason == sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				!fixture.family_pin->valid() && family.exact_active_match_count == 0U &&
				family.exact_retired_match_count == 0U &&
				family.exact_quarantined_match_count == 2U && snapshot.registry_quarantined;
			::_exit(closed ? 0 : 1);
		}
		status = 0;
		require(::waitpid(retirement_child, &status, 0) == retirement_child,
				"wait duplicate-family retirement counterexample");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"duplicate family cannot be laundered through last-pin retirement");
		clean_fixture(fixture);
	}

	void verify_shared_family_unregister_waits_for_exact_coordinator_quiescence()
	{
		auto fixture = make_fixture(5U, true);
		auto member_count = std::make_shared<std::atomic_int>(0);
		auto member_owner = std::make_shared<runtime_owner_probe>(member_count);
		auto member = register_alias(*fixture.registry,
									 fixture.process_instance,
									 fixture.cohort,
									 25U,
									 member_owner,
									 &fixture.family);
		const auto member_owner_weak = std::weak_ptr<runtime_owner_probe>{member_owner};
		member_owner.reset();

		require(fixture.registry->release_activity(*fixture.activity).has_value(),
				"release first-epoch activity");
		fixture.activity.reset();
		require(fixture.registry->release_family(*fixture.family_pin).has_value(),
				"release first alias first-epoch family pin");
		fixture.family_pin.reset();
		require(fixture.registry->release_family(*member.family_pin).has_value(),
				"last first-epoch family pin retires coordinator");
		member.family_pin.reset();
		const auto retired = fixture.registry->family_snapshot(fixture.family);
		require(retired.exact_active_match_count == 0U && retired.exact_retired_match_count == 1U,
				"shared aliases retain a retired first family epoch");

		auto first_fresh = fixture.registry->install_or_join_family(*fixture.alias, fixture.family);
		require(first_fresh.has_value(), "first alias installs fresh family epoch");
		fixture.family_pin.emplace(std::move(first_fresh.value()));
		auto second_fresh = fixture.registry->pin_existing_family(*member.alias, fixture.family);
		require(second_fresh.has_value(), "second alias joins fresh family epoch");
		member.family_pin.emplace(std::move(second_fresh.value()));
		auto fresh_activity = fixture.registry->acquire_activity(*fixture.family_pin);
		require(fresh_activity.has_value(), "acquire fresh-epoch activity");
		fixture.activity.emplace(std::move(fresh_activity.value()));
		const auto fresh = fixture.registry->family_snapshot(fixture.family);
		require(fresh.exact_active_match_count == 1U && fresh.exact_retired_match_count == 1U,
				"fresh active family coexists only with retired predecessor");

		auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
			*fixture.registry, *fixture.activity);
		require(coordinator != nullptr, "activity resolves exact fresh family coordinator");
		const auto connection = identity("test.registry.connection", 5U);
		const auto open_epoch = identity("test.registry.open-epoch", 5U);
		const auto gate = sqlite_same_process_shm_lease_test_peer::eligibility(
			fixture.family,
			connection,
			open_epoch,
			effect_gate(connection, 5U),
			identity("test.registry.complete-gate", 5U));
		auto eligibility = coordinator->install_writer_eligibility(gate);
		require(eligibility.has_value(), "install coordinator nonquiescence token");

		require(fixture.registry->release_activity(*fixture.activity).has_value(),
				"release first alias fresh-epoch activity");
		fixture.activity.reset();
		require(fixture.registry->release_family(*fixture.family_pin).has_value(),
				"release nonlast fresh shared family pin");
		fixture.family_pin.reset();
		require(fixture.registry->begin_alias_unregister(*fixture.alias).has_value(),
				"hide first alias admission before unregister");
		const auto waiting = fixture.registry->poll_alias_unregister(*fixture.alias);
		require(!waiting.has_value() &&
					waiting.error().reason == sqlite_shm_lease_rejection_reason::retiring,
				"historical family participation waits for coordinator quiescence");

		require(coordinator->revoke_writer_eligibility(eligibility.value()).has_value(),
				"revoke coordinator nonquiescence token");
		require(fixture.registry->poll_alias_unregister(*fixture.alias).has_value(),
				"quiescent shared coordinator permits exact alias detach");
		const auto first_unregistration =
			sqlite_same_process_shm_registry_test_peer::unregistration_receipt(
				fixture.process_instance,
				fixture.cohort,
				fixture.alias_lifetime,
				fixture.runtime_identity,
				fixture.runtime_pin_identity,
				fixture.registration_epoch,
				fixture.unregistration_epoch);
		require(fixture.registry->confirm_alias_unregistered(*fixture.alias, first_unregistration)
					.has_value(),
				"confirm first shared alias detach");
		fixture.alias.reset();
		require(fixture.owner.expired() &&
					fixture.owner_destruction_count->load(std::memory_order_relaxed) == 1,
				"first shared alias releases only its runtime owner");

		unregister_alias(*fixture.registry, fixture.process_instance, fixture.cohort, member);
		require(member_owner_weak.expired() && member_count->load(std::memory_order_relaxed) == 1,
				"last shared alias retires family and releases its runtime owner");
	}

	void verify_nonquiescent_last_family_release_is_retryable()
	{
		auto fixture = make_fixture(20U, true);
		auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
			*fixture.registry, *fixture.activity);
		require(coordinator != nullptr, "resolve last-pin retry coordinator");
		const auto connection = identity("test.registry.retry-connection", 20U);
		const auto open_epoch = identity("test.registry.retry-open-epoch", 20U);
		auto eligibility =
			install_eligibility(*coordinator, fixture.family, connection, open_epoch, 20U);
		require(fixture.registry->release_activity(*fixture.activity).has_value(),
				"release last-pin retry activity");
		fixture.activity.reset();

		const auto blocked = fixture.registry->release_family(*fixture.family_pin);
		const auto blocked_snapshot = fixture.registry->snapshot();
		const auto blocked_family = fixture.registry->family_snapshot(fixture.family);
		require(!blocked.has_value() &&
					blocked.error().reason == sqlite_shm_lease_rejection_reason::retiring &&
					blocked.error().action ==
						sqlite_shm_lease_recovery_action::await_complete_attachment_gate_boundary &&
					fixture.alias->valid() && fixture.family_pin->valid() &&
					blocked_snapshot.active_family_pin_count == 1U &&
					blocked_snapshot.quarantined_alias_count == 0U &&
					blocked_snapshot.quarantined_family_count == 0U &&
					blocked_family.exact_active_match_count == 1U &&
					blocked_family.alias_pin_count == 1U,
				"nonquiescent last-pin release fails closed without consuming authority");

		require(coordinator->revoke_writer_eligibility(eligibility).has_value(),
				"make last-pin retry coordinator quiescent");
		require(fixture.registry->release_family(*fixture.family_pin).has_value(),
				"same last family pin releases after quiescence");
		require(!fixture.family_pin->valid(),
				"successful last-pin retry consumes its exact authority once");
		fixture.family_pin.reset();
		clean_fixture(fixture);
	}

	void verify_nonquiescent_last_family_abandonment_reaches_historical_participant()
	{
		auto fixture = make_fixture(26U, true);
		auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
			*fixture.registry, *fixture.activity);
		require(coordinator != nullptr, "resolve last-pin abandonment coordinator");

		auto member_count = std::make_shared<std::atomic_int>(0);
		auto member_owner = std::make_shared<runtime_owner_probe>(member_count);
		auto member = register_alias(*fixture.registry,
									 fixture.process_instance,
									 fixture.cohort,
									 76U,
									 member_owner,
									 &fixture.family);
		const auto member_owner_weak = std::weak_ptr<runtime_owner_probe>{member_owner};
		member_owner.reset();

		const auto connection = identity("test.registry.abandon-retry-connection", 26U);
		const auto open_epoch = identity("test.registry.abandon-retry-open-epoch", 26U);
		auto eligibility =
			install_eligibility(*coordinator, fixture.family, connection, open_epoch, 26U);
		const auto child = ::fork();
		require(child >= 0, "fork nonquiescent last-family abandonment");
		if (child == 0)
		{
			require(fixture.registry->release_activity(*fixture.activity).has_value(),
					"release historical abandonment participant activity");
			fixture.activity.reset();
			require(fixture.registry->release_family(*fixture.family_pin).has_value(),
					"release historical abandonment participant nonlast family pin");
			fixture.family_pin.reset();
			require(fixture.registry->begin_alias_unregister(*fixture.alias).has_value(),
					"begin historical abandonment participant unregister");
			const auto waiting = fixture.registry->poll_alias_unregister(*fixture.alias);
			const bool initially_waiting = !waiting.has_value() &&
				waiting.error().reason == sqlite_shm_lease_rejection_reason::retiring;

			member.family_pin.reset();
			const auto poll = fixture.registry->poll_alias_unregister(*fixture.alias);
			const auto unregistration =
				sqlite_same_process_shm_registry_test_peer::unregistration_receipt(
					fixture.process_instance,
					fixture.cohort,
					fixture.alias_lifetime,
					fixture.runtime_identity,
					fixture.runtime_pin_identity,
					fixture.registration_epoch,
					fixture.unregistration_epoch);
			const auto confirm =
				fixture.registry->confirm_alias_unregistered(*fixture.alias, unregistration);
			const auto snapshot = fixture.registry->snapshot();
			const auto family_snapshot = fixture.registry->family_snapshot(fixture.family);
			const bool historical_quarantined = !poll.has_value() &&
				poll.error().reason == sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				poll.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				!confirm.has_value() &&
				confirm.error().reason == sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				confirm.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				!fixture.alias->valid() && !member.alias->valid() && !fixture.owner.expired() &&
				!member_owner_weak.expired() &&
				fixture.owner_destruction_count->load(std::memory_order_relaxed) == 0 &&
				member_count->load(std::memory_order_relaxed) == 0 &&
				snapshot.active_family_pin_count == 0U &&
				snapshot.active_activity_pin_count == 0U &&
				snapshot.quarantined_alias_count == 2U && snapshot.quarantined_family_count == 1U &&
				!snapshot.registry_quarantined && family_snapshot.exact_active_match_count == 0U &&
				family_snapshot.exact_quarantined_match_count == 1U &&
				!family_snapshot.lookup_visible;
			::_exit(initially_waiting && historical_quarantined ? 0 : 1);
		}
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait nonquiescent last-family abandonment");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"last-family abandonment quarantines historical participants without retry");

		require(coordinator->revoke_writer_eligibility(eligibility).has_value(),
				"restore parent coordinator quiescence after abandonment probe");
		clean_fixture(fixture);
		unregister_alias(*fixture.registry, fixture.process_instance, fixture.cohort, member);
		require(member_owner_weak.expired() && member_count->load(std::memory_order_relaxed) == 1,
				"last-family abandonment probe leaves parent lifecycle clean");
	}

	void verify_unregister_hides_admission_then_drains_existing_activity()
	{
		auto fixture = make_fixture(14U, true);
		require(fixture.registry->begin_alias_unregister(*fixture.alias).has_value(),
				"unregister hides alias admission");
		const auto new_activity = fixture.registry->acquire_activity(*fixture.family_pin);
		require(!new_activity.has_value() &&
					new_activity.error().reason == sqlite_shm_lease_rejection_reason::retiring,
				"unregistering alias cannot mint new activity");
		const auto waiting = fixture.registry->poll_alias_unregister(*fixture.alias);
		require(!waiting.has_value() &&
					waiting.error().reason == sqlite_shm_lease_rejection_reason::retiring,
				"unregister waits for existing activity");
		const auto premature_receipt =
			sqlite_same_process_shm_registry_test_peer::unregistration_receipt(
				fixture.process_instance,
				fixture.cohort,
				fixture.alias_lifetime,
				fixture.runtime_identity,
				fixture.runtime_pin_identity,
				fixture.registration_epoch,
				fixture.unregistration_epoch);
		const auto premature =
			fixture.registry->confirm_alias_unregistered(*fixture.alias, premature_receipt);
		require(!premature.has_value() &&
					premature.error().reason == sqlite_shm_lease_rejection_reason::retiring,
				"native unregister confirmation cannot bypass activity drain");

		require(fixture.registry->release_activity(*fixture.activity).has_value(),
				"drain existing activity");
		fixture.activity.reset();
		require(fixture.registry->release_family(*fixture.family_pin).has_value(),
				"drain existing family pin");
		fixture.family_pin.reset();
		require(fixture.registry->poll_alias_unregister(*fixture.alias).has_value(),
				"drained alias becomes unregisterable");
		const auto mismatch_child = ::fork();
		require(mismatch_child >= 0, "fork unregistration receipt mismatch");
		if (mismatch_child == 0)
		{
			const auto mismatched =
				sqlite_same_process_shm_registry_test_peer::unregistration_receipt(
					fixture.process_instance,
					identity("test.registry.wrong-cohort", 14U),
					fixture.alias_lifetime,
					fixture.runtime_identity,
					fixture.runtime_pin_identity,
					fixture.registration_epoch,
					fixture.unregistration_epoch);
			const auto rejected =
				fixture.registry->confirm_alias_unregistered(*fixture.alias, mismatched);
			const auto snapshot = fixture.registry->snapshot();
			::_exit(!rejected.has_value() &&
							rejected.error().reason ==
								sqlite_shm_lease_rejection_reason::receipt_mismatch &&
							snapshot.quarantined_alias_count == 1U && !fixture.owner.expired()
						? 0
						: 1);
		}
		int status{};
		require(::waitpid(mismatch_child, &status, 0) == mismatch_child,
				"wait unregistration receipt mismatch");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"unregistration receipt mismatch quarantines without owner release");
		const auto receipt = sqlite_same_process_shm_registry_test_peer::unregistration_receipt(
			fixture.process_instance,
			fixture.cohort,
			fixture.alias_lifetime,
			fixture.runtime_identity,
			fixture.runtime_pin_identity,
			fixture.registration_epoch,
			fixture.unregistration_epoch);
		require(fixture.registry->confirm_alias_unregistered(*fixture.alias, receipt).has_value(),
				"exact drained alias unregisters");
		fixture.alias.reset();
		require(fixture.owner.expired() &&
					fixture.owner_destruction_count->load(std::memory_order_relaxed) == 1,
				"drained unregister releases runtime owner once");
	}

	void verify_foreign_release_never_consumes_another_registry_pin()
	{
		auto first = make_fixture(7U, true);
		auto second = make_fixture(8U, true);
		auto second_sealed = second.activity->seal_for_audit();
		require(second_sealed.has_value(), "mint foreign-release preservation seal");
		require(!sqlite_same_process_shm_registry_test_peer::activity_seal_matches(
					*first.registry,
					second_sealed.value(),
					second.process_instance,
					second.family,
					second.alias_lifetime) &&
					sqlite_same_process_shm_registry_test_peer::activity_seal_matches(
						*second.registry,
						second_sealed.value(),
						second.process_instance,
						second.family,
						second.alias_lifetime),
				"closed matcher rejects another registry while exact registry still accepts");
		const auto child = ::fork();
		require(child >= 0, "fork foreign-pin counterexample");
		if (child == 0)
		{
			sqlite_same_process_shm_registry_test_peer::invalidate_process_instance(
				*first.registry);
			const auto activity_rejection = first.registry->release_activity(*second.activity);
			const auto family_rejection = first.registry->release_family(*second.family_pin);
			const auto second_snapshot = second.registry->snapshot();
			const bool preserved = !activity_rejection.has_value() &&
				activity_rejection.error().reason ==
					sqlite_shm_lease_rejection_reason::receipt_mismatch &&
				!family_rejection.has_value() &&
				family_rejection.error().reason ==
					sqlite_shm_lease_rejection_reason::receipt_mismatch &&
				second.activity->valid() && second_sealed->valid() && second.family_pin->valid() &&
				second_snapshot.active_activity_pin_count == 1U &&
				second_snapshot.active_family_pin_count == 1U;
			::_exit(preserved ? 0 : 1);
		}
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait foreign-pin counterexample");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"foreign registry rejects without disarming another registry pin");
		clean_fixture(first);
		clean_fixture(second);
	}

	void verify_activity_abandonment_quarantines_local_authority()
	{
		auto fixture = make_fixture(9U, true);
		const auto state_destruction_before =
			sqlite_same_process_shm_registry_test_peer::state_destruction_count();
		const auto child = ::fork();
		require(child >= 0, "fork activity-abandonment counterexample");
		if (child == 0)
		{
			fixture.activity.reset();
			const auto snapshot = fixture.registry->snapshot();
			const bool quarantined = !fixture.alias->valid() && !fixture.family_pin->valid() &&
				snapshot.quarantined_alias_count == 1U && snapshot.quarantined_family_count == 1U &&
				snapshot.active_activity_pin_count == 0U && !snapshot.registry_quarantined &&
				!fixture.owner.expired() &&
				fixture.owner_destruction_count->load(std::memory_order_relaxed) == 0;
			fixture.family_pin.reset();
			const auto drained = fixture.registry->snapshot();
			fixture.alias.reset();
			fixture.registry.reset();
			const bool state_released =
				sqlite_same_process_shm_registry_test_peer::state_destruction_count() ==
				state_destruction_before + 1U;
			const bool owner_retained = !fixture.owner.expired() &&
				fixture.owner_destruction_count->load(std::memory_order_relaxed) == 0;
			::_exit(quarantined && drained.active_family_pin_count == 0U &&
							drained.active_activity_pin_count == 0U && state_released &&
							owner_retained
						? 0
						: 1);
		}
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait activity-abandonment counterexample");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"abandoned activity removes authority and retains quarantined owner");
		clean_fixture(fixture);
	}

	void verify_family_pin_abandonment_with_live_activity_is_cleanup_only()
	{
		auto fixture = make_fixture(21U, true);
		auto sibling_count = std::make_shared<std::atomic_int>(0);
		auto sibling_owner = std::make_shared<runtime_owner_probe>(sibling_count);
		auto sibling = register_alias(
			*fixture.registry, fixture.process_instance, fixture.cohort, 73U, sibling_owner);
		const auto sibling_owner_weak = std::weak_ptr<runtime_owner_probe>{sibling_owner};
		sibling_owner.reset();
		const sqlite_shm_lease_family_binding distinct_family{
			fixture.process_instance,
			fixture.cohort,
			identity("test.registry.distinct-file-family", 21U),
		};
		const auto state_destruction_before =
			sqlite_same_process_shm_registry_test_peer::state_destruction_count();
		const auto child = ::fork();
		require(child >= 0, "fork live-activity family-pin abandonment");
		if (child == 0)
		{
			fixture.family_pin.reset();
			const auto quarantined_snapshot = fixture.registry->snapshot();
			const auto quarantined_family = fixture.registry->family_snapshot(fixture.family);
			const auto sibling_exact_family =
				fixture.registry->pin_existing_family(*sibling.alias, fixture.family);
			const bool authority_removed = !fixture.alias->valid() && !fixture.activity->valid() &&
				sibling.alias->valid() && !sibling_exact_family.has_value() &&
				sibling_exact_family.error().reason ==
					sqlite_shm_lease_rejection_reason::quarantined &&
				sibling_exact_family.error().action ==
					sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				quarantined_snapshot.quarantined_alias_count == 1U &&
				quarantined_snapshot.quarantined_family_count == 1U &&
				quarantined_snapshot.active_family_pin_count == 1U &&
				quarantined_snapshot.active_activity_pin_count == 1U &&
				!quarantined_snapshot.registry_quarantined &&
				quarantined_family.exact_quarantined_match_count == 1U &&
				!quarantined_family.lookup_visible;

			auto distinct_pin =
				fixture.registry->install_or_join_family(*sibling.alias, distinct_family);
			require(distinct_pin.has_value(), "healthy sibling installs distinct family");
			sibling.family_pin.emplace(std::move(distinct_pin.value()));
			auto distinct_activity = fixture.registry->acquire_activity(*sibling.family_pin);
			require(distinct_activity.has_value(), "healthy sibling acquires distinct activity");
			require(fixture.registry->release_activity(distinct_activity.value()).has_value(),
					"healthy sibling releases distinct activity");
			unregister_alias(*fixture.registry, fixture.process_instance, fixture.cohort, sibling);
			const bool sibling_clean =
				sibling_owner_weak.expired() && sibling_count->load(std::memory_order_relaxed) == 1;

			const auto cleanup = fixture.registry->release_activity(*fixture.activity);
			const auto drained_snapshot = fixture.registry->snapshot();
			const bool cleanup_only = cleanup.has_value() && !fixture.activity->valid() &&
				drained_snapshot.active_family_pin_count == 0U &&
				drained_snapshot.active_activity_pin_count == 0U && !fixture.owner.expired() &&
				fixture.owner_destruction_count->load(std::memory_order_relaxed) == 0;
			fixture.activity.reset();
			fixture.alias.reset();
			fixture.registry.reset();
			const bool state_released =
				sqlite_same_process_shm_registry_test_peer::state_destruction_count() ==
				state_destruction_before + 1U;
			const bool owner_retained = !fixture.owner.expired() &&
				fixture.owner_destruction_count->load(std::memory_order_relaxed) == 0;
			::_exit(authority_removed && sibling_clean && cleanup_only && state_released &&
							owner_retained
						? 0
						: 1);
		}
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait live-activity family-pin abandonment");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"abandoned live family pin quarantines authority but permits exact cleanup");
		clean_fixture(fixture);
		unregister_alias(*fixture.registry, fixture.process_instance, fixture.cohort, sibling);
		require(sibling_owner_weak.expired() && sibling_count->load(std::memory_order_relaxed) == 1,
				"family-pin abandonment probe leaves sibling parent lifecycle clean");
	}

	void verify_terminal_coordinator_quarantine_stays_family_local()
	{
		auto fixture = make_fixture(24U, true);
		auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
			*fixture.registry, *fixture.activity);
		require(coordinator != nullptr, "resolve terminal-quarantine coordinator");

		auto sibling_count = std::make_shared<std::atomic_int>(0);
		auto sibling_owner = std::make_shared<runtime_owner_probe>(sibling_count);
		auto sibling = register_alias(
			*fixture.registry, fixture.process_instance, fixture.cohort, 74U, sibling_owner);
		const auto sibling_owner_weak = std::weak_ptr<runtime_owner_probe>{sibling_owner};
		sibling_owner.reset();
		const sqlite_shm_lease_family_binding distinct_family{
			fixture.process_instance,
			fixture.cohort,
			identity("test.registry.terminal-distinct-family", 24U),
		};

		const auto child = ::fork();
		require(child >= 0, "fork terminal coordinator quarantine");
		if (child == 0)
		{
			const auto connection = identity("test.registry.terminal-connection", 24U);
			const auto open_epoch = identity("test.registry.terminal-open-epoch", 24U);
			int page{};
			const auto request =
				writer_request(fixture.family, fixture.alias_lifetime, connection, open_epoch, 75U);
			auto pending = install_pending(*coordinator, request, open_epoch, mapping(&page), 75U);
			sqlite_same_process_shm_lease_test_peer::fail_next_writer_attachment_seal_transition(
				*coordinator);
			const auto terminal = coordinator->begin_writer_cleanup(pending, callback(76U));
			const auto terminal_snapshot = coordinator->snapshot();
			const bool terminal_inner = !terminal.has_value() && !pending.valid() &&
				terminal_snapshot.quarantined && terminal_snapshot.writer_cleanup_count == 1U;

			require(fixture.registry->release_activity(*fixture.activity).has_value(),
					"drain registry activity after inner terminal quarantine");
			fixture.activity.reset();
			const auto hidden_family = fixture.registry->family_snapshot(fixture.family);
			const bool family_pin_hidden = !fixture.family_pin->valid();
			auto blocked_activity = fixture.registry->acquire_activity(*fixture.family_pin);
			const bool activity_hidden = !blocked_activity.has_value() &&
				blocked_activity.error().reason == sqlite_shm_lease_rejection_reason::quarantined &&
				blocked_activity.error().action ==
					sqlite_shm_lease_recovery_action::quarantine_no_retry;
			if (blocked_activity)
				(void)fixture.registry->release_activity(blocked_activity.value());
			const auto locally_quarantined = fixture.registry->snapshot();
			const bool local_only = hidden_family.exact_active_match_count == 0U &&
				hidden_family.exact_quarantined_match_count == 1U &&
				!hidden_family.lookup_visible && family_pin_hidden &&
				!locally_quarantined.registry_quarantined &&
				locally_quarantined.quarantined_alias_count == 1U &&
				locally_quarantined.registered_alias_count == 1U &&
				locally_quarantined.quarantined_family_count == 1U;

			const auto last_release = fixture.registry->release_family(*fixture.family_pin);
			const auto released_snapshot = fixture.registry->snapshot();
			const bool terminal_consumed = !last_release.has_value() &&
				last_release.error().reason ==
					sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				last_release.error().action ==
					sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				!fixture.family_pin->valid() && released_snapshot.active_family_pin_count == 0U &&
				!released_snapshot.registry_quarantined && !fixture.owner.expired() &&
				fixture.owner_destruction_count->load(std::memory_order_relaxed) == 0;

			auto distinct_pin =
				fixture.registry->install_or_join_family(*sibling.alias, distinct_family);
			require(distinct_pin.has_value(),
					"healthy sibling installs family beside terminal quarantine");
			sibling.family_pin.emplace(std::move(distinct_pin.value()));
			auto distinct_activity = fixture.registry->acquire_activity(*sibling.family_pin);
			require(distinct_activity.has_value(),
					"healthy sibling acquires activity beside terminal quarantine");
			require(fixture.registry->release_activity(distinct_activity.value()).has_value(),
					"healthy sibling releases activity beside terminal quarantine");
			unregister_alias(*fixture.registry, fixture.process_instance, fixture.cohort, sibling);
			const auto sibling_cleanup = fixture.registry->snapshot();
			const bool sibling_healthy = sibling_owner_weak.expired() &&
				sibling_count->load(std::memory_order_relaxed) == 1 &&
				!sibling_cleanup.registry_quarantined &&
				sibling_cleanup.quarantined_alias_count == 1U &&
				sibling_cleanup.quarantined_family_count == 1U;
			::_exit(terminal_inner && activity_hidden && local_only && terminal_consumed &&
							sibling_healthy
						? 0
						: 1);
		}
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait terminal coordinator quarantine");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"inner terminal quarantine stays local and drains without revival");

		clean_fixture(fixture);
		unregister_alias(*fixture.registry, fixture.process_instance, fixture.cohort, sibling);
		require(sibling_owner_weak.expired() && sibling_count->load(std::memory_order_relaxed) == 1,
				"terminal quarantine probe leaves sibling parent lifecycle clean");
	}

	void verify_terminal_quarantine_reaches_historical_family_participant()
	{
		auto fixture = make_fixture(25U, true);
		auto* coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
			*fixture.registry, *fixture.activity);
		require(coordinator != nullptr, "resolve historical-participant coordinator");

		auto member_count = std::make_shared<std::atomic_int>(0);
		auto member_owner = std::make_shared<runtime_owner_probe>(member_count);
		auto member = register_alias(*fixture.registry,
									 fixture.process_instance,
									 fixture.cohort,
									 75U,
									 member_owner,
									 &fixture.family);
		const auto member_owner_weak = std::weak_ptr<runtime_owner_probe>{member_owner};
		member_owner.reset();

		const auto child = ::fork();
		require(child >= 0, "fork historical terminal-family participant");
		if (child == 0)
		{
			require(fixture.registry->release_activity(*fixture.activity).has_value(),
					"release historical participant activity");
			fixture.activity.reset();
			require(fixture.registry->release_family(*fixture.family_pin).has_value(),
					"release historical participant nonlast family pin");
			fixture.family_pin.reset();
			require(fixture.registry->begin_alias_unregister(*fixture.alias).has_value(),
					"begin historical participant unregister");

			auto member_activity = fixture.registry->acquire_activity(*member.family_pin);
			require(member_activity.has_value(), "acquire remaining participant terminal activity");
			const auto connection = identity("test.registry.historical-terminal-connection", 25U);
			const auto open_epoch = identity("test.registry.historical-terminal-open-epoch", 25U);
			int page{};
			const auto request =
				writer_request(fixture.family, member.alias_lifetime, connection, open_epoch, 77U);
			auto pending = install_pending(*coordinator, request, open_epoch, mapping(&page), 77U);
			sqlite_same_process_shm_lease_test_peer::fail_next_writer_attachment_seal_transition(
				*coordinator);
			const auto terminal = coordinator->begin_writer_cleanup(pending, callback(78U));
			const bool terminal_inner =
				!terminal.has_value() && !pending.valid() && coordinator->snapshot().quarantined;
			require(fixture.registry->release_activity(member_activity.value()).has_value(),
					"drain remaining participant registry activity");

			const auto hidden_family = fixture.registry->family_snapshot(fixture.family);
			const auto poll = fixture.registry->poll_alias_unregister(*fixture.alias);
			const auto unregistration =
				sqlite_same_process_shm_registry_test_peer::unregistration_receipt(
					fixture.process_instance,
					fixture.cohort,
					fixture.alias_lifetime,
					fixture.runtime_identity,
					fixture.runtime_pin_identity,
					fixture.registration_epoch,
					fixture.unregistration_epoch);
			const auto confirm =
				fixture.registry->confirm_alias_unregistered(*fixture.alias, unregistration);
			const auto localized = fixture.registry->snapshot();
			const bool historical_quarantined = !poll.has_value() &&
				poll.error().reason == sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				poll.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				!confirm.has_value() &&
				confirm.error().reason == sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				confirm.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				!fixture.alias->valid() && !fixture.owner.expired() &&
				fixture.owner_destruction_count->load(std::memory_order_relaxed) == 0 &&
				hidden_family.exact_active_match_count == 0U &&
				hidden_family.exact_quarantined_match_count == 1U &&
				!hidden_family.lookup_visible && !localized.registry_quarantined &&
				localized.quarantined_alias_count == 2U && localized.quarantined_family_count == 1U;

			const auto member_release = fixture.registry->release_family(*member.family_pin);
			const auto drained = fixture.registry->snapshot();
			const bool remaining_consumed = !member_release.has_value() &&
				member_release.error().reason ==
					sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				member_release.error().action ==
					sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				!member.family_pin->valid() && drained.active_family_pin_count == 0U &&
				drained.active_activity_pin_count == 0U && !drained.registry_quarantined &&
				!member_owner_weak.expired() && member_count->load(std::memory_order_relaxed) == 0;
			::_exit(terminal_inner && historical_quarantined && remaining_consumed ? 0 : 1);
		}
		int status{};
		require(::waitpid(child, &status, 0) == child,
				"wait historical terminal-family participant");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"terminal family quarantines active and historical participants without retry");

		clean_fixture(fixture);
		unregister_alias(*fixture.registry, fixture.process_instance, fixture.cohort, member);
		require(member_owner_weak.expired() && member_count->load(std::memory_order_relaxed) == 1,
				"historical participant probe leaves parent lifecycle clean");
	}

	void verify_all_registry_identity_counters_exhaust_fail_closed()
	{
		{
			const auto process = identity("test.registry.counter-process", 10U);
			const auto cohort = identity("test.registry.counter-cohort", 10U);
			auto registry = make_registry(process);
			auto count = std::make_shared<std::atomic_int>(0);
			auto owner = std::make_shared<runtime_owner_probe>(count);
			const auto owner_weak = std::weak_ptr<runtime_owner_probe>{owner};
			const auto runtime = identity("test.registry.counter-runtime", 10U);
			const auto runtime_pin = identity("test.registry.counter-runtime-pin", 10U);
			auto adopted = sqlite_same_process_shm_registry_test_peer::adopt_runtime_lifetime(
				*registry, runtime, runtime_pin, owner);
			require(adopted.has_value(), "adopt alias-counter runtime owner");
			owner.reset();
			auto binding = sqlite_same_process_shm_registry_test_peer::alias_binding(
				process,
				cohort,
				identity("test.registry.counter-alias", 10U),
				std::move(adopted.value()));
			sqlite_same_process_shm_registry_test_peer::exhaust_counter(
				*registry, detail::sqlite_shm_registry_counter_for_testing::alias_token);
			const auto rejected = registry->reserve_alias(std::move(binding));
			auto revival_count = std::make_shared<std::atomic_int>(0);
			auto revival_owner = std::make_shared<runtime_owner_probe>(revival_count);
			const auto revival_owner_weak = std::weak_ptr<runtime_owner_probe>{revival_owner};
			const auto revival = sqlite_same_process_shm_registry_test_peer::adopt_runtime_lifetime(
				*registry,
				identity("test.registry.counter-revival-runtime", 10U),
				identity("test.registry.counter-revival-pin", 10U),
				revival_owner);
			revival_owner.reset();
			const auto snapshot = registry->snapshot();
			require(
				!rejected.has_value() &&
					rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::generation_exhausted &&
					rejected.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					!revival.has_value() &&
					revival.error().reason == sqlite_shm_lease_rejection_reason::quarantined &&
					revival.error().action ==
						sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					snapshot.registry_quarantined && snapshot.alias_record_count == 0U &&
					snapshot.family_record_count == 0U && snapshot.active_family_pin_count == 0U &&
					snapshot.active_activity_pin_count == 0U && owner_weak.expired() &&
					count->load(std::memory_order_relaxed) == 1 && revival_owner_weak.expired() &&
					revival_count->load(std::memory_order_relaxed) == 1,
				"alias token exhaustion quarantines without retaining a pre-effect owner");
		}

		const auto process = identity("test.registry.counter-process", 11U);
		const auto cohort = identity("test.registry.counter-cohort", 11U);
		const sqlite_shm_lease_family_binding family{
			process,
			cohort,
			identity("test.registry.counter-family", 11U),
		};
		const sqlite_shm_lease_family_binding fresh_family{
			process,
			cohort,
			identity("test.registry.counter-fresh-family", 11U),
		};
		auto registry = make_registry(process);
		auto first_count = std::make_shared<std::atomic_int>(0);
		auto first_owner = std::make_shared<runtime_owner_probe>(first_count);
		auto first = register_alias(*registry, process, cohort, 31U, first_owner);
		first_owner.reset();

		const auto family_epoch_state_before =
			sqlite_same_process_shm_registry_test_peer::state_destruction_count();
		const auto family_epoch_child = ::fork();
		require(family_epoch_child >= 0, "fork family-epoch exhaustion");
		if (family_epoch_child == 0)
		{
			sqlite_same_process_shm_registry_test_peer::exhaust_counter(
				*registry, detail::sqlite_shm_registry_counter_for_testing::family_epoch);
			const auto rejected = registry->install_or_join_family(*first.alias, family);
			const auto revival = registry->install_or_join_family(*first.alias, family);
			const auto snapshot = registry->snapshot();
			first.alias.reset();
			registry.reset();
			const bool state_released =
				sqlite_same_process_shm_registry_test_peer::state_destruction_count() ==
				family_epoch_state_before + 1U;
			const bool owner_retained =
				!first.owner.expired() && first_count->load(std::memory_order_relaxed) == 0;
			const bool closed = !rejected.has_value() &&
				rejected.error().reason ==
					sqlite_shm_lease_rejection_reason::generation_exhausted &&
				rejected.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				!revival.has_value() &&
				revival.error().reason == sqlite_shm_lease_rejection_reason::quarantined &&
				revival.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				snapshot.registry_quarantined && snapshot.alias_record_count == 1U &&
				snapshot.quarantined_alias_count == 1U && snapshot.family_record_count == 0U &&
				snapshot.active_family_pin_count == 0U && snapshot.active_activity_pin_count == 0U;
			::_exit(closed && state_released && owner_retained ? 0 : 1);
		}
		int status{};
		require(::waitpid(family_epoch_child, &status, 0) == family_epoch_child,
				"wait family-epoch exhaustion");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"family entry epoch exhaustion fails closed");

		auto pinned = registry->install_or_join_family(*first.alias, family);
		require(pinned.has_value(), "install family before pin-counter exhaustion");
		first.family_pin.emplace(std::move(pinned.value()));
		auto second_count = std::make_shared<std::atomic_int>(0);
		auto second_owner = std::make_shared<runtime_owner_probe>(second_count);
		auto second = register_alias(*registry, process, cohort, 32U, second_owner);
		second_owner.reset();

		const auto family_pin_baseline = registry->snapshot();
		const auto family_pin_state_before =
			sqlite_same_process_shm_registry_test_peer::state_destruction_count();
		const auto family_pin_child = ::fork();
		require(family_pin_child >= 0, "fork family-pin exhaustion");
		if (family_pin_child == 0)
		{
			sqlite_same_process_shm_registry_test_peer::exhaust_counter(
				*registry, detail::sqlite_shm_registry_counter_for_testing::family_pin_token);
			const auto rejected = registry->install_or_join_family(*second.alias, fresh_family);
			const auto revival = registry->install_or_join_family(*second.alias, fresh_family);
			const auto snapshot = registry->snapshot();
			const auto partial_family = registry->family_snapshot(fresh_family);
			first.family_pin.reset();
			second.alias.reset();
			first.alias.reset();
			registry.reset();
			const bool state_released =
				sqlite_same_process_shm_registry_test_peer::state_destruction_count() ==
				family_pin_state_before + 1U;
			const bool owners_retained = !first.owner.expired() && !second.owner.expired() &&
				first_count->load(std::memory_order_relaxed) == 0 &&
				second_count->load(std::memory_order_relaxed) == 0;
			const bool closed = !rejected.has_value() &&
				rejected.error().reason ==
					sqlite_shm_lease_rejection_reason::generation_exhausted &&
				rejected.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				!revival.has_value() &&
				revival.error().reason == sqlite_shm_lease_rejection_reason::quarantined &&
				revival.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				snapshot.registry_quarantined &&
				snapshot.alias_record_count == family_pin_baseline.alias_record_count &&
				snapshot.family_record_count == family_pin_baseline.family_record_count + 1U &&
				snapshot.active_family_count == 0U && snapshot.quarantined_family_count == 2U &&
				snapshot.active_family_pin_count == family_pin_baseline.active_family_pin_count &&
				snapshot.active_activity_pin_count == 0U &&
				partial_family.exact_quarantined_match_count == 1U &&
				partial_family.exact_active_match_count == 0U && !partial_family.lookup_visible;
			::_exit(closed && state_released && owners_retained ? 0 : 1);
		}
		status = 0;
		require(::waitpid(family_pin_child, &status, 0) == family_pin_child,
				"wait family-pin exhaustion");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"family pin token exhaustion fails closed");

		const auto activity_baseline = registry->snapshot();
		const auto activity_state_before =
			sqlite_same_process_shm_registry_test_peer::state_destruction_count();
		const auto activity_child = ::fork();
		require(activity_child >= 0, "fork activity exhaustion");
		if (activity_child == 0)
		{
			sqlite_same_process_shm_registry_test_peer::exhaust_counter(
				*registry, detail::sqlite_shm_registry_counter_for_testing::activity_token);
			const auto rejected = registry->acquire_activity(*first.family_pin);
			const auto revival = registry->acquire_activity(*first.family_pin);
			const auto snapshot = registry->snapshot();
			first.family_pin.reset();
			second.alias.reset();
			first.alias.reset();
			registry.reset();
			const bool state_released =
				sqlite_same_process_shm_registry_test_peer::state_destruction_count() ==
				activity_state_before + 1U;
			const bool owners_retained = !first.owner.expired() && !second.owner.expired() &&
				first_count->load(std::memory_order_relaxed) == 0 &&
				second_count->load(std::memory_order_relaxed) == 0;
			const bool closed = !rejected.has_value() &&
				rejected.error().reason ==
					sqlite_shm_lease_rejection_reason::generation_exhausted &&
				rejected.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				!revival.has_value() &&
				revival.error().reason == sqlite_shm_lease_rejection_reason::quarantined &&
				revival.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				snapshot.registry_quarantined &&
				snapshot.alias_record_count == activity_baseline.alias_record_count &&
				snapshot.family_record_count == activity_baseline.family_record_count &&
				snapshot.active_family_count == 0U && snapshot.quarantined_family_count == 1U &&
				snapshot.active_family_pin_count == activity_baseline.active_family_pin_count &&
				snapshot.active_activity_pin_count == 0U;
			::_exit(closed && state_released && owners_retained ? 0 : 1);
		}
		status = 0;
		require(::waitpid(activity_child, &status, 0) == activity_child,
				"wait activity exhaustion");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"activity token exhaustion fails closed");

		unregister_alias(*registry, process, cohort, second);
		unregister_alias(*registry, process, cohort, first);
		require(first.owner.expired() && second.owner.expired() &&
					first_count->load(std::memory_order_relaxed) == 1 &&
					second_count->load(std::memory_order_relaxed) == 1,
				"counter probes leave parent lifecycle clean");
	}

	void verify_registry_generation_source_spans_fresh_families_and_exhausts()
	{
		auto fixture = make_fixture(15U, true);
		auto* first_coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
			*fixture.registry, *fixture.activity);
		require(first_coordinator != nullptr, "resolve first generation coordinator");
		const auto first_connection = identity("test.registry.connection", 60U);
		const auto first_open_epoch = identity("test.registry.open-epoch", 60U);
		auto first_gate = install_eligibility(
			*first_coordinator, fixture.family, first_connection, first_open_epoch, 60U);
		int first_page{};
		const auto first_request = writer_request(
			fixture.family, fixture.alias_lifetime, first_connection, first_open_epoch, 60U);
		auto first_pending = install_pending(
			*first_coordinator, first_request, first_open_epoch, mapping(&first_page), 60U);
		auto first_holder = promote_writer(*first_coordinator, first_pending, first_gate);
		require(first_holder.generation() == 1U,
				"first registry family mints first process generation");
		retire_writer(*first_coordinator, first_holder, 160U);
		require(first_coordinator->revoke_writer_eligibility(first_gate).has_value(),
				"revoke first generation eligibility");
		require(fixture.registry->release_activity(*fixture.activity).has_value(),
				"release first generation activity");
		fixture.activity.reset();
		require(fixture.registry->release_family(*fixture.family_pin).has_value(),
				"retire first generation family");
		fixture.family_pin.reset();

		auto fresh_family =
			fixture.registry->install_or_join_family(*fixture.alias, fixture.family);
		require(fresh_family.has_value(), "install successor generation family");
		fixture.family_pin.emplace(std::move(fresh_family.value()));
		auto fresh_activity = fixture.registry->acquire_activity(*fixture.family_pin);
		require(fresh_activity.has_value(), "acquire successor generation activity");
		fixture.activity.emplace(std::move(fresh_activity.value()));
		auto* second_coordinator = sqlite_same_process_shm_registry_test_peer::coordinator(
			*fixture.registry, *fixture.activity);
		require(second_coordinator != nullptr, "fresh family exposes its exact coordinator");
		const auto second_connection = identity("test.registry.connection", 61U);
		const auto second_open_epoch = identity("test.registry.open-epoch", 61U);
		auto second_gate = install_eligibility(
			*second_coordinator, fixture.family, second_connection, second_open_epoch, 61U);
		int second_page{};
		const auto second_request = writer_request(
			fixture.family, fixture.alias_lifetime, second_connection, second_open_epoch, 61U);
		auto second_pending = install_pending(
			*second_coordinator, second_request, second_open_epoch, mapping(&second_page), 61U);
		auto second_holder = promote_writer(*second_coordinator, second_pending, second_gate);
		require(second_holder.generation() == 2U,
				"fresh family coordinator shares monotonic process generation source");
		retire_writer(*second_coordinator, second_holder, 161U);
		require(second_coordinator->revoke_writer_eligibility(second_gate).has_value(),
				"revoke successor generation eligibility");
		clean_fixture(fixture);

		const auto process = identity("test.registry.exhausted-process", 16U);
		const auto cohort = identity("test.registry.exhausted-cohort", 16U);
		const sqlite_shm_lease_family_binding family{
			process,
			cohort,
			identity("test.registry.exhausted-family", 16U),
		};
		auto owner_receipt = sqlite_same_process_shm_registry_test_peer::process_owner(process);
		auto created = sqlite_same_process_shm_registry_test_peer::create_with_generation(
			std::move(owner_receipt), std::numeric_limits<std::uint64_t>::max());
		require(created.has_value(), "create max-generation registry");
		auto registry = std::move(created.value());
		auto owner_count = std::make_shared<std::atomic_int>(0);
		auto runtime_owner = std::make_shared<runtime_owner_probe>(owner_count);
		auto member = register_alias(*registry, process, cohort, 62U, runtime_owner, &family);
		const auto runtime_owner_weak = std::weak_ptr<runtime_owner_probe>{runtime_owner};
		runtime_owner.reset();
		auto activity = registry->acquire_activity(*member.family_pin);
		require(activity.has_value(), "acquire max-generation activity");
		auto* coordinator =
			sqlite_same_process_shm_registry_test_peer::coordinator(*registry, activity.value());
		require(coordinator != nullptr, "resolve max-generation coordinator");
		const auto connection = identity("test.registry.connection", 62U);
		const auto open_epoch = identity("test.registry.open-epoch", 62U);
		auto gate = install_eligibility(*coordinator, family, connection, open_epoch, 62U);
		int page{};
		const auto request =
			writer_request(family, member.alias_lifetime, connection, open_epoch, 62U);
		auto last_pending = install_pending(*coordinator, request, open_epoch, mapping(&page), 62U);
		auto last_holder = promote_writer(*coordinator, last_pending, gate);
		require(last_holder.generation() == std::numeric_limits<std::uint64_t>::max(),
				"last process generation is valid exactly once");
		retire_writer(*coordinator, last_holder, 162U);

		const auto exhausted_request =
			writer_request(family, member.alias_lifetime, connection, open_epoch, 63U);
		auto exhausted_pending =
			install_pending(*coordinator, exhausted_request, open_epoch, mapping(&page), 63U);
		const auto exhausted = coordinator->promote_writer(exhausted_pending, gate);
		require(!exhausted.has_value() &&
					exhausted.error().reason ==
						sqlite_shm_lease_rejection_reason::generation_exhausted,
				"registry generation source never wraps");
		cleanup_pending(*coordinator, exhausted_pending, 163U);
		require(coordinator->revoke_writer_eligibility(gate).has_value(),
				"revoke max-generation eligibility");
		require(registry->release_activity(activity.value()).has_value(),
				"release max-generation activity");
		unregister_alias(*registry, process, cohort, member);
		require(runtime_owner_weak.expired() && owner_count->load(std::memory_order_relaxed) == 1,
				"max-generation lifecycle releases runtime owner");
	}

	void verify_stale_unreserved_runtime_pin_retains_owner()
	{
		const auto process = identity("test.registry.stale-pin-process", 23U);
		auto registry = make_registry(process);
		auto destruction_count = std::make_shared<std::atomic_int>(0);
		auto runtime_owner = std::make_shared<runtime_owner_probe>(destruction_count);
		const auto runtime_owner_weak = std::weak_ptr<runtime_owner_probe>{runtime_owner};
		auto adopted = sqlite_same_process_shm_registry_test_peer::adopt_runtime_lifetime(
			*registry,
			identity("test.registry.stale-pin-runtime", 23U),
			identity("test.registry.stale-pin-identity", 23U),
			runtime_owner);
		require(adopted.has_value(), "adopt unreserved stale-owner probe");
		runtime_owner.reset();
		std::optional<sqlite_shm_registry_runtime_lifetime_pin> runtime_pin;
		runtime_pin.emplace(std::move(adopted.value()));

		const auto child = ::fork();
		require(child >= 0, "fork unreserved stale-owner probe");
		if (child == 0)
		{
			sqlite_same_process_shm_registry_test_peer::invalidate_process_instance(*registry);
			runtime_pin.reset();
			registry.reset();
			::_exit(!runtime_owner_weak.expired() &&
							destruction_count->load(std::memory_order_relaxed) == 0
						? 0
						: 1);
		}
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait unreserved stale-owner probe");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"stale unreserved runtime pin never invokes inherited owner destruction");

		runtime_pin.reset();
		require(runtime_owner_weak.expired() &&
					destruction_count->load(std::memory_order_relaxed) == 1,
				"live parent releases unreserved runtime owner exactly once");
	}

	void verify_fork_stale_destructors_never_touch_inherited_mutex()
	{
		auto fixture = make_fixture(4U, true);
		auto sealed = fixture.activity->seal_for_audit();
		require(sealed.has_value() && sealed->valid(), "mint fork-stale activity seal");
		const auto state_destruction_before =
			sqlite_same_process_shm_registry_test_peer::state_destruction_count();
		sqlite_same_process_shm_registry_test_peer::lock_registry_mutex(*fixture.registry);
		const auto child = ::fork();
		if (child < 0)
		{
			sqlite_same_process_shm_registry_test_peer::unlock_registry_mutex(*fixture.registry);
			require(false, "fork stale-destructor counterexample");
		}
		if (child == 0)
		{
			::alarm(5U);
			sqlite_same_process_shm_registry_test_peer::invalidate_process_instance(
				*fixture.registry);
			const auto stale_snapshot = fixture.registry->snapshot();
			const auto stale_release = fixture.registry->release_activity(*fixture.activity);
			const bool stale_matcher =
				!sqlite_same_process_shm_registry_test_peer::activity_seal_matches(
					*fixture.registry,
					sealed.value(),
					fixture.process_instance,
					fixture.family,
					fixture.alias_lifetime);
			const bool stale_before_destruction = !fixture.alias->valid() &&
				!fixture.family_pin->valid() && !fixture.activity->valid() && !sealed->valid() &&
				!stale_snapshot.process_live && !stale_release.has_value() &&
				stale_release.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
				stale_matcher &&
				sqlite_same_process_shm_registry_test_peer::generation_source_identity(
					*fixture.registry) == nullptr;
			fixture.activity.reset();
			fixture.family_pin.reset();
			fixture.alias.reset();
			fixture.registry.reset();
			const bool retained = !fixture.owner.expired() &&
				fixture.owner_destruction_count->load(std::memory_order_relaxed) == 0 &&
				sqlite_same_process_shm_registry_test_peer::state_destruction_count() ==
					state_destruction_before;
			::_exit(stale_before_destruction && retained ? 0 : 2);
		}
		sqlite_same_process_shm_registry_test_peer::unlock_registry_mutex(*fixture.registry);
		int status{};
		require(::waitpid(child, &status, 0) == child, "wait stale-destructor counterexample");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"stale child destructors avoid inherited mutex and native owner destruction");
		clean_fixture(fixture);
	}
} // namespace

int main()
{
	try
	{
		verify_registry_writer_member_is_exact_and_cleanup_only();
		verify_exact_attachment_cleanup_drains_all_bound_members();
		verify_shallower_anchor_cannot_bypass_nested_writer_callback();
		verify_registry_writer_begin_rejects_used_or_mismatched_epoch();
		verify_registry_writer_attachment_origin_never_mixes();
		verify_registry_writer_liveness_loss_blocks_admission_but_keeps_cleanup();
		verify_registry_writer_no_map_liveness_boundary_is_sticky();
		verify_confirmed_cleanup_terminalizes_liveness_loss();
		verify_registry_writer_final_publish_liveness_losers_roll_back();
		verify_activity_seal_is_one_shot_weak_and_exactly_bound();
		verify_activity_owner_destruction_never_takes_registry_mutex();
		verify_activity_abandonment_races_registry_visibility();
		verify_quarantined_activity_owner_remains_exactly_releasable();
		verify_activity_owner_and_seal_do_not_retain_registry_state();
		verify_process_owner_is_one_shot();
		verify_registration_arm_and_exact_receipt_binding();
		verify_reserved_alias_teardown_releases_runtime_owner_once();
		verify_registered_alias_abandonment_retains_owner_through_state_teardown();
		verify_shared_runtime_owner_and_identity_tombstones();
		verify_registration_epoch_replay_and_armed_abandonment();
		verify_receipt_epochs_are_direction_and_lifecycle_unique();
		verify_registered_family_happy_path();
		verify_duplicate_family_blocks_existing_pin_admission();
		verify_shared_family_unregister_waits_for_exact_coordinator_quiescence();
		verify_nonquiescent_last_family_release_is_retryable();
		verify_nonquiescent_last_family_abandonment_reaches_historical_participant();
		verify_unregister_hides_admission_then_drains_existing_activity();
		verify_foreign_release_never_consumes_another_registry_pin();
		verify_activity_abandonment_quarantines_local_authority();
		verify_family_pin_abandonment_with_live_activity_is_cleanup_only();
		verify_terminal_coordinator_quarantine_stays_family_local();
		verify_terminal_quarantine_reaches_historical_family_participant();
		verify_all_registry_identity_counters_exhaust_fail_closed();
		verify_registry_generation_source_spans_fresh_families_and_exhausts();
		verify_stale_unreserved_runtime_pin_retains_owner();
		verify_fork_stale_destructors_never_touch_inherited_mutex();
	}
	catch (const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
	return 0;
}
