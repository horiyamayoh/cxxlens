#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sdk/sqlite_writer_shm_mapping_semantics_internal.hpp"

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
		return {identity("test.mapping-semantics.process", marker),
				identity("test.mapping-semantics.runtime-vfs", marker),
				identity("test.mapping-semantics.file-family", marker)};
	}

	[[nodiscard]] sqlite_shm_callback_execution_receipt callback(const std::uint8_t marker)
	{
		return {identity("test.mapping-semantics.thread", marker),
				0U,
				identity("test.mapping-semantics.invocation", marker)};
	}

	struct native_lifetime_fixture
	{
		sqlite_writer_shm_native_lifetime_revoker revoker;
		sqlite_writer_shm_native_lifetime_source source;
		std::shared_ptr<int> owner;
		std::weak_ptr<int> weak_owner;
	};

	[[nodiscard]] native_lifetime_fixture
	make_native_lifetime(const sqlite_writer_shm_native_lifetime_role role,
						 const sqlite_backend_opaque_identity& semantic_receipt,
						 std::optional<sqlite_backend_opaque_identity> native_xopen_receipt,
						 const std::uint8_t marker)
	{
		auto owner = std::make_shared<int>(marker);
		auto weak_owner = std::weak_ptr<int>{owner};
		auto lifetime = sqlite_writer_shm_mapping_epoch_test_peer::native_lifetime_source(
			role,
			identity("test.mapping-semantics.native-lifetime", marker),
			semantic_receipt,
			std::move(native_xopen_receipt),
			owner);
		return {std::move(lifetime.first),
				std::move(lifetime.second),
				std::move(owner),
				std::move(weak_owner)};
	}

	[[nodiscard]] sqlite_writer_shm_native_lifetime_pin mint_pin(native_lifetime_fixture& lifetime)
	{
		auto pin = lifetime.source.mint_pin();
		require(pin.has_value(), "native lifetime source did not mint an epoch pin");
		return std::move(*pin);
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
			auto parent_pin = mint_pin(parent);
			auto main_pin = mint_pin(main);
			auto wal_pin = mint_pin(wal);
			auto shm_pin = mint_pin(shm);
			parent.owner.reset();
			main.owner.reset();
			wal.owner.reset();
			shm.owner.reset();
			return {binding,
					std::move(parent_pin),
					std::move(main_pin),
					std::move(wal_pin),
					std::move(shm_pin)};
		}

		[[nodiscard]] std::array<std::weak_ptr<int>, 4> owner_refs() const
		{
			return {parent.weak_owner, main.weak_owner, wal.weak_owner, shm.weak_owner};
		}
	};

	[[nodiscard]] epoch_resources
	make_epoch_resources(const sqlite_writer_shm_mapping_epoch_binding& binding,
						 const std::uint8_t marker)
	{
		return {
			binding,
			make_native_lifetime(sqlite_writer_shm_native_lifetime_role::retained_parent,
								 binding.retained_parent_receipt,
								 std::nullopt,
								 static_cast<std::uint8_t>(marker + 1U)),
			make_native_lifetime(sqlite_writer_shm_native_lifetime_role::main_database,
								 binding.map_request.attachment.main_native_file_receipt(),
								 binding.map_request.attachment.main_xopen_receipt(),
								 static_cast<std::uint8_t>(marker + 2U)),
			make_native_lifetime(sqlite_writer_shm_native_lifetime_role::write_ahead_log,
								 binding.wal_native_file_receipt,
								 binding.wal_xopen_receipt,
								 static_cast<std::uint8_t>(marker + 3U)),
			make_native_lifetime(sqlite_writer_shm_native_lifetime_role::shared_memory_attachment,
								 binding.shm_native_attachment_receipt,
								 std::nullopt,
								 static_cast<std::uint8_t>(marker + 4U)),
		};
	}

	[[nodiscard]] sqlite_writer_shm_stat_census direct_stat(const std::uint8_t marker,
															const std::uint64_t byte_count)
	{
		return {
			sqlite_writer_shm_entry_state::direct_regular,
			identity("test.mapping-semantics.parent-namespace", marker),
			identity("test.mapping-semantics.filesystem", marker),
			identity("test.mapping-semantics.mount", marker),
			identity("test.mapping-semantics.shm-object", marker),
			identity("test.mapping-semantics.shm-entry", marker),
			byte_count,
		};
	}

	[[nodiscard]] sqlite_writer_shm_stat_census absent_stat(const std::uint8_t marker)
	{
		return {
			sqlite_writer_shm_entry_state::absent,
			identity("test.mapping-semantics.parent-namespace", marker),
			identity("test.mapping-semantics.filesystem", marker),
			identity("test.mapping-semantics.mount", marker),
			std::nullopt,
			std::nullopt,
			0U,
		};
	}

	enum class scenario_kind : std::uint8_t
	{
		unchanged,
		preallocated,
		grown,
		created,
	};

	struct epoch_scenario
	{
		sqlite_writer_shm_mapping_epoch_binding binding;
		sqlite_backend_opaque_identity epoch_identity;
		sqlite_backend_opaque_identity watch_receipt;
		sqlite_writer_shm_stat_census pre;
		sqlite_writer_shm_mapping_epoch_post_observation post;
	};

	[[nodiscard]] sqlite_writer_shm_mapping_semantic_route expected_route(const scenario_kind kind)
	{
		switch (kind)
		{
			case scenario_kind::unchanged:
				return sqlite_writer_shm_mapping_semantic_route::zero_zero_preexisting_unchanged;
			case scenario_kind::preallocated:
				return sqlite_writer_shm_mapping_semantic_route::one_one_preexisting_preallocated;
			case scenario_kind::grown:
				return sqlite_writer_shm_mapping_semantic_route::one_one_preexisting_grown;
			case scenario_kind::created:
				return sqlite_writer_shm_mapping_semantic_route::one_one_absent_created;
		}
		throw std::runtime_error{"unknown mapping semantics scenario"};
	}

	[[nodiscard]] sqlite_shm_writer_extend_pair expected_pair(const scenario_kind kind)
	{
		return kind == scenario_kind::unchanged ? sqlite_shm_writer_extend_pair::zero_zero
												: sqlite_shm_writer_extend_pair::one_one;
	}

	[[nodiscard]] sqlite_writer_shm_observed_transition
	expected_transition(const scenario_kind kind)
	{
		switch (kind)
		{
			case scenario_kind::unchanged:
				return sqlite_writer_shm_observed_transition::preexisting_unchanged;
			case scenario_kind::preallocated:
				return sqlite_writer_shm_observed_transition::preexisting_preallocated;
			case scenario_kind::grown:
				return sqlite_writer_shm_observed_transition::preexisting_grown;
			case scenario_kind::created:
				return sqlite_writer_shm_observed_transition::absent_created;
		}
		throw std::runtime_error{"unknown mapping semantics scenario"};
	}

	[[nodiscard]] epoch_scenario make_scenario(const scenario_kind kind, const std::uint8_t marker)
	{
		const auto binding_family = family(marker);
		const auto alias = identity("test.mapping-semantics.alias", marker);
		const auto connection = identity("test.mapping-semantics.connection", marker);
		const auto main_receipt = identity("test.mapping-semantics.main-native", marker);
		const auto main_xopen = identity("test.mapping-semantics.main-xopen", marker);
		auto attachment = sqlite_shm_native_attachment_identity::bind(
			binding_family,
			alias,
			connection,
			main_receipt,
			main_xopen,
			identity("test.mapping-semantics.open-epoch", marker),
			identity("test.mapping-semantics.callback-cohort", marker),
			identity("test.mapping-semantics.attachment-epoch", marker));
		require(attachment.has_value(), "bind native attachment for semantics scenario");

		const auto caller_extend = kind == scenario_kind::unchanged ? 0 : 1;
		const auto delegated_extend = caller_extend;
		auto pre = kind == scenario_kind::created
			? absent_stat(marker)
			: direct_stat(marker, kind == scenario_kind::grown ? 4096U : 8192U);
		auto post_stat = direct_stat(marker, 8192U);
		auto watch_receipt = identity("test.mapping-semantics.watch", marker);
		const auto expected_leaf = identity("test.mapping-semantics.shm-leaf", marker);

		sqlite_writer_shm_namespace_event_census events;
		events.watch_epoch = watch_receipt;
		events.expected_shm_leaf = expected_leaf;
		events.expected_leaf_create = kind == scenario_kind::created
			? sqlite_writer_shm_bounded_count::one
			: sqlite_writer_shm_bounded_count::zero;
		events.trusted_stat_watch_profile = true;

		sqlite_writer_shm_effect_census effects;
		effects.sqlite_source_id = identity("test.mapping-semantics.sqlite-source", marker);
		effects.callback_transcript = identity("test.mapping-semantics.transcript", marker);
		effects.wal_write_lock_receipt = identity("test.mapping-semantics.wal-write-lock", marker);
		effects.effect_gate_receipt = identity("test.mapping-semantics.effect-gate", marker);
		effects.effect_receipt = identity("test.mapping-semantics.effect", marker);
		effects.create_count = kind == scenario_kind::created
			? sqlite_writer_shm_bounded_count::one
			: sqlite_writer_shm_bounded_count::zero;
		effects.extend_count = kind == scenario_kind::grown ? sqlite_writer_shm_bounded_count::one
															: sqlite_writer_shm_bounded_count::zero;
		effects.size_before = pre.byte_count;
		effects.size_after = post_stat.byte_count;
		effects.requested_range_end = 8192U;
		effects.complete = true;
		effects.result_confirmed_success = true;

		return {
			sqlite_writer_shm_mapping_epoch_binding{
				sqlite_shm_writer_map_request{
					binding_family,
					alias,
					connection,
					std::move(*attachment),
					callback(marker),
					1,
					4096,
					caller_extend,
				},
				delegated_extend,
				expected_leaf,
				identity("test.mapping-semantics.parent-receipt", marker),
				identity("test.mapping-semantics.wal-native", marker),
				identity("test.mapping-semantics.wal-xopen", marker),
				identity("test.mapping-semantics.shm-attachment", marker),
			},
			identity("test.mapping-semantics.epoch", marker),
			watch_receipt,
			std::move(pre),
			sqlite_writer_shm_mapping_epoch_post_observation{std::move(post_stat),
															 std::move(events),
															 std::move(effects),
															 expected_transition(kind)},
		};
	}

	class scripted_observation_port final : public sqlite_writer_shm_mapping_epoch_observation_port
	{
	  public:
		explicit scripted_observation_port(
			sqlite_writer_shm_mapping_epoch_post_observation observation)
			: observation_{std::move(observation)}
		{
		}

		[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_post_observation>
		observe_after_native_map(const sqlite_writer_shm_mapping_epoch_binding&,
								 const sqlite_writer_shm_stat_census&,
								 const volatile void*) override
		{
			++calls;
			return observation_;
		}

		std::size_t calls{};

	  private:
		sqlite_writer_shm_mapping_epoch_post_observation observation_;
	};

	class scripted_epoch_port final : public sqlite_writer_shm_mapping_epoch_port
	{
	  public:
		scripted_epoch_port(sqlite_backend_opaque_identity epoch_identity,
							sqlite_backend_opaque_identity watch_receipt,
							sqlite_writer_shm_stat_census pre,
							std::shared_ptr<scripted_observation_port> observation)
			: epoch_identity_{std::move(epoch_identity)}, watch_receipt_{std::move(watch_receipt)},
			  pre_{std::move(pre)}, observation_{std::move(observation)}
		{
		}

		std::size_t calls{};

	  protected:
		[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_preparation>
		arm_watch_before_pre_stat(const sqlite_writer_shm_mapping_epoch_request&) override
		{
			++calls;
			return sqlite_writer_shm_mapping_epoch_preparation{
				epoch_identity_, watch_receipt_, pre_, observation_};
		}

	  private:
		sqlite_backend_opaque_identity epoch_identity_;
		sqlite_backend_opaque_identity watch_receipt_;
		sqlite_writer_shm_stat_census pre_;
		std::shared_ptr<scripted_observation_port> observation_;
	};

	struct sealed_fixture
	{
		epoch_resources resources;
		std::shared_ptr<int> native_page;
		sqlite_writer_shm_mapping_epoch_arm arm;
		sqlite_writer_shm_mapping_epoch_receipt receipt;
	};

	using scenario_mutator = std::function<void(epoch_scenario&)>;

	[[nodiscard]] sealed_fixture seal_genuine_scenario(const scenario_kind kind,
													   const std::uint8_t marker,
													   const scenario_mutator& mutate = {})
	{
		auto scenario = make_scenario(kind, marker);
		if (mutate)
			mutate(scenario);
		auto resources = make_epoch_resources(scenario.binding, marker);
		auto observation = std::make_shared<scripted_observation_port>(std::move(scenario.post));
		scripted_epoch_port port{
			scenario.epoch_identity, scenario.watch_receipt, scenario.pre, observation};
		auto activation = port.arm(resources.take_request());
		require(activation.has_value(), "valid semantics scenario did not arm");
		auto arm = activation->take_arm();
		auto observer = activation->take_observer();
		auto native_page = std::make_shared<int>(marker);
		auto receipt = seal_sqlite_writer_shm_mapping_epoch(observer, native_page.get());
		require(receipt.has_value() && port.calls == 1U && observation->calls == 1U && arm.valid(),
				"valid semantics scenario did not produce a genuine epoch receipt");
		return {std::move(resources), std::move(native_page), std::move(arm), std::move(*receipt)};
	}

	void require_determinate_rejection(
		const sqlite_shm_lease_result<sqlite_writer_shm_mapping_semantic_audit>& result,
		const std::string_view message)
	{
		require(
			!result &&
				result.error().reason == sqlite_shm_lease_rejection_reason::receipt_mismatch &&
				result.error().action ==
					sqlite_shm_lease_recovery_action::attempt_nonremoving_unmap_then_outer_ioerr,
			message);
	}

	void require_ambiguous_rejection(
		const sqlite_shm_lease_result<sqlite_writer_shm_mapping_semantic_audit>& result,
		const std::string_view message)
	{
		require(!result &&
					result.error().reason ==
						sqlite_shm_lease_rejection_reason::lifecycle_ambiguous &&
					result.error().action == sqlite_shm_lease_recovery_action::quarantine_no_retry,
				message);
	}

	void expect_determinate(const scenario_mutator& mutate,
							const std::string_view message,
							const scenario_kind kind = scenario_kind::unchanged)
	{
		static std::uint16_t sequence = 60U;
		const auto marker = static_cast<std::uint8_t>(sequence++);
		auto fixture = seal_genuine_scenario(kind, marker, mutate);
		require_determinate_rejection(
			validate_sqlite_writer_shm_mapping_semantics_for_audit(fixture.receipt), message);
	}

	void expect_ambiguous(const scenario_mutator& mutate,
						  const std::string_view message,
						  const scenario_kind kind = scenario_kind::unchanged)
	{
		static std::uint16_t sequence = 150U;
		const auto marker = static_cast<std::uint8_t>(sequence++);
		auto fixture = seal_genuine_scenario(kind, marker, mutate);
		require_ambiguous_rejection(
			validate_sqlite_writer_shm_mapping_semantics_for_audit(fixture.receipt), message);
	}

	void verify_four_positive_routes_and_output()
	{
		constexpr std::array kinds{
			scenario_kind::unchanged,
			scenario_kind::preallocated,
			scenario_kind::grown,
			scenario_kind::created,
		};
		std::uint8_t marker = 1U;
		for (const auto kind : kinds)
		{
			auto fixture = seal_genuine_scenario(kind, marker);
			const auto result =
				validate_sqlite_writer_shm_mapping_semantics_for_audit(fixture.receipt);
			require(result.has_value() && result->route == expected_route(kind) &&
						result->extend_pair == expected_pair(kind) &&
						result->mapping ==
							sqlite_shm_mapping_tuple{
								1, 4096, 4096U, 4096U, fixture.native_page.get(), 8192U} &&
						result->holder_specific_effect_receipt ==
							fixture.receipt.post_observation().effects.effect_receipt,
					"positive route lost its exact route, pair, tuple, or effect receipt");
			++marker;
		}
	}

	void verify_pair_matrix()
	{
		expect_determinate(
			[](auto& scenario)
			{
				scenario.binding.map_request.caller_extend = 1;
				scenario.binding.delegated_extend = 1;
			},
			"one/one pair accepted zero/zero unchanged semantics");
		expect_determinate(
			[](auto& scenario)
			{
				scenario.binding.map_request.caller_extend = 0;
				scenario.binding.delegated_extend = 0;
			},
			"zero/zero pair accepted absent/create semantics",
			scenario_kind::created);

		constexpr std::array invalid_pairs{
			std::pair{0, 1},
			std::pair{1, 0},
			std::pair{-1, 0},
			std::pair{0, 2},
		};
		std::uint8_t marker = 10U;
		for (const auto& [caller_extend, delegated_extend] : invalid_pairs)
		{
			auto scenario = make_scenario(scenario_kind::unchanged, marker);
			scenario.binding.map_request.caller_extend = caller_extend;
			scenario.binding.delegated_extend = delegated_extend;
			auto resources = make_epoch_resources(scenario.binding, marker);
			auto observation =
				std::make_shared<scripted_observation_port>(std::move(scenario.post));
			scripted_epoch_port port{
				scenario.epoch_identity, scenario.watch_receipt, scenario.pre, observation};
			auto activation = port.arm(resources.take_request());
			require(!activation &&
						activation.error().reason ==
							sqlite_shm_lease_rejection_reason::invalid_extend_pair &&
						activation.error().action ==
							sqlite_shm_lease_recovery_action::deny_before_native_map &&
						port.calls == 0U && observation->calls == 0U,
					"invalid extend pair reached the post-native semantics boundary");
			++marker;
		}
	}

	void verify_transition_matrix()
	{
		constexpr std::array kinds{
			scenario_kind::unchanged,
			scenario_kind::preallocated,
			scenario_kind::grown,
			scenario_kind::created,
		};
		constexpr std::array transitions{
			sqlite_writer_shm_observed_transition::preexisting_unchanged,
			sqlite_writer_shm_observed_transition::preexisting_preallocated,
			sqlite_writer_shm_observed_transition::preexisting_grown,
			sqlite_writer_shm_observed_transition::absent_created,
			sqlite_writer_shm_observed_transition::unclassified,
		};
		for (const auto kind : kinds)
			for (const auto transition : transitions)
				if (transition != expected_transition(kind))
					expect_determinate(
						[transition](auto& scenario)
						{
							scenario.post.transition = transition;
						},
						"wrong transition classification escaped determinate cleanup",
						kind);
	}

	void verify_stat_state_identity_and_context_matrix()
	{
		expect_determinate(
			[](auto& scenario)
			{
				scenario.pre.state = sqlite_writer_shm_entry_state::absent;
				scenario.pre.object_identity.reset();
				scenario.pre.directory_entry_identity.reset();
				scenario.pre.byte_count = 0U;
				scenario.post.effects.size_before = 0U;
			},
			"zero/zero route accepted an absent pre-state");
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.stat.state = sqlite_writer_shm_entry_state::absent;
				scenario.post.stat.object_identity.reset();
				scenario.post.stat.directory_entry_identity.reset();
				scenario.post.stat.byte_count = 0U;
				scenario.post.effects.size_after = 0U;
			},
			"zero/zero route accepted an absent post-state");
		expect_determinate(
			[](auto& scenario)
			{
				scenario.pre.state = sqlite_writer_shm_entry_state::direct_regular;
				scenario.pre.object_identity =
					identity("test.mapping-semantics.unexpected-pre-object", 201U);
				scenario.pre.directory_entry_identity =
					identity("test.mapping-semantics.unexpected-pre-entry", 201U);
			},
			"create route accepted a direct pre-state",
			scenario_kind::created);
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.stat.state = sqlite_writer_shm_entry_state::absent;
				scenario.post.stat.object_identity.reset();
				scenario.post.stat.directory_entry_identity.reset();
				scenario.post.stat.byte_count = 0U;
				scenario.post.effects.size_after = 0U;
			},
			"create route accepted an absent post-state",
			scenario_kind::created);

		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.stat.object_identity =
					identity("test.mapping-semantics.replaced-object", 1U);
			},
			"direct route accepted object identity drift");
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.stat.directory_entry_identity =
					identity("test.mapping-semantics.replaced-entry", 1U);
			},
			"direct route accepted directory-entry identity drift");
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.stat.parent_namespace_identity =
					identity("test.mapping-semantics.replaced-parent", 1U);
			},
			"post-stat parent namespace drift escaped cleanup");
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.stat.filesystem_profile =
					identity("test.mapping-semantics.replaced-filesystem", 1U);
			},
			"post-stat filesystem-profile drift escaped cleanup");
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.stat.mount_identity =
					identity("test.mapping-semantics.replaced-mount", 1U);
			},
			"post-stat mount identity drift escaped cleanup");
	}

	void verify_malformed_stat_shapes_fail_at_the_genuine_boundary()
	{
		using stat_mutator = void (*)(sqlite_writer_shm_stat_census&);
		constexpr std::array<stat_mutator, 7> malformed_shapes{
			+[](sqlite_writer_shm_stat_census& stat)
			{
				stat.state = sqlite_writer_shm_entry_state::absent;
				stat.object_identity =
					identity("test.mapping-semantics.impossible-absent-object", 1U);
				stat.directory_entry_identity.reset();
				stat.byte_count = 0U;
			},
			+[](sqlite_writer_shm_stat_census& stat)
			{
				stat.state = sqlite_writer_shm_entry_state::absent;
				stat.object_identity.reset();
				stat.directory_entry_identity =
					identity("test.mapping-semantics.impossible-absent-entry", 1U);
				stat.byte_count = 0U;
			},
			+[](sqlite_writer_shm_stat_census& stat)
			{
				stat.state = sqlite_writer_shm_entry_state::absent;
				stat.object_identity.reset();
				stat.directory_entry_identity.reset();
				stat.byte_count = 1U;
			},
			+[](sqlite_writer_shm_stat_census& stat)
			{
				stat.object_identity.reset();
			},
			+[](sqlite_writer_shm_stat_census& stat)
			{
				stat.directory_entry_identity.reset();
			},
			+[](sqlite_writer_shm_stat_census& stat)
			{
				stat.object_identity = sqlite_backend_opaque_identity{};
			},
			+[](sqlite_writer_shm_stat_census& stat)
			{
				stat.directory_entry_identity = sqlite_backend_opaque_identity{};
			},
		};

		std::uint8_t marker = 205U;
		for (const auto mutate : malformed_shapes)
		{
			auto scenario = make_scenario(scenario_kind::unchanged, marker);
			mutate(scenario.pre);
			auto resources = make_epoch_resources(scenario.binding, marker);
			auto observation =
				std::make_shared<scripted_observation_port>(std::move(scenario.post));
			scripted_epoch_port port{
				scenario.epoch_identity, scenario.watch_receipt, scenario.pre, observation};
			auto activation = port.arm(resources.take_request());
			require(!activation &&
						activation.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch &&
						activation.error().action ==
							sqlite_shm_lease_recovery_action::deny_before_native_map &&
						port.calls == 1U && observation->calls == 0U,
					"malformed pre-stat shape crossed the pre-native epoch boundary");
			++marker;
		}

		for (const auto mutate : malformed_shapes)
		{
			auto scenario = make_scenario(scenario_kind::unchanged, marker);
			mutate(scenario.post.stat);
			auto resources = make_epoch_resources(scenario.binding, marker);
			auto observation =
				std::make_shared<scripted_observation_port>(std::move(scenario.post));
			scripted_epoch_port port{
				scenario.epoch_identity, scenario.watch_receipt, scenario.pre, observation};
			auto activation = port.arm(resources.take_request());
			require(activation.has_value(), "malformed post-stat fixture did not arm");
			auto arm = activation->take_arm();
			auto observer = activation->take_observer();
			int native_page{};
			auto receipt = seal_sqlite_writer_shm_mapping_epoch(observer, &native_page);
			require(!receipt &&
						receipt.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch &&
						receipt.error().action ==
							sqlite_shm_lease_recovery_action::
								attempt_nonremoving_unmap_then_outer_ioerr &&
						arm.valid() && port.calls == 1U && observation->calls == 1U,
					"malformed post-stat shape escaped exact post-native cleanup");
			++marker;
		}
	}

	void verify_size_and_range_matrix()
	{
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.effects.size_before.reset();
			},
			"missing size-before evidence was accepted");
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.effects.size_after.reset();
			},
			"missing size-after evidence was accepted");
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.effects.requested_range_end.reset();
			},
			"missing requested-range-end evidence was accepted");
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.effects.size_before = 4096U;
			},
			"wrong size-before evidence was accepted");
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.effects.size_after = 4096U;
			},
			"wrong size-after evidence was accepted");
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.effects.requested_range_end = 4096U;
			},
			"wrong requested-range-end evidence was accepted");
		expect_determinate(
			[](auto& scenario)
			{
				scenario.pre.byte_count = 4096U;
				scenario.post.stat.byte_count = 4096U;
				scenario.post.effects.size_before = 4096U;
				scenario.post.effects.size_after = 4096U;
			},
			"mapping range beyond sealed SHM size was accepted");
		expect_determinate(
			[](auto& scenario)
			{
				scenario.pre.byte_count = 8192U;
				scenario.post.effects.size_before = 8192U;
			},
			"growth route accepted a pre-size already covering the range",
			scenario_kind::grown);
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.stat.byte_count = 12288U;
				scenario.post.effects.size_after = 12288U;
			},
			"growth route accepted a post-size beyond the exact requested end",
			scenario_kind::grown);
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.stat.byte_count = 12288U;
				scenario.post.effects.size_after = 12288U;
			},
			"create route accepted a post-size beyond the exact requested end",
			scenario_kind::created);
	}

	void verify_request_domain_extremes_and_self_consistent_drift()
	{
		struct invalid_range_request
		{
			int page_number;
			int page_size;
		};
		constexpr std::array invalid_requests{
			invalid_range_request{-1, 4096},
			invalid_range_request{1, 0},
			invalid_range_request{1, -4096},
		};
		std::uint8_t marker = 220U;
		for (const auto request : invalid_requests)
		{
			auto scenario = make_scenario(scenario_kind::unchanged, marker);
			scenario.binding.map_request.page_number = request.page_number;
			scenario.binding.map_request.page_size = request.page_size;
			auto resources = make_epoch_resources(scenario.binding, marker);
			auto observation =
				std::make_shared<scripted_observation_port>(std::move(scenario.post));
			scripted_epoch_port port{
				scenario.epoch_identity, scenario.watch_receipt, scenario.pre, observation};
			auto activation = port.arm(resources.take_request());
			require(!activation &&
						activation.error().reason ==
							sqlite_shm_lease_rejection_reason::invalid_request &&
						activation.error().action ==
							sqlite_shm_lease_recovery_action::deny_before_native_map &&
						port.calls == 0U && observation->calls == 0U,
					"negative page or non-positive page size reached the native epoch port");
			++marker;
		}

		constexpr auto maximum_int = std::numeric_limits<int>::max();
		const auto maximum_page = static_cast<std::uint64_t>(maximum_int);
		const auto maximum_size = static_cast<std::uint64_t>(maximum_int);
		const auto maximum_offset = maximum_page * maximum_size;
		const auto maximum_end = maximum_offset + maximum_size;
		auto maximum_fixture =
			seal_genuine_scenario(scenario_kind::unchanged,
								  marker++,
								  [maximum_end](auto& scenario)
								  {
									  scenario.binding.map_request.page_number = maximum_int;
									  scenario.binding.map_request.page_size = maximum_int;
									  scenario.pre.byte_count = maximum_end;
									  scenario.post.stat.byte_count = maximum_end;
									  scenario.post.effects.size_before = maximum_end;
									  scenario.post.effects.size_after = maximum_end;
									  scenario.post.effects.requested_range_end = maximum_end;
								  });
		const auto maximum_result =
			validate_sqlite_writer_shm_mapping_semantics_for_audit(maximum_fixture.receipt);
		require(maximum_result &&
					maximum_result->route ==
						sqlite_writer_shm_mapping_semantic_route::zero_zero_preexisting_unchanged &&
					maximum_result->extend_pair == sqlite_shm_writer_extend_pair::zero_zero &&
					maximum_result->mapping ==
						sqlite_shm_mapping_tuple{maximum_int,
												 maximum_int,
												 maximum_offset,
												 maximum_size,
												 maximum_fixture.native_page.get(),
												 maximum_end},
				"INT_MAX page/size did not produce its checked exact mapping tuple");

		constexpr std::uint64_t drift = 4096U;
		auto drift_fixture = seal_genuine_scenario(scenario_kind::unchanged,
												   marker,
												   [](auto& scenario)
												   {
													   scenario.binding.map_request.page_number = 2;
													   scenario.pre.byte_count += drift;
													   scenario.post.stat.byte_count += drift;
													   *scenario.post.effects.size_before += drift;
													   *scenario.post.effects.size_after += drift;
													   *scenario.post.effects.requested_range_end +=
														   drift;
												   });
		const auto drift_result =
			validate_sqlite_writer_shm_mapping_semantics_for_audit(drift_fixture.receipt);
		require(drift_result &&
					drift_result->route ==
						sqlite_writer_shm_mapping_semantic_route::zero_zero_preexisting_unchanged &&
					drift_result->extend_pair == sqlite_shm_writer_extend_pair::zero_zero &&
					drift_result->mapping ==
						sqlite_shm_mapping_tuple{
							2, 4096, 8192U, 4096U, drift_fixture.native_page.get(), 12288U} &&
					drift_result->holder_specific_effect_receipt ==
						drift_fixture.receipt.post_observation().effects.effect_receipt,
				"self-consistent range/stat/effect drift depended on the baseline fixture");
	}

	void verify_every_bounded_namespace_and_effect_count()
	{
		using namespace_member =
			sqlite_writer_shm_bounded_count sqlite_writer_shm_namespace_event_census::*;
		constexpr std::array<namespace_member, 5> namespace_counts{
			&sqlite_writer_shm_namespace_event_census::expected_leaf_create,
			&sqlite_writer_shm_namespace_event_census::other_create,
			&sqlite_writer_shm_namespace_event_census::delete_event,
			&sqlite_writer_shm_namespace_event_census::move_event,
			&sqlite_writer_shm_namespace_event_census::other_relevant_event,
		};
		for (const auto member : namespace_counts)
		{
			expect_determinate(
				[member](auto& scenario)
				{
					scenario.post.namespace_events.*member = sqlite_writer_shm_bounded_count::one;
				},
				"single unexpected namespace event escaped determinate cleanup");
			expect_ambiguous(
				[member](auto& scenario)
				{
					scenario.post.namespace_events.*member =
						sqlite_writer_shm_bounded_count::multiple_or_overflow;
				},
				"overflowed namespace count escaped terminal quarantine");
		}

		using effect_member = sqlite_writer_shm_bounded_count sqlite_writer_shm_effect_census::*;
		constexpr std::array<effect_member, 6> effect_counts{
			&sqlite_writer_shm_effect_census::create_count,
			&sqlite_writer_shm_effect_census::initialize_count,
			&sqlite_writer_shm_effect_census::truncate_count,
			&sqlite_writer_shm_effect_census::extend_count,
			&sqlite_writer_shm_effect_census::delete_count,
			&sqlite_writer_shm_effect_census::resize_count,
		};
		for (const auto member : effect_counts)
		{
			expect_determinate(
				[member](auto& scenario)
				{
					scenario.post.effects.*member = sqlite_writer_shm_bounded_count::one;
				},
				"single unexpected effect escaped determinate cleanup");
			expect_ambiguous(
				[member](auto& scenario)
				{
					scenario.post.effects.*member =
						sqlite_writer_shm_bounded_count::multiple_or_overflow;
				},
				"overflowed effect count escaped terminal quarantine");
		}
	}

	void verify_watch_profile_and_ambiguity_flags()
	{
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.namespace_events.trusted_stat_watch_profile = false;
			},
			"untrusted stat/watch profile was accepted");

		using namespace_flag = bool sqlite_writer_shm_namespace_event_census::*;
		constexpr std::array<namespace_flag, 4> ambiguous_namespace_flags{
			&sqlite_writer_shm_namespace_event_census::watch_lost,
			&sqlite_writer_shm_namespace_event_census::queue_overflow,
			&sqlite_writer_shm_namespace_event_census::census_overflow,
			&sqlite_writer_shm_namespace_event_census::replacement_or_aba,
		};
		for (const auto member : ambiguous_namespace_flags)
			expect_ambiguous(
				[member](auto& scenario)
				{
					scenario.post.namespace_events.*member = true;
				},
				"namespace loss, overflow, or A-B-A escaped terminal quarantine");

		for (const auto mismatch_leaf : {false, true})
		{
			const auto marker = static_cast<std::uint8_t>(230U + (mismatch_leaf ? 1U : 0U));
			auto scenario = make_scenario(scenario_kind::unchanged, marker);
			if (mismatch_leaf)
				scenario.post.namespace_events.expected_shm_leaf =
					identity("test.mapping-semantics.wrong-leaf", marker);
			else
				scenario.post.namespace_events.watch_epoch =
					identity("test.mapping-semantics.wrong-watch", marker);
			auto resources = make_epoch_resources(scenario.binding, marker);
			auto observation =
				std::make_shared<scripted_observation_port>(std::move(scenario.post));
			scripted_epoch_port port{
				scenario.epoch_identity, scenario.watch_receipt, scenario.pre, observation};
			auto activation = port.arm(resources.take_request());
			require(activation.has_value(), "watch mismatch fixture did not arm");
			auto arm = activation->take_arm();
			auto observer = activation->take_observer();
			int native_page{};
			auto sealed = seal_sqlite_writer_shm_mapping_epoch(observer, &native_page);
			require(!sealed &&
						sealed.error().reason ==
							sqlite_shm_lease_rejection_reason::receipt_mismatch &&
						sealed.error().action ==
							sqlite_shm_lease_recovery_action::
								attempt_nonremoving_unmap_then_outer_ioerr &&
						arm.valid() && observation->calls == 1U,
					"watch/leaf binding mismatch minted a genuine epoch receipt");
		}
	}

	void verify_effect_completeness_outcomes_and_identities()
	{
		using identity_member = sqlite_backend_opaque_identity sqlite_writer_shm_effect_census::*;
		constexpr std::array<identity_member, 5> effect_identities{
			&sqlite_writer_shm_effect_census::sqlite_source_id,
			&sqlite_writer_shm_effect_census::callback_transcript,
			&sqlite_writer_shm_effect_census::wal_write_lock_receipt,
			&sqlite_writer_shm_effect_census::effect_gate_receipt,
			&sqlite_writer_shm_effect_census::effect_receipt,
		};
		for (const auto member : effect_identities)
			expect_determinate(
				[member](auto& scenario)
				{
					scenario.post.effects.*member = {};
				},
				"missing effect outcome identity was accepted");

		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.effects.complete = false;
			},
			"incomplete effect transcript was accepted");
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.effects.result_confirmed_success = false;
			},
			"unconfirmed effect result was accepted");
		expect_ambiguous(
			[](auto& scenario)
			{
				scenario.post.effects.outcome_unknown = true;
			},
			"unknown effect outcome escaped terminal quarantine");
		expect_ambiguous(
			[](auto& scenario)
			{
				scenario.post.effects.census_overflow = true;
			},
			"effect census overflow escaped terminal quarantine");
	}

	void verify_create_and_growth_exactness()
	{
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.effects.extend_count = sqlite_writer_shm_bounded_count::zero;
			},
			"growth route accepted a missing exact extend",
			scenario_kind::grown);
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.effects.create_count = sqlite_writer_shm_bounded_count::one;
			},
			"growth route accepted a create effect",
			scenario_kind::grown);
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.namespace_events.expected_leaf_create =
					sqlite_writer_shm_bounded_count::one;
			},
			"growth route accepted a leaf-create event",
			scenario_kind::grown);
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.effects.create_count = sqlite_writer_shm_bounded_count::zero;
			},
			"create route accepted a missing exact create effect",
			scenario_kind::created);
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.namespace_events.expected_leaf_create =
					sqlite_writer_shm_bounded_count::zero;
			},
			"create route accepted a missing exact leaf-create event",
			scenario_kind::created);
		expect_determinate(
			[](auto& scenario)
			{
				scenario.post.effects.extend_count = sqlite_writer_shm_bounded_count::one;
			},
			"create route accepted a separate extend effect",
			scenario_kind::created);
	}

	void verify_copied_receipt_is_audit_only_after_arm_expiry()
	{
		std::optional<sqlite_writer_shm_mapping_epoch_receipt> copied_receipt;
		std::array<std::weak_ptr<int>, 4> owners;
		std::shared_ptr<int> mapped_page;
		const volatile void* native_mapping{};
		{
			auto fixture = seal_genuine_scenario(scenario_kind::grown, 250U);
			owners = fixture.resources.owner_refs();
			mapped_page = fixture.native_page;
			native_mapping = mapped_page.get();
			copied_receipt.emplace(fixture.receipt);
			const auto live_audit =
				validate_sqlite_writer_shm_mapping_semantics_for_audit(*copied_receipt);
			require(live_audit && fixture.arm.valid() &&
						live_audit->route ==
							sqlite_writer_shm_mapping_semantic_route::one_one_preexisting_grown,
					"copied audit receipt changed semantics while its arm was live");
		}
		require(copied_receipt.has_value(), "audit receipt copy was not retained");
		require(mapped_page && native_mapping == mapped_page.get(),
				"test discarded the opaque mapping storage before audit validation");
		for (const auto& owner : owners)
			require(owner.expired(), "copyable audit receipt retained a native lifetime owner");

		const auto expired_audit =
			validate_sqlite_writer_shm_mapping_semantics_for_audit(*copied_receipt);
		require(expired_audit &&
					expired_audit->route ==
						sqlite_writer_shm_mapping_semantic_route::one_one_preexisting_grown &&
					expired_audit->extend_pair == sqlite_shm_writer_extend_pair::one_one &&
					expired_audit->mapping.native_mapping == native_mapping &&
					expired_audit->holder_specific_effect_receipt ==
						copied_receipt->post_observation().effects.effect_receipt,
				"audit-only classifier incorrectly treated an expired arm as authority or drift");
	}
} // namespace

int main()
{
	try
	{
		verify_four_positive_routes_and_output();
		verify_pair_matrix();
		verify_transition_matrix();
		verify_stat_state_identity_and_context_matrix();
		verify_malformed_stat_shapes_fail_at_the_genuine_boundary();
		verify_size_and_range_matrix();
		verify_request_domain_extremes_and_self_consistent_drift();
		verify_every_bounded_namespace_and_effect_count();
		verify_watch_profile_and_ambiguity_flags();
		verify_effect_completeness_outcomes_and_identities();
		verify_create_and_growth_exactness();
		verify_copied_receipt_is_audit_only_after_arm_expiry();
	}
	catch (const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
	return 0;
}
