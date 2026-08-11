#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "sdk/sqlite_writer_shm_mapping_epoch_internal.hpp"

namespace cxxlens::sdk
{
	class sqlite_writer_shm_mapping_epoch_test_peer
	{
	  public:
		[[nodiscard]] static std::pair<sqlite_writer_shm_native_lifetime_revoker,
									   sqlite_writer_shm_native_lifetime_source>
		native_lifetime_source(sqlite_writer_shm_native_lifetime_role role,
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
	};
} // namespace cxxlens::sdk

namespace
{
	using namespace cxxlens::sdk;

	static_assert(!std::is_default_constructible_v<sqlite_writer_shm_native_lifetime_pin>);
	static_assert(!std::is_copy_constructible_v<sqlite_writer_shm_native_lifetime_pin>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_writer_shm_native_lifetime_pin>);
	static_assert(std::is_nothrow_destructible_v<sqlite_writer_shm_native_lifetime_pin>);
	static_assert(!std::is_copy_constructible_v<sqlite_writer_shm_native_lifetime_revoker>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_writer_shm_native_lifetime_revoker>);
	static_assert(!std::is_copy_constructible_v<sqlite_writer_shm_native_lifetime_source>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_writer_shm_native_lifetime_source>);
	static_assert(!std::is_copy_constructible_v<sqlite_writer_shm_mapping_epoch_arm>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_writer_shm_mapping_epoch_arm>);
	static_assert(!std::is_copy_constructible_v<sqlite_writer_shm_mapping_epoch_observer>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_writer_shm_mapping_epoch_observer>);
	static_assert(!std::is_copy_constructible_v<sqlite_writer_shm_mapping_epoch_activation>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_writer_shm_mapping_epoch_activation>);
	static_assert(std::is_copy_constructible_v<sqlite_writer_shm_mapping_epoch_receipt>);
	static_assert(!std::is_default_constructible_v<sqlite_writer_shm_mapping_epoch_receipt>);
	static_assert(noexcept(seal_sqlite_writer_shm_mapping_epoch(
		std::declval<sqlite_writer_shm_mapping_epoch_observer&>(), nullptr)));

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

	[[nodiscard]] sqlite_shm_lease_family_binding family(const std::uint8_t marker)
	{
		return {identity("test.epoch.process", marker),
				identity("test.epoch.runtime-vfs", marker),
				identity("test.epoch.file-family", marker)};
	}

	[[nodiscard]] sqlite_shm_callback_execution_receipt callback(const std::uint8_t marker)
	{
		return {
			identity("test.epoch.thread", marker), 0U, identity("test.epoch.invocation", marker)};
	}

	struct owner_probe
	{
		explicit owner_probe(std::shared_ptr<std::atomic_int> destruction_count) noexcept
			: destruction_count_{std::move(destruction_count)}
		{
		}

		~owner_probe()
		{
			destruction_count_->fetch_add(1, std::memory_order_relaxed);
		}

		std::shared_ptr<std::atomic_int> destruction_count_;
	};

	struct native_lifetime_fixture
	{
		sqlite_writer_shm_native_lifetime_revoker revoker;
		sqlite_writer_shm_native_lifetime_source source;
		std::shared_ptr<owner_probe> retained_owner;
		std::weak_ptr<owner_probe> owner;
		std::shared_ptr<std::atomic_int> destruction_count;
	};

	[[nodiscard]] native_lifetime_fixture
	make_native_lifetime(const sqlite_writer_shm_native_lifetime_role role,
						 const sqlite_backend_opaque_identity& semantic_receipt,
						 std::optional<sqlite_backend_opaque_identity> native_xopen_receipt,
						 const std::uint8_t marker)
	{
		auto destruction_count = std::make_shared<std::atomic_int>();
		auto owner = std::make_shared<owner_probe>(destruction_count);
		auto weak_owner = std::weak_ptr<owner_probe>{owner};
		auto lifetime = sqlite_writer_shm_mapping_epoch_test_peer::native_lifetime_source(
			role,
			identity("test.epoch.native-lifetime", marker),
			semantic_receipt,
			std::move(native_xopen_receipt),
			owner);
		return {std::move(lifetime.first),
				std::move(lifetime.second),
				std::move(owner),
				std::move(weak_owner),
				std::move(destruction_count)};
	}

	[[nodiscard]] sqlite_writer_shm_native_lifetime_pin mint_pin(native_lifetime_fixture& lifetime,
																 const std::string_view message)
	{
		auto minted = lifetime.source.mint_pin();
		require(minted.has_value(), message);
		return std::move(*minted);
	}

	void verify_production_lifetime_factory_is_complete_and_revocable()
	{
		auto destruction_count = std::make_shared<std::atomic_int>();
		auto owner = std::make_shared<owner_probe>(destruction_count);
		auto invalid = sqlite_writer_shm_native_lifetime_production_factory::create_source(
			sqlite_writer_shm_native_lifetime_role::main_database,
			identity("test.epoch.production-lifetime", 1U),
			identity("test.epoch.production-semantic", 1U),
			std::nullopt,
			owner);
		require(!invalid, "production lifetime factory accepted a missing xOpen receipt");
		auto duplicate_semantic =
			sqlite_writer_shm_native_lifetime_production_factory::create_source(
				sqlite_writer_shm_native_lifetime_role::main_database,
				identity("test.epoch.production-duplicate", 4U),
				identity("test.epoch.production-duplicate", 4U),
				identity("test.epoch.production-xopen", 4U),
				owner);
		require(!duplicate_semantic,
				"production lifetime factory accepted a duplicate lifetime/semantic identity");
		auto duplicate_lifetime_xopen =
			sqlite_writer_shm_native_lifetime_production_factory::create_source(
				sqlite_writer_shm_native_lifetime_role::main_database,
				identity("test.epoch.production-duplicate-lifetime", 6U),
				identity("test.epoch.production-semantic", 6U),
				identity("test.epoch.production-duplicate-lifetime", 6U),
				owner);
		require(!duplicate_lifetime_xopen,
				"production lifetime factory accepted a duplicate lifetime/xOpen identity");
		auto duplicate_xopen = sqlite_writer_shm_native_lifetime_production_factory::create_source(
			sqlite_writer_shm_native_lifetime_role::main_database,
			identity("test.epoch.production-lifetime", 5U),
			identity("test.epoch.production-semantic", 5U),
			identity("test.epoch.production-semantic", 5U),
			owner);
		require(
			!duplicate_xopen,
			"production lifetime factory accepted an xOpen identity reused as semantic receipt");
		auto unknown_role = sqlite_writer_shm_native_lifetime_production_factory::create_source(
			static_cast<sqlite_writer_shm_native_lifetime_role>(0xffU),
			identity("test.epoch.production-lifetime", 3U),
			identity("test.epoch.production-semantic", 3U),
			std::nullopt,
			owner);
		require(!unknown_role, "production lifetime factory accepted an unknown role");

		auto produced = sqlite_writer_shm_native_lifetime_production_factory::create_source(
			sqlite_writer_shm_native_lifetime_role::main_database,
			identity("test.epoch.production-lifetime", 2U),
			identity("test.epoch.production-semantic", 2U),
			identity("test.epoch.production-xopen", 2U),
			owner);
		require(produced.has_value(), "production lifetime factory rejected a complete receipt");
		native_lifetime_fixture fixture{std::move(produced->first),
										std::move(produced->second),
										owner,
										owner,
										destruction_count};
		owner.reset();
		{
			auto pin = mint_pin(fixture, "production lifetime source did not mint a pin");
			fixture.retained_owner.reset();
			// The pin owns the node independently; close revocation only removes authority.
			require(pin.valid() && !fixture.owner.expired(),
					"production lifetime pin was not retained before revocation");
			require(fixture.revoker.revoke(), "production lifetime revocation was not one-shot");
			require(!fixture.source.valid() && !pin.valid(),
					"production lifetime revocation did not invalidate all authority");
		}
		require(fixture.owner.expired() && destruction_count->load(std::memory_order_relaxed) == 1,
				"production lifetime pin retained its owner after terminal destruction");
	}

