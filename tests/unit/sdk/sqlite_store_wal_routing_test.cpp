#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk.hpp>

#include "../../../src/sdk/sqlite_backend_observation_internal.hpp"

#include "../../support/sqlite_store_fixture.hpp"

namespace cxxlens::sdk
{
	void set_sqlite_source_shm_symbols_available_for_testing(bool) noexcept;
	[[nodiscard]] bool sqlite_source_shm_map_event_read_lock_valid_for_testing(
		bool native_cantinit_heap_route,
		bool native_mapping_nonnull,
		std::uint32_t read_lock_index);
	[[nodiscard]] bool sqlite_source_shm_authentic_heap_trigger_valid_for_testing(
		bool mapped_event_precedes_cantinit, int cantinit_page, bool repeat_cantinit);
	[[nodiscard]] bool sqlite_source_shm_callback_epoch_binding_valid_for_testing(
		bool same_epoch_identity);
	[[nodiscard]] bool sqlite_active_wal_header_receipt_required_for_testing(
		bool initial_observation, std::uint32_t read_lock_index) noexcept;
	[[nodiscard]] bool sqlite_active_wal_shm_recheck_transition_valid_for_testing(
		bool same_object_identity,
		bool same_entry_identity,
		std::uint64_t before_byte_count,
		std::uint64_t after_byte_count) noexcept;
	[[nodiscard]] result<void> observe_sqlite_active_wal_prequalification_for_testing(
		const sqlite_backend_namespace_census& source_census,
		std::string_view logical_main_locator,
		std::size_t& qualification_call_count);
	[[nodiscard]] result<void> recheck_sqlite_source_shm_epoch_for_testing(
		const std::shared_ptr<sqlite_source_shm_target_namespace_epoch>& epoch,
		bool transaction_live,
		bool used_cantinit_heap_route,
		std::uint32_t read_lock_index);
	[[nodiscard]] result<void> finish_sqlite_source_shm_epoch_for_testing(
		const std::shared_ptr<sqlite_source_shm_target_namespace_epoch>& epoch,
		bool connection_close_confirmed);
	[[nodiscard]] result<void> validate_sqlite_source_shm_epoch_census_for_testing(
		const std::shared_ptr<sqlite_source_shm_target_namespace_epoch>& epoch,
		const sqlite_backend_namespace_census& source);
} // namespace cxxlens::sdk

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::test::sqlite_fixture;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	struct error_tuple
	{
		std::string_view code;
		std::string_view field;
		std::string_view detail;
	};

	template <class Value>
	void require_error(const sdk::result<Value>& actual,
					   const error_tuple expected,
					   const std::string_view label)
	{
		if (actual || actual.error().code != expected.code ||
			actual.error().field != expected.field || actual.error().detail != expected.detail)
		{
			std::cerr << label;
			if (actual)
				std::cerr << ": expected failure but operation succeeded";
			else
				std::cerr << ": actual=" << actual.error().code << '/' << actual.error().field
						  << '/' << actual.error().detail;
			std::cerr << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] sdk::relation_descriptor descriptor()
	{
		sdk::relation_descriptor value;
		value.id = "company.test.sqlite_wal_route.v1";
		value.name = "company.test.sqlite_wal_route";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.test.sqlite_wal_route/1";
		value.owner_namespace = "company.test";
		value.columns = {
			{"company.test.sqlite_wal_route.v1.key",
			 "key",
			 {sdk::scalar_kind::typed_id, "sqlite_wal_route_id", false},
			 true,
			 sdk::column_role::claim_key},
			{"company.test.sqlite_wal_route.v1.value",
			 "value",
			 {sdk::scalar_kind::utf8_string, {}, false},
			 true,
			 sdk::column_role::authoritative_payload},
		};
		value.key_columns = {"company.test.sqlite_wal_route.v1.key"};
		value.merge = sdk::merge_mode::set;
		value.descriptor_digest =
			*sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
								  value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] sdk::relation_engine engine()
	{
		sdk::relation_registry registry;
		require(registry.add(descriptor()).has_value(), "WAL route descriptor was rejected");
		auto built = registry.build("engine-sqlite-wal-route-test");
		require(built.has_value(), "WAL route engine was rejected");
		return std::move(*built);
	}

	[[nodiscard]] sdk::detached_row row(std::string key, std::string payload)
	{
		auto relation = descriptor();
		sdk::row_builder builder{relation};
		require(builder
					.set({relation.id, relation.columns[0].id, relation.columns[0].type},
						 sdk::detached_cell::typed("sqlite_wal_route_id", std::move(key)))
					.has_value(),
				"WAL route key was rejected");
		require(builder
					.set({relation.id, relation.columns[1].id, relation.columns[1].type},
						 sdk::detached_cell::utf8(std::move(payload)))
					.has_value(),
				"WAL route payload was rejected");
		auto finished = std::move(builder).finish();
		require(finished.has_value(), "WAL route row did not finish");
		return std::move(*finished);
	}

	constexpr std::string_view producer_digest =
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

	[[nodiscard]] sdk::claim make_claim(const sdk::relation_engine& value,
										std::string key,
										std::string payload)
	{
		sdk::observation observed{
			row(std::move(key), std::move(payload)),
			{"universe-wal-route", {"all"}},
			"company.test.sqlite-wal-route-canonical",
			{"company.test.sqlite-wal-route-provider", std::string{producer_digest}},
			{"sha256:9999999999999999999999999999999999999999999999999999999999999999"},
			"evidence:sqlite-wal-route",
			{"exact", "partition", "assumptions:none", {"schema_validated"}},
		};
		auto claim = sdk::make_assertion(value, std::move(observed));
		require(claim.has_value(), "WAL route claim was rejected");
		return std::move(*claim);
	}

	[[nodiscard]] sdk::partition_draft partition(const sdk::relation_engine& value,
											   const std::string_view payload)
	{
		sdk::partition_draft draft;
		draft.relation_descriptor_id = descriptor().id;
		draft.scope = "compile-unit-wal-route";
		draft.condition = {"universe-wal-route", {"all"}};
		draft.interpretation = "company.test.sqlite-wal-route-canonical";
		draft.producer_semantics = producer_digest;
		draft.precision_profile = "exact";
		draft.assumption_set_id = "assumptions-empty";
		draft.claims = {make_claim(value, "route:1", std::string{payload})};
		auto basis = sdk::claim_input_basis_digest(draft.claims.front().input_basis);
		require(basis.has_value(), "WAL route input-basis digest was rejected");
		draft.producer_input_basis_digest = std::move(*basis);
		draft.coverage = {{"compile-unit", "compile-unit-wal-route", "covered", ""}};
		return draft;
	}

	[[nodiscard]] sdk::snapshot_series_selector selector(const sdk::relation_engine& value)
	{
		return {
			"catalog-sqlite-wal-route",
			"stable",
			std::string{value.generation()},
			"universe-wal-route",
			std::string{value.registry_digest()},
			"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
			"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
		};
	}

	[[nodiscard]] sdk::snapshot_draft
	snapshot_draft(const sdk::relation_engine& value,
				   std::optional<std::string> parent = std::nullopt)
	{
		return {
			selector(value),
			{1U, 0U, 0U},
			"sha256:6666666666666666666666666666666666666666666666666666666666666666",
			std::move(parent),
		};
	}

	[[nodiscard]] sdk::snapshot_handle publish(sdk::snapshot_store& store,
											 const sdk::relation_engine& value,
											 std::optional<std::string> parent,
											 const std::string_view payload)
	{
		auto writer = store.begin(snapshot_draft(value, std::move(parent)));
		require(writer.has_value(), "WAL route writer did not begin");
		require(writer->stage(partition(value, payload)).has_value(),
				"WAL route partition did not stage");
		require(writer->validate().has_value(), "WAL route candidate did not validate");
		auto published = writer->publish();
		require(published.has_value(), "WAL route candidate did not publish");
		return std::move(*published);
	}

	struct current_identity
	{
		std::string snapshot;
		std::string publication;
		std::uint64_t sequence{};
	};

	[[nodiscard]] current_identity seed_current(const std::filesystem::path& path,
											  const sdk::relation_engine& value)
	{
		current_identity output;
		{
			auto store = sdk::open_sqlite_snapshot_store(path.string(), value);
			require(store.has_value(), "WAL route source Store was unavailable");
			auto published = publish(*store, value, std::nullopt, "seed");
			output = current_identity{std::string{published.id()},
								  published.publication().publication_id,
								  published.publication().sequence};
		}
		quiesce_wal_sidecars(path);
		return output;
	}

	void require_current(sdk::snapshot_store& store,
						 const sdk::relation_engine& value,
						 const current_identity& expected,
						 const std::string_view label)
	{
		auto current = store.current(selector(value));
		require(current && current->id() == expected.snapshot &&
				current->publication().publication_id == expected.publication &&
				current->publication().sequence == expected.sequence,
			label);
	}

	[[nodiscard]] std::filesystem::path wal_path(const std::filesystem::path& main)
	{
		return std::filesystem::path{main.string() + "-wal"};
	}

	[[nodiscard]] std::filesystem::path shm_path(const std::filesystem::path& main)
	{
		return std::filesystem::path{main.string() + "-shm"};
	}

	void write_file(const std::filesystem::path& path, const std::span<const unsigned char> bytes)
	{
		std::ofstream output{path, std::ios::binary | std::ios::trunc};
		require(static_cast<bool>(output), "WAL route fixture could not open a file for writing");
		output.write(reinterpret_cast<const char*>(bytes.data()),
					 static_cast<std::streamsize>(bytes.size()));
		output.close();
		require(static_cast<bool>(output), "WAL route fixture write failed");
	}

	enum class closed_wal_shape
	{
		zero_bytes,
		valid_header_without_commit,
		torn_header,
		invalid_header,
		valid_commit_with_torn_suffix,
	};

	void reshape_wal(const std::filesystem::path& main, const closed_wal_shape shape)
	{
		const auto path = wal_path(main);
		auto bytes = read_file(path);
		require(bytes.size() > 32U, "WAL route source did not contain a committed WAL");
		switch (shape)
		{
			case closed_wal_shape::zero_bytes:
				bytes.clear();
				break;
			case closed_wal_shape::valid_header_without_commit:
				bytes.resize(32U);
				break;
			case closed_wal_shape::torn_header:
				bytes.resize(17U);
				break;
			case closed_wal_shape::invalid_header:
				bytes.front() ^= 0xffU;
				break;
			case closed_wal_shape::valid_commit_with_torn_suffix:
				bytes.push_back(0x7fU);
				break;
		}
		write_file(path, bytes);
	}

	void copy_active_family(const std::filesystem::path& source,
							const std::filesystem::path& destination,
							const bool copy_wal,
							const bool copy_shm)
	{
		active_wal_sidecar_fixture active{source};
		require(std::filesystem::copy_file(active.path(), destination),
				"WAL route main copy failed");
		if (copy_wal)
			require(std::filesystem::copy_file(wal_path(active.path()), wal_path(destination)),
					"WAL route WAL copy failed");
		if (copy_shm)
			require(std::filesystem::copy_file(shm_path(active.path()), shm_path(destination)),
					"WAL route SHM copy failed");
		active.close();
	}

	void check_active_current_happy_path()
	{
		const auto value = engine();
		temporary_directory directory{"sqlite-store-active-wal-route"};
		const auto path = directory.path() / "active.sqlite";
		[[maybe_unused]] const auto expected = seed_current(path, value);

		active_wal_sidecar_fixture active{path};
		const auto before = capture_files(active.path());
		auto opened = sdk::open_sqlite_snapshot_store(active.path().string(), value);
#if defined(__linux__) && defined(F_OFD_SETLK)
		require(opened.has_value(), "active current WAL+SHM route was unavailable");
		require_current(*opened, value, expected, "active current WAL+SHM authority drifted");
		require(capture_files(active.path()) == before,
				"active current WAL+SHM route changed source bytes");
#else
		require_error(opened,
				  {"store.backend-unavailable",
				   "sqlite",
				   "source-shm-readonly-qualification"},
				  "active current WAL+SHM route did not report qualification unavailability");
		require(capture_files(active.path()) == before,
				"unavailable active current WAL+SHM route changed source bytes");
#endif

		// A mid-factory identity replacement must be injected after the source census and before
		// finish_private_read().  The public API has no deterministic barrier at that boundary;
		// sleep/thread races are deliberately not used as an oracle here.
	}

	void check_source_shm_read_lock_route_guard()
	{
		require(sdk::sqlite_source_shm_map_event_read_lock_valid_for_testing(
					true, false, 0U),
				"CANTINIT heap WAL-index route rejected read lock zero");
		require(!sdk::sqlite_source_shm_map_event_read_lock_valid_for_testing(
					 true, false, 1U),
				"CANTINIT heap WAL-index route accepted a nonzero read lock");
		require(sdk::sqlite_source_shm_map_event_read_lock_valid_for_testing(
					false, true, 1U),
				"READONLY mapped WAL-index route rejected its existing nonzero-lock rule");
		require(!sdk::sqlite_source_shm_map_event_read_lock_valid_for_testing(
					 false, false, 0U),
				"normalized READONLY/null was accepted as an authentic heap trigger");
		require(sdk::sqlite_source_shm_authentic_heap_trigger_valid_for_testing(
					false, 0, false),
				"authentic page-zero CANTINIT/null heap trigger was rejected");
		require(sdk::sqlite_source_shm_authentic_heap_trigger_valid_for_testing(
					false, 0, true),
				"repeated page-zero CANTINIT/null abandoned an established heap route");
		require(!sdk::sqlite_source_shm_authentic_heap_trigger_valid_for_testing(
					 false, 1, false),
				"non-page-zero CANTINIT/null was accepted as a heap trigger");
		require(!sdk::sqlite_source_shm_authentic_heap_trigger_valid_for_testing(
					 true, 0, false),
				"CANTINIT/null after a mapped event was accepted as a heap trigger");
	}

	struct epoch_test_state
	{
		std::size_t rechecks{};
		std::size_t retained_entry_calls{};
		std::size_t header_reads{};
		std::size_t finishes{};
		std::size_t destructors{};
		std::size_t namespace_object_opens{};
		std::size_t namespace_object_closes{};
		std::size_t logical_path_observations{};
	};

	[[nodiscard]] sdk::sqlite_backend_opaque_identity epoch_test_identity(
		const std::string_view label)
	{
		sdk::sqlite_backend_opaque_identity output{"test.sqlite-epoch-identity.v1", {}};
		for (const auto value : label)
			output.bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
		if (output.bytes.empty())
			output.bytes.push_back(std::byte{0U});
		return output;
	}

	class epoch_test_held_object final : public sdk::sqlite_backend_held_object
	{
	  public:
		epoch_test_held_object(const sdk::sqlite_backend_file_role role,
						   const std::string_view label,
						   epoch_test_state* state = nullptr)
			: role_{role}, object_{epoch_test_identity(std::string{label} + ".object")},
			  entry_{epoch_test_identity(std::string{label} + ".entry")},
			  filesystem_{epoch_test_identity("filesystem")},
			  mount_{epoch_test_identity("mount")}, state_{state}
		{
		}

		[[nodiscard]] sdk::sqlite_backend_file_role role() const noexcept override
		{
			return role_;
		}
		[[nodiscard]] const sdk::sqlite_backend_opaque_identity&
		object_identity() const noexcept override
		{
			return object_;
		}
		[[nodiscard]] const sdk::sqlite_backend_opaque_identity&
		directory_entry_identity() const noexcept override
		{
			return entry_;
		}
		[[nodiscard]] const std::optional<sdk::sqlite_backend_opaque_identity>&
		object_filesystem_profile() const noexcept override
		{
			return filesystem_;
		}
		[[nodiscard]] const std::optional<sdk::sqlite_backend_opaque_identity>&
		object_mount_identity() const noexcept override
		{
			return mount_;
		}
		[[nodiscard]] sdk::result<std::uint64_t> size() const override { return 0U; }
		[[nodiscard]] sdk::result<void>
		read_exact(std::uint64_t, std::span<std::byte> output) const override
		{
			if (state_ != nullptr)
				++state_->header_reads;
			return output.empty()
				? sdk::result<void>{}
				: sdk::result<void>{sdk::error{"test.epoch", "read", "unexpected"}};
		}
		[[nodiscard]] sdk::result<std::string> sha256() const override
		{
			return std::string{
				"sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"};
		}
		[[nodiscard]] sdk::result<std::shared_ptr<sdk::sqlite_backend_private_snapshot>>
		copy_exact(sdk::sqlite_backend_private_snapshot_builder&,
				   std::span<std::byte>) const override
		{
			return sdk::error{"test.epoch", "copy", "unexpected"};
		}
		[[nodiscard]] sdk::result<sdk::sqlite_backend_replacement_state>
		recheck_current_entry() const override
		{
			return sdk::sqlite_backend_replacement_state::exact_same_entry_and_object;
		}

	  private:
		sdk::sqlite_backend_file_role role_;
		sdk::sqlite_backend_opaque_identity object_;
		sdk::sqlite_backend_opaque_identity entry_;
		std::optional<sdk::sqlite_backend_opaque_identity> filesystem_;
		std::optional<sdk::sqlite_backend_opaque_identity> mount_;
		epoch_test_state* state_{};
	};

	[[nodiscard]] sdk::sqlite_backend_namespace_census
	epoch_test_census(epoch_test_state* state = nullptr)
	{
		sdk::sqlite_backend_namespace_census census;
		census.profile = "test.sqlite-epoch-census.v1";
		census.capability_token = epoch_test_identity("capability");
		census.parent_namespace_identity = epoch_test_identity("parent");
		constexpr std::array roles{
			sdk::sqlite_backend_file_role::main_database,
			sdk::sqlite_backend_file_role::write_ahead_log,
			sdk::sqlite_backend_file_role::shared_memory,
		};
		for (std::size_t index{}; index < roles.size(); ++index)
		{
			auto held = std::make_shared<epoch_test_held_object>(
				roles[index], "role-" + std::to_string(index), state);
			census.entries[index] = sdk::sqlite_backend_entry_observation{
				roles[index],
				sdk::sqlite_backend_entry_state::held_regular,
				held->object_identity(),
				held->directory_entry_identity(),
				held,
				held->object_filesystem_profile(),
				true,
			};
		}
		census.entries[3U] = sdk::sqlite_backend_entry_observation{
			sdk::sqlite_backend_file_role::rollback_journal,
			sdk::sqlite_backend_entry_state::absent,
			{},
			{},
			{},
			{},
			false,
		};
		return census;
	}

	class test_target_namespace_epoch final
		: public sdk::sqlite_source_shm_target_namespace_epoch
	{
	  public:
		explicit test_target_namespace_epoch(
			epoch_test_state& state,
			std::optional<sdk::sqlite_backend_namespace_census> census = std::nullopt)
			: state_{state}, census_{std::move(census)}
		{
			identity_.profile = "test.sqlite-source-shm-target-epoch.v1";
			identity_.bytes.push_back(std::byte{1U});
		}

		~test_target_namespace_epoch() override { ++state_.destructors; }

		[[nodiscard]] std::string_view logical_main_locator() const noexcept override
		{
			return "logical.sqlite";
		}

		[[nodiscard]] std::string_view anchored_main_locator() const noexcept override
		{
			return "anchored.sqlite";
		}

		[[nodiscard]] const sdk::sqlite_backend_opaque_identity&
		identity() const noexcept override
		{
			return identity_;
		}

		[[nodiscard]] sdk::result<sdk::sqlite_backend_entry_observation>
		retained_entry(const sdk::sqlite_backend_file_role role) const override
		{
			++state_.retained_entry_calls;
			if (census_)
				for (const auto& entry : census_->entries)
					if (entry.role == role)
						return entry;
			return sdk::error{"test.epoch", "sqlite", "unused-retained-entry"};
		}

		[[nodiscard]] sdk::result<void> recheck() const override
		{
			++state_.rechecks;
			return {};
		}

		[[nodiscard]] sdk::result<void> finish() override
		{
			++state_.finishes;
			return {};
		}

	  private:
		epoch_test_state& state_;
		std::optional<sdk::sqlite_backend_namespace_census> census_;
		sdk::sqlite_backend_opaque_identity identity_;
	};

	void check_target_namespace_epoch_read_close_order_guard()
	{
		constexpr error_tuple qualification_failure{
			"store.backend-unavailable",
			"sqlite",
			"source-shm-readonly-qualification",
		};
		epoch_test_state state;
		auto epoch = std::make_shared<test_target_namespace_epoch>(state);

		auto wrong_lock = sdk::recheck_sqlite_source_shm_epoch_for_testing(
			epoch, true, true, 1U);
		require_error(wrong_lock,
				  qualification_failure,
				  "heap epoch recheck accepted a nonzero WAL read lock");
		auto ended_transaction = sdk::recheck_sqlite_source_shm_epoch_for_testing(
			epoch, false, true, 0U);
		require_error(ended_transaction,
				  qualification_failure,
				  "epoch recheck accepted an ended read transaction");
		require(state.rechecks == 0U,
				"invalid live-read preconditions touched the retained namespace epoch");

		require(sdk::recheck_sqlite_source_shm_epoch_for_testing(epoch, true, true, 0U)
				.has_value(),
				"live heap read did not recheck the retained namespace epoch");
		require(state.rechecks == 1U && state.namespace_object_opens == 0U &&
				state.namespace_object_closes == 0U,
				"live epoch recheck reopened or closed a namespace object");

		auto unconfirmed_close =
			sdk::finish_sqlite_source_shm_epoch_for_testing(epoch, false);
		require_error(unconfirmed_close,
				  qualification_failure,
				  "epoch finish accepted an unconfirmed SQLite close");
		require(state.finishes == 0U && state.destructors == 0U,
				"unconfirmed SQLite close consumed or released the retained epoch");

		require(sdk::finish_sqlite_source_shm_epoch_for_testing(epoch, true).has_value(),
				"confirmed SQLite close did not finish the retained epoch");
		require(state.finishes == 1U && state.destructors == 0U,
				"epoch finish released the retained object before its owner did");
		epoch.reset();
		require(state.destructors == 1U,
				"retained epoch was not released after confirmed close and finish");
	}

	void check_source_shm_callback_epoch_binding_guard()
	{
		require(sdk::sqlite_source_shm_callback_epoch_binding_valid_for_testing(true),
				"matching callback target-epoch identity was rejected");
		require(!sdk::sqlite_source_shm_callback_epoch_binding_valid_for_testing(false),
				"callback receipt with the wrong target-epoch identity was accepted");
	}

	void check_active_wal_header_receipt_guard()
	{
		require(sdk::sqlite_active_wal_header_receipt_required_for_testing(true, 0U),
				"initial lock-zero route omitted its WAL header-and-salt receipt");
		require(sdk::sqlite_active_wal_header_receipt_required_for_testing(true, 1U),
				"initial lock-N route omitted its WAL header-and-salt receipt");
		require(!sdk::sqlite_active_wal_header_receipt_required_for_testing(false, 0U),
				"lock-zero final recheck incorrectly froze a resettable WAL header");
		require(sdk::sqlite_active_wal_header_receipt_required_for_testing(false, 1U),
				"lock-N final recheck omitted its exact WAL header comparison");
	}

	void check_retained_epoch_census_avoids_logical_path_reobservation()
	{
		epoch_test_state state;
		auto census = epoch_test_census();
		auto epoch = std::make_shared<test_target_namespace_epoch>(state, census);
		require(sdk::validate_sqlite_source_shm_epoch_census_for_testing(epoch, census)
				.has_value(),
				"retained epoch census did not match its sealed source entries");
		require(state.retained_entry_calls == 3U && state.logical_path_observations == 0U &&
				state.namespace_object_opens == 0U && state.namespace_object_closes == 0U,
				"active epoch census reopened or re-resolved a logical target pathname");

		auto wrong = census;
		wrong.entries[1U].object_identity = epoch_test_identity("wrong-wal-object");
		auto rejected =
			sdk::validate_sqlite_source_shm_epoch_census_for_testing(epoch, wrong);
		require_error(rejected,
				  {"store.backend-unavailable",
				   "sqlite",
				   "source-shm-readonly-qualification"},
				  "retained epoch census accepted a mismatched WAL object");
		require(state.logical_path_observations == 0U,
				"retained epoch mismatch fell back to logical-path observation");
	}

	void check_active_wal_shm_recheck_transition_guard()
	{
		require(sdk::sqlite_active_wal_shm_recheck_transition_valid_for_testing(
					true, true, 32'768U, 65'536U),
				"same-object active SHM extension was rejected");
		require(!sdk::sqlite_active_wal_shm_recheck_transition_valid_for_testing(
					 true, true, 65'536U, 32'768U),
				"active SHM shrink was accepted");
		require(!sdk::sqlite_active_wal_shm_recheck_transition_valid_for_testing(
					 false, true, 32'768U, 32'768U) &&
					!sdk::sqlite_active_wal_shm_recheck_transition_valid_for_testing(
						true, false, 32'768U, 32'768U),
				"active SHM object or directory-entry replacement was accepted");
	}

	void check_active_wal_direct_entry_gate_precedes_reads_and_qualification()
	{
		constexpr error_tuple qualification_failure{
			"store.backend-unavailable",
			"sqlite",
			"source-shm-readonly-qualification",
		};
		epoch_test_state state;
		auto symlink_observation = epoch_test_census(&state);
		for (std::size_t index{}; index < 3U; ++index)
			symlink_observation.entries[index].direct_regular_entry = false;
		std::size_t qualification_calls{};
		auto rejected = sdk::observe_sqlite_active_wal_prequalification_for_testing(
			symlink_observation, "logical.sqlite", qualification_calls);
		require_error(rejected,
				  qualification_failure,
				  "held-regular symlink observations passed the active direct-entry gate");
		require(state.header_reads == 0U && qualification_calls == 0U,
				"active symlink observations reached the header or qualification callback");
	}

	void check_active_branch_header_precedes_qualification()
	{
#if defined(__linux__) && defined(F_OFD_SETLK)
		const auto value = engine();
		temporary_directory directory{"sqlite-store-active-qualification-order"};
		const auto path = directory.path() / "qualification % order.sqlite";
		(void)seed_current(path, value);
		active_wal_sidecar_fixture active{path};
		auto bytes = read_file(active.path());
		require(bytes.size() >= 20U && bytes[18U] == 2U && bytes[19U] == 2U,
				"active qualification-order fixture was not WAL-mode authority");
		bytes[18U] = 1U;
		bytes[19U] = 1U;
		write_file(active.path(), bytes);
		const auto before = capture_files(active.path());

		sdk::set_sqlite_source_shm_symbols_available_for_testing(false);
		auto opened = sdk::open_sqlite_snapshot_store(active.path().string(), value);
		sdk::set_sqlite_source_shm_symbols_available_for_testing(true);
		require_error(opened,
				  {"store.sqlite-failure", "sqlite-journal-mode", "expected-wal"},
				  "non-WAL main header did not precede active-branch qualification");
		require(capture_files(active.path()) == before,
				"non-WAL active-pair classification changed the target source family");
#endif
	}

	void check_wal_only_eager_read_and_mutation_handoff()
	{
		const auto value = engine();
		temporary_directory directory{"sqlite-store-wal-only-route"};
		const auto source_path = directory.path() / "source.sqlite";
		const auto expected = seed_current(source_path, value);

		{
			auto cold = make_cold_wal_only_copy(source_path, directory.path() / "eager.sqlite");
			const auto before = capture_files(cold.path());
			{
				auto opened = sdk::open_sqlite_snapshot_store(cold.path().string(), value);
				require(opened.has_value(), "WAL-only eager route was unavailable");
				require_current(*opened, value, expected, "WAL-only eager authority drifted");
				require(capture_files(cold.path()) == before,
						"WAL-only eager open/read changed the source file family");
			}
			require(capture_files(cold.path()) == before,
					"WAL-only eager Store close changed the source file family");
		}

		{
			auto cold = make_cold_wal_only_copy(source_path, directory.path() / "mutation.sqlite");
			std::string next_publication;
			std::uint64_t next_sequence{};
			{
				auto opened = sdk::open_sqlite_snapshot_store(cold.path().string(), value);
				require(opened.has_value(), "WAL-only mutation route was unavailable");
				auto next = publish(*opened, value, expected.publication, "after-recovery");
				next_publication = next.publication().publication_id;
				next_sequence = next.publication().sequence;
				require(next_sequence == expected.sequence + 1U,
						"WAL-only first mutation did not perform one recovery handoff");
			}
			auto reopened = sdk::open_sqlite_snapshot_store(cold.path().string(), value);
			require(reopened.has_value(), "WAL-only mutation result did not reopen");
			auto current = reopened->current(selector(value));
			require(current && current->publication().publication_id == next_publication &&
					current->publication().sequence == next_sequence,
					"WAL-only first mutation was not durable");
		}

		{
			auto cold = make_cold_wal_only_copy(source_path, directory.path() / "drift.sqlite");
			auto opened = sdk::open_sqlite_snapshot_store(cold.path().string(), value);
			require(opened.has_value(), "WAL-only drift route was unavailable");
			auto writer = opened->begin(snapshot_draft(value, expected.publication));
			require(writer && writer->stage(partition(value, "must-not-publish")) &&
					writer->validate(),
					"WAL-only pre-effect drift candidate setup failed");
			auto bytes = read_file(wal_path(cold.path()));
			bytes.push_back(0x5aU);
			write_file(wal_path(cold.path()), bytes);
			const auto drifted_source = capture_files(cold.path());
			auto rejected = writer->publish();
			require_error(rejected,
					  {"store.sqlite-failure",
					   "sqlite-initialization-sidecar",
					   "concurrent-source-change"},
					  "WAL-only pre-effect drift tuple changed");
			require(capture_files(cold.path()) == drifted_source,
					"WAL-only pre-effect drift rejection wrote to the source family");
			require_current(*opened,
						value,
						expected,
						"WAL-only pre-effect drift discarded the eager authority");
		}
	}

	void check_closed_wal_classifier()
	{
		const auto value = engine();
		temporary_directory directory{"sqlite-store-closed-wal-route"};
		const auto source_path = directory.path() / "source.sqlite";
		const auto expected = seed_current(source_path, value);
		constexpr error_tuple unrecognized{
			"store.sqlite-failure",
			"sqlite-initialization-sidecar",
			"unrecognized-preauthority-state",
		};
		struct route_case
		{
			std::string_view label;
			closed_wal_shape shape;
			bool accepted;
		};
		constexpr std::array cases{
			route_case{"zero-byte WAL", closed_wal_shape::zero_bytes, true},
			route_case{"valid header without commit",
					   closed_wal_shape::valid_header_without_commit,
					   true},
			route_case{"torn WAL header", closed_wal_shape::torn_header, false},
			route_case{"invalid WAL header", closed_wal_shape::invalid_header, false},
			route_case{"valid commit with torn suffix",
					   closed_wal_shape::valid_commit_with_torn_suffix,
					   true},
		};

		for (std::size_t index{}; index < cases.size(); ++index)
		{
			const auto destination =
				directory.path() / ("closed-" + std::to_string(index) + ".sqlite");
			auto cold = make_cold_wal_only_copy(source_path, destination);
			reshape_wal(cold.path(), cases[index].shape);
			const auto before = capture_files(cold.path());
			auto opened = sdk::open_sqlite_snapshot_store(cold.path().string(), value);
			if (cases[index].accepted)
			{
				require(opened.has_value(),
						std::string{cases[index].label} + " route was rejected");
				require_current(*opened,
							value,
							expected,
							std::string{cases[index].label} + " changed main-only authority");
			}
			else
				require_error(opened,
						  unrecognized,
						  std::string{cases[index].label} + " rejection tuple changed");
			require(capture_files(cold.path()) == before,
					std::string{cases[index].label} + " route changed the source file family");
		}
	}

	void check_closed_sidecar_topology()
	{
		const auto value = engine();
		temporary_directory directory{"sqlite-store-sidecar-topology"};
		const auto source_path = directory.path() / "source.sqlite";
		const auto expected = seed_current(source_path, value);

		{
			const auto destination = directory.path() / "shm-only.sqlite";
			copy_active_family(source_path, destination, false, true);
			const auto before = capture_files(destination);
			auto opened = sdk::open_sqlite_snapshot_store(destination.string(), value);
			require_error(opened,
					  {"store.sqlite-failure",
					   "sqlite-sidecar-state",
					   "incomplete-wal-shm-pair"},
					  "SHM-only topology tuple changed");
			require(capture_files(destination) == before,
					"SHM-only topology rejection changed the source family");
		}

		{
			const auto destination = directory.path() / "unreadable-shm.sqlite";
			copy_active_family(source_path, destination, true, true);
			require(std::filesystem::remove(shm_path(destination)),
					"unreadable SHM fixture removal failed");
			std::filesystem::create_symlink("missing-shm-target", shm_path(destination));
			auto opened = sdk::open_sqlite_snapshot_store(destination.string(), value);
			require_error(opened,
					  {"store.sqlite-failure",
					   "sqlite-sidecar-state",
					   "unreadable-wal-shm-pair"},
					  "unreadable SHM topology tuple changed");
		}

		{
			auto cold =
				make_cold_wal_only_copy(source_path, directory.path() / "unreadable-wal.sqlite");
			require(std::filesystem::remove(wal_path(cold.path())),
					"unreadable WAL fixture removal failed");
			std::filesystem::create_symlink("missing-wal-target", wal_path(cold.path()));
			auto opened = sdk::open_sqlite_snapshot_store(cold.path().string(), value);
			require_error(opened,
					  {"store.sqlite-failure",
					   "sqlite-initialization-sidecar",
					   "unreadable-wal-only"},
					  "unreadable WAL-only topology tuple changed");
		}

		{
			auto cold =
				make_cold_wal_only_copy(source_path, directory.path() / "missing-shm.sqlite");
			const auto before = capture_files(cold.path());
			auto opened = sdk::open_sqlite_snapshot_store(cold.path().string(), value);
			require(opened.has_value(), "missing SHM did not select the WAL-only route");
			require_current(*opened, value, expected, "missing SHM changed WAL-only authority");
			require(capture_files(cold.path()) == before,
					"missing SHM WAL-only route changed the source family");
		}
	}
} // namespace

int main()
{
	check_source_shm_read_lock_route_guard();
	check_source_shm_callback_epoch_binding_guard();
	check_active_wal_header_receipt_guard();
	check_retained_epoch_census_avoids_logical_path_reobservation();
	check_target_namespace_epoch_read_close_order_guard();
	check_active_wal_shm_recheck_transition_guard();
	check_active_wal_direct_entry_gate_precedes_reads_and_qualification();
	check_active_branch_header_precedes_qualification();
	check_active_current_happy_path();
	check_wal_only_eager_read_and_mutation_handoff();
	check_closed_wal_classifier();
	check_closed_sidecar_topology();
	return 0;
}
