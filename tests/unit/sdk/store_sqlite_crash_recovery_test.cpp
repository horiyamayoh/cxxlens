#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <cxxlens/sdk.hpp>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../support/sqlite_store_fixture.hpp"
#include "../../support/sqlite_store_v3_scenario.hpp"

namespace
{
	using cxxlens::test::sqlite_v3_scenario::exact_v2_scenario;
	using sqlite_row = cxxlens::test::sqlite_fixture::row;

	[[noreturn]] void fail(std::string message)
	{
		throw std::runtime_error{std::move(message)};
	}

	void require(const bool condition, std::string message)
	{
		if (!condition)
			fail(std::move(message));
	}

	[[nodiscard]] std::string error_text(const cxxlens::sdk::error& value)
	{
		return value.code + '/' + value.field + '/' + value.detail;
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
		const auto captured = cxxlens::test::sqlite_fixture::capture_files(main_path);
		exact_family_state output;
		output.directory_entries = captured.directory_entries;
		for (const auto& [leaf, bytes] : captured.bytes_by_name)
		{
			const auto path = main_path.parent_path() / leaf;
			struct stat metadata{};
			if (::lstat(path.c_str(), &metadata) != 0)
				throw std::system_error{errno, std::generic_category(), "lstat Store crash file"};
			require(S_ISREG(metadata.st_mode), "Store crash family contains a non-regular file");
			require(metadata.st_size >= 0 &&
						static_cast<std::uintmax_t>(metadata.st_size) == bytes.size(),
					"Store crash file size disagrees with its captured bytes");
			output.files.emplace(leaf,
								 exact_file_state{static_cast<std::uintmax_t>(metadata.st_dev),
												  static_cast<std::uintmax_t>(metadata.st_ino),
												  static_cast<std::uintmax_t>(metadata.st_size),
												  static_cast<std::uintmax_t>(metadata.st_mode),
												  bytes});
		}
		return output;
	}

	[[nodiscard]] std::string family_leaf(const std::filesystem::path& path,
										  const std::string_view suffix = {})
	{
		return path.filename().string() + std::string{suffix};
	}

	void require_complete_paused_family(const exact_family_state& state,
										const std::filesystem::path& path)
	{
		const auto main = family_leaf(path);
		const auto wal = family_leaf(path, "-wal");
		const auto shm = family_leaf(path, "-shm");
		const auto journal = family_leaf(path, "-journal");
		require(state.files.size() == 3U && state.files.contains(main) &&
					state.files.contains(wal) && state.files.contains(shm) &&
					!state.files.contains(journal),
				"paused migration is not an exact main/WAL/SHM family");
		require(state.files.at(main).byte_count > 0U && state.files.at(shm).byte_count > 0U,
				"paused migration contains an empty main database or SHM file");
		std::vector expected_entries{main, wal, shm};
		std::ranges::sort(expected_entries);
		require(state.directory_entries == expected_entries,
				"paused migration directory contains an unexpected entry");
	}

	struct v2_store_census
	{
		std::vector<sqlite_row> publications;
		std::vector<sqlite_row> heads;
		std::vector<sqlite_row> metadata;
		std::vector<sqlite_row> schema;

		[[nodiscard]] bool operator==(const v2_store_census&) const = default;
	};

	[[nodiscard]] v2_store_census capture_v2_store_census(const std::filesystem::path& path)
	{
		cxxlens::test::sqlite_fixture::connection database{path, true};
		return {
			database.query(
				"SELECT publication_id,series_id,snapshot_id,sequence,generation,parent,state,"
				"checksum,payload FROM cxxlens_ng_publication ORDER BY publication_id"),
			database.query(
				"SELECT series_id,current_publication,sequence FROM cxxlens_ng_series_head "
				"ORDER BY series_id"),
			database.query("SELECT key,value FROM cxxlens_ng_metadata ORDER BY key"),
			database.query(
				"SELECT type,name,tbl_name,rootpage,sql FROM sqlite_schema ORDER BY type,name"),
		};
	}

	void require_complete_v2_census(const v2_store_census& census,
									const exact_v2_scenario& expected)
	{
		using cxxlens::test::sqlite_fixture::integer;
		using cxxlens::test::sqlite_fixture::text;
		bool found_prior{};
		bool found_current{};
		bool found_noncommitted{};
		for (const auto& row : census.publications)
		{
			require(row.size() == 9U, "v2 crash census publication width drifted");
			found_prior = found_prior || text(row[0U]) == expected.prior.publication_id;
			found_current = found_current || text(row[0U]) == expected.current.publication_id;
			found_noncommitted = found_noncommitted || integer(row[6U]) != 3;
		}
		const auto current_head = std::ranges::find_if(
			census.heads,
			[&](const auto& row)
			{
				return row.size() == 3U && text(row[1U]) == expected.current.publication_id;
			});
		require(found_prior && found_current && found_noncommitted &&
					current_head != census.heads.end() && !census.metadata.empty() &&
					!census.schema.empty(),
				"v2 crash fixture lacks prior/current/diagnostic/head authority");
	}