	struct epoch_resources
	{
		sqlite_writer_shm_mapping_epoch_binding binding;
		native_lifetime_fixture parent;
		native_lifetime_fixture main;
		native_lifetime_fixture wal;
		native_lifetime_fixture shm;

		[[nodiscard]] sqlite_writer_shm_mapping_epoch_request take_request()
		{
			auto parent_pin = mint_pin(parent, "mint retained-parent epoch pin");
			auto main_pin = mint_pin(main, "mint main native-file epoch pin");
			auto wal_pin = mint_pin(wal, "mint WAL native-file epoch pin");
			auto shm_pin = mint_pin(shm, "mint SHM native-attachment epoch pin");
			parent.retained_owner.reset();
			main.retained_owner.reset();
			wal.retained_owner.reset();
			shm.retained_owner.reset();
			return {binding,
					std::move(parent_pin),
					std::move(main_pin),
					std::move(wal_pin),
					std::move(shm_pin)};
		}

		[[nodiscard]] bool owners_alive() const
		{
			return !parent.owner.expired() && !main.owner.expired() && !wal.owner.expired() &&
				!shm.owner.expired();
		}

		[[nodiscard]] bool owners_expired() const
		{
			return parent.owner.expired() && main.owner.expired() && wal.owner.expired() &&
				shm.owner.expired();
		}
	};

	[[nodiscard]] epoch_resources make_epoch_resources(const std::uint8_t marker,
													   const int caller_extend = 0,
													   const int delegated_extend = 0)
	{
		const auto binding_family = family(marker);
		const auto alias_lifetime = identity("test.epoch.alias", marker);
		const auto connection = identity("test.epoch.connection", marker);
		const auto main_receipt = identity("test.epoch.main-native-file", marker);
		const auto main_xopen = identity("test.epoch.main-xopen", marker);
		const auto open_epoch = identity("test.epoch.open", marker);
		auto attachment = sqlite_shm_native_attachment_identity::bind(
			binding_family,
			alias_lifetime,
			connection,
			main_receipt,
			main_xopen,
			open_epoch,
			identity("test.epoch.callback-cohort", marker),
			identity("test.epoch.attachment-epoch", marker));
		require(attachment.has_value(), "bind native attachment for mapping epoch");

		const auto parent_receipt = identity("test.epoch.retained-parent", marker);
		const auto wal_receipt = identity("test.epoch.wal-native-file", marker);
		const auto shm_receipt = identity("test.epoch.shm-native-attachment", marker);
		sqlite_writer_shm_mapping_epoch_binding epoch_binding{
			sqlite_shm_writer_map_request{binding_family,
										  alias_lifetime,
										  connection,
										  std::move(*attachment),
										  callback(marker),
										  0,
										  4096,
										  caller_extend},
			delegated_extend,
			identity("test.epoch.shm-leaf", marker),
			parent_receipt,
			wal_receipt,
			identity("test.epoch.wal-xopen", marker),
			shm_receipt};

		return {
			epoch_binding,
			make_native_lifetime(sqlite_writer_shm_native_lifetime_role::retained_parent,
								 parent_receipt,
								 std::nullopt,
								 static_cast<std::uint8_t>(marker + 1U)),
			make_native_lifetime(sqlite_writer_shm_native_lifetime_role::main_database,
								 main_receipt,
								 main_xopen,
								 static_cast<std::uint8_t>(marker + 2U)),
			make_native_lifetime(sqlite_writer_shm_native_lifetime_role::write_ahead_log,
								 wal_receipt,
								 epoch_binding.wal_xopen_receipt,
								 static_cast<std::uint8_t>(marker + 3U)),
			make_native_lifetime(sqlite_writer_shm_native_lifetime_role::shared_memory_attachment,
								 shm_receipt,
								 std::nullopt,
								 static_cast<std::uint8_t>(marker + 4U))};
	}

	[[nodiscard]] sqlite_writer_shm_stat_census direct_stat(const std::uint8_t marker,
															const std::uint64_t byte_count = 4096U)
	{
		return {sqlite_writer_shm_entry_state::direct_regular,
				identity("test.epoch.parent-namespace", marker),
				identity("test.epoch.filesystem", marker),
				identity("test.epoch.mount", marker),
				identity("test.epoch.shm-object", marker),
				identity("test.epoch.shm-entry", marker),
				byte_count};
	}

	struct retained_epoch_state
	{
		bool stable{true};
		std::size_t target_rechecks{};
		std::size_t retained_entry_calls{};
		std::size_t held_rechecks{};
		std::size_t size_calls{};
		std::size_t duplicate_namespace_opens{};
		std::uint64_t shm_size{4096U};
		bool shm_absent{};
	};

	class retained_epoch_held_object final : public sqlite_backend_held_object
	{
	  public:
		retained_epoch_held_object(retained_epoch_state& state, const std::uint8_t marker)
			: state_{state}, object_{identity("test.retained-epoch.shm-object", marker)},
			  entry_{identity("test.retained-epoch.shm-entry", marker)},
			  filesystem_{identity("test.retained-epoch.filesystem", marker)},
			  mount_{identity("test.retained-epoch.mount", marker)}
		{
		}

