#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

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
		require(
			sqlite_source_shm_native_ok_projection_production_activation_enabled(),
			"native SQLITE_OK source-SHM projection is enabled only through the qualified route");
		auto optional_port = make_sqlite_source_shm_readonly_preflight(
			sqlite_default_observation_binding{}, sqlite_backend_opaque_identity{});
		require(optional_port.has_value() && !*optional_port,
				"missing active-WAL callback dependency leaves baseline observation available");
	}

	void exercise_outer_read_phase_order()
	{
		// This validates the source-private outer receipt order used by the active-WAL runtime.
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
			require(detail::is_sqlite_shm_reader_outer_read_transition(complete[index - 1U],
																	   complete[index]),
					"complete outer read follows the source-private state-only graph");
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
} // namespace

int main()
{
	exercise_strict_uri();
	exercise_branch_local_capability_absence();
	exercise_outer_read_phase_order();
	exercise_map_sequence_proof();
#if defined(__linux__) && defined(F_OFD_SETLK)
	exercise_repeated_exact_census();
#endif
	return 0;
}
