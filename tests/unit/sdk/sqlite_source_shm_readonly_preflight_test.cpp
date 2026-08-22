#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "sdk/sqlite_default_forwarding_vfs_internal.hpp"
#include "sdk/sqlite_same_process_shm_reader_lifecycle_internal.hpp"
#include "sdk/sqlite_source_shm_readonly_preflight_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << "FAIL: " << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] sqlite_backend_opaque_identity active_read_identity(const std::string_view label)
	{
		sqlite_backend_opaque_identity output{"test.active-read.identity.v1", {}};
		for (const auto byte : label)
			output.bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		return output;
	}

	class active_read_held_object final : public sqlite_backend_held_object
	{
	  public:
		active_read_held_object(const sqlite_backend_file_role role,
								const std::string_view label,
								const sqlite_backend_opaque_identity& filesystem,
								const sqlite_backend_opaque_identity& mount)
			: role_{role}, object_{active_read_identity(std::string{label} + ".object")},
			  entry_{active_read_identity(std::string{label} + ".entry")}, filesystem_{filesystem},
			  mount_{mount}
		{
		}

		[[nodiscard]] sqlite_backend_file_role role() const noexcept override
		{
			return role_;
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
			return retained_recheck_ok ? result<void>{}
									   : unexpected(active_read_error("retained-recheck"));
		}

		[[nodiscard]] result<std::uint64_t> size() const override
		{
			return 4096U;
		}

		[[nodiscard]] result<void> read_exact(const std::uint64_t,
											  const std::span<std::byte>) const override
		{
			return {};
		}

		[[nodiscard]] result<std::string> sha256() const override
		{
			return std::string{"sha256:"} + std::string(64U, 'a');
		}

		[[nodiscard]] result<std::shared_ptr<sqlite_backend_private_snapshot>>
		copy_exact(sqlite_backend_private_snapshot_builder&,
				   const std::span<std::byte>) const override
		{
			return unexpected(active_read_error("unexpected-copy"));
		}

		[[nodiscard]] result<sqlite_backend_replacement_state>
		recheck_current_entry() const override
		{
			return replacement;
		}

		bool retained_recheck_ok{true};
		sqlite_backend_replacement_state replacement{
			sqlite_backend_replacement_state::exact_same_entry_and_object};

	  private:
		static error active_read_error(const std::string_view detail)
		{
			return {"test.failure", "sqlite-active-read", std::string{detail}};
		}

		sqlite_backend_file_role role_{};
		sqlite_backend_opaque_identity object_;
		sqlite_backend_opaque_identity entry_;
		std::optional<sqlite_backend_opaque_identity> filesystem_;
		std::optional<sqlite_backend_opaque_identity> mount_;
	};

	class active_read_namespace_guard final : public sqlite_source_shm_namespace_guard
	{
	  public:
		active_read_namespace_guard(std::string locator, sqlite_backend_opaque_identity identity)
			: locator_{std::move(locator)}, identity_{std::move(identity)}
		{
		}

		[[nodiscard]] std::string_view logical_main_locator() const noexcept override
		{
			return locator_;
		}

		[[nodiscard]] std::string_view anchored_main_locator() const noexcept override
		{
			return anchored_;
		}

		[[nodiscard]] const sqlite_backend_opaque_identity& identity() const noexcept override
		{
			return identity_;
		}

		[[nodiscard]] result<sqlite_backend_entry_observation>
		retained_entry(const sqlite_backend_file_role) const override
		{
			return unexpected(
				error{"test.failure", "sqlite-active-read", "unexpected-retained-entry"});
		}

		[[nodiscard]] result<void> recheck() const override
		{
			return recheck_ok
				? result<void>{}
				: unexpected(error{"test.failure", "sqlite-active-read", "guard-recheck"});
		}

		[[nodiscard]] result<void> claim_target_epoch() override
		{
			return {};
		}

		[[nodiscard]] result<void> finish() override
		{
			return {};
		}

		std::string anchored_{"/proc/self/fd/active-read/main.db"};
		bool recheck_ok{true};

	  private:
		std::string locator_;
		sqlite_backend_opaque_identity identity_;
	};

	class active_read_fixture
	{
	  public:
		active_read_fixture()
		{
			const auto filesystem = active_read_identity("filesystem");
			const auto mount = active_read_identity("mount");
			main = std::make_shared<active_read_held_object>(
				sqlite_backend_file_role::main_database, "main", filesystem, mount);
			wal = std::make_shared<active_read_held_object>(
				sqlite_backend_file_role::write_ahead_log, "wal", filesystem, mount);
			shm = std::make_shared<active_read_held_object>(
				sqlite_backend_file_role::shared_memory, "shm", filesystem, mount);
			const auto parent = active_read_identity("parent");
			guard = std::make_shared<active_read_namespace_guard>(
				path, active_read_identity("continuous-guard"));
			request.canonical_vfs_locator = path;
			request.source_census.profile = "default-filesystem-v1";
			request.source_census.capability_token = active_read_identity("capability");
			request.source_census.parent_namespace_identity = parent;
			request.source_census.source_shm_guard = guard;
			request.source_census.entries = {
				make_entry(sqlite_backend_file_role::main_database, main),
				make_entry(sqlite_backend_file_role::write_ahead_log, wal),
				make_entry(sqlite_backend_file_role::shared_memory, shm),
				make_absent(sqlite_backend_file_role::rollback_journal),
			};

			int runtime_identity{};
			int runtime_image_identity{};
			runtime_identity_ = &runtime_identity;
			runtime_image_identity_ = &runtime_image_identity;
			runtime_lifetime_ = std::make_shared<int>(1);
			request.runtime.runtime_identity = runtime_identity_;
			request.runtime.runtime_image_identity = runtime_image_identity_;
			request.runtime.runtime_lifetime_identity = runtime_lifetime_.get();
			request.runtime.runtime_lifetime = runtime_lifetime_;
			request.runtime.open_v2 = &fake_open;
			request.runtime.close_v2 = &fake_close;
			request.runtime.exec = &fake_exec;
			request.runtime.errmsg = &fake_errmsg;
			request.runtime.free_memory = &fake_free;
			request.runtime.source_id = &fake_source_id;
			request.runtime.uri_parameter = &fake_uri_parameter;
			request.runtime.uri_key = &fake_uri_key;
			request.runtime.vfs_find = &fake_vfs_find;
			request.runtime.vfs_register = &fake_vfs_register;
			request.runtime.vfs_unregister = &fake_vfs_unregister;
			request.forwarding_vfs_identity = &forwarding_vfs_identity_;
			request.pinned_underlying_vfs_identity = &underlying_vfs_identity_;
			request.pinned_underlying_vfs_app_data_identity = &underlying_app_data_identity_;
			request.runtime_epoch = active_read_identity("runtime-epoch");
			request.vfs_epoch = active_read_identity("vfs-epoch");
			request.process_instance = active_read_identity("process");
			request.fork_generation = active_read_identity("fork");
			request.outer_custody = active_read_identity("outer-custody");
			request.pre_effect = sqlite_active_read_pre_effect_census{
				true,  // source_family_complete
				true,  // source_family_unchanged
				false, // watch_loss_or_overflow_observed
				false, // runtime_drift_observed
				false, // vfs_drift_observed
				false, // process_drift_observed
				false, // fork_drift_observed
				false, // unload_requested
				false, // late_callback_observed
				false, // nested_mapping_started
				false, // create_observed
				false, // write_observed
				false, // truncate_observed
				false, // extend_observed
				false, // delete_observed
				false, // resize_observed
			};
			request.connection.profile = "default-filesystem-v1";
			request.connection.capability_token = request.source_census.capability_token;
			request.connection.connection_token = active_read_identity("connection");
			request.connection_custody = request.connection.connection_token;
			request.connection.complete = true;
			request.connection.main_handle_open = true;
			constexpr int main_flags =
				0x00000001 | 0x00000040 | 0x00000100 | 0x00010000 | 0x00040000;
			request.connection.open_events = {
				{sqlite_backend_file_role::main_database,
				 main_flags,
				 sqlite_backend_open_outcome::succeeded,
				 main_flags,
				 main->object_identity(),
				 main->directory_entry_identity()},
				{sqlite_backend_file_role::write_ahead_log,
				 0x00000002 | 0x00000004 | 0x00080000,
				 sqlite_backend_open_outcome::succeeded,
				 0x00000001 | 0x00080000,
				 wal->object_identity(),
				 wal->directory_entry_identity()},
			};
			request.connection.shared_memory_object_identity = shm->object_identity();
			request.connection.shared_memory_entry_identity = shm->directory_entry_identity();
			request.connection.source_shm_open_callback_receipt =
				sqlite_source_shm_open_callback_receipt{
					"sqlite-source-shm-readonly-unix-uri-v1",
					request.connection.connection_token,
					active_read_identity("qualification"),
					active_read_identity("target-namespace-epoch"),
					path,
					std::string{guard->anchored_main_locator()},
					"file:%2Ftmp%2Factive-read%2Fmain.db?mode=ro&cache=private&readonly_shm=1",
					"cxxlens-test-forwarding-vfs",
					"ro",
					"private",
					"1",
					main_flags,
					request.runtime.runtime_identity,
					request.forwarding_vfs_identity,
					request.pinned_underlying_vfs_identity,
					request.pinned_underlying_vfs_app_data_identity,
				};
		}

		[[nodiscard]] static sqlite_backend_entry_observation
		make_entry(const sqlite_backend_file_role role,
				   const std::shared_ptr<active_read_held_object>& object)
		{
			return {role,
					sqlite_backend_entry_state::held_regular,
					object->object_identity(),
					object->directory_entry_identity(),
					object,
					object->object_filesystem_profile().value(),
					true};
		}

		[[nodiscard]] static sqlite_backend_entry_observation
		make_absent(const sqlite_backend_file_role role)
		{
			return {role, sqlite_backend_entry_state::absent, {}, {}, {}, {}, false};
		}

		std::string path{"/tmp/active-read/main.db"};
		std::shared_ptr<active_read_held_object> main;
		std::shared_ptr<active_read_held_object> wal;
		std::shared_ptr<active_read_held_object> shm;
		std::shared_ptr<active_read_namespace_guard> guard;
		sqlite_active_read_connection_request request;

	  private:
		static int fake_open(const char*, void**, int, const char*)
		{
			return 0;
		}
		static int fake_close(void*)
		{
			return 0;
		}
		static int fake_exec(
			void*, const char*, sqlite_source_shm_runtime_binding::exec_callback, void*, char**)
		{
			return 0;
		}
		static const char* fake_errmsg(void*)
		{
			return "";
		}
		static void fake_free(void*) {}
		static const char* fake_source_id()
		{
			return "test-sqlite";
		}
		static const char* fake_uri_parameter(const char*, const char*)
		{
			return nullptr;
		}
		static const char* fake_uri_key(const char*, int)
		{
			return nullptr;
		}
		static void* fake_vfs_find(const char*)
		{
			return nullptr;
		}
		static int fake_vfs_register(void*, int)
		{
			return 0;
		}
		static int fake_vfs_unregister(void*)
		{
			return 0;
		}

		const void* runtime_identity_{};
		const void* runtime_image_identity_{};
		std::shared_ptr<void> runtime_lifetime_;
		int forwarding_vfs_identity_{};
		int underlying_vfs_identity_{};
		int underlying_app_data_identity_{};
	};