		[[nodiscard]] sqlite_backend_file_role role() const noexcept override
		{
			return sqlite_backend_file_role::shared_memory;
		}
		[[nodiscard]] const sqlite_backend_opaque_identity&
		object_identity() const noexcept override
		{
			return object_;
		}
		[[nodiscard]] const sqlite_backend_opaque_identity&
		directory_entry_identity() const noexcept override
		{
			return entry_;
		}
		[[nodiscard]] const std::optional<sqlite_backend_opaque_identity>&
		object_filesystem_profile() const noexcept override
		{
			return filesystem_;
		}
		[[nodiscard]] const std::optional<sqlite_backend_opaque_identity>&
		object_mount_identity() const noexcept override
		{
			return mount_;
		}
		[[nodiscard]] result<void> recheck_retained_object() const override
		{
			++state_.held_rechecks;
			return state_.stable
				? result<void>{}
				: result<void>{error{"test.retained-epoch", "shm", "stale-retained-object"}};
		}
		[[nodiscard]] result<std::uint64_t> size() const override
		{
			++state_.size_calls;
			return state_.stable
				? result<std::uint64_t>{state_.shm_size}
				: result<std::uint64_t>{error{"test.retained-epoch", "shm", "stale-size"}};
		}
		[[nodiscard]] result<void> read_exact(std::uint64_t, std::span<std::byte>) const override
		{
			return error{"test.retained-epoch", "shm", "unexpected-read"};
		}
		[[nodiscard]] result<std::string> sha256() const override
		{
			return error{"test.retained-epoch", "shm", "unexpected-hash"};
		}
		[[nodiscard]] result<std::shared_ptr<sqlite_backend_private_snapshot>>
		copy_exact(sqlite_backend_private_snapshot_builder&, std::span<std::byte>) const override
		{
			return error{"test.retained-epoch", "shm", "unexpected-copy"};
		}
		[[nodiscard]] result<sqlite_backend_replacement_state>
		recheck_current_entry() const override
		{
			return state_.stable
				? result<sqlite_backend_replacement_state>{sqlite_backend_replacement_state::
															   exact_same_entry_and_object}
				: result<sqlite_backend_replacement_state>{
					  error{"test.retained-epoch", "shm", "stale-entry"}};
		}

	  private:
		retained_epoch_state& state_;
		sqlite_backend_opaque_identity object_;
		sqlite_backend_opaque_identity entry_;
		std::optional<sqlite_backend_opaque_identity> filesystem_;
		std::optional<sqlite_backend_opaque_identity> mount_;
	};

	class retained_epoch_target final : public sqlite_source_shm_target_namespace_epoch
	{
	  public:
		retained_epoch_target(retained_epoch_state& state, const std::uint8_t marker)
			: state_{state}, held_{std::make_shared<retained_epoch_held_object>(state, marker)},
			  parent_namespace_identity_{::identity("test.retained-epoch.parent", marker)},
			  identity_{::identity("test.retained-epoch.target", marker)}
		{
			entry_ = {sqlite_backend_file_role::shared_memory,
					  sqlite_backend_entry_state::held_regular,
					  held_->object_identity(),
					  held_->directory_entry_identity(),
					  held_,
					  held_->object_filesystem_profile(),
					  true};
		}

		[[nodiscard]] std::string_view logical_main_locator() const noexcept override
		{
			return "test.retained-epoch.logical";
		}
		[[nodiscard]] std::string_view anchored_main_locator() const noexcept override
		{
			return "test.retained-epoch.anchored";
		}
		[[nodiscard]] const sqlite_backend_opaque_identity& identity() const noexcept override
		{
			return identity_;
		}
		[[nodiscard]] const sqlite_backend_opaque_identity&
		parent_namespace_identity() const noexcept override
		{
			return parent_namespace_identity_;
		}
		[[nodiscard]] result<sqlite_backend_entry_observation>
		retained_entry(const sqlite_backend_file_role role) const override
		{
			++state_.retained_entry_calls;
			if (!state_.stable || role != sqlite_backend_file_role::shared_memory)
				return error{"test.retained-epoch", "shm", "stale-entry"};
			if (state_.shm_absent)
				return sqlite_backend_entry_observation{sqlite_backend_file_role::shared_memory,
														sqlite_backend_entry_state::absent,
														std::nullopt,
														std::nullopt,
														nullptr,
														std::nullopt,
														false};
			return entry_;
		}
		[[nodiscard]] result<void> recheck() const override
		{
			++state_.target_rechecks;
			return state_.stable
				? result<void>{}
				: result<void>{error{"test.retained-epoch", "target", "stale-target"}};
		}
		[[nodiscard]] result<void> finish() override
		{
			state_.stable = false;
			return {};
		}

	  private:
		retained_epoch_state& state_;
		std::shared_ptr<retained_epoch_held_object> held_;
		sqlite_backend_entry_observation entry_;
		sqlite_backend_opaque_identity parent_namespace_identity_;
		sqlite_backend_opaque_identity identity_;
	};

	[[nodiscard]] sqlite_writer_shm_mapping_epoch_platform_binding
	retained_epoch_platform_binding(const std::shared_ptr<retained_epoch_target>& target,
									const std::uint8_t marker,
									const bool absent_context = false)
	{
		return {target,
				identity("test.retained-epoch.parent", marker),
				identity("test.retained-epoch.sqlite-source", marker),
				identity("test.retained-epoch.wal-lock", marker),
				identity("test.retained-epoch.effect-gate", marker),
				identity("test.retained-epoch.effect", marker),
				absent_context ? std::optional<sqlite_backend_opaque_identity>{identity(
									 "test.retained-epoch.filesystem", marker)}
							   : std::nullopt,
				absent_context ? std::optional<sqlite_backend_opaque_identity>{identity(
									 "test.retained-epoch.mount", marker)}
							   : std::nullopt};
	}

	[[nodiscard]] sqlite_writer_shm_mapping_epoch_post_observation
	valid_post_observation(const std::uint8_t marker,
						   const sqlite_backend_opaque_identity& watch_receipt,
						   const sqlite_backend_opaque_identity& expected_leaf,
						   const sqlite_writer_shm_stat_census& stat)
	{
		sqlite_writer_shm_namespace_event_census namespace_events;
		namespace_events.watch_epoch = watch_receipt;
		namespace_events.expected_shm_leaf = expected_leaf;
		namespace_events.trusted_stat_watch_profile = true;

		sqlite_writer_shm_effect_census effects;
		effects.sqlite_source_id = identity("test.epoch.sqlite-source-id", marker);
		effects.callback_transcript = identity("test.epoch.callback-transcript", marker);
		effects.wal_write_lock_receipt = identity("test.epoch.wal-write-lock", marker);
		effects.effect_gate_receipt = identity("test.epoch.effect-gate", marker);
		effects.effect_receipt = identity("test.epoch.effect", marker);
		effects.size_before = stat.byte_count;
		effects.size_after = stat.byte_count;
		effects.requested_range_end = 4096U;
		effects.complete = true;
		effects.result_confirmed_success = true;

		return {stat,
				std::move(namespace_events),
				std::move(effects),
				sqlite_writer_shm_observed_transition::preexisting_unchanged};
	}

