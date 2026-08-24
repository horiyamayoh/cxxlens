#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <spawn.h>

#if defined(__linux__)
#include <csignal>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cxxlens/sdk.hpp>

#include "../../support/sqlite_store_fixture.hpp"
#include "../../support/sqlite_store_v3_scenario.hpp"

#if defined(__linux__) && defined(F_OFD_SETLK)
extern char** environ;

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::test::sqlite_fixture;
	using namespace cxxlens::test::sqlite_v3_scenario;

	constexpr std::string_view child_mode{"--missing-source-shm-symbols-child"};
	constexpr auto child_timeout = std::chrono::seconds{20};
	constexpr auto child_poll_interval = std::chrono::milliseconds{10};
	constexpr std::size_t maximum_environment_entries = 4'096U;
	constexpr std::size_t maximum_environment_bytes = 1024U * 1024U;

	[[noreturn]] void fail(std::string message)
	{
		throw std::runtime_error{std::move(message)};
	}

	void require(const bool condition, std::string message)
	{
		if (!condition)
			fail(std::move(message));
	}

	struct exact_file_state
	{
		std::uintmax_t device{};
		std::uintmax_t inode{};
		std::uintmax_t byte_count{};
		std::uintmax_t mode{};
		std::vector<unsigned char> bytes;

		[[nodiscard]] bool operator==(const exact_file_state&) const = default;
	};

	struct exact_family_state
	{
		std::vector<std::string> directory_entries;
		std::map<std::string, exact_file_state, std::less<>> files;

		[[nodiscard]] bool operator==(const exact_family_state&) const = default;
	};

	[[nodiscard]] exact_family_state capture_exact_family(const std::filesystem::path& main_path)
	{
		const auto captured = capture_files(main_path);
		exact_family_state output;
		output.directory_entries = captured.directory_entries;
		for (const auto& [leaf, bytes] : captured.bytes_by_name)
		{
			const auto path = main_path.parent_path() / leaf;
			struct stat metadata{};
			if (::lstat(path.c_str(), &metadata) != 0)
				throw std::system_error{errno, std::generic_category(), "lstat Store source file"};
			require(S_ISREG(metadata.st_mode), "Store source family contains a non-regular file");
			require(metadata.st_size >= 0 &&
						static_cast<std::uintmax_t>(metadata.st_size) == bytes.size(),
					"Store source file size disagrees with its captured byte stream");
			output.files.emplace(leaf,
								 exact_file_state{static_cast<std::uintmax_t>(metadata.st_dev),
												  static_cast<std::uintmax_t>(metadata.st_ino),
												  static_cast<std::uintmax_t>(metadata.st_size),
												  static_cast<std::uintmax_t>(metadata.st_mode),
												  bytes});
		}
		return output;
	}

	[[nodiscard]] std::string source_leaf(const std::filesystem::path& main_path,
										  const std::string_view suffix = {})
	{
		return main_path.filename().string() + std::string{suffix};
	}

	void require_exact_active_family(const exact_family_state& state,
									 const std::filesystem::path& main_path)
	{
		const auto main = source_leaf(main_path);
		const auto wal = source_leaf(main_path, "-wal");
		const auto shm = source_leaf(main_path, "-shm");
		const auto journal = source_leaf(main_path, "-journal");
		require(state.files.size() == 3U && state.files.contains(main) &&
					state.files.contains(wal) && state.files.contains(shm) &&
					!state.files.contains(journal),
				"exact v2 active source is not a main/WAL/SHM family");
		require(state.files.at(main).byte_count > 0U && state.files.at(wal).byte_count > 32U &&
					state.files.at(shm).byte_count > 0U,
				"exact v2 active source contains an empty authority file");
	}

	void require_exact_v2_projection(sdk::snapshot_store& store,
									 const sdk::relation_engine& engine,
									 const exact_v2_scenario& expected,
									 const std::string_view label)
	{
		const auto compatibility = store.compatibility();
		require(compatibility.backend == "sqlite" &&
					compatibility.readable_format == sdk::semantic_version{2U, 6U, 0U} &&
					compatibility.direct_open && compatibility.migration_required,
				std::string{label} + " lost the exact v2 compatibility tuple");
		auto current = store.current(selector(engine));
		auto exported = store.canonical_export(expected.current.snapshot_id);
		require(current && exported && current->id() == expected.current.snapshot_id &&
					current->publication().publication_id == expected.current.publication_id &&
					current->publication().sequence == expected.current.sequence &&
					*exported == expected.current_canonical_export,
				std::string{label} + " changed the exact v2 logical authority");
	}

	[[nodiscard]] std::vector<std::string>
	child_environment(const std::filesystem::path& interposer)
	{
		std::vector<std::string> output;
		output.reserve(maximum_environment_entries + 1U);
		std::size_t total_bytes{};
		std::size_t index{};
		for (; environ[index] != nullptr; ++index)
		{
			require(index < maximum_environment_entries,
					"child environment exceeds the entry bound");
			std::string entry{environ[index]};
			require(entry.size() <= maximum_environment_bytes - total_bytes,
					"child environment exceeds the byte bound");
			total_bytes += entry.size();
			if (!entry.starts_with("LD_PRELOAD=") && !entry.starts_with("CXXLENS_TEST_"))
				output.push_back(std::move(entry));
		}
		std::string preload{"LD_PRELOAD="};
		preload.append(interposer.string());
		require(preload.size() <= maximum_environment_bytes - total_bytes,
				"child preload path exceeds the environment byte bound");
		output.push_back(std::move(preload));
		return output;
	}

	[[nodiscard]] pid_t spawn_filtered_child(const std::filesystem::path& executable,
											 const std::filesystem::path& interposer,
											 const std::filesystem::path& database_path)
	{
		auto environment = child_environment(interposer);
		std::vector<char*> environment_pointers;
		environment_pointers.reserve(environment.size() + 1U);
		for (auto& entry : environment)
			environment_pointers.push_back(entry.data());
		environment_pointers.push_back(nullptr);

		auto executable_text = executable.string();
		auto mode_text = std::string{child_mode};
		auto database_text = database_path.string();
		std::array arguments{executable_text.data(),
							 mode_text.data(),
							 database_text.data(),
							 static_cast<char*>(nullptr)};
		pid_t child{};
		const auto spawned = ::posix_spawn(&child,
										   executable_text.c_str(),
										   nullptr,
										   nullptr,
										   arguments.data(),
										   environment_pointers.data());
		if (spawned != 0)
			throw std::system_error{
				spawned, std::generic_category(), "spawn source-SHM symbol-filter child"};
		return child;
	}

	class owned_child
	{
	  public:
		explicit owned_child(const pid_t value) noexcept : value_{value} {}
		owned_child(const owned_child&) = delete;
		owned_child& operator=(const owned_child&) = delete;
		~owned_child()
		{
			terminate_and_reap();
		}

		[[nodiscard]] pid_t get() const noexcept
		{
			return value_;
		}

		void release() noexcept
		{
			value_ = -1;
		}

		void terminate_and_reap() noexcept
		{
			if (value_ <= 0)
				return;
			(void)::kill(value_, SIGKILL);
			int status{};
			while (::waitpid(value_, &status, 0) < 0 && errno == EINTR)
			{
			}
			value_ = -1;
		}

	  private:
		pid_t value_{-1};
	};

	class owned_library
	{
	  public:
		explicit owned_library(void* value) noexcept : value_{value} {}
		owned_library(const owned_library&) = delete;
		owned_library& operator=(const owned_library&) = delete;
		~owned_library()
		{
			if (value_ != nullptr)
				(void)::dlclose(value_);
		}

		[[nodiscard]] void* get() const noexcept
		{
			return value_;
		}

	  private:
		void* value_{};
	};

	void wait_for_filtered_child(const pid_t process)
	{
		owned_child child{process};
		const auto deadline = std::chrono::steady_clock::now() + child_timeout;
		int status{};
		while (true)
		{
			const auto waited = ::waitpid(child.get(), &status, WNOHANG);
			if (waited == child.get())
			{
				child.release();
				break;
			}
			if (waited < 0 && errno == EINTR)
				continue;
			if (waited < 0)
			{
				if (errno == ECHILD)
					child.release();
				throw std::system_error{
					errno, std::generic_category(), "wait source-SHM symbol-filter child"};
			}
			if (std::chrono::steady_clock::now() >= deadline)
			{
				child.terminate_and_reap();
				fail("source-SHM symbol-filter child exceeded the 20 second bound");
			}
			std::this_thread::sleep_for(child_poll_interval);
		}
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"source-SHM symbol-filter child did not report the exact fail-closed tuple");
	}

	int run_filtered_child(const std::filesystem::path& database_path)
	{
		try
		{
			void* sqlite_library{};
			for (const auto* candidate : {"libsqlite3.so.0", "libsqlite3.so"})
			{
				sqlite_library = ::dlopen(candidate, RTLD_NOW | RTLD_GLOBAL);
				if (sqlite_library != nullptr)
					break;
			}
			if (sqlite_library == nullptr)
			{
				std::cerr << "source-SHM child could not load its SQLite runtime\n";
				return 9;
			}
			owned_library sqlite{sqlite_library};
			if (::dlsym(RTLD_DEFAULT, "sqlite3_sourceid") == nullptr)
			{
				std::cerr << "source-SHM child blocked RTLD_DEFAULT before target capture\n";
				return 9;
			}
			if (::dlsym(sqlite.get(), "sqlite3_libversion_number") == nullptr ||
				::dlsym(sqlite.get(), "sqlite3_open_v2") == nullptr)
			{
				std::cerr << "source-SHM child lost an unrelated SQLite base symbol\n";
				return 9;
			}
			for (const auto* symbol :
				 {"sqlite3_sourceid", "sqlite3_uri_parameter", "sqlite3_uri_key"})
				if (::dlsym(sqlite.get(), symbol) != nullptr)
				{
					std::cerr << "source-SHM child retained target symbol " << symbol << '\n';
					return 9;
				}
			if (::dlsym(RTLD_DEFAULT, "sqlite3_sourceid") == nullptr)
			{
				std::cerr << "source-SHM child blocked RTLD_DEFAULT after target capture\n";
				return 9;
			}
			auto value = make_engine();
			auto opened = sdk::open_sqlite_snapshot_store(database_path.string(), std::move(value));
			if (opened)
			{
				std::cerr << "source-SHM symbol absence admitted the active WAL source\n";
				return 10;
			}
			const auto& error = opened.error();
			if (error.code != "store.backend-unavailable" || error.field != "sqlite" ||
				error.detail != "source-shm-readonly-qualification")
			{
				std::cerr << "source-SHM symbol absence returned " << error.code << '/'
						  << error.field << '/' << error.detail << '\n';
				return 11;
			}
			return 0;
		}
		catch (const std::exception& error)
		{
			std::cerr << "source-SHM symbol-filter child failed: " << error.what() << '\n';
			return 12;
		}
	}

	void run_parent(const std::filesystem::path& interposer)
	{
		require(std::filesystem::is_regular_file(interposer),
				"source-SHM dlsym interposer is not a regular file");
		const auto executable = std::filesystem::read_symlink("/proc/self/exe");
		require(executable.is_absolute() && std::filesystem::is_regular_file(executable),
				"source-SHM integration executable identity is unavailable");

		auto value = make_engine();
		temporary_directory directory{"sqlite-source-shm-missing-symbols"};
		const auto path = directory.path() / "exact-v2.sqlite";
		const auto expected = create_exact_v2_scenario(path, value);
		active_wal_sidecar_fixture active{path, wal_source_authority::predecessor_v2};
		const auto before = capture_exact_family(path);
		require_exact_active_family(before, path);

		{
			auto baseline = sdk::open_sqlite_snapshot_store(path.string(), value);
			require(baseline.has_value(),
					"unfiltered production Store could not read the exact v2 active source");
			require_exact_v2_projection(*baseline, value, expected, "unfiltered active-WAL open");
		}
		require(capture_exact_family(path) == before,
				"unfiltered active-WAL baseline changed source identity, size, or bytes");

		const auto child = spawn_filtered_child(executable, interposer, path);
		wait_for_filtered_child(child);
		require(capture_exact_family(path) == before,
				"source-SHM symbol failure changed source identity, size, or bytes");

		active.close();
		const auto shm = std::filesystem::path{path.string() + "-shm"};
		require(std::filesystem::remove(shm),
				"explicit source-SHM removal did not perform the independent transition");
		const auto wal_only = capture_exact_family(path);
		const auto main_leaf = source_leaf(path);
		const auto wal_leaf = source_leaf(path, "-wal");
		require(wal_only.files.size() == 2U && wal_only.files.contains(main_leaf) &&
					wal_only.files.contains(wal_leaf) &&
					wal_only.files.at(main_leaf) == before.files.at(main_leaf) &&
					wal_only.files.at(wal_leaf) == before.files.at(wal_leaf),
				"explicit SHM removal changed main or WAL identity, size, or bytes");

		{
			auto recovered = sdk::open_sqlite_snapshot_store(path.string(), value);
			require(recovered.has_value(),
					"independent WAL-only attempt did not recover after explicit SHM removal");
			require_exact_v2_projection(*recovered, value, expected, "WAL-only recovery");
		}
		require(capture_exact_family(path) == wal_only,
				"WAL-only recovery changed source identity, size, or bytes");
	}
} // namespace
#endif

int main(const int argc, char** argv)
{
#if defined(__linux__) && defined(F_OFD_SETLK)
	if (argc == 3 && std::string_view{argv[1]} == child_mode)
		return run_filtered_child(argv[2]);
	if (argc != 2)
	{
		std::cerr << "usage: " << argv[0] << " <source-shm-dlsym-interposer>\n";
		return 2;
	}
	try
	{
		run_parent(std::filesystem::absolute(argv[1]).lexically_normal());
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
#else
	(void)argc;
	(void)argv;
	std::cerr << "source-SHM symbol integration test was registered on an unsupported platform\n";
	return 1;
#endif
}