#if defined(__linux__) && defined(F_OFD_SETLK)
	class scratch_family
	{
	  public:
		scratch_family()
		{
			std::array pattern{'/', 't', 'm', 'p', '/', 'c', 'x', 'x', 'l', 'e', 'n', 's',
							   '-', 's', 'h', 'm', '-', 'p', 'r', 'e', 'f', 'l', 'i', 'g',
							   'h', 't', '-', 'X', 'X', 'X', 'X', 'X', 'X', '\0'};
			auto* created = ::mkdtemp(pattern.data());
			require(created != nullptr, "create scratch family directory");
			path_ = created;
			directory_ = ::open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			require(directory_ >= 0, "open scratch family directory");
			for (const auto* name : {"main.db", "main.db-wal", "main.db-shm"})
				create(name);
		}

		scratch_family(const scratch_family&) = delete;
		scratch_family& operator=(const scratch_family&) = delete;

		~scratch_family()
		{
			if (directory_ >= 0)
			{
				for (const auto* name : {"main.db", "main.db-wal", "main.db-shm", "extra"})
					(void)::unlinkat(directory_, name, 0);
				(void)::close(directory_);
			}
			if (!path_.empty())
				(void)::rmdir(path_.c_str());
		}

		[[nodiscard]] int descriptor() const noexcept
		{
			return directory_;
		}

		void create(const char* name) const
		{
			const auto descriptor = ::openat(
				directory_, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
			require(descriptor >= 0, "create scratch family member");
			require(::close(descriptor) == 0, "close scratch family member");
		}

	  private:
		std::string path_;
		int directory_{-1};
	};

	void exercise_repeated_exact_census()
	{
		scratch_family family;
		require(validate_sqlite_source_shm_readonly_scratch_family(family.descriptor()).has_value(),
				"first exact family census");
		require(validate_sqlite_source_shm_readonly_scratch_family(family.descriptor()).has_value(),
				"second exact family census uses a fresh open-file description");

		family.create("extra");
		require(!validate_sqlite_source_shm_readonly_scratch_family(family.descriptor()),
				"extra scratch entry rejected");
		require(::unlinkat(family.descriptor(), "extra", 0) == 0, "remove extra scratch entry");
		require(::unlinkat(family.descriptor(), "main.db-wal", 0) == 0,
				"remove required scratch entry");
		require(!validate_sqlite_source_shm_readonly_scratch_family(family.descriptor()),
				"missing scratch entry rejected");
		family.create("main.db-wal");
		require(validate_sqlite_source_shm_readonly_scratch_family(family.descriptor()).has_value(),
				"restored exact family accepted");
	}
#endif

	void exercise_strict_uri()
	{
		auto uri = make_sqlite_source_shm_readonly_uri("/tmp/A b?#%~_.-");
		require(uri.has_value() &&
					*uri ==
						"file:%2Ftmp%2FA%20b%3F%23%25~_.-"
						"?mode=ro&cache=private&readonly_shm=1",
				"strict uppercase-percent-encoded URI");

		std::string non_ascii{"/tmp/"};
		non_ascii.push_back(static_cast<char>(0x80));
		auto encoded_non_ascii = make_sqlite_source_shm_readonly_uri(non_ascii);
		require(encoded_non_ascii.has_value() && encoded_non_ascii->contains("%80"),
				"URI encodes bytes without signed-char drift");
		require(!make_sqlite_source_shm_readonly_uri("relative/path"), "relative path rejected");

		const std::array embedded_nul{'/', 't', 'm', 'p', '/', 'x', '\0', 'y'};
		require(!make_sqlite_source_shm_readonly_uri(
					std::string_view{embedded_nul.data(), embedded_nul.size()}),
				"embedded NUL rejected");
	}

	void exercise_branch_local_capability_absence()
	{
		require(!sqlite_source_shm_native_ok_projection_production_activation_enabled(),
				"native SQLITE_OK source-SHM projection remains disabled until qualification");
		auto optional_port = make_sqlite_source_shm_readonly_preflight(
			sqlite_default_observation_binding{}, sqlite_backend_opaque_identity{});
		require(optional_port.has_value() && !*optional_port,
				"missing active-WAL callback dependency leaves baseline observation available");
	}

	void exercise_active_read_connection_receipt()
	{
		active_read_fixture fixture;
		auto receipt = validate_sqlite_active_read_connection(fixture.request);
		require(receipt.has_value(), "authenticated active-read connection preflight");
		require(receipt->contract == "cxxlens.sqlite-active-read-connection.v1",
				"active-read receipt contract");
		require(receipt->phase ==
					detail::sqlite_active_read_connection_phase::active_read_connection,
				"active-read receipt terminates at active connection");
		require(receipt->source_namespace_guard == fixture.guard,
				"active-read receipt retains the source namespace guard");
		require(receipt->source_guard_identity == fixture.guard->identity(),
				"active-read receipt binds the continuous guard identity");
		require(receipt->connection.shm_map_events.empty() &&
					receipt->connection.held_shm_locks.empty(),
				"active-read receipt is sealed before the first SHM map/lock");
		require(receipt->pre_effect.source_family_unchanged && !receipt->pre_effect.write_observed,
				"active-read receipt retains the authenticated pre-effect census");
		require(detail::is_sqlite_active_read_connection_transition(
					detail::sqlite_active_read_connection_phase::outer_custody_open,
					detail::sqlite_active_read_connection_phase::active_read_connection),
				"outer custody opens before active-read connection is sealed");
		require(!detail::is_sqlite_active_read_connection_transition(
					detail::sqlite_active_read_connection_phase::active_read_connection,
					detail::sqlite_active_read_connection_phase::unopened),
				"active-read connection cannot rewind to unopened");

		auto mutated = fixture.request;
		mutated.pre_effect.write_observed = true;
		require(!validate_sqlite_active_read_connection(mutated),
				"pre-effect write evidence rejects the active-read product");
		mutated = fixture.request;
		mutated.connection.shm_map_events.push_back(sqlite_backend_shm_map_observation{});
		require(!validate_sqlite_active_read_connection(mutated),
				"active-read product cannot be sealed after a first map");
		mutated = fixture.request;
		mutated.pre_effect.nested_mapping_started = true;
		require(!validate_sqlite_active_read_connection(mutated),
				"nested mapping cannot be smuggled into the active-read product");
		mutated = fixture.request;
		mutated.connection.open_events.front().input_flags |= 0x00000002;
		require(!validate_sqlite_active_read_connection(mutated),
				"read-write main open is rejected before active-read custody");
		mutated = fixture.request;
		mutated.connection.profile = "different-profile";
		require(!validate_sqlite_active_read_connection(mutated),
				"connection profile drift is rejected before active-read custody");
		mutated = fixture.request;
		mutated.connection.source_shm_open_callback_receipt->delegated_vfs_locator = fixture.path;
		require(
			!validate_sqlite_active_read_connection(mutated),
			"callback host-path delegation is rejected instead of bypassing the retained parent");
		mutated = fixture.request;
		mutated.connection.source_shm_open_callback_receipt->application_generated_uri =
			"file:/tmp/active-read/main.db?mode=ro&cache=private&readonly_shm=1";
		require(!validate_sqlite_active_read_connection(mutated),
				"noncanonical callback URI is rejected before active-read custody");
		mutated = fixture.request;
		mutated.source_census.entries[1].state = sqlite_backend_entry_state::absent;
		require(!validate_sqlite_active_read_connection(mutated),
				"missing WAL is not classified as an empty active read");
		mutated = fixture.request;
		fixture.wal->replacement = sqlite_backend_replacement_state::replaced;
		require(!validate_sqlite_active_read_connection(mutated),
				"source-family replacement fails closed");
		fixture.wal->replacement = sqlite_backend_replacement_state::exact_same_entry_and_object;
		for (const auto forbidden :
			 {&sqlite_active_read_pre_effect_census::watch_loss_or_overflow_observed,
			  &sqlite_active_read_pre_effect_census::runtime_drift_observed,
			  &sqlite_active_read_pre_effect_census::vfs_drift_observed,
			  &sqlite_active_read_pre_effect_census::process_drift_observed,
			  &sqlite_active_read_pre_effect_census::fork_drift_observed,
			  &sqlite_active_read_pre_effect_census::unload_requested,
			  &sqlite_active_read_pre_effect_census::late_callback_observed})
		{
			mutated = fixture.request;
			mutated.pre_effect.*forbidden = true;
			require(!validate_sqlite_active_read_connection(mutated),
					"runtime/lifetime revocation evidence fails closed before active-read custody");
		}
	}

	void exercise_active_read_identity_and_pre_effect_matrix()
	{
		active_read_fixture fixture;

		using identity_member =
			sqlite_backend_opaque_identity sqlite_active_read_connection_request::*;
		constexpr std::array custody_identities{
			identity_member{&sqlite_active_read_connection_request::runtime_epoch},
			identity_member{&sqlite_active_read_connection_request::vfs_epoch},
			identity_member{&sqlite_active_read_connection_request::process_instance},
			identity_member{&sqlite_active_read_connection_request::fork_generation},
			identity_member{&sqlite_active_read_connection_request::connection_custody},
			identity_member{&sqlite_active_read_connection_request::outer_custody},
		};
		for (const auto member : custody_identities)
		{
			auto mutated = fixture.request;
			mutated.*member = {};
			require(!validate_sqlite_active_read_connection(mutated),
					"missing runtime, process, fork, or custody identity fails closed");
		}

		using census_flag = bool sqlite_active_read_pre_effect_census::*;
		constexpr std::array forbidden_flags{
			census_flag{&sqlite_active_read_pre_effect_census::watch_loss_or_overflow_observed},
			census_flag{&sqlite_active_read_pre_effect_census::runtime_drift_observed},
			census_flag{&sqlite_active_read_pre_effect_census::vfs_drift_observed},
			census_flag{&sqlite_active_read_pre_effect_census::process_drift_observed},
			census_flag{&sqlite_active_read_pre_effect_census::fork_drift_observed},
			census_flag{&sqlite_active_read_pre_effect_census::unload_requested},
			census_flag{&sqlite_active_read_pre_effect_census::late_callback_observed},
			census_flag{&sqlite_active_read_pre_effect_census::nested_mapping_started},
			census_flag{&sqlite_active_read_pre_effect_census::create_observed},
			census_flag{&sqlite_active_read_pre_effect_census::write_observed},
			census_flag{&sqlite_active_read_pre_effect_census::truncate_observed},
			census_flag{&sqlite_active_read_pre_effect_census::extend_observed},
			census_flag{&sqlite_active_read_pre_effect_census::delete_observed},
			census_flag{&sqlite_active_read_pre_effect_census::resize_observed},
		};
		for (const auto member : forbidden_flags)
		{
			auto mutated = fixture.request;
			mutated.pre_effect.*member = true;
			require(!validate_sqlite_active_read_connection(mutated),
					"every pre-map effect, drift, unload, and nested-map marker fails closed");
		}

		for (const auto member : {&sqlite_active_read_pre_effect_census::source_family_complete,
								  &sqlite_active_read_pre_effect_census::source_family_unchanged})
		{
			auto mutated = fixture.request;
			mutated.pre_effect.*member = false;
			require(!validate_sqlite_active_read_connection(mutated),
					"incomplete or changed source-family census fails closed");
		}
	}

	void exercise_outer_read_phase_order()
	{
		// DF-0201 scope is state/test-only; this does not authorize runtime binding or activation.
		using phase = detail::sqlite_shm_reader_outer_read_phase;
		constexpr std::array complete{
			phase::unresolved,
			phase::runtime_vfs_filesystem_sealed,
			phase::retained_parent_held,
			phase::no_effect_boundary_armed,
			phase::typed_family_census,
			phase::active_read_connection_open,
			phase::wal_lock_and_prefix_held,
			phase::mapping_subprotocol_or_private_index,
			phase::eager_decode,
			phase::decoded_read_candidate_sealed,
			phase::connection_revoking,
			phase::outer_custody_join_pending,
			phase::outer_custody_join_sealed,
			phase::connection_closed,
			phase::zero_effect_callback_receipt_sealed,
			phase::logical_read_receipt,
		};
		for (std::size_t index = 1U; index < complete.size(); ++index)
			require(
				detail::is_sqlite_shm_reader_outer_read_transition(complete[index - 1U],
																   complete[index]),
				"complete outer read follows the proposed production-inactive state-only graph");
		require(detail::validate_sqlite_shm_reader_outer_read_path(complete),
				"state-only validator recognizes the complete logical-read receipt candidate path");
		require(!detail::is_sqlite_shm_reader_outer_read_transition(phase::wal_lock_and_prefix_held,
																	phase::eager_decode),
				"outer read cannot skip the mapping subprotocol or private index route");
		constexpr std::array skipped_mapping{
			phase::unresolved,
			phase::runtime_vfs_filesystem_sealed,
			phase::retained_parent_held,
			phase::no_effect_boundary_armed,
			phase::typed_family_census,
			phase::active_read_connection_open,
			phase::wal_lock_and_prefix_held,
			phase::eager_decode,
			phase::decoded_read_candidate_sealed,
			phase::connection_revoking,
			phase::outer_custody_join_pending,
			phase::outer_custody_join_sealed,
			phase::connection_closed,
			phase::zero_effect_callback_receipt_sealed,
			phase::logical_read_receipt,
		};
		require(!detail::validate_sqlite_shm_reader_outer_read_path(skipped_mapping),
				"outer read path rejects a logical receipt after a skipped mapping route");
		require(!detail::is_sqlite_shm_reader_outer_read_transition(
					phase::active_read_connection_open, phase::zero_effect_callback_receipt_sealed),
				"active connection cannot skip decode and custody closure");
		require(!detail::is_sqlite_shm_reader_outer_read_transition(
					phase::decoded_read_candidate_sealed, phase::logical_read_receipt),
				"decoded candidate cannot mint a receipt before close and zero-effect proof");
		require(!detail::is_sqlite_shm_reader_outer_read_transition(phase::connection_closed,
																	phase::logical_read_receipt),
				"connection close alone cannot mint a logical receipt");
		constexpr std::array skipped_close{
			phase::unresolved,
			phase::runtime_vfs_filesystem_sealed,
			phase::retained_parent_held,
			phase::no_effect_boundary_armed,
			phase::typed_family_census,
			phase::active_read_connection_open,
			phase::wal_lock_and_prefix_held,
			phase::mapping_subprotocol_or_private_index,
			phase::eager_decode,
			phase::decoded_read_candidate_sealed,
			phase::connection_revoking,
			phase::outer_custody_join_pending,
			phase::outer_custody_join_sealed,
			phase::logical_read_receipt,
		};
		require(!detail::validate_sqlite_shm_reader_outer_read_path(skipped_close),
				"outer read path rejects receipt before connection close and zero-effect proof");
	}

	void exercise_normalization_entry_phase_order()
	{
		using phase = detail::sqlite_shm_reader_normalization_phase;
		constexpr std::array complete{
			phase::no_authenticated_receipt,
			phase::logical_read_receipt_authenticated,
			phase::exclusive_source_revalidated,
			phase::pre_effect_receipt_sealed,
			phase::effect_armed,
			phase::effect_transcript_sealed,
			phase::durability_barrier_sealed,
			phase::effect_confirmed,
			phase::connection_closed,
			phase::post_effect_projection_validated,
		};
		require(
			detail::validate_sqlite_shm_reader_normalization_path(complete),
			"normalization path requires the authenticated logical-read receipt and post proof");
		require(!detail::is_sqlite_shm_reader_normalization_transition(
					phase::no_authenticated_receipt, phase::exclusive_source_revalidated),
				"source recheck cannot bypass the logical-read receipt");
		require(!detail::is_sqlite_shm_reader_normalization_transition(
					phase::logical_read_receipt_authenticated, phase::effect_armed),
				"effect cannot bypass exclusive recheck and pre-effect sealing");
		require(!detail::is_sqlite_shm_reader_normalization_transition(
					phase::pre_effect_receipt_sealed, phase::effect_confirmed),
				"effect confirmation cannot bypass the effect arm");
		require(!detail::is_sqlite_shm_reader_normalization_transition(phase::effect_armed,
																	   phase::effect_confirmed),
				"effect confirmation cannot bypass the callback transcript");
		require(!detail::is_sqlite_shm_reader_normalization_transition(
					phase::effect_transcript_sealed, phase::effect_confirmed),
				"effect confirmation cannot bypass the parent durability barrier");
		require(!detail::is_sqlite_shm_reader_normalization_transition(
					phase::durability_barrier_sealed, phase::connection_closed),
				"close cannot bypass the sealed effect receipt");
		require(detail::is_sqlite_shm_reader_normalization_transition(
					phase::exclusive_source_revalidated, phase::terminal_quarantined),
				"source drift has a terminal quarantine route");
		require(!detail::is_sqlite_shm_reader_normalization_transition(phase::terminal_quarantined,
																	   phase::terminal_quarantined),
				"normalization quarantine is sticky and cannot be replayed");

		constexpr std::array skipped_receipt{
			phase::no_authenticated_receipt,
			phase::exclusive_source_revalidated,
			phase::pre_effect_receipt_sealed,
			phase::effect_armed,
			phase::effect_confirmed,
			phase::connection_closed,
			phase::post_effect_projection_validated,
		};
		require(!detail::validate_sqlite_shm_reader_normalization_path(skipped_receipt),
				"normalization path rejects a missing logical-read receipt");

		constexpr std::array skipped_pre_effect{
			phase::no_authenticated_receipt,
			phase::logical_read_receipt_authenticated,
			phase::exclusive_source_revalidated,
			phase::effect_armed,
			phase::effect_confirmed,
			phase::connection_closed,
			phase::post_effect_projection_validated,
		};
		require(!detail::validate_sqlite_shm_reader_normalization_path(skipped_pre_effect),
				"normalization path rejects effect arm before pre-effect sealing");

		constexpr std::array quarantined{
			phase::no_authenticated_receipt,
			phase::logical_read_receipt_authenticated,
			phase::exclusive_source_revalidated,
			phase::terminal_quarantined,
		};
		require(!detail::validate_sqlite_shm_reader_normalization_path(quarantined),
				"quarantine is terminal and cannot be projected as normalization success");

		constexpr std::array missing_transcript{
			phase::no_authenticated_receipt,
			phase::logical_read_receipt_authenticated,
			phase::exclusive_source_revalidated,
			phase::pre_effect_receipt_sealed,
			phase::effect_armed,
			phase::durability_barrier_sealed,
			phase::effect_confirmed,
			phase::connection_closed,
			phase::post_effect_projection_validated,
		};
		require(!detail::validate_sqlite_shm_reader_normalization_path(missing_transcript),
				"normalization path rejects an effect receipt without its transcript");

		constexpr std::array missing_barrier{
			phase::no_authenticated_receipt,
			phase::logical_read_receipt_authenticated,
			phase::exclusive_source_revalidated,
			phase::pre_effect_receipt_sealed,
			phase::effect_armed,
			phase::effect_transcript_sealed,
			phase::effect_confirmed,
			phase::connection_closed,
			phase::post_effect_projection_validated,
		};
		require(!detail::validate_sqlite_shm_reader_normalization_path(missing_barrier),
				"normalization path rejects a close without parent durability receipts");
	}

	void exercise_map_sequence_proof()
	{
		constexpr int readonly = 8;
		constexpr int cant_initialize = readonly | (5 << 8);
		int vfs_identity{};
		int app_data_identity{};
		int mapping_identity{};
		const auto event =
			[&](const int page, const int status, const bool mapping, const bool seen_before)
		{
			sqlite_backend_shm_map_observation observation{};
			observation.page = page;
			observation.page_size = 32768;
			observation.caller_extend = 1;
			observation.delegated_extend = 0;
			observation.native_status = status;
			observation.returned_status = status;
			observation.native_mapping_nonnull = mapping;
			observation.returned_mapping_nonnull = mapping;
			observation.readonly_family_seen_before = seen_before;
			observation.readonly_family_seen_after = true;
			observation.pinned_underlying_vfs_identity = &vfs_identity;
			observation.pinned_underlying_vfs_app_data_identity = &app_data_identity;
			observation.native_mapping_identity =
				mapping ? static_cast<const volatile void*>(&mapping_identity) : nullptr;
			return observation;
		};
		const std::array exact_cold{event(0, cant_initialize, false, false)};
		require(validate_sqlite_source_shm_readonly_map_sequence(
					exact_cold, &vfs_identity, &app_data_identity, true, false),
				"cold proof accepts exact first page-zero CANTINIT/null event");

		auto normalized_readonly_null = event(0, readonly, false, false);
		normalized_readonly_null.returned_status = cant_initialize;
		require(
			validate_sqlite_source_shm_readonly_map_sequence(std::array{normalized_readonly_null},
															 &vfs_identity,
															 &app_data_identity,
															 true,
															 false),
			"cold proof accepts READONLY/null normalized to CANTINIT/null");

		const std::array late_page_zero{event(1, readonly, true, false),
										event(0, cant_initialize, false, true)};
		require(!validate_sqlite_source_shm_readonly_map_sequence(
					late_page_zero, &vfs_identity, &app_data_identity, true, false),
				"cold proof rejects an earlier nonzero-page mapped event");

		const std::array exact_mapped{event(0, readonly, true, false),
									  event(1, readonly, true, true)};
		require(validate_sqlite_source_shm_readonly_map_sequence(
					exact_mapped, &vfs_identity, &app_data_identity, false, true),
				"warm proof accepts mapped events with exact callback pointer evidence");

		auto native_ok_null = event(0, 0, false, false);
		native_ok_null.returned_status = cant_initialize;
		require(!validate_sqlite_source_shm_readonly_map_sequence(
					std::array{native_ok_null}, &vfs_identity, &app_data_identity, true, false),
				"qualified readonly proof rejects native SQLITE_OK without a mapping");
		auto native_ok_mapped = event(0, 0, true, false);
		native_ok_mapped.returned_status = readonly;
		require(!validate_sqlite_source_shm_readonly_map_sequence(
					std::array{native_ok_mapped}, &vfs_identity, &app_data_identity, false, false),
				"qualified readonly proof rejects native SQLITE_OK with a mapping");

		const auto cantinit_after_mapped = event(0, cant_initialize, false, true);
		const std::array reversed_transition{
			exact_mapped[0], exact_mapped[1], cantinit_after_mapped};
		require(!validate_sqlite_source_shm_readonly_map_sequence(
					reversed_transition, &vfs_identity, &app_data_identity, false, true),
				"warm proof rejects CANTINIT/null after a mapped route");

		auto missing_pointer = exact_mapped;
		missing_pointer[0].native_mapping_identity = nullptr;
		require(!validate_sqlite_source_shm_readonly_map_sequence(
					missing_pointer, &vfs_identity, &app_data_identity, false, true),
				"warm proof rejects a non-null mapping without exact callback pointer evidence");

		auto invalid_page = exact_cold;
		invalid_page[0].page = -1;
		require(!validate_sqlite_source_shm_readonly_map_sequence(
					invalid_page, &vfs_identity, &app_data_identity, true, false),
				"map proof rejects a negative page");
		auto invalid_page_size = exact_cold;
		invalid_page_size[0].page_size = 0;
		require(!validate_sqlite_source_shm_readonly_map_sequence(
					invalid_page_size, &vfs_identity, &app_data_identity, true, false),
				"map proof rejects a nonpositive page size");
		auto inconsistent_pointer_flags = exact_cold;
		inconsistent_pointer_flags[0].native_mapping_identity =
			static_cast<const volatile void*>(&mapping_identity);
		require(!validate_sqlite_source_shm_readonly_map_sequence(
					inconsistent_pointer_flags, &vfs_identity, &app_data_identity, true, false),
				"map proof rejects pointer/nonnull metadata disagreement");
	}

	void exercise_nested_terminal_and_logical_receipt_barriers()
	{
		using nested = detail::sqlite_nested_mapping_terminal_phase;
		constexpr std::array mapped_success{
			nested::active_read_connection,
			nested::attempt_pin_acquired,
			nested::native_callback_entered,
			nested::native_result_observed,
			nested::pending_lease,
			nested::published_reader_lease,
			nested::revoke_intent,
			nested::registry_hidden,
			nested::callbacks_drained,
			nested::native_cleanup_complete,
			nested::nested_mapping_terminal,
		};
		require(detail::validate_sqlite_nested_mapping_terminal_path(mapped_success),
				"nested mapping terminal requires native observation and hide-before-cleanup");

		constexpr std::array nonpromotable_cleanup{
			nested::active_read_connection,
			nested::attempt_pin_acquired,
			nested::native_callback_entered,
			nested::native_result_observed,
			nested::nonpromotable_outcome,
			nested::revoke_intent,
			nested::registry_hidden,
			nested::callbacks_drained,
			nested::native_cleanup_complete,
			nested::nested_mapping_terminal,
		};
		require(detail::validate_sqlite_nested_mapping_terminal_path(nonpromotable_cleanup),
				"nonpromotable native outcome still requires custody cleanup before terminal");

		constexpr std::array quarantined{
			nested::active_read_connection,
			nested::attempt_pin_acquired,
			nested::native_callback_entered,
			nested::terminal_quarantined,
		};
		require(detail::validate_sqlite_nested_mapping_terminal_path(quarantined),
				"ambiguous callback has a fail-closed nested quarantine terminal");

		constexpr std::array promoted_before_native{
			nested::active_read_connection,
			nested::attempt_pin_acquired,
			nested::pending_lease,
			nested::published_reader_lease,
			nested::revoke_intent,
			nested::registry_hidden,
			nested::callbacks_drained,
			nested::native_cleanup_complete,
			nested::nested_mapping_terminal,
		};
		require(!detail::validate_sqlite_nested_mapping_terminal_path(promoted_before_native),
				"lease cannot be published before native callback result");

		constexpr std::array cleanup_before_hide{
			nested::active_read_connection,
			nested::attempt_pin_acquired,
			nested::native_callback_entered,
			nested::native_result_observed,
			nested::pending_lease,
			nested::published_reader_lease,
			nested::revoke_intent,
			nested::native_cleanup_complete,
			nested::nested_mapping_terminal,
		};
		require(!detail::validate_sqlite_nested_mapping_terminal_path(cleanup_before_hide),
				"cleanup cannot precede registry hide and callback drain");

		constexpr std::array callbacks_before_hide{
			nested::active_read_connection,
			nested::attempt_pin_acquired,
			nested::native_callback_entered,
			nested::native_result_observed,
			nested::pending_lease,
			nested::published_reader_lease,
			nested::revoke_intent,
			nested::callbacks_drained,
			nested::registry_hidden,
			nested::native_cleanup_complete,
			nested::nested_mapping_terminal,
		};
		require(!detail::validate_sqlite_nested_mapping_terminal_path(callbacks_before_hide),
				"callback drain cannot precede registry hide");

		constexpr std::array cleanup_before_drain{
			nested::active_read_connection,
			nested::attempt_pin_acquired,
			nested::native_callback_entered,
			nested::native_result_observed,
			nested::pending_lease,
			nested::published_reader_lease,
			nested::revoke_intent,
			nested::registry_hidden,
			nested::native_cleanup_complete,
			nested::callbacks_drained,
			nested::nested_mapping_terminal,
		};
		require(!detail::validate_sqlite_nested_mapping_terminal_path(cleanup_before_drain),
				"native cleanup cannot precede callback drain");

		constexpr std::array hide_before_revoke{
			nested::active_read_connection,
			nested::attempt_pin_acquired,
			nested::native_callback_entered,
			nested::native_result_observed,
			nested::pending_lease,
			nested::published_reader_lease,
			nested::registry_hidden,
			nested::revoke_intent,
			nested::callbacks_drained,
			nested::native_cleanup_complete,
			nested::nested_mapping_terminal,
		};
		require(!detail::validate_sqlite_nested_mapping_terminal_path(hide_before_revoke),
				"registry hide cannot precede revoke intent");

		constexpr std::array revoke_before_publish{
			nested::active_read_connection,
			nested::attempt_pin_acquired,
			nested::native_callback_entered,
			nested::native_result_observed,
			nested::pending_lease,
			nested::revoke_intent,
			nested::registry_hidden,
			nested::callbacks_drained,
			nested::native_cleanup_complete,
			nested::nested_mapping_terminal,
		};
		require(!detail::validate_sqlite_nested_mapping_terminal_path(revoke_before_publish),
				"revoke cannot bypass pending and published lease states");

		using logical = detail::sqlite_logical_read_receipt_phase;
		constexpr std::array logical_success{
			logical::active_read_connection,
			logical::eager_logical_read,
			logical::nested_terminal_consumed,
			logical::outer_connection_revoking,
			logical::outer_custody_join_pending,
			logical::outer_custody_join_sealed,
			logical::outer_connection_closed,
			logical::zero_effect_census_sealed,
			logical::logical_read_receipt,
		};
		require(detail::validate_sqlite_logical_read_receipt_path(logical_success),
				"logical receipt requires nested terminal, custody join, close, and zero-effect "
				"census");

		constexpr std::array missing_nested_terminal{
			logical::active_read_connection,
			logical::eager_logical_read,
			logical::outer_connection_revoking,
			logical::outer_custody_join_pending,
			logical::outer_custody_join_sealed,
			logical::outer_connection_closed,
			logical::zero_effect_census_sealed,
			logical::logical_read_receipt,
		};
		require(!detail::validate_sqlite_logical_read_receipt_path(missing_nested_terminal),
				"logical receipt cannot bypass nested terminal consumption");

		constexpr std::array missing_join_and_census{
			logical::active_read_connection,
			logical::eager_logical_read,
			logical::nested_terminal_consumed,
			logical::outer_connection_revoking,
			logical::outer_custody_join_pending,
			logical::outer_connection_closed,
			logical::logical_read_receipt,
		};
		require(!detail::validate_sqlite_logical_read_receipt_path(missing_join_and_census),
				"logical receipt cannot bypass sealed custody and zero-effect census");

		constexpr std::array logical_quarantine{
			logical::active_read_connection,
			logical::eager_logical_read,
			logical::terminal_quarantined,
		};
		require(detail::validate_sqlite_logical_read_receipt_path(logical_quarantine),
				"outer ambiguity remains a fail-closed terminal");
	}
} // namespace

int main()
{
	exercise_strict_uri();
	exercise_branch_local_capability_absence();
	exercise_active_read_connection_receipt();
	exercise_active_read_identity_and_pre_effect_matrix();
	exercise_outer_read_phase_order();
	exercise_normalization_entry_phase_order();
	exercise_map_sequence_proof();
	exercise_nested_terminal_and_logical_receipt_barriers();
#if defined(__linux__) && defined(F_OFD_SETLK)
	exercise_repeated_exact_census();
#endif
	return 0;
}