	class scripted_observation_port final : public sqlite_writer_shm_mapping_epoch_observation_port
	{
	  public:
		sqlite_writer_shm_mapping_epoch_post_observation post;
		std::function<void()> during_observation;
		std::size_t calls{};
		std::optional<sqlite_writer_shm_mapping_epoch_binding> observed_binding;
		std::optional<sqlite_writer_shm_stat_census> observed_pre_stat;
		const volatile void* observed_native_mapping{};

		[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_post_observation>
		observe_after_native_map(const sqlite_writer_shm_mapping_epoch_binding& binding,
								 const sqlite_writer_shm_stat_census& pre_stat,
								 const volatile void* native_mapping) override
		{
			++calls;
			observed_binding.emplace(binding);
			observed_pre_stat.emplace(pre_stat);
			observed_native_mapping = native_mapping;
			if (during_observation)
				during_observation();
			return post;
		}
	};

	class scripted_epoch_port final : public sqlite_writer_shm_mapping_epoch_port
	{
	  public:
		scripted_epoch_port(sqlite_backend_opaque_identity epoch_identity,
							sqlite_backend_opaque_identity watch_receipt,
							sqlite_writer_shm_stat_census pre_stat,
							std::shared_ptr<scripted_observation_port> observation)
			: epoch_identity_{std::move(epoch_identity)}, watch_receipt_{std::move(watch_receipt)},
			  pre_stat_{std::move(pre_stat)}, observation_{std::move(observation)}
		{
		}

		std::size_t arm_calls{};
		std::vector<std::string> preparation_order;
		std::optional<sqlite_writer_shm_mapping_epoch_binding> armed_binding;
		std::function<void(const sqlite_writer_shm_mapping_epoch_request&)> during_pre_stat;

	  protected:
		[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_preparation>
		arm_watch_before_pre_stat(const sqlite_writer_shm_mapping_epoch_request& request) override
		{
			++arm_calls;
			armed_binding.emplace(request.binding);
			preparation_order.emplace_back("watch-arm");
			if (during_pre_stat)
				during_pre_stat(request);
			preparation_order.emplace_back("pre-stat");
			return sqlite_writer_shm_mapping_epoch_preparation{
				epoch_identity_, watch_receipt_, pre_stat_, observation_};
		}

	  private:
		sqlite_backend_opaque_identity epoch_identity_;
		sqlite_backend_opaque_identity watch_receipt_;
		sqlite_writer_shm_stat_census pre_stat_;
		std::shared_ptr<scripted_observation_port> observation_;
	};

	struct port_fixture
	{
		sqlite_backend_opaque_identity epoch_identity;
		sqlite_backend_opaque_identity watch_receipt;
		sqlite_writer_shm_stat_census pre_stat;
		std::shared_ptr<scripted_observation_port> observation;
		scripted_epoch_port port;
	};

	[[nodiscard]] port_fixture make_port_fixture(const epoch_resources& resources,
												 const std::uint8_t marker)
	{
		auto epoch_identity = identity("test.epoch.identity", marker);
		auto watch_receipt = identity("test.epoch.watch", marker);
		auto pre_stat = direct_stat(marker);
		auto observation = std::make_shared<scripted_observation_port>();
		observation->post = valid_post_observation(
			marker, watch_receipt, resources.binding.expected_shm_leaf, pre_stat);
		return {epoch_identity,
				watch_receipt,
				pre_stat,
				observation,
				scripted_epoch_port{epoch_identity, watch_receipt, pre_stat, observation}};
	}

	void verify_pin_owner_and_close_revocation_are_separate()
	{
		const auto semantic_receipt = identity("test.epoch.single-native", 1U);
		const auto xopen_receipt = identity("test.epoch.single-xopen", 1U);
		auto lifetime = make_native_lifetime(sqlite_writer_shm_native_lifetime_role::main_database,
											 semantic_receipt,
											 xopen_receipt,
											 2U);
		{
			auto first = mint_pin(lifetime, "mint first native lifetime pin");
			auto second = mint_pin(lifetime, "mint second native lifetime pin");
			require(lifetime.source.valid() && lifetime.revoker.valid() && first.valid() &&
						second.valid() &&
						first.role() == sqlite_writer_shm_native_lifetime_role::main_database &&
						first.native_lifetime_identity() == second.native_lifetime_identity() &&
						first.semantic_receipt() == semantic_receipt &&
						first.native_xopen_receipt() == xopen_receipt &&
						second.native_xopen_receipt() == xopen_receipt &&
						first.pin_identity() != second.pin_identity() && !lifetime.owner.expired(),
					"one native source did not mint distinct exact lifetime pins");

			lifetime.retained_owner.reset();
			require(lifetime.revoker.revoke() && !lifetime.revoker.valid() &&
						!lifetime.source.valid() && !first.valid() && !second.valid() &&
						!lifetime.owner.expired() &&
						lifetime.destruction_count->load(std::memory_order_relaxed) == 0,
					"native close did not revoke every pin while retaining implementation storage");
			auto after_close = lifetime.source.mint_pin();
			require(!after_close &&
						after_close.error().reason == sqlite_shm_lease_rejection_reason::retiring &&
						after_close.error().action ==
							sqlite_shm_lease_recovery_action::deny_before_native_map,
					"revoked native source minted a fresh pin");
			require(!lifetime.revoker.revoke(), "native lifetime revocation was not one-shot");
		}
		require(lifetime.owner.expired() &&
					lifetime.destruction_count->load(std::memory_order_relaxed) == 1,
				"revoked pins did not release their retained memory owner exactly once");
	}

