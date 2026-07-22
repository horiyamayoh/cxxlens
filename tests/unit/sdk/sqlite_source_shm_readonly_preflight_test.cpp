#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

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
		auto optional_port = make_sqlite_source_shm_readonly_preflight(
			sqlite_default_observation_binding{}, sqlite_backend_opaque_identity{});
		require(optional_port.has_value() && !*optional_port,
				"missing active-WAL callback dependency leaves baseline observation available");
	}

	void exercise_map_sequence_proof()
	{
		constexpr int readonly = 8;
		constexpr int cant_initialize = readonly | (5 << 8);
		int vfs_identity{};
		int app_data_identity{};
		const auto event =
			[&](const int page, const int status, const bool mapping, const bool seen_before)
		{
			return sqlite_backend_shm_map_observation{page,
													  32768,
													  1,
													  0,
													  status,
													  status,
													  mapping,
													  mapping,
													  seen_before,
													  true,
													  &vfs_identity,
													  &app_data_identity};
		};
		const std::array exact_cold{event(0, cant_initialize, false, false)};
		require(validate_sqlite_source_shm_readonly_map_sequence(
					exact_cold, &vfs_identity, &app_data_identity, true, false),
				"cold proof accepts exact first page-zero CANTINIT/null event");

		const std::array late_page_zero{event(1, readonly, true, false),
										event(0, cant_initialize, false, true)};
		require(!validate_sqlite_source_shm_readonly_map_sequence(
					late_page_zero, &vfs_identity, &app_data_identity, true, false),
				"cold proof rejects an earlier nonzero-page mapped event");
	}
} // namespace

int main()
{
	exercise_strict_uri();
	exercise_branch_local_capability_absence();
	exercise_map_sequence_proof();
#if defined(__linux__) && defined(F_OFD_SETLK)
	exercise_repeated_exact_census();
#endif
	return 0;
}