	void require_exact_v2_projection(const std::filesystem::path& path,
									 const cxxlens::sdk::relation_engine& engine,
									 const exact_v2_scenario& expected,
									 const std::string_view label)
	{
		auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), engine);
		require(store.has_value(),
				std::string{label} +
					" Store reopen failed: " + (store ? std::string{} : error_text(store.error())));
		const auto compatibility = store->compatibility();
		require(compatibility.backend == "sqlite" &&
					compatibility.readable_format == cxxlens::sdk::semantic_version{2U, 6U, 0U} &&
					compatibility.direct_open && compatibility.migration_required,
				std::string{label} + " changed the exact v2 compatibility tuple");
		auto current = store->current(cxxlens::test::sqlite_v3_scenario::selector(engine));
		auto prior = store->open_publication(expected.prior.publication_id);
		auto exported = store->canonical_export(expected.current.snapshot_id);
		require(current && prior && exported && current->id() == expected.current.snapshot_id &&
					current->publication().publication_id == expected.current.publication_id &&
					current->publication().sequence == expected.current.sequence &&
					prior->publication().publication_id == expected.prior.publication_id &&
					*exported == expected.current_canonical_export,
				std::string{label} + " changed the exact predecessor authority");
	}

	class owned_descriptor
	{
	  public:
		explicit owned_descriptor(const int value = -1) noexcept : value_{value} {}
		owned_descriptor(const owned_descriptor&) = delete;
		owned_descriptor& operator=(const owned_descriptor&) = delete;
		~owned_descriptor()
		{
			reset();
		}

		[[nodiscard]] int get() const noexcept
		{
			return value_;
		}

		void reset(const int value = -1) noexcept
		{
			if (value_ >= 0)
				(void)::close(value_);
			value_ = value;
		}

	  private:
		int value_{-1};
	};

	class owned_child
	{
	  public:
		explicit owned_child(const pid_t value) noexcept : value_{value} {}
		owned_child(const owned_child&) = delete;
		owned_child& operator=(const owned_child&) = delete;
		~owned_child()
		{
			if (value_ <= 0)
				return;
			(void)::kill(value_, SIGKILL);
			int status{};
			while (::waitpid(value_, &status, 0) < 0 && errno == EINTR)
			{
			}
		}

		[[nodiscard]] int kill_and_wait()
		{
			if (::kill(value_, SIGKILL) != 0)
				throw std::system_error{errno, std::generic_category(), "kill migration child"};
			int status{};
			pid_t waited{};
			do
			{
				waited = ::waitpid(value_, &status, 0);
			} while (waited < 0 && errno == EINTR);
			if (waited != value_)
				throw std::system_error{errno, std::generic_category(), "wait migration child"};
			value_ = -1;
			return status;
		}

	  private:
		pid_t value_{-1};
	};

	[[nodiscard]] std::string canonical_executable(const char* value)
	{
		std::error_code error;
		auto path = std::filesystem::canonical(value, error);
		return error ? std::string{value} : path.string();
	}

	[[nodiscard]] exact_family_state crash_once(const std::string& executable,
												const std::filesystem::path& test_shim,
												const std::filesystem::path& database,
												const exact_family_state& before)
	{
		std::array<int, 2U> ready_pipe{};
		if (::pipe(ready_pipe.data()) != 0)
			throw std::system_error{errno, std::generic_category(), "create crash ready pipe"};
		owned_descriptor ready_read{ready_pipe[0U]};
		owned_descriptor ready_write{ready_pipe[1U]};
		const auto child = ::fork();
		if (child < 0)
			throw std::system_error{errno, std::generic_category(), "fork migration crash child"};
		if (child == 0)
		{
			ready_read.reset();
			const auto ready_descriptor = std::to_string(ready_write.get());
			if (::setenv("LD_PRELOAD", test_shim.c_str(), 1) != 0 ||
				::setenv("CXXLENS_TEST_SQLITE_CRASH_PAUSE", "1", 1) != 0 ||
				::setenv("CXXLENS_TEST_SQLITE_CRASH_READY_FD", ready_descriptor.c_str(), 1) != 0)
				std::_Exit(125);
			std::array arguments{const_cast<char*>(executable.c_str()),
								 const_cast<char*>("--crash-child"),
								 const_cast<char*>(database.c_str()),
								 static_cast<char*>(nullptr)};
			::execv(executable.c_str(), arguments.data());
			std::_Exit(126);
		}

		owned_child running_child{child};
		ready_write.reset();
		pollfd readiness{ready_read.get(), POLLIN, 0};
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
		for (;;)
		{
			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
				deadline - std::chrono::steady_clock::now());
			if (remaining <= std::chrono::milliseconds::zero())
				fail("migration child did not reach the marker deadline");
			const auto waited = ::poll(&readiness, 1U, static_cast<int>(remaining.count()));
			if (waited < 0 && errno == EINTR)
				continue;
			if (waited != 1 || (readiness.revents & POLLIN) == 0)
				fail(waited == 0 ? "migration child did not reach the marker deadline"
								 : "migration child exited before the marker boundary");
			break;
		}
		char marker{};
		ssize_t received{};
		do
		{
			received = ::read(ready_read.get(), &marker, sizeof(marker));
		} while (received < 0 && errno == EINTR);
		require(received == sizeof(marker) && marker == 'R',
				"migration child emitted an invalid ready marker");

		const auto paused = capture_exact_family(database);
		require_complete_paused_family(paused, database);
		const auto main = family_leaf(database);
		require(before.files.contains(main) && paused.files.at(main) == before.files.at(main),
				"precommit migration changed the main database identity, size, or bytes");
		const auto status = running_child.kill_and_wait();
		require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
				"migration child did not terminate by SIGKILL");
		const auto after = capture_exact_family(database);
		require(after == paused,
				"process termination changed paused main/WAL/SHM identities, sizes, or bytes");
		return after;
	}

	int run_child(const std::filesystem::path& database)
	{
		try
		{
			const auto engine = cxxlens::test::sqlite_v3_scenario::make_engine();
			auto store = cxxlens::sdk::open_sqlite_snapshot_store(database.string(), engine);
			if (!store)
				return 10;
			const auto migrated = store->compact();
			return migrated ? 11 : 12;
		}
		catch (const std::exception& error)
		{
			std::cerr << "migration child failed: " << error.what() << '\n';
			return 13;
		}
	}

	void run_parent(const char* executable_argument, const char* test_shim_argument)
	{
		const auto executable = canonical_executable(executable_argument);
		const auto test_shim = std::filesystem::canonical(test_shim_argument);
		const auto engine = cxxlens::test::sqlite_v3_scenario::make_engine();
		cxxlens::test::sqlite_fixture::temporary_directory directory{"store-sqlite-crash-recovery"};
		const auto database = directory.path() / "predecessor.sqlite";
		const auto expected =
			cxxlens::test::sqlite_v3_scenario::create_exact_v2_scenario(database, engine);
		const auto expected_census = capture_v2_store_census(database);
		require_complete_v2_census(expected_census, expected);

		for (const auto label : {std::string_view{"first crash"}, std::string_view{"recrash"}})
		{
			const auto before = capture_exact_family(database);
			(void)crash_once(executable, test_shim, database, before);
			require_exact_v2_projection(database, engine, expected, label);
			const auto recovered_census = capture_v2_store_census(database);
			require(recovered_census == expected_census,
					std::string{label} +
						" changed publication rows, heads, metadata, or schema census");
		}

		{
			auto store = cxxlens::sdk::open_sqlite_snapshot_store(database.string(), engine);
			require(store.has_value(), "post-recrash Store unavailable");
			auto migrated = store->compact();
			require(migrated.has_value(),
					"post-recrash migration failed: " +
						(migrated ? std::string{} : error_text(migrated.error())));
			const auto compatibility = store->compatibility();
			require(compatibility.backend == "sqlite" &&
						compatibility.readable_format ==
							cxxlens::sdk::semantic_version{3U, 0U, 0U} &&
						compatibility.direct_open && !compatibility.migration_required,
					"post-recrash migration did not install the exact v3 compatibility tuple");
		}
		{
			auto reopened = cxxlens::sdk::open_sqlite_snapshot_store(database.string(), engine);
			require(reopened.has_value(), "post-recrash v3 did not cold reopen");
			const auto compatibility = reopened->compatibility();
			require(compatibility.backend == "sqlite" &&
						compatibility.readable_format ==
							cxxlens::sdk::semantic_version{3U, 0U, 0U} &&
						compatibility.direct_open && !compatibility.migration_required,
					"cold-reopened v3 compatibility tuple drifted");
			auto current = reopened->current(cxxlens::test::sqlite_v3_scenario::selector(engine));
			auto prior = reopened->open_publication(expected.prior.publication_id);
			auto exported = reopened->canonical_export(expected.current.snapshot_id);
			require(current && prior && exported && current->id() == expected.current.snapshot_id &&
						current->publication().publication_id == expected.current.publication_id &&
						current->publication().sequence == expected.current.sequence &&
						prior->id() == expected.prior.snapshot_id &&
						prior->publication().publication_id == expected.prior.publication_id &&
						prior->publication().sequence == expected.prior.sequence &&
						*exported == expected.current_canonical_export,
					"post-recrash v3 changed the semantic authority");
		}
	}
} // namespace

int main(const int argc, char** argv)
{
#if !defined(__linux__)
	(void)argc;
	(void)argv;
	std::cerr << "Store SQLite crash recovery test was registered on an unsupported platform\n";
	return 1;
#else
	if (argc == 3 && std::string_view{argv[1]} == "--crash-child")
		return run_child(argv[2]);
	if (argc != 2)
	{
		std::cerr << "usage: store_sqlite_crash_recovery_test <test-shim>\n";
		return 2;
	}
	try
	{
		run_parent(argv[0], argv[1]);
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
#endif
}