	void verify_watch_is_armed_before_pre_stat_and_observation()
	{
		auto resources = make_epoch_resources(10U);
		auto fixture = make_port_fixture(resources, 10U);
		require(fixture.port.preparation_order.empty() && fixture.observation->calls == 0U,
				"epoch port performed work before arm");

		auto activation = fixture.port.arm(resources.take_request());
		require(activation.has_value() && fixture.port.arm_calls == 1U &&
					fixture.port.preparation_order ==
						std::vector<std::string>{"watch-arm", "pre-stat"} &&
					fixture.port.armed_binding == resources.binding &&
					fixture.observation->calls == 0U && resources.owners_alive(),
				"epoch arm did not establish watch-before-pre-stat strong authority");

		auto arm = activation->take_arm();
		auto observer = activation->take_observer();
		int native_page{};
		auto sealed = seal_sqlite_writer_shm_mapping_epoch(observer, &native_page);
		require(sealed.has_value(), "valid post-native observation did not seal");
		require(arm.valid() && !observer.valid(),
				"completed post-native observation did not consume observer availability");
		require(fixture.observation->calls == 1U,
				"valid seal did not observe post-map state exactly once");
		require(fixture.observation->observed_binding == resources.binding,
				"observation port received the wrong mapping binding");
		require(fixture.observation->observed_pre_stat == fixture.pre_stat,
				"observation port received the wrong pre-stat census");
		require(fixture.observation->observed_native_mapping == &native_page,
				"observation port received the wrong native mapping");
		require(sealed->epoch_identity() == fixture.epoch_identity,
				"sealed receipt lost its epoch identity");
		require(sealed->watch_arm_receipt() == fixture.watch_receipt,
				"sealed receipt lost its watch-arm receipt");
		require(sealed->binding() == resources.binding,
				"sealed receipt lost its exact mapping binding");
		require(sealed->native_mapping() == &native_page, "sealed receipt lost its native mapping");
	}

	void verify_one_source_revokes_all_pins_and_common_epoch_liveness()
	{
		auto resources = make_epoch_resources(11U);
		auto fixture = make_port_fixture(resources, 11U);
		auto sibling_main_pin = mint_pin(resources.main, "mint sibling main native-file pin");
		auto request = resources.take_request();
		require(sibling_main_pin.valid() && request.main_native_file.valid() &&
					sibling_main_pin.native_lifetime_identity() ==
						request.main_native_file.native_lifetime_identity() &&
					sibling_main_pin.pin_identity() != request.main_native_file.pin_identity() &&
					sibling_main_pin.native_xopen_receipt() ==
						resources.binding.map_request.attachment.main_xopen_receipt(),
				"one main lifetime source did not mint distinct pins for one close epoch");

		auto activation = fixture.port.arm(std::move(request));
		require(activation.has_value(), "arm shared native lifetime revocation fixture");
		auto arm = activation->take_arm();
		auto observer = activation->take_observer();
		require(arm.valid() && observer.valid() && resources.main.source.valid(),
				"shared native lifetime epoch was not initially live");

		require(resources.main.revoker.revoke() && !sibling_main_pin.valid() &&
					!resources.main.source.valid() && !arm.valid() && !observer.valid() &&
					resources.owners_alive(),
				"one native revoker did not invalidate every pin and common epoch liveness");
		auto after_close = resources.main.source.mint_pin();
		require(!after_close &&
					after_close.error().reason == sqlite_shm_lease_rejection_reason::retiring &&
					after_close.error().action ==
						sqlite_shm_lease_recovery_action::deny_before_native_map,
				"revoked main lifetime source minted a later epoch pin");

		int native_page{};
		auto revoked = seal_sqlite_writer_shm_mapping_epoch(observer, &native_page);
		require(
			!revoked &&
				revoked.error().reason == sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				revoked.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				fixture.observation->calls == 0U,
			"revoked common epoch reached post-map observation");
	}

	void verify_revocation_during_pre_stat_prevents_arm_publication()
	{
		auto resources = make_epoch_resources(12U);
		auto fixture = make_port_fixture(resources, 12U);
		bool pre_stat_pin_was_live{};
		bool revoked_during_pre_stat{};
		fixture.port.during_pre_stat =
			[&resources, &pre_stat_pin_was_live, &revoked_during_pre_stat](
				const sqlite_writer_shm_mapping_epoch_request& request)
		{
			pre_stat_pin_was_live = request.main_native_file.valid();
			revoked_during_pre_stat = resources.main.revoker.revoke();
		};

		auto activation = fixture.port.arm(resources.take_request());
		require(!activation &&
					activation.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					activation.error().action ==
						sqlite_shm_lease_recovery_action::deny_before_native_map &&
					pre_stat_pin_was_live && revoked_during_pre_stat &&
					fixture.port.arm_calls == 1U &&
					fixture.port.preparation_order ==
						std::vector<std::string>{"watch-arm", "pre-stat"} &&
					fixture.observation->calls == 0U && !resources.main.source.valid() &&
					resources.owners_expired(),
				"native close during pre-stat published a usable epoch arm");
	}

	void verify_invalid_pairs_fail_before_the_port()
	{
		constexpr std::array invalid_pairs{
			std::pair{1, 0},
			std::pair{0, 1},
			std::pair{-1, 0},
			std::pair{2, 1},
			std::pair{0, 2},
		};
		std::uint8_t marker = 20U;
		for (const auto& [caller_extend, delegated_extend] : invalid_pairs)
		{
			auto resources = make_epoch_resources(marker, caller_extend, delegated_extend);
			auto fixture = make_port_fixture(resources, marker);
			auto armed = fixture.port.arm(resources.take_request());
			require(!armed &&
						armed.error().reason ==
							sqlite_shm_lease_rejection_reason::invalid_extend_pair &&
						armed.error().action ==
							sqlite_shm_lease_recovery_action::deny_before_native_map &&
						fixture.port.arm_calls == 0U && fixture.port.preparation_order.empty() &&
						fixture.observation->calls == 0U && resources.owners_expired(),
					"invalid extend pair reached the platform epoch port or retained authority");
			++marker;
		}
	}

	void verify_main_and_wal_xopen_receipts_are_exact()
	{
		struct xopen_case
		{
			bool main;
			bool missing;
		};
		constexpr std::array cases{
			xopen_case{true, true},
			xopen_case{true, false},
			xopen_case{false, true},
			xopen_case{false, false},
		};
		std::uint8_t marker = 26U;
		for (const auto test : cases)
		{
			auto resources = make_epoch_resources(marker);
			auto fixture = make_port_fixture(resources, marker);
			const auto semantic_receipt = test.main
				? resources.binding.map_request.attachment.main_native_file_receipt()
				: resources.binding.wal_native_file_receipt;
			auto supplied_xopen = test.missing ? std::optional<sqlite_backend_opaque_identity>{}
											   : std::optional<sqlite_backend_opaque_identity>{
													 identity("test.epoch.wrong-xopen", marker)};
			auto replacement = make_native_lifetime(
				test.main ? sqlite_writer_shm_native_lifetime_role::main_database
						  : sqlite_writer_shm_native_lifetime_role::write_ahead_log,
				semantic_receipt,
				std::move(supplied_xopen),
				static_cast<std::uint8_t>(marker + 100U));
			if (test.missing)
			{
				auto missing_pin = replacement.source.mint_pin();
				require(!replacement.source.valid() && !missing_pin &&
							missing_pin.error().reason ==
								sqlite_shm_lease_rejection_reason::invalid_identity &&
							missing_pin.error().action ==
								sqlite_shm_lease_recovery_action::deny_before_native_map &&
							fixture.port.arm_calls == 0U &&
							fixture.port.preparation_order.empty() &&
							fixture.observation->calls == 0U,
						"MAIN/WAL source without xOpen minted a pin or reached the epoch port");
				replacement.retained_owner.reset();
				require(replacement.owner.expired(),
						"malformed xOpen source retained its native memory owner");
				++marker;
				continue;
			}

			auto request = resources.take_request();
			auto replacement_pin = mint_pin(replacement, "mint wrong native xOpen pin");
			auto rejected = [&]()
			{
				if (test.main)
					return fixture.port.arm(sqlite_writer_shm_mapping_epoch_request{
						request.binding,
						std::move(request.retained_parent),
						std::move(replacement_pin),
						std::move(request.wal_native_file),
						std::move(request.shm_native_attachment)});
				return fixture.port.arm(sqlite_writer_shm_mapping_epoch_request{
					request.binding,
					std::move(request.retained_parent),
					std::move(request.main_native_file),
					std::move(replacement_pin),
					std::move(request.shm_native_attachment)});
			}();
			require(!rejected &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::invalid_request &&
						rejected.error().action ==
							sqlite_shm_lease_recovery_action::deny_before_native_map &&
						fixture.port.arm_calls == 0U && fixture.port.preparation_order.empty() &&
						fixture.observation->calls == 0U,
					"wrong MAIN/WAL xOpen receipt reached the epoch port");
			if (test.main)
			{
				[[maybe_unused]] auto discarded = std::move(request.main_native_file);
			}
			else
			{
				[[maybe_unused]] auto discarded = std::move(request.wal_native_file);
			}
			require(resources.owners_expired(),
					"rejected xOpen request retained an original native memory owner");
			replacement.retained_owner.reset();
			require(replacement.owner.expired(),
					"rejected xOpen pin retained its native memory owner");
			++marker;
		}
	}

	void verify_native_role_and_xopen_shapes_fail_closed()
	{
		for (const auto& [role, marker] :
			 {std::pair{sqlite_writer_shm_native_lifetime_role::retained_parent, std::uint8_t{83U}},
			  std::pair{sqlite_writer_shm_native_lifetime_role::shared_memory_attachment,
						std::uint8_t{84U}}})
		{
			auto malformed = make_native_lifetime(role,
												  identity("test.epoch.no-xopen-role", marker),
												  identity("test.epoch.unexpected-xopen", marker),
												  marker);
			auto pin = malformed.source.mint_pin();
			require(!malformed.source.valid() && !pin &&
						pin.error().reason == sqlite_shm_lease_rejection_reason::invalid_identity &&
						pin.error().action ==
							sqlite_shm_lease_recovery_action::deny_before_native_map,
					"parent or SHM lifetime source accepted an xOpen receipt");
			malformed.retained_owner.reset();
			require(malformed.owner.expired(),
					"malformed native lifetime source retained its owner");
		}

		auto resources = make_epoch_resources(85U);
		auto fixture = make_port_fixture(resources, 85U);
		auto request = resources.take_request();
		auto wrong_role = make_native_lifetime(
			sqlite_writer_shm_native_lifetime_role::write_ahead_log,
			resources.binding.map_request.attachment.main_native_file_receipt(),
			resources.binding.map_request.attachment.main_xopen_receipt(),
			185U);
		auto wrong_role_pin = mint_pin(wrong_role, "mint structurally valid wrong-role pin");
		auto rejected = fixture.port.arm(
			sqlite_writer_shm_mapping_epoch_request{request.binding,
													std::move(request.retained_parent),
													std::move(wrong_role_pin),
													std::move(request.wal_native_file),
													std::move(request.shm_native_attachment)});
		require(!rejected &&
					rejected.error().reason == sqlite_shm_lease_rejection_reason::invalid_request &&
					rejected.error().action ==
						sqlite_shm_lease_recovery_action::deny_before_native_map &&
					fixture.port.arm_calls == 0U && fixture.port.preparation_order.empty() &&
					fixture.observation->calls == 0U,
				"wrong native lifetime role reached the epoch port");
		{
			[[maybe_unused]] auto discarded = std::move(request.main_native_file);
		}
		require(resources.owners_expired(),
				"wrong-role rejection retained an original native memory owner");
		wrong_role.retained_owner.reset();
		require(wrong_role.owner.expired(),
				"wrong-role replacement pin retained its native memory owner");
	}

	void verify_observer_is_weak_and_requires_the_strong_arm()
	{
		auto resources = make_epoch_resources(30U);
		auto fixture = make_port_fixture(resources, 30U);
		auto activation = fixture.port.arm(resources.take_request());
		require(activation.has_value(), "arm weak-observer fixture");
		auto observer = activation->take_observer();
		{
			auto arm = activation->take_arm();
			require(arm.valid() && observer.valid() && resources.owners_alive(),
					"weak observer did not see the live strong epoch arm");
		}
		require(!observer.valid() && resources.owners_expired(),
				"weak observer or activation retained native lifetime after arm destruction");

		int native_page{};
		auto stale = seal_sqlite_writer_shm_mapping_epoch(observer, &native_page);
		require(!stale && stale.error().reason == sqlite_shm_lease_rejection_reason::stale_token &&
					stale.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					fixture.observation->calls == 0U,
				"observer without a strong arm reached post-native observation");
	}

	void verify_seal_is_one_shot_and_replay_is_terminal()
	{
		auto resources = make_epoch_resources(40U);
		auto fixture = make_port_fixture(resources, 40U);
		auto activation = fixture.port.arm(resources.take_request());
		require(activation.has_value(), "arm one-shot seal fixture");
		auto arm = activation->take_arm();
		auto observer = activation->take_observer();
		int native_page{};
		auto first = seal_sqlite_writer_shm_mapping_epoch(observer, &native_page);
		require(first.has_value() && fixture.observation->calls == 1U && !observer.valid(),
				"first epoch seal did not mint one audit receipt");
		const auto audit_copy = *first; // NOLINT(performance-unnecessary-copy-initialization)
		require(audit_copy.epoch_identity() == first->epoch_identity() &&
					audit_copy.native_mapping() == &native_page && arm.valid(),
				"copyable audit receipt changed exact sealed evidence");

		auto replay = seal_sqlite_writer_shm_mapping_epoch(observer, &native_page);
		require(
			!replay &&
				replay.error().reason == sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				replay.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				fixture.observation->calls == 1U && arm.valid(),
			"epoch seal replay observed twice or escaped terminal rejection");
	}

	void verify_null_seal_attempt_cannot_be_replayed_as_nonnull()
	{
		auto resources = make_epoch_resources(41U);
		auto fixture = make_port_fixture(resources, 41U);
		auto activation = fixture.port.arm(resources.take_request());
		require(activation.has_value(), "arm null-to-nonnull replay fixture");
		auto arm = activation->take_arm();
		auto observer = activation->take_observer();

		auto null_result = seal_sqlite_writer_shm_mapping_epoch(observer, nullptr);
		require(!null_result &&
					null_result.error().reason ==
						sqlite_shm_lease_rejection_reason::receipt_mismatch &&
					null_result.error().action ==
						sqlite_shm_lease_recovery_action::outer_ioerr_no_retry &&
					fixture.observation->calls == 0U && arm.valid() && !observer.valid(),
				"null seal attempt did not return the exact no-mapping rejection");

		int native_page{};
		auto replay = seal_sqlite_writer_shm_mapping_epoch(observer, &native_page);
		require(
			!replay &&
				replay.error().reason == sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
				replay.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry &&
				fixture.observation->calls == 0U && arm.valid(),
			"null seal attempt was replayed as a later nonnull authority");
	}

	void verify_revocation_before_and_during_observation_fails_closed()
	{
		{
			auto resources = make_epoch_resources(50U);
			auto fixture = make_port_fixture(resources, 50U);
			auto activation = fixture.port.arm(resources.take_request());
			require(activation.has_value(), "arm pre-observation revocation fixture");
			auto arm = activation->take_arm();
			auto observer = activation->take_observer();
			require(resources.main.revoker.revoke() && !arm.valid() && !observer.valid() &&
						resources.owners_alive(),
					"main close did not revoke the armed epoch while retaining memory owners");

			int native_page{};
			auto revoked = seal_sqlite_writer_shm_mapping_epoch(observer, &native_page);
			require(!revoked &&
						revoked.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						revoked.error().action ==
							sqlite_shm_lease_recovery_action::quarantine_no_retry &&
						fixture.observation->calls == 0U,
					"pre-observation close reached the observation port or minted a receipt");
		}

		{
			auto resources = make_epoch_resources(51U);
			auto fixture = make_port_fixture(resources, 51U);
			fixture.observation->during_observation = [&resources]() noexcept
			{
				(void)resources.wal.revoker.revoke();
			};
			auto activation = fixture.port.arm(resources.take_request());
			require(activation.has_value(), "arm in-observation revocation fixture");
			auto arm = activation->take_arm();
			auto observer = activation->take_observer();

			int native_page{};
			auto revoked = seal_sqlite_writer_shm_mapping_epoch(observer, &native_page);
			require(!revoked &&
						revoked.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
						revoked.error().action ==
							sqlite_shm_lease_recovery_action::quarantine_no_retry &&
						fixture.observation->calls == 1U && !arm.valid() && !observer.valid() &&
						resources.owners_alive(),
					"WAL close during post-map observation minted or revived authority");
		}
	}

	void verify_malformed_post_observation_never_seals()
	{
		for (std::uint8_t mutation = 0U; mutation < 3U; ++mutation)
		{
			const auto marker = static_cast<std::uint8_t>(60U + mutation);
			auto resources = make_epoch_resources(marker);
			auto fixture = make_port_fixture(resources, marker);
			switch (mutation)
			{
				case 0U:
					fixture.observation->post.stat.state = sqlite_writer_shm_entry_state::absent;
					break;
				case 1U:
					fixture.observation->post.namespace_events.watch_epoch =
						identity("test.epoch.wrong-watch", marker);
					break;
				case 2U:
					fixture.observation->post.namespace_events.expected_shm_leaf =
						identity("test.epoch.wrong-leaf", marker);
					break;
				default:
					throw std::runtime_error{"unreachable post-observation mutation"};
			}

			auto activation = fixture.port.arm(resources.take_request());
			require(activation.has_value(), "arm malformed post-observation fixture");
			auto arm = activation->take_arm();
			auto observer = activation->take_observer();
			int native_page{};
			auto malformed = seal_sqlite_writer_shm_mapping_epoch(observer, &native_page);
			require(!malformed &&
						malformed.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch &&
						malformed.error().action ==
							sqlite_shm_lease_recovery_action::
								attempt_nonremoving_unmap_then_outer_ioerr &&
						fixture.observation->calls == 1U && arm.valid() && !observer.valid(),
					"malformed post-map observation minted an epoch receipt");
		}
	}

	void verify_audit_receipt_copy_does_not_retain_authority()
	{
		auto resources = make_epoch_resources(70U);
		auto fixture = make_port_fixture(resources, 70U);
		std::optional<sqlite_writer_shm_mapping_epoch_receipt> retained_audit;
		int native_page{};
		{
			auto activation = fixture.port.arm(resources.take_request());
			require(activation.has_value(), "arm audit-only receipt fixture");
			auto arm = activation->take_arm();
			auto observer = activation->take_observer();
			auto sealed = seal_sqlite_writer_shm_mapping_epoch(observer, &native_page);
			require(sealed.has_value() && resources.owners_alive(),
					"seal audit-only receipt fixture");
			retained_audit.emplace(*sealed);
			require(retained_audit->binding() == resources.binding &&
						retained_audit->native_mapping() == &native_page,
					"audit receipt copy lost immutable evidence");
		}

		require(resources.owners_expired() &&
					resources.parent.destruction_count->load(std::memory_order_relaxed) == 1 &&
					resources.main.destruction_count->load(std::memory_order_relaxed) == 1 &&
					resources.wal.destruction_count->load(std::memory_order_relaxed) == 1 &&
					resources.shm.destruction_count->load(std::memory_order_relaxed) == 1,
				"copyable audit receipt retained native lifetime authority");
		const auto second_copy = *retained_audit;
		require(second_copy.epoch_identity() == fixture.epoch_identity &&
					second_copy.watch_arm_receipt() == fixture.watch_receipt &&
					second_copy.native_mapping() == &native_page && resources.owners_expired(),
				"copying audit evidence revived its expired strong epoch");
	}

	void verify_retained_namespace_port_proves_only_existing_zero_effect_route()
	{
		constexpr auto marker = std::uint8_t{90U};
		retained_epoch_state state;
		auto target = std::make_shared<retained_epoch_target>(state, marker);
		auto platform = retained_epoch_platform_binding(target, marker);
		auto resources = make_epoch_resources(marker);
		sqlite_retained_namespace_writer_shm_mapping_epoch_port port{std::move(platform)};
		auto activation = port.arm(resources.take_request());
		require(activation.has_value(), "retained namespace epoch did not arm");
		auto arm = activation->take_arm();
		auto observer = activation->take_observer();
		std::array<std::byte, 4096U> native_page{};
		auto sealed = seal_sqlite_writer_shm_mapping_epoch(observer, native_page.data());
		require(sealed.has_value(), "retained namespace epoch did not seal");
		require(sealed->pre_stat().state == sqlite_writer_shm_entry_state::direct_regular &&
					sealed->post_observation().stat == sealed->pre_stat() &&
					sealed->post_observation().transition ==
						sqlite_writer_shm_observed_transition::preexisting_unchanged &&
					sealed->post_observation().namespace_events.trusted_stat_watch_profile &&
					sealed->post_observation().effects.create_count ==
						sqlite_writer_shm_bounded_count::zero &&
					sealed->post_observation().effects.extend_count ==
						sqlite_writer_shm_bounded_count::zero &&
					sealed->post_observation().effects.resize_count ==
						sqlite_writer_shm_bounded_count::zero &&
					sealed->post_observation().effects.complete &&
					sealed->post_observation().effects.result_confirmed_success && arm.valid(),
				"retained namespace epoch did not preserve the exact zero-effect proof");
		require(state.target_rechecks >= 3U && state.retained_entry_calls == 2U &&
					state.held_rechecks == 2U && state.size_calls == 2U &&
					state.duplicate_namespace_opens == 0U,
				"retained namespace epoch did not recheck the held object without reopening it");
	}

	void verify_retained_namespace_port_supports_writer_extend_matrix_and_stale_post_map()
	{
		{
			constexpr auto marker = std::uint8_t{91U};
			retained_epoch_state state;
			auto target = std::make_shared<retained_epoch_target>(state, marker);
			auto resources = make_epoch_resources(marker, 1, 1);
			sqlite_retained_namespace_writer_shm_mapping_epoch_port port{
				retained_epoch_platform_binding(target, marker)};
			auto activation = port.arm(resources.take_request());
			require(activation.has_value(),
					"retained namespace port rejected one-one preallocation");
			auto arm = activation->take_arm();
			auto observer = activation->take_observer();
			std::array<std::byte, 4096U> native_page{};
			auto preallocated = seal_sqlite_writer_shm_mapping_epoch(observer, native_page.data());
			require(preallocated.has_value() &&
						preallocated->post_observation().transition ==
							sqlite_writer_shm_observed_transition::preexisting_preallocated &&
						preallocated->post_observation().effects.extend_count ==
							sqlite_writer_shm_bounded_count::zero &&
						arm.valid(),
					"retained namespace port did not preserve one-one preallocation");
		}

		{
			constexpr auto marker = std::uint8_t{94U};
			retained_epoch_state state;
			state.shm_absent = true;
			auto target = std::make_shared<retained_epoch_target>(state, marker);
			auto resources = make_epoch_resources(marker, 1, 1);
			sqlite_retained_namespace_writer_shm_mapping_epoch_port port{
				retained_epoch_platform_binding(target, marker, true)};
			auto activation = port.arm(resources.take_request());
			require(activation.has_value(),
					"retained namespace port rejected absent one-one create");
			auto arm = activation->take_arm();
			auto observer = activation->take_observer();
			state.shm_absent = false;
			std::array<std::byte, 4096U> native_page{};
			auto created = seal_sqlite_writer_shm_mapping_epoch(observer, native_page.data());
			require(created.has_value() &&
						created->pre_stat().state == sqlite_writer_shm_entry_state::absent &&
						created->post_observation().transition ==
							sqlite_writer_shm_observed_transition::absent_created &&
						created->post_observation().namespace_events.expected_leaf_create ==
							sqlite_writer_shm_bounded_count::one &&
						created->post_observation().effects.create_count ==
							sqlite_writer_shm_bounded_count::one &&
						arm.valid(),
					"retained namespace port did not preserve absent one-one create");
		}

		{
			constexpr auto marker = std::uint8_t{93U};
			retained_epoch_state state;
			auto target = std::make_shared<retained_epoch_target>(state, marker);
			auto resources = make_epoch_resources(marker, 1, 1);
			resources.binding.map_request.page_number = 1;
			sqlite_retained_namespace_writer_shm_mapping_epoch_port port{
				retained_epoch_platform_binding(target, marker)};
			auto activation = port.arm(resources.take_request());
			require(activation.has_value(), "retained namespace port rejected one-one growth");
			auto arm = activation->take_arm();
			auto observer = activation->take_observer();
			state.shm_size = 8192U;
			std::array<std::byte, 4096U> native_page{};
			auto grown = seal_sqlite_writer_shm_mapping_epoch(observer, native_page.data());
			require(grown.has_value() &&
						grown->post_observation().transition ==
							sqlite_writer_shm_observed_transition::preexisting_grown &&
						grown->post_observation().effects.extend_count ==
							sqlite_writer_shm_bounded_count::one &&
						arm.valid(),
					"retained namespace port did not preserve one-one growth");
		}

		constexpr auto marker = std::uint8_t{92U};
		retained_epoch_state state;
		auto target = std::make_shared<retained_epoch_target>(state, marker);
		auto resources = make_epoch_resources(marker);
		sqlite_retained_namespace_writer_shm_mapping_epoch_port port{
			retained_epoch_platform_binding(target, marker)};
		auto activation = port.arm(resources.take_request());
		require(activation.has_value(), "arm retained namespace stale-post fixture");
		auto arm = activation->take_arm();
		auto observer = activation->take_observer();
		state.stable = false;
		std::array<std::byte, 4096U> native_page{};
		auto stale = seal_sqlite_writer_shm_mapping_epoch(observer, native_page.data());
		require(!stale &&
					stale.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					stale.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry &&
					arm.valid() && state.retained_entry_calls == 1U,
				"retained namespace port retried a stale target after native mapping");
	}
} // namespace

int main()
{
	try
	{
		verify_production_lifetime_factory_is_complete_and_revocable();
		verify_pin_owner_and_close_revocation_are_separate();
		verify_watch_is_armed_before_pre_stat_and_observation();
		verify_one_source_revokes_all_pins_and_common_epoch_liveness();
		verify_revocation_during_pre_stat_prevents_arm_publication();
		verify_invalid_pairs_fail_before_the_port();
		verify_main_and_wal_xopen_receipts_are_exact();
		verify_native_role_and_xopen_shapes_fail_closed();
		verify_observer_is_weak_and_requires_the_strong_arm();
		verify_seal_is_one_shot_and_replay_is_terminal();
		verify_null_seal_attempt_cannot_be_replayed_as_nonnull();
		verify_revocation_before_and_during_observation_fails_closed();
		verify_malformed_post_observation_never_seals();
		verify_audit_receipt_copy_does_not_retain_authority();
		verify_retained_namespace_port_proves_only_existing_zero_effect_route();
		verify_retained_namespace_port_supports_writer_extend_matrix_and_stale_post_map();
	}
	catch (const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
	return 0;
}
